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
# observed 2026-11-02 (main development branches)
fetch ffmpeg   https://github.com/FFmpeg/FFmpeg.git               a88838af482100fa22dd22c94be592c35642de46
fetch openssl  https://github.com/openssl/openssl.git             aad00a888ccb47ec49afeb7545f2dddf9397fbb5
fetch libplist https://github.com/libimobiledevice/libplist.git   fe3dc34dc3484006e12b403f7ceb06f0ad40b6f4
