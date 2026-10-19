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

# THE FIX: let pkg-config see wherever projectM put its .pc files
PC_DIRS=$(find /usr/local -name '*.pc' -exec dirname {} \; | sort -u | paste -sd: -)
export PKG_CONFIG_PATH="$PC_DIRS${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"

# pmstream
meson setup build --wipe 2>/dev/null || meson setup build
meson compile -C build
sudo meson install -C build