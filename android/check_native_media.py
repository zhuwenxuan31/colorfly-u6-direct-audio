"""Play two near-silent WAVs through stock Android MediaPlayer and capture its native route."""
import argparse
import json
from pathlib import Path
import re
import subprocess
from tempfile import TemporaryDirectory
from time import sleep
import wave

ROOT = Path(__file__).resolve().parents[1]
TOOLS = ROOT / "vendor/test-tools"
JAVA = TOOLS / "jdk-17.0.21+9/Contents/Home/bin"
DEFAULT_ADB = Path("adb")
CARD = "/proc/asound/card1/pcm0p/sub0"
WAV_BITS = {44100: 16, 96000: 24, 192000: 24}


def execute(adb, host, direct=False, rates=(44100, 96000)):
    if direct:
        from check_live_direct import capture, evaluate

    def call(*args):
        return subprocess.run([adb, "-d", *args], text=True, capture_output=True,
                              timeout=10, check=True).stdout.replace("\r", "")

    def shell(command):
        return call("shell", command).strip()

    if shell("getprop ro.product.device") != "tulip-u6":
        raise RuntimeError("wrong device")
    if shell("getprop ro.build.display.id") != "MOB30J.20191206" or shell(f"cat {CARD}/hw_params") != "closed":
        raise RuntimeError("expected idle U6 running the 2.0 build")
    if direct and shell("service check u6.direct_audio") != "Service u6.direct_audio: found":
        raise RuntimeError("direct audio service is not registered")

    subprocess.run([JAVA / "javac", "--release", "8", "-classpath", TOOLS / "android23.jar",
                    "-d", host, ROOT / "tests/U6MediaPlayerProbe.java"], check=True)
    jar = host / "probe.jar"
    subprocess.run([JAVA / "java", "-cp", TOOLS / "r8-8.3.37.jar", "com.android.tools.r8.D8",
                    "--min-api", "23", "--lib", TOOLS / "android23.jar", "--output", jar,
                    host / "U6MediaPlayerProbe.class"], check=True)

    device = shell("mktemp -d /data/local/tmp/u6-native-media.XXXXXX")
    if not re.fullmatch(r"/data/local/tmp/u6-native-media\.[A-Za-z0-9]+", device):
        raise RuntimeError("unexpected test directory")
    results = []
    media = None
    filenames = [f"{rate}-{WAV_BITS[rate]}.wav" for rate in rates]
    try:
        path_on_u6 = shell("mktemp -d /sdcard/.u6-native-media.XXXXXX")
        if not re.fullmatch(r"/sdcard/\.u6-native-media\.[A-Za-z0-9]+", path_on_u6):
            raise RuntimeError("unexpected media test directory")
        media = path_on_u6
        call("push", jar, f"{device}/probe.jar")
        for rate, filename in zip(rates, filenames):
            bits = WAV_BITS[rate]
            path = host / filename
            with wave.open(str(path), "wb") as audio:
                audio.setparams((2, bits // 8, rate, 0, "NONE", "not compressed"))
                quiet = (1).to_bytes(bits // 8, "little", signed=True)
                audio.writeframes(quiet * rate * 12)
            call("push", path, f"{media}/{filename}")
            label = f"{device.rsplit('.', 1)[1]}_{rate}"
            attributes = " music-attributes" if direct else ""
            proc = subprocess.Popen(
                [adb, "-d", "shell", f"CLASSPATH={device}/probe.jar /system/bin/app_process64 "
                 f"/system/bin U6MediaPlayerProbe {media}/{filename} {label}{attributes}; printf '\\nu6_status=%s\\n' $?"],
                text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            )
            try:
                sleep(0.4)
                for _ in range(10):
                    params = shell(f"cat {CARD}/hw_params")
                    status = shell(f"cat {CARD}/status")
                    if f"rate: {rate} " in params and "RUNNING" in status:
                        break
                    sleep(0.14)
                flinger = shell("dumpsys media.audio_flinger")
                if direct:
                    first = capture(shell)
                    sleep(0.5)
                    second = capture(shell)
            finally:
                output, _ = proc.communicate(timeout=12)
            player_ok = "U6 native WAV probe OK" in output and "u6_status=0" in output
            if direct:
                checks = evaluate(first, second, rate, "S16_LE" if bits == 16 else "S32_LE",
                                  registered=True)
                checks["player_completed"] = player_ok
                result = {"source_rate": rate, "source_bits": bits, "checks": checks,
                          "samples": [first, second], "player_output": output.strip()}
                if not all(checks.values()):
                    raise RuntimeError(json.dumps(result))
                results.append(result)
                continue
            if not player_ok:
                raise RuntimeError(output)
            log = shell("logcat -d -t 350").split("BEGIN " + label, 1)
            logs = [] if len(log) == 1 else [
                line for line in log[1].splitlines()
                if re.search(r"CedarX|awplayer|SoundDevice|AudioTrack|AudioPolicy|AudioFlinger"
                             r"|MediaPlayerService|MediaPlayerFactory|NuPlayer", line)
            ]
            configuration = f"SamplingRate {rate}, Format 0x{(1 if bits == 16 else 3):06x}"
            if not any("AudioFlinger: openOutput()" in line and configuration in line for line in logs):
                raise RuntimeError(f"did not capture native request for {rate} Hz / {bits} bits")
            tid = re.search(r"owner_pid\s*:\s*(\d+)", status)
            starts = list(re.finditer(r"^Output thread .*", flinger, re.MULTILINE))
            route = []
            for i, found in enumerate(starts):
                segment = flinger[found.start():starts[i+1].start() if i+1 < len(starts) else len(flinger)]
                if tid and re.search(rf"\b{tid[1]}\b", segment):
                    route = [line for line in segment.splitlines() if
                             re.search(r"(?:Thread|format|rate|Track|\b" + str(rate) + r"\b)", line, re.I)][:24]
                    break
            results.append({"source_rate": rate, "source_bits": bits, "hw_params": params,
                            "status": status, "route": route, "native_logs": logs})
        return results
    finally:
        if media:
            files = " ".join(f"{media}/{filename}" for filename in filenames)
            shell(f"rm -f {files}; rmdir {media}")
        shell(f"rm -f {device}/probe.jar; rmdir {device}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--adb", type=Path, default=DEFAULT_ADB)
    parser.add_argument("--direct", action="store_true", help="verify the boot candidate's DAC route")
    parser.add_argument("--rate", type=int, choices=WAV_BITS, action="append",
                        help="select a WAV sample rate; may be specified more than once")
    args = parser.parse_args()
    with TemporaryDirectory(prefix="u6-native-media-") as tmp:
        print(json.dumps(execute(args.adb, Path(tmp), direct=args.direct,
                                 rates=args.rate or (44100, 96000)), indent=2))


if __name__ == "__main__":
    main()
