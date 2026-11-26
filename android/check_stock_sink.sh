#!/bin/sh
set -eu
project=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
adb=${U6_ADB:-adb}
ndk=${U6_NDK:-"$project/vendor/ndk"}
toolchain="$ndk/toolchains/llvm/prebuilt/darwin-x86_64/bin"

test "$("$adb" -d shell getprop ro.product.device | tr -d '\r')" = tulip-u6
test "$("$adb" -d shell getprop ro.build.display.id | tr -d '\r')" = MOB30J.20191206
local_tmp=$(mktemp -d "${TMPDIR:-/tmp}/u6-stock-sink.XXXXXX")
remote=
cleanup() {
    if [ -n "$remote" ]; then
        "$adb" -d shell "rm -f $remote/probe32 $remote/probe64; rmdir $remote" >/dev/null 2>&1 || true
    fi
    rm -rf "$local_tmp"
}
trap cleanup EXIT

created=$("$adb" -d shell 'mktemp -d /data/local/tmp/u6-stock-sink.XXXXXX' | tr -d '\r')
case "$created" in /data/local/tmp/u6-stock-sink.*) ;; *) exit 1 ;; esac
case "$created" in *[!A-Za-z0-9_./-]*) exit 1 ;; esac
remote="$created"
for bits in 32 64; do
    case "$bits" in
        32)
            abi=lib
            target=armv7a-linux-androideabi23
            expected=8b047b77900dd88ceca5ce06885db8a3a467671dd48a38d88f91966a5966116f
            ;;
        64)
            abi=lib64
            target=aarch64-linux-android23
            expected=5e3276305488b91fee1460d5af325bff5af090c82a0886e8b1d3432343e1ec3e
            ;;
    esac
    "$adb" -d pull "/system/$abi/libplayer.so" "$local_tmp/libplayer.so" >/dev/null
    test "$(shasum -a 256 "$local_tmp/libplayer.so" | cut -d' ' -f1)" = "$expected"
    "$toolchain/$target-clang++" -std=c++11 -Wall -Wextra -Werror -nostdlib++ \
        -Wl,--no-undefined "$project/tests/u6_stock_sink_probe.cpp" -ldl -o "$local_tmp/probe"
    "$adb" -d push "$local_tmp/probe" "$remote/probe$bits"
    result=$("$adb" -d shell "chmod 700 $remote/probe$bits && $remote/probe$bits; printf '\\nu6_status=%s\\n' \$?" | tr -d '\r')
    printf '%s\n' "$result"
    printf '%s\n' "$result" | grep -qxF 'U6 stock AudioSink 44100 Hz format=1 lifecycle OK'
    printf '%s\n' "$result" | grep -qxF 'U6 stock AudioSink 96000 Hz format=1 lifecycle OK'
    printf '%s\n' "$result" | grep -qxF 'u6_status=0'
done
