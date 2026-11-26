#!/bin/sh
set -eu
project=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
ndk=${U6_NDK:-"$project/vendor/ndk"}
clang="$ndk/toolchains/llvm/prebuilt/darwin-x86_64/bin/aarch64-linux-android23-clang"
stock=${U6_STOCK_LINK_LIBS:-"$project/vendor/stock-libs"}
stock_boot=${U6_STOCK_BOOT:-"$project/vendor/boot.fex"}
payload=${1:-"$project/out/deploy-candidate-alsa"}
output=${2:-"$project/out/boot-overlay-candidate"}
mkdir "$output"
python3 "$project/deploy/prepare_overlay.py" "$payload" "$output/transfer" \
    "$output/overlay_manifest.h" --stock "$stock"
"$clang" -std=c11 -Wall -Wextra -Werror -O2 -Wl,--no-undefined -I "$output" \
    "$project/deploy/stage_overlay.c" -ldl -o "$output/stage-overlay"
cat "$project/deploy/sepolicy.rules" "$project/deploy/overlay.policy" > "$output/rules"
if [ -f "$output/transfer/lib-libu6_native_output.so" ]; then
    cat "$project/deploy/native_output.policy" >> "$output/rules"
fi
python3 - "$project" "$output" <<'PY'
import os
from pathlib import Path
import subprocess
from tempfile import TemporaryDirectory
import sys
project, output = (Path(arg).resolve() for arg in sys.argv[1:])
with TemporaryDirectory(prefix="u6-boot-policy-") as temporary:
    staged = Path(temporary) / "compiled"
    rules = str(output / "rules")
    subprocess.run(["sh", project / "deploy/build_candidate.sh", staged],
                   env={**os.environ, "U6_POLICY_RULES": rules}, check=True)
    for name in ("sepolicy", "policy-diff.txt"):
        (output / name).write_bytes((staged / name).read_bytes())
PY
python3 "$project/deploy/build_boot.py" "$stock_boot" "$output/sepolicy" \
    "$output/boot.img" --overlay "$output" > "$output/boot-manifest.json"
(cd "$output" && shasum -a 256 boot.img boot-manifest.json sepolicy policy-diff.txt \
    stage-overlay overlay_manifest.h rules transfer/* > SHA256SUMS)
