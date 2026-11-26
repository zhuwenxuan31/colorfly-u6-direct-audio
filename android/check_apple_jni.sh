#!/bin/sh
set -eu
project=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
adb=${U6_ADB:-adb}
ndk=${U6_NDK:-"$project/vendor/ndk"}
output=${U6_BUILD_DIR:-"$project/out/android"}
tools_dir="$project/vendor/test-tools"
java="$tools_dir/jdk-17.0.21+9/Contents/Home/bin"
toolchain="$ndk/toolchains/llvm/prebuilt/darwin-x86_64/bin"
startup=false
case "${1:-}" in
    '') ;;
    --startup) startup=true ;;
    *) exit 2 ;;
esac
tmp=$(mktemp -d "${TMPDIR:-/tmp}/u6-jni.XXXXXX")
device=
cleanup() {
    if [ -n "$device" ]; then
        "$adb" -d shell "rm -f $device/probe.jar $device/probe32.so $device/probe64.so \
            $device/client32.so $device/client64.so \
            $device/lib/libandroid_runtime.so $device/lib/libu6_direct_client.so \
            $device/lib64/libandroid_runtime.so $device/lib64/libu6_direct_client.so; \
            rmdir $device/lib $device/lib64 $device 2>/dev/null" >/dev/null 2>&1 || true
    fi
    rm -rf "$tmp"
}
trap cleanup EXIT
test "$("$adb" -d shell getprop ro.product.device | tr -d '\r')" = tulip-u6
test "$("$adb" -d shell getprop ro.build.display.id | tr -d '\r')" = MOB30J.20191206
test "$("$adb" -d shell service check u6.direct_audio | tr -d '\r')" = 'Service u6.direct_audio: not found'

"$java/javac" --release 8 -classpath "$tools_dir/android23.jar" -d "$tmp" "$project/tests/U6BridgeProbe.java"
"$java/java" -cp "$tools_dir/r8-8.3.37.jar" com.android.tools.r8.D8 --min-api 23 \
    --lib "$tools_dir/android23.jar" --output "$tmp/probe.jar" "$tmp/U6BridgeProbe.class"
created=$("$adb" -d shell 'mktemp -d /data/local/tmp/u6-apple-jni.XXXXXX' | tr -d '\r')
case "$created" in
    /data/local/tmp/u6-apple-jni.*) ;;
    *) exit 1 ;;
esac
case "$created" in
    *[!A-Za-z0-9_./-]*) exit 1 ;;
esac
device="$created"
"$adb" -d push "$tmp/probe.jar" "$device/probe.jar"

for arch in 32 64; do
    case "$arch" in
        32)
            abi=lib
            target=armv7a-linux-androideabi23
            expected=722f4f47b055538346c6ed9b6075972cce6211b60835b554d8a17c67068ae6fa
            ;;
        64)
            abi=lib64
            target=aarch64-linux-android23
            expected=7124ffab4ef9a1412223f70f49ec7f33469e3e14f320a9ea077c141a82711910
            ;;
    esac
    "$adb" -d pull "/system/$abi/libandroid_runtime.so" "$tmp/runtime.so" >/dev/null
    test "$(shasum -a 256 "$tmp/runtime.so" | cut -d' ' -f1)" = "$expected"
    if "$startup"; then
        "$adb" -d shell "mkdir $device/$abi" >/dev/null
        runtime="$device/$abi/libandroid_runtime.so"
        client="$device/$abi/libu6_direct_client.so"
        "$adb" -d push "$output/$abi/startup/libandroid_runtime.so" "$runtime"
        for mode in fallback stock direct; do
            name=local.u6.bridge_probe
            expectation=stock
            content=3
            if [ "$mode" = fallback ]; then
                name=com.apple.android.music:u6_bridge_probe
            elif [ "$mode" = stock ]; then
                "$adb" -d push "$output/$abi/libu6_direct_client.so" "$client"
            else
                name=com.apple.android.music:u6_bridge_probe
                expectation=direct
                content=0
            fi
            result=$("$adb" -d shell "LD_LIBRARY_PATH=$device/$abi CLASSPATH=$device/probe.jar /system/bin/app_process$arch /system/bin --nice-name=$name U6BridgeProbe startup $runtime $expectation 1 $content; printf '\\nu6_status=%s\\n' \$?" | tr -d '\r')
            printf '%s startup %s\n%s\n' "$arch" "$mode" "$result"
            printf '%s\n' "$result" | grep -qxF "U6 probe $expectation 48000 OK"
            printf '%s\n' "$result" | grep -qxF "U6 probe $expectation 192000 OK"
            printf '%s\n' "$result" | grep -qxF 'u6_status=0'
        done
        continue
    fi
    "$toolchain/$target-clang++" -std=c++11 -Wall -Wextra -Werror -shared -nostdlib++ \
        -Wl,--no-undefined "$project/tests/apple_jni_probe.cpp" -ldl -o "$tmp/probe$arch.so"
    "$adb" -d push "$tmp/probe$arch.so" "$device/probe$arch.so"
    "$adb" -d push "$output/$abi/libu6_direct_client.so" "$device/client$arch.so"
    for mode in stock direct; do
        name=local.u6.bridge_probe
        content=3
        if [ "$mode" = direct ]; then content=0; fi
        result=$("$adb" -d shell "CLASSPATH=$device/probe.jar /system/bin/app_process$arch /system/bin --nice-name=$name U6BridgeProbe $device/probe$arch.so $device/client$arch.so $mode 1 $content; printf '\\nu6_status=%s\\n' \$?" | tr -d '\r')
        printf '%s %s\n%s\n' "$arch" "$mode" "$result"
        printf '%s\n' "$result" | grep -qxF "U6 probe $mode 48000 OK"
        printf '%s\n' "$result" | grep -qxF "U6 probe $mode 192000 OK"
        printf '%s\n' "$result" | grep -qxF 'u6_status=0'
    done
done
