"""Extract AudioTrack's 35 JNI entries from the two checked U6 2.0 libraries."""
import argparse
import hashlib
import json
from pathlib import Path

from elftools.elf.elffile import ELFFile

BINDINGS = {
    "lib": (0x9cbe4, 0x9ccfc, 0xe952c, ".rel.dyn", 23),
    "lib64": (0x109050, 0x1094dc, 0x1a9a40, ".rela.dyn", 1027),
}


def packed_relative(section, has_rela):
    if not section.startswith(b"APS2"):
        raise ValueError("expected Android APS2 relocations")
    cursor = 4

    def signed_leb128():
        nonlocal cursor
        value = shift = 0
        while True:
            byte = section[cursor]
            cursor += 1
            value |= (byte & 127) << shift
            shift += 7
            if not byte & 128:
                return value - (1 << shift) if byte & 64 else value
            if shift > 70:
                raise ValueError("invalid signed LEB128")

    remaining = signed_leb128()
    address = signed_leb128()
    addend = 0
    while remaining:
        group_size = signed_leb128()
        flags = signed_leb128()
        if not 0 < group_size <= remaining or not 0 <= flags <= 15:
            raise ValueError("invalid APS2 group")
        delta = signed_leb128() if flags & 2 else None
        group_info = signed_leb128() if flags & 1 else None
        if flags & 8 and not has_rela:
            raise ValueError("addend in REL relocation")
        if flags & 8 and flags & 4:
            addend += signed_leb128()
        elif not flags & 8:
            addend = 0
        for _ in range(group_size):
            address += signed_leb128() if delta is None else delta
            info = signed_leb128() if group_info is None else group_info
            if flags & 8 and not flags & 4:
                addend += signed_leb128()
            yield address, info, addend
        remaining -= group_size


def inspect(path, binding, abi):
    registration, call, table, section_name, relative_type = binding
    raw = path.read_bytes()
    with path.open("rb") as stream:
        elf = ELFFile(stream)
        wide = elf.elfclass == 64
        width = elf.elfclass // 8
        relocation = elf.get_section_by_name(section_name)
        refs = dict((address, (info, addend))
                    for address, info, addend in packed_relative(relocation.data(), wide))

        def offset(address):
            return next(elf.address_offsets(address, 1))

        def ptr(address):
            info, addend = refs[address]
            if info != relative_type:
                raise ValueError(f"unexpected relocation at 0x{address:x}")
            start = offset(address)
            return addend if wide else int.from_bytes(raw[start:start + width], "little")

        def text(address):
            start = offset(address)
            return raw[start:raw.index(0, start)].decode("ascii")

        methods = []
        for index in range(35):
            row = table + index * 3 * width
            name, signature = text(ptr(row)), text(ptr(row + width))
            entry = ptr(row + 2 * width)
            offset(entry & ~1)
            methods.append({"name": name, "signature": signature, "entry_rva": f"0x{entry:x}",
                            "table_slot_rva": f"0x{row + 2 * width:x}"})
        expected = {"native_setup", "native_start", "native_stop", "native_pause", "native_flush",
                    "native_release", "native_write_native_bytes", "native_get_position",
                    "native_get_timestamp", "native_setVolume"}
        if methods[0]["name"] != "native_start" or not expected <= {m["name"] for m in methods}:
            raise ValueError(f"unexpected JNI table: {abi}")
        return {"abi": abi, "sha256": hashlib.sha256(raw).hexdigest(),
                "registration_rva": f"0x{registration:x}", "register_call_rva": f"0x{call:x}",
                "table_rva": f"0x{table:x}", "methods": methods}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("libraries", type=Path)
    args = parser.parse_args()
    results = [inspect(args.libraries / abi / "libandroid_runtime.so", binding, abi)
               for abi, binding in BINDINGS.items()]
    signatures = [{(row["name"], row["signature"]) for row in abi["methods"]} for abi in results]
    if signatures[0] != signatures[1]:
        raise ValueError("AudioTrack JNI signatures differ by ABI")
    print(json.dumps(results, indent=2))


if __name__ == "__main__":
    main()
