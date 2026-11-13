#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Clone the pinned main-branch commits of the native build's dependencies:
# the rows of build_inputs.tsv (next to this script) that name a repository
# and a directory.  The same file is what the program lists in "About".
# Usage: fetch_deps.sh <source_root>
set -euo pipefail
source_root="$1"
inputs="$(dirname "${BASH_SOURCE[0]}")/build_inputs.tsv"
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
fetched=0
# (no field of the list is empty, so that a tab separates exactly two fields)
while IFS=$'\t' read -r kind name check version revision date licence homepage repository directory; do
    case "$kind" in
        '' | '#'*) continue ;;
    esac
    directory="${directory%$'\r'}"
    if [ "$repository" = "-" ] || [ "$directory" = "-" ]; then
        continue
    fi
    fetch "$directory" "$repository" "$revision"
    fetched=$((fetched + 1))
done < "$inputs"
if [ "$fetched" -eq 0 ]; then
    echo "fetch_deps.sh: $inputs names no input to fetch" >&2
    exit 1
fi
