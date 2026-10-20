#!/usr/bin/env python3
"""Build an own signed instrument for actual original MobileGestalt float queries."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import struct
import subprocess
import uuid
ROOT=Path(__file__).resolve().parents[1]
parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--framebuffer-geometry',action='store_true');parser.add_argument('--framebuffer-power',action='store_true');parser.add_argument('--framebuffer-protocol',action='store_true');parser.add_argument('--empty-end',action='store_true');args=parser.parse_args()
if args.empty_end and not args.framebuffer_protocol:parser.error('--empty-end requires the framebuffer protocol instrument')
if sum((args.framebuffer_geometry,args.framebuffer_power,args.framebuffer_protocol))>1:parser.error('Select one framebuffer instrument')
kind='framebuffer-geometry-diagnostic' if args.framebuffer_geometry else 'framebuffer-power-diagnostic' if args.framebuffer_power else 'framebuffer-empty-protocol-diagnostic' if args.empty_end else 'framebuffer-protocol-diagnostic' if (args.framebuffer_protocol or args.framebuffer_power or args.framebuffer_geometry) else 'display-environment-diagnostic'
OUT=ROOT/'build'/kind
OUT.mkdir(parents=True,exist_ok=True)
def sha(path):return hashlib.sha256(Path(path).read_bytes()).hexdigest()
def run(cmd):return subprocess.run([str(v) for v in cmd],check=True,timeout=60)
sdk=subprocess.check_output(['xcrun','--sdk','iphoneos','--show-sdk-path'],text=True).strip()
source=ROOT/('kits/uke/FramebufferGeometryDiagnostic.cpp' if args.framebuffer_geometry else 'kits/uke/FramebufferPowerDiagnostic.cpp' if args.framebuffer_power else 'kits/uke/FramebufferProtocolDiagnostic.cpp' if (args.framebuffer_protocol or args.framebuffer_power or args.framebuffer_geometry) else 'kits/uke/DisplayEnvironmentDiagnostic.cpp');exe=OUT/'air-native'
run(['xcrun','clang++','-target','arm64-apple-ios17.0','-isysroot',sdk,'-std=c++17','-O2','-g',
     '-Wall','-Wextra','-Werror','-fstack-protector-strong','-DFRAMEBUFFER_PROTOCOL_EMPTY_END='+str(int(args.empty_end)),source,'-framework','CoreFoundation',*(['-framework','IOSurface'] if args.framebuffer_geometry else []),'-o',exe])
run(['codesign','--force','--sign','-','--pagesize','4096','--identifier','org.blu-sharky.'+kind,exe])
run(['codesign','--verify','--strict',exe])
details=subprocess.run(['codesign','--display','--verbose=4',exe],capture_output=True,text=True,check=True).stderr
cdhash=re.search(r'^CDHash=([a-f0-9]{40})$',details,re.M)[1]
assert re.search(r'^Page size=4096$',details,re.M)
module=struct.pack('<I16sI',2,uuid.uuid5(uuid.NAMESPACE_URL,kind+':'+cdhash).bytes,1)+bytes.fromhex(cdhash)+bytes([2,0,0,0])
original=(ROOT/'build/original-static-system-trust/original-static-system.bundle').read_bytes()
assert struct.unpack_from('<I',original)[0]==2
offsets=struct.unpack_from('<2I',original,4)
modules=[module,original[offsets[0]:offsets[1]],original[offsets[1]:]]
cursor=16;starts=[]
for item in modules:starts.append(cursor);cursor+=len(item)
trust=struct.pack('<4I',3,*starts)+b''.join(modules);(OUT/'offline-trust.bundle').write_bytes(trust)
manifest=OUT/'test-inputs.json';manifest.write_text(json.dumps(dict(source_sha256=sha(source),
    framebuffer_geometry=args.framebuffer_geometry,framebuffer_power=args.framebuffer_power,framebuffer_protocol=args.framebuffer_protocol,empty_transaction_end=args.empty_end,original_runtime_queries=True,query_key=None if (args.framebuffer_protocol or args.framebuffer_power or args.framebuffer_geometry) else "main-screen-scale",query_defaults=[] if (args.framebuffer_protocol or args.framebuffer_power or args.framebuffer_geometry) else [1,2,7],query_slots=[] if (args.framebuffer_protocol or args.framebuffer_power or args.framebuffer_geometry) else [6,7,8],capabilities_overridden=False,original_desktop=False),indent=2)+'\n')
(OUT/'build-report.json').write_text(json.dumps(dict(process_sha256=sha(exe),source_sha256=sha(source),
    manifest_sha256=sha(manifest),trust_bundle_sha256=hashlib.sha256(trust).hexdigest(),cdhash=cdhash,
    code_signature_page_bytes=4096,original_trust_modules_unchanged=True,offline_test_only=True),indent=2)+'\n')
print('PASS: own ARM64/iOS17 original-runtime instrument built and signed with 4K CodeDirectory: '+kind)
