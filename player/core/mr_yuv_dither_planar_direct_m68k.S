/*
 * MintVID - experimental direct YUV420P -> AGA planar kernel for m68k.
 *
 * This path is used only by the hardware-test fused build.  It preserves the
 * established depth-8 6x6x6/Bayer palette exactly, but removes the separate
 * chunky tile and C2P pass:
 *
 *   YUV -> q6 palette indices -> 32-pixel staging block -> RiVA register C2P
 *       -> final plane-major queue
 *
 * The staging block is only 32 bytes and lives in the routine's stack frame.
 * It exists solely because the actual RiVA GrayC2P network consumes 32 chunky
 * bytes at once; there is no full scanline/frame INDEX8 buffer and no second
 * C/asm traversal.  The q6 path is also compile-time specialised here, so the
 * three per-pixel "am I depth 8?" tests in the general dither kernel disappear.
 *
 * The 32-pixel transpose below is adapted from RiVA's RendererAGAC2P.i.
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
#if defined(MR_M68K_ASM)
	.text
	.even
	.globl	mr_yuv420_dither8_planar_direct_m68k
	.type	mr_yuv420_dither8_planar_direct_m68k,@function
	.extern	mr_yuv420_dither8_m68k_base

/* Scratch frame (96 bytes): */
	.set	S_RED,       0
	.set	S_GREEN,     4
	.set	S_BLUE,      8
	.set	S_BAYER,    12
	.set	S_TAP,      16
	.set	S_BPR,      20
	.set	S_PLANE,    24
	.set	S_LEFT,     28
	.set	S_ROWOFF,   32
	.set	S_SAVE_ROW, 36
	.set	S_SAVE_X,   40
	.set	S_SAVE_PAL, 44
	.set	S_SAVE_A0,  48
	.set	S_SAVE_A1,  52
	.set	S_SAVE_A2,  56
	.set	S_GROUP,    60
	.set	S_SIZE,     96

/* After saving d2-d7/a2-a6 (44), allocating 96 scratch bytes, and the return
 * address, the original C arguments begin at +144. */
	.set	A_Y,       144
	.set	A_YSTR,    148
	.set	A_U,       152
	.set	A_USTR,    156
	.set	A_V,       160
	.set	A_VSTR,    164
	.set	A_WIDTH,   168
	.set	A_DSTH,    172
	.set	A_VSCALE,  176
	.set	A_OUT,     180
	.set	A_OUTSTR,  184
	.set	A_YBASE,   188
	.set	A_LUMA,    192
	.set	A_E409,    196
	.set	A_DM100,   200
	.set	A_EM208,   204
	.set	A_D516,    208
	.set	A_LUTR,    212
	.set	A_LUTG,    216
	.set	A_LUTB,    220

/* Clear the current 32-byte destination-aligned staging block. */
	.macro	DP_CLEAR_GROUP
	lea	S_GROUP(%sp),%a3
	clr.l	(%a3)+
	clr.l	(%a3)+
	clr.l	(%a3)+
	clr.l	(%a3)+
	clr.l	(%a3)+
	clr.l	(%a3)+
	clr.l	(%a3)+
	clr.l	(%a3)+
	.endm

/* Reload the state needed by the dither pair loop after .Ldp_flush32, which is
 * intentionally free to clobber every data/address register. */
	.macro	DP_RELOAD_AFTER_FLUSH
	move.l	S_SAVE_ROW(%sp),%d0
	move.l	S_SAVE_X(%sp),%d4
	move.l	S_SAVE_PAL(%sp),%d7
	movea.l	S_SAVE_A0(%sp),%a0
	movea.l	S_SAVE_A1(%sp),%a1
	movea.l	S_SAVE_A2(%sp),%a2
	move.l	A_WIDTH(%sp),%d2
	movea.l	A_LUTB(%sp),%a5
	lea	.Ldp_q6_weights,%a6
	.endm

/* Store one completed palette index (d7) into the current 32-pixel block.
 * If this pixel starts a new destination-aligned 32-pixel block, flush the
 * previous one first.  XADD is 0 for lane 0 and 1 for lane 1. */
	.macro	DP_STORE XADD
	move.l	%d4,%d6
	.if \XADD
	addq.l	#1,%d6
	.endif
	move.l	%d6,%d5			/* d5 = source x */
	add.l	S_LEFT(%sp),%d6		/* d6 = padded destination x */
	move.l	%d6,%d3
	and.l	#31,%d3
	bne.w	.Ldp_store_now\@
	tst.l	%d5
	beq.w	.Ldp_store_now\@

	/* Current pixel is the first pixel of a new 32-pixel output block.  Save
	 * the dither state and flush the completed previous block. */
	move.l	%d0,S_SAVE_ROW(%sp)
	move.l	%d4,S_SAVE_X(%sp)
	move.l	%d7,S_SAVE_PAL(%sp)
	move.l	%a0,S_SAVE_A0(%sp)
	move.l	%a1,S_SAVE_A1(%sp)
	move.l	%a2,S_SAVE_A2(%sp)

	lsr.l	#5,%d6			/* current block index */
	subq.l	#1,%d6			/* completed previous block */
	lsl.l	#2,%d6			/* four bytes per plane per block */
	add.l	S_ROWOFF(%sp),%d6
	movea.l	A_OUT(%sp),%a1
	adda.l	%d6,%a1			/* plane-0 destination */
	lea	S_GROUP(%sp),%a2		/* 32 chunky bytes */
	movea.l	S_PLANE(%sp),%a5		/* plane spacing */
	bsr.w	.Ldp_flush32

	DP_CLEAR_GROUP
	DP_RELOAD_AFTER_FLUSH

	move.l	%d4,%d6
	.if \XADD
	addq.l	#1,%d6
	.endif
	add.l	S_LEFT(%sp),%d6
	and.l	#31,%d6
	move.b	%d7,S_GROUP(%sp,%d6.l)
	bra.w	.Ldp_store_done\@

.Ldp_store_now\@:
	move.b	%d7,S_GROUP(%sp,%d3.l)
.Ldp_store_done\@:
	.endm

/* One q6 pixel.  The arithmetic is intentionally the same as the established
 * dither kernel, minus the runtime q6/non-q6 branches. */
	.macro	DP_PIXEL YOFF, XADD
	clr.l	%d6
	move.b	(\YOFF,%a0,%d4.l),%d6
	sub.l	#16,%d6
	bge.b	.Ldp_y_ok\@
	moveq	#0,%d6
.Ldp_y_ok\@:
	movea.l	A_LUMA(%sp),%a4
	move.l	(%a4,%d6.l*4),%d5

	move.l	%d4,%d6
	.if \XADD
	addq.l	#1,%d6
	.endif
	and.l	#3,%d6
	clr.l	%d7
	move.b	(S_BAYER,%sp,%d6.l),%d7
	lsl.w	#8,%d7
	move.w	%d7,S_TAP(%sp)

	/* red */
	move.l	%d5,%d6
	add.l	S_RED(%sp),%d6
	asr.l	#8,%d6
	cmp.l	#255,%d6
	bls.b	.Ldp_r_ok\@
	tst.l	%d6
	bmi.b	.Ldp_r_zero\@
	move.l	#255,%d6
	bra.b	.Ldp_r_ok\@
.Ldp_r_zero\@:
	moveq	#0,%d6
.Ldp_r_ok\@:
	add.w	S_TAP(%sp),%d6
	move.b	(%a5,%d6.w),%d6
	and.l	#255,%d6
	move.b	(%a6,%d6.w),%d7

	/* green */
	move.l	%d5,%d6
	add.l	S_GREEN(%sp),%d6
	asr.l	#8,%d6
	cmp.l	#255,%d6
	bls.b	.Ldp_g_ok\@
	tst.l	%d6
	bmi.b	.Ldp_g_zero\@
	move.l	#255,%d6
	bra.b	.Ldp_g_ok\@
.Ldp_g_zero\@:
	moveq	#0,%d6
.Ldp_g_ok\@:
	add.w	S_TAP(%sp),%d6
	move.b	(%a5,%d6.w),%d6
	and.l	#255,%d6
	add.b	8(%a6,%d6.w),%d7

	/* blue */
	move.l	%d5,%d6
	add.l	S_BLUE(%sp),%d6
	asr.l	#8,%d6
	cmp.l	#255,%d6
	bls.b	.Ldp_b_ok\@
	tst.l	%d6
	bmi.b	.Ldp_b_zero\@
	move.l	#255,%d6
	bra.b	.Ldp_b_ok\@
.Ldp_b_zero\@:
	moveq	#0,%d6
.Ldp_b_ok\@:
	add.w	S_TAP(%sp),%d6
	add.b	(%a5,%d6.w),%d7

	DP_STORE \XADD
	.endm

/*
 * Same ABI as mr_yuv420_dither8_m68k().  This direct worker is selected only
 * while mr_yuv_planar_queue_active is true; validation below falls back to the
 * established chunky worker if that contract is violated.
 */
mr_yuv420_dither8_planar_direct_m68k:
	movem.l	%d2-%d7/%a2-%a6,-(%sp)
	sub.l	#S_SIZE,%sp

	move.l	A_WIDTH(%sp),%d2
	tst.l	%d2
	ble.w	.Ldp_done
	move.l	A_DSTH(%sp),%d6
	tst.l	%d6
	ble.w	.Ldp_done
	cmpi.l	#1,A_VSCALE(%sp)
	bne.w	.Ldp_fallback
	move.l	A_OUTSTR(%sp),%d6
	cmp.l	%d2,%d6
	blt.w	.Ldp_fallback
	move.l	%d6,%d5
	and.l	#31,%d5
	bne.w	.Ldp_fallback

	/* Confirm the depth-8 q6 LUT shape. */
	movea.l	A_LUTB(%sp),%a4
	tst.l	%a4
	beq.w	.Ldp_fallback
	clr.l	%d5
	move.b	2176(%a4),%d5
	cmpi.b	#3,%d5
	bne.w	.Ldp_fallback

	/* bpr, plane size, and source centring inside the padded row. */
	lsr.l	#3,%d6
	move.l	%d6,S_BPR(%sp)
	mulu.l	A_DSTH(%sp),%d6
	move.l	%d6,S_PLANE(%sp)

	move.l	A_OUTSTR(%sp),%d6
	sub.l	%d2,%d6
	asr.l	#1,%d6
	move.l	%d6,S_LEFT(%sp)

	moveq	#0,%d0			/* output/source row */

.Ldp_row:
	cmp.l	A_DSTH(%sp),%d0
	bge.w	.Ldp_done
	move.l	A_WIDTH(%sp),%d2
	movea.l	A_LUTB(%sp),%a5
	lea	.Ldp_q6_weights,%a6

	/* Y row. */
	move.l	%d0,%d6
	muls.l	A_YSTR(%sp),%d6
	movea.l	A_Y(%sp),%a0
	adda.l	%d6,%a0

	/* Shared 4:2:0 U/V row. */
	move.l	%d0,%d5
	asr.l	#1,%d5
	move.l	%d5,%d6
	muls.l	A_USTR(%sp),%d6
	movea.l	A_U(%sp),%a1
	adda.l	%d6,%a1
	move.l	%d5,%d6
	muls.l	A_VSTR(%sp),%d6
	movea.l	A_V(%sp),%a2
	adda.l	%d6,%a2

	/* Source-relative 4x4 Bayer row. */
	move.l	A_YBASE(%sp),%d3
	add.l	%d0,%d3
	and.l	#3,%d3
	lea	.Ldp_bayer4,%a4
	lsl.l	#2,%d3
	adda.l	%d3,%a4
	move.l	(%a4),%d3
	move.l	%d3,S_BAYER(%sp)

	move.l	%d0,%d6
	mulu.l	S_BPR(%sp),%d6
	move.l	%d6,S_ROWOFF(%sp)
	DP_CLEAR_GROUP
	clr.l	%d4			/* source x */

.Ldp_pair_test:
	move.l	%d2,%d6
	subq.l	#1,%d6
	cmp.l	%d6,%d4
	bge.w	.Ldp_pair_done

	/* Shared chroma additions for this two-pixel pair. */
	move.l	%d4,%d5
	asr.l	#1,%d5

	clr.l	%d6
	move.b	(0,%a1,%d5.l),%d6
	movea.l	A_D516(%sp),%a4
	move.l	(%a4,%d6.l*4),%d7
	add.l	#128,%d7
	move.l	%d7,S_BLUE(%sp)

	movea.l	A_DM100(%sp),%a4
	move.l	(%a4,%d6.l*4),%d7

	clr.l	%d6
	move.b	(0,%a2,%d5.l),%d6
	movea.l	A_EM208(%sp),%a4
	add.l	(%a4,%d6.l*4),%d7
	add.l	#128,%d7
	move.l	%d7,S_GREEN(%sp)

	movea.l	A_E409(%sp),%a4
	move.l	(%a4,%d6.l*4),%d7
	add.l	#128,%d7
	move.l	%d7,S_RED(%sp)

	DP_PIXEL 0,0
	DP_PIXEL 1,1
	addq.l	#2,%d4
	bra.w	.Ldp_pair_test

.Ldp_pair_done:
	cmp.l	%d2,%d4
	bge.w	.Ldp_row_flush

	/* Odd-width final source pixel has its own (shared) chroma sample. */
	move.l	%d4,%d5
	asr.l	#1,%d5
	clr.l	%d6
	move.b	(0,%a1,%d5.l),%d6
	movea.l	A_D516(%sp),%a4
	move.l	(%a4,%d6.l*4),%d7
	add.l	#128,%d7
	move.l	%d7,S_BLUE(%sp)
	movea.l	A_DM100(%sp),%a4
	move.l	(%a4,%d6.l*4),%d7
	clr.l	%d6
	move.b	(0,%a2,%d5.l),%d6
	movea.l	A_EM208(%sp),%a4
	add.l	(%a4,%d6.l*4),%d7
	add.l	#128,%d7
	move.l	%d7,S_GREEN(%sp)
	movea.l	A_E409(%sp),%a4
	move.l	(%a4,%d6.l*4),%d7
	add.l	#128,%d7
	move.l	%d7,S_RED(%sp)
	DP_PIXEL 0,0

.Ldp_row_flush:
	/* Flush the final (possibly partial) destination block.  All untouched
	 * positions in S_GROUP are black from DP_CLEAR_GROUP. */
	move.l	%d0,S_SAVE_ROW(%sp)
	move.l	S_LEFT(%sp),%d6
	add.l	A_WIDTH(%sp),%d6
	subq.l	#1,%d6
	lsr.l	#5,%d6
	lsl.l	#2,%d6
	add.l	S_ROWOFF(%sp),%d6
	movea.l	A_OUT(%sp),%a1
	adda.l	%d6,%a1
	lea	S_GROUP(%sp),%a2
	movea.l	S_PLANE(%sp),%a5
	bsr.w	.Ldp_flush32
	move.l	S_SAVE_ROW(%sp),%d0
	addq.l	#1,%d0
	bra.w	.Ldp_row

.Ldp_fallback:
	add.l	#S_SIZE,%sp
	movem.l	(%sp)+,%d2-%d7/%a2-%a6
	jmp	mr_yuv420_dither8_m68k_base

.Ldp_done:
	add.l	#S_SIZE,%sp
	movem.l	(%sp)+,%d2-%d7/%a2-%a6
	rts

/*
 * Local no-save/no-restore adaptation of RiVA GrayC2P for exactly one
 * 32-pixel block.
 *
 * Inputs:
 *   a2 = 32-byte chunky block
 *   a1 = plane-0 destination longword
 *   a5 = bytes between corresponding longwords of consecutive planes
 * Clobbers d0-d7/a0-a6.  The caller deliberately saves/reloads its live
 * dither state around intermediate calls, or is finished with the row.
 */
.Ldp_flush32:
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
	adda.l	%a5,%a4
	lsr.l	#1,%d7
	eor.l	%d7,%d4
	move.l	%d4,(%a4)
	adda.l	%a5,%a4

	move.l	%a3,%d4
	move.l	%d2,%d7
	lsl.l	#1,%d7
	eor.l	%d4,%d7
	and.l	#0xaaaaaaaa,%d7
	eor.l	%d7,%d4
	move.l	%d4,(%a4)
	adda.l	%a5,%a4
	lsr.l	#1,%d7
	eor.l	%d7,%d2
	move.l	%d2,(%a4)
	rts

	.even
.Ldp_bayer4:
	.byte	0,8,2,10
	.byte	12,4,14,6
	.byte	3,11,1,9
	.byte	15,7,13,5

	.even
.Ldp_q6_weights:
	.byte	0,36,72,108,144,180,0,0
	.byte	0,6,12,18,24,30,0,0

	.size	mr_yuv420_dither8_planar_direct_m68k,.-mr_yuv420_dither8_planar_direct_m68k
#endif /* MR_M68K_ASM */

#if defined(__ELF__)
	.section .note.GNU-stack,"",@progbits
#endif
