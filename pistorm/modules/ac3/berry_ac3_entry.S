    .section .text.header,"ax",@progbits
    .balign 4
    .global berry_ac3_image
    .global berry_ac3_entry
berry_ac3_image:
    .byte 0x42,0x4d,0x4d,0x42       // BMMB
    .byte 0x59,0x4d,0x4d,0x59       // YMMY
    .byte 0x01,0x02,0x00,0x00       // ABI 1, codec module type 2
    .ascii "AC34"
    .zero 48

/* BerryCore CALL_MODULE: x0=mailbox, x1=phys->ARM alias addend, x2=module base.
   Return four independent 32-bit values in w0-w3. */
berry_ac3_entry:
    stp x29, x30, [sp, #-32]!
    mov x29, sp
    add x3, sp, #16
    bl berry_ac3_c_entry
    ldr w0, [sp, #16]
    ldr w1, [sp, #20]
    ldr w2, [sp, #24]
    ldr w3, [sp, #28]
    ldp x29, x30, [sp], #32
    ret
