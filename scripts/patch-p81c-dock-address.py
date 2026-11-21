# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Retry the stock Pencil address lookup for a bonded public-address P81c."""

from pathlib import Path
import runpy
import struct


BASE = 0x100000000
LOOKUP = 0x10012449C
PATCH = 0x100B9BEA0
PATCH_END = 0x100B9C000
LOG = 0x1000038C0
LOG_END = 0x100003A30
ENTITLEMENT = LOG + 0x80
DEVICE_REGISTRY = 0x100C1B210
XPC_TYPE_BOOL = 0x100BFCA00
CENTRAL_LOG = 0x100CAE4B8


def patch(data, compiler='clang'):
    profile = runpy.run_path(str(Path(__file__).with_name('patch-p81c-bluetooth-profile-floor.py')))
    data = bytearray(profile['patch'](data, compiler))
    offset = LOOKUP - BASE
    cave = PATCH - BASE
    log_cave = LOG - BASE
    command_end = 32 + struct.unpack_from('<I', data, 20)[0]
    if (data[offset:offset + 12] != bytes.fromhex('a06301d1549f1f9440090035')
            or command_end > log_cave or any(data[log_cave:LOG_END - BASE])
            or any(data[cave:PATCH_END - BASE])):
        raise ValueError('Pencil address lookup or executable reserve differs')

    # This is the stock central address request, after normal/random resolution.
    # x19 is its NSString, x20 its server session, x24 the initialized registry
    # page, and fp-0x58 its result UUID. The SDK-independent system bond check
    # is the same routine that populates kCBMsgArgPairingState on 24A446.
    source = f'''
    .text
    .set bool_page, {(XPC_TYPE_BOOL & ~4095) - (PATCH & ~4095)}
    .set entitlement_page, {(ENTITLEMENT & ~4095) - (PATCH & ~4095)}
    ldp x8, x9, [x29, #-0x58]
    orr x8, x8, x9
    cbnz x8, resume
    sub sp, sp, #64
    mov x0, x19
    brk #0x741
    lsr x8, x0, #48
    cmp x8, #1
    b.ne done
    and x8, x0, #0xffffffffffff
    cbz x8, done
    str x8, [sp, #48]
    str x25, [sp, #32]
    mov w25, #1
    ldr x0, [x20, #0x10]
    cbz x0, record
    adrp x1, entitlement_page
    add x1, x1, #{ENTITLEMENT & 4095}
    brk #0x742
    cbz x0, record
    str x0, [sp, #40]
    brk #0x743
    adrp x8, bool_page
    ldr x8, [x8, #{XPC_TYPE_BOOL & 4095}]
    cmp x0, x8
    mov w0, #0
    b.ne release_entitlement
    ldr x0, [sp, #40]
    brk #0x744
release_entitlement:
    str w0, [sp, #56]
    ldr x0, [sp, #40]
    brk #0x745
    ldr w8, [sp, #56]
    cbz w8, record

    // Use the same public-address resolver as the stock XPC handler.
    // w3=0 disables the new-peer branch. w6=1 also consults the live LE
    // map, which may know this peer before the persistent maps contain it.
    mov w25, #2
    stp xzr, xzr, [sp]
    mov x8, sp
    ldr x0, [x24, #{DEVICE_REGISTRY & 4095}]
    ldr x1, [sp, #48]
    mov x2, xzr
    mov x3, xzr
    mov x4, xzr
    mov x5, xzr
    mov w6, #1
    brk #0x746
    ldp x8, x9, [sp]
    orr x8, x8, x9
    cbz x8, record
    mov w25, #3
    mov x0, sp
    brk #0x747
    brk #0x748
    cbz x0, record
    str x0, [sp, #40]
    mov x1, x0
    ldr x0, [x24, #{DEVICE_REGISTRY & 4095}]
    adr x2, tag
    brk #0x749
    cbz w0, release_peer
    mov w25, #4
    ldr x0, [sp, #40]
    brk #0x74a
    cbz w0, release_peer
    ldp x8, x9, [sp]
    stp x8, x9, [x29, #-0x58]
    mov w25, #5
release_peer:
    ldr x0, [sp, #40]
    brk #0x74b
record:
    mov w0, w25
    ldr x25, [sp, #32]
    brk #0x74d
done:
    add sp, sp, #64
resume:
    sub x0, x29, #0x58
    brk #0x74c
    .p2align 3
tag:
    .ascii "UkeP81c"
    .zero 16
    .byte 7
'''
    code = profile['assemble'](source, compiler, PATCH, PATCH_END,
        ((0x741, 0x100124740, True), (0x742, 0x10090C3E0, True), (0x743, 0x10090C7D0, True),
         (0x744, 0x10090C3B0, True), (0x745, 0x10090C810, True), (0x746, 0x10082ADF0, True),
         (0x747, 0x100011444, True), (0x748, 0x10090B030, True), (0x749, 0x10002D43C, True),
         (0x74a, 0x10002D364, True), (0x74b, 0x10090B140, True), (0x74c, LOOKUP + 4, False),
         (0x74d, LOG, True)))
    # Preserve the original handler's stack and emit only a stage number.
    log_source = f'''
    .text
    .arch armv8.3-a
    .set image_page, {BASE - (LOG & ~4095)}
    .set log_page, {(CENTRAL_LOG & ~4095) - (LOG & ~4095)}
    pacibsp
    stp x29, x30, [sp, #-32]!
    mov x29, sp
    str w0, [sp, #20]
    mov w8, #0x100
    movk w8, #0x402, lsl #16
    str w8, [sp, #16]
    adrp x0, image_page
    adrp x1, log_page
    ldr x1, [x1, #{CENTRAL_LOG & 4095}]
    mov w2, #0
    adr x3, format
    add x4, sp, #16
    mov w5, #8
    brk #0x74e
    ldp x29, x30, [sp], #32
    retab
format:
    .asciz "UKE P81c dock public lookup result=%{{public}}d"
    .org {ENTITLEMENT - LOG}
    .asciz "com.apple.runningboard.pencil.pairing"
'''
    log = profile['assemble'](log_source, compiler, LOG, LOG_END, ((0x74e, 0x100909E70, True),))
    baseline = runpy.run_path(str(Path(__file__).with_name('patch-p81c-bluetooth-interval.py')))
    data[offset:offset + 4] = baseline['branch'](LOOKUP, PATCH)
    data[cave:cave + len(code)] = code
    data[log_cave:log_cave + len(log)] = log
    return bytes(data)
