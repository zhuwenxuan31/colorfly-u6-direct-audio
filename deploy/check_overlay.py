"""Exercise the staged overlay on a stock U6. Every device path is temporary."""
import argparse
import os
from pathlib import Path
import re
import subprocess
from tempfile import TemporaryDirectory

from prepare_overlay import FILES, NATIVE_OUTPUT

PROJECT = Path(__file__).resolve().parents[1]
DEFAULT_ADB = Path("adb")
DEFAULT_NDK = PROJECT / "vendor/ndk"
TOOLS = PROJECT / "vendor/test-tools"
JAVA = TOOLS / "jdk-17.0.21+9/Contents/Home/bin"


def check(args, host):
    def adb(*command, capture=False, binary=False):
        result = subprocess.run([args.adb, "-d", *command], check=True, capture_output=capture,
                                text=not binary)
        return result.stdout

    def shell(command):
        return adb("shell", command, capture=True).replace("\r", "").strip()

    def run(command, expected=0):
        result = shell(f"{command}; printf '\\nu6_status=%s\\n' $?")
        if not result.endswith(f"u6_status={expected}"):
            raise RuntimeError(result)
        return result.rsplit("\nu6_status=", 1)[0]

    assert shell("getprop ro.product.device") == "tulip-u6"
    assert shell("getprop ro.build.display.id") == "MOB30J.20191206"
    assert shell("service check u6.direct_audio") == "Service u6.direct_audio: not found"

    bundle = args.boot_overlay
    if bundle is None:
        bundle = host / "bundle"
        subprocess.run(["sh", PROJECT / "deploy/build_overlay.sh", args.candidate, bundle],
                       env={**os.environ, "U6_NDK": str(args.ndk)}, check=True)
    else:
        subprocess.run(["shasum", "-a", "256", "-c", "SHA256SUMS"], cwd=bundle, check=True)
    transfer = bundle / "transfer"
    helper = bundle / "stage-overlay"
    native = (transfer / NATIVE_OUTPUT.replace("/", "-")).is_file()
    files = FILES + ([NATIVE_OUTPUT] if native else [])
    if native:
        cxx = (args.ndk / "toolchains/llvm/prebuilt/darwin-x86_64/bin"
               / "armv7a-linux-androideabi23-clang++")
        subprocess.run([cxx, "-std=c++11", "-Wall", "-Wextra", "-Werror", "-fno-rtti",
                        "-fno-exceptions", "-nostdlib++", "-Wl,--no-undefined",
                        PROJECT / "tests/native_output_probe.cpp", "-ldl", "-o",
                        host / "native-probe"], check=True)
    subprocess.run([JAVA / "javac", "--release", "8", "-classpath", TOOLS / "android23.jar",
                    "-d", host, PROJECT / "tests/U6BridgeProbe.java"], check=True)
    probe = host / "probe.jar"
    subprocess.run([JAVA / "java", "-cp", TOOLS / "r8-8.3.37.jar", "com.android.tools.r8.D8",
                    "--min-api", "23", "--lib", TOOLS / "android23.jar",
                    "--output", probe, host / "U6BridgeProbe.class"], check=True)

    device = shell("mktemp -d /data/local/tmp/u6-overlay.XXXXXX")
    if not re.fullmatch(r"/data/local/tmp/u6-overlay\.[A-Za-z0-9]+", device):
        raise RuntimeError("failed to create a private U6 test directory")
    source, overlay = f"{device}/source", f"{device}/ready"
    try:
        run(f"mkdir {source}")
        adb("push", f"{transfer}/.", source)
        adb("push", helper, f"{device}/prepare")
        adb("push", probe, f"{device}/probe.jar")
        run(f"chmod 700 {device}/prepare && {device}/prepare {source} {overlay}")

        for name in files:
            actual = adb("exec-out", "cat", f"{overlay}/{name}", capture=True, binary=True)
            if actual != (args.candidate / "system" / name).read_bytes():
                raise RuntimeError(f"{name}: staged file differs")
        assert run(f"LD_LIBRARY_PATH={overlay}/lib64 {overlay}/bin/u6-direct-audio --check").find(
            "u6-direct-audio: ALSA ownership gate resolved") != -1
        if native:
            adb("push", host / "native-probe", f"{device}/native-probe")
            result = run(f"chmod 700 {device}/native-probe && LD_LIBRARY_PATH={overlay}/lib "
                         f"LD_PRELOAD={overlay}/{NATIVE_OUTPUT} {device}/native-probe missing")
            for rate, encoding in ((44100, 1), (96000, 3)):
                assert f"native AudioOutput {rate} Hz format={encoding} missing OK" in result.splitlines()
            print("ARMv7 native output loaded from the reconstructed overlay", flush=True)

        for abi, bits in (("lib", 32), ("lib64", 64)):
            runtime = f"{overlay}/{abi}/libandroid_runtime.so"
            cases = [("stock", "local.u6.overlay_probe", 1, 3),
                     ("direct", "com.apple.android.music:u6_overlay_probe", 1, 0)]
            if args.music_scope:
                cases += [("direct", "com.accusilicon.musicplayer:u6_overlay_probe", 1, 0),
                          ("direct", "local.u6.other_music_probe", 1, 2),
                          ("direct", "local.u6.other_music_probe", 1, 0),
                          ("direct", "local.u6.other_music_probe", 0, 0),
                          ("stock", "local.u6.notification_probe", 5, 4),
                          ("stock", "com.apple.android.music:u6_overlay_probe", 1, 3),
                          ("stock", "local.u6.game_probe", 14, 2),
                          ("stock", "local.u6.speech_probe", 1, 1)]
            for mode, name, usage, content in cases:
                result = run(f"LD_LIBRARY_PATH={overlay}/{abi} CLASSPATH={device}/probe.jar "
                             f"/system/bin/app_process{bits} /system/bin --nice-name={name} "
                             f"U6BridgeProbe startup {runtime} {mode} {usage} {content}")
                for rate in (48000, 192000):
                    if f"U6 probe {mode} {rate} OK" not in result.splitlines():
                        raise RuntimeError(result)
                print(f"{abi}: {name} usage={usage} content={content} → {mode}", flush=True)

        file = "lib64-libtinyalsa.so"
        data = bytearray((transfer / file).read_bytes())
        data[len(data) // 2] ^= 1
        tampered = host / "tampered"
        tampered.write_bytes(data)
        adb("push", tampered, f"{source}/{file}")
        assert "u6 overlay rejected" in run(
            f"{device}/prepare {source} {device}/rejected", expected=1)
        run(f"test ! -e {device}/rejected")
        print("Same-size tampered library rejected without publishing the overlay", flush=True)
    finally:
        paths = [f"{source}/{path.name}" for path in transfer.iterdir()]
        paths += [f"{overlay}/{name}" for name in files]
        paths += [f"{device}/prepare", f"{device}/probe.jar", f"{device}/native-probe"]
        shell("rm -f " + " ".join(paths))
        shell(f"rmdir {overlay}/bin {overlay}/lib {overlay}/lib64 {overlay} {source} {device} 2>/dev/null || true")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("candidate", type=Path)
    parser.add_argument("--music-scope", action="store_true")
    parser.add_argument("--boot-overlay", type=Path)
    parser.add_argument("--adb", type=Path, default=DEFAULT_ADB)
    parser.add_argument("--ndk", type=Path, default=DEFAULT_NDK)
    args = parser.parse_args()
    with TemporaryDirectory(prefix="u6-overlay-") as host:
        check(args, Path(host))


if __name__ == "__main__":
    main()
