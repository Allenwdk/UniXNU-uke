#!/usr/bin/env python3
"""Package the Uke pen picker alongside the unmodified Settings app."""
# SPDX-License-Identifier: BSD-2-Clause-Patent
import argparse
import hashlib
import json
from pathlib import Path
import plistlib
import runpy
import struct
import subprocess
import zipfile

DEVICE = Path(__file__).resolve().parents[1]
IDENTIFIER = 'org.unixnu.uke.pen-pairing'
EXECUTABLE = 'UkePenPairing'


def sha(data):
    return hashlib.sha256(data).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('addon', 'trust', 'output'):
        parser.add_argument('--' + name, type=Path, required=True)
    parser.add_argument('--core', type=Path, default=DEVICE.parent / 'UniXNU')
    parser.add_argument('--driver-source', type=Path, help='NativeFocusPenPro source checkout')
    parser.add_argument('--sdk', type=Path, required=True, help='iPhoneOS 18.4 or newer SDK')
    parser.add_argument('--linker', type=Path,
                        default=Path.home() / '.local/share/unixnu-startup-lto/toolchain-gpu/bin/ld')
    parser.add_argument('--ldid', type=Path, default=Path.home() / '.cache/ldid/ldid')
    args = parser.parse_args()
    framework = args.sdk / 'System/Library/Frameworks/AccessorySetupKit.framework'
    if not (framework / 'Headers/AccessorySetupKit.h').is_file():
        raise ValueError('SDK does not provide AccessorySetupKit headers')
    output = args.output.resolve()
    bundle = output / 'pencil-pairing/apps/UkePenPairing.app'
    bundle.mkdir(parents=True, exist_ok=True)
    executable = bundle / EXECUTABLE
    info = bundle / 'Info.plist'
    info.write_bytes(plistlib.dumps({
        'CFBundleDisplayName': '触控笔配对', 'CFBundleName': EXECUTABLE,
        'CFBundleExecutable': EXECUTABLE, 'CFBundleIdentifier': IDENTIFIER,
        'CFBundlePackageType': 'APPL', 'CFBundleVersion': '12', 'CFBundleShortVersionString': '2.1',
        'CFBundleSupportedPlatforms': ['iPhoneOS'], 'MinimumOSVersion': '27.0', 'UIDeviceFamily': [2],
        'UILaunchScreen': {}, 'NSBluetoothAlwaysUsageDescription': '用于连接小米焦点触控笔 Pro。',
        'UIApplicationSupportsIndirectInputEvents': True,
        'NSAccessorySetupKitSupports': ['Bluetooth'],
        'NSAccessorySetupBluetoothServices': ['FEF5'],
        'NSAccessorySetupBluetoothNames': ['Xiaomi Focus Pen Pro'],
    }))
    source = (args.driver_source or args.core / 'drivers/NativeFocusPenPro').resolve() / 'UkePenPairingApp.m'
    subprocess.run(['clang', '-target', 'arm64e-apple-ios27.0', '-isysroot', str(args.sdk.resolve()),
                    f'--ld-path={args.linker.resolve()}', '-fobjc-arc', '-fblocks', '-Xclang',
                    '-fptrauth-block-descriptor-pointers', '-Wl,-fixup_chains',
                    '-O2', '-Wall', '-Wextra', '-Werror', source, '-framework', 'CoreBluetooth',
                    '-framework', 'UIKit', '-framework', 'AccessorySetupKit',
                    '-framework', 'PencilKit',
                    '-o', str(executable)], check=True)
    subprocess.run([str(args.ldid), '-Hsha256', '-P12', '-Cadhoc', '-S', str(bundle)], check=True)
    # This ldid's bundle mode creates the resource envelope but ignores -C.
    # Sign the main image with explicit flags and the same two bundle hashes.
    resources = bundle / '_CodeSignature/CodeResources'
    subprocess.run([str(args.ldid), '-Hsha256', '-P12', '-Cadhoc', '-S', '-I' + IDENTIFIER,
                    f'-E1:{info}', f'-E3:{resources}', str(executable)], check=True)
    data = executable.read_bytes()
    magic, cpu, subtype, kind, _ = struct.unpack_from('<5I', data)
    if (magic, cpu, subtype & 0xffffff, kind) != (0xfeedfacf, 0x0100000c, 2, 2):
        raise ValueError('expected arm64e iPadOS application')
    at = 32
    versions = []
    for _ in range(struct.unpack_from('<I', data, 16)[0]):
        command, size = struct.unpack_from('<II', data, at)
        if command == 0x32:
            versions.append(struct.unpack_from('<III', data, at + 8))
        at += size
    if len(versions) != 1 or versions[0][:2] != (2, 0x1b0000) or versions[0][2] < 0x120400:
        raise ValueError('expected an iPadOS 27 application built with SDK 18.4 or newer')
    signing = runpy.run_path(str(DEVICE / 'scripts/build-backlight-runtime.py'))
    signature = signing['signature'](data)
    signing['require_adhoc']({EXECUTABLE: signature})
    if signature['identifier'] != IDENTIFIER or signature['entitlements']:
        raise ValueError('expected the owner app identity with no private entitlements')
    at = 32
    for _ in range(struct.unpack_from('<I', data, 16)[0]):
        command, size = struct.unpack_from('<II', data, at)
        if command == 0x1d:
            offset, length = struct.unpack_from('<II', data, at + 8)
            break
        at += size
    else:
        raise ValueError('missing bundle signature')
    cache_signing = runpy.run_path(str(DEVICE / 'scripts/build-icon-service-cache.py'))
    _, used = struct.unpack_from('>II', data, offset)
    if used > length or any(data[offset + used:offset + length]):
        raise ValueError('unexpected signature padding')
    directory = cache_signing['signature_blobs'](data[offset:offset + used])[0]
    hashes, _, special = struct.unpack_from('>3I', directory, 16)
    if special < 3 or any(
            directory[hashes - slot * 32:hashes - (slot - 1) * 32] != hashlib.sha256(path.read_bytes()).digest()
            for slot, path in ((1, info), (3, resources))):
        raise ValueError('Info.plist or CodeResources hash mismatch')
    extend = runpy.run_path(str(DEVICE / 'scripts/build-pencil-runtime.py'))['extend_trust']
    trust = extend(args.trust.read_bytes(), {signature['cdhash']: 0}, label=IDENTIFIER)
    (output / 'owner-trust.bundle').write_bytes(trust)

    addon = runpy.run_path(str(args.core / 'scripts/addon-add-dir.py'))
    base = addon['read'](args.addon)
    entries = {name: (mode, body) for name, mode, body in base}
    jobs = plistlib.loads(entries['xpc/launchd.plist'][1])['LaunchDaemons']
    if (any(job.get('Label') == 'org.unixnu.uke.pencild' for job in jobs.values())
            or any(name.startswith(('pencil-settings/', 'pencil-pairing/')) or name.endswith('/uke-pencild')
                   for name in entries)):
        raise ValueError('require the base addon with stock Settings and without automatic pairing')
    files = [path for path in sorted(bundle.rglob('*')) if path.is_file()]
    added = [(path.relative_to(output).as_posix(), 0o755 if path == executable else 0o644, path.read_bytes())
             for path in files]
    target = output / 'gpuaddon.bin'
    addon['write'](target, base + added)
    if addon['read'](target) != base + added:
        raise ValueError('unexpected picker addon contents')
    with zipfile.ZipFile(output / 'UkePenPairing.ipa', 'w', zipfile.ZIP_DEFLATED) as ipa:
        for path in files:
            ipa.write(path, 'Payload/' + path.relative_to(bundle.parent).as_posix())
        if ipa.testzip() is not None:
            raise ValueError('picker IPA failed archive validation')
    report = dict(app_cdhash=signature['cdhash'], addon_sha256=sha(target.read_bytes()),
                  trust_sha256=sha(trust), source_sha256=sha(source.read_bytes()),
                  sdk_version=hex(versions[0][2]), hardware_tested=False)
    (output / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report, indent=2))


if __name__ == '__main__':
    main()
