/* Exact PCM = clamp16(U / -66562), no compiler division helper. */
.text
.even
.globl scale_clamp_m68k
.globl _scale_clamp_m68k
scale_clamp_m68k:
 link.w %a6,#0
 movem.l %d2-%d7/%a2-%a5,-(%sp)
 movea.l 8(%a6),%a2
 movea.l 12(%a6),%a3
 move.l 16(%a6),%d7
 add.l %d7,%d7
 lea 256(%a2),%a4
.Lsc_loop:
 move.l (%a2)+,%d0
 move.l (%a2)+,%d1
 suba.l %a5,%a5
 tst.l %d0
 jge .Lsc_mag
 moveq #1,%d6
 movea.l %d6,%a5
 neg.l %d1
 negx.l %d0
.Lsc_mag:
 moveq #16,%d6
 tst.l %d0
 jne .Lsc_sat
 cmpi.l #0x82010000,%d1
 jcs .Lsc_div
.Lsc_sat:
 tst.l %a5
 beq .Lsc_sat_low
 move.w #32767,(%a3)
 bra .Lsc_next
.Lsc_sat_low:
 move.w #-32768,(%a3)
 bra .Lsc_next
.Lsc_div:
 move.l %d1,%d2
 andi.l #65535,%d2
 mulu.w #64526,%d2
 move.l %d1,%d4
 lsr.l %d6,%d4
 mulu.w #64526,%d4
 move.l %d4,%d5
 lsr.l %d6,%d5
 move.l %d4,%d3
 andi.l #65535,%d3
 lsr.l %d6,%d2
 add.l %d2,%d3
 lsr.l %d6,%d3
 add.l %d3,%d5
 move.l %d5,%d3
 mulu.w #1026,%d3
 move.l %d5,%d2
 lsl.l %d6,%d2
 add.l %d3,%d2
 cmp.l %d1,%d2
 jls .Lsc_qok
 subq.l #1,%d5
.Lsc_qok:
 tst.l %a5
 jne .Lsc_positive
 neg.w %d5
.Lsc_positive:
 move.w %d5,(%a3)
.Lsc_next:
 adda.l %d7,%a3
 cmpa.l %a4,%a2
 bne .Lsc_loop
 movem.l (%sp)+,%d2-%d7/%a2-%a5
 unlk %a6
 rts
_scale_clamp_m68k = scale_clamp_m68k
