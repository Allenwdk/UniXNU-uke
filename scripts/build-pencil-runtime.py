#!/usr/bin/env python3
"""Build the P81c radio service for an existing system bond."""
# SPDX-License-Identifier: BSD-2-Clause-Patent
import argparse
import hashlib
import json
from pathlib import Path
import plistlib
import runpy
import struct
import subprocess
import uuid

DEVICE = Path(__file__).resolve().parents[1]
LABEL = 'org.unixnu.uke.pencild'
JOB = f'/System/Library/LaunchDaemons/{LABEL}.plist'
BINARY = 'bluetooth-bin/uke-pencild'
BLUETOOTHD = 'bluetooth-bin/bluetoothd'


def sha(data):
    return hashlib.sha256(data).hexdigest()


def adapter_address(adt, parse_adt):
    # BluetoothProbe::address programs this exact address from the boot ADT.
    seed = parse_adt(adt)['/device-tree/chosen']['unique-chip-id']
    if len(seed) < 8 or not any(seed):
        raise ValueError('the payload ADT has no valid platform identity')
    value = 0xcbf29ce484222325
    for byte in b'unixnu-bluetooth' + seed:
        value = ((value ^ byte) * 0x100000001b3) & 0xffffffffffffffff
    address = bytearray(value.to_bytes(8, 'little')[:6])
    address[0] = (address[0] & 0xfe) | 2
    return bytes(address)


def extend_trust(data, identities, label=LABEL):
    parse = runpy.run_path(str(DEVICE / 'scripts/build-wifi-trust.py'))['modules']
    base = parse(data)
    entries = {bytes.fromhex(cdhash): bytes([2, 0, role, 0]) for cdhash, role in identities.items()}
    if (not entries or any(len(cdhash) != 20 for cdhash in entries)
            or any(module[offset:offset + 20] in entries
                   for module in base for offset in range(24, len(module), 24))):
        raise ValueError('require new and distinct Pencil trust identities')
    ident = uuid.uuid5(uuid.NAMESPACE_URL, label + ':' + ':'.join(identities))
    module = struct.pack('<I16sI', 2, ident.bytes, len(entries))
    module += b''.join(key + entries[key] for key in sorted(entries))
    offsets = [4 * (len(base) + 2)]
    for previous in base:
        offsets.append(offsets[-1] + len(previous))
    result = struct.pack(f'<{len(offsets) + 1}I', len(offsets), *offsets) + b''.join(base) + module
    if parse(result) != [*base, module]:
        raise ValueError('owner trust module assembly mismatch')
    return result


def build_p81c_interval(args, out, base, trust, signer):
    script = ('patch-p81c-dock-address.py' if args.p81c_dock_public_address
              else 'patch-p81c-bluetooth-profile-floor.py')
    policy = runpy.run_path(str(DEVICE / 'scripts' / script))
    mode, original = base[BLUETOOTHD]
    identity = signer['signature'](original)
    modules = runpy.run_path(str(DEVICE / 'scripts/build-wifi-trust.py'))['modules'](trust)
    old_hash = bytes.fromhex(identity['cdhash'])
    roles = [module[offset + 22] for module in modules for offset in range(24, len(module), 24)
             if module[offset:offset + 20] == old_hash]
    if roles != [5]:
        raise ValueError('existing Bluetooth launch role differs')
    daemon = out / BLUETOOTHD
    daemon.write_bytes(policy['patch'](original, args.compiler))
    daemon.chmod(mode)
    entitlements = out / 'p81c-bluetoothd.entitlements.plist'
    entitlements.write_bytes(plistlib.dumps(identity['entitlements']))
    subprocess.run([str(args.ldid), '-Hsha256', '-P12', '-Cadhoc', '-S' + str(entitlements),
                    '-I' + identity['identifier'], str(daemon)], check=True)
    signed = daemon.read_bytes()
    updated = signer['signature'](signed)
    signer['require_adhoc']({BLUETOOTHD: updated})
    if any(identity[key] != updated[key] for key in ('identifier', 'flags', 'entitlements')):
        raise ValueError('P81c interval policy changed Bluetooth signing identity')
    base[BLUETOOTHD] = (mode, signed)
    return updated['cdhash']


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for flag in ('addon', 'trust', 'stock-cache-signature', 'output', 'adt', 'sdk'):
        parser.add_argument(f'--{flag}', type=Path, required=True)
    parser.add_argument('--core', type=Path, default=DEVICE.parent / 'UniXNU')
    parser.add_argument('--driver-source', type=Path, help='NativeFocusPenPro source checkout')
    parser.add_argument('--compiler', default='clang')
    parser.add_argument('--stock-dock-bridge', action='store_true',
                        help='publish verified dock events via the stock paired accessory transport')
    parser.add_argument('--p81c-active-interval', action='store_true',
                        help='enable the verified 24A446 P81c active-link timing policy')
    parser.add_argument('--p81c-dock-public-address', action='store_true',
                        help='enable a bonded P81c address fallback for the stock Pencil service')
    parser.add_argument('--linker', type=Path,
                        default=Path.home() / '.local/share/unixnu-startup-lto/toolchain-gpu/bin/ld')
    parser.add_argument('--ldid', type=Path, default=Path.home() / '.cache/ldid/ldid')
    args = parser.parse_args()
    if args.p81c_dock_public_address and not (args.stock_dock_bridge and args.p81c_active_interval):
        parser.error('the P81c address fallback requires the stock dock bridge and active interval policy')
    out = args.output.resolve()
    binary = out / BINARY
    binary.parent.mkdir(parents=True, exist_ok=True)
    owned_entitlements = {
        'com.apple.private.security.no-container': True,
        'com.apple.bluetooth.internal': True,
        'com.apple.bluetooth.pairing': True,
        'com.apple.bluetooth.system': True,
        'com.apple.developer.bluetooth-central-background': True,
        'com.apple.private.tcc.allow': ['kTCCServiceBluetoothAlways', 'kTCCServiceBluetoothPeripheral'],
    }
    entitlements = out / 'pencild.entitlements.plist'
    entitlements.write_bytes(plistlib.dumps(owned_entitlements))
    parse_adt = runpy.run_path(str(DEVICE / 'scripts/check-sheng-root-release.py'))['parse_adt']
    info = out / 'pencild.info.plist'
    info.write_bytes(plistlib.dumps({
        'CFBundleIdentifier': LABEL, 'CFBundleName': 'Uke Pencil',
        'NSBluetoothAlwaysUsageDescription': 'Connects the selected Xiaomi Focus Pen Pro.',
        'UIBackgroundModes': ['bluetooth-central'],
        'UkeP81cHostAddress': adapter_address(args.adt.read_bytes(), parse_adt),
    }))
    driver = (args.driver_source or args.core / 'drivers/NativeFocusPenPro').resolve()
    sources = [driver / name for name in
               ('UkePencilPair.m', 'UkePenRadio.m', 'UkePenRadioProtocol.c')]
    feature_flags = []
    if args.stock_dock_bridge:
        sources.append(driver / 'UkePenDock.m')
        feature_flags.append('-DUKE_PEN_STOCK_DOCK_BRIDGE=1')
    if args.p81c_active_interval:
        feature_flags.append('-DUKE_PEN_ACTIVE_INTERVAL=1')
    command = [args.compiler, '-target', 'arm64e-apple-ios27.0', '-isysroot', str(args.sdk.resolve()),
               f'--ld-path={args.linker.resolve()}', '-fobjc-arc', '-fblocks',
               '-Xclang', '-fptrauth-block-descriptor-pointers', '-Wl,-fixup_chains',
               '-O2', '-Wall', '-Wextra', '-Werror', *feature_flags,
               '-Wl,-sectcreate,__TEXT,__info_plist,' + str(info),
               *sources, '-framework', 'CoreBluetooth', '-framework', 'Foundation', '-framework', 'IOKit',
               '-o', str(binary)]
    subprocess.run(command, check=True)
    subprocess.run([str(args.ldid), '-Hsha256', '-P12', '-Cadhoc', '-S' + str(entitlements),
                    '-I' + LABEL, str(binary)], check=True)
    signer = runpy.run_path(str(DEVICE / 'scripts/build-backlight-runtime.py'))
    executable = binary.read_bytes()
    signature = signer['signature'](executable)
    signer['require_adhoc']({BINARY: signature})
    if signature['identifier'] != LABEL or signature['entitlements'] != owned_entitlements:
        raise ValueError('Pencil service identity or entitlements mismatch')
    magic, cpu, subtype, kind, count = struct.unpack_from('<5I', executable)
    if (magic, cpu, subtype & 0xffffff, kind) != (0xfeedfacf, 0x0100000c, 2, 2):
        raise ValueError('not an arm64e executable')
    builds = []
    offset = 32
    for _ in range(count):
        opcode, size = struct.unpack_from('<2I', executable, offset)
        if opcode == 0x32:
            builds.append(struct.unpack_from('<2I', executable, offset + 8))
        offset += size
    if builds != [(2, 0x001b0000)]:
        raise ValueError('require a native iPadOS 27 deployment target')

    addon = runpy.run_path(str(args.core / 'scripts/addon-add-dir.py'))
    base = {name: (mode, data) for name, mode, data in addon['read'](args.addon)}
    if BINARY in base or BLUETOOTHD not in base:
        raise ValueError('require an existing Bluetooth addon without a Pencil service')
    base_trust = args.trust.read_bytes()
    bluetoothd_hash = build_p81c_interval(args, out, base, base_trust, signer) if args.p81c_active_interval else None
    cache_path = 'xpc/launchd.plist'
    cache_bits = base[cache_path][1]
    cache_signature = runpy.run_path(str(DEVICE / 'scripts/build-icon-service-cache.py'))
    cache_signature['verify_signature'](cache_bits, base[cache_path + '.sig'][1])
    config = plistlib.loads(cache_bits)
    if config['VersionNumber'] != 7 or JOB in config['LaunchDaemons']:
        raise ValueError('unsupported launchd cache or existing Pencil job')
    job = {'Label': LABEL, 'ProgramArguments': ['/usr/sbin/uke-pencild'],
           'RunAtLoad': True, 'KeepAlive': True, 'ThrottleInterval': 10, 'UserName': 'root',
           'ProcessType': 'Interactive', 'Umask': 0o77}
    config['LaunchDaemons'][JOB] = job
    updated = plistlib.dumps(config, fmt=plistlib.FMT_BINARY, sort_keys=False)
    roundtrip = plistlib.loads(updated)
    if roundtrip['LaunchDaemons'].pop(JOB) != job or roundtrip != plistlib.loads(cache_bits):
        raise ValueError('Pencil launchd change contains an unrelated edit')
    detached = cache_signature['sign_data'](updated, args.stock_cache_signature.read_bytes(),
                                             f'{LABEL}.launchd-cache')
    cache_hash = cache_signature['verify_signature'](updated, detached)
    service_hash = signature['cdhash']
    if service_hash == cache_hash:
        raise ValueError('service and cache must have distinct trust identities')
    identities = {service_hash: 5, cache_hash: 0}
    if bluetoothd_hash:
        identities[bluetoothd_hash] = 5
    trust = extend_trust(base_trust, identities)
    (out / 'owner-trust.bundle').write_bytes(trust)
    merged = dict(base)
    merged.update({BINARY: (0o755, executable),
                   cache_path: (base[cache_path][0], updated),
                   cache_path + '.sig': (base[cache_path + '.sig'][0], detached)})
    target = out / 'gpuaddon.bin'
    addon['write'](target, [(name, mode, data) for name, (mode, data) in merged.items()])
    if {name: (mode, data) for name, mode, data in addon['read'](target)} != merged:
        raise ValueError('Pencil addon roundtrip failed')
    report = dict(service_cdhash=service_hash, cache_cdhash=cache_hash, job=job,
                  addon_sha256=sha(target.read_bytes()), trust_sha256=sha(trust),
                  source_sha256={source.name: sha(source.read_bytes()) for source in sources},
                  apple_libxpc_verified=False, hardware_tested=False, stock_dock_bridge=args.stock_dock_bridge)
    if bluetoothd_hash:
        report['p81c_bluetoothd_cdhash'] = bluetoothd_hash
        report['p81c_dock_public_address'] = args.p81c_dock_public_address
    (out / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps({k: report[k] for k in ('service_cdhash', 'cache_cdhash', 'addon_sha256')}, indent=2))


if __name__ == '__main__':
    main()
