"""Build an offline U6 2.0 runtime copy with a single AudioTrack registration redirect."""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import subprocess
import tempfile

from elftools.elf.elffile import ELFFile

PROJECT = Path(__file__).resolve().parent
BINDINGS = {
    "lib": ("armv7a-linux-androideabi23", 0x4b92c, bytes.fromhex("aef716ee")),
    "lib64": ("aarch64-linux-android23", 0x7d580, bytes.fromhex("29d0fd97")),
}


def build(stock, destination, toolchain, evidence):
    abi = evidence["abi"]
    target, original, original_call = BINDINGS[abi]
    entry = int(evidence["register_call_rva"], 16)
    source = stock / abi / "libandroid_runtime.so"
    with source.open("rb") as file:
        raw = bytearray(file.read())
        if hashlib.sha256(raw).hexdigest() != evidence["sha256"]:
            raise ValueError(f"{source}: unrecognized original SHA-256")
        elf = ELFFile(file)
        call_offset = next(elf.address_offsets(entry, 4))
        if raw[call_offset:call_offset + 4] != original_call:
            raise ValueError("original registration instruction differs")
        segments = list(elf.iter_segments())
        segment_index, executable = next((i, seg) for i, seg in enumerate(segments)
                                         if seg["p_type"] == "PT_LOAD" and seg["p_flags"] == 5)
        if executable["p_filesz"] != executable["p_memsz"]:
            raise ValueError("cannot extend executable segment with zero-filled memory")
        file_end = executable["p_offset"] + executable["p_filesz"]
        start = (file_end + 3) & ~3
        # Stay in the existing final executable page. Never overlap the next LOAD.
        bias = executable["p_vaddr"] - executable["p_offset"]
        later_loads = [seg for seg in segments if seg["p_type"] == "PT_LOAD" and
                       seg["p_vaddr"] > executable["p_vaddr"]]
        limit = min((file_end + 4095) & ~4095,
                    *(min(seg["p_offset"], (seg["p_vaddr"] & ~4095) - bias)
                      for seg in later_loads))
        occupied = [(sec["sh_offset"], sec["sh_offset"] + sec["sh_size"])
                    for sec in elf.iter_sections()
                    if sec["sh_type"] != "SHT_NOBITS" and sec["sh_size"]]
        stub_address = executable["p_vaddr"] + start - executable["p_offset"]
        dynsym = elf.get_section_by_name(".dynsym")
        plt = elf.get_section_by_name(".rela.plt" if elf.elfclass == 64 else ".rel.plt")
        symbols = {dynsym.get_symbol(rel["r_info_sym"]).name: rel["r_offset"]
                   for rel in plt.iter_relocations()}
        header = elf["e_phoff"] + segment_index * elf["e_phentsize"]
        size_format, size_offset = ("<QQ", 32) if elf.elfclass == 64 else ("<II", 16)

    with tempfile.TemporaryDirectory(prefix="u6-runtime-") as temp:
        work = Path(temp)
        obj = work / "stub.o"
        linked = work / "stub.elf"
        script = work / "layout.ld"
        script.write_text(f"""
u6_stock_register = {original:#x};
u6_dlopen_slot = {symbols['dlopen']:#x};
u6_dlsym_slot = {symbols['dlsym']:#x};
SECTIONS {{
    .u6_call {entry:#x} : {{ *(.text.u6_call) }}
    .u6_stub {stub_address:#x} : {{ *(.text.u6_stub) }}
}}
""")
        subprocess.run([toolchain / f"{target}-clang", "-c",
                        PROJECT / "runtime_audio_registration.S", "-o", obj], check=True)
        subprocess.run([toolchain / "ld.lld", "-T", script, obj, "-o", linked,
                        "--entry", hex(entry)], check=True)
        with linked.open("rb") as file:
            patch = ELFFile(file)
            call = patch.get_section_by_name(".u6_call").data()
            stub = patch.get_section_by_name(".u6_stub").data()

    stub_end = start + len(stub)
    if (len(call) != 4 or not stub or stub_end > limit or any(raw[file_end:stub_end]) or
            any(low < stub_end and high > start for low, high in occupied)):
        raise ValueError("registration stub overlaps original mapped data")
    size = start + len(stub) - executable["p_offset"]
    raw[call_offset:call_offset + len(call)] = call
    raw[start:start + len(stub)] = stub
    struct.pack_into(size_format, raw, header + size_offset, size, size)
    candidate = destination / abi / "startup" / source.name
    candidate.parent.mkdir(parents=True, exist_ok=True)
    candidate.write_bytes(raw)
    return {
        "abi": abi, "original_sha256": evidence["sha256"],
        "candidate_sha256": hashlib.sha256(raw).hexdigest(),
        "registration_call": hex(entry), "original_plt": hex(original),
        "stub_address": hex(stub_address), "stub_size": len(stub),
        "source": str(source), "candidate": str(candidate),
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("stock", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("toolchain", type=Path)
    args = parser.parse_args()
    evidence = json.loads((PROJECT.parent / "evidence/stock_audiotrack_jni.json").read_text())
    manifest = [build(args.stock, args.output, args.toolchain, row) for row in evidence]
    print(json.dumps(manifest, indent=2))


if __name__ == "__main__":
    main()
