#!/usr/bin/env bash
# Configure + build the Windows-native UxPlay with MSYS2 CLANG64.
# Usage: build_uxplay_native.sh <source_dir> <build_dir> <deps_prefix> [Release|RelWithDebInfo]
set -euo pipefail
source_dir="$1"
build_dir="$2"
deps_prefix="$3"
build_type="${4:-Release}"

export MSYSTEM=CLANG64
export PATH=/clang64/bin:/usr/bin
export PKG_CONFIG_SYSTEM_LIBRARY_PATH=/clang64/lib
export PKG_CONFIG_SYSTEM_INCLUDE_PATH=/clang64/include
export PKG_CONFIG_PATH="$deps_prefix/lib/pkgconfig:/clang64/lib/pkgconfig"
export LANG=C

cmake -S "$source_dir" -B "$build_dir" -G Ninja \
    -DCMAKE_BUILD_TYPE="$build_type" \
    -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
    -DCMAKE_C_FLAGS="-march=x86-64-v2 -mtune=generic -ffile-prefix-map=$(cygpath -m "$source_dir")=uxplay" \
    -DCMAKE_CXX_FLAGS="-march=x86-64-v2 -mtune=generic -ffile-prefix-map=$(cygpath -m "$source_dir")=uxplay" \
    -DOPENSSL_ROOT_DIR="$deps_prefix" -DOPENSSL_USE_STATIC_LIBS=TRUE \
    -DLIBPLIST="$deps_prefix/lib/libplist-2.0.a" \
    -DCMAKE_PREFIX_PATH="$deps_prefix;/clang64" \
    -DCMAKE_EXE_LINKER_FLAGS="-static -Wl,--gc-sections" \
    -Wno-dev
cmake --build "$build_dir" -- -k 0
