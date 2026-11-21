# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Add the P81c HID profile exception at the connection-request caller."""

import runpy
import struct
import subprocess
import tempfile
from pathlib import Path

BASE = 0x100000000
CALLER_CHECK = 0x100810c1c
CALLER_EXPECTED = 0x34000400
PATCH = 0x100b9bec0
PATCH_END = 0x100b9c000


def branch(pc, target):
    delta = target - pc
    if delta % 4 or not -(1 << 27) <= delta < (1 << 27):
        raise ValueError('P81c caller branch is out of range')
    return struct.pack('<I', 0x14000000 | ((delta // 4) & 0x03ffffff))


def patch(data, compiler='clang'):
    active = runpy.run_path(str(Path(__file__).with_name('patch-p81c-bluetooth-interval.py')))
    data = bytearray(active['patch'](data, compiler))
    offset = CALLER_CHECK - BASE
    if struct.unpack_from('<I', data, offset)[0] != CALLER_EXPECTED:
        raise ValueError('P81c caller policy instruction differs')
    source = '''
        .text
        ldurh w8, [x29, #-0x72]
        cmp w8, #6
        b.ne generic
        ldurh w8, [x29, #-0x74]
        cmp w8, #9
        b.ne generic
        ldurh w8, [x29, #-0x78]
        cmp w8, #12
        b.ne generic
        cmp w20, #5
        b.ne generic
        cmp w19, #500
        b.ne generic
        b accept
    generic:
        brk #0x721
    accept:
        brk #0x722
'''
    with tempfile.TemporaryDirectory(prefix='uke-p81c-interval-v2-') as directory:
        asm = Path(directory) / 'policy.s'
        obj = asm.with_suffix('.o')
        raw = asm.with_suffix('.bin')
        asm.write_text(source)
        subprocess.run([compiler, '-target', 'aarch64-linux-gnu', '-c', asm, '-o', obj], check=True)
        if 'R_AARCH64_' in subprocess.check_output(['llvm-readobj', '--relocations', obj], text=True):
            raise ValueError('P81c caller policy contains unresolved relocations')
        subprocess.run(['llvm-objcopy', '-O', 'binary', '--only-section=.text', obj, raw], check=True)
        code = raw.read_bytes()
    if not 16 < len(code) <= PATCH_END - PATCH:
        raise ValueError('P81c caller policy exceeds executable reserve')
    for marker, target in ((0x721, 0x100810c9c), (0x722, 0x100810c20)):
        trap = struct.pack('<I', 0xd4200000 | (marker << 5))
        at = code.find(trap)
        if at < 0 or code.count(trap) != 1:
            raise ValueError('P81c caller policy placeholder differs')
        # Target addresses are replaced after the code is placed below.
        code = code[:at] + branch(PATCH + at, target) + code[at + 4:]
    data[PATCH - BASE:PATCH - BASE + len(code)] = code
    data[offset:offset + 4] = branch(CALLER_CHECK, PATCH)
    return bytes(data)
