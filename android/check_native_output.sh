#!/bin/sh
set -eu
project=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
adb=${U6_ADB:-adb}
ndk=${U6_NDK:-"$project/vendor/ndk"}
output=${U6_BUILD_DIR:-"$project/out/android"}
toolchain="$ndk/toolchains/llvm/prebuilt/darwin-x86_64/bin"
cc="$toolchain/armv7a-linux-androideabi23-clang"
cxx="$toolchain/armv7a-linux-androideabi23-clang++"

test "$("$adb" -d shell getprop ro.product.device | tr -d '\r')" = tulip-u6
test "$("$adb" -d shell getprop ro.build.display.id | tr -d '\r')" = MOB30J.20191206
test "$("$adb" -d shell service check u6.direct_audio | tr -d '\r')" = 'Service u6.direct_audio: not found'
tmp=$(mktemp -d "${TMPDIR:-/tmp}/u6-native-output.XXXXXX")
device=
cleanup() {
    if [ -n "$device" ]; then
        "$adb" -d shell "rm -f $device/probe $device/libu6_native_output.so $device/libu6_direct_client.so; rmdir $device" >/dev/null 2>&1 || true
    fi
    rm -rf "$tmp"
}
trap cleanup EXIT
"$adb" -d pull /system/lib/libmediaplayerservice.so "$tmp/stock.so" >/dev/null
test "$(shasum -a 256 "$tmp/stock.so" | cut -d' ' -f1)" = 61276ce6610ddbd1f945ad2189550c5819baa8e0085d059d9ac6bdc7be83488c

"$cxx" -std=c++11 -Wall -Wextra -Werror -fno-rtti -fno-exceptions -nostdlib++ \
    -Wl,--no-undefined "$project/tests/native_output_probe.cpp" -ldl -o "$tmp/probe"
"$cc" -std=c11 -Wall -Wextra -Werror -fPIC -shared -Wl,-soname,libu6_direct_client.so \
    -I "$project/src" -I "$project/android" "$project/tests/native_output_mock.c" \
    "$project/android/apple_track.c" "$project/src/pcm_service.c" -lm -o "$tmp/mock.so"
created=$("$adb" -d shell 'mktemp -d /data/local/tmp/u6-native-output.XXXXXX' | tr -d '\r')
case "$created" in /data/local/tmp/u6-native-output.*) ;; *) exit 1 ;; esac
case "$created" in *[!A-Za-z0-9_./-]*) exit 1 ;; esac
device="$created"
"$adb" -d push "$tmp/probe" "$device/probe"
"$adb" -d push "$output/lib/libu6_native_output.so" "$device/libu6_native_output.so"
for mode in missing mock; do
    case "$mode" in
        missing) client="$output/lib/libu6_direct_client.so" ;;
        mock) client="$tmp/mock.so" ;;
    esac
    "$adb" -d push "$client" "$device/libu6_direct_client.so"
    result=$("$adb" -d shell "chmod 700 $device/probe && LD_LIBRARY_PATH=$device LD_PRELOAD=$device/libu6_native_output.so $device/probe $mode; printf '\\nu6_status=%s\\n' \$?" | tr -d '\r')
    printf '%s\n' "$result"
    printf '%s\n' "$result" | grep -qxF "native AudioOutput 44100 Hz format=1 $mode OK"
    printf '%s\n' "$result" | grep -qxF "native AudioOutput 96000 Hz format=3 $mode OK"
    printf '%s\n' "$result" | grep -qxF 'u6_status=0'
done
