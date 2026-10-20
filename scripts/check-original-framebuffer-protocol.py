#!/usr/bin/env python3
"""Qualify genuine original Begin and empty End; neither is a rendered frame."""
import hashlib
import json
from pathlib import Path
import struct

ROOT = Path(__file__).resolve().parents[1]
def require(value, reason):
    if not value:
        raise SystemExit(reason)
def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()

observations = []
for empty in (False, True):
    kind = 'framebuffer-' + ('empty-' if empty else '') + 'protocol-diagnostic'
    out = ROOT / 'build' / kind
    build = json.loads((out / 'build-report.json').read_text())
    data = json.loads((out / 'data-report.json').read_text())
    manifest = json.loads((out / 'test-inputs.json').read_text())
    path = ROOT / ('build/darwinboot-owner-hvf-legacy-display-start-scale-' +
                   ('empty-protocol' if empty else 'protocol') + '/gdb-stop.json')
    d = json.loads(path.read_text())
    require(sha(ROOT / 'kits/uke/FramebufferProtocolDiagnostic.cpp') == build['source_sha256'] and
            sha(out / 'air-native') == build['process_sha256'] == data['process_sha256'] and
            sha(out / 'test-inputs.json') == build['manifest_sha256'] == data['manifest_sha256'] and
            manifest['framebuffer_protocol'] and manifest['empty_transaction_end'] == empty,
            'Exact independently signed instrument and protocol variant')
    require(sha(out / 'compiler-data-32.cdr') == data['sha256'] == d['scratch_data']['actual_seed_sha256'] and
            sha(out / 'offline-trust.bundle') == build['trust_bundle_sha256'] == d['trust_cache_bundle_sha256'],
            'Actual staged own Data and trust identity')
    original = (ROOT / 'build/original-static-system-trust/original-static-system.bundle').read_bytes()
    trust = (out / 'offline-trust.bundle').read_bytes()
    old = struct.unpack_from('<2I', original, 4)
    new = struct.unpack_from('<3I', trust, 4)
    require(struct.unpack_from('<I', trust)[0] == 3 and
            [original[old[0]:old[1]], original[old[1]:]] == [trust[new[1]:new[2]], trust[new[2]:]],
            'Both original trust modules unchanged')
    require(not d['bounded_timeout_interrupted'] and d['stop_symbol'] == '_exception_triage' and
            d['actual_process']['name'] == 'air-native' and d['actual_process']['pid'] == 1 and
            len(d['entry_user_state_candidates']) == 1, 'Actual own instrument boundary')
    u = d['entry_user_state_candidates'][0]
    values = struct.unpack('<16Q', bytes.fromhex(u['register_memory']['13:80']['bytes']))
    require(int(u['cpsr'], 16) & 31 == 0 and u['instruction_bytes'].startswith('000020d4') and
            values[:7] == (0x46425054, 1, 4096, 4096, 1, 1, 0) and values[7] and values[8] == 0 and
            struct.unpack('<2d', struct.pack('<2Q', *values[9:11])) == (800, 600) and
            values[11:13] == (0, 1) and values[13:] == ((0, 5, 0) if empty else (2**64-1, 4, 0)),
            'Real original display discovery/geometry/Begin generation and optional empty End')
    require(d['native_4k_mmu_observed'] and d['original_amfi_table_owner_observed'] and
            d['owner_root_binding_audit']['boot_root_accepted'] and
            d['actual_signature_failures'] == dict(_cs_validate_page_bad_hash=0, _cs_validate_page_no_hash=0),
            'Native CPU/user 4K and original AMFI/root remain intact')
    observations.append(dict(report_sha256=sha(path), empty_end=empty, token=values[12],
                             rendered_frame=False, gpu_fence=False))
(ROOT / 'build/framebuffer-protocol-diagnostic/checkpoint.json').write_text(
    json.dumps(dict(observations=observations, original_desktop=False, hardware_tested=False), indent=2)+'\n')
print('PASS: genuine original display discovery, geometry and Begin; separate empty End returns success')
print('Token is a transaction generation, not a GPU fence; empty End presents no rendered frame')
