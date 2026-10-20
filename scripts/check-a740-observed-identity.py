#!/usr/bin/env python3
"""Check current original shader code against the observed complete Linux chip ID."""
import hashlib
import json
import os
from pathlib import Path
import subprocess
ROOT=Path(__file__).resolve().parents[1]
OUT=ROOT/'build/air-nir/observed-identity';OUT.mkdir(parents=True,exist_ok=True)
report=json.loads((ROOT/'build/air-nir/report.json').read_text())
observed=json.loads((ROOT/'scripts/data/observations/msm-readonly.json').read_text())
chip=observed['params']['CHIP_ID']['value']
if chip!=0xffff43050a01 or observed['gpu_test_submission'] or observed['gpu_smmu_active_registers_observed']:
    raise SystemExit('Expected full read-only Linux chip observation, not a hardware shader/SMMU test')
exe=ROOT/'build/mesa-ir3/src/freedreno/ir3/air_nir'
audit=json.loads((ROOT/'build/gpu-analysis/air/target-inputs.json').read_text())
inputs={(v['library'],v['index']):v for v in audit['modules']}
entries=report['original_cases']+report['original_resource_checks']['originals']+report['original_fragment_checks']['originals']+report['original_fragment_input_checks']['originals']
if len(entries)!=22:raise SystemExit('Exact qualified compiler inventory required')
results=[]
for entry in entries:
    lib,index=entry['library'],entry['index']
    src=ROOT/f'build/gpu-analysis/air/{lib}/module-{index}.bc'
    if hashlib.sha256(src.read_bytes()).hexdigest()!=inputs[(lib,index)]['bitcode_sha256']:
        raise SystemExit('Original shader input changed')
    prefix=OUT/f'{lib}-{index}'
    r=subprocess.run([str(exe),str(src),hex(chip),str(prefix),'--compile'],env=dict(os.environ,IR3_SHADER_DEBUG='asmroundtrip'),
        capture_output=True,text=True,timeout=10)
    if r.returncode or not json.loads(r.stdout)['asm_roundtrip']:raise SystemExit('Observed full chip compiler refusal: '+r.stderr)
    digest=hashlib.sha256(prefix.with_suffix('.bin').read_bytes()).hexdigest()
    if digest!=entry['binary_sha256']:raise SystemExit('Observed full chip ID changed original code')
    abi=json.loads(prefix.with_suffix('.ir3.json').read_text())
    if abi['chip_id']!=hex(chip) or abi['gpu_name']!='FD740' or abi['load_shader_consts_via_preamble'] or abi['gpu_executed']:
        raise SystemExit('Actual retained compiler identity/capability differs')
    # All retained ABI fields other than the explicit chip identity must match.
    base=ROOT/'build/air-nir'
    if entry in report['original_resource_checks']['originals']:base=base/'resources'
    elif entry in report['original_fragment_checks']['originals']:base=base/'fragments'
    elif entry in report['original_fragment_input_checks']['originals']:base=base/'fragment-inputs'
    original=json.loads((base/f'{lib}-{index}.ir3.json').read_text())
    original['chip_id']=hex(chip)
    if original!=abi:raise SystemExit('Actual full-ID backend ABI differs from qualified profile')
    results.append(dict(library=lib,index=index,stage=abi['stage'],binary_sha256=digest,whole_abi_equal_except_identity=True))
(OUT/'report.json').write_text(json.dumps(dict(observed_linux_chip_id=hex(chip),catalog_name='FD740',original_shaders=results,
    gpu_execution=False,gpu_smmu_registers_observed=False,native_hardware_driver_qualified=False),indent=2)+'\n')
print('PASS: all 22 unchanged original shaders compile with observed complete 0xffff43050a01; identical binary and full ABI except chip identity, actual roundtrip')
