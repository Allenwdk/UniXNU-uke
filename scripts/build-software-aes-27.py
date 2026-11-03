#!/usr/bin/env python3
"""Qualify and bind the complete SoftwareAES implementation to 24A446 IOAESAccelerator.

Uses the same authenticated-vtable and N_ABS BTI/B adapters as the board's
24A446 display adapter. Original code and PAC schemas are never modified.
"""
from pathlib import Path
import argparse,hashlib,json,os,shutil,struct,subprocess,sys

parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('--tree',type=Path,default=Path(__file__).resolve().parents[1])
ROOT=parser.parse_args().tree.resolve()
OUT=ROOT/'build/software-aes-27';OUT.mkdir(parents=True,exist_ok=True)
sys.path.insert(0,str(ROOT/'kits/XnuPrelink'))
from kc_image import Image
sha=lambda b:hashlib.sha256(b).hexdigest()
def require(v,why):
 if not v:raise SystemExit(why)
def run(cmd,**kw):return subprocess.check_output([str(v) for v in cmd],timeout=300,**kw)

cache=next((p/'ipsw/24A446/kernelcache.decompressed' for p in ROOT.parents
            if (p/'ipsw/24A446/kernelcache.decompressed').is_file()),None)
require(cache is not None,'Fetch the original 24A446 kernelcache first')
raw=cache.read_bytes();require(sha(raw)=='38e6d9fa5f789b8228f4617ef4c6d2a3fd8ab564abdc9b62920f1db53fc1780a','Wrong kernelcache')
original=Image(raw);fix=original.fixups();slide=0x1f000000000
aes_vtable=0xfffffe0008312668;meta=0xfffffe000bb3ddf0
slots=[fix[aes_vtable+16+i*8] for i in range(181)]
require(aes_vtable+16+181*8 not in fix,'Exact original 181-slot AES vtable')
# Original MetaClass constructor explicitly advertises 0x120 and parent IOCryptoAccelerator.
require(original.read(0xfffffe000a0a7f24,4)==struct.pack('<I',0x52802403),'Original AES size is 0x120')
require(original.read(0xfffffe000797f660,17)==b'IOAESAccelerator\0','Original AES class identity')

source=ROOT/'drivers/SoftwareAES/NativeAES.cpp';header=source.with_name('OriginalAESABI.h')
require(source.is_file() and header.is_file(),'SoftwareAES source missing')
# In 24A446 getAlgorithmName is introduced by IOCryptoAccelerator; declaring it
# first in IOAESAccelerator would use the wrong arm64e virtual PAC discriminator.
header27=header.read_text().replace('OSDeclareDefaultStructors(IOCryptoAccelerator)',
 'OSDeclareDefaultStructors(IOCryptoAccelerator)\npublic:\n  virtual const char *getAlgorithmName();')
header27=header27.replace('  virtual const char *getAlgorithmName();\n  virtual IOReturn performAES',
                         '  const char *getAlgorithmName() override;\n  virtual IOReturn performAES')
(OUT/'OriginalAESABI.h').write_text(header27)
generated=OUT/'NativeAES.cpp'
generated.write_text(source.read_text()+'''\nextern "C" IOService *native_software_aes_create(void) { return new NativeAES; }\n''')
recipe=ROOT/'build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837/iokit/DEVELOPMENT/IOPlatformExpert.cpo.json'
r=json.loads(recipe.read_text().strip().rstrip(','));args=r['arguments'];command=[];i=0
obj=OUT/'NativeAES.o'
while i<len(args):
 value=args[i]
 if value==r['file']:command.append(str(generated))
 elif value in ('-o','-MF','-MT','-MJ'):
  if value=='-o':command.extend(['-o',str(obj)])
  i+=1
 elif value=='-Xclang' and args[i+1].startswith('-W'):i+=1
 elif not value.startswith('-W'):command.append(value)
 i+=1
command+=['-Wall','-Wextra','-Werror','-Wno-unused-parameter','-Wno-unnecessary-virtual-specifier',
          '-Wno-missing-designated-field-initializers',
          '-Wno-c99-designator','-Wno-reorder-init-list','-Wno-deprecated-enum-enum-conversion',
          '-I',str(ROOT/'.cache/xnu-work/osfmk')]
require(any('arm64e-apple-ios27' in x for x in command),'Require actual 27 arm64e compile recipe')
p=subprocess.run(command,cwd=r['directory'],capture_output=True,text=True)
(OUT/'compile.log').write_text(p.stdout+p.stderr);(OUT/'command.json').write_text(json.dumps(command,indent=2)+'\n')
require(p.returncode==0,(p.stdout+p.stderr)[-6000:])

# Object parsing follows build-native-legacy-display-27.py's relocation audit.
data=obj.read_bytes();offset=32;sections=[];symtab=None
for _ in range(struct.unpack_from('<I',data,16)[0]):
 cmd,size=struct.unpack_from('<II',data,offset)
 if cmd==2:symtab=struct.unpack_from('<4I',data,offset+8)
 if cmd==0x19:
  for index in range(struct.unpack_from('<I',data,offset+64)[0]):
   s=offset+72+80*index
   name=data[s:s+16].rstrip(b'\0').decode();va,length,fileoff=struct.unpack_from('<QQI',data,s+32)
   reloc,count=struct.unpack_from('<II',data,s+56);sections.append((name,va,length,fileoff,reloc,count))
 offset+=size
require(symtab is not None,'Object has symbols')
symoff,count,stroff,_=symtab;symbols=[]
for index in range(count):
 strx,kind,_,_,va=struct.unpack_from('<IBBHQ',data,symoff+16*index)
 name=data[stroff+strx:data.index(b'\0',stroff+strx)].decode();symbols.append((name,va,kind))
vtable=next(va for name,va,_ in symbols if name=='__ZTV9NativeAES');entries={}
for _,va,size,fileoff,reloc,count in sections:
 for index in range(count):
  loc,bits=struct.unpack_from('<II',data,reloc+index*8)
  if bits>>27&1 and bits>>25&3==3 and bits>>28==11:
   word=struct.unpack_from('<Q',data,fileoff+loc)[0];entries[va+loc]=(symbols[bits&0xffffff][0],word)
compiled=[entries.get(vtable+16+i*8) for i in range(181)]
require(all(compiled) and vtable+16+181*8 not in entries,'Exact compiled 181-slot NativeAES vtable')
bindings={};checked=[]
for index,((name,word),pointer) in enumerate(zip(compiled,slots)):
 require(word>>63 and word>>48&1 and (word>>49&3)==0 and word>>32&0xffff==pointer[3],
         f'AES slot {index} {name}: real original PAC schema {pointer[3]:#x}, compiled {(word>>32)&0xffff:#x}')
 if '16IOAESAccelerator' in name:
  target=pointer[0]+slide
  require(bindings.get(name,target)==target,'One original target per inherited AES method')
  bindings[name]=target
 checked.append({'slot':index,'name':name,'original':hex(pointer[0]),'diversity':hex(pointer[3])})
bindings.update({'__ZN16IOAESAcceleratorC2EPK11OSMetaClass':0xfffffff00a0a7e14,
                 '__ZN16IOAESAcceleratorD2Ev':0xfffffff00a0a7e84,
                 '__ZN16IOAESAccelerator10gMetaClassE':meta+slide})
undefined={n for n,_,k in symbols if k==1 and '16IOAESAccelerator' in n}
require(undefined<=bindings.keys(),'Every original AES import bound: '+str(undefined-bindings.keys()))
text='.text\n.p2align 2\n';addresses=''
for name,target in sorted(bindings.items()):
 if name.endswith('10gMetaClassE'):
  addresses+=f'.globl {name}\n.set {name}, {target:#x}\n'
 else:
  addresses+=f'.globl {name}.original\n.set {name}.original, {target:#x}\n'
  text+=f'.globl {name}\n{name}:\n bti c\n b {name}.original\n'
(OUT/'original-bindings.S').write_text(text);(OUT/'original-addresses.S').write_text(addresses)
clang=command[0]
for n in ['original-bindings','original-addresses']:
 run([clang,'-target','arm64e-apple-ios27.0','-c',OUT/(n+'.S'),'-o',OUT/(n+'.o')])
objcopy=shutil.which('llvm-objcopy') or '/opt/unixnu-linux/llvm/bin/llvm-objcopy'
run([objcopy,'--remove-section=__DATA,__mod_init_func','--globalize-symbol=__GLOBAL__sub_I_NativeAES.cpp',obj,OUT/'NativeAES.late.o'])
archive=OUT/'libSoftwareAES27.a';archive.unlink(missing_ok=True)
run(['xcrun','ar','rcs',archive,OUT/'NativeAES.late.o',OUT/'original-bindings.o',OUT/'original-addresses.o'])
report={'kernelcache_sha256':sha(raw),'source_sha256':sha(source.read_bytes()),'header_sha256':sha(header.read_bytes()),
 'qualified_header_sha256':sha(header27.encode()),'getAlgorithmName_original_owner':'IOCryptoAccelerator',
 'generated_only_adds_factory':True,'instance_size':0x120,'original_metaclass':hex(meta),'original_vtable':hex(aes_vtable),
 'slots':checked,'bindings':{k:hex(v) for k,v in bindings.items()},'archive_sha256':sha(archive.read_bytes()),
 'deferred_metaclass_registration':True,'runtime_KAT_required_before_registration':True,'hardware_tested':False}
(OUT/'report.json').write_text(json.dumps(report,indent=2)+'\n')
print(f'SoftwareAES27: 181 exact PAC schemas, 0x120 original size, full source KAT retained; {archive}')
