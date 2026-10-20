#!/usr/bin/env python3
"""Build an own native iOS17 instrument for the unchanged original Metal runtime."""
import hashlib
import json
from pathlib import Path
import re
import struct
import subprocess
import uuid
ROOT=Path(__file__).resolve().parents[1];OUT=ROOT/'build/metal-runtime-diagnostic'
OUT.mkdir(parents=True,exist_ok=True)
def run(cmd,**kw):return subprocess.run([str(x) for x in cmd],check=True,timeout=60,**kw)
def sha(path):return hashlib.sha256(Path(path).read_bytes()).hexdigest()
run(['python3',ROOT/'scripts/audit-target-metal-device.py'])
manifest=ROOT/'build/gpu-analysis/metal-device-abi.json';abi=json.loads(manifest.read_text())
text='struct MetalMethod { bool meta; const char *selector, *encoding; uintptr_t implementation; };\n'
text+='static const uintptr_t expected_metal_create_address = '+hex(abi['functions']['_MTLCreateSystemDefaultDevice'])+'ULL;\n'
text+='static const size_t expected_metal_instance_bytes = '+str(abi['device']['instance_size'])+';\n'
text+='static const MetalMethod expected_metal_methods[] = {\n'
for kind in ('device','metaclass'):
    for method in abi[kind]['methods']:
        text+='  {'+('true' if kind=='metaclass' else 'false')+', '+json.dumps(method['selector'])+', '+json.dumps(method['encoding'])+', '+hex(method['implementation'])+'ULL},\n'
text+='};\n'
(OUT/'MetalRuntimeDiagnosticInputs.h').write_text(text)
sdk=subprocess.check_output(['xcrun','--sdk','iphoneos','--show-sdk-path'],text=True).strip()
source=ROOT/'kits/uke/MetalRuntimeDiagnostic.cpp';exe=OUT/'air-native'
# Reuse the existing explicit offline init slot. This executable is the Metal
# observer, independently signed and reported; it is not the AIR compiler.
run(['xcrun','clang++','-target','arm64-apple-ios17.0','-isysroot',sdk,'-O2','-g',
     '-Wall','-Wextra','-Werror','-fstack-protector-strong','-fno-exceptions','-fno-rtti',
     '-I'+str(OUT),source,'-o',exe])
run(['xcrun','strip','-S','-x',exe])
run(['codesign','--force','--sign','-','--pagesize','4096','--identifier','org.blu-sharky.offline-metal-runtime',exe])
run(['codesign','--verify','--strict',exe])
details=subprocess.run(['codesign','--display','--verbose=4',exe],capture_output=True,text=True,check=True).stderr
cdhash=re.search(r'^CDHash=([a-f0-9]{40})$',details,re.M)[1]
assert re.search(r'^Page size=4096$',details,re.M)
module=struct.pack('<I16sI',2,uuid.uuid5(uuid.NAMESPACE_URL,'offline-metal-runtime:'+cdhash).bytes,1)+bytes.fromhex(cdhash)+bytes([2,0,0,0])
original=(ROOT/'build/original-static-system-trust/original-static-system.bundle').read_bytes()
n=struct.unpack_from('<I',original)[0];offsets=struct.unpack_from('<'+'I'*n,original,4)
assert n==2
modules=[module]+[original[start:end] for start,end in zip(offsets,(*offsets[1:],len(original)))]
cursor=4+4*len(modules);new_offsets=[]
for m in modules:new_offsets.append(cursor);cursor+=len(m)
bundle=struct.pack('<'+'I'*(len(modules)+1),len(modules),*new_offsets)+b''.join(modules)
(OUT/'offline-trust.bundle').write_bytes(bundle)
(OUT/'build-report.json').write_text(json.dumps(dict(process_sha256=sha(exe),process_bytes=exe.stat().st_size,
    source_sha256=sha(source),manifest_sha256=sha(manifest),instance_methods=len(abi['device']['methods']),
    class_methods=len(abi['metaclass']['methods']),cdhash=cdhash,code_signature_page_bytes=4096,
    trust_bundle_sha256=hashlib.sha256(bundle).hexdigest(),original_trust_modules_unchanged=True,
    offline_test_only=True,apple_personalization=False,native_execution_qualified=False),indent=2)+'\n')
print('PASS: independent own Metal runtime observer cross-built and signed; no GPU implementation')
