#!/usr/bin/env python3
"""ASAN/UBSAN checks of the production bounded recorder and pixel writer."""
import hashlib
import json
from pathlib import Path
import subprocess

root=Path(__file__).resolve().parents[1]
out=root/'build/ufs-root-diagnostics/progress/host'
out.mkdir(parents=True,exist_ok=True)
(out/'report.json').unlink(missing_ok=True)
source=root/'kits/uke/BootProgressTests.c'
subprocess.run(['clang','-std=c11','-O1','-g','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',
  '-I'+str(root/'uefi-sheng/Silicon/Silicium/SiliciumPkg/Library/FrameBufferSerialPortLib'),
  str(source),'-o',str(out/'test')],check=True,timeout=60)
r=subprocess.run([str(out/'test')],check=True,capture_output=True,text=True,timeout=30)
(out/'report.json').write_text(json.dumps({'passed':True,'output':r.stdout,
  'sources':{str(p.relative_to(root)):hashlib.sha256(p.read_bytes()).hexdigest() for p in
   [source,source.with_name('BootProgressCore.h'),source.with_name('BootProgressDisplay.h')]},
  'hardware_observed':False},indent=2)+'\n')
print(r.stdout,end='')
