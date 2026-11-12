# Building from source

These steps build the app for development on a desktop. Releases are built with
[packaging/release](packaging/release/README.md) instead, which pins and builds
every dependency itself.

## Dependencies (Ubuntu 26.04)

```sh
sudo apt install build-essential cmake pkg-config curl libopenxr-dev libmpv-dev libglfw3-dev libgl-dev libegl-dev \
  libvulkan-dev libpango1.0-dev libcairo2-dev libcurl4-openssl-dev libgdk-pixbuf-2.0-dev \
  nlohmann-json3-dev libglib2.0-dev libavformat-dev libavcodec-dev libavutil-dev libswscale-dev \
  libsqlite3-dev librsvg2-dev glslang-tools libudfread-dev libbluray-dev fonts-noto-extra libssl-dev
```

The app also needs **libsmb2 6.3 or newer** and **libssh 0.12 or newer**.
Ubuntu 26.04 has libsmb2 6.2 and libssh 0.11, so build the pinned versions
first, outside this checkout:

```sh
curl -fL https://codeload.github.com/sahlberg/libsmb2/tar.gz/2bcb014b23a8c8cccf534eea8bda2fe5df453cfd -o libsmb2.tar.gz
echo '72727c2b6bb19367b4224e655cd41e1d80b453bf910e967cec5704cbbc40b589  libsmb2.tar.gz' | sha256sum -c -
tar -xf libsmb2.tar.gz
cmake -S libsmb2-2bcb014b23a8c8cccf534eea8bda2fe5df453cfd -B libsmb2-build \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
  -DBUILD_SHARED_LIBS=ON -DENABLE_LIBKRB5=OFF -DENABLE_GSSAPI=OFF -DENABLE_EXAMPLES=OFF
cmake --build libsmb2-build -j2
sudo cmake --install libsmb2-build
sudo ldconfig
pkg-config --modversion libsmb2

curl -fLO https://www.libssh.org/files/0.12/libssh-0.12.2.tar.xz
echo '49560f677d96e3706a904ac2de1116e25f3680937d51e5c92198fcba4a1c1e9f  libssh-0.12.2.tar.xz' | sha256sum -c -
tar -xf libssh-0.12.2.tar.xz
cmake -S libssh-0.12.2 -B libssh-build -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=ON \
  -DWITH_GSSAPI=OFF -DWITH_SERVER=OFF -DWITH_ZLIB=OFF -DWITH_PCAP=OFF -DWITH_EXAMPLES=OFF
cmake --build libssh-build -j2
sudo cmake --install libssh-build
sudo ldconfig
pkg-config --modversion libssh
```

Blu-ray disc images need **libudfread 1.2 or newer** and **libbluray 1.4 or
newer** (Ubuntu 26.04 has 1.2.0 and 1.4.1). On distributions without a
libudfread package, build it the same way:

```sh
curl -fLO https://download.videolan.org/pub/videolan/libudfread/libudfread-1.2.0.tar.xz
echo 'bb477cbd4cfbfc7787d9d05b71ee5e70430f5cfebf1297497f7e83547958050f  libudfread-1.2.0.tar.xz' | sha256sum -c -
tar -xf libudfread-1.2.0.tar.xz
meson setup libudfread-build libudfread-1.2.0 --buildtype=release
ninja -C libudfread-build
sudo meson install -C libudfread-build
sudo ldconfig
pkg-config --modversion libudfread
```

If you install them somewhere else, point `PKG_CONFIG_PATH` at their
`lib/pkgconfig` before the first build.

## Build and run

```sh
./build.sh                       # builds into build/
./build/videoframevr [video]     # needs SteamVR (or another OpenXR runtime) running
```

You need OpenGL 4.5, Vulkan 1.2 and an active OpenXR runtime. Desktop hardware
decoding uses NVDEC on NVIDIA. AMD support is experimental and unverified on
hardware: use Mesa 26.1 or newer with radeonsi, RADV and the Mesa VA-API driver,
and a libmpv/FFmpeg build with VA-API enabled. Playback requests `vaapi,vaapi-copy`;
seek previews use `vaapi-copy`. Hardware codec and resolution limits depend on
the GPU. `--hwdec=vaapi-copy` selects copy-back decoding for troubleshooting;
`--hwdec=no` disables hardware decoding for playback and previews. AMD library
thumbnails currently use software decoding.
