#!/usr/bin/env python3
"""Check bounded SSH observations against pins; never qualify a Darwin/GPU boot."""
import hashlib
import json
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
OUT=ROOT/'.cache/sheng-observations'
r=json.loads((OUT/'collected-readonly.json').read_text())
pins=json.loads((ROOT/'scripts/data/adreno-inputs.json').read_text())
assert r['compatible']==['xiaomi,sheng','qcom,sm8550'] and r['machine']=='aarch64'
assert r['kernel_release']=='7.2.6-sm8550-00103-g42f3b40c702a' and r['linux_userspace_page_bytes']==4096
assert not any(r[k] for k in ['cpu_tcr_ttbr_sctlr_observed','gpu_smmu_active_registers_observed','darwin_hardware_boot_observed','target_metal_or_ui_observed','device_writes','gpu_test_submission'])
for name,value in r['firmware'].items():
    expected=pins['firmware']['files']['qcom/'+name]
    assert value['bytes']==expected['bytes'] and value['sha256']==expected['sha256']
nodes=r['device_tree'];gpu=nodes['soc@0/gpu@3d00000'];gmu=nodes['soc@0/gmu@3d6a000'];mmu=nodes['soc@0/iommu@3da0000']
assert gpu['compatible']==['qcom,adreno-43050a01','qcom,adreno']
assert gmu['compatible']==['qcom,adreno-gmu-740.1','qcom,adreno-gmu']
assert mmu['#iommu-cells']==[2] and mmu['dma-coherent'] and 'arm,mmu-500' in mmu['compatible']
assert gpu['iommus']==[mmu['phandle'][0],0,0,mmu['phandle'][0],1,0] and gmu['iommus']==[mmu['phandle'][0],5,0]
assert gpu['reg']==[0,0x3d00000,0,0x40000,0,0x3d9e000,0,0x1000,0,0x3d61000,0,0x800]
assert gmu['reg']==[0,0x3d6a000,0,0x35000,0,0x3d50000,0,0x10000,0,0xb280000,0,0x10000]
assert mmu['reg']==[0,0x3da0000,0,0x40000]
assert r['display_connectors']['card0-DSI-1']['status']==['connected'] and r['display_connectors']['card0-DSI-1']['enabled']==['enabled']
assert set(r['display_connectors']['card0-DSI-1']['modes'])=={'3048x2032'}
state=r['existing_drm_state']['relevant_lines']
assert 'active=1' in state and 'format=XR30 little-endian (0x30335258)' in state and 'pitch[0]=12288' in state
assert any(v.startswith('mode: "3048x2032": 144 ') for v in state)
uapi=json.loads((OUT/'msm-readonly.json').read_text())
assert uapi['ioctl']=='0xc0186440' and uapi['struct_bytes']==24 and not uapi['gpu_test_submission'] and not uapi['set_param']
q={k:v['value'] for k,v in uapi['params'].items()}
assert q['GPU_ID']==0 and q['CHIP_ID']==0xffff43050a01 and q['GMEM_SIZE']==0x300000 and q['MAX_FREQ']==680000000
assert q['VA_START']==0x100000000 and q['VA_SIZE']==0xffff00000000 and q['FAULTS']==0
assert q['HIGHEST_BANK_BIT']==16 and q['UBWC_SWIZZLE']==6 and q['MACROTILE_MODE']==1 and q['PRIORITIES']==12
# The source identifies this exact full chip ID as the no-speedbin fallback.
manifest=json.loads((ROOT/'scripts/data/sheng-readonly-inputs.json').read_text())
for name,digest in manifest['files'].items():
    assert hashlib.sha256((ROOT/name).read_bytes()).hexdigest()==digest
summary=dict(linux_only=True,read_only=True,reported_full_chip_id=hex(q['CHIP_ID']),gmem_bytes=q['GMEM_SIZE'],
    linux_userspace_page_bytes=4096,reported_va_start=hex(q['VA_START']),reported_va_size=hex(q['VA_SIZE']),
    observed_drm_mode='3048x2032@144',input_firmware_hashes_match=True,runtime_gpu_smmu_registers_qualified=False,
    target_darwin_or_metal_qualified=False,
    report_sha256={p.name:hashlib.sha256(p.read_bytes()).hexdigest() for p in [OUT/'collected-readonly.json',OUT/'msm-readonly.json']})
(OUT/'checked-summary.json').write_text(json.dumps(summary,indent=2)+'\n')
print('PASS: actual existing-Linux DT/DRM, three full firmware hashes and read-only MSM capabilities; no Darwin/active-SMMU/Metal boot claim')
