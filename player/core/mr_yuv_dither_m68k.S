/*
 * MintVID - hand-tuned m68k fused YUV420P -> indexed dither.
 *
 * One kernel for every palette (8-bit 6x6x6 AGA, 5-bit 4x4x2 and 4-bit
 * 2x4x2 ECS/OCS), ~20 instructions per pixel. It reads its own tables,
 * built by mr_yuv_dither.c from the current depth's LUTs, not the LUT
 * arguments (kept so the ABI and the direct-planar dispatcher are
 * unchanged); see the comment above .Lyd_q6_kernel. Bit-identical to the C
 * path (tests/mr_yuv_dither_check.c).
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
 * Scratch: the four lane offsets (0..15) and the output row (16).
 */
mr_yuv420_dither8_m68k:
	movem.l	%d2-%d7/%a2-%a6,-(%sp)
	sub.l	#20,%sp

	move.l	92(%sp),%d2
	tst.l	%d2
	ble	.Lyd_done
	tst.l	96(%sp)
	ble	.Lyd_done

/*
 * The kernel: ~20 instructions per pixel (the per-channel weighted-LUT
 * kernel it replaced took ~56).
 *
 * mr_yuv_dither.c builds two tables for it (build_q6_tables()):
 *
 *   mr_yuv_dither_q6_block: the YUV addends, indexed like the RGB24 kernel
 *     in mr_yuv_m68k.S. d6 holds a slot base and move.b drops the sample
 *     into its low byte:
 *       luma  (0x200|y)*4: 298*max(y-16,0) + 128 (the rounding term)
 *       V     (0x200|v)*8: { 409*(v-128), -208*(v-128) }
 *       U     (0x300|u)*8: { 516*(u-128), -100*(u-128) }
 *     The V pair is loaded last, so d6 is left as 0x2yy for the luma loads.
 *   mr_yuv_dither_q6_tab: for every Bayer threshold t and every channel sum
 *     s = (luma + addend) >> 8 in -258..534, one longword
 *     { lut_r[t][c], lut_g[t][c], lut_b[t][c], 0 }, c being s clipped to
 *     0..255: each channel's dither and quantise for the current palette
 *     ({ 36q, 6q, q } for 6x6x6). Entry (t, s) sits at index
 *     t*793 + s + 258.
 *
 * A pixel's threshold is fixed per row and x&3, so each of the four lanes
 * of a 4-pixel group adds (t*793) << 8 to its luma value before the shift:
 * floor((x + k*256)/256) = floor(x/256) + k, so the shifted sum is already
 * the table index. Clipping, dithering and quantising are then one byte
 * lookup per channel, and the three lookups add up to the palette index.
 * Output is bit-identical to the C path (tests/mr_yuv_dither_check.c).
 *
 * Locals: 0..15 the four lane offsets, 16 the output row.
 */
	.macro	q6_pair
	move.l	#0x300,%d6
	move.b	(%a1)+,%d6
	move.l	(%a5,%d6.l*8),%d7	/* blue addend */
	move.l	(4,%a5,%d6.l*8),%d2	/* green, U part */
	move.l	#0x200,%d6
	move.b	(%a2)+,%d6
	move.l	(%a5,%d6.l*8),%d0	/* red addend */
	add.l	(4,%a5,%d6.l*8),%d2	/* green, V part */
	.endm

	.macro	q6_pixel k
	move.b	\k(%a0),%d6
	move.l	(%a5,%d6.l*4),%d5
	add.l	4*\k(%sp),%d5
	move.l	%d5,%d4
	add.l	%d0,%d4
	asr.l	#8,%d4
	move.b	(0,%a4,%d4.l*4),%d3
	move.l	%d5,%d4
	add.l	%d2,%d4
	asr.l	#8,%d4
	add.b	(1,%a4,%d4.l*4),%d3
	add.l	%d7,%d5
	asr.l	#8,%d5
	add.b	(2,%a4,%d5.l*4),%d3
	move.b	%d3,\k(%a3)
	.endm

.Lyd_q6_kernel:
	clr.l	16(%sp)
.Lq6_row:
	move.l	16(%sp),%d0
	cmp.l	96(%sp),%d0
	bge	.Lyd_done

	/* src_row = vscale*oy + vscale/2; chroma row = src_row >> 1. */
	move.l	100(%sp),%d1
	muls.l	%d0,%d1
	move.l	100(%sp),%d6
	asr.l	#1,%d6
	add.l	%d6,%d1
	move.l	%d1,%d6
	muls.l	72(%sp),%d6
	movea.l	68(%sp),%a0
	adda.l	%d6,%a0
	asr.l	#1,%d1
	move.l	%d1,%d6
	muls.l	80(%sp),%d6
	movea.l	76(%sp),%a1
	adda.l	%d6,%a1
	move.l	%d1,%d6
	muls.l	88(%sp),%d6
	movea.l	84(%sp),%a2
	adda.l	%d6,%a2
	move.l	%d0,%d6
	muls.l	108(%sp),%d6
	movea.l	104(%sp),%a3
	adda.l	%d6,%a3

	/* This row's four lane offsets, from Bayer row (y_base + oy) & 3. */
	move.l	112(%sp),%d6
	add.l	%d0,%d6
	and.l	#3,%d6
	lea	.Lyd_bayer4,%a4
	lea	(%a4,%d6.l*4),%a4
	moveq	#3,%d1
.Lq6_lane:
	moveq	#0,%d4
	move.b	(%a4,%d1.l),%d4
	mulu.w	#793,%d4
	lsl.l	#8,%d4
	move.l	%d4,(%sp,%d1.l*4)
	subq.l	#1,%d1
	bpl.b	.Lq6_lane

	lea	mr_yuv_dither_q6_tab+1032,%a4	/* entry (0, s=0) */
	lea	mr_yuv_dither_q6_block-2048,%a5	/* virtual index 0x200 */
	move.l	#0x200,%d6

	move.l	92(%sp),%d1
	lsr.l	#2,%d1
	beq.w	.Lq6_quads_done
.Lq6_quad:
	q6_pair
	q6_pixel 0
	q6_pixel 1
	q6_pair
	q6_pixel 2
	q6_pixel 3
	addq.l	#4,%a0
	addq.l	#4,%a3
	subq.l	#1,%d1
	bne	.Lq6_quad
.Lq6_quads_done:
	/* Up to three trailing pixels, at the same lane offsets. */
	move.l	92(%sp),%d1
	and.l	#3,%d1
	beq.w	.Lq6_row_end
	q6_pair
	q6_pixel 0
	subq.l	#1,%d1
	beq.w	.Lq6_row_end
	q6_pixel 1
	subq.l	#1,%d1
	beq.w	.Lq6_row_end
	q6_pair
	q6_pixel 2
.Lq6_row_end:
	addq.l	#1,16(%sp)
	bra	.Lq6_row

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

#endif /* MR_M68K_ASM */

#if defined(__ELF__)
	.section .note.GNU-stack,"",@progbits
#endif
