#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Generate 24A446 native Pencil battery-card presentation fixups."""

import argparse
import hashlib
import json
from pathlib import Path
import runpy


BASE = 0x100000000
STOCK_DIGEST = '7b005a16606712e7db1991ed33a8d503fb609f66835bf06cce65d4fce45ad4d8'
PRESENTATION = 0x100043AF8
DYNAMIC = 0x1000447A4
SESSION_ID_OFFSET = 0x1000EE580
DYNAMIC_CONTROLLER_OFFSET = 0x1000EAA38
CODE = 0x1000D4410
CODE_END = 0x1000D4500
DYNAMIC_CODE_OFFSET = 0x90
# Aligned, zero-filled __DATA padding after __common. In the pinned image
# the final __DATA chained fixup is at 0xec100.
STATE = 0x1000EE600
PACIBSP = bytes.fromhex('7f2303d5')


def fixups(data, compiler='clang'):
    if hashlib.sha256(data).hexdigest() != STOCK_DIGEST:
        raise ValueError('expected the original J820 24A446 PencilPairingAngel executable')
    if (any(data[CODE - BASE:CODE_END - BASE]) or any(data[STATE - BASE:STATE - BASE + 24])
            or any(data[entry - BASE:entry - BASE + 4] != PACIBSP
                   for entry in (PRESENTATION, DYNAMIC))):
        raise ValueError('Pencil presentation entry or reserved padding changed')

    # 0x43af8 saves the presentation mode (w4) and session (x5) for the
    # post-presentation continuation at 0x43bd0, but calls 0x5da24 first.
    # That first call creates the BannerKit scene. Check modes 2 and 3 here,
    # before any scene creation, and resume duplicate requests as Void.
    # Pack the session generation and both phase bits into one atomic word.
    assembly = f'''
    .text
    .arch armv8.3-a
    .set session_page, {(SESSION_ID_OFFSET & ~4095) - (CODE & ~4095)}
    .set state_page, {(STATE & ~4095) - (CODE & ~4095)}
    .set controller_page, {(DYNAMIC_CONTROLLER_OFFSET & ~4095) - (CODE & ~4095)}
    uxtb w10, w4
    sub w10, w10, #2
    cmp w10, #1
    b.hi present
    cbz x5, present
    mov w11, #1
    lsl w10, w11, w10
    adrp x8, session_page
    ldr x8, [x8, #{SESSION_ID_OFFSET & 4095}]
    ldr x9, [x5, x8]
    add x9, x9, #1
    cbz x9, present
    lsr x11, x9, #62
    cbnz x11, present
    lsl x9, x9, #2
    adrp x8, state_page
    add x8, x8, #{STATE & 4095}
claim:
    ldaxr x11, [x8]
    and x12, x11, #0xfffffffffffffffc
    cmp x9, x12
    b.lo finish
    csel x11, x11, x9, eq
    tst x11, x10
    b.ne finish
    orr x11, x11, x10
    stlxr w12, x11, [x8]
    cbnz w12, claim
present:
    pacibsp
    brk #0x751
finish:
    clrex
    add x17, x22, #8
    ldr x16, [x17]
    movk x17, #0xd707, lsl #48
    braa x16, x17

    // The dynamic battery controller posts a hosted banner asynchronously.
    // Its coordinator only stores the controller after posting. Coalesce
    // session requests during that interval. A controller already installed
    // for a valid session still receives the stock in-place update.
    .org {DYNAMIC_CODE_OFFSET}
    cbz x0, present_dynamic
    adrp x8, session_page
    ldr x8, [x8, #{SESSION_ID_OFFSET & 4095}]
    ldr x9, [x0, x8]
    tbnz x9, #63, present_dynamic
    add x9, x9, #1
    adrp x8, state_page
    add x8, x8, #{STATE & 4095}
    ldr x10, [x8, #16]
    cmp x9, x10
    b.lo skip_dynamic
    adrp x11, controller_page
    ldr x11, [x11, #{DYNAMIC_CONTROLLER_OFFSET & 4095}]
    ldr x11, [x20, x11]
    cbnz x11, present_dynamic
    b.eq skip_dynamic
    str x9, [x8, #16]
present_dynamic:
    pacibsp
    brk #0x753
skip_dynamic:
    ret
'''
    scripts = Path(__file__).resolve().parent
    assemble = runpy.run_path(str(scripts / 'patch-p81c-bluetooth-profile-floor.py'))['assemble']
    branch = runpy.run_path(str(scripts / 'patch-p81c-bluetooth-interval.py'))['branch']
    code = assemble(assembly, compiler, CODE, CODE_END,
                    ((0x751, PRESENTATION + 4, False), (0x753, DYNAMIC + 4, False)))
    # Write the code before publishing its branch entries in ADT.
    changes = [(CODE + at, code[at:at + 16]) for at in range(0, len(code), 16)]
    changes += [(PRESENTATION, branch(PRESENTATION, CODE)),
                (DYNAMIC, branch(DYNAMIC, CODE + DYNAMIC_CODE_OFFSET))]
    result = []
    for address, replacement in changes:
        offset = address - BASE
        original = data[offset:offset + len(replacement)]
        if offset // 4096 != (offset + len(replacement) - 1) // 4096:
            raise ValueError('Pencil text fixup crosses a 4 KiB page')
        result.append(f'PencilPairingAngel,{offset:x},{original.hex()},{replacement.hex()}')
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('stock', type=Path)
    parser.add_argument('output', type=Path)
    parser.add_argument('--compiler', default='clang')
    args = parser.parse_args()
    data = args.stock.read_bytes()
    args.output.write_text(json.dumps(fixups(data, args.compiler), indent=2) + '\n')


if __name__ == '__main__':
    main()
