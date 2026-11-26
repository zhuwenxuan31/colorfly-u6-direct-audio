"""Copy the SHA-verified original ALSA library with a separate SONAME for the gate."""
from io import BytesIO
from pathlib import Path
import sys

from elftools.elf.elffile import ELFFile

stock, renamed = map(Path, sys.argv[1:])
original = stock.read_bytes()
image = bytearray(original)
elf = ELFFile(BytesIO(original))
tags = tuple(elf.get_section_by_name(".dynamic").iter_tags("DT_SONAME"))
if len(tags) != 1 or tags[0].soname != "libtinyalsa.so":
    raise ValueError("unexpected original ALSA SONAME")
strings = elf.get_section_by_name(".dynstr")
start = tags[0]["d_val"]
offset = strings["sh_offset"] + start
old = b"libtinyalsa.so\0"
new = b"libu6_alsa.so\0"
if original[offset:offset + len(old)] != old:
    raise ValueError("original ALSA SONAME does not match its original bytes")
for symbol in elf.get_section_by_name(".dynsym").iter_symbols():
    if start <= symbol["st_name"] < start + len(old):
        raise ValueError("SONAME overlaps an exported symbol name")
image[offset:offset + len(old)] = new.ljust(len(old), b"\0")
renamed.write_bytes(image)
