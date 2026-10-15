#!/usr/bin/sh

# download projectM source
mkdir projectM
git clone https://github.com/projectM-visualizer/projectm.git ./projectM
cd ./projectM
git fetch --all --tags
git submodule init
git submodule update

# configure cmake for projectM
mkdir build
cd build
cmake -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr/local ..

# make and install projectM
cmake --build . -- -j && sudo cmake --build . --target install

# make and install pmstream
cd ../../
gcc -O2 -o pmstream pmstream.c \
    -I/usr/local/include $(pkg-config --cflags --libs chafa) \
    -L/usr/local/lib -Wl,-rpath,/usr/local/lib \
    -lprojectM-4-playlist -lprojectM-4 -lEGL -lGL \
    -lpulse-simple -lpulse -lpthread


sudo cp pmstream /usr/local/bin
