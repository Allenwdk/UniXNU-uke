#!/usr/bin/env python3
"""Sign and fully verify the original 24A446 OS image on macOS: the root policy of source storage.

`build-owner-os-policy.py` is pinned to the iPadOS 17.7.11 image and known size; this variant takes the image,
the OS profile and the VerifiedRoot checkout as arguments and records what it actually attached. The kernel only
carries the public anchor (trusted-key.bin); the private key never leaves the runner.
"""
import argparse
import ctypes
import hashlib
import importlib.util
import json
import plistlib
import stat
import subprocess
import sys
import tomllib
import uuid
from pathlib import Path

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--image', type=Path, required=True, help='the OS disk image extracted from the IPSW')
parser.add_argument('--profile', type=Path, required=True, help='profiles/os/<profile>/profile.toml (core checkout)')
parser.add_argument('--verified-root', type=Path, required=True, help='kits/VerifiedRoot checkout')
parser.add_argument('--output', type=Path, required=True, help='policy directory (manifest.bin, trusted-key.bin, ...)')
parser.add_argument('--host', type=Path, help='keys/ and verified-root.dylib (default: output/host)')
args = parser.parse_args()
if sys.platform != 'darwin':
    raise SystemExit('macOS hdiutil/diskutil are required')
profile = tomllib.loads(args.profile.read_text())
root_profile = profile['root']
expected_uuid = uuid.UUID(root_profile['os_volume_uuid'])
expected_digest = root_profile['os_image_sha256']
out = args.output.resolve()
out.mkdir(parents=True, exist_ok=True)
host = (args.host or out / 'host').resolve()
keys = host / 'keys'
keys.mkdir(parents=True, exist_ok=True)
if not (keys / 'owner.pem').is_file():
    subprocess.run(['openssl', 'genrsa', '-out', str(keys / 'owner.pem'), '3072'], check=True)
    subprocess.run(['openssl', 'rsa', '-in', str(keys / 'owner.pem'), '-pubout', '-out', str(keys / 'owner-public.pem')],
                   check=True, capture_output=True)
dylib = host / 'verified-root.dylib'
if not dylib.is_file():
    openssl = subprocess.check_output(['brew', '--prefix', 'openssl@3'], text=True).strip()
    subprocess.run(['xcrun', 'clang', '-std=c11', '-Wall', '-Wextra', '-Werror', '-dynamiclib',
                    '-I', str(args.verified_root / 'include'), '-I', f'{openssl}/include',
                    str(args.verified_root / 'src/verified_root.c'), str(args.verified_root / 'tests/openssl_provider.c'),
                    '-L', f'{openssl}/lib', '-lcrypto', '-o', str(dylib)], check=True)
source = args.image.resolve()
digest = hashlib.sha256()
with source.open('rb') as stream:
    for chunk in iter(lambda: stream.read(1 << 20), b''):
        digest.update(chunk)
    stream.seek(-512, 2)
    if stream.read(4) != b'koly':
        raise SystemExit('Expected the actual original UDIF trailer')
attached = plistlib.loads(subprocess.check_output(['hdiutil', 'attach', '-nomount', '-readonly', '-noverify',
                                                   '-noautofsck', '-plist', str(source)]))
entities = attached['system-entities']
whole = [e['dev-entry'] for e in entities if e.get('content-hint') == 'GUID_partition_scheme']
if len(whole) != 1:
    raise SystemExit('Require one whole virtual media mapping')
try:
    info = plistlib.loads(subprocess.check_output(['diskutil', 'info', '-plist', whole[0]]))
    if info.get('BusProtocol') != 'Disk Image' or info.get('Writable') is not False or info.get('DeviceNode') != whole[0]:
        raise SystemExit('Only the actual read-only Disk Image is accepted')
    volume = None
    for entity in entities:
        device = entity.get('dev-entry')
        if not device or device == whole[0]:
            continue
        try:
            volume_info = plistlib.loads(subprocess.check_output(['diskutil', 'info', '-plist', device]))
        except subprocess.CalledProcessError:
            continue
        if volume_info.get('VolumeUUID') == str(expected_uuid).upper() and volume_info.get('Writable') is False:
            volume = device
            break
    if volume is None:
        raise SystemExit(f'no read-only volume with UUID {expected_uuid} in the image')
    disk = Path('/dev/r' + Path(whole[0]).name)
    if not stat.S_ISCHR(disk.stat().st_mode):
        raise SystemExit('Require the verified virtual-media raw character device')
    spec = importlib.util.spec_from_file_location('verified_manifest', args.verified_root / 'tools/manifest.py')
    manifest = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(manifest)
    with disk.open('rb', buffering=1 << 20) as stream:
        record = manifest.make(stream, info['TotalSize'], keys / 'owner.pem', keys / 'owner-public.pem',
                               expected_uuid, 1, out)

    class View(ctypes.Structure):
        _fields_ = [('hashes', ctypes.c_void_p), ('image_bytes', ctypes.c_uint64), ('block_count', ctypes.c_uint64),
                    ('generation', ctypes.c_uint64), ('volume_uuid', ctypes.c_uint8 * 16),
                    ('image_sha256', ctypes.c_uint8 * 32)]

    native = ctypes.CDLL(str(dylib))
    native.vr_host_open.argtypes = [ctypes.c_void_p, ctypes.c_size_t, ctypes.c_void_p, ctypes.c_uint64,
                                    ctypes.POINTER(View)]
    native.vr_host_block.argtypes = [ctypes.POINTER(View), ctypes.c_uint64, ctypes.c_void_p, ctypes.c_size_t]
    encoded = ctypes.create_string_buffer((out / 'manifest.bin').read_bytes())
    anchor = ctypes.create_string_buffer((out / 'trusted-key.bin').read_bytes())
    view = View()
    if native.vr_host_open(encoded, len(encoded) - 1, anchor, 1, ctypes.byref(view)) or \
            bytes(view.volume_uuid) != expected_uuid.bytes:
        raise SystemExit('Real C signature/volume authentication failed')
    complete = hashlib.sha256()
    indices = [0, 1, view.block_count // 2, view.block_count - 1]
    samples, lengths = [], []
    with disk.open('rb', buffering=1 << 20) as stream:
        for index in range(view.block_count):
            block = stream.read(min(4096, view.image_bytes - index * 4096))
            if native.vr_host_block(ctypes.byref(view), index, block, len(block)) != 0:
                raise SystemExit('Actual C block verification failed at ' + str(index))
            complete.update(block)
            if index in indices:
                samples.append(block.ljust(4096, b'\0'))
                lengths.append(len(block))
        if stream.read(1):
            raise SystemExit('Virtual media exceeded its authenticated exact bounds')
    if complete.digest() != bytes(view.image_sha256) or complete.hexdigest() != expected_digest:
        raise SystemExit('Actual complete original OS digest differs from the profile')
    (out / 'original-samples.bin').write_bytes(b''.join(samples))
    report = dict(image_bytes=view.image_bytes, blocks_verified=view.block_count, image_sha256=complete.hexdigest(),
                  volume_uuid=str(expected_uuid).upper(), generation=view.generation,
                  source_compressed_sha256=digest.hexdigest(), source_format='UDIF',
                  host_bus_protocol='Disk Image', host_media_writable=False,
                  trusted_key_sha256=record['trusted_key_sha256'], actual_sample_indices=indices,
                  actual_sample_lengths=lengths, real_c_signature_and_block_verification=True,
                  original_image_modified=False, host_only=True, boot_root_authenticated=False,
                  qemu_tested=False, hardware_tested=False, os_profile=profile['os']['name'])
    (out / 'verification.json').write_text(json.dumps(report, indent=2) + '\n')
    print(f'PASS: {profile["os"]["name"]} OS policy, {view.block_count} blocks, uuid {expected_uuid}')
finally:
    subprocess.run(['hdiutil', 'detach', whole[0]], capture_output=True)
