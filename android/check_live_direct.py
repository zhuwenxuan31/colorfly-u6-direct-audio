"""Capture read-only U6 evidence for one manually selected offline playback sample."""
import argparse
import json
from pathlib import Path
import re
import subprocess
from time import sleep

from check_native_media import CARD, DEFAULT_ADB

DAEMON = "/u6-payload/bin-u6-direct-audio"
WIDTHS = {"S16_LE": 16, "S32_LE": 32}


def fields(text):
    return {key.strip(): value.strip()
            for line in text.splitlines() if ":" in line
            for key, value in [line.split(":", 1)]}


def tracks(flinger):
    outputs = re.split(r"(?m)^Output thread ", flinger)[1:]
    if not outputs:
        raise ValueError("AudioFlinger output threads were not found")
    result = []
    for output in outputs:
        kind = output.splitlines()[0]
        table = re.search(r"(?m)^  (\d+) Tracks(?: of which (\d+) are active)?$", output)
        if table is None or (int(table[1]) and table[2] is None):
            raise ValueError(f"unrecognized AudioFlinger track count for {kind}")
        count = int(table[1])
        rows = output[table.end():].splitlines()[2:2 + count]
        if count and not output[table.end():].splitlines()[1].split()[:4] == ["Name", "Active", "Client", "Type"]:
            raise ValueError(f"unrecognized AudioFlinger track columns for {kind}")
        parsed = [re.match(r"^ +\S+ +(yes|no) +(\d+) +(\d+) +[0-9a-fA-F]{8} ", row)
                  for row in rows]
        if len(parsed) != count or any(row is None for row in parsed):
            raise ValueError(f"inconsistent AudioFlinger track rows for {kind}")
        active = [row for row in parsed if row[1] == "yes"]
        if len(active) != int(table[2] or 0):
            raise ValueError(f"inconsistent AudioFlinger active tracks for {kind}")
        result.append({"output": kind, "active": [
            {"pid": int(row[2]), "stream_type": int(row[3])} for row in active
        ]})
    return result


def capture(shell):
    before = fields(shell(f"cat {CARD}/status"))
    config = fields(shell(f"cat {CARD}/hw_params"))
    outputs = tracks(shell("dumpsys media.audio_flinger"))
    after = fields(shell(f"cat {CARD}/status"))
    network = re.findall(r"(?m)^Active default network: (.+)$", shell("dumpsys connectivity"))
    status = after.get("state", "closed")
    tid = after.get("owner_pid", "")
    identity = ("state", "owner_pid", "trigger_time")
    stable = status == "RUNNING" and all(before.get(key) and before[key] == after.get(key)
                                        for key in identity)
    program = ""
    if tid.isdecimal():
        owner = fields(shell(f"cat /proc/{tid}/status"))
        tgid = owner.get("Tgid", "")
        if tgid.isdecimal():
            program = shell(f"cat /proc/{tgid}/cmdline").split("\0", 1)[0]
    rate = int(config.get("rate", "0").split()[0])
    channels = int(config.get("channels", "0"))
    encoding = config.get("format")
    return {
        "state": status, "stable": stable, "config": config, "rate_hz": rate,
        "encoding": encoding, "container_kbps": rate * channels * WIDTHS.get(encoding, 0) / 1000,
        "owner_tid": tid, "owner_program": program, "trigger_time": after.get("trigger_time"),
        "hw_ptr": int(after.get("hw_ptr", "-1")), "appl_ptr": int(after.get("appl_ptr", "-1")),
        "audio_flinger": outputs, "no_default_network": network == ["none"],
    }


def evaluate(first, second, rate, encoding, registered):
    checks = {
        "service_registered": registered,
        "offline": all(sample["no_default_network"] for sample in (first, second)),
        "dac_clock_and_container": all(sample["stable"] and sample["rate_hz"] == rate
                                       and sample["encoding"] == encoding
                                       and sample["config"].get("channels") == "2"
                                       for sample in (first, second)),
        "service_owns_dac": all(sample["owner_program"] == DAEMON for sample in (first, second)),
        "same_playback": bool(first["owner_tid"] and first["trigger_time"])
        and first["owner_tid"] == second["owner_tid"]
        and first["trigger_time"] == second["trigger_time"],
        "frames_advance": all(0 <= first[key] < second[key] for key in ("hw_ptr", "appl_ptr")),
        "no_active_audio_flinger_tracks": all(not row["active"] for sample in (first, second)
                                             for row in sample["audio_flinger"]),
    }
    return checks


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("label", help="Manually observed player and track title")
    parser.add_argument("rate", type=int)
    parser.add_argument("encoding", choices=WIDTHS)
    parser.add_argument("--adb", type=Path, default=DEFAULT_ADB)
    args = parser.parse_args()

    def shell(command):
        return subprocess.run([args.adb, "-d", "shell", command], capture_output=True, text=True,
                              timeout=8, check=True).stdout.replace("\r", "").strip()

    if shell("getprop ro.product.device") != "tulip-u6":
        raise RuntimeError("wrong device")
    first = capture(shell)
    sleep(0.5)
    second = capture(shell)
    registered = shell("service check u6.direct_audio") == "Service u6.direct_audio: found"
    checks = evaluate(first, second, args.rate, args.encoding, registered)
    print(json.dumps({"label_manual": args.label, "expected_rate_hz": args.rate,
                      "expected_encoding": args.encoding, "checks": checks, "both_samples": [first, second]},
                     ensure_ascii=False, indent=2))
    raise SystemExit(0 if all(checks.values()) else 1)


if __name__ == "__main__":
    main()
