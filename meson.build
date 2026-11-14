project('pmstream', 'c',
  version: '0.1.0',
  meson_version: '>= 1.1.0',
  default_options: ['c_std=gnu11', 'buildtype=release', 'optimization=2'],
)

fs = import('fs')
cc = meson.get_compiler('c')

pm_dirs = ['/usr/local/lib', '/usr/local/lib64']

chafa_dep = dependency('chafa', version: '>=1.16', required: false)
if not chafa_dep.found()
  error('pmstream needs libchafa >= 1.16 (for the ChafaTermInfo getters). ' +
        'Debian 13 ships 1.14.x; install a newer chafa and re-run.')
endif

deps = [
  chafa_dep,
  dependency('egl'),
  dependency('gl'),
  dependency('libpulse-simple'),
  dependency('libpulse'),
  dependency('threads'),
  cc.find_library('projectM-4', dirs: pm_dirs),
  cc.find_library('projectM-4-playlist', dirs: pm_dirs),
]

executable('pmstream', 'pmstream.c',
  dependencies: deps,
  include_directories: include_directories('/usr/local/include'),
  install: true,
  install_rpath: get_option('projectm_rpath'),
)

presets_src = 'presets-cream-of-the-crop'
if get_option('install_presets') and fs.is_dir(presets_src)
  install_subdir(presets_src,
    install_dir: get_option('datadir') / 'projectM' / 'presets',
    strip_directory: true,
    exclude_directories: ['.git'],
  )
endif