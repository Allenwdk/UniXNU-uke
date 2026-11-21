# SPDX-License-Identifier: BSD-2-Clause-Patent
"""24A446 host policy for the P81c's observed active connection tuple."""

import hashlib
from pathlib import Path
import struct
import subprocess
import tempfile


# Signed Uke bluetoothd from the WCN6750 runtime, with the Qca trampolines.
BASE_SHA256 = '5a642752f8b976c379130b3ec06fe1f4eddb916ad94dba59ea6da66760839415'
BASE = 0x100000000
CHECK = 0x100811528
GENERIC = 0x10081152c
LIMITS = 0x10081167c
PATCH = 0x100b9bdc0
PATCH_END = 0x100b9c000


def branch(pc, target, link=False):
    delta = target - pc
    if delta % 4 or not -(1 << 27) <= delta < (1 << 27):
        raise ValueError('P81c interval policy branch is out of range')
    opcode = 0x94000000 if link else 0x14000000
    return struct.pack('<I', opcode | ((delta // 4) & 0x03ffffff))


def patch(data, compiler='clang'):
    if hashlib.sha256(data).hexdigest() != BASE_SHA256:
        raise ValueError('P81c interval policy requires the verified 24A446 Uke bluetoothd')
    offset = CHECK - BASE
    cave = PATCH - BASE
    if data[offset:offset + 4] != bytes.fromhex('b50a00b5') or any(data[cave:PATCH_END - BASE]):
        raise ValueError('P81c interval policy instruction or executable reserve differs')

    # The original validator already checked the input bounds. Its HID
    # exceptions jump to LIMITS, which still checks latency and supervision.
    # The P81c link is the only observed 7.5 ms / CE2 / latency-5 profile on
    # this board. Keep the exception at that exact controller tuple.
    source = f'''
    .text
    cbnz x21, allowed
    cmp w27, #6
    b.ne fallback
    cmp w28, #9
    b.ne fallback
    cmp w24, #5
    b.ne fallback
    cmp w26, #500
    b.ne fallback
    nop
allowed:
    brk #0x714
fallback:
    brk #0x715
    .p2align 3
'''
    with tempfile.TemporaryDirectory(prefix='uke-p81c-interval-') as directory:
        asm = Path(directory) / 'policy.s'
        obj = asm.with_suffix('.o')
        raw = asm.with_suffix('.bin')
        asm.write_text(source)
        subprocess.run([compiler, '-target', 'aarch64-linux-gnu', '-c', asm, '-o', obj], check=True)
        relocations = subprocess.check_output(['llvm-readobj', '--relocations', obj], text=True)
        if 'R_AARCH64_' in relocations:
            raise ValueError('P81c interval policy contains unresolved relocations')
        subprocess.run(['llvm-objcopy', '-O', 'binary', '--only-section=.text', obj, raw], check=True)
        instructions = raw.read_bytes()
    if not 24 < len(instructions) <= PATCH_END - PATCH:
        raise ValueError('P81c interval policy exceeds executable reserve')
    for marker, target, link in ((0x714, LIMITS, False), (0x715, GENERIC, False)):
        trap = struct.pack('<I', 0xd4200000 | (marker << 5))
        at = instructions.find(trap)
        if at < 0 or at % 4 or instructions.count(trap) != 1:
            raise ValueError('P81c interval policy branch placeholder differs')
        instructions = instructions[:at] + branch(PATCH + at, target, link) + instructions[at + 4:]
    new = bytearray(data)
    new[offset:offset + 4] = branch(CHECK, PATCH)
    new[cave:cave + len(instructions)] = instructions
    return bytes(new)
