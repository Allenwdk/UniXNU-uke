#!/usr/bin/env python3
"""Make the inputs of UniXNU's audit-mobile-graphics-legacy.py, which build-native-legacy-display.py runs first.

No UniXNU or UniXNU-sheng script produces them:

- The candidate: the kernel step's kernel.macho and its prelink plan, under the name the audit reads
  (build/native-prelink/native-owner-mobile-graphics-candidate.*). The audit accepts it by the digest of the original
  IOMobileGraphicsFamily code, which is the same in every kernel of this IPSW.
- The observation of the original class: gdb-stop.py boots the candidate from the payload with
  offline-mobile-graphics-load=1, which loads IOMobileGraphicsFamily from bsd_init. From bsd_init on it stops at
  OSMetaClass's constructor when the class name is IOMobileFramebufferLegacy and runs it to its return, so the report
  holds the metaclass, the parent class and the instance size the constructor was called with
  (build/darwinboot-owner-hvf-mobile-graphics-legacy-class/gdb-stop.json).

Run in the build tree after "unixnu build kernel" and "unixnu build payload". Takes a few minutes under TCG.
"""
from pathlib import Path
import re
import shutil
import subprocess
import sys

from qemu_esp import ROOT, WORK, stage

kernel = WORK / 'kernel/kernel.macho'
if not kernel.is_file():
    raise SystemExit(f'{kernel} missing (unixnu build kernel)')
candidate = ROOT / 'build/native-prelink/native-owner-mobile-graphics-candidate.macho'
candidate.parent.mkdir(parents=True, exist_ok=True)
shutil.copyfile(kernel, candidate)
shutil.copyfile(kernel.with_suffix('.json'), candidate.with_suffix('.json'))

symbols = subprocess.check_output(['xcrun', 'nm', candidate], text=True)


def symbol(name):
    found = re.findall(rf'^([0-9a-f]{{16}}) \w {re.escape(name)}$', symbols, re.M)
    if len(found) != 1:
        raise SystemExit(f'{name} not found once in the candidate')
    return int(found[0], 16)


constructor = symbol('__ZN11OSMetaClassC2EPKcPKS_j')   # OSMetaClass(const char *, const OSMetaClass *, unsigned int)
out = ROOT / 'build/darwinboot-owner-hvf-mobile-graphics-legacy-class'
stage(out / 'esp', candidate, 'offline-mobile-graphics-load=1')
# Argument 1 is the class name. Every class of the kernel and of the kexts loaded before the family is skipped.
name = b'IOMobileFramebufferLegacy\0'.hex()
subprocess.run([sys.executable, ROOT / 'scripts/qemu-entry/gdb-stop.py', hex(constructor), '--esp', out / 'esp',
                '--kernel', candidate, '--fixture', out, '--qemu-ramfb', '--gic-version', '3', '--qemu-memory-mib', '8192',
                '--primary-start-address', hex(symbol('_bsd_init')), '--entry-kernel-bytes-field', f'1:0:{name}',
                '--entry-max-skips', '65536', '--finish-function', '--system-registers', '--timeout', '900'], check=True)
(ROOT / 'build/display-analysis').mkdir(exist_ok=True)       # the audit writes its result there without creating it
print('observation:', out / 'gdb-stop.json')
