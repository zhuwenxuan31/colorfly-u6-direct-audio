"""Create an exact-address, flat transfer bundle and pin contents in the boot helper."""
import argparse
from hashlib import sha256
from pathlib import Path

FILES = [
    f"{abi}/{name}"
    for abi in ("lib", "lib64")
    for name in ("libandroid_runtime.so", "libtinyalsa.so", "libu6_alsa.so",
                 "libu6_direct_client.so")
]
FILES.append("bin/u6-direct-audio")
NATIVE_OUTPUT = "lib/libu6_native_output.so"


def ranges(before, after):
    edits = []
    for offset, (old, new) in enumerate(zip(before, after, strict=True)):
        if old == new:
            continue
        if edits and offset == edits[-1][0] + len(edits[-1][1]):
            edits[-1][1].append(new)
        else:
            edits.append((offset, bytearray([new])))
    return edits


def hex_bytes(data):
    return ", ".join(f"0x{value:02x}" for value in data)


def prepare(candidate, transfer, header, stock=None):
    expected = {}
    for row in (candidate / "SHA256SUMS").read_text().splitlines():
        digest, name = row.split(maxsplit=1)
        expected[name.strip()] = digest
    payloads = []
    names = FILES + ([NATIVE_OUTPUT] if f"system/{NATIVE_OUTPUT}" in expected else [])
    for name in names:
        partition_path = f"system/{name}"
        data = (candidate / partition_path).read_bytes()
        digest = sha256(data).digest()
        if digest.hex() != expected.get(partition_path):
            raise ValueError(f"manifest mismatch for {name}")
        payloads.append((name.replace("/", "-"), name, data, digest))
    transfer.mkdir()
    rows, declarations = [], []
    for index, (staging_name, name, data, digest) in enumerate(payloads):
        mode = "0755" if name.startswith("bin/") else "0644"
        abi, basename = name.split("/")
        original = {"libandroid_runtime.so": "libandroid_runtime.so",
                    "libu6_alsa.so": "libtinyalsa.so"}.get(basename)
        common = f'"{staging_name}", "{name}", {len(data)}, {mode}, {{{hex_bytes(digest)}}}'
        if stock is None or original is None:
            (transfer / staging_name).write_bytes(data)
            rows.append(f"    {{{common}, NULL, {{0}}, NULL, 0}},")
            continue
        before = (stock / abi / original).read_bytes()
        edits = ranges(before, data)
        if not edits or sum(len(delta) for _, delta in edits) > 1024:
            raise ValueError(f"unexpected patch size for {name}")
        array = f"patch_{index}"
        declarations.append(f"static const struct overlay_patch {array}[] = {{")
        for offset, delta in edits:
            escaped = "".join(f"\\x{byte:02x}" for byte in delta)
            declarations.append(
                f'    {{{offset}, {len(delta)}, (const unsigned char *)"{escaped}"}},'
            )
        declarations.append("};")
        rows.append(f'    {{{common}, "/system/{abi}/{original}", '
                    f'{{{hex_bytes(sha256(before).digest())}}}, {array}, {len(edits)}}},')
    header.write_text(
        "\n".join(declarations) + "\n"
        + 'static const char *const dirs[] = {"lib", "lib64", "bin"};\n'
        + "static const struct overlay_file files[] = {\n" + "\n".join(rows) + "\n};\n"
    )


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("candidate", type=Path)
    parser.add_argument("transfer", type=Path)
    parser.add_argument("header", type=Path)
    parser.add_argument("--stock", type=Path)
    args = parser.parse_args()
    prepare(args.candidate, args.transfer, args.header, args.stock)


if __name__ == "__main__":
    main()
