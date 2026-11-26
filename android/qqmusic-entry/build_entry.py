#!/usr/bin/env python3
"""Build from Java, preserve the existing entry manifest structure, sign locally."""
import argparse
import hashlib
import json
from pathlib import Path
import secrets
import struct
import subprocess
import zipfile

ROOT = Path(__file__).resolve().parent
ORIGINAL_SHA = '7cfb4088d3e5b2f26fddd3af65123b5f9e6570e43d164dce6b69ea4c80c47157'


def run(args):
    result = subprocess.run([str(a) for a in args], capture_output=True, text=True)
    if result.returncode:
        raise RuntimeError(result.stdout + result.stderr)
    return result.stdout + result.stderr


def manifest(original, package, label, version):
    """Rewrite string pool and selected typed attributes without changing indices."""
    data = original
    assert struct.unpack_from('<HH', data) == (3, 8)
    chunks = []
    offset = 8
    strings = None
    while offset < len(data):
        kind, header, size = struct.unpack_from('<HHI', data, offset)
        chunk = bytearray(data[offset:offset + size])
        assert len(chunk) == size and size >= header
        if kind == 1:
            count, styles, flags, start, style_start = struct.unpack_from('<IIIII', chunk, 8)
            assert not styles and not style_start and header == 28
            assert not (flags & 256), 'Expected original UTF16 manifest'
            strings = []
            for i in range(count):
                pos = start + struct.unpack_from('<I', chunk, header + 4 * i)[0]
                length = struct.unpack_from('<H', chunk, pos)[0]
                pos += 2
                if length & 0x8000:
                    length = ((length & 0x7fff) << 16) | struct.unpack_from('<H', chunk, pos)[0]
                    pos += 2
                strings.append(bytes(chunk[pos:pos + length * 2]).decode('utf-16le'))
            strings = [package if s == 'com.tencent.qqmusic' else
                       version if s == 'U6-entry-1.1' else s for s in strings]
            label_index = len(strings)
            strings.append(label)
            strings.extend(['uses-permission', 'android.permission.DUMP'])
            pool = bytearray()
            offsets = []
            for value in strings:
                raw = value.encode('utf-16le')
                units = len(raw) // 2
                assert units < 0x8000
                offsets.append(len(pool))
                pool += struct.pack('<H', units) + raw + b'\0\0'
            pool += b'\0' * (-len(pool) % 4)
            start = header + 4 * len(strings)
            chunk = bytearray(struct.pack('<HHIIIIII', 1, header, start + len(pool),
                                         len(strings), 0, flags & ~1, start, 0))
            chunk += struct.pack('<' + 'I' * len(offsets), *offsets) + pool
        elif kind == 0x102:
            assert strings is not None
            tag = strings[struct.unpack_from('<I', chunk, 20)[0]]
            attr_start, attr_size, attr_count = struct.unpack_from('<HHH', chunk, 24)
            for i in range(attr_count):
                pos = 16 + attr_start + i * attr_size
                name_index = struct.unpack_from('<I', chunk, pos + 4)[0]
                name = strings[name_index]
                if tag == 'manifest' and name == 'versionCode':
                    struct.pack_into('<I', chunk, pos + 16, 1130)
                if name == 'theme':
                    struct.pack_into('<I', chunk, pos + 8, 0xffffffff)
                    chunk[pos + 15] = 1
                    struct.pack_into('<I', chunk, pos + 16, 16974401)
                if tag == 'application' and name == 'label':
                    struct.pack_into('<I', chunk, pos + 8, label_index)
                    chunk[pos + 15] = 3
                    struct.pack_into('<I', chunk, pos + 16, label_index)
        if kind == 0x102 and tag == 'application':
            # Android namespace/name already carry their original resource-map IDs.
            ns = strings.index('http://schemas.android.com/apk/res/android')
            name_id = strings.index('name')
            tag_id = strings.index('uses-permission')
            value_id = strings.index('android.permission.DUMP')
            start_node = struct.pack('<HHIII', 0x102, 16, 56, 1, 0xffffffff)
            start_node += struct.pack('<IIHHHHHH', 0xffffffff, tag_id, 20, 20, 1, 0, 0, 0)
            start_node += struct.pack('<IIIHBBI', ns, name_id, value_id, 8, 0, 3, value_id)
            end_node = struct.pack('<HHIIIII', 0x103, 16, 24, 1, 0xffffffff, 0xffffffff, tag_id)
            chunks.extend([start_node, end_node])
        chunks.append(bytes(chunk))
        offset += size
    assert offset == len(data)
    body = b''.join(chunks)
    return struct.pack('<HHI', 3, 8, len(body) + 8) + body


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--template', type=Path, required=True)
    parser.add_argument('--java-home', type=Path, required=True)
    parser.add_argument('--android-jar', type=Path, required=True)
    parser.add_argument('--r8-jar', type=Path, required=True)
    parser.add_argument('--probe', action='store_true')
    args = parser.parse_args()
    if hashlib.sha256(args.template.read_bytes()).hexdigest() != ORIGINAL_SHA:
        parser.error('The template must be the verified U6-entry-1.1 bridge APK')
    name = 'probe' if args.probe else 'entry'
    output = ROOT.parents[1] / 'out' / 'qqmusic-entry' / name
    classes = output / 'classes'
    dex = output / 'dex'
    classes.mkdir(parents=True, exist_ok=True)
    dex.mkdir(exist_ok=True)
    source = ROOT / 'EntryActivity.java'
    run([args.java_home / 'bin/javac', '-source', '8', '-target', '8',
         '-bootclasspath', args.android_jar, '-d', classes, source])
    run([args.java_home / 'bin/java', '-cp', args.r8_jar,
         'com.android.tools.r8.D8', '--min-api', '23', '--lib', args.android_jar,
         '--output', dex, *sorted(classes.rglob('*.class'))])
    key_dir = output.parent / 'signing'
    key_dir.mkdir(mode=0o700, exist_ok=True)
    password = key_dir / 'store-password.txt'
    key = key_dir / 'u6-entry.p12'
    if not key.exists():
        password.write_text(secrets.token_urlsafe(32))
        password.chmod(0o600)
        run([args.java_home / 'bin/keytool', '-genkeypair', '-alias', 'u6entry',
             '-keyalg', 'RSA', '-keysize', '2048', '-sigalg', 'SHA256withRSA',
             '-validity', '3650', '-dname', 'CN=U6 Local Audio Entry', '-storetype', 'PKCS12',
             '-keystore', key, '-storepass:file', password, '-keypass:file', password])
        key.chmod(0o600)
    package = 'local.u6.directprobe' if args.probe else 'com.tencent.qqmusic'
    label = 'U6 模式验证' if args.probe else 'Apple Music 直出'
    apk = output / ('U6-Mode-Probe.apk' if args.probe else 'U6-AppleMusic-Direct-Entry.apk')
    with zipfile.ZipFile(args.template) as old, zipfile.ZipFile(apk, 'w', zipfile.ZIP_DEFLATED) as new:
        new.writestr('AndroidManifest.xml', manifest(old.read('AndroidManifest.xml'),
                     package, label, 'U6-probe-1.3' if args.probe else 'U6-entry-1.3'))
        new.writestr('resources.arsc', old.read('resources.arsc'))
        new.writestr('classes.dex', (dex / 'classes.dex').read_bytes())
    run([args.java_home / 'bin/jarsigner', '-keystore', key, '-storepass:file', password,
         '-sigalg', 'SHA256withRSA', '-digestalg', 'SHA-256', apk, 'u6entry'])
    verified = run([args.java_home / 'bin/jarsigner', '-verify', apk])
    assert 'jar verified.' in verified
    report = {'source_sha256': hashlib.sha256(source.read_bytes()).hexdigest(),
              'template_sha256': ORIGINAL_SHA, 'package': package,
              'apk': str(apk), 'bytes': apk.stat().st_size,
              'sha256': hashlib.sha256(apk.read_bytes()).hexdigest(),
              'dex_magic': (dex / 'classes.dex').read_bytes()[:8].decode(),
              'signature_verification': verified}
    (output / 'build-report.json').write_text(json.dumps(report, ensure_ascii=False, indent=2))
    print(json.dumps(report, ensure_ascii=False, indent=2))


if __name__ == '__main__':
    main()
