#!/usr/bin/env python3
"""Run the actual LLCC controller operations against a host MMIO peer."""
import subprocess
import tempfile
import tomllib
from pathlib import Path

root = Path(__file__).resolve().parents[1]
package = root / 'package'
soc = root.parent / 'UniXNU-sm7675-common/kits/Sm7675Platform'
if not package.exists():
    package = root / 'kits/uke'
    soc = root / 'kits/Sm7675Platform'
board = tomllib.loads((package.parents[0] / 'board.toml').read_text()) if (package.parents[0] / 'board.toml').exists() else None
if board:
    assert board['gpu']['chip_id'] == '0x43030b00'
    assert board['gpu']['system_cache'] == dict(
        default_mode=2, compatible='qcom,cliffs7-llcc', version=50, base='0x25000000', bytes='0x800000',
        broadcast='0x25800000', broadcast_bytes='0x200000', gpu_slice=9, gpu_capacity_kib=3200,
        gpuhtw_slice=11, gpuhtw_capacity_kib=256)
with tempfile.TemporaryDirectory() as tmp:
    exe = Path(tmp) / 'llcc'
    subprocess.run(['clang++', '-std=c++17', '-Wall', '-Wextra', '-Werror', '-O1',
                    '-fsanitize=address,undefined', '-I', str(package), '-I', str(soc),
                    str(package / 'UkeGpuSystemCacheTests.cpp'), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
