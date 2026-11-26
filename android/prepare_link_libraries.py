"""Normalize legacy ELF64 boundary symbols in link-only copies."""
import argparse
from pathlib import Path
from shutil import copyfile

from elftools.elf.elffile import ELFFile

COMMON_LIBRARIES = {"libbinder.so", "libutils.so", "libtinyalsa.so", "libc++.so",
                    "libcutils.so", "liblog.so", "libm.so", "libmixer.so"}
LINK_LIBRARIES = {"lib": COMMON_LIBRARIES | {"libc.so", "libdl.so"},
                  "lib64": COMMON_LIBRARIES}

BOUNDARY_NAMES = {
    "__bss_start__", "_bss_end__", "_edata", "__bss_end__",
    "_end", "__end__", "__bss_start", "_etext",
}


def copy_for_link(source, target):
    if source.parent.name == "lib" or source.name == "libm.so":
        copyfile(source, target)
        return
    data = bytearray(source.read_bytes())
    with source.open("rb") as file:
        elf = ELFFile(file)
        if elf.elfclass != 64 or not elf.little_endian or elf["e_machine"] != "EM_AARCH64":
            raise ValueError(f"unexpected linkage input: {source}")
        table = elf.get_section_by_name(".dynsym")
        first_global = table["sh_info"]
        for index, symbol in enumerate(table.iter_symbols()):
            if index < first_global or symbol["st_info"]["bind"] != "STB_LOCAL":
                continue
            if symbol.name not in BOUNDARY_NAMES:
                raise ValueError(f"unexpected local dynamic symbol {symbol.name!r} in {source}")
            entry = table["sh_offset"] + index * table["sh_entsize"]
            data[entry + 4] |= 0x10  # STB_GLOBAL. Never used on the device.
            data[entry + 5] = (data[entry + 5] & ~3) | 2  # STV_HIDDEN.
    target.write_bytes(data)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("originals", type=Path)
    parser.add_argument("destination", type=Path)
    args = parser.parse_args()
    for abi in ("lib", "lib64"):
        directory = args.destination / abi
        directory.mkdir(parents=True, exist_ok=True)
        for name in sorted(LINK_LIBRARIES[abi]):
            copy_for_link(args.originals / abi / name, directory / name)


if __name__ == "__main__":
    main()
