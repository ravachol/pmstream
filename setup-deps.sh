#!/bin/sh
set -e

# projectM (CMake project; installs the .pc files Meson looks for)
git clone --recurse-submodules https://github.com/projectM-visualizer/projectm.git projectM
cmake -S projectM -B projectM/build \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr/local -DENABLE_PLAYLIST=ON
cmake --build projectM/build -j
sudo cmake --install projectM/build

# presets (shallow clone; Meson installs them)
git clone --depth 1 https://github.com/projectM-visualizer/presets-cream-of-the-crop.git
