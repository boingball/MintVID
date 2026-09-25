/*
 * Hand-written m68k HAM8/HAM6 row encoders, bit-identical to the portable C
 * rows in core/mr_ham.c (mr_ham_encode_ex()), plain and dithered.
 *
 * GCC runs short of registers on that C and spills through the pixel loop,
 * about 60 instructions a pixel for HAM8. Here every value lives in a
 * register:
 *
 *   a0 source RGB24 row      a1 destination row     a2 aux table
 *   a3 abs table centre      a4/a5/a6 held R/G/B    d7 pixel count
 *   d6 dither thresholds (the dithered variants only)
 *
 * The held colour is kept as a pointer, a3 - 4*held, into a table of longs
 * holding |i| for i = -255..255. Indexing it by the new sample C gives
 * |C - held| in one load. A modify write sets the pointer with one lea.
 *
 * HAM8 decision, with m = |C - held| and l = C & 3 per channel: the C
 * compares the base-colour error e_set against er = S + (lR - mR) and
 * likewise for G/B, where S = mR + mG + mB. So it keeps
 *   E    = e_set - S                     (serr8 per channel, minus m)
 *   best = min over channels of (l - m)  (strict <, so R, then G, wins ties)
 * and picks the base colour when E <= best. HAM6 keeps held = S, best of
 * (C & 15) - m, the grey index sum R+G+B, and compares |C - grey| summed
 * against held + best, as the C does.
 *
 * Tables (built by mr_ham.c, passed as arguments; byte offsets):
 *   abs:  511 longs, the pointer passed is entry 255
 *   HAM8 aux: longs serr8[256] at 0, q4[256] at 1024, -4*s4[256] at 2048,
 *             -4*(v & ~3)[256] at 3072; output bytes R/G/B[256] at 4096,
 *             4352, 4608
 *   HAM6 aux: longs grey value[766] at 0, grey index[766] at 3064,
 *             -4*(v & ~15)[256] at 6128; output bytes R/G/B[256] at 7152,
 *             7408, 7664
 * Dithered variants add the row's threshold for pixel x & 3, the low byte
 * of d6, before the modify write's truncation, and rotate d6 a byte per
 * pixel. The caller packs thresholds already scaled to one modify step.
 *
 * Every table access and ALU op is 32-bit and the source advances once
 * per pixel: under qemu, and roughly under a JIT such as Emu68, word
 * results and extra postincrements cost several host instructions each
 * (see core/mr_yuv_m68k.S).
 *
 * ABI: void fn(const uint8_t *src, uint8_t *dst, int32_t w,
 *              const int32_t *abs_centre, const int32_t *aux, uint32_t tw)
 * Saves d2-d7/a2-a6 (44 bytes), so argument N sits at 48 + 4*N.
 */
#if defined(MR_M68K_ASM) && !defined(MR_HAM_NO_ASM)
	.text

/* Dithered modify value: d0 = min(d0 + low byte of d6, 255). d1 free. */
	.macro	MR_HAM_DITHER_ADD
	moveq	#0,%d1
	move.b	%d6,%d1
	add.l	%d1,%d0
	cmp.l	#255,%d0
	bls	88f
	moveq	#0,%d0
	not.b	%d0
88:
	.endm

/* Modify one channel and store the pixel: off is the channel's byte in the
 * source pixel, held its held-colour register, heldtab the aux offset of the
 * new held-pointer offsets (-4 * truncated value) and codetab that of the
 * channel's output bytes (control bits | truncated value), both indexed by
 * the (dithered) value. */
	.macro	MR_HAM_MODIFY off, held, heldtab, codetab, dither
	moveq	#0,%d0
	move.b	\off(%a0),%d0
	.if	\dither
	MR_HAM_DITHER_ADD
	.endif
	move.l	(\heldtab,%a2,%d0.l*4),%d1
	lea	(%a3,%d1.l),\held
	move.b	(\codetab,%a2,%d0.l),(%a1)+
	.endm

	.macro	MR_HAM8_ROW name, dither
	.globl	\name
	.even
\name:
	movem.l	%d2-%d7/%a2-%a6,-(%sp)
	move.l	48(%sp),%a0
	move.l	52(%sp),%a1
	move.l	56(%sp),%d7
	move.l	60(%sp),%a3
	move.l	64(%sp),%a2
	.if	\dither
	move.l	68(%sp),%d6
	.endif
	subq.l	#1,%d7
	bmi	.Lh8_done\@
	moveq	#3,%d5
	move.l	%a3,%a4
	move.l	%a3,%a5
	move.l	%a3,%a6
.Lh8_loop\@:
	/* R: E = serr8[R] - mR, best = (R & 3) - mR, ch = 0 */
	moveq	#0,%d0
	move.b	(%a0),%d0
	move.l	(%a4,%d0.l*4),%d1
	move.l	(%a2,%d0.l*4),%d2
	sub.l	%d1,%d2
	move.l	%d5,%d3
	and.l	%d0,%d3
	sub.l	%d1,%d3
	moveq	#0,%d4
	/* G */
	moveq	#0,%d0
	move.b	1(%a0),%d0
	move.l	(%a5,%d0.l*4),%d1
	add.l	(%a2,%d0.l*4),%d2
	sub.l	%d1,%d2
	and.l	%d5,%d0
	sub.l	%d1,%d0
	cmp.l	%d3,%d0
	bge	1f
	move.l	%d0,%d3
	moveq	#1,%d4
1:
	/* B */
	moveq	#0,%d0
	move.b	2(%a0),%d0
	move.l	(%a6,%d0.l*4),%d1
	add.l	(%a2,%d0.l*4),%d2
	sub.l	%d1,%d2
	and.l	%d5,%d0
	sub.l	%d1,%d0
	cmp.l	%d3,%d0
	bge	2f
	move.l	%d0,%d3
	moveq	#2,%d4
2:
	cmp.l	%d3,%d2
	ble	.Lh8_set\@
	tst.l	%d4
	bne	.Lh8_gb\@
	MR_HAM_MODIFY 0, %a4, 3072, 4096, \dither
	bra	.Lh8_next\@
.Lh8_gb\@:
	subq.l	#1,%d4
	bne	.Lh8_b\@
	MR_HAM_MODIFY 1, %a5, 3072, 4352, \dither
	bra	.Lh8_next\@
.Lh8_b\@:
	MR_HAM_MODIFY 2, %a6, 3072, 4608, \dither
	bra	.Lh8_next\@
.Lh8_set\@:
	/* Base colour: (q4[R] << 4) | (q4[G] << 2) | q4[B]; held = s4. */
	moveq	#0,%d0
	move.b	(%a0),%d0
	move.l	(2048,%a2,%d0.l*4),%d1
	lea	(%a3,%d1.l),%a4
	move.l	(1024,%a2,%d0.l*4),%d2
	moveq	#0,%d0
	move.b	1(%a0),%d0
	move.l	(2048,%a2,%d0.l*4),%d1
	lea	(%a3,%d1.l),%a5
	lsl.l	#2,%d2
	or.l	(1024,%a2,%d0.l*4),%d2
	moveq	#0,%d0
	move.b	2(%a0),%d0
	move.l	(2048,%a2,%d0.l*4),%d1
	lea	(%a3,%d1.l),%a6
	lsl.l	#2,%d2
	or.l	(1024,%a2,%d0.l*4),%d2
	move.b	%d2,(%a1)+
.Lh8_next\@:
	.if	\dither
	ror.l	#8,%d6
	.endif
	addq.l	#3,%a0
	dbra	%d7,.Lh8_loop\@
.Lh8_done\@:
	movem.l	(%sp)+,%d2-%d7/%a2-%a6
	rts
	.endm

	.macro	MR_HAM6_ROW name, dither
	.globl	\name
	.even
\name:
	movem.l	%d2-%d7/%a2-%a6,-(%sp)
	move.l	48(%sp),%a0
	move.l	52(%sp),%a1
	move.l	56(%sp),%d7
	move.l	60(%sp),%a3
	move.l	64(%sp),%a2
	.if	\dither
	move.l	68(%sp),%d6
	.endif
	subq.l	#1,%d7
	bmi	.Lh6_done\@
	move.l	%a3,%a4
	move.l	%a3,%a5
	move.l	%a3,%a6
.Lh6_loop\@:
	/* R: sum = R, held = mR, best = (R & 15) - mR, ch = 0 */
	moveq	#0,%d0
	move.b	(%a0),%d0
	move.l	%d0,%d1
	move.l	(%a4,%d0.l*4),%d2
	moveq	#15,%d3
	and.l	%d0,%d3
	sub.l	%d2,%d3
	moveq	#0,%d4
	/* G */
	moveq	#0,%d0
	move.b	1(%a0),%d0
	add.l	%d0,%d1
	move.l	(%a5,%d0.l*4),%d5
	add.l	%d5,%d2
	and.l	#15,%d0
	sub.l	%d5,%d0
	cmp.l	%d3,%d0
	bge	1f
	move.l	%d0,%d3
	moveq	#1,%d4
1:
	/* B */
	moveq	#0,%d0
	move.b	2(%a0),%d0
	add.l	%d0,%d1
	move.l	(%a6,%d0.l*4),%d5
	add.l	%d5,%d2
	and.l	#15,%d0
	sub.l	%d5,%d0
	cmp.l	%d3,%d0
	bge	2f
	move.l	%d0,%d3
	moveq	#2,%d4
2:
	/* d2 = held + best; d3 = |R-g| + |G-g| + |B-g| for the grey g. */
	add.l	%d3,%d2
	move.l	(%a2,%d1.l*4),%d5
	moveq	#0,%d0
	move.b	(%a0),%d0
	sub.l	%d5,%d0
	move.l	(%a3,%d0.l*4),%d3
	moveq	#0,%d0
	move.b	1(%a0),%d0
	sub.l	%d5,%d0
	add.l	(%a3,%d0.l*4),%d3
	moveq	#0,%d0
	move.b	2(%a0),%d0
	sub.l	%d5,%d0
	add.l	(%a3,%d0.l*4),%d3
	cmp.l	%d2,%d3
	ble	.Lh6_set\@
	tst.l	%d4
	bne	.Lh6_gb\@
	MR_HAM_MODIFY 0, %a4, 6128, 7152, \dither
	bra	.Lh6_next\@
.Lh6_gb\@:
	subq.l	#1,%d4
	bne	.Lh6_b\@
	MR_HAM_MODIFY 1, %a5, 6128, 7408, \dither
	bra	.Lh6_next\@
.Lh6_b\@:
	MR_HAM_MODIFY 2, %a6, 6128, 7664, \dither
	bra	.Lh6_next\@
.Lh6_set\@:
	/* Grey base colour: index from the sum, all three held = grey. */
	neg.l	%d5
	lea	(%a3,%d5.l*4),%a4
	move.l	%a4,%a5
	move.l	%a4,%a6
	move.l	(3064,%a2,%d1.l*4),%d0
	move.b	%d0,(%a1)+
.Lh6_next\@:
	.if	\dither
	ror.l	#8,%d6
	.endif
	addq.l	#3,%a0
	dbra	%d7,.Lh6_loop\@
.Lh6_done\@:
	movem.l	(%sp)+,%d2-%d7/%a2-%a6
	rts
	.endm

	MR_HAM8_ROW mr_ham8_row_m68k, 0
	MR_HAM8_ROW mr_ham8d_row_m68k, 1
	MR_HAM6_ROW mr_ham6_row_m68k, 0
	MR_HAM6_ROW mr_ham6d_row_m68k, 1

#endif /* MR_M68K_ASM */

#if defined(__ELF__)
	.section .note.GNU-stack,"",@progbits
#endif
