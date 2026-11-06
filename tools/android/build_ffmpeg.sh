#!/bin/sh
# Cross-compiles a minimal static FFmpeg for Android arm64: just what the
# Type X2 movies need (ASF container, WMV1/2/3 + VC-1 video, WMA audio).
#   tools/android/build_ffmpeg.sh <ffmpeg-src> <install-prefix> [ndk] [api]
set -e
SRC=$(cd "$1" && pwd)
PREFIX=$2
NDK=${3:-${ANDROID_NDK_HOME:-$(ls -d "$HOME"/Library/Android/sdk/ndk/* | sort -V | tail -1)}}
API=${4:-33}
case "$(uname -s)" in Darwin) HOST=darwin-x86_64 ;; *) HOST=linux-x86_64 ;; esac
TC=$NDK/toolchains/llvm/prebuilt/$HOST/bin
BUILD=$PREFIX/build
mkdir -p "$BUILD" "$PREFIX"
cd "$BUILD"
"$SRC"/configure \
  --prefix="$PREFIX" \
  --target-os=android --arch=aarch64 --cpu=armv8-a --enable-cross-compile \
  --cc="$TC/aarch64-linux-android$API-clang" --cxx="$TC/aarch64-linux-android$API-clang++" \
  --ar="$TC/llvm-ar" --ranlib="$TC/llvm-ranlib" --nm="$TC/llvm-nm" --strip="$TC/llvm-strip" \
  --enable-static --disable-shared --enable-pic \
  --disable-programs --disable-doc --disable-avdevice --disable-avfilter \
  --disable-network --disable-everything --disable-debug \
  --enable-protocol=file \
  --enable-demuxer=asf \
  --enable-decoder=wmv1,wmv2,wmv3,vc1,wmav1,wmav2,wmapro,wmalossless,wmavoice \
  --enable-swscale --enable-swresample \
  --extra-cflags="-O2 -fPIC"
make -j"$(sysctl -n hw.ncpu 2>/dev/null || nproc)"
make install
