#!/bin/sh
set -eu
project=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
adb=${U6_ADB:-adb}
output=${U6_BUILD_DIR:-"$project/out/android"}

prop() { "$adb" -d shell getprop "$1" | tr -d '\r'; }
test "$(prop ro.product.device)" = tulip-u6
test "$(prop ro.build.version.sdk)" = 23
test "$(prop ro.build.display.id)" = MOB30J.20191206

for abi in lib lib64; do
    test -x "$output/$abi/u6-direct-audio"
    test -f "$output/$abi/libu6_direct_client.so"
done
temp_dir=$("$adb" -d shell 'mktemp -d /data/local/tmp/u6-direct-check.XXXXXX' | tr -d '\r')
case "$temp_dir" in
    /data/local/tmp/u6-direct-check.*) ;;
    *) printf '%s\n' 'Could not create a U6 test directory' >&2; exit 1 ;;
esac
case "$temp_dir" in
    *[!A-Za-z0-9_./-]*) exit 1 ;;
esac
trap '"$adb" -d shell "rm -f $temp_dir/service32 $temp_dir/service64 $temp_dir/client32.so $temp_dir/client64.so; rmdir $temp_dir" >/dev/null 2>&1 || true' EXIT

"$adb" -d push "$output/lib/u6-direct-audio" "$temp_dir/service32"
"$adb" -d push "$output/lib64/u6-direct-audio" "$temp_dir/service64"
"$adb" -d push "$output/lib/libu6_direct_client.so" "$temp_dir/client32.so"
"$adb" -d push "$output/lib64/libu6_direct_client.so" "$temp_dir/client64.so"
for arch in 32 64; do
    binary="service$arch"
    result=$("$adb" -d shell "chmod 700 $temp_dir/$binary && $temp_dir/$binary --check $temp_dir/client$arch.so; printf 'u6_status=%s\\n' \$?" | tr -d '\r')
    printf '%s: %s\n' "$binary" "$result"
    printf '%s\n' "$result" | grep -qxF 'u6-direct-audio: original raw symbols resolved'
    printf '%s\n' "$result" | grep -qxF 'u6-direct-audio: client exports resolved'
    printf '%s\n' "$result" | grep -qxF 'u6_status=0'
    volume=$("$adb" -d shell "$temp_dir/$binary --read-volume; printf 'u6_status=%s\\n' \$?" | tr -d '\r')
    printf '%s: %s\n' "$binary" "$volume"
    printf '%s\n' "$volume" | grep -Eq '^u6-volume index=[0-9]+ device=[248] lineout=[01] balance=-?[0-9]+\.[0-9]+ dac=[0-9]+,[0-9]+ muted=[01]$'
    printf '%s\n' "$volume" | grep -qxF 'u6_status=0'
done
