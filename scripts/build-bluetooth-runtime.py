#!/usr/bin/env python3
"""Extend a verified Uke addon with the upstream iPadOS 27 Bluetooth runtime."""
# SPDX-License-Identifier: BSD-2-Clause-Patent
import argparse
import ctypes
import hashlib
import importlib.util
import json
import platform
import plistlib
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import uuid

ROOT = Path(__file__).resolve().parents[1]


def load(path):
    spec = importlib.util.spec_from_file_location(path.stem, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def sha(data):
    return hashlib.sha256(data).hexdigest()


def merge_cache(data, baseline_signature, stock_cache, job, output, apple_cache=None):
    baseline = plistlib.loads(data)
    updated = plistlib.loads(data)
    jobs = updated['LaunchDaemons']
    required = {
        'com.apple.BlueTool': '/usr/sbin/BlueTool',
        'com.apple.bluetoothd': '/usr/sbin/bluetoothd',
        'org.unixnu.wifi.supplicant': '/usr/libexec/unixnu-wpa_supplicant',
    }
    keys = {}
    for label, executable in required.items():
        matches = [key for key, item in jobs.items() if item.get('Label') == label]
        if len(matches) != 1 or jobs[matches[0]]['ProgramArguments'][0] != executable:
            raise ValueError(f'unexpected or missing launchd job {label}')
        keys[label] = matches[0]
    key = keys['com.apple.BlueTool']
    stock_jobs = plistlib.loads(stock_cache.read_bytes())['LaunchDaemons']
    if any(jobs[keys[label]] != stock_jobs[keys[label]]
           for label in ('com.apple.BlueTool', 'com.apple.bluetoothd')):
        raise ValueError('expected both stock Apple Bluetooth jobs in the base cache')
    fields = ('UserName', 'EnableTransactions', 'EnablePressuredExit', 'ThrottleInterval')
    jobs[key].update({name: job[name] for name in fields})
    cache = load(ROOT / 'scripts/build-icon-service-cache.py')
    cache.verify_signature(data, baseline_signature)
    path = output / 'launchd.plist'
    detached = path.with_suffix('.plist.sig')
    darwin = platform.system() == 'Darwin'
    if apple_cache:
        receipt = json.loads((apple_cache / 'cache-report.json').read_bytes())
        raw = (apple_cache / 'launchd.plist').read_bytes()
        signature = (apple_cache / 'launchd.plist.sig').read_bytes()
        if (receipt['base_sha256'] != sha(data) or receipt['output_sha256'] != sha(raw)
                or receipt['signature_sha256'] != sha(signature)
                or receipt['cdhash'] != cache.verify_signature(raw, signature)
                or receipt['apple_libxpc_and_codesign_verified'] is not True):
            raise ValueError('Apple launchd cache receipt mismatch')
        path.write_bytes(raw)
        detached.write_bytes(signature)
    elif darwin:
        path.write_bytes(plistlib.dumps(updated, fmt=plistlib.FMT_XML, sort_keys=False))
        subprocess.run(['/usr/bin/plutil', '-convert', 'binary1', path], check=True)
        raw = path.read_bytes()
        xpc = ctypes.CDLL('/usr/lib/system/libxpc.dylib')
        xpc.xpc_create_from_plist.argtypes = [ctypes.c_void_p, ctypes.c_size_t]
        xpc.xpc_create_from_plist.restype = ctypes.c_void_p
        xpc.xpc_release.argtypes = [ctypes.c_void_p]
        parsed = xpc.xpc_create_from_plist(ctypes.create_string_buffer(raw), len(raw))
        if not parsed:
            raise ValueError('Apple libxpc rejected the Bluetooth launchd cache')
        xpc.xpc_release(parsed)
        subprocess.run(['/usr/bin/codesign', '--force', '--sign', '-', '--pagesize', '4096',
                        '--digest-algorithm', 'sha256', '--identifier', 'org.unixnu.bluetooth.launchd-cache',
                        '--detached', detached, path], check=True)
        subprocess.run(['/usr/bin/codesign', '--verify', '--strict', '--detached', detached, path], check=True)
    else:
        path.write_bytes(plistlib.dumps(updated, fmt=plistlib.FMT_BINARY, sort_keys=False))
        detached.write_bytes(cache.sign_data(path.read_bytes(), stock_cache.with_suffix('.plist.sig').read_bytes(),
                                                  'org.unixnu.bluetooth.launchd-cache'))
    observed = plistlib.loads(path.read_bytes())
    if observed != updated:
        raise ValueError('serialized cache mismatch')
    observed['LaunchDaemons'][key] = baseline['LaunchDaemons'][key]
    if observed != baseline:
        raise ValueError('an unrelated launchd field changed')
    cdhash = cache.verify_signature(path.read_bytes(), detached.read_bytes())
    verified = bool(apple_cache or darwin)
    receipt = dict(base_sha256=sha(data), output_sha256=sha(path.read_bytes()),
                   signature_sha256=sha(detached.read_bytes()), cdhash=cdhash,
                   apple_libxpc_and_codesign_verified=verified)
    return cdhash, receipt


def append_trust(base, entries):
    parse = load(ROOT / 'scripts/build-wifi-trust.py').modules
    modules = parse(base)
    hashes = {bytes.fromhex(key): role for key, role in entries.items()}
    if len(hashes) != 4 or any(module[at:at + 20] in hashes
                              for module in modules for at in range(24, len(module), 24)):
        raise ValueError('require four new, distinct Bluetooth trust identities')
    ident = uuid.uuid5(uuid.NAMESPACE_URL, 'org.unixnu.uke.bluetooth:' + ''.join(sorted(entries)))
    owner = struct.pack('<I16sI', 2, ident.bytes, len(hashes))
    owner += b''.join(key + bytes([2, 0, role, 0]) for key, role in sorted(hashes.items()))
    offsets = [4 + 4 * (len(modules) + 1)]
    for module in modules:
        offsets.append(offsets[-1] + len(module))
    result = struct.pack(f'<{len(offsets) + 1}I', len(offsets), *offsets) + b''.join(modules) + owner
    if parse(result)[:-1] != modules:
        raise ValueError('base trust modules changed')
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('core', 'component', 'os-root', 'helpers', 'addon', 'trust', 'output'):
        parser.add_argument('--' + name, type=Path, required=True)
    parser.add_argument('--apple-cache', type=Path)
    parser.add_argument('--firmware', type=Path, default=ROOT / '.cache/firmware/uke-bluetooth')
    parser.add_argument('--ldid', type=Path, default=Path.home() / '.cache/ldid/ldid')
    args = parser.parse_args()
    out = args.output.resolve()
    for subdir in ('bluetooth-bin', 'bluetooth-lib', 'firmware/qca', 'xpc'):
        (out / subdir).mkdir(parents=True, exist_ok=True)
    sys.path.insert(0, str(args.component.resolve() / 'tools'))
    from native_ios27 import NativeImage, ORIGINAL_SHA256, ORIGINAL_UUID, NATIVE_DYLIB, _inspect

    signer = load(ROOT / 'scripts/build-backlight-runtime.py')
    original = args.os_root / 'usr/sbin/bluetoothd'
    native = NativeImage(original)
    signed = signer.signature(original.read_bytes())
    patched = native.vendor_routing().volatile_root_keys().h4_shutdown().capability_adapter()
    daemon = out / 'bluetooth-bin/bluetoothd'
    daemon.write_bytes(patched.data)
    daemon.chmod(0o755)
    entitlements = out / 'bluetoothd.entitlements.plist'
    entitlements.write_bytes(plistlib.dumps(signed['entitlements']))
    if platform.system() == 'Darwin':
        subprocess.run(['/usr/bin/codesign', '--force', '--sign', '-', '--pagesize', '4096',
                        '--identifier', signed['identifier'], '--entitlements', entitlements, daemon], check=True)
        subprocess.run(['/usr/bin/codesign', '--verify', '--strict', daemon], check=True)
    else:
        subprocess.run([str(args.ldid), '-Hsha256', '-P12', '-Cadhoc', '-S' + str(entitlements),
                        '-I' + signed['identifier'], str(daemon)], check=True)
    daemon_signature = signer.signature(daemon.read_bytes())
    if any(daemon_signature[k] != signed[k] for k in ('entitlements', 'identifier', 'flags')):
        raise ValueError('native daemon signing identity changed')
    final = _inspect.Image(daemon)
    if final.uuid != ORIGINAL_UUID or final.subtype != 0x80000002 or final.data.count(NATIVE_DYLIB + b'\0') != 1:
        raise ValueError('signed daemon UUID, arm64e ABI or load command changed')

    signatures = {'bluetooth-bin/bluetoothd': daemon_signature}
    for file, report, destination, identifier, digests in (
        ('bluetool/BlueTool', 'bluetool/report.json', 'bluetooth-bin/BlueTool',
         'org.unixnu.bluetooth.BlueTool', 'BlueTool'),
        ('type0/QcaBTType0.dylib', 'type0/type0-report.json', 'bluetooth-lib/QcaBTType0.dylib',
         'org.unixnu.bluetooth.Type0', 'QcaBTType0.dylib'),
    ):
        source = args.helpers / file
        data = source.read_bytes()
        receipt = json.loads((args.helpers / report).read_bytes())
        meta = signer.signature(data)
        if (meta['identifier'] != identifier or meta['cdhash'] != receipt['cdhash']
                or sha(data) != receipt['files'][digests] or receipt['target'] != 'arm64e-apple-ios27.0'):
            raise ValueError(f'Apple helper provenance mismatch for {source}')
        shutil.copy2(source, out / destination)
        (out / destination).chmod(0o755)
        signatures[destination] = meta
    signer.require_adhoc(signatures)
    summary = {key: {**{k: value[k] for k in ('identifier', 'cdhash', 'pages')},
                     'sha256': sha((out / key).read_bytes())} for key, value in signatures.items()}
    (out / 'bluetooth-bin/QcaBluetooth.runtime').write_text(
        json.dumps(dict(native_input_sha256=ORIGINAL_SHA256, files=summary), indent=2) + '\n')

    manifest = json.loads((ROOT / 'firmware/bluetooth/manifest.json').read_text())
    for file, digest in manifest['files'].items():
        data = (args.firmware / file).read_bytes()
        if sha(data) != digest:
            raise ValueError(f'original WCN6750 firmware mismatch for {file}')
        (out / 'firmware/qca' / file).write_bytes(data)

    addon = load(args.core / 'scripts/addon-add-dir.py')
    base = {name: (mode, data) for name, mode, data in addon.read(args.addon)}
    if 'wifi-bin/unixnu-wpa_supplicant' not in base:
        raise ValueError('Bluetooth runtime requires a matched Uke Wi-Fi addon')
    cache_hash, cache_receipt = merge_cache(
        base['xpc/launchd.plist'][1], base['xpc/launchd.plist.sig'][1],
        args.os_root / 'System/Library/xpc/launchd.plist',
        plistlib.loads((args.helpers / 'bluetool/com.apple.BlueTool.plist').read_bytes()), out / 'xpc', args.apple_cache)
    prefixes = ['bluetooth-bin', 'bluetooth-lib', 'firmware/qca', 'xpc']
    if any(path.startswith('audio/ShengAudio.driver/') for path in base):
        name = 'BTAudioHALPlugin.driver'
        prefix = 'audio/' + name
        source = args.os_root / 'System/Library/Audio/Plug-Ins/HAL' / name
        if not (source / name).is_file():
            raise ValueError('the existing HAL overlay needs the complete original Bluetooth plugin')
        shutil.copytree(source, out / prefix, dirs_exist_ok=True)
        prefixes.append(prefix)
    target = out / 'gpuaddon.bin'
    subprocess.run([sys.executable, args.core / 'scripts/addon-add-dir.py', args.addon, target,
                    *(str(value) for prefix in prefixes for value in (prefix, out / prefix))], check=True)
    assembled = {name: (mode, data) for name, mode, data in addon.read(target)}
    replacements = {'xpc/launchd.plist', 'xpc/launchd.plist.sig'}
    if any(assembled[name] != value for name, value in base.items()
           if name not in replacements and not name.startswith('audio/BTAudioHALPlugin.driver/')):
        raise ValueError('an unrelated baseline addon entry changed')
    entries = {meta['cdhash']: 5 if name.startswith('bluetooth-bin/') else 0 for name, meta in summary.items()}
    entries[cache_hash] = 0
    (out / 'owner-trust.bundle').write_bytes(append_trust(args.trust.read_bytes(), entries))
    report = dict(native_input_sha256=ORIGINAL_SHA256, native_input_uuid=ORIGINAL_UUID,
                  signatures=summary, trust_roles=entries, firmware=manifest, apple_cache_verified=cache_receipt['apple_libxpc_and_codesign_verified'],
                  launchd_cache=cache_receipt,
                  wifi_supplicant_sha256=sha(assembled['wifi-bin/unixnu-wpa_supplicant'][1]),
                  files=len(assembled), hardware_verified=False)
    (out / 'report.json').write_text(json.dumps(report, indent=2) + '\n')


if __name__ == '__main__':
    main()
