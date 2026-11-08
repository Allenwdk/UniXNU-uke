#!/usr/bin/env python3
"""Declare the owner supplicant's launch role in a v2 trust entry for 24A446.

The unchanged target AMFI accepts category 5 for a system service: platform
validation, with an init-process parent or its original mbfloagent condition.
Do not assign this role to libraries or data. See the pinned binary proof in
docs/status/wifi-amfi-launch-role-2026-11-08.md at the workspace root.
"""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import struct
import uuid


def modules(data):
    count = struct.unpack_from('<I', data)[0]
    if not 0 < count <= 128:
        raise ValueError('invalid trust bundle count')
    offsets = list(struct.unpack_from(f'<{count}I', data, 4)) + [len(data)]
    result = []
    for i in range(count):
        start, end = offsets[i:i + 2]
        if start < 4 + 4 * count or start % 4 or end <= start or end > len(data):
            raise ValueError('invalid module range')
        module = data[start:end]
        version, _, entries = struct.unpack_from('<I16sI', module)
        if version != 2 or len(module) != 24 + 24 * entries:
            raise ValueError('require complete v2 modules')
        hashes = [module[24 + n * 24:44 + n * 24] for n in range(entries)]
        if hashes != sorted(set(hashes)):
            raise ValueError('module hashes must be strictly ordered')
        result.append(module)
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--base', type=Path, required=True)
    parser.add_argument('--userspace', type=Path, required=True)
    parser.add_argument('--cache', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    service = json.loads((args.userspace / 'report.json').read_text())
    binary = args.userspace / 'bin/unixnu-wpa_supplicant'
    if (service.get('target') != 'ios27' or
            service.get('identifier') != 'org.unixnu.wifi.supplicant' or
            service.get('embedded_signature_pages_verified') is not True or
            service['sha256'] != hashlib.sha256(binary.read_bytes()).hexdigest()):
        raise ValueError('require the verified owner supplicant report and exact binary')
    spec = importlib.util.spec_from_file_location('cache_signature',
        Path(__file__).resolve().parent / 'build-icon-service-cache.py')
    verifier = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(verifier)
    cache_hash = verifier.verify_signature((args.cache / 'launchd.plist').read_bytes(),
                                          (args.cache / 'launchd.plist.sig').read_bytes())
    service_hash = service['cdhash']
    entries = {bytes.fromhex(service_hash): bytes([2, 0, 5, 0]),
               bytes.fromhex(cache_hash): bytes([2, 0, 0, 0])}
    if len(entries) != 2 or any(len(key) != 20 for key in entries):
        raise ValueError('service and cache identities must be distinct CDHashes')
    original = modules(args.base.read_bytes())
    for module in original:
        for at in range(24, len(module), 24):
            if module[at:at + 20] in entries:
                raise ValueError('base already contains a target hash; do not leave competing launch roles')
    identity = uuid.uuid5(uuid.NAMESPACE_URL,
        'org.unixnu.uke.wifi.launch-role-24A446:' + service_hash + ':' + cache_hash).bytes
    owner = struct.pack('<I16sI', 2, identity, len(entries))
    owner += b''.join(key + entries[key] for key in sorted(entries))
    output_modules = [*original, owner]
    cursor, offsets = 4 + 4 * len(output_modules), []
    for module in output_modules:
        offsets.append(cursor)
        cursor += len(module)
    result = struct.pack(f'<{len(output_modules) + 1}I', len(output_modules), *offsets) + b''.join(output_modules)
    assert modules(result)[:-1] == original
    args.output.write_bytes(result)
    report = dict(target='ipados-27.0.1-24A446', module_version=2,
        modules=len(output_modules), original_modules_byte_identical=True,
        service_cdhash=service_hash, service_constraint_category=5,
        cache_cdhash=cache_hash, cache_constraint_category=0,
        signature_hash_types_unchanged=True, amfi_decisions_unchanged=True,
        sha256=hashlib.sha256(result).hexdigest(), hardware_verified=False)
    args.output.with_suffix('.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report, indent=2))


if __name__ == '__main__':
    main()
