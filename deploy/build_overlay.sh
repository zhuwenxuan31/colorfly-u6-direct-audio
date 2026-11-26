#!/bin/sh
set -eu
project=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
ndk=${U6_NDK:-"$project/vendor/ndk"}
clang="$ndk/toolchains/llvm/prebuilt/darwin-x86_64/bin/aarch64-linux-android23-clang"
candidate=${1:-"$project/out/deploy-candidate-alsa"}
output=${2:-"$project/out/overlay-verified"}

mkdir "$output"
python3 "$project/deploy/prepare_overlay.py" "$candidate" "$output/transfer" "$output/overlay_manifest.h"
"$clang" -std=c11 -Wall -Wextra -Werror -O2 -Wl,--no-undefined -I "$output" \
    "$project/deploy/stage_overlay.c" -ldl -o "$output/stage-overlay"
(cd "$output" && shasum -a 256 stage-overlay overlay_manifest.h transfer/* > SHA256SUMS)
