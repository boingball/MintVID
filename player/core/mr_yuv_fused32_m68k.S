/*
 * MintVID - YUV420P to five-plane indexed output.
 * Colour conversion uses the existing generic q6 table worker, specialised
 * for aligned 32-pixel blocks and integer vertical sampling. A 32-byte stack
 * tile feeds a local RiVA transpose which stores only planes 0..4.
 * There is no full chunky frame and no per-tile C call.
 *
 * The register transpose is adapted from RiVA RendererAGAC2P.i, preserving
 * the notice below from mr_yuv_dither_planar_direct_m68k.S.
 *
 * MIT License
 *
 * Copyright (c) 2004 Stephen Fellner
 * Copyright (c) 2026 MintVID contributors (adaptation/integration)
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */
/* Five-plane variant of the generic table-driven colour worker.
 * ABI is the same as mr_yuv420_dither8_m68k. The C dispatcher guarantees
 * depth 5, width == out_stride, width divisible by 32, and integer vertical
 * sampling. Resizing in the other axis and odd widths retain the C path.
 *
 * Stack: lane offsets 0..15; row 16; tile count 20; plane-0 destination 24;
 * plane spacing 28; 32-byte indexed tile 32..63; saved registers 64..123.
 * Arguments start at 176 (return + 44 saved bytes + 128 local bytes).
 * The table worker still does ~20 instructions/pixel, with one inline
 * register transpose per 32 pixels, no full chunky frame and no C callback.
 */
#if defined(MR_M68K_ASM)
	.text
	.even
	.globl	mr_yuv420_fused32_m68k

/* Same argument ABI as mr_yuv420_dither8_m68k. The C wrapper sets up
 * the depth-five tables before calling. The contract and frame layout are
 * documented above; all argument offsets include the 128-byte local frame. */
mr_yuv420_fused32_m68k:
	movem.l	%d2-%d7/%a2-%a6,-(%sp)
	sub.l	#128,%sp

	move.l	200(%sp),%d2
	tst.l	%d2
	ble	.Lyd_done
	tst.l	204(%sp)
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
	move.l	216(%sp),%d6
	lsr.l	#3,%d6
	mulu.l	204(%sp),%d6
	move.l	%d6,28(%sp)
	clr.l	16(%sp)
.Lq6_row:
	move.l	16(%sp),%d0
	cmp.l	204(%sp),%d0
	bge	.Lyd_done

	/* src_row = vscale*oy + vscale/2; chroma row = src_row >> 1. */
	move.l	208(%sp),%d1
	muls.l	%d0,%d1
	move.l	208(%sp),%d6
	asr.l	#1,%d6
	add.l	%d6,%d1
	move.l	%d1,%d6
	muls.l	180(%sp),%d6
	movea.l	176(%sp),%a0
	adda.l	%d6,%a0
	asr.l	#1,%d1
	move.l	%d1,%d6
	muls.l	188(%sp),%d6
	movea.l	184(%sp),%a1
	adda.l	%d6,%a1
	move.l	%d1,%d6
	muls.l	196(%sp),%d6
	movea.l	192(%sp),%a2
	adda.l	%d6,%a2
	move.l	%d0,%d6
	muls.l	216(%sp),%d6
	lsr.l	#3,%d6
	movea.l	212(%sp),%a3
	adda.l	%d6,%a3
	move.l	%a3,24(%sp)
	lea	32(%sp),%a3
	clr.l	20(%sp)

	/* This row's four lane offsets, from Bayer row (y_base + oy) & 3. */
	move.l	220(%sp),%d6
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

	move.l	200(%sp),%d1
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
	addq.l	#4,20(%sp)
	cmpi.l	#32,20(%sp)
	bne.b	.Lf32_no_flush
	/* Preserve the colour worker across the register transpose. */
	movem.l	%d0-%d7/%a0-%a6,64(%sp)
	movea.l	24(%sp),%a1
	lea	32(%sp),%a2
	movea.l	28(%sp),%a5
	bsr.w	.Lf32_flush
	movem.l	64(%sp),%d0-%d7/%a0-%a6
	addq.l	#4,24(%sp)
	lea	32(%sp),%a3
	clr.l	20(%sp)
.Lf32_no_flush:
	subq.l	#1,%d1
	bne	.Lq6_quad
.Lq6_quads_done:
.Lq6_row_end:
	addq.l	#1,16(%sp)
	bra	.Lq6_row

.Lyd_done:
	add.l	#128,%sp
	movem.l	(%sp)+,%d2-%d7/%a2-%a6
	rts

.Lf32_flush:
	move.l	(%a2),%d0
	move.l	8(%a2),%d2
	move.w	16(%a2),%d0
	move.w	24(%a2),%d2
	move.l	%d0,%d7
	lsl.l	#8,%d7
	eor.l	%d2,%d7
	and.l	#0xff00ff00,%d7
	eor.l	%d7,%d2
	lsr.l	#8,%d7
	eor.l	%d7,%d0

	move.l	4(%a2),%d1
	move.l	12(%a2),%d3
	move.w	20(%a2),%d1
	move.w	28(%a2),%d3
	move.l	%d1,%d7
	lsl.l	#8,%d7
	eor.l	%d3,%d7
	and.l	#0xff00ff00,%d7
	eor.l	%d7,%d3
	lsr.l	#8,%d7
	eor.l	%d7,%d1

	move.l	%d0,%d7
	lsl.l	#4,%d7
	eor.l	%d1,%d7
	and.l	#0xf0f0f0f0,%d7
	eor.l	%d7,%d1
	lsr.l	#4,%d7
	eor.l	%d7,%d0
	move.l	%d2,%d7
	lsl.l	#4,%d7
	eor.l	%d3,%d7
	and.l	#0xf0f0f0f0,%d7
	eor.l	%d7,%d3
	lsr.l	#4,%d7
	eor.l	%d7,%d2

	move.l	2(%a2),%d4
	move.l	10(%a2),%d6
	move.w	18(%a2),%d4
	move.w	26(%a2),%d6
	move.l	%d4,%d7
	lsl.l	#8,%d7
	eor.l	%d6,%d7
	and.l	#0xff00ff00,%d7
	eor.l	%d7,%d6
	lsr.l	#8,%d7
	eor.l	%d7,%d4

	move.l	%d0,%a3
	move.l	6(%a2),%d5
	move.l	14(%a2),%d0
	move.w	22(%a2),%d5
	move.w	30(%a2),%d0
	move.l	%d5,%d7
	lsl.l	#8,%d7
	eor.l	%d0,%d7
	and.l	#0xff00ff00,%d7
	eor.l	%d7,%d0
	lsr.l	#8,%d7
	eor.l	%d7,%d5

	move.l	%d4,%d7
	lsl.l	#4,%d7
	eor.l	%d5,%d7
	and.l	#0xf0f0f0f0,%d7
	eor.l	%d7,%d5
	lsr.l	#4,%d7
	eor.l	%d7,%d4
	move.l	%d6,%d7
	lsl.l	#4,%d7
	eor.l	%d0,%d7
	and.l	#0xf0f0f0f0,%d7
	eor.l	%d7,%d0
	lsr.l	#4,%d7
	eor.l	%d7,%d6

	move.l	%d2,%d7
	lsl.l	#2,%d7
	eor.l	%d6,%d7
	and.l	#0xcccccccc,%d7
	eor.l	%d7,%d6
	lsr.l	#2,%d7
	eor.l	%d7,%d2
	move.l	%d1,%d7
	lsl.l	#2,%d7
	eor.l	%d5,%d7
	and.l	#0xcccccccc,%d7
	eor.l	%d7,%d5
	lsr.l	#2,%d7
	eor.l	%d7,%d1
	move.l	%d3,%d7
	lsl.l	#2,%d7
	eor.l	%d0,%d7
	and.l	#0xcccccccc,%d7
	eor.l	%d7,%d0
	lsr.l	#2,%d7
	eor.l	%d7,%d3
	exg	%d2,%a3
	move.l	%d2,%d7
	lsl.l	#2,%d7
	eor.l	%d4,%d7
	and.l	#0xcccccccc,%d7
	eor.l	%d7,%d4
	lsr.l	#2,%d7
	eor.l	%d7,%d2

	movea.l	%a1,%a4
	move.l	%d5,%d7
	lsl.l	#1,%d7
	eor.l	%d0,%d7
	and.l	#0xaaaaaaaa,%d7
	eor.l	%d7,%d0
	move.l	%d0,(%a4)
	adda.l	%a5,%a4
	lsr.l	#1,%d7
	eor.l	%d7,%d5
	move.l	%d5,(%a4)
	adda.l	%a5,%a4

	move.l	%d1,%d7
	lsl.l	#1,%d7
	eor.l	%d3,%d7
	and.l	#0xaaaaaaaa,%d7
	eor.l	%d7,%d3
	move.l	%d3,(%a4)
	adda.l	%a5,%a4
	lsr.l	#1,%d7
	eor.l	%d7,%d1
	move.l	%d1,(%a4)
	adda.l	%a5,%a4

	move.l	%d4,%d7
	lsl.l	#1,%d7
	eor.l	%d6,%d7
	and.l	#0xaaaaaaaa,%d7
	eor.l	%d7,%d6
	move.l	%d6,(%a4)
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
