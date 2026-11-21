# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Allow the verified P81c's active request below the 24A446 profile floor."""

import hashlib
from pathlib import Path
import runpy
import struct
import subprocess
import tempfile


BASE = 0x100000000
PROFILE_FLOOR = 0x1008118F8
PATCH = 0x100B9BDC0
PATCH_END = 0x100B9C000
DEVICE_REGISTRY = 0x100C1B210


def assemble(source, compiler, start, end, branches):
    baseline = runpy.run_path(str(Path(__file__).with_name('patch-p81c-bluetooth-interval.py')))
    with tempfile.TemporaryDirectory(prefix='uke-p81c-assembly-') as directory:
        asm = Path(directory) / 'patch.s'
        obj = asm.with_suffix('.o')
        raw = asm.with_suffix('.bin')
        asm.write_text(source)
        subprocess.run([compiler, '-target', 'aarch64-linux-gnu', '-c', asm, '-o', obj], check=True)
        if 'R_AARCH64_' in subprocess.check_output(['llvm-readobj', '--relocations', obj], text=True):
            raise ValueError('Bluetooth patch contains unresolved relocations')
        subprocess.run(['llvm-objcopy', '-O', 'binary', '--only-section=.text', obj, raw], check=True)
        code = raw.read_bytes()
    if len(code) > end - start:
        raise ValueError('Bluetooth patch exceeds executable reserve')
    for marker, target, link in branches:
        trap = struct.pack('<I', 0xd4200000 | (marker << 5))
        at = code.find(trap)
        if at < 0 or at % 4 or code.count(trap) != 1:
            raise ValueError('Bluetooth patch branch placeholder differs')
        code = code[:at] + baseline['branch'](start + at, target, link) + code[at + 4:]
    return code


def patch(data, compiler='clang'):
    baseline = runpy.run_path(str(Path(__file__).with_name('patch-p81c-bluetooth-interval.py')))
    if hashlib.sha256(data).hexdigest() != baseline['BASE_SHA256']:
        raise ValueError('require the signed 24A446 Uke Bluetooth baseline')
    offset = PROFILE_FLOOR - BASE
    cave = PATCH - BASE
    if data[offset:offset + 8] != bytes.fromhex('0021201e24040054') or any(data[cave:PATCH_END - BASE]):
        raise ValueError('Bluetooth profile instruction or executable reserve differs')

    # At this point the stock validator has already validated the timing tuple.
    # x19 is the peer UUID. The device registry lookup takes std::string,
    # whose short representation on this binary stores the length at +23.
    source = f'''
    .text
    .set registry_page, {(DEVICE_REGISTRY & ~4095) - (PATCH & ~4095)}
    ldurh w8, [x23]
    cmp w8, #6
    b.ne fallback
    ldurh w8, [x22]
    cmp w8, #9
    b.ne fallback
    cmp w24, #5
    b.ne fallback
    mov w8, #5000
    cmp w26, w8
    b.ne fallback
    cbz x19, fallback
    adrp x0, registry_page
    ldr x0, [x0, #{DEVICE_REGISTRY & 4095}]
    cbz x0, fallback
    sub sp, sp, #32
    str s0, [sp, #24]
    adr x8, tag
    ldp x9, x10, [x8]
    stp x9, x10, [sp]
    ldr x9, [x8, #16]
    str x9, [sp, #16]
    mov x1, x19
    mov x2, sp
    brk #0x731
    ldr s0, [sp, #24]
    add sp, sp, #32
    cbz w0, fallback
    brk #0x732
fallback:
    fcmp s8, s0
    brk #0x733
    .p2align 3
tag:
    .ascii "UkeP81c"
    .zero 16
    .byte 7
'''
    code = assemble(source, compiler, PATCH, PATCH_END,
                    ((0x731, 0x10002D43C, True), (0x732, 0x100811978, False),
                     (0x733, PROFILE_FLOOR + 4, False)))
    result = bytearray(data)
    result[offset:offset + 4] = baseline['branch'](PROFILE_FLOOR, PATCH)
    result[cave:cave + len(code)] = code
    return bytes(result)
