#!/usr/bin/env python3
"""Add the stock ABL display hand-off node to the appended sheng DTB.

Stock ABL (UpdateSplashMemInfo) powers down the panel and display controller
when /reserved-memory/splash_region is absent, which mainline sheng DTs omit.
The node mirrors the stock sheng dtbo overlay (dtbo entries "8550 sheng"):
label "cont_splash_region", reg <0 0xb8000000 0 0x2b00000>; ABL then patches
reg with the live splash buffer. Output: build/sheng/sheng-abl.dtb (ignored).
"""
import shutil
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / 'build/sheng/sheng.dtb'
OUTPUT = ROOT / 'build/sheng/sheng-abl.dtb'
NODE = '/reserved-memory/splash_region'
REG = ['0x0', '0xb8000000', '0x0', '0x2b00000']


def main():
    fdtput = shutil.which('fdtput')
    fdtget = shutil.which('fdtget')
    if not fdtput or not fdtget:
        raise SystemExit('Put dtc tools (fdtput/fdtget) on PATH')
    if subprocess.run([fdtget, '-p', SOURCE, '/reserved-memory'], capture_output=True).returncode:
        raise SystemExit('Source DTB has no /reserved-memory')
    shutil.copyfile(SOURCE, OUTPUT)
    subprocess.run([fdtput, '-c', OUTPUT, NODE], check=True)
    subprocess.run([fdtput, '-t', 's', OUTPUT, NODE, 'label', 'cont_splash_region'], check=True)
    subprocess.run([fdtput, '-t', 'x', OUTPUT, NODE, 'reg', *REG], check=True)
    reg = subprocess.check_output([fdtget, '-t', 'x', OUTPUT, NODE, 'reg'], text=True).split()
    if [int(v, 16) for v in reg] != [int(v, 16) for v in REG]:
        raise SystemExit('splash_region round-trip failed')
    print(f'{OUTPUT.relative_to(ROOT)}: {NODE} reg={" ".join(reg)}')


if __name__ == '__main__':
    main()
