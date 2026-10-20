#!/usr/bin/env python3
"""Build actual source-reference VPC controls for native iOS17 execution."""
import hashlib
import json
from pathlib import Path
import re
import shlex
import struct
import subprocess
import uuid
ROOT=Path(__file__).resolve().parents[1];OUT=ROOT/'build/adreno-vpc-native-tests';OUT.mkdir(parents=True,exist_ok=True)
KIT=ROOT/'kits/AdrenoPM4';BUILD=ROOT/'build/mesa-ir3-ios';HEADERS=ROOT/'build/adreno-vpc'
def run(cmd,**kw):return subprocess.run([str(v) for v in cmd],check=True,timeout=60,**kw)
def sha(p):return hashlib.sha256(Path(p).read_bytes()).hexdigest()
reference=json.loads((HEADERS/'report.json').read_text());revision=subprocess.check_output(['git','-C',KIT,'rev-parse','HEAD'],text=True).strip()
if revision!=reference['kit_revision']:raise SystemExit('Source reference must match this kit revision')
commands=subprocess.check_output(['ninja','-C',BUILD,'-t','commands','src/freedreno/ir3/air_nir'],text=True).splitlines()
compile=next(shlex.split(x) for x in commands if shlex.split(x)[-1]==str(ROOT/'kits/AirNir/MesaBackend.c'))
for flag in ('-MQ','-MF'):
 i=compile.index(flag);del compile[i:i+2]
compile=[x for x in compile if x not in ('-c','-MD')]
exe=OUT/'air-native';source=ROOT/'kits/uke/AdrenoVPCNativeTests.c'
compile[compile.index('-o')+1]=str(exe);compile[-1]=str(source)
compile[1:1]=['-O2','-Wall','-Wextra','-Werror','-Wno-unused-parameter','-DMESA_REFERENCE','-DFD_NO_DEPRECATED_PACK','-I'+str(KIT),'-I'+str(HEADERS)]
compile += [str(KIT/'AdrenoVPC.c'),str(KIT/'AdrenoPM4.c')];run(compile,cwd=BUILD)
run(['xcrun','strip','-S','-x',exe])
run(['codesign','--force','--sign','-','--pagesize','4096','--identifier','org.blu-sharky.offline-vpc-controls',exe]);run(['codesign','--verify','--strict',exe])
details=subprocess.run(['codesign','--display','--verbose=4',exe],capture_output=True,text=True,check=True).stderr
cdhash=re.search(r'^CDHash=([a-f0-9]{40})$',details,re.M)[1];assert re.search(r'^Page size=4096$',details,re.M)
module=struct.pack('<I16sI',2,uuid.uuid5(uuid.NAMESPACE_URL,'offline-vpc-controls:'+cdhash).bytes,1)+bytes.fromhex(cdhash)+bytes([2,0,0,0])
original=(ROOT/'build/original-static-system-trust/original-static-system.bundle').read_bytes();assert struct.unpack_from('<I',original)[0]==2
starts=struct.unpack_from('<2I',original,4);modules=[module,original[starts[0]:starts[1]],original[starts[1]:]]
cursor=16;offsets=[]
for data in modules:offsets.append(cursor);cursor+=len(data)
bundle=struct.pack('<4I',3,*offsets)+b''.join(modules);(OUT/'offline-trust.bundle').write_bytes(bundle)
inputs=dict(kit_revision=revision,sources={str(p.relative_to(ROOT)):sha(p) for p in (source,KIT/'AdrenoVPC.h',KIT/'AdrenoVPC.c',KIT/'AdrenoPM4.h',KIT/'AdrenoPM4.c',KIT/'test_vpc.c')},
    primary_reference_manifest_sha256=sha(ROOT/'scripts/data/adreno-vpc-inputs.json'),primary_reference=json.loads((ROOT/'scripts/data/adreno-vpc-inputs.json').read_text()),
    generated_header_sha256={p.name:sha(p) for p in sorted(HEADERS.glob('*.xml.h'))},positive_profiles=16597,transactional_refusals=36,
    host_report_sha256=sha(HEADERS/'report.json'))
manifest=OUT/'test-inputs.json';manifest.write_text(json.dumps(inputs,indent=2)+'\n')
(OUT/'build-report.json').write_text(json.dumps(dict(process_sha256=sha(exe),process_bytes=exe.stat().st_size,source_sha256=sha(source),
    manifest_sha256=sha(manifest),cdhash=cdhash,code_signature_page_bytes=4096,trust_bundle_sha256=hashlib.sha256(bundle).hexdigest(),
    original_trust_modules_unchanged=True,offline_test_only=True,native_execution_qualified=False),indent=2)+'\n')
print('PASS: native VPC encoder and actual primary-reference unit cross-built and explicitly signed')
