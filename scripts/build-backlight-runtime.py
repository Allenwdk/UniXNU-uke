#!/usr/bin/env python3
"""Package only upstream BacklightMode for 24A446, preserving the existing addon and trust modules."""
import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import plistlib
import shutil
import struct
import subprocess
import uuid

PIN = 'fbba7fd2925990dd859b24629ecab5cdbf6f2fac8c1c357ec0f9e412ded75559'
DYLIB = '/usr/lib/UniXNUBacklight.dylib'
ROOT = Path(__file__).resolve().parents[1]


def sha(data):
    return hashlib.sha256(data).hexdigest()


def signature(data):
    """Independently verify every signed code page, special blob and CDHash."""
    at, found = 32, []
    for _ in range(struct.unpack_from('<I', data, 16)[0]):
        command, size = struct.unpack_from('<II', data, at)
        if command == 0x1d:
            off, length = struct.unpack_from('<II', data, at + 8)
            found.append(data[off:off + length])
        at += size
    if len(found) != 1:
        raise ValueError('expected one embedded signature')
    blob = found[0]
    magic, size, count = struct.unpack_from('>III', blob)
    if magic != 0xfade0cc0 or size > len(blob):
        raise ValueError('invalid signature superblob')
    slots = {}
    for i in range(count):
        kind, off = struct.unpack_from('>II', blob, 12 + i * 8)
        length = struct.unpack_from('>I', blob, off + 4)[0]
        slots[kind] = blob[off:off + length]
    directory = slots[0]
    magic, size, version, flags, hashes, identity, special, pages, limit = struct.unpack_from('>9I', directory)
    width, kind, _, page = directory[36:40]
    if magic != 0xfade0c02 or width != 32 or kind != 2 or page != 12:
        raise ValueError('expected SHA256/4KiB CodeDirectory')
    if pages != (limit + 4095) // 4096:
        raise ValueError('code page count mismatch')
    for i in range(pages):
        if bytes.fromhex(sha(data[i * 4096:min(limit, (i + 1) * 4096)])) != directory[hashes + i * 32:hashes + (i + 1) * 32]:
            raise ValueError(f'code page {i} hash mismatch')
    for slot in (2, 5, 7):
        if slot <= special and slot in slots:
            if bytes.fromhex(sha(slots[slot])) != directory[hashes - slot * 32:hashes - (slot - 1) * 32]:
                raise ValueError(f'special slot {slot} hash mismatch')
    name = directory[identity:directory.index(0, identity)].decode()
    return dict(identifier=name, flags=flags, cdhash=sha(directory[:size])[:40], pages=pages,
                entitlements=plistlib.loads(slots[5][8:]) if 5 in slots else {})


def require_adhoc(signatures):
    if any(not value['flags'] & 2 for value in signatures.values()):
        raise ValueError('owner signatures without CMS must declare CS_ADHOC')


def service_trust(base, service_hash, library_hash):
    """Reuse the proven 24A446 category5 role for just the launchd-owned service.

    See build-wifi-trust.py and the exact AMFI proof: category5 checks platform
    validation plus the original parent predicate. The library stays category0.
    """
    spec = importlib.util.spec_from_file_location('uke_wifi_trust', ROOT / 'scripts/build-wifi-trust.py')
    helper = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(helper)
    original = helper.modules(base)
    entries = {bytes.fromhex(service_hash): bytes([2, 0, 5, 0]),
               bytes.fromhex(library_hash): bytes([2, 0, 0, 0])}
    if len(entries) != 2 or any(len(key) != 20 for key in entries):
        raise ValueError('service and library must have distinct valid CDHashes')
    for module in original:
        if any(module[at:at + 20] in entries for at in range(24, len(module), 24)):
            raise ValueError('base already contains target hashes with competing roles')
    identity = uuid.uuid5(uuid.NAMESPACE_URL, 'org.unixnu.uke.backlight.launch-role-24A446:' + service_hash + ':' + library_hash).bytes
    owner = struct.pack('<I16sI', 2, identity, len(entries))
    owner += b''.join(key + entries[key] for key in sorted(entries))
    output = [*original, owner]
    cursor, offsets = 4 + 4 * len(output), []
    for module in output:
        offsets.append(cursor)
        cursor += len(module)
    result = struct.pack(f'<{len(output) + 1}I', len(output), *offsets) + b''.join(output)
    assert helper.modules(result)[:-1] == original
    return result


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--os-root', type=Path, required=True)
    p.add_argument('--output', type=Path, required=True)
    p.add_argument('--addon', type=Path, required=True)
    p.add_argument('--trust', type=Path, required=True)
    p.add_argument('--core', type=Path, default=ROOT.parent / 'UniXNU')
    p.add_argument('--sdk', type=Path, default=Path(os.environ.get('UNIXNU_SDKROOT', Path.home() / '.cache/ios-sdks/iPhoneOS17.0.sdk')))
    p.add_argument('--compiler', default='clang')
    p.add_argument('--linker', type=Path, default=Path.home() / '.local/share/unixnu-startup-lto/toolchain-gpu/bin/ld')
    p.add_argument('--ldid', default=str(Path.home() / '.cache/ldid/ldid'))
    args = p.parse_args()
    out, core = args.output.resolve(), args.core.resolve()
    out.mkdir(parents=True, exist_ok=True)
    (out / 'report.json').unlink(missing_ok=True)
    original = (args.os_root / 'usr/libexec/backboardd').read_bytes()
    if sha(original) != PIN:
        raise ValueError('backboardd is not the pinned original 24A446 input')
    spec = importlib.util.spec_from_file_location('upstream_appsync_macho', core / 'scripts/appsync/macho.py')
    helper = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(helper)
    original_signature = signature(original)
    for directory in ('lib', 'libexec'):
        (out / directory).mkdir(exist_ok=True)
    source = core / 'scripts/appsync/BacklightMode.m'
    library, binary = out / 'lib/UniXNUBacklight.dylib', out / 'libexec/backboardd'
    subprocess.run([args.compiler, '--ld-path=' + str(args.linker), '-target', 'arm64e-apple-ios27.0', '-isysroot', str(args.sdk),
                    '-fobjc-arc', '-fblocks', '-O2', '-Wall', '-Wextra', '-Werror', '-dynamiclib', str(source),
                    '-framework', 'Foundation', '-install_name', DYLIB,
                    '-Wl,-compatibility_version,1', '-Wl,-current_version,1', '-o', str(library)], check=True)
    subprocess.run([args.ldid, '-Hsha256', '-P12', '-Cadhoc', '-S', '-Iorg.unixnu.backlight', str(library)], check=True)
    binary.write_bytes(helper.append_dylib(original, DYLIB, PIN))
    binary.chmod(0o755)
    entitlement = out / 'backboardd-entitlements.plist'
    entitlement.write_bytes(plistlib.dumps(original_signature['entitlements']))
    subprocess.run([args.ldid, '-Hsha256', '-P12', '-Cadhoc', '-S' + str(entitlement),
                    '-I' + original_signature['identifier'], str(binary)], check=True)
    signatures = {str(path.relative_to(out)): signature(path.read_bytes()) for path in (library, binary)}
    require_adhoc(signatures)
    if signatures['libexec/backboardd']['entitlements'] != original_signature['entitlements']:
        raise ValueError('backboardd entitlements changed')
    # All original dependencies/ordinals remain byte-identical. The brightness
    # adapter is not interposed over the original Foundation import namespace.
    subprocess.run(['python3', str(core / 'scripts/addon-add-dir.py'), str(args.addon), str(out / 'gpuaddon.bin'),
                    'backlight/lib', str(out / 'lib'), 'backlight/libexec', str(out / 'libexec')], check=True)
    (out / 'owner-trust.bundle').write_bytes(service_trust(args.trust.read_bytes(),
        signatures['libexec/backboardd']['cdhash'], signatures['lib/UniXNUBacklight.dylib']['cdhash']))
    # License is distributed with the owner runtime as well as in its source tree.
    shutil.copyfile(core / 'scripts/appsync/LICENSE', out / 'lib/UniXNUBacklight.COPYING')
    subprocess.run(['python3', str(core / 'scripts/addon-add-dir.py'), str(out / 'gpuaddon.bin'),
                    str(out / 'gpuaddon.bin'), 'backlight/lib', str(out / 'lib')], check=True)
    report = dict(build='24A446', original_sha256=PIN, source_sha256=sha(source.read_bytes()),
                  scope='backboardd brightness mode only', hardware_verified=False, signatures=signatures,
                  dependency_mode='append-in-zero-header-padding', original_dependencies_preserved=True,
                  service_constraint_category=5, library_constraint_category=0, amfi_decisions_unchanged=True,
                  files={str(path.relative_to(out)): sha(path.read_bytes()) for path in
                         (library, binary, out / 'gpuaddon.bin', out / 'owner-trust.bundle')})
    (out / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(dict(runtime=str(out), files=report['files']), indent=2))


if __name__ == '__main__':
    main()
