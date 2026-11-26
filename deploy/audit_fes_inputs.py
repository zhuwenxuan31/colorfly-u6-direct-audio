"""Audit the factory U6 IMAGEWTY components used for volatile FES boot."""
import argparse
from hashlib import sha256
import json
from pathlib import Path
import struct

EXPECTED = {
    "fes1.fex": (b"FES     ", b"FES_1-0000000000"),
    "u-boot.fex": (b"12345678", b"UBOOT_0000000000"),
    "sunxi.fex": (b"COMMON  ", b"DTB_CONFIG000000"),
    "config.fex": (b"COMMON  ", b"SYS_CONFIG_BIN00"),
}


def audit(path):
    data = {}
    identities = set()
    with path.open("rb") as image:
        header = image.read(0x7800)
        if header[:8] != b"IMAGEWTY":
            raise ValueError("unexpected firmware header")
        for offset in range(1024, len(header), 1024):
            entry = header[offset:offset + 1024]
            name = entry[0x24:0x124].split(b"\0", 1)[0].decode("ascii")
            identity = (entry[8:16], entry[16:32])
            identities.add(identity)
            if name not in EXPECTED:
                continue
            size, start = struct.unpack_from("<Q", entry, 0x124)[0], struct.unpack_from("<Q", entry, 0x134)[0]
            if identity != EXPECTED[name] or name in data or not 0 < size <= 2 * 1024 * 1024:
                raise ValueError(f"unexpected firmware entry {name}")
            image.seek(start)
            payload = image.read(size)
            if len(payload) != size:
                raise ValueError(f"truncated firmware entry {name}")
            data[name] = payload
    if set(data) != EXPECTED.keys():
        raise ValueError("missing required factory components")
    fes, uboot, dtb = (data[name] for name in ("fes1.fex", "u-boot.fex", "sunxi.fex"))
    checksum, length = struct.unpack_from("<II", fes, 0x0c)
    words = list(struct.unpack("<" + "I" * (len(fes) // 4), fes))
    words[3] = 0x5f0a6c39
    result_address, entry_address = struct.unpack_from("<II", fes, 0x1c)
    if (fes[4:12] != b"eGON.BT0" or length != len(fes) or sum(words) & 0xffffffff != checksum
            or result_address != 0x17210 or entry_address != 0x18000):
        raise ValueError("unexpected factory DRAM bootstrap")
    uboot_address = struct.unpack_from("<I", uboot, 0x2c)[0]
    if (uboot[4:12] != b"uboot\0\0\0" or uboot_address != 0x4a000000
            or uboot[0xe0:0xe4] != bytes(4)):
        raise ValueError("unexpected factory U-Boot header")
    dtb_magic, dtb_length = struct.unpack_from(">II", dtb)
    if (dtb_magic != 0xd00dfeed or not 40 <= dtb_length <= len(dtb)
            or len(dtb) - dtb_length > 15 or any(dtb[dtb_length:])):
        raise ValueError("unexpected factory DTB")
    return {
        "components": {
            name: {"size": len(payload), "sha256": sha256(payload).hexdigest()}
            for name, payload in data.items()
        },
        "fes_dram_entry": hex(entry_address),
        "fes_dram_result": hex(result_address),
        "uboot_link_address": hex(uboot_address),
        "uboot_work_mode_offset": "0xe0",
        "standalone_board_config_entry": (b"COMMON  ", b"BOARD_CONFIG_BIN") in identities,
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("firmware", type=Path)
    args = parser.parse_args()
    print(json.dumps(audit(args.firmware), indent=2))


if __name__ == "__main__":
    main()
