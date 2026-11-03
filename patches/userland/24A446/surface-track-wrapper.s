.arch armv8.3-a
.section .text.wrapper,"ax"
.global surface_track_wrapper
surface_track_wrapper:
    // Original PACIBSP remains at the real factory entry (also its BTI landing).
    stp x19, x30, [sp, #-16]!
    mov x19, x0
    adr x30, factory_return
    pacibsp
    // Replay the one replaced prologue instruction before entering its body.
    stp x24, x23, [sp, #-0x40]!
    b original_factory_body
factory_return:
    cbz x0, wrapper_return
    mov x1, x0
    mov x0, x19
    mov x19, x1
    bl original_track_surface
    mov x0, x19
wrapper_return:
    ldp x19, x30, [sp], #16
    retab
