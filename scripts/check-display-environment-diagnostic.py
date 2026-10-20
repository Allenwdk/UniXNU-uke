#!/usr/bin/env python3
"""Verify actual original-runtime queries from the independent own process."""
import hashlib
import json
from pathlib import Path
import struct

ROOT=Path(__file__).resolve().parents[1];OUT=ROOT/'build/display-environment-diagnostic'
def require(value,reason):
    if not value:raise SystemExit(reason)
def sha(path):return hashlib.sha256(Path(path).read_bytes()).hexdigest()
build=json.loads((OUT/'build-report.json').read_text());data=json.loads((OUT/'data-report.json').read_text())
path=ROOT/'build/darwinboot-owner-hvf-display-environment-final/gdb-stop.json';d=json.loads(path.read_text())
require(sha(ROOT/'kits/uke/DisplayEnvironmentDiagnostic.cpp')==build['source_sha256'] and
        sha(OUT/'air-native')==build['process_sha256']==data['process_sha256'] and
        sha(OUT/'test-inputs.json')==build['manifest_sha256']==data['manifest_sha256'],
        'Exact own query source, signed executable and staged input manifest')
require(sha(OUT/'compiler-data-32.cdr')==data['sha256']==d['scratch_data']['actual_seed_sha256'] and
        sha(OUT/'offline-trust.bundle')==build['trust_bundle_sha256']==d['trust_cache_bundle_sha256'],
        'Actual Data and explicit own trust bundle')
original=(ROOT/'build/original-static-system-trust/original-static-system.bundle').read_bytes()
trust=(OUT/'offline-trust.bundle').read_bytes();old=struct.unpack_from('<2I',original,4);new=struct.unpack_from('<3I',trust,4)
require(struct.unpack_from('<I',trust)[0]==3 and
        [original[old[0]:old[1]],original[old[1]:]]==[trust[new[1]:new[2]],trust[new[2]:]],
        'Both original trust modules unchanged')
require(not d['bounded_timeout_interrupted'] and d['stop_symbol']=='_exception_triage' and
        d['actual_process']['name']=='air-native' and d['actual_process']['pid']==1 and
        len(d['entry_user_state_candidates'])==1,'Actual owned instrument observation boundary')
u=d['entry_user_state_candidates'][0];values=struct.unpack('<16Q',bytes.fromhex(u['register_memory']['13:80']['bytes']))
require(int(u['cpsr'],16)&31==0 and u['instruction_bytes'].startswith('000020d4') and
        values[:6]==(0x4d47514b,1,4096,4096,1,1) and
        values[6:9]==values[12:15]==(0x3f800000,0x40000000,0x40e00000) and
        values[9:12]==(0,0,0) and values[15]==0,
        'Actual original library/function lookup succeeds; this process receives each requested default float')
require(d['native_4k_mmu_observed'] and d['original_amfi_table_owner_observed'] and
        d['owner_root_binding_audit']['boot_root_accepted'] and
        d['actual_signature_failures']==dict(_cs_validate_page_bad_hash=0,_cs_validate_page_no_hash=0),
        'Original AMFI/root and native 4K remain intact')
(OUT/'checkpoint.json').write_text(json.dumps(dict(checkpoint_sha256=sha(path),process_sha256=build['process_sha256'],
    actual_query_key='main-screen-scale',requested_defaults=[1,2,7],actual_results=[1,2,7],
    scope='own signed offline process only; original SpringBoard answer is not qualified by this query',
    capabilities_overridden=False,original_desktop=False,hardware_tested=False),indent=2)+'\n')
print('PASS: original MobileGestalt queries execute and return requested defaults in the own signed process')
print('This does not qualify the original SpringBoard query or supply a display capability')
