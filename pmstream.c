/* pmstream - Run projectM headless and stream frames to stdout

Copyright (C) 2022 Ravachol

http://codeberg.org/ravachol/pmstream

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version, provided that the above
copyright notice and this permission notice appear in all copies.

THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE. */

// Build (Linux):
//   gcc -O2 -o pm_stream pm_stream.c \
//       -I/usr/local/include $(pkg-config --cflags --libs sdl2 chafa) \
//       -L/usr/local/lib -Wl,-rpath,/usr/local/lib \
//       -lprojectM-4-playlist -lprojectM-4 -lEGL -lGL
//
// Usage:
//   pm_stream --stream 80x40 --mode ascii  [--preset random|N]
//   [--preset-interval 60] pm_stream --stream 80x40 --mode sixels [--cell-size
//   8x16] [--fps 15]
//             [--audio-device NAME] [--presets DIR]
//   pm_stream --list-devices
//
// Frame protocol: one frame per block, terminated by "\n\n".
//   ascii : rows separated by '\n', ANSI color per cell
//   sixels: one DCS sixel sequence (all newlines stripped) + "\n\n"

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <EGL/egl.h>
#include <EGL/eglext.h>
#define GL_GLEXT_PROTOTYPES
#include <GL/gl.h>
#include <GL/glext.h>

#include <chafa.h>
#include <projectM-4/playlist.h>
#include <projectM-4/projectM.h>

#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

enum { MODE_ASCII = 0,
       MODE_SIXELS = 1 };

static volatile sig_atomic_t g_quit = 0;
static void on_signal(int s)
{
        (void)s;
        g_quit = 1;
}

// Output buffer, grown on demand (sixel frames vary in size).
static char *g_buf = NULL;
static size_t g_cap = 0;

static int ensure_cap(size_t need)
{
        if (need <= g_cap)
                return 0;
        size_t nc = g_cap ? g_cap : 4096;
        while (nc < need)
                nc *= 2;
        char *p = realloc(g_buf, nc);
        if (!p)
                return -1;
        g_buf = p;
        g_cap = nc;
        return 0;
}

/* ------------------------------------------------------- Pixel to ASCII */

#define MACRO_STRLEN(s) (sizeof(s) / sizeof(s[0]))

static char scale[] =
    "$@&B%8WM#ZO0QoahkbdpqwmLCJUYXIjft/\\|()1{}[]l?zcvunxr!<>i;:*-+~_,\"^`'.";
static unsigned int brightness_levels = MACRO_STRLEN(scale) - 2;

unsigned char luminance_from_r_g_b(unsigned char r, unsigned char g,
                                   unsigned char b)
{
        return (unsigned char)(0.2126 * r + 0.7152 * g + 0.0722 * b);
}

static unsigned char calc_ascii_char(unsigned char r, unsigned char g,
                                     unsigned char b)
{
        unsigned char ch = luminance_from_r_g_b(r, g, b);
        int rescaled = ch * brightness_levels / 256;

        return scale[brightness_levels - rescaled];
}

// px: RGBA, GL order (row 0 = bottom). Colored ASCII, one escape per color
// change.
static size_t encode_ascii(const uint8_t *px, int W, int H, char *out)
{
        size_t n = 0;

        for (int y = H - 1; y >= 0; y--) {
                const uint8_t *row = px + (size_t)y * W * 4;
                int pr = -1, pg = -1, pb = -1;

                for (int x = 0; x < W; x++) {
                        const uint8_t *p = row + (size_t)x * 4;
                        int r = p[0] & 0xF0, g = p[1] & 0xF0, b = p[2] & 0xF0;

                        char c = (char)calc_ascii_char(p[0], p[1], p[2]);

                        if (c != ' ' && (r != pr || g != pg || b != pb)) {
                                n += (size_t)sprintf(out + n, "\033[38;2;%d;%d;%dm", r, g, b);
                                pr = r;
                                pg = g;
                                pb = b;
                        }
                        out[n++] = c;
                }

                memcpy(out + n, "\033[0m\n", 5);
                n += 5;
        }

        out[n++] = '\n';
        return n;
}

/* ------------------------------------------------- Pixel to sixels (chafa) */

// Mirrors kew's img_func.c: chafa >= 1.16 prints with NULL term info,
// older versions use the detected one.
#if CHAFA_VERSION_CUR_STABLE >= G_ENCODE_VERSION(1, 16)
#define SIXEL_PRINT_TERMINFO NULL
#else
#define SIXEL_PRINT_TERMINFO g_tinfo
#endif

static ChafaTermInfo *g_tinfo = NULL;
static ChafaCanvasConfig *g_cfg = NULL;
static ChafaSymbolMap *g_symbol_map = NULL;
static uint8_t *g_flip = NULL;
static size_t g_flip_cap = 0;

static void detect_terminal(ChafaTermInfo **term_info_out,
                            ChafaCanvasMode *mode_out,
                            ChafaPixelMode *pixel_mode_out,
                            ChafaPassthrough *passthrough_out,
                            ChafaSymbolMap **symbol_map_out)
{
        ChafaCanvasMode mode;
        ChafaPixelMode pixel_mode;
        ChafaPassthrough passthrough;
        ChafaTermInfo *term_info;
        gchar **envp;

        envp = g_get_environ();
        term_info = chafa_term_db_detect(chafa_term_db_get_default(), envp);

        mode = chafa_term_info_get_best_canvas_mode(term_info);
        pixel_mode = chafa_term_info_get_best_pixel_mode(term_info);
        passthrough =
            chafa_term_info_get_is_pixel_passthrough_needed(term_info, pixel_mode)
                ? chafa_term_info_get_passthrough_type(term_info)
                : CHAFA_PASSTHROUGH_NONE;

        const gchar *term_name = chafa_term_info_get_name(term_info);

        if (strstr(term_name, "tmux") != NULL &&
            pixel_mode != CHAFA_PIXEL_MODE_KITTY) {

                pixel_mode = CHAFA_PIXEL_MODE_SIXELS;
                mode = CHAFA_CANVAS_MODE_TRUECOLOR;
        }

        *symbol_map_out = chafa_symbol_map_new();
        chafa_symbol_map_add_by_tags(*symbol_map_out,
                                     chafa_term_info_get_safe_symbol_tags(term_info));

        *term_info_out = term_info;
        *mode_out = mode;
        *pixel_mode_out = pixel_mode;
        *passthrough_out = passthrough;

        g_strfreev(envp);
}

static void sixel_shutdown(void)
{
        free(g_flip);
        g_flip = NULL;
        g_flip_cap = 0;

        if (g_cfg) {
                chafa_canvas_config_unref(g_cfg);
                g_cfg = NULL;
        }

        if (g_symbol_map) {
                chafa_symbol_map_unref(g_symbol_map);
                g_symbol_map = NULL;
        }

        if (g_tinfo) {
                chafa_term_info_unref(g_tinfo);
                g_tinfo = NULL;
        }
}

static int sixel_init(int cols, int rows, int cell_w, int cell_h, int PW,
                      int PH)
{
        sixel_shutdown();

        if (cols <= 0 || rows <= 0 || cell_w <= 0 || cell_h <= 0 || PW <= 0 ||
            PH <= 0)
                return -1;

        ChafaCanvasMode mode;
        ChafaPixelMode pixel_mode;
        ChafaPassthrough passthrough;

        detect_terminal(&g_tinfo, &mode, &pixel_mode, &passthrough, &g_symbol_map);

        if (!g_tinfo || !g_symbol_map)
                goto fail;

        // force SIXEL output.
        pixel_mode = CHAFA_PIXEL_MODE_SIXELS;

        const gchar *term_name = chafa_term_info_get_name(g_tinfo);
        if (term_name && strstr(term_name, "tmux") != NULL)
                mode = CHAFA_CANVAS_MODE_TRUECOLOR;

        g_cfg = chafa_canvas_config_new();
        if (!g_cfg)
                goto fail;

        chafa_canvas_config_set_canvas_mode(g_cfg, mode);
        chafa_canvas_config_set_pixel_mode(g_cfg, pixel_mode);
        chafa_canvas_config_set_geometry(g_cfg, cols, rows);
        chafa_canvas_config_set_cell_geometry(g_cfg, cell_w, cell_h);
        chafa_canvas_config_set_passthrough(g_cfg, passthrough);
        chafa_canvas_config_set_symbol_map(g_cfg, g_symbol_map);
        chafa_canvas_config_set_work_factor(g_cfg, 0.3f);

        if ((size_t)PW > SIZE_MAX / 4 / (size_t)PH)
                goto fail;

        g_flip_cap = (size_t)PW * PH * 4;
        g_flip = malloc(g_flip_cap);
        if (!g_flip)
                goto fail;

        return 0;

fail:
        sixel_shutdown();
        return -1;
}

// px: RGBA, GL order. Returns bytes placed in g_buf (0 = skip this frame).
static size_t encode_sixel(const uint8_t *px, int PW, int PH)
{
        if (!px || !g_flip || !g_cfg || PW <= 0 || PH <= 0)
                return 0;

        if ((size_t)PW > SIZE_MAX / 4 / (size_t)PH)
                return 0;

        size_t pixel_bytes = (size_t)PW * PH * 4;

        // Reinitialize if the requested framebuffer is larger than allocated.
        if (pixel_bytes > g_flip_cap) {
                uint8_t *new_flip = realloc(g_flip, pixel_bytes);
                if (!new_flip)
                        return 0;

                g_flip = new_flip;
                g_flip_cap = pixel_bytes;
        }

        // GL bottom-up -> Chafa top-down; force opaque alpha.
        for (int y = 0; y < PH; y++) {
                const uint8_t *src = px + (size_t)(PH - 1 - y) * PW * 4;
                uint8_t *dst = g_flip + (size_t)y * PW * 4;

                for (int x = 0; x < PW; x++) {
                        dst[x * 4 + 0] = src[x * 4 + 0];
                        dst[x * 4 + 1] = src[x * 4 + 1];
                        dst[x * 4 + 2] = src[x * 4 + 2];
                        dst[x * 4 + 3] = 255;
                }
        }

        ChafaCanvas *canvas = chafa_canvas_new(g_cfg);
        if (!canvas)
                return 0;

        GString *s = NULL;

#if CHAFA_VERSION_CUR_STABLE >= G_ENCODE_VERSION(1, 14)

        ChafaFrame *frame =
            chafa_frame_new(g_flip, CHAFA_PIXEL_RGBA8_UNASSOCIATED, PW, PH, PW * 4);

        if (frame) {
                ChafaImage *image = chafa_image_new();

                if (image) {
                        chafa_image_set_frame(image, frame);

                        ChafaPlacement *placement = chafa_placement_new(image, 1);

                        if (placement) {
                                chafa_placement_set_tuck(placement, CHAFA_TUCK_FIT);
                                chafa_placement_set_halign(placement, CHAFA_ALIGN_CENTER);
                                chafa_placement_set_valign(placement, CHAFA_ALIGN_CENTER);
                                chafa_canvas_set_placement(canvas, placement);

#if CHAFA_VERSION_CUR_STABLE >= G_ENCODE_VERSION(1, 16)
                                s = chafa_canvas_print(canvas, NULL);
#else
                                s = chafa_canvas_print(canvas, g_tinfo);
#endif

                                chafa_placement_unref(placement);
                        }

                        chafa_image_unref(image);
                }

                chafa_frame_unref(frame);
        }

#else

        chafa_canvas_draw_all_pixels(canvas, CHAFA_PIXEL_RGBA8_UNASSOCIATED, g_flip,
                                     PW, PH, PW * 4);
        s = chafa_canvas_print(canvas, g_tinfo);

#endif

        chafa_canvas_unref(canvas);

        if (!s)
                return 0;

        // Preserve Chafa's output bytes; append the frame delimiter.
        size_t len = s->len;

        while (len > 0 && s->str[len - 1] == '\n')
                len--;

        if (ensure_cap(len + 3) != 0) {
                g_string_free(s, TRUE);
                return 0;
        }

        memcpy(g_buf, s->str, len);

        size_t n = len;
        g_buf[n++] = '\n';
        g_buf[n++] = '\n';
        g_buf[n] = '\0';

        g_string_free(s, TRUE);

        static int warned = 0;
        if (n >= 8 * 1024 * 1024 && !warned) {
                warned = 1;
                fprintf(stderr,
                        "warning: frame is %zu bytes; kew drops frames >= "
                        "VISUALIZATIONS_MAX_BUF (8 * 1024 * 1024)\n",
                        n);
        }

        return n;
}

/* ------------------------------------------------------------------ EGL */

static EGLDisplay g_dpy = EGL_NO_DISPLAY;
static EGLContext g_ctx = EGL_NO_CONTEXT;

static int egl_init(void)
{
        PFNEGLGETPLATFORMDISPLAYEXTPROC get_pd =
            (PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress(
                "eglGetPlatformDisplayEXT");

        if (get_pd)
                g_dpy = get_pd(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, NULL);
        if (g_dpy == EGL_NO_DISPLAY)
                g_dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
        if (g_dpy == EGL_NO_DISPLAY) {
                fprintf(stderr, "EGL: no display\n");
                return -1;
        }

        EGLint maj, min;
        if (!eglInitialize(g_dpy, &maj, &min)) {
                fprintf(stderr, "EGL: init failed\n");
                return -1;
        }
        if (!eglBindAPI(EGL_OPENGL_API)) {
                fprintf(stderr, "EGL: no desktop GL\n");
                return -1;
        }

        const EGLint cfg_attr[] = {EGL_SURFACE_TYPE,
                                   EGL_PBUFFER_BIT,
                                   EGL_RENDERABLE_TYPE,
                                   EGL_OPENGL_BIT,
                                   EGL_RED_SIZE,
                                   8,
                                   EGL_GREEN_SIZE,
                                   8,
                                   EGL_BLUE_SIZE,
                                   8,
                                   EGL_ALPHA_SIZE,
                                   8,
                                   EGL_NONE};
        EGLConfig cfg;
        EGLint ncfg = 0;
        if (!eglChooseConfig(g_dpy, cfg_attr, &cfg, 1, &ncfg) || ncfg < 1) {
                fprintf(stderr, "EGL: no config\n");
                return -1;
        }

        const EGLint ctx_attr[] = {EGL_CONTEXT_MAJOR_VERSION,
                                   3,
                                   EGL_CONTEXT_MINOR_VERSION,
                                   3,
                                   EGL_CONTEXT_OPENGL_PROFILE_MASK,
                                   EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT,
                                   EGL_NONE};
                                   
        g_ctx = eglCreateContext(g_dpy, cfg, EGL_NO_CONTEXT, ctx_attr);

        if (g_ctx == EGL_NO_CONTEXT) {
                fprintf(stderr, "EGL: context failed\n");
                return -1;
        }

        // Surfaceless: we only ever render into our own FBO.
        if (!eglMakeCurrent(g_dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, g_ctx)) {
                fprintf(stderr, "EGL: make current failed (no surfaceless support?)\n");
                return -1;
        }

        return 0;
}

static void egl_shutdown(void)
{
        if (g_dpy != EGL_NO_DISPLAY) {
                eglMakeCurrent(g_dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
                if (g_ctx != EGL_NO_CONTEXT)
                        eglDestroyContext(g_dpy, g_ctx);
                eglTerminate(g_dpy);
        }
}

/* ------------------------------------------------------------------ FBO */

static GLuint g_fbo, g_rbo;

static int fbo_init(int w, int h)
{
        glGenFramebuffers(1, &g_fbo);
        glGenRenderbuffers(1, &g_rbo);
        glBindRenderbuffer(GL_RENDERBUFFER, g_rbo);
        glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, w, h);
        glBindFramebuffer(GL_FRAMEBUFFER, g_fbo);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                                  GL_RENDERBUFFER, g_rbo);

        if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
                fprintf(stderr, "FBO incomplete\n");
                return -1;
        }

        return 0;
}

/* ---------------------------------------------------------------- audio */

static projectm_handle g_pm;


#include <pthread.h>
#include <pulse/simple.h>
#include <pulse/error.h>

static projectm_handle g_pm;
static pa_simple *g_pa = NULL;
static pthread_t g_audio_thr;
static int g_audio_running = 0;

static pthread_mutex_t g_audio_lock = PTHREAD_MUTEX_INITIALIZER;
static int g_audio_stop = 0;

static void *audio_thread(void *ud)
{
        (void)ud;
        float buf[512 * 2];
        int err;

        for (;;) {
                if (pa_simple_read(g_pa, buf, sizeof(buf), &err) < 0) {
                        fprintf(stderr, "pa_simple_read: %s\n", pa_strerror(err));
                        break;
                }

                pthread_mutex_lock(&g_audio_lock);
                if (g_audio_stop) {
                        pthread_mutex_unlock(&g_audio_lock);
                        break;
                }
                projectm_pcm_add_float(g_pm, buf, 512, PROJECTM_STEREO);
                pthread_mutex_unlock(&g_audio_lock);
        }
        return NULL;
}

static int audio_start(const char *device)
{
        // NULL/empty = monitor of whatever the default output is.
        const char *src = (device && *device) ? device : "@DEFAULT_MONITOR@";

        static const pa_sample_spec ss = {
            .format = PA_SAMPLE_FLOAT32LE,
            .rate = 44100,
            .channels = 2,
        };
        // Small fragments for low latency.
        pa_buffer_attr attr = {
            .maxlength = (uint32_t)-1,
            .tlength = (uint32_t)-1,
            .prebuf = (uint32_t)-1,
            .minreq = (uint32_t)-1,
            .fragsize = 512 * 2 * sizeof(float),
        };

        int err;
        g_pa = pa_simple_new(NULL, "pmstream", PA_STREAM_RECORD, src,
                             "visualizer", &ss, NULL, &attr, &err);
        if (!g_pa) {
                fprintf(stderr, "pa_simple_new(%s): %s\n", src, pa_strerror(err));
                return -1;
        }

        fprintf(stderr, "capturing from: %s\n", src);

        if (pthread_create(&g_audio_thr, NULL, audio_thread, NULL) != 0) {
                pa_simple_free(g_pa);
                g_pa = NULL;
                return -1;
        }
        g_audio_running = 1;
        return 0;
}

static void audio_stop(void)
{
        pthread_mutex_lock(&g_audio_lock);
        g_audio_stop = 1;
        pthread_mutex_unlock(&g_audio_lock);
}

/* ----------------------------------------------------------------- misc */

// Cell size in pixels, from the controlling terminal (inherited from kew), else
// 8x16.
static void detect_cell_size(int *cw, int *ch)
{
        *cw = 8;
        *ch = 16;

        struct winsize ws;
        int fds[] = {STDIN_FILENO, STDERR_FILENO};
        for (size_t i = 0; i < sizeof(fds) / sizeof(fds[0]); i++) {
                if (ioctl(fds[i], TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0 && ws.ws_row > 0 &&
                    ws.ws_xpixel > 0 && ws.ws_ypixel > 0) {
                        *cw = ws.ws_xpixel / ws.ws_col;
                        *ch = ws.ws_ypixel / ws.ws_row;
                        break;
                }
        }
        if (*cw < 4 || *cw > 100)
                *cw = 8;
        if (*ch < 8 || *ch > 100)
                *ch = 16;
}

static void sleep_until(struct timespec *next, long period_ns)
{
        next->tv_nsec += period_ns;
        while (next->tv_nsec >= 1000000000L) {
                next->tv_nsec -= 1000000000L;
                next->tv_sec++;
        }

        clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, next, NULL);
}

/* ----------------------------------------------------------------- main */

int main(int argc, char **argv)
{
        int W = 80, H = 40; // in character cells
        int mode = MODE_ASCII;
        int fps = 0;                // 0 = pick a default per mode
        int cell_w = 0, cell_h = 0; // 0 = autodetect (sixels only)
        int preset_idx = -1;        // -1 = random
        double interval = 25.0;
        const char *device = NULL;
        const char *preset_dir = NULL;

        for (int i = 1; i < argc; i++) {
                if (!strcmp(argv[i], "--stream") && i + 1 < argc)
                        sscanf(argv[++i], "%dx%d", &W, &H);
                else if (!strcmp(argv[i], "--mode") && i + 1 < argc) {
                        const char *v = argv[++i];
                        if (!strcmp(v, "ascii"))
                                mode = MODE_ASCII;
                        else if (!strcmp(v, "sixels") || !strcmp(v, "sixel"))
                                mode = MODE_SIXELS;
                        else {
                                fprintf(stderr, "unknown mode '%s' (use ascii or sixels)\n", v);
                                return 2;
                        }
                } else if (!strcmp(argv[i], "--fps") && i + 1 < argc)
                        fps = atoi(argv[++i]);
                else if (!strcmp(argv[i], "--cell-size") && i + 1 < argc)
                        sscanf(argv[++i], "%dx%d", &cell_w, &cell_h);
                else if (!strcmp(argv[i], "--preset") && i + 1 < argc) {
                        const char *v = argv[++i];
                        preset_idx = strcmp(v, "random") ? atoi(v) : -1;
                } else if (!strcmp(argv[i], "--preset-interval") && i + 1 < argc)
                        interval = atof(argv[++i]);
                else if (!strcmp(argv[i], "--audio-device") && i + 1 < argc)
                        device = argv[++i];
                else if ((!strcmp(argv[i], "--presets") || !strcmp(argv[i], "-p")) &&
                         i + 1 < argc)
                        preset_dir = argv[++i];
                else if (!strcmp(argv[i], "-c") && i + 1 < argc)
                        i++; // chroma config path: ignored here
        }
        if (W < 1)
                W = 1;
        if (H < 1)
                H = 1;
        if (fps <= 0)
                fps = (mode == MODE_SIXELS) ? 24 : 30;

        if (!preset_dir) {
                static char home_dir[512];
                const char *home = getenv("HOME");
                const char *cands[] = {
                    NULL,
                    "/usr/local/share/projectM/presets",
                    "/usr/share/projectM/presets",
                    "/usr/share/projectm/presets",
                };
                if (home) {
                        snprintf(home_dir, sizeof(home_dir), "%s/.local/share/projectM/presets",
                                 home);
                        cands[0] = home_dir;
                }
                for (size_t i = 0; i < sizeof(cands) / sizeof(cands[0]); i++) {
                        if (cands[i] && access(cands[i], R_OK) == 0) {
                                preset_dir = cands[i];
                                break;
                        }
                }
                if (!preset_dir) {
                        fprintf(stderr, "no preset directory found, use --presets DIR\n");
                        preset_dir = ".";
                }
        }

        // Render size: ascii = one pixel per cell; sixels = full pixel size of the
        // cell area.
        int RW = W, RH = H;
        if (mode == MODE_SIXELS) {
                if (cell_w <= 0 || cell_h <= 0)
                        detect_cell_size(&cell_w, &cell_h);
                RW = W * cell_w;
                RH = H * cell_h;
                fprintf(stderr,
                        "sixel mode: %dx%d cells, cell %dx%d px, render %dx%d px, %d fps\n",
                        W, H, cell_w, cell_h, RW, RH, fps);
        }

        int out_fd = dup(STDOUT_FILENO);
        dup2(STDERR_FILENO, STDOUT_FILENO);
        FILE *out = fdopen(out_fd, "w");
        setvbuf(out, NULL, _IOFBF, 1 << 16);

        signal(SIGPIPE, SIG_IGN);
        signal(SIGINT, on_signal);
        signal(SIGTERM, on_signal);

        if (egl_init() != 0)
                return 1;
        if (fbo_init(RW, RH) != 0)
                return 1;

        if (mode == MODE_SIXELS) {
                if (sixel_init(W, H, cell_w, cell_h, RW, RH) != 0) {
                        fprintf(stderr, "sixel init failed\n");
                        return 1;
                }
                if (ensure_cap(256 * 1024) != 0)
                        return 1;
        } else {
                if (ensure_cap((size_t)W * H * 24 + (size_t)H * 8 + 64) != 0)
                        return 1;
        }

        g_pm = projectm_create();

        if (!g_pm) {
                fprintf(stderr, "projectm_create failed\n");
                return 1;
        }
        projectm_set_window_size(g_pm, (size_t)RW, (size_t)RH);
        projectm_set_fps(g_pm, fps);
        projectm_set_preset_duration(g_pm, interval);

        projectm_playlist_handle pl = projectm_playlist_create(g_pm);
        uint32_t count = projectm_playlist_add_path(pl, preset_dir, true, false);
        fprintf(stderr, "loaded %u presets from %s\n", count, preset_dir);

        if (count == 0)
                fprintf(stderr, "warning: no presets found, use --presets DIR\n");

        if (preset_idx < 0) {
                projectm_playlist_set_shuffle(pl, true);
                projectm_playlist_play_next(pl, true);
        } else {
                projectm_playlist_set_shuffle(pl, false);
                projectm_playlist_set_position(
                    pl, (uint32_t)preset_idx % (count ? count : 1), true);
                // Fixed preset: effectively disable auto-advance.
                projectm_set_preset_duration(g_pm, 1e9);
        }

        if (audio_start(device) != 0)
                fprintf(stderr, "continuing without audio input\n");

        uint8_t *px = malloc((size_t)RW * RH * 4);
        if (!px)
                return 1;

        struct timespec next;
        clock_gettime(CLOCK_MONOTONIC, &next);
        const long period_ns = 1000000000L / fps;

        while (!g_quit) {

                glBindFramebuffer(GL_FRAMEBUFFER, g_fbo);
                glViewport(0, 0, RW, RH);
                projectm_opengl_render_frame_fbo(g_pm, g_fbo);

                glBindFramebuffer(GL_READ_FRAMEBUFFER, g_fbo);
                glPixelStorei(GL_PACK_ALIGNMENT, 1);
                glReadPixels(0, 0, RW, RH, GL_RGBA, GL_UNSIGNED_BYTE, px);

                size_t n = (mode == MODE_SIXELS) ? encode_sixel(px, RW, RH)
                                                 : encode_ascii(px, W, H, g_buf);

                if (n > 0) {
                        if (fwrite(g_buf, 1, n, out) != n || fflush(out) != 0)
                                break; // caller closed the pipe
                }

                sleep_until(&next, period_ns);
        }

        audio_stop();

        projectm_playlist_destroy(pl);
        projectm_destroy(g_pm);

        if (mode == MODE_SIXELS)
                sixel_shutdown();

        egl_shutdown();
        free(px);
        free(g_buf);

        return 0;
}
