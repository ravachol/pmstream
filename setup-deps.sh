#!/bin/sh
set -e

# projectM
[ -d projectM ] || git clone --recurse-submodules https://github.com/projectM-visualizer/projectm.git projectM
cmake -S projectM -B projectM/build \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr/local -DENABLE_PLAYLIST=ON
cmake --build projectM/build -j
sudo cmake --install projectM/build

# presets
[ -d presets-cream-of-the-crop ] || git clone --depth 1 \
  https://github.com/projectM-visualizer/presets-cream-of-the-crop.git

# let pkg-config see wherever projectM put its .pc files
PC_DIRS=$(find /usr/local -name '*.pc' -exec dirname {} \; | sort -u | paste -sd: -)
export PKG_CONFIG_PATH="$PC_DIRS${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"

# use the first pkg-config on PATH that can actually see chafa
# (e.g. Homebrew's can shadow the system one on Linux); honors $PKG_CONFIG
if [ -z "$PKG_CONFIG" ] || ! "$PKG_CONFIG" --exists chafa 2>/dev/null; then
  PKG_CONFIG=
  IFS=:
  for d in $PATH; do
    for n in pkg-config pkgconf; do
      if [ -x "$d/$n" ] && "$d/$n" --exists chafa 2>/dev/null; then
        PKG_CONFIG="$d/$n"; break 2
      fi
    done
  done
  unset IFS
fi
[ -n "$PKG_CONFIG" ] || { echo "No pkg-config can find chafa; install its dev package." >&2; exit 1; }
export PKG_CONFIG

# pmstream
rm -rf build
meson setup build
meson compile -C build
sudo meson install -C build