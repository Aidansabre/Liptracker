#!/bin/sh
# Reproducible native or static ARM64 build without installing system packages.
set -eu
repo=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
build_root=${LIPTRACKER_BUILD_ROOT:-"$repo/.build"}
tools=${LIPTRACKER_TOOLS:-"$build_root/tools"}
target=${1:-steam-frame}
case "$target" in native|steam-frame) ;; *) echo 'usage: scripts/build.sh [native|steam-frame]' >&2; exit 2;; esac
mkdir -p "$build_root/downloads" "$repo/dist"
if [ ! -x "$tools/cmake/data/bin/cmake" ] || [ ! -x "$tools/ziglang/zig" ]; then
  python3 -m pip install --target "$tools" ziglang==0.13.0 cmake==3.31.6 ninja==1.11.1.4
fi
export ZIG_GLOBAL_CACHE_DIR="$build_root/zig-cache"
export ZIG_LOCAL_CACHE_DIR="$build_root/zig-local-cache"
cmake="$tools/cmake/data/bin/cmake"

fetch() {
  filename=$1 url=$2 checksum=$3
  archive="$build_root/downloads/$filename"
  if [ ! -f "$archive" ]; then
    curl -fL --retry 2 "$url" -o "$archive.part"
    mv "$archive.part" "$archive"
  fi
  printf '%s  %s\n' "$checksum" "$archive" | sha256sum -c -
}
fetch libusb-1.0.27.tar.bz2 https://github.com/libusb/libusb/releases/download/v1.0.27/libusb-1.0.27.tar.bz2 ffaa41d741a8a3bee244ac8e54a72ea05bf2879663c098c82fc5757853441575
fetch libuvc.tar.gz https://codeload.github.com/libuvc/libuvc/tar.gz/68d07a00e11d1944e27b7295ee69673239c00b4b eaf476a5e928a638be7f0a732934d8b0719934912518a9cc078cce66fc4af80f
fetch stb.tar.gz https://codeload.github.com/nothings/stb/tar.gz/2c980bb59875b0d32144a71867fbdebb2f77cd20 9a955b1b49a4410088a2e0ee2a9c057c3c907d0c1d75454144cb980aca0ba515
sources="$build_root/sources"
mkdir -p "$sources"
if [ ! -f "$sources/libusb-1.0.27/configure" ]; then tar -xjf "$build_root/downloads/libusb-1.0.27.tar.bz2" -C "$sources"; fi
if [ ! -f "$sources/libuvc-68d07a00e11d1944e27b7295ee69673239c00b4b/CMakeLists.txt" ]; then tar -xzf "$build_root/downloads/libuvc.tar.gz" -C "$sources"; fi
if [ ! -f "$sources/stb-2c980bb59875b0d32144a71867fbdebb2f77cd20/stb_image_write.h" ]; then tar -xzf "$build_root/downloads/stb.tar.gz" -C "$sources"; fi

prefix="$build_root/$target/prefix"
mkdir -p "$build_root/$target/libusb"
if [ "$target" = steam-frame ]; then
  compiler="$build_root/$target/cc"
  # Zig supplies the musl libc/sysroot and ARM64 compiler in userspace.
  printf '#!/bin/sh\nexec "%s" cc -target aarch64-linux-musl "$@"\n' "$tools/ziglang/zig" > "$compiler"
  chmod +x "$compiler"
  configure_host=--host=aarch64-linux-musl
  cmake_target='-DCMAKE_SYSTEM_NAME=Linux -DCMAKE_SYSTEM_PROCESSOR=aarch64'
  static=-static
else
  compiler=$(command -v cc)
  configure_host=
  cmake_target=
  static=
fi
export PKG_CONFIG_LIBDIR="$prefix/lib/pkgconfig"
jobs=${LIPTRACKER_JOBS:-4}
if [ ! -f "$prefix/lib/libusb-1.0.a" ]; then
  (cd "$build_root/$target/libusb"
   CC="$compiler" "$sources/libusb-1.0.27/configure" $configure_host --prefix="$prefix" --disable-shared --enable-static --disable-udev --disable-tests-build --disable-examples-build
   make -j"$jobs"
   make install)
fi
if [ ! -f "$prefix/lib/libuvc.a" ]; then
  "$cmake" -S "$sources/libuvc-68d07a00e11d1944e27b7295ee69673239c00b4b" -B "$build_root/$target/libuvc" \
    -DCMAKE_C_COMPILER="$compiler" -DCMAKE_INSTALL_PREFIX="$prefix" -DCMAKE_INSTALL_LIBDIR=lib \
    -DCMAKE_BUILD_TARGET=Static -DBUILD_EXAMPLE=OFF -DBUILD_TEST=OFF -DCMAKE_DISABLE_FIND_PACKAGE_JpegPkg=TRUE $cmake_target
  "$cmake" --build "$build_root/$target/libuvc" --parallel "$jobs"
  "$cmake" --install "$build_root/$target/libuvc"
fi
"$compiler" -O2 -Wall -Wextra $static \
  -I"$prefix/include" -I"$prefix/include/libusb-1.0" \
  -I"$sources/stb-2c980bb59875b0d32144a71867fbdebb2f77cd20" \
  "$repo/vft-stream.c" "$repo/tracker.c" "$repo/image.c" "$repo/bulk_capture.c" "$repo/uvc_bulk.c" \
  "$prefix/lib/libuvc.a" "$prefix/lib/libusb-1.0.a" -lm -lpthread \
  -o "$repo/dist/vft-stream-$target"
printf 'Built %s\n' "$repo/dist/vft-stream-$target"
