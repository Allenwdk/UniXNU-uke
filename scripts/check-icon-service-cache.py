#!/usr/bin/env python3
"""Validate icon-service cache scope, detached page hashes and input refusals."""
import argparse
import importlib.util
import json
from pathlib import Path
import plistlib
import tempfile

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--core', type=Path, required=True)
parser.add_argument('--source', type=Path, required=True)
parser.add_argument('--signature', type=Path, required=True)
parser.add_argument('--cache', type=Path, required=True)
args = parser.parse_args()
root = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('icon_cache', root / 'scripts/build-icon-service-cache.py')
icon = importlib.util.module_from_spec(spec)
spec.loader.exec_module(icon)
spec = importlib.util.spec_from_file_location('runtime', args.core / 'scripts/build-boot-runtime.py')
runtime = importlib.util.module_from_spec(spec)
spec.loader.exec_module(runtime)
original_bytes = args.source.read_bytes()
original_signature = args.signature.read_bytes()
original = plistlib.loads(original_bytes)
icon.verify_signature(original_bytes, original_signature)
data = args.cache.read_bytes()
signed = args.cache.with_suffix('.plist.sig').read_bytes()
cdhash = icon.verify_signature(data, signed)
for target, sig in [(bytes([data[0] ^ 1]) + data[1:], signed), (data[:-1], signed)]:
    try:
        icon.verify_signature(target, sig)
    except ValueError:
        pass
    else:
        raise AssertionError('tampered data or stale file length accepted')
assert icon.sign_data(data, original_signature, 'org.unixnu.iconservices.launchd-cache') == signed
policy = {'com.apple.iconservices.iconservicesagent':
          {'FORCE_SOFTWARE_RENDERER': '0', 'CI_ENABLE_METAL_GPU': '0'}}
with tempfile.TemporaryDirectory() as td:
    work = Path(td)
    daemons = work / 'daemons'
    daemons.mkdir()
    output = work / 'launchd.plist'
    runtime.merge_launchd_cache(args.source, daemons, output, policy)
    assert output.read_bytes() == data
    # A shared empty environment dictionary must not apply the icon policy to another job.
    shared = {}
    fixture = {'VersionNumber': 7, 'LaunchDaemons': {
        'icon': {'Label': 'com.apple.iconservices.iconservicesagent', 'EnvironmentVariables': shared},
        'other': {'Label': 'other', 'EnvironmentVariables': shared}}}
    source = work / 'original.plist'
    source.write_bytes(plistlib.dumps(fixture, fmt=plistlib.FMT_BINARY))
    runtime.merge_launchd_cache(source, daemons, output, policy)
    merged = plistlib.loads(output.read_bytes())
    assert merged['LaunchDaemons']['other']['EnvironmentVariables'] == {}
    assert merged['LaunchDaemons']['icon']['EnvironmentVariables'] == next(iter(policy.values()))
    for bad in [{}, {'missing.job': {'MODE': '1'}}, {'com.apple.iconservices.iconservicesagent': {'MODE': 1}}]:
        try:
            runtime.merge_launchd_cache(source, daemons, output, bad)
        except ValueError:
            pass
        else:
            raise AssertionError('invalid or empty override accepted')
print(json.dumps(dict(passed=True, original_jobs=len(original['LaunchDaemons']),
                      output_jobs=len(plistlib.loads(data)['LaunchDaemons']), cdhash=cdhash,
                      tampered_page_and_length_rejected=True, deterministic_signature=True,
                      shared_dictionary_isolation=True, hardware_accepted=False), indent=2))
