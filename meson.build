project('pmstream', 'c',
  version: '0.1.0',
  default_options: ['c_std=gnu11', 'buildtype=release', 'optimization=2'],
)

fs = import('fs')

deps = [
  dependency('chafa'),
  dependency('projectM-4'),
  dependency('projectM-4-playlist'),
  dependency('egl'),
  dependency('gl'),
  dependency('libpulse-simple'),
  dependency('libpulse'),
  dependency('threads'),
]

executable('pmstream', 'pmstream.c',
  dependencies: deps,
  install: true,                                  # -> <prefix>/bin
  install_rpath: get_option('projectm_rpath'),    # same as -Wl,-rpath,/usr/local/lib
)

# Presets: install the cloned repo if it is present
presets_src = 'presets-cream-of-the-crop'
if get_option('install_presets') and fs.is_dir(presets_src)
  install_subdir(presets_src,
    install_dir: get_option('datadir') / 'projectM' / 'presets',
    strip_directory: true,
    exclude_directories: ['.git'],
  )
endif
