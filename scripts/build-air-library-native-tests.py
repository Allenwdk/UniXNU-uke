#!/usr/bin/env python3
"""Cross-build the actual generic library refusal/guard suite as an own native process."""
import hashlib
import json
from pathlib import Path
import re
import struct
import subprocess
import uuid
ROOT=Path(__file__).resolve().parents[1];OUT=ROOT/'build/air-library-native-tests'
OUT.mkdir(parents=True,exist_ok=True)
def run(cmd):return subprocess.run([str(v) for v in cmd],check=True,timeout=60)
def sha(p):return hashlib.sha256(Path(p).read_bytes()).hexdigest()
kit=ROOT/'kits/AirNir';source=ROOT/'kits/uke/AirLibraryNativeTests.c';exe=OUT/'air-native'
sdk=subprocess.check_output(['xcrun','--sdk','iphoneos','--show-sdk-path'],text=True).strip()
run(['xcrun','clang','-target','arm64-apple-ios17.0','-isysroot',sdk,'-std=c11','-O2','-Wall','-Wextra','-Werror',
     '-Wno-deprecated-declarations','-fstack-protector-strong','-I',kit,source,kit/'Library.c','-o',exe])
run(['codesign','--force','--sign','-','--pagesize','4096','--identifier','org.blu-sharky.offline-library-controls',exe])
run(['codesign','--verify','--strict',exe])
details=subprocess.run(['codesign','--display','--verbose=4',exe],capture_output=True,text=True,check=True).stderr
cdhash=re.search(r'^CDHash=([a-f0-9]{40})$',details,re.M)[1];assert re.search(r'^Page size=4096$',details,re.M)
module=struct.pack('<I16sI',2,uuid.uuid5(uuid.NAMESPACE_URL,'offline-library-controls:'+cdhash).bytes,1)+bytes.fromhex(cdhash)+bytes([2,0,0,0])
original=(ROOT/'build/original-static-system-trust/original-static-system.bundle').read_bytes();assert struct.unpack_from('<I',original)[0]==2
starts=struct.unpack_from('<2I',original,4);modules=[module,original[starts[0]:starts[1]],original[starts[1]:]]
cursor=16;offsets=[]
for data in modules:offsets.append(cursor);cursor+=len(data)
bundle=struct.pack('<4I',3,*offsets)+b''.join(modules);(OUT/'offline-trust.bundle').write_bytes(bundle)
inputs=dict(sources={str(p.relative_to(ROOT)):sha(p) for p in (source,kit/'Library.h',kit/'Library.c',kit/'tests/test_library.c')},
    kit_revision=subprocess.check_output(['git','-C',kit,'rev-parse','HEAD'],text=True).strip(),positive_profiles=12,transactional_refusals=4456,
    target_libraries=json.loads((ROOT/'build/air-library/report.json').read_text())['libraries'])
manifest=OUT/'test-inputs.json';manifest.write_text(json.dumps(inputs,indent=2)+'\n')
(OUT/'build-report.json').write_text(json.dumps(dict(process_sha256=sha(exe),process_bytes=exe.stat().st_size,source_sha256=sha(source),
    manifest_sha256=sha(manifest),cdhash=cdhash,code_signature_page_bytes=4096,trust_bundle_sha256=hashlib.sha256(bundle).hexdigest(),
    original_trust_modules_unchanged=True,offline_test_only=True,native_execution_qualified=False),indent=2)+'\n')
print('PASS: actual native library guard/refusal program built and explicitly signed; runtime not yet observed')
