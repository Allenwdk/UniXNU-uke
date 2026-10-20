#!/usr/bin/env python3
"""Bind native GPU SMMU integration to pinned DT and Qualcomm source policy."""
import hashlib,json,re,subprocess,sys
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
def sha(p):return hashlib.sha256(p.read_bytes()).hexdigest()
def prepare():
 subprocess.run([sys.executable,ROOT/'kits/uke/audit-adreno-platform.py'],check=True,timeout=30)
 pins=json.loads((ROOT/'scripts/data/adreno-inputs.json').read_text());platform=json.loads((ROOT/'build/adreno-firmware/sheng-platform.json').read_text())
 name='drivers/iommu/arm/arm-smmu/arm-smmu-qcom.c';source=ROOT/'.cache/adreno-source'/name
 if sha(source)!=pins['linux']['files'][name]['sha256']:raise SystemExit('Qualcomm source mismatch')
 text=source.read_text();start=text.index('static int qcom_adreno_smmu_alloc_context_bank(');end=text.index('static bool qcom_adreno_can_do_ttbr1(',start)
 body=text[start:end]
 if not all(s in body for s in ['start = 0;','count = 1;','start = 1;','count = smmu->num_context_banks;']):raise SystemExit('Context ownership policy changed')
 if 'reg |= ARM_SMMU_SCTLR_HUPCF;' not in text:raise SystemExit('Adreno fault continuation policy changed')
 ranges=platform['resources']['smmu']['ranges']
 if len(ranges)!=1 or platform['gpu_streams']!=[0,1] or platform['gmu_stream']!=5:raise SystemExit('Stream binding changed')
 out=ROOT/'build/sheng-gpu-smmu';out.mkdir(exist_ok=True);region=ranges[0]
 header=out/'ShengGpuSmmuSource.h';header.write_text('/* Pinned source/DT policy, not observed hardware. */\n#include <stdint.h>\n'+
 '#define SHENG_GPU_SMMU_BASE UINT64_C('+hex(region['base'])+')\n#define SHENG_GPU_SMMU_BYTES '+str(region['bytes'])+'u\n'+
 '#define SHENG_GPU_SMMU_GPU_STREAMS 2u\n#define SHENG_GPU_SMMU_GMU_SID 5u\nstatic const uint16_t sheng_gpu_smmu_sids[2]={0,1};\n')
 report=dict(linux_revision=pins['linux']['commit'],board_revision=pins['board']['commit'],
 platform_report_sha256=sha(ROOT/'build/adreno-firmware/sheng-platform.json'),qcom_source_sha256=sha(source),header_sha256=sha(header),
 gpu_context_bank=0,gmu_context_bank_min=1,gpu_sids=[0,1],gmu_sid=5,register_range=region,hardware_observed=False)
 (out/'source-report.json').write_text(json.dumps(report,indent=2)+'\n')
 print('PASS: pinned GPU/GMU bank policy and exact SMMU/stream DT binding (source only)');return out,report
if __name__=='__main__':prepare()
