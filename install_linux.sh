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
cmake -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr/local -DENABLE_PLAYLIST=ON ..

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

# download presets (shallow clone, we only need the files)
git clone --depth 1 https://github.com/projectM-visualizer/presets-cream-of-the-crop.git ./presets-cream-of-the-crop

# pick the first existing preset directory
PRESET_DIR=""
for dir in \
    /usr/local/share/projectM/presets \
    /usr/share/projectM/presets \
    /usr/share/projectm/presets
do
    if [ -d "$dir" ]; then
        PRESET_DIR="$dir"
        break
    fi
done

# fall back to creating the default location if none exist
if [ -z "$PRESET_DIR" ]; then
    PRESET_DIR=/usr/local/share/projectM/presets
    sudo mkdir -p "$PRESET_DIR"
fi

echo "Installing presets to $PRESET_DIR"

# copy preset contents (excluding git metadata and repo readme files)
sudo cp -r ./presets-cream-of-the-crop/. "$PRESET_DIR"/
sudo rm -rf "$PRESET_DIR"/.git
