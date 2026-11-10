.section .text.header,"ax",@progbits
.balign 4
.global berry_eac3_image
.global berry_eac3_entry
.global berry_asimd_probe
berry_eac3_image:
.byte 0x42,0x4d,0x4d,0x42
.byte 0x59,0x4d,0x4d,0x59
.byte 0x01,0x02,0x00,0x00
.ascii "E32G"
.zero 48
berry_eac3_entry:
    stp x29,x30,[sp,#-32]!
    mov x29,sp
    add x3,sp,#16
    bl berry_eac3_compat_entry
    ldr w0,[sp,#16]
    ldr w1,[sp,#20]
    ldr w2,[sp,#24]
    ldr w3,[sp,#28]
    ldp x29,x30,[sp],#32
    ret
berry_asimd_probe:
    movi v0.2d,#0
    ret
