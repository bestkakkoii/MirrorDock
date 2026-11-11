#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Clone the pinned main-branch commits of the native build's dependencies.
# Usage: fetch_deps.sh <source_root>
set -euo pipefail
source_root="$1"
mkdir -p "$source_root"
fetch()
{
    local name="$1" url="$2" commit="$3"
    if [ ! -d "$source_root/$name/.git" ]; then
        git clone --filter=blob:none "$url" "$source_root/$name"
    fi
    git -C "$source_root/$name" fetch --depth 1 origin "$commit"
    git -C "$source_root/$name" checkout --detach "$commit"
}
# observed 2026-11-12 (main development branches)
fetch ffmpeg   https://github.com/FFmpeg/FFmpeg.git               7b00cbd7edf64c439209fb473dd21943c49a7178
fetch openssl  https://github.com/openssl/openssl.git             df281a12096f3ac80272444b128135d065ac3e4d
fetch libplist https://github.com/libimobiledevice/libplist.git   3cfe22475244cc1e924b6fd189234c130d6c5914
