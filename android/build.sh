#!/bin/sh
set -eu
project=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
ndk=${U6_NDK:-"$project/vendor/ndk"}
headers=${U6_AOSP_HEADERS:-"$project/vendor/aosp"}
stock=${U6_STOCK_LINK_LIBS:-"$project/vendor/stock-libs"}
output=${U6_BUILD_DIR:-"$project/out/android"}

test -f "$headers/native/binder/Parcel.h"
test -f "$headers/core/utils/RefBase.h"
test -f "$headers/kernel/linux/binder.h"
(cd "$stock" && shasum -a 256 -c SHA256SUMS)
link_only="$output/link-only"
python_deps=${U6_PYTHON_DEPS:-""}
PYTHONPATH="$python_deps" \
    python3 "$project/android/prepare_link_libraries.py" "$stock" "$link_only"

toolchain="$ndk/toolchains/llvm/prebuilt/darwin-x86_64/bin"
for abi in lib lib64; do
    case "$abi" in
        lib) target=armv7a-linux-androideabi23 ;;
        lib64) target=aarch64-linux-android23 ;;
    esac
    cc="$toolchain/$target-clang"
    cxx="$toolchain/$target-clang++"
    dir="$output/$abi"
    mkdir -p "$dir"
    for module in pcm_service pcm_session u6_alsa u6_board u6_raw u6_volume u6_lease; do
        "$cc" -std=c11 -Wall -Wextra -Werror -fPIC -I "$project/src" \
            -c "$project/src/$module.c" -o "$dir/$module.o"
    done
    "$cc" -std=c11 -Wall -Wextra -Werror -fPIC -I "$project/src" \
        -c "$project/android/apple_track.c" -o "$dir/apple_track.o"
    for module in service_main direct_client apple_audiotrack_jni; do
        "$cxx" -std=c++11 -Wall -Wextra -Werror -fPIC -fno-exceptions -fno-rtti \
            -I "$project/src" -isystem "$headers/kernel" -isystem "$headers/native" -isystem "$headers/core" \
            -c "$project/android/$module.cpp" -o "$dir/$module.o"
    done
    "$cxx" -nostdlib++ -Wl,--no-undefined -L "$link_only/$abi" \
        "$dir/service_main.o" "$dir/pcm_service.o" "$dir/pcm_session.o" "$dir/u6_alsa.o" "$dir/u6_board.o" "$dir/u6_raw.o" "$dir/u6_volume.o" "$dir/u6_lease.o" \
        -lbinder -lutils -llog -ltinyalsa -l:libc++.so -ldl -lm -o "$dir/u6-direct-audio"
    "$cxx" -shared -nostdlib++ -Wl,--no-undefined -L "$link_only/$abi" \
        "$dir/direct_client.o" "$dir/apple_track.o" "$dir/apple_audiotrack_jni.o" "$dir/pcm_service.o" "$dir/pcm_session.o" \
        -lbinder -lutils -llog -l:libc++.so -ldl -lm -o "$dir/libu6_direct_client.so"
    symbols=$("$toolchain/llvm-nm" -D --defined-only "$dir/u6-direct-audio" "$dir/libu6_direct_client.so")
    if printf '%s\n' "$symbols" | grep -Eq ' [A-Za-z] pcm_(start|stop)$'; then
        printf '%s\n' "U6 binaries override a stock tinyalsa trigger symbol ($abi)" >&2
        exit 1
    fi
    mkdir -p "$dir/gate" "$link_only/$abi/gate"
    PYTHONPATH="$python_deps" python3 "$project/android/rename_tinyalsa.py" \
        "$stock/$abi/libtinyalsa.so" "$dir/gate/libu6_alsa.so"
    PYTHONPATH="$python_deps" python3 "$project/android/rename_tinyalsa.py" \
        "$link_only/$abi/libtinyalsa.so" "$link_only/$abi/gate/libu6_alsa.so"
    "$cc" -std=c11 -Wall -Wextra -Werror -fPIC -shared -Wl,--no-undefined \
        -Wl,-soname,libtinyalsa.so -I "$project/src" "$project/android/tinyalsa_gate.c" "$dir/u6_lease.o" \
        -L "$link_only/$abi/gate" -Wl,--no-as-needed -l:libu6_alsa.so -ldl -o "$dir/gate/libtinyalsa.so"
    if [ "$abi" = lib ]; then
        "$cxx" -std=c++11 -Wall -Wextra -Werror -fPIC -fno-exceptions -fno-rtti \
            -I "$project/src" -shared -nostdlib++ -Wl,--no-undefined -Wl,-soname,libu6_native_output.so \
            "$project/android/native_audio_output.cpp" -L "$dir" -l:libu6_direct_client.so -ldl \
            -o "$dir/libu6_native_output.so"
    fi
    file "$dir/u6-direct-audio" "$dir/libu6_direct_client.so"
done

PYTHONPATH="$python_deps" python3 "$project/android/build_runtime_candidate.py" \
    "$stock" "$output" "$toolchain" > "$output/runtime-candidate.json"
