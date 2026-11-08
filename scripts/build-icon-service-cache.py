#!/usr/bin/env python3
"""Build the board's icon service launchd cache and its owner detached signature."""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import plistlib
import platform
import shutil
import struct
import tempfile
import tomllib


def signature_blobs(signature):
    magic, size, count = struct.unpack_from('>III', signature)
    if magic != 0xfade0cc0 or size != len(signature) or count > 128:
        raise ValueError('invalid embedded signature container')
    blobs = {}
    for index in range(count):
        slot, offset = struct.unpack_from('>II', signature, 12 + index * 8)
        _, length = struct.unpack_from('>II', signature, offset)
        if offset < 12 + count * 8 or length < 8 or offset + length > size or slot in blobs:
            raise ValueError('invalid signature blob range')
        blobs[slot] = signature[offset:offset + length]
    return blobs


def verify_signature(data, signature):
    blobs = signature_blobs(signature)
    directory = blobs[0]
    magic, size, version, flags, hashes, identifier, special, slots, limit = struct.unpack_from('>9I', directory)
    # Apple codesign chooses older CodeDirectory versions for detached data;
    # the original OS template uses 0x20500. Their base hash fields are shared.
    if magic != 0xfade0c02 or size != len(directory) or version not in (
            0x20001, 0x20100, 0x20200, 0x20300, 0x20400, 0x20500, 0x20600):
        raise ValueError(f'unexpected CodeDirectory layout: magic={magic:x}, version={version:x}, size={size}/{len(directory)}')
    if directory[36:40] != bytes([32, 2, 0, 12]) or not flags & 2:
        raise ValueError('require ad-hoc SHA256 with 4KiB signing pages')
    if limit != len(data) or slots != (limit + 4095) // 4096 or hashes + slots * 32 != size:
        raise ValueError('signature does not cover the exact file length')
    if hashes - special * 32 < 40 or not 40 <= identifier < hashes - special * 32:
        raise ValueError('invalid CodeDirectory identifier/hash ranges')
    for index in range(slots):
        expected = directory[hashes + index * 32:hashes + (index + 1) * 32]
        if hashlib.sha256(data[index * 4096:(index + 1) * 4096]).digest() != expected:
            raise ValueError('signed page hash mismatch')
    for slot in range(1, special + 1):
        expected = directory[hashes - slot * 32:hashes - (slot - 1) * 32]
        observed = hashlib.sha256(blobs[slot]).digest() if slot in blobs else bytes(32)
        if expected != observed:
            raise ValueError('special slot hash mismatch')
    return hashlib.sha256(directory).digest()[:20].hex()


def sign_data(data, template, identifier):
    """Use the selected OS cache's CodeDirectory layout for an owner ad-hoc signature."""
    blobs = signature_blobs(template)
    original = blobs[0]
    _, _, version, _, old_hashes, ident_offset, special, _, _ = struct.unpack_from('>9I', original)
    if version != 0x20500 or ident_offset != 96 or special != 2:
        raise ValueError('unsupported original detached data signature')
    if original[44:88] != bytes(44) or original[92:96] != bytes(4):
        raise ValueError('unexpected scatter/team/runtime fields in data signature')
    identifier = identifier.encode('ascii') + b'\0'
    special_hashes = original[old_hashes - special * 32:old_hashes]
    prefix = bytearray(original[:ident_offset] + identifier + special_hashes)
    slots = (len(data) + 4095) // 4096
    hashes = b''.join(hashlib.sha256(data[i:i + 4096]).digest() for i in range(0, len(data), 4096))
    struct.pack_into('>I', prefix, 4, len(prefix) + len(hashes))
    struct.pack_into('>I', prefix, 12, 2)  # CS_ADHOC, owner trust supplies the identity.
    struct.pack_into('>I', prefix, 88, 0)  # No hardened-runtime policy for data.
    struct.pack_into('>I', prefix, 16, len(prefix))
    struct.pack_into('>II', prefix, 28, slots, len(data))
    directory = bytes(prefix) + hashes
    requirements = blobs[2]
    result = struct.pack('>7I', 0xfade0cc0, 28 + len(directory) + len(requirements), 2,
                         0, 28, 2, 28 + len(directory)) + directory + requirements
    verify_signature(data, result)
    return result


def build(core, device, source, signature, destination, daemons=None, apple_cache=None):
    policy = tomllib.loads((device / 'board.toml').read_text())['payload']['runtime']['launchd_job_environment']
    expected = {'com.apple.iconservices.iconservicesagent':
                {'FORCE_SOFTWARE_RENDERER': '0', 'CI_ENABLE_METAL_GPU': '0'}}
    if policy != expected:
        raise ValueError('unexpected icon service policy')
    original_data = source.read_bytes()
    template = signature.read_bytes()
    original_cdhash = verify_signature(original_data, template)
    original = plistlib.loads(original_data)
    if original['VersionNumber'] != 7:
        raise ValueError('require the selected iPadOS 27 launchd cache')
    if daemons and not apple_cache and platform.system() != 'Darwin':
        raise ValueError('owner Wi-Fi jobs require a macOS-built --apple-cache; plistlib/plutil shims are insufficient')
    destination.mkdir(parents=True, exist_ok=False)
    output = destination / 'launchd.plist'
    apple_report = None
    if apple_cache:
        apple_report = json.loads((apple_cache / 'cache-report.json').read_text())
        data = (apple_cache / 'launchd.plist').read_bytes()
        detached = (apple_cache / 'launchd.plist.sig').read_bytes()
        if (apple_report.get('serializer') != 'Apple /usr/bin/plutil binary1' or
                apple_report.get('apple_libxpc_verified') is not True or
                apple_report.get('apple_codesign_verified') is not True or
                apple_report.get('original_sha256') != hashlib.sha256(original_data).hexdigest() or
                apple_report.get('output_sha256') != hashlib.sha256(data).hexdigest() or
                apple_report.get('detached_signature_sha256') != hashlib.sha256(detached).hexdigest() or
                apple_report.get('cdhash') != verify_signature(data, detached)):
            raise ValueError('Apple cache provenance, bytes or signature mismatch')
        output.write_bytes(data)
    else:
        spec = importlib.util.spec_from_file_location('boot_runtime', core / 'scripts/build-boot-runtime.py')
        runtime = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(runtime)
        with tempfile.TemporaryDirectory() as td:
            runtime.merge_launchd_cache(source, daemons or Path(td), output, policy)
    merged = plistlib.loads(output.read_bytes())
    label = next(iter(policy))
    keys = [key for key, job in original['LaunchDaemons'].items() if job.get('Label') == label]
    if len(keys) != 1:
        raise ValueError('require exactly one original icon service job')
    # The owner jobs this run adds are the only new keys; everything else must
    # still equal the input, apart from the icon environment itself.
    added = [key for key in merged['LaunchDaemons'] if key not in original['LaunchDaemons']]
    wanted = sorted('/System/Library/LaunchDaemons/' + path.name for path in (daemons.glob('*.plist') if daemons else []))
    if sorted(added) != wanted:
        raise ValueError('cache gained keys outside the owner job list: ' + ', '.join(sorted(added)))
    for path in daemons.glob('*.plist') if daemons else []:
        if merged['LaunchDaemons']['/System/Library/LaunchDaemons/' + path.name] != plistlib.loads(path.read_bytes()):
            raise ValueError('cache owner job differs from the staged service: ' + path.name)
    reverted = plistlib.loads(output.read_bytes())
    for key in added:
        del reverted['LaunchDaemons'][key]
    reverted['LaunchDaemons'][keys[0]]['EnvironmentVariables'] = original['LaunchDaemons'][keys[0]]['EnvironmentVariables']
    if reverted != original or merged['LaunchDaemons'][keys[0]]['EnvironmentVariables'] != policy[label]:
        raise ValueError('cache changed outside the two selected environment variables')
    signed = ((apple_cache / 'launchd.plist.sig').read_bytes() if apple_cache else
              sign_data(output.read_bytes(), template, 'org.unixnu.iconservices.launchd-cache'))
    (destination / 'launchd.plist.sig').write_bytes(signed)
    report = dict(original_sha256=hashlib.sha256(original_data).hexdigest(), original_cdhash=original_cdhash,
                  output_sha256=hashlib.sha256(output.read_bytes()).hexdigest(), output_bytes=output.stat().st_size,
                  cdhash=verify_signature(output.read_bytes(), signed), job=keys[0], environment=policy[label],
                  original_jobs=len(original['LaunchDaemons']), new_jobs=len(merged['LaunchDaemons']),
                  added_jobs=added, other_entries_unchanged=True, hardware_accepted=False,
                  serializer=apple_report['serializer'] if apple_report else
                      ('Apple plutil binary1' if platform.system() == 'Darwin' else 'host plutil; no Apple consumer validation'),
                  apple_libxpc_verified=bool(apple_report and apple_report['apple_libxpc_verified']),
                  detached_signature_sha256=hashlib.sha256(signed).hexdigest())
    if apple_cache:
        shutil.copy2(apple_cache / 'cache-report.json', destination / 'apple-cache-report.json')
    return report


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--core', type=Path, required=True)
    parser.add_argument('--device', type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument('--source', type=Path, required=True)
    parser.add_argument('--signature', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--daemons', type=Path,
                        help='owner launchd jobs (org.unixnu.*) to add beside the icon policy')
    parser.add_argument('--apple-cache', type=Path,
                        help='verified macOS artifact (cache, detached signature, cache-report.json)')
    args = parser.parse_args()
    report = build(args.core.resolve(), args.device.resolve(), args.source, args.signature, args.output,
                   args.daemons.resolve() if args.daemons else None,
                   args.apple_cache.resolve() if args.apple_cache else None)
    print(json.dumps(report, indent=2))
