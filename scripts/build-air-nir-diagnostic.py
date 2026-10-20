#!/usr/bin/env python3
"""Link a bounded native compiler instrument using the exact native build commands."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shlex
import shutil
import struct
import subprocess
import uuid
ROOT=Path(__file__).resolve().parents[1]
parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--library-input',action='store_true');parser.add_argument('--fragment-inputs',action='store_true');args=parser.parse_args()
if args.fragment_inputs and not args.library_input:parser.error('Fragment inputs use the actual original library path')
BUILD=ROOT/'build/mesa-ir3-ios';OUT=ROOT/('build/air-fragment-inputs-diagnostic' if args.fragment_inputs else 'build/air-library-diagnostic' if args.library_input else 'build/air-nir-diagnostic')
source=ROOT/('kits/uke/AirLibraryDiagnostic.cpp' if args.library_input else 'kits/uke/AirNirDiagnostic.cpp')
OUT.mkdir(parents=True,exist_ok=True)
def run(cmd,**kw):return subprocess.run([str(x) for x in cmd],check=True,timeout=180,**kw)
manifest=ROOT/('scripts/data/native-air-fragment-inputs.json' if args.fragment_inputs else 'scripts/data/native-air-diagnostic-inputs.json')
cases=json.loads(manifest.read_text())['cases']
expected_cases=22 if args.fragment_inputs else 17
assert len(cases)==expected_cases and len({c['name'] for c in cases})==expected_cases
header='struct DiagnosticInput { const char *name, *input, *binary, *abi, *interface; };\n'
header+='static const DiagnosticInput diagnostic_inputs[] = {\n'
for case in cases:
    header+='  {'+', '.join(json.dumps(case[k]) for k in ('name','input_sha256','binary_sha256','abi_sha256','interface_sha256'))+'},\n'
header+='};\n'
(OUT/'AirNirDiagnosticInputs.h').write_text(header)
libraries=[]
if args.library_input:
    inputs=json.loads((ROOT/'build/gpu-analysis/air/target-inputs.json').read_text())['inputs']
    text='struct OriginalLibrary { const char *name, *path, *sha256; };\nstatic const OriginalLibrary original_libraries[] = {\n'
    for library in inputs:
        record=dict(name=Path(library['file']).parent.stem,path='/System/Library/'+library['file'],sha256=library['sha256'])
        libraries.append(record)
        text+='  {'+', '.join(json.dumps(record[k]) for k in ('name','path','sha256'))+'},\n'
    text+='};\n'
    (OUT/'AirLibraryDiagnosticInputs.h').write_text(text)
native_report_path=ROOT/'build/air-nir-ios/build-report.json'
native_report=json.loads(native_report_path.read_text())
shutil.copyfile(native_report_path,OUT/'native-compiler-build-report.json')
for name,digest in native_report['kit_sources'].items():
    if hashlib.sha256((ROOT/'kits/AirNir'/name).read_bytes()).hexdigest()!=digest:raise SystemExit('Native compiler source changed; rebuild it first')
if hashlib.sha256((BUILD/'src/freedreno/ir3/air_nir').read_bytes()).hexdigest()!=native_report['executable_sha256']:raise SystemExit('Native compiler image differs from its build report')
commands=subprocess.check_output(['ninja','-C',str(BUILD),'-t','commands','src/freedreno/ir3/air_nir'],text=True).splitlines()
frontend=str(ROOT/'kits/AirNir/Frontend.cpp')
compile=next(shlex.split(x) for x in commands if shlex.split(x)[-1]==frontend)
old_object=compile[compile.index('-o')+1];obj=OUT/'diagnostic.o';exe=OUT/'air-native'
for flag,value in [('-o',str(obj)),('-MQ',str(obj)),('-MF',str(obj)+'.d')]:compile[compile.index(flag)+1]=value
compile[-1]=str(source)
compile[1:1]=['-I'+str(OUT)]
run(compile,cwd=BUILD)
link=shlex.split(commands[-1]);link[link.index('-o')+1]=str(exe)
link=[str(obj) if x==old_object else x for x in link]
run(link,cwd=BUILD)
run(['xcrun','strip','-S','-x',exe])
run(['codesign','--force','--sign','-','--pagesize','4096','--identifier','org.blu-sharky.offline-air-native',exe])
run(['codesign','--verify','--strict',exe])
details=subprocess.run(['codesign','--display','--verbose=4',exe],capture_output=True,text=True,check=True).stderr
cdhash=re.search(r'^CDHash=([a-f0-9]{40})$',details,re.M)[1]
assert re.search(r'^Page size=4096$',details,re.M)
module=struct.pack('<I16sI',2,uuid.uuid5(uuid.NAMESPACE_URL,'offline-air-native:'+cdhash).bytes,1)+bytes.fromhex(cdhash)+bytes([2,0,0,0])
original=(ROOT/'build/original-static-system-trust/original-static-system.bundle').read_bytes()
n=struct.unpack_from('<I',original)[0];offsets=struct.unpack_from('<'+'I'*n,original,4)
modules=[module]+[original[start:end] for start,end in zip(offsets,(*offsets[1:],len(original)))]
assert n==2
cursor=4+4*len(modules);new_offsets=[]
for m in modules:new_offsets.append(cursor);cursor+=len(m)
bundle=struct.pack('<'+'I'*(len(modules)+1),len(modules),*new_offsets)+b''.join(modules)
(OUT/'offline-trust.bundle').write_bytes(bundle)
def sha(path):return hashlib.sha256(Path(path).read_bytes()).hexdigest()
(OUT/'build-report.json').write_text(json.dumps(dict(process_sha256=sha(exe),process_bytes=exe.stat().st_size,
    native_compiler_build_report_sha256=sha(native_report_path),kit_revision=subprocess.check_output(['git','-C',str(ROOT/'kits/AirNir'),'rev-parse','HEAD'],text=True).strip(),
    source_sha256=sha(source),direct_original_library_input=args.library_input,original_libraries=libraries,manifest_sha256=sha(manifest),cases=len(cases),cdhash=cdhash,code_signature_page_bytes=4096,
    trust_bundle_sha256=hashlib.sha256(bundle).hexdigest(),original_trust_modules_unchanged=True,
    offline_test_only=True,apple_personalization=False,native_execution_qualified=False),indent=2)+'\n')
print('PASS: own signed compiler instrument built; native execution not yet observed')
