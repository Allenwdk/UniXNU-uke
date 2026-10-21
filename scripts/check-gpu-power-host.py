#!/usr/bin/env python3
"""Host test for the gated GPU power observation sequence (kits/uke/GpuPowerBoard.h) over a software register peer.

Run without the cross-compiler on PATH. A model test; nothing here touches hardware.
"""
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
KIT = ROOT / 'kits/uke'
if not KIT.is_dir():
    # The device repository keeps the package as package/; the build tree
    # links it as kits/<kit>.
    KIT = ROOT / 'package'
with tempfile.TemporaryDirectory() as tmp:
    exe = Path(tmp) / 'gpb'
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-O1', '-fsanitize=address,undefined',
                    '-I', str(KIT), str(KIT / 'GpuPowerBoardTests.c'), '-o', str(exe)], check=True)
    print(subprocess.run([str(exe)], check=True, capture_output=True, text=True).stdout.strip())
