#!/bin/sh
set -eu

project=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
output=${U6_BUILD_DIR:-"$project/out/android"}
stock_boot=${U6_STOCK_BOOT:-"$project/vendor/boot.fex"}
stock_policy=${U6_STOCK_POLICY:-"$project/vendor/sepolicy"}
adb=${U6_ADB:-adb}
policy_tool=${U6_POLICY_TOOL:-"$project/out/tools/magisk-v27/magiskpolicy"}
rules=${U6_POLICY_RULES:-"$project/deploy/sepolicy.rules"}
candidate=${1:-"$project/out/deploy-candidate"}
stock_sha=c51a5d6733acd7283391edaef4982276ca079e70a4f73a7517004ae877a36a30
tool_sha=e1c4a2677b78c7fc79771b804bcd97ccb6d960bcad5d539b56e783d8c59cc0f5

test ! -e "$candidate"
test "$(shasum -a 256 "$stock_policy" | cut -d' ' -f1)" = "$stock_sha"
test "$(shasum -a 256 "$policy_tool" | cut -d' ' -f1)" = "$tool_sha"
test "$("$adb" -d shell getprop ro.product.device | tr -d '\r')" = tulip-u6
test "$("$adb" -d shell getprop ro.build.display.id | tr -d '\r')" = MOB30J.20191206

local_tmp=$(mktemp -d "${TMPDIR:-/tmp}/u6-deploy.XXXXXX")
device_tmp=
cleanup() {
    if [ -n "$device_tmp" ]; then
        "$adb" -d shell "rm -f $device_tmp/tool $device_tmp/stock $device_tmp/patch $device_tmp/candidate; rmdir $device_tmp" >/dev/null 2>&1 || true
    fi
    rm -rf "$local_tmp"
}
trap cleanup EXIT
"$adb" -d pull /sepolicy "$local_tmp/device-policy" >/dev/null
cmp "$stock_policy" "$local_tmp/device-policy"
created=$("$adb" -d shell 'mktemp -d /data/local/tmp/u6-policy.XXXXXX' | tr -d '\r')
case "$created" in /data/local/tmp/u6-policy.*) ;; *) exit 1 ;; esac
case "$created" in *[!A-Za-z0-9_./-]*) exit 1 ;; esac
device_tmp="$created"
"$adb" -d push "$policy_tool" "$device_tmp/tool"
"$adb" -d push "$stock_policy" "$device_tmp/stock"
"$adb" -d push "$rules" "$device_tmp/patch"

run_policy() {
    "$adb" -d shell "$1; printf '\\nu6_status=%s\\n' \$?" | tr -d '\r' > "$2"
    grep -qxF u6_status=0 "$2"
}
run_policy "chmod 700 $device_tmp/tool && $device_tmp/tool --load $device_tmp/stock --apply $device_tmp/patch --save $device_tmp/candidate" "$local_tmp/compile.log"
test "$(sed '/^$/d' "$local_tmp/compile.log")" = u6_status=0
run_policy "$device_tmp/tool --load $device_tmp/stock --print-rules" "$local_tmp/stock.rules"
run_policy "$device_tmp/tool --load $device_tmp/candidate --print-rules" "$local_tmp/candidate.rules"
"$adb" -d pull "$device_tmp/candidate" "$local_tmp/sepolicy" >/dev/null

stage="$local_tmp/stage"
mkdir -p "$stage/system/bin" "$stage/system/lib" "$stage/system/lib64"
python3 "$project/deploy/verify_policy.py" "$rules" \
    "$local_tmp/stock.rules" "$local_tmp/candidate.rules" > "$stage/policy-diff.txt"
python3 "$project/deploy/build_boot.py" "$stock_boot" "$local_tmp/sepolicy" "$stage/boot.img" > "$stage/boot-manifest.json"
cp "$local_tmp/sepolicy" "$stage/sepolicy"
cp "$output/runtime-candidate.json" "$stage/runtime-candidate.json"
for abi in lib lib64; do
    cp "$output/$abi/startup/libandroid_runtime.so" "$stage/system/$abi/libandroid_runtime.so"
    cp "$output/$abi/libu6_direct_client.so" "$stage/system/$abi/libu6_direct_client.so"
    cp "$output/$abi/gate/libtinyalsa.so" "$stage/system/$abi/libtinyalsa.so"
    cp "$output/$abi/gate/libu6_alsa.so" "$stage/system/$abi/libu6_alsa.so"
    chmod 0644 "$stage/system/$abi/"*.so
done
if [ "${U6_NATIVE_OUTPUT:-0}" = 1 ]; then
    cp "$output/lib/libu6_native_output.so" "$stage/system/lib/libu6_native_output.so"
    chmod 0644 "$stage/system/lib/libu6_native_output.so"
fi
cp "$output/lib64/u6-direct-audio" "$stage/system/bin/u6-direct-audio"
chmod 0755 "$stage/system/bin/u6-direct-audio"
(cd "$stage" && shasum -a 256 boot.img sepolicy boot-manifest.json runtime-candidate.json policy-diff.txt system/*/* > SHA256SUMS)
mv "$stage" "$candidate"
printf '%s\n' "$candidate"
