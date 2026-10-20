#!/usr/bin/env python3
"""Verify the original public power request's genuine unsupported result."""
import hashlib
import json
from pathlib import Path
import struct
ROOT=Path(__file__).resolve().parents[1];OUT=ROOT/'build/framebuffer-power-diagnostic'
def require(v,s):
    if not v:raise SystemExit(s)
def sha(p):return hashlib.sha256(p.read_bytes()).hexdigest()
build=json.loads((OUT/'build-report.json').read_text());data=json.loads((OUT/'data-report.json').read_text())
path=ROOT/'build/darwinboot-owner-hvf-legacy-display-start-scale-power/gdb-stop.json';d=json.loads(path.read_text())
require(sha(ROOT/'kits/uke/FramebufferPowerDiagnostic.cpp')==build['source_sha256'] and
        sha(OUT/'air-native')==build['process_sha256']==data['process_sha256'] and
        sha(OUT/'test-inputs.json')==build['manifest_sha256']==data['manifest_sha256'], 'Exact own source, signed process and manifest')
require(sha(OUT/'compiler-data-32.cdr')==data['sha256']==d['scratch_data']['actual_seed_sha256'] and
        sha(OUT/'offline-trust.bundle')==build['trust_bundle_sha256']==d['trust_cache_bundle_sha256'], 'Actual Data/trust identity')
def modules(raw):
    n=struct.unpack_from('<I',raw)[0];starts=struct.unpack_from('<'+'I'*n,raw,4)
    return [raw[a:b] for a,b in zip(starts,(*starts[1:],len(raw)))]
require(modules((OUT/'offline-trust.bundle').read_bytes())[1:]==modules(
        (ROOT/'build/original-static-system-trust/original-static-system.bundle').read_bytes()), 'Both original trust modules unchanged')
require(not d['bounded_timeout_interrupted'] and d['stop_symbol']=='_exception_triage' and
        d['actual_process']['name']=='air-native' and d['actual_process']['pid']==1, 'Actual own diagnostic boundary')
u=d['entry_user_state_candidates'][0];values=struct.unpack('<16Q',bytes.fromhex(u['register_memory']['13:80']['bytes']))
require(int(u['cpsr'],16)&31==0 and u['instruction_bytes'].startswith('000020d4') and
        values[:7]==(0x46425057,1,4096,4096,1,1,0) and values[7] and
        values[8:12]==(0xe00002c7,1,3,0) and values[12]>=24000000 and values[13:]==(0,0,0),
        'Real original display lookup succeeds but public On request returns unsupported, followed by actual one-second wait')
require(d['native_4k_mmu_observed'] and d['original_amfi_table_owner_observed'] and
        d['owner_root_binding_audit']['boot_root_accepted'] and not d['native_legacy_display']['guard_slot'] and
        d['actual_signature_failures']==dict(_cs_validate_page_bad_hash=0,_cs_validate_page_no_hash=0), 'Original AMFI/root/native 4K intact')
(OUT/'checkpoint.json').write_text(json.dumps(dict(report_sha256=sha(path),requested_raw_state=1,
    original_api_status='0xe00002c7',power_transition_completed=False,private_state_modified=False,
    original_desktop=False,hardware_tested=False),indent=2)+'\n')
print('PASS: original public On request genuinely returns Unsupported; no power transition is claimed')
