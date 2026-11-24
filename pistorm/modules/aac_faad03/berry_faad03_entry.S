.section .text.berry_entry,"ax",%progbits
.align 4
.global berry_faad_image
berry_faad_image:
    .byte 0x42,0x4d,0x4d,0x42      /* BMMB */
    .byte 0x59,0x4d,0x4d,0x59      /* YMMY */
    .byte 1                         /* ABI */
    .byte 2                         /* CODEC */
    .byte 0,0
    .ascii "FAD1"
    .space 48,0
.global berry_faad_module_entry
berry_faad_module_entry:
    /* Save service context on the original CPU1 stack first. */
    stp x27,x28,[sp,#-16]!
    stp x29,x30,[sp,#-16]!
    mov x27,sp

    /* FAAD2 has large automatic arrays.  Use the tail of the shared work
       buffer as a private stack instead of relying on the Emu68 service stack.
       work_phys @ +48, work_len @ +52, x1 = phys->ARM alias addend. */
    ldr w8,[x0,#48]
    ldr w9,[x0,#52]
    movz w10,#0x8040
    movk w10,#0x0002,lsl #16      /* WORK_MIN = 0x28040 = 163904 */
    cmp w9,w10
    b.lo 1f
    uxtw x8,w8
    add x8,x8,x1
    uxtw x9,w9
    add x9,x8,x9
    and x9,x9,#-16
    mov sp,x9
1:
    bl berry_faad_c_entry

    /* AAPCS64 big-endian: struct {u32 a,b,c,d} returns as
       x0=(a<<32)|b, x1=(c<<32)|d.  BerryCore wants
       w0=a, w1=b, w2=status(c), w3=result2(d). */
    mov x4,x0
    mov x5,x1
    lsr x6,x4,#32
    mov w0,w6
    mov w1,w4
    lsr x6,x5,#32
    mov w2,w6
    mov w3,w5

    /* Restore original service stack and callee-saved context. */
    mov sp,x27
    ldp x29,x30,[sp],#16
    ldp x27,x28,[sp],#16
    ret
