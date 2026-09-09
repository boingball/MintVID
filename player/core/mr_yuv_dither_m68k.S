/*
 * MintVID - hand-tuned m68k fused YUV420P -> indexed dither.
 *
 * The normal path consumes the three weighted 16x256 LUTs built by
 * mr_yuv_dither.c.  For the 8-bit 6x6x6 cube, lut_b is already the
 * unweighted 6-level quantiser (0..5).  This file detects that depth once
 * from lut_b[8][128] (3 for the 6-level cube, 1 for the 2-level blue
 * channel used by 4/5-bit output), then uses only that one 4 KB table plus
 * two tiny weight tables:
 *
 *   red   = q * 36
 *   green = q * 6
 *   blue  = q
 *
 * That keeps the hot dither lookup footprint at ~4 KB instead of 12 KB on
 * 8-bit AGA while preserving the exact same palette indices.  4/5-bit
 * output keeps the established weighted-LUT path unchanged.
 */
#if defined(MR_M68K_ASM)
	.text
	.even
	.globl	mr_yuv420_dither8_m68k

/*
 * void mr_yuv420_dither8_m68k(const uint8_t *y_plane, int y_stride,
 *     const uint8_t *u_plane, int u_stride, const uint8_t *v_plane,
 *     int v_stride, int width, int dst_h, int vscale, uint8_t *out,
 *     int out_stride, int y_base, const int *luma_x298, const int *e_x409,
 *     const int *d_xm100, const int *e_xm208, const int *d_x516,
 *     const uint8_t *lut_r, const uint8_t *lut_g, const uint8_t *lut_b);
 *
 * Saves d2-d7/a2-a6 (44 bytes) plus 20 bytes local scratch, so stack
 * arguments start at +68:
 *   68 y_plane       100 vscale       132 d_x516
 *   72 y_stride      104 out          136 lut_r
 *   76 u_plane       108 out_stride   140 lut_g
 *   80 u_stride      112 y_base       144 lut_b
 *   84 v_plane       116 luma_x298
 *   88 v_stride      120 e_x409
 *   92 width         124 d_xm100
 *   96 dst_h         128 e_xm208
 *
 * Scratch:
 *   0 red_add (long)
 *   4 green_add (long)
 *   8 blue_add (long)
 *  12 row's four Bayer taps
 *  16 tap << 8 (word)
 *  18 padding (word; preserves the existing stack argument offsets)
 */
mr_yuv420_dither8_m68k:
	movem.l	%d2-%d7/%a2-%a6,-(%sp)
	sub.l	#20,%sp

	move.l	92(%sp),%d2
	tst.l	%d2
	ble	.Lyd_done
	tst.l	96(%sp)
	ble	.Lyd_done

	clr.l	%d0
	move.l	100(%sp),%d1
	asr.l	#1,%d1

	move.l	112(%sp),%d3
	and.l	#3,%d3

	movea.l	136(%sp),%a5		/* lut_r on the normal path */
	movea.l	140(%sp),%a6		/* lut_g on the normal path */

	/* Detect depth=8 without changing the C/asm ABI.  At threshold 8 the
	 * dither offset is zero.  lut_b[8][128] is therefore 3 for the 6-level
	 * blue quantiser (depth=8), but 1 for the 2-level blue quantiser used by
	 * depth=4/5.  In q6 mode a5 becomes lut_b and a6 becomes the tiny
	 * contribution-weight table. */
	movea.l	144(%sp),%a4
	clr.l	%d6
	move.b	2176(%a4),%d6		/* 8 * 256 + 128 */
	cmpi.b	#3,%d6
	bne	.Lyd_weighted_kernel
	movea.l	%a4,%a5
	lea	.Lyd_q6_weights,%a6
	bra	.Lyd_q6_kernel

	/* Assemble two compact kernels: choose the palette once per call,
	 * removing three mode tests and their branches from every pixel.
	 * Both retain the original clipping, Bayer taps and LUT footprint. */
	.macro yd_kernel q6
	cmpi.l	#2,100(%sp)
	beq	.Lyd_v2_init\@

.Lyd_row\@:
	move.l	%d0,%d6
	cmp.l	96(%sp),%d6
	bge	.Lyd_done

	move.l	%d1,%d6
	muls.l	72(%sp),%d6
	movea.l	68(%sp),%a0
	adda.l	%d6,%a0

	move.l	%d1,%d4
	asr.l	#1,%d4

	move.l	%d4,%d6
	muls.l	80(%sp),%d6
	movea.l	76(%sp),%a1
	adda.l	%d6,%a1

	move.l	%d4,%d6
	muls.l	88(%sp),%d6
	movea.l	84(%sp),%a2
	adda.l	%d6,%a2

	move.l	%d0,%d6
	muls.l	108(%sp),%d6
	movea.l	104(%sp),%a3
	adda.l	%d6,%a3
	bra	.Lyd_row_ready\@

.Lyd_v2_init\@:
	movea.l	68(%sp),%a0
	adda.l	72(%sp),%a0
	movea.l	76(%sp),%a1
	movea.l	84(%sp),%a2
	movea.l	104(%sp),%a3

.Lyd_v2_row\@:
	move.l	%d0,%d6
	cmp.l	96(%sp),%d6
	bge	.Lyd_done

.Lyd_row_ready\@:
	lea	.Lyd_bayer4,%a4
	move.l	%d3,%d6
	lsl.l	#2,%d6
	adda.l	%d6,%a4
	move.l	(%a4),%d6
	move.l	%d6,12(%sp)

	clr.l	%d4

.Lyd_pair_test\@:
	move.l	%d2,%d6
	subq.l	#1,%d6
	cmp.l	%d6,%d4
	bge	.Lyd_pair_done\@

	move.l	%d4,%d5
	asr.l	#1,%d5

	clr.l	%d6
	move.b	(0,%a1,%d5.l),%d6
	movea.l	132(%sp),%a4
	move.l	(%a4,%d6.l*4),%d7
	add.l	#128,%d7
	move.l	%d7,8(%sp)

	movea.l	124(%sp),%a4
	move.l	(%a4,%d6.l*4),%d7

	clr.l	%d6
	move.b	(0,%a2,%d5.l),%d6
	movea.l	128(%sp),%a4
	add.l	(%a4,%d6.l*4),%d7
	add.l	#128,%d7
	move.l	%d7,4(%sp)

	movea.l	120(%sp),%a4
	move.l	(%a4,%d6.l*4),%d7
	add.l	#128,%d7
	move.l	%d7,0(%sp)

	/* lane 0 */
	clr.l	%d6
	move.b	(0,%a0,%d4.l),%d6
	sub.l	#16,%d6
	bge.b	.Lyd_l0ok\@
	moveq	#0,%d6
.Lyd_l0ok\@:
	movea.l	116(%sp),%a4
	move.l	(%a4,%d6.l*4),%d5

	move.l	%d4,%d6
	and.l	#3,%d6
	clr.l	%d7
	move.b	(12,%sp,%d6.l),%d7
	lsl.w	#8,%d7
	move.w	%d7,16(%sp)

	/* red */
	move.l	%d5,%d6
	add.l	0(%sp),%d6
	asr.l	#8,%d6
	cmp.l	#255,%d6
	bls.b	2f
	tst.l	%d6
	bmi.b	1f
	move.l	#255,%d6
	bra.b	2f
1:	moveq	#0,%d6
2:
	add.w	16(%sp),%d6
	.if \q6
	move.b	(%a5,%d6.w),%d6
	and.l	#255,%d6
	move.b	(%a6,%d6.w),%d7
	.else
	move.b	(%a5,%d6.w),%d7
	.endif
	/* green */
	move.l	%d5,%d6
	add.l	4(%sp),%d6
	asr.l	#8,%d6
	cmp.l	#255,%d6
	bls.b	2f
	tst.l	%d6
	bmi.b	1f
	move.l	#255,%d6
	bra.b	2f
1:	moveq	#0,%d6
2:
	add.w	16(%sp),%d6
	.if \q6
	move.b	(%a5,%d6.w),%d6
	and.l	#255,%d6
	add.b	8(%a6,%d6.w),%d7
	.else
	add.b	(%a6,%d6.w),%d7
	.endif
	/* blue */
	move.l	%d5,%d6
	add.l	8(%sp),%d6
	asr.l	#8,%d6
	cmp.l	#255,%d6
	bls.b	2f
	tst.l	%d6
	bmi.b	1f
	move.l	#255,%d6
	bra.b	2f
1:	moveq	#0,%d6
2:
	add.w	16(%sp),%d6
	.if \q6
	add.b	(%a5,%d6.w),%d7
	.else
	movea.l	144(%sp),%a4
	add.b	(%a4,%d6.w),%d7
	.endif
	move.b	%d7,(0,%a3,%d4.l)

	/* lane 1 */
	clr.l	%d6
	move.b	(1,%a0,%d4.l),%d6
	sub.l	#16,%d6
	bge.b	.Lyd_l1ok\@
	moveq	#0,%d6
.Lyd_l1ok\@:
	movea.l	116(%sp),%a4
	move.l	(%a4,%d6.l*4),%d5

	move.l	%d4,%d6
	addq.l	#1,%d6
	and.l	#3,%d6
	clr.l	%d7
	move.b	(12,%sp,%d6.l),%d7
	lsl.w	#8,%d7
	move.w	%d7,16(%sp)

	/* red */
	move.l	%d5,%d6
	add.l	0(%sp),%d6
	asr.l	#8,%d6
	cmp.l	#255,%d6
	bls.b	2f
	tst.l	%d6
	bmi.b	1f
	move.l	#255,%d6
	bra.b	2f
1:	moveq	#0,%d6
2:
	add.w	16(%sp),%d6
	.if \q6
	move.b	(%a5,%d6.w),%d6
	and.l	#255,%d6
	move.b	(%a6,%d6.w),%d7
	.else
	move.b	(%a5,%d6.w),%d7
	.endif
	/* green */
	move.l	%d5,%d6
	add.l	4(%sp),%d6
	asr.l	#8,%d6
	cmp.l	#255,%d6
	bls.b	2f
	tst.l	%d6
	bmi.b	1f
	move.l	#255,%d6
	bra.b	2f
1:	moveq	#0,%d6
2:
	add.w	16(%sp),%d6
	.if \q6
	move.b	(%a5,%d6.w),%d6
	and.l	#255,%d6
	add.b	8(%a6,%d6.w),%d7
	.else
	add.b	(%a6,%d6.w),%d7
	.endif
	/* blue */
	move.l	%d5,%d6
	add.l	8(%sp),%d6
	asr.l	#8,%d6
	cmp.l	#255,%d6
	bls.b	2f
	tst.l	%d6
	bmi.b	1f
	move.l	#255,%d6
	bra.b	2f
1:	moveq	#0,%d6
2:
	add.w	16(%sp),%d6
	.if \q6
	add.b	(%a5,%d6.w),%d7
	.else
	movea.l	144(%sp),%a4
	add.b	(%a4,%d6.w),%d7
	.endif
	move.b	%d7,(1,%a3,%d4.l)

	addq.l	#2,%d4
	bra	.Lyd_pair_test\@

.Lyd_pair_done\@:
	cmp.l	%d2,%d4
	bge	.Lyd_tail_done\@

	move.l	%d4,%d5
	asr.l	#1,%d5

	clr.l	%d6
	move.b	(0,%a1,%d5.l),%d6
	movea.l	132(%sp),%a4
	move.l	(%a4,%d6.l*4),%d7
	add.l	#128,%d7
	move.l	%d7,8(%sp)

	movea.l	124(%sp),%a4
	move.l	(%a4,%d6.l*4),%d7

	clr.l	%d6
	move.b	(0,%a2,%d5.l),%d6
	movea.l	128(%sp),%a4
	add.l	(%a4,%d6.l*4),%d7
	add.l	#128,%d7
	move.l	%d7,4(%sp)

	movea.l	120(%sp),%a4
	move.l	(%a4,%d6.l*4),%d7
	add.l	#128,%d7
	move.l	%d7,0(%sp)

	clr.l	%d6
	move.b	(0,%a0,%d4.l),%d6
	sub.l	#16,%d6
	bge.b	.Lyd_tok\@
	moveq	#0,%d6
.Lyd_tok\@:
	movea.l	116(%sp),%a4
	move.l	(%a4,%d6.l*4),%d5

	move.l	%d4,%d6
	and.l	#3,%d6
	clr.l	%d7
	move.b	(12,%sp,%d6.l),%d7
	lsl.w	#8,%d7
	move.w	%d7,16(%sp)

	/* red */
	move.l	%d5,%d6
	add.l	0(%sp),%d6
	asr.l	#8,%d6
	cmp.l	#255,%d6
	bls.b	2f
	tst.l	%d6
	bmi.b	1f
	move.l	#255,%d6
	bra.b	2f
1:	moveq	#0,%d6
2:
	add.w	16(%sp),%d6
	.if \q6
	move.b	(%a5,%d6.w),%d6
	and.l	#255,%d6
	move.b	(%a6,%d6.w),%d7
	.else
	move.b	(%a5,%d6.w),%d7
	.endif
	/* green */
	move.l	%d5,%d6
	add.l	4(%sp),%d6
	asr.l	#8,%d6
	cmp.l	#255,%d6
	bls.b	2f
	tst.l	%d6
	bmi.b	1f
	move.l	#255,%d6
	bra.b	2f
1:	moveq	#0,%d6
2:
	add.w	16(%sp),%d6
	.if \q6
	move.b	(%a5,%d6.w),%d6
	and.l	#255,%d6
	add.b	8(%a6,%d6.w),%d7
	.else
	add.b	(%a6,%d6.w),%d7
	.endif
	/* blue */
	move.l	%d5,%d6
	add.l	8(%sp),%d6
	asr.l	#8,%d6
	cmp.l	#255,%d6
	bls.b	2f
	tst.l	%d6
	bmi.b	1f
	move.l	#255,%d6
	bra.b	2f
1:	moveq	#0,%d6
2:
	add.w	16(%sp),%d6
	.if \q6
	add.b	(%a5,%d6.w),%d7
	.else
	movea.l	144(%sp),%a4
	add.b	(%a4,%d6.w),%d7
	.endif
	move.b	%d7,(0,%a3,%d4.l)

.Lyd_tail_done\@:
	addq.l	#1,%d0
	addq.l	#1,%d3
	and.l	#3,%d3
	add.l	100(%sp),%d1
	cmpi.l	#2,100(%sp)
	beq	.Lyd_v2_advance\@
	bra	.Lyd_row\@

.Lyd_v2_advance\@:
	move.l	72(%sp),%d6
	add.l	%d6,%d6
	adda.l	%d6,%a0
	adda.l	80(%sp),%a1
	adda.l	88(%sp),%a2
	adda.l	108(%sp),%a3
	bra	.Lyd_v2_row\@
	.endm

.Lyd_weighted_kernel:
	yd_kernel 0
.Lyd_q6_kernel:
	yd_kernel 1

.Lyd_done:
	add.l	#20,%sp
	movem.l	(%sp)+,%d2-%d7/%a2-%a6
	rts

	.even
.Lyd_bayer4:
	.byte	0,8,2,10
	.byte	12,4,14,6
	.byte	3,11,1,9
	.byte	15,7,13,5

	.even
.Lyd_q6_weights:
	/* q * 36 for red, then q * 6 for green. */
	.byte	0,36,72,108,144,180,0,0
	.byte	0,6,12,18,24,30,0,0

#endif /* MR_M68K_ASM */

#if defined(__ELF__)
	.section .note.GNU-stack,"",@progbits
#endif
