#!/usr/bin/env python3
"""Serialize and sign the Wi-Fi launchd cache using Apple's actual consumers."""
import argparse
import ctypes
import hashlib
import importlib.util
import json
from pathlib import Path
import platform
import plistlib
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--core', type=Path, required=True)
    parser.add_argument('--device', type=Path, required=True)
    parser.add_argument('--source', type=Path, required=True)
    parser.add_argument('--signature', type=Path, required=True)
    parser.add_argument('--daemons', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    if platform.system() != 'Darwin':
        parser.error('requires Apple /usr/bin/plutil, codesign and libxpc on macOS')
    device = args.device.resolve()
    spec = importlib.util.spec_from_file_location('icon_cache', device / 'scripts/build-icon-service-cache.py')
    cache = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(cache)
    # The existing builder also proves that every original job is preserved,
    # except for the two explicitly selected icon-service environment values.
    report = cache.build(args.core.resolve(), device, args.source.resolve(), args.signature.resolve(),
                         args.output.resolve(), args.daemons.resolve())
    output = args.output.resolve() / 'launchd.plist'
    subprocess.run(['/usr/bin/plutil', '-convert', 'binary1', str(output)], check=True)
    data = output.read_bytes()
    expected = plistlib.loads(data)
    key = '/System/Library/LaunchDaemons/org.unixnu.wifi.supplicant.plist'
    job = expected['LaunchDaemons'][key]
    program = '/usr/libexec/unixnu-wpa_supplicant'
    if job['ProgramArguments'][0] != program or report['added_jobs'] != [key]:
        raise ValueError('exactly one direct supplicant job is required')
    xpc = ctypes.CDLL('/usr/lib/system/libxpc.dylib')
    xpc.xpc_create_from_plist.argtypes = [ctypes.c_void_p, ctypes.c_size_t]
    xpc.xpc_create_from_plist.restype = ctypes.c_void_p
    xpc.xpc_dictionary_get_value.argtypes = [ctypes.c_void_p, ctypes.c_char_p]
    xpc.xpc_dictionary_get_value.restype = ctypes.c_void_p
    xpc.xpc_dictionary_get_count.argtypes = [ctypes.c_void_p]
    xpc.xpc_dictionary_get_count.restype = ctypes.c_size_t
    xpc.xpc_array_get_count.argtypes = [ctypes.c_void_p]
    xpc.xpc_array_get_count.restype = ctypes.c_size_t
    xpc.xpc_array_get_string.argtypes = [ctypes.c_void_p, ctypes.c_size_t]
    xpc.xpc_array_get_string.restype = ctypes.c_char_p
    xpc.xpc_release.argtypes = [ctypes.c_void_p]
    raw = ctypes.create_string_buffer(data)
    parsed = xpc.xpc_create_from_plist(raw, len(data))
    if not parsed:
        raise ValueError('Apple libxpc rejected the serialized cache')
    try:
        jobs = xpc.xpc_dictionary_get_value(parsed, b'LaunchDaemons')
        if not jobs or xpc.xpc_dictionary_get_count(jobs) != len(expected['LaunchDaemons']):
            raise ValueError('libxpc job count mismatch')
        owner = xpc.xpc_dictionary_get_value(jobs, key.encode())
        arguments = xpc.xpc_dictionary_get_value(owner, b'ProgramArguments') if owner else None
        if not arguments or xpc.xpc_array_get_count(arguments) != len(job['ProgramArguments']):
            raise ValueError('libxpc did not preserve owner arguments')
        for i, argument in enumerate(job['ProgramArguments']):
            if xpc.xpc_array_get_string(arguments, i) != argument.encode():
                raise ValueError('libxpc owner argument mismatch')
    finally:
        xpc.xpc_release(parsed)
    detached = output.with_suffix('.plist.sig')
    subprocess.run(['/usr/bin/codesign', '--force', '--sign', '-', '--pagesize', '4096',
                    '--digest-algorithm', 'sha256', '--identifier', 'org.unixnu.wifi.launchd-cache',
                    '--detached', str(detached), str(output)], check=True)
    subprocess.run(['/usr/bin/codesign', '--verify', '--strict', '--detached', str(detached), str(output)], check=True)
    report.update(cdhash=cache.verify_signature(data, detached.read_bytes()),
                  output_sha256=hashlib.sha256(data).hexdigest(), output_bytes=len(data),
                  detached_signature_sha256=hashlib.sha256(detached.read_bytes()).hexdigest(),
                  serializer='Apple /usr/bin/plutil binary1', apple_libxpc_verified=True,
                  apple_codesign_verified=True, hardware_accepted=False)
    (args.output / 'cache-report.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report, indent=2))


if __name__ == '__main__':
    main()
