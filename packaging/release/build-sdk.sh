#!/usr/bin/env bash
# Runs only in the pinned SDK. /src and /downloads are read-only; /work is private.
set -euo pipefail
arch=$1 jobs=$2
[[ $arch == aarch64 || $arch == x86_64 ]]
prefix=/work/prefix
mkdir -p "$prefix" /work/home /work/source /work/config
exec > >(tee /work/build.log) 2>&1
set -x
triplet=x86_64-linux-gnu
cross=()
cmake_cross=()
ffmpeg_cross=()
if [[ $arch == aarch64 ]]; then
  triplet=aarch64-linux-gnu
  export CC=aarch64-linux-gnu-gcc CXX=aarch64-linux-gnu-g++ AR=aarch64-linux-gnu-ar
  export STRIP=aarch64-linux-gnu-strip
  ffmpeg_cross=(--enable-cross-compile --cross-prefix=aarch64-linux-gnu- --arch=aarch64 --target-os=linux)
  cat > /work/config/cross.ini <<'MESON'
[binaries]
c = 'aarch64-linux-gnu-gcc'
cpp = 'aarch64-linux-gnu-g++'
ar = 'aarch64-linux-gnu-ar'
strip = 'aarch64-linux-gnu-strip'
pkg-config = '/work/config/pkg-config'
[host_machine]
system = 'linux'
cpu_family = 'aarch64'
cpu = 'aarch64'
endian = 'little'
[properties]
needs_exe_wrapper = true
MESON
  cross=(--cross-file=/work/config/cross.ini)
  cat > /work/config/toolchain.cmake <<'CMAKE'
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)
set(CMAKE_C_COMPILER aarch64-linux-gnu-gcc)
set(CMAKE_CXX_COMPILER aarch64-linux-gnu-g++)
set(CMAKE_LIBRARY_ARCHITECTURE aarch64-linux-gnu)
set(CMAKE_FIND_ROOT_PATH /usr/aarch64-linux-gnu /)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE BOTH)
set(PKG_CONFIG_EXECUTABLE /work/config/pkg-config CACHE FILEPATH "Target pkg-config")
CMAKE
  cmake_cross=(-DCMAKE_TOOLCHAIN_FILE=/work/config/toolchain.cmake)
else
  export CC=gcc CXX=g++ AR=ar STRIP=strip
fi
cat > /work/config/pkg-config <<EOF
#!/bin/sh
export PKG_CONFIG_LIBDIR='$prefix/lib/pkgconfig:/usr/lib/$triplet/pkgconfig:/usr/share/pkgconfig'
unset PKG_CONFIG_PATH
exec /usr/bin/pkg-config "\$@"
EOF
chmod +x /work/config/pkg-config
export PKG_CONFIG=/work/config/pkg-config
export PKG_CONFIG_LIBDIR="$prefix/lib/pkgconfig:/usr/lib/$triplet/pkgconfig:/usr/share/pkgconfig"
unset PKG_CONFIG_PATH
export CFLAGS="-O2 -ffile-prefix-map=/work=. -ffile-prefix-map=/src=."
export CXXFLAGS="$CFLAGS"

json_source=/work/source/json
mpv_source=/work/source/mpv-0.40.0
ffmpeg_source=/work/source/rpi-ffmpeg-d0fe46c27ff128e6263ceb3251cbc9d0ed3d3932
smb2_source=/work/source/libsmb2-2bcb014b23a8c8cccf534eea8bda2fe5df453cfd
ssh_source=/work/source/libssh-0.12.2
udfread_source=/work/source/libudfread-1.2.0
bluray_source=/work/source/libbluray-1.5.1
if [[ ! -f /work/.dependencies-complete ]]; then
  # No compiler, package or source download is permitted in this phase.
  python3 - <<'PY'
import json, tarfile
from pathlib import Path
lock = json.load(open('/src/packaging/release/sources.json'))
for item in lock['sources']:
    with tarfile.open('/downloads/' + item['file']) as archive:
        archive.extractall('/work/source', filter='data')
PY
for smb2_patch in /src/packaging/libsmb2/*.patch; do
  [[ -f "$smb2_patch" ]] || continue
  patch -d "$smb2_source" -p1 < "$smb2_patch"
done
cmake -S "$smb2_source" -B /work/libsmb2-build -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_POLICY_VERSION_MINIMUM=3.5 -DCMAKE_INSTALL_PREFIX="$prefix" -DBUILD_SHARED_LIBS=ON \
  -DENABLE_LIBKRB5=OFF -DENABLE_GSSAPI=OFF -DENABLE_EXAMPLES=OFF "${cmake_cross[@]}"
cmake --build /work/libsmb2-build -j"$jobs"
cmake --install /work/libsmb2-build
cp /work/libsmb2-build/CMakeCache.txt /work/config/libsmb2-cmake-cache.txt
"$PKG_CONFIG" --modversion libsmb2
# SFTP. OpenSSL is the Platform's; no server, compression, GSSAPI or pcap.
cmake -S "$ssh_source" -B /work/libssh-build -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX="$prefix" -DCMAKE_INSTALL_LIBDIR=lib -DBUILD_SHARED_LIBS=ON \
  -DWITH_GSSAPI=OFF -DWITH_SERVER=OFF -DWITH_ZLIB=OFF -DWITH_PCAP=OFF -DWITH_EXAMPLES=OFF \
  -DWITH_DEBUG_CALLTRACE=OFF -DWITH_NACL=OFF -DUNIT_TESTING=OFF -DCLIENT_TESTING=OFF \
  -DSERVER_TESTING=OFF "${cmake_cross[@]}"
cmake --build /work/libssh-build -j"$jobs"
cmake --install /work/libssh-build
cp /work/libssh-build/CMakeCache.txt /work/config/libssh-cmake-cache.txt
"$PKG_CONFIG" --modversion libssh
# Blu-ray disc images, read for their file system and playlists only:
# libbluray on the libudfread above, without BD-J, tools, FreeType,
# fontconfig or libxml2. AACS and BD+ libraries are never bundled.
meson setup /work/libudfread-build "$udfread_source" "${cross[@]}" \
  --prefix="$prefix" --libdir=lib --buildtype=release -Ddefault_library=shared \
  -Denable_examples=false
ninja -C /work/libudfread-build -j"$jobs"
meson install -C /work/libudfread-build
meson setup /work/libbluray-build "$bluray_source" "${cross[@]}" \
  --prefix="$prefix" --libdir=lib --buildtype=release --wrap-mode=nofallback \
  -Ddefault_library=shared -Dbdj_jar=disabled -Dfreetype=disabled -Dfontconfig=disabled \
  -Dlibxml2=disabled -Denable_tools=false -Denable_devtools=false -Denable_examples=false \
  -Denable_docs=false
ninja -C /work/libbluray-build -j"$jobs"
meson install -C /work/libbluray-build
cp /work/libudfread-build/meson-info/intro-buildoptions.json /work/config/libudfread-options.json
cp /work/libbluray-build/meson-info/intro-buildoptions.json /work/config/libbluray-options.json
"$PKG_CONFIG" --modversion libudfread libbluray
# Both architectures: 3D Blu-ray (MVC) in both views. The Frame's patches
# apply on top of it.
mkdir -p "$mpv_source/video/decode/mvc"
cp /src/packaging/mpv/mvc/*.[ch] "$mpv_source/video/decode/mvc/"
patch -d "$mpv_source" -p1 < /src/packaging/mpv/mpv-mvc.patch
if [[ $arch == aarch64 ]]; then
  patch -d "$mpv_source" -p1 < /src/packaging/frame/mpv-v4l2-reset.patch
  patch -d "$mpv_source" -p1 < /src/packaging/frame/mpv-v4l2-hardware-only.patch
  patch -d "$mpv_source" -p1 < /src/packaging/frame/mpv-v4l2-interrupt.patch
  patch -d "$mpv_source" -p1 < /src/packaging/frame/mpv-export-release.patch
  cp /src/packaging/frame/mpv/dmabuf_ubwc.{c,h} /src/src/video_export.h "$mpv_source/video/out/hwdec/"
  glslangValidator -V --target-env vulkan1.2 --vn dmabuf_ubwc_spv /src/packaging/frame/mpv/dmabuf_ubwc.comp \
    -o "$mpv_source/video/out/hwdec/dmabuf_ubwc.spv.h"
  patch -d "$ffmpeg_source" -p1 < /src/packaging/frame/ffmpeg-iris.patch
  patch -d "$ffmpeg_source" -p1 < /src/packaging/frame/ffmpeg-main10.patch
  patch -d "$ffmpeg_source" -p1 < /src/packaging/frame/ffmpeg-capture-pool.patch
  patch -d "$ffmpeg_source" -p1 < /src/packaging/frame/ffmpeg-drain-seek.patch
  patch -d "$ffmpeg_source" -p1 < /src/packaging/frame/ffmpeg-staged-start.patch
  patch -d "$ffmpeg_source" -p1 < /src/packaging/frame/ffmpeg-capture-interrupt.patch
fi
cmake -S "$json_source" -B /work/json-build -DJSON_BuildTests=OFF \
  -DJSON_Install=ON -DCMAKE_INSTALL_PREFIX="$prefix" "${cmake_cross[@]}"
cmake --install /work/json-build
# Desktop: NVDEC and VA-API. NVIDIA headers only; FFmpeg and mpv open the
# driver's libcuda/libnvcuvid at run time (Steam Linux Runtime imports them).
# VA-API libraries come from the Platform, its vendor driver from the host.
ffmpeg_desktop=()
mpv_desktop=()
# The Frame's FFmpeg carries this repository's GPL-3.0 patches, so it is built
# as GPLv3 as a whole (LGPL-2.1 section 3). The desktop's is unmodified LGPL.
ffmpeg_licence=()
expected=(GPL=0 VERSION3=0 NONFREE=0)
if [[ $arch == aarch64 ]]; then
  ffmpeg_licence=(--enable-gpl --enable-version3 --disable-postproc)
  expected=(GPL=1 VERSION3=1 NONFREE=0)
fi
if [[ $arch == x86_64 ]]; then
  make -C /work/source/nv-codec-headers-12.0.16.1 PREFIX="$prefix" install
  ffmpeg_desktop=(--enable-ffnvcodec --enable-cuda --enable-nvdec --enable-cuvid --enable-vaapi)
  mpv_desktop=(-Dcuda-hwaccel=enabled -Dcuda-interop=enabled -Dvaapi=enabled -Dvaapi-drm=enabled)
fi
mkdir -p /work/ffmpeg-build
cd /work/ffmpeg-build
"$ffmpeg_source/configure" --prefix="$prefix" "${ffmpeg_cross[@]}" "${ffmpeg_desktop[@]}" "${ffmpeg_licence[@]}" \
  --pkg-config="$PKG_CONFIG" --disable-autodetect --enable-libdrm --enable-v4l2-m2m \
  --enable-zlib --enable-bzlib --enable-lzma --enable-gnutls --enable-libdav1d --enable-libopus \
  --enable-shared --disable-static --disable-doc --disable-debug --disable-programs \
  --disable-indevs --disable-outdevs --extra-cflags="$CFLAGS"
make -j"$jobs"
make install
for flag in "${expected[@]}"; do
  grep -qx "#define CONFIG_${flag%=*} ${flag#*=}" /work/ffmpeg-build/config.h
done
if [[ $arch == x86_64 ]]; then
  for flag in NVDEC CUDA HEVC_NVDEC_HWACCEL H264_NVDEC_HWACCEL AV1_NVDEC_HWACCEL \
              VAAPI HEVC_VAAPI_HWACCEL H264_VAAPI_HWACCEL AV1_VAAPI_HWACCEL VP9_VAAPI_HWACCEL; do
    # grep reads the files itself: behind a pipe, -q's early exit gets cat
    # killed by SIGPIPE, which pipefail makes a failed build.
    grep -qhx "#define CONFIG_${flag} 1" /work/ffmpeg-build/config.h /work/ffmpeg-build/config_components.h
  done
fi
cd /work
meson setup /work/mpv-build "$mpv_source" "${cross[@]}" \
  --prefix="$prefix" --libdir=lib --buildtype=release --wrap-mode=nofallback \
  -Dauto_features=disabled -Dgpl=true -Dlibmpv=true -Dcplayer=false -Dbuild-date=false \
  -Dlua=disabled -Dmanpage-build=disabled -Dpdf-build=disabled -Dhtml-build=disabled \
  -Dgl=enabled -Dplain-gl=enabled -Degl=enabled -Ddrm=enabled -Dgbm=enabled \
  -Dlibavdevice=enabled -Dlcms2=enabled -Dzlib=enabled -Diconv=enabled \
  -Dalsa=enabled -Dpulse=enabled -Dpipewire=enabled "${mpv_desktop[@]}"
ninja -C /work/mpv-build -j"$jobs"
meson install -C /work/mpv-build
"$CC" --version > /work/compiler.txt
cp /work/ffmpeg-build/config.h /work/ffmpeg-build/ffbuild/config.mak /work/config/
cp /work/mpv-build/meson-info/intro-buildoptions.json /work/config/mpv-options.json
touch /work/.dependencies-complete
fi
cmake -S /src -B /work/app-build -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_PREFIX_PATH="$prefix" "${cmake_cross[@]}" \
  '-DCMAKE_INSTALL_RPATH=$ORIGIN/../lib' -DCMAKE_INSTALL_RPATH_USE_LINK_PATH=OFF
cmake --build /work/app-build --target videoframevr -j"$jobs"
cmake --install /work/app-build --prefix /work/bundle
mkdir -p /work/bundle/lib
cp -a "$prefix"/lib/*.so* /work/bundle/lib/
# Debian binary supplements are recorded independently from source builds.
for package in /work/runtime-debs/*; do
  find "$package/usr/lib/$triplet" -maxdepth 1 -name '*.so*' -exec cp -a '{}' /work/bundle/lib/ ';'
done
