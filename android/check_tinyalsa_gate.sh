#!/bin/sh
set -eu
project=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
adb=${U6_ADB:-adb}
ndk=${U6_NDK:-"$project/vendor/ndk"}
output=${U6_BUILD_DIR:-"$project/out/android"}
toolchain="$ndk/toolchains/llvm/prebuilt/darwin-x86_64/bin"

test "$("$adb" -d shell getprop ro.product.device | tr -d '\r')" = tulip-u6
test "$("$adb" -d shell getprop ro.build.display.id | tr -d '\r')" = MOB30J.20191206
temp_host=$(mktemp -d "${TMPDIR:-/tmp}/u6-gate.XXXXXX")
temp_device=
cleanup() {
    if [ -n "$temp_device" ]; then
        for abi in lib lib64; do
            dir="$temp_device/$abi"
            "$adb" -d shell "rm -f $dir/libtinyalsa.so $dir/libu6_alsa.so $dir/service $dir/probe $dir/board.lock; rmdir $dir" >/dev/null 2>&1 || true
        done
        "$adb" -d shell "rmdir $temp_device" >/dev/null 2>&1 || true
    fi
    rm -rf "$temp_host"
}
trap cleanup EXIT
created=$("$adb" -d shell 'mktemp -d /data/local/tmp/u6-gate.XXXXXX' | tr -d '\r')
case "$created" in /data/local/tmp/u6-gate.*) ;; *) exit 1 ;; esac
case "$created" in *[!A-Za-z0-9_./-]*) exit 1 ;; esac
temp_device="$created"

for abi in lib lib64; do
    case "$abi" in
        lib) target=armv7a-linux-androideabi23 ;;
        lib64) target=aarch64-linux-android23 ;;
    esac
    cc="$toolchain/$target-clang"
    dir="$temp_device/$abi"
    "$adb" -d shell "mkdir $dir"
    "$adb" -d push "$output/$abi/gate/libtinyalsa.so" "$dir/libtinyalsa.so"
    "$adb" -d push "$output/$abi/gate/libu6_alsa.so" "$dir/libu6_alsa.so"
    "$adb" -d push "$output/$abi/u6-direct-audio" "$dir/service"
    "$adb" -d shell "chmod 700 $dir/service"
    result=$("$adb" -d shell "LD_LIBRARY_PATH=$dir $dir/service --check; printf '\\nu6_status=%s\\n' \$?" | tr -d '\r')
    printf '%s production gate\n%s\n' "$abi" "$result"
    printf '%s\n' "$result" | grep -qxF 'u6-direct-audio: original raw symbols resolved'
    printf '%s\n' "$result" | grep -qxF 'u6-direct-audio: ALSA ownership gate resolved'
    printf '%s\n' "$result" | grep -qxF u6_status=0

    # Build a test-only gate with a lock file writable by the ADB shell.
    "$cc" -std=c11 -Wall -Wextra -Werror -fPIC -shared -Wl,--no-undefined \
        -Wl,-soname,libtinyalsa.so -I "$project/src" "-DU6_ALSA_LOCK_PATH=\"$dir/board.lock\"" \
        "$project/android/tinyalsa_gate.c" "$output/$abi/u6_lease.o" \
        -L "$output/link-only/$abi/gate" -Wl,--no-as-needed -l:libu6_alsa.so -ldl \
        -o "$temp_host/libtinyalsa.so"
    "$cc" -std=c11 -Wall -Wextra -Werror -I "$project/src" \
        "$project/tests/tinyalsa_gate_probe.c" -ldl -o "$temp_host/probe"
    "$adb" -d push "$temp_host/libtinyalsa.so" "$dir/libtinyalsa.so"
    "$adb" -d push "$temp_host/probe" "$dir/probe"
    "$adb" -d shell "chmod 700 $dir/probe"
    result=$("$adb" -d shell "LD_LIBRARY_PATH=$dir $dir/probe $dir/board.lock; printf '\\nu6_status=%s\\n' \$?" | tr -d '\r')
    printf '%s contention probe\n%s\n' "$abi" "$result"
    printf '%s\n' "$result" | grep -qxF 'stock ALSA symbols and cross-process refusal verified'
    printf '%s\n' "$result" | grep -qxF u6_status=0
done
