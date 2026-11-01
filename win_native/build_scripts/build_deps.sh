#!/usr/bin/env bash
# Build pinned main-branch dependencies as static libraries for the
# UxPlay Windows-native build (MSYS2 CLANG64, Skylake-tuned).
# Usage: build_deps.sh <source_root> <install_prefix> <step...>
set -euo pipefail

source_root="$1"
install_prefix="$2"
shift 2

export PATH=/clang64/bin:/usr/bin
export CC=clang CXX=clang++ AR=llvm-ar RANLIB=llvm-ranlib NM=llvm-nm STRIP=llvm-strip
export PKG_CONFIG_PATH="$install_prefix/lib/pkgconfig"
# libtool.m4 ships with the CLANG64 libtool package, not the MSYS one.
export ACLOCAL_PATH=/clang64/share/aclocal:/usr/share/aclocal
tuning_flags="-O3 -march=skylake -mtune=skylake"
jobs=8
mkdir -p "$install_prefix"

build_libusb()
{
    cd "$source_root/libusb"
    ./bootstrap.sh
    ./configure --prefix="$install_prefix" --enable-static --disable-shared \
        --disable-examples-build --disable-tests-build CFLAGS="$tuning_flags"
    make -j"$jobs"
    make install
}

build_libplist()
{
    cd "$source_root/libplist"
    NOCONFIGURE=1 ./autogen.sh
    ./configure --prefix="$install_prefix" --enable-static --disable-shared \
        --without-cython --without-tools CFLAGS="$tuning_flags" CXXFLAGS="$tuning_flags"
    make -j"$jobs"
    make install
}

build_openssl()
{
    cd "$source_root/openssl"
    perl Configure mingw64 --prefix="$install_prefix" --libdir=lib \
        no-shared no-tests no-docs no-apps no-module no-legacy no-engine \
        CC=clang AR=llvm-ar RANLIB=llvm-ranlib CFLAGS="$tuning_flags"
    make -j"$jobs" build_libs
    make install_dev
}

build_ffmpeg()
{
    cd "$source_root/ffmpeg"
    ./configure --prefix="$install_prefix" \
        --cc=clang --cxx=clang++ --ar=llvm-ar --ranlib=llvm-ranlib --nm=llvm-nm --strip=llvm-strip \
        --target-os=mingw32 --arch=x86_64 \
        --enable-static --disable-shared --disable-debug --disable-doc --disable-programs \
        --disable-autodetect --enable-w32threads --enable-d3d11va \
        --disable-everything --disable-network \
        --disable-avdevice --disable-avformat --disable-avfilter --disable-swscale \
        --enable-avcodec --enable-avutil --enable-swresample \
        --enable-decoder=h264,hevc,aac,alac,pcm_s16le,pcm_f32le \
        --enable-parser=h264,hevc,aac \
        --enable-hwaccel=h264_d3d11va,h264_d3d11va2,hevc_d3d11va,hevc_d3d11va2 \
        --extra-cflags="$tuning_flags"
    make -j"$jobs"
    make install
}

for step in "$@"
do
    echo "=== building $step"
    "build_$step"
done
echo "=== done: $*"
