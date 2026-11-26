"""Validate U6 boot readback files without writing to the player."""
import argparse
import json
from pathlib import Path
import struct
from zlib import crc32

from build_boot import BOOT_LIMIT, STOCK_BOOT_HASH, aligned, digest, split_boot

SECTOR = 512
COPY_SIZE = 16384
COPIES = 4
BOOT_NAME = b"boot"


def boot_location(table):
    if len(table) != COPIES * COPY_SIZE:
        raise ValueError("expected four 16 KiB Sunxi MBR copies")
    positions = []
    for index in range(COPIES):
        mbr = table[index * COPY_SIZE:(index + 1) * COPY_SIZE]
        checksum, version, magic, count, copy_index, partitions, _ = struct.unpack_from("<II8sIIII", mbr)
        if (magic != b"softw411" or version != 0x200 or count != COPIES or copy_index != index
                or not 0 < partitions <= (COPY_SIZE - 32) // 128 or crc32(mbr[4:]) != checksum):
            raise ValueError(f"invalid Sunxi MBR copy {index}")
        found = []
        for slot in range(partitions):
            offset = 32 + slot * 128
            name = mbr[offset + 32:offset + 48].split(b"\0", 1)[0]
            if name == BOOT_NAME:
                high, low, size_high, size_low = struct.unpack_from("<IIII", mbr, offset)
                found.append(((high << 32) | low, (size_high << 32) | size_low))
        if len(found) != 1:
            raise ValueError(f"expected one boot entry in MBR copy {index}")
        positions.extend(found)
    if len(set(positions)) != 1:
        raise ValueError("MBR copies disagree about boot range")
    lba, sectors = positions[0]
    if (lba, sectors) != (0x20000, BOOT_LIMIT // SECTOR):
        raise ValueError(f"boot range differs from the verified U6 firmware: {lba:#x}+{sectors:#x}")
    return {"boot_logical_lba": lba, "boot_sectors": sectors, "partition_bytes": sectors * SECTOR}


def verify_partition(raw):
    if len(raw) != BOOT_LIMIT or raw[:8] != b"ANDROID!":
        raise ValueError("expected a full U6 boot partition beginning with ANDROID!")
    kernel_size, _, ramdisk_size, _, _, _, _, page = struct.unpack_from("<8I", raw, 8)
    if page != 2048:
        raise ValueError("unexpected Android boot page size")
    end = page + aligned(kernel_size, page) + aligned(ramdisk_size, page)
    if end > len(raw):
        raise ValueError("boot image exceeds the raw partition")
    image, tail = raw[:end], raw[end:]
    split_boot(image)
    return {
        "partition_sha256": digest(raw),
        "boot_image_sha256": digest(image),
        "boot_image_bytes": end,
        "matches_stock_v20_image": digest(image) == STOCK_BOOT_HASH,
        "bytes_after_boot_image": len(tail),
        "tail_all_zero": not any(tail),
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("mbr", type=Path)
    parser.add_argument("--boot", type=Path)
    args = parser.parse_args()
    result = boot_location(args.mbr.read_bytes())
    if args.boot is not None:
        result.update(verify_partition(args.boot.read_bytes()))
    print(json.dumps(result, indent=2))


if __name__ == "__main__":
    main()
