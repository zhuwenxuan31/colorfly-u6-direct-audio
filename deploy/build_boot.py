"""Build the U6 2.0 engineering boot image from stock and compiled offline policy."""
import argparse
import gzip
import hashlib
import json
from pathlib import Path
from prepare_overlay import NATIVE_OUTPUT
import struct

STOCK_BOOT_HASH = "18449a425ede09dafb979496978d910a4fb5e56a952147aed8b335f0e23fe3ee"
STOCK_POLICY_HASH = "c51a5d6733acd7283391edaef4982276ca079e70a4f73a7517004ae877a36a30"
BOOT_LIMIT = 32768 * 512  # boot size in the v2.0 sys_partition.fex, in sectors
HERE = Path(__file__).resolve().parent


def digest(data):
    return hashlib.sha256(data).hexdigest()


def aligned(size, unit):
    return (size + unit - 1) // unit * unit


def boot_id(kernel, compressed):
    size = struct.pack("<I", len(kernel))
    rest = struct.pack("<II", len(compressed), 0)
    return hashlib.sha1(kernel + size + compressed + rest).digest()


def split_boot(image):
    ksize, _, rsize, _, second, _, _, page, dt_size, reserved = struct.unpack_from("<10I", image, 8)
    if image[:8] != b"ANDROID!" or page != 2048 or any((second, dt_size, reserved)):
        raise ValueError("unexpected U6 boot header")
    offset = page + aligned(ksize, page)
    if len(image) != offset + aligned(rsize, page):
        raise ValueError("unrecognized boot trailer or truncated image")
    kernel, compressed = image[page:page + ksize], image[offset:offset + rsize]
    if image[576:596] != boot_id(kernel, compressed):
        raise ValueError("unexpected boot image digest in header")
    return page, offset, kernel, compressed


def records(archive):
    offset = 0
    used = set()
    while offset + 110 <= len(archive):
        header = archive[offset:offset + 110]
        if header[:6] != b"070701":
            raise ValueError("unexpected cpio entry magic")
        size, namesize = int(header[54:62], 16), int(header[94:102], 16)
        name_start = offset + 110
        name_end = name_start + namesize
        if not namesize or archive[name_end - 1:name_end] != b"\0":
            raise ValueError("invalid cpio name")
        name = archive[name_start:name_end - 1].decode()
        start = aligned(name_end, 4)
        end = aligned(start + size, 4)
        if end > len(archive) or name in used:
            raise ValueError(f"invalid cpio entry {name}")
        if name == "TRAILER!!!":
            if size or any(archive[end:]):
                raise ValueError("unrecognized data after cpio trailer")
            yield name, archive[offset:end], b"", b""
            return
        used.add(name)
        yield name, archive[offset:end], archive[offset:start], archive[start:start + size]
        offset = end
    raise ValueError("missing cpio trailer")


def change_init(original, overlay=False, native_output=False, daemon_abi=b"lib64"):
    fragment = (HERE / "init.u6_direct_audio.rc").read_bytes()
    actions, declaration = fragment.split(b"\nservice ", 1)
    prefix = b"on post-fs-data\n"
    anchor = b"    restorecon_recursive /data\n"
    if not actions.startswith(prefix) or original.count(anchor) != 1 or b"u6_direct_audio" in original:
        raise ValueError("unexpected U6 init layout")
    if overlay:
        early = b"on early-init\n"
        if original.count(early) != 1:
            raise ValueError("unexpected U6 early-init layout")
        # Label the executable while the root filesystem still permits xattr changes.
        original = original.replace(early, early + b"    restorecon /u6-payload/bin-u6-direct-audio\n")
        actions += (b"    exec u:r:u6_overlay:s0 root root -- /sbin/u6-stage-overlay "
                    b"/u6-payload /dev/u6_audio\n")
        # The stock /dev tmpfs is nosuid, so exec from it cannot transition domains.
        declaration = declaration.replace(b"/system/bin/u6-direct-audio", b"/u6-payload/bin-u6-direct-audio")
        declaration += b"    setenv LD_LIBRARY_PATH " + library_path(daemon_abi) + b"\n"
        original = add_library_path(add_library_path(original, b"media", b"lib"),
                                    b"devsettings", b"lib")
        if native_output:
            original = add_service_env(original, b"media", b"LD_PRELOAD",
                                       f"/dev/u6_audio/{NATIVE_OUTPUT}".encode())
    return (original.replace(anchor, actions[len(prefix):].strip(b"\n") + b"\n" + anchor)
            + b"\nservice " + declaration)


def library_path(abi):
    # Android 6 also uses this environment variable for Java JNI library search.
    return b":".join(root + abi for root in (b"/dev/u6_audio/", b"/vendor/", b"/system/"))


def add_library_path(init, service, abi):
    return add_service_env(init, service, b"LD_LIBRARY_PATH", library_path(abi))


def add_service_env(init, service, key, value):
    lines = init.splitlines(keepends=True)
    prefix = b"service " + service + b" "
    matches = [i for i, line in enumerate(lines) if line.startswith(prefix)]
    if len(matches) != 1:
        raise ValueError(f"unexpected init service definition for {service!r}")
    lines.insert(matches[0] + 1, b"    setenv " + key + b" " + value + b"\n")
    return b"".join(lines)


def add_cpio(name, content, mode, ino):
    name = name.encode() + b"\0"
    fields = (ino, mode, 0, 0, 1, 0, len(content), 0, 0, 0, 0, len(name), 0)
    header = b"070701" + "".join(f"{value:08x}" for value in fields).encode() + name
    return (header + bytes(aligned(len(header), 4) - len(header))
            + content + bytes(aligned(len(content), 4) - len(content)))


def append_contexts(original, fragment):
    for line in fragment.splitlines():
        if line.split()[0] in {row.split()[0] for row in original.splitlines() if row.strip()}:
            raise ValueError("SELinux context already declared")
    return original.rstrip(b"\n") + b"\n" + fragment


def build(stock, policy, overlay=None):
    if digest(stock) != STOCK_BOOT_HASH:
        raise ValueError("unrecognized stock boot image SHA-256")
    page, offset, kernel, compressed = split_boot(stock)
    old_archive = gzip.decompress(compressed)
    old_entries = list(records(old_archive))
    original = {name: content for name, _, _, content in old_entries}
    if digest(original["sepolicy"]) != STOCK_POLICY_HASH or policy == original["sepolicy"]:
        raise ValueError("unrecognized stock policy or no compiled policy changes")
    native_payload = NATIVE_OUTPUT.replace("/", "-")
    native_output = overlay is not None and (overlay / "transfer" / native_payload).is_file()
    daemon_abi = b"lib64"
    if overlay is not None:
        daemon = (overlay / "transfer/bin-u6-direct-audio").read_bytes()
        architectures = {(1, 40): b"lib", (2, 183): b"lib64"}
        if len(daemon) < 20 or daemon[:4] != b"\x7fELF" or daemon[5] != 1:
            raise ValueError("invalid direct audio service ELF header")
        daemon_abi = architectures.get((daemon[4], int.from_bytes(daemon[18:20], "little")))
        if daemon_abi is None:
            raise ValueError("unsupported direct audio service architecture")
    replacements = {
        "init.rc": change_init(original["init.rc"], bool(overlay), native_output, daemon_abi),
        "sepolicy": policy,
        "file_contexts": append_contexts(original["file_contexts"], (HERE / "file_contexts").read_bytes()),
        "service_contexts": append_contexts(original["service_contexts"], (HERE / "service_contexts").read_bytes()),
    }
    additions = {}
    if overlay is not None:
        replacements["init.zygote64_32.rc"] = add_library_path(add_library_path(
            original["init.zygote64_32.rc"], b"zygote", b"lib64"), b"zygote_secondary", b"lib")
        replacements["file_contexts"] = append_contexts(
            replacements["file_contexts"], (HERE / "overlay.file_contexts").read_bytes())
        additions["sbin/u6-stage-overlay"] = ((overlay / "stage-overlay").read_bytes(), 0o100755)
        additions["u6-payload"] = (b"", 0o040755)
        for staged in sorted((overlay / "transfer").iterdir()):
            mode = 0o100755 if staged.name == "bin-u6-direct-audio" else 0o100644
            additions[f"u6-payload/{staged.name}"] = (staged.read_bytes(), mode)
        if set(original) & additions.keys():
            raise ValueError("overlay overlaps existing ramdisk entries")
    archive = bytearray()
    for name, entry, header, _ in old_entries:
        if name in replacements:
            payload = replacements[name]
            archive.extend(header[:54] + f"{len(payload):08x}".encode() + header[62:])
            archive.extend(payload)
            archive.extend(bytes(aligned(len(payload), 4) - len(payload)))
        else:
            if name == "TRAILER!!!":
                for i, (extra, (payload, mode)) in enumerate(additions.items()):
                    archive.extend(add_cpio(extra, payload, mode, 0x7fff0000 + i))
            archive.extend(entry)
    archive.extend(bytes(aligned(len(archive), 512) - len(archive)))
    compressed = gzip.compress(archive, compresslevel=9, mtime=0)
    header = bytearray(stock[:offset])
    struct.pack_into("<I", header, 16, len(compressed))
    header[576:596] = boot_id(kernel, compressed)
    result = header + compressed + bytes(aligned(len(compressed), page) - len(compressed))
    if len(result) > BOOT_LIMIT:
        raise ValueError("candidate exceeds the factory boot partition")
    _, _, verified_kernel, verified_ramdisk = split_boot(result)
    new_entries = list(records(gzip.decompress(verified_ramdisk)))
    expected_names = [row[0] for row in old_entries[:-1]] + list(additions) + ["TRAILER!!!"]
    if verified_kernel != kernel or [row[0] for row in new_entries] != expected_names:
        raise ValueError("unexpected kernel or cpio entries")
    rebuilt = {row[0]: row for row in new_entries}
    for name, (content, mode) in additions.items():
        entry = rebuilt[name]
        if entry[3] != content or int(entry[2][14:22], 16) != mode:
            raise ValueError(f"unexpected ramdisk payload for {name}")
    for name, raw, metadata, _ in old_entries:
        new = rebuilt[name]
        if name in replacements:
            if ((metadata[:54], metadata[62:]) != (new[2][:54], new[2][62:]) or
                    new[3] != replacements[name]):
                raise ValueError(f"unexpected archive metadata or bytes for {name}")
        elif raw != new[1]:
            raise ValueError(f"unexpected change to {name}")
    return result, {
        "original_boot_sha256": digest(stock), "candidate_boot_sha256": digest(result),
        "kernel_sha256": digest(kernel), "candidate_bytes": len(result),
        "boot_partition_bytes": BOOT_LIMIT, "ramdisk_members_changed": list(replacements),
        "ramdisk_members_added": list(additions), "ramdisk_members_verified": len(new_entries) - 1,
        "candidate_policy_sha256": digest(policy),
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("stock_boot", type=Path)
    parser.add_argument("compiled_policy", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--overlay", type=Path)
    args = parser.parse_args()
    image, manifest = build(args.stock_boot.read_bytes(), args.compiled_policy.read_bytes(), args.overlay)
    with args.output.open("xb") as output:
        output.write(image)
    print(json.dumps(manifest, indent=2))


if __name__ == "__main__":
    main()
