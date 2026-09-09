/*
 * MintVID - native RiVA 32-pixel chunky-to-planar kernel for m68k.
 *
 * This is an adaptation of the GrayC2P inner loop from RiVA's
 * RendererAGAC2P.i.  Unlike MintVID's earlier "RiVA-style" transpose8
 * implementation, this is the register-scheduled 32-pixel network RiVA
 * actually used.  The original self-modifying plane displacements and screen
 * globals are replaced with a simple contiguous plane-major destination, which
 * is exactly the representation used by the experimental YUV planar queue.
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
	.globl	mr_c2p8_riva_native_m68k
	.type	mr_c2p8_riva_native_m68k,@function

/*
 * void mr_c2p8_riva_native_m68k(const uint8_t *chunky, int pw, int h,
 *                                int chunky_stride, uint8_t *planar,
 *                                int plane_size, int bpr, int y0);
 *
 * `pw` must be a positive multiple of 32. `planar` contains eight planes
 * consecutively, each `plane_size` bytes.  The first output row is y0.
 *
 * Saves d2-d7/a2-a6 (44 bytes) and uses 16 bytes local scratch, therefore
 * arguments start at +64:
 *   64 chunky          80 planar
 *   68 pw              84 plane_size
 *   72 h               88 bpr
 *   76 chunky_stride   92 y0
 *
 * Scratch:
 *    0 groups_per_row
 *    4 groups_left
 *    8 rows_left
 *   12 source_row_skip (chunky_stride - pw)
 */
mr_c2p8_riva_native_m68k:
	movem.l	%d2-%d7/%a2-%a6,-(%sp)
	sub.l	#16,%sp

	move.l	68(%sp),%d0
	tst.l	%d0
	ble.w	.Lrn_done
	move.l	%d0,%d1
	and.l	#31,%d1
	bne.w	.Lrn_done
	asr.l	#5,%d0
	move.l	%d0,0(%sp)

	move.l	72(%sp),%d0
	tst.l	%d0
	ble.w	.Lrn_done
	move.l	%d0,8(%sp)

	move.l	76(%sp),%d0
	sub.l	68(%sp),%d0
	move.l	%d0,12(%sp)

	movea.l	64(%sp),%a2		/* current chunky source */
	movea.l	80(%sp),%a1		/* plane-0 destination */
	movea.l	84(%sp),%a5		/* constant plane spacing */

	/* destination starts at row y0 in plane 0 */
	move.l	92(%sp),%d0
	mulu.l	88(%sp),%d0
	adda.l	%d0,%a1

.Lrn_row:
	move.l	0(%sp),%d0
	move.l	%d0,4(%sp)

.Lrn_group:
	/* RiVA GrayC2P: transpose 32 chunky bytes into eight planar longwords.
	 * The load pattern is intentionally non-linear: it schedules the three
	 * exchange stages so the 68k keeps the whole transpose in registers. */
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

	/* Final 1-bit exchange.  Store each completed plane as soon as it is
	 * available; a4 walks the constant plane_size spacing. */
	movea.l	%a1,%a4

	move.l	%d5,%d7
	lsl.l	#1,%d7
	eor.l	%d0,%d7
	and.l	#0xaaaaaaaa,%d7
	eor.l	%d7,%d0
	move.l	%d0,(%a4)		/* plane 0 */
	adda.l	%a5,%a4
	lsr.l	#1,%d7
	eor.l	%d7,%d5
	move.l	%d5,(%a4)		/* plane 1 */
	adda.l	%a5,%a4

	move.l	%d1,%d7
	lsl.l	#1,%d7
	eor.l	%d3,%d7
	and.l	#0xaaaaaaaa,%d7
	eor.l	%d7,%d3
	move.l	%d3,(%a4)		/* plane 2 */
	adda.l	%a5,%a4
	lsr.l	#1,%d7
	eor.l	%d7,%d1
	move.l	%d1,(%a4)		/* plane 3 */
	adda.l	%a5,%a4

	move.l	%d4,%d7
	lsl.l	#1,%d7
	eor.l	%d6,%d7
	and.l	#0xaaaaaaaa,%d7
	eor.l	%d7,%d6
	move.l	%d6,(%a4)		/* plane 4 */
	adda.l	%a5,%a4
	lsr.l	#1,%d7
	eor.l	%d7,%d4
	move.l	%d4,(%a4)		/* plane 5 */
	adda.l	%a5,%a4

	move.l	%a3,%d4
	move.l	%d2,%d7
	lsl.l	#1,%d7
	eor.l	%d4,%d7
	and.l	#0xaaaaaaaa,%d7
	eor.l	%d7,%d4
	move.l	%d4,(%a4)		/* plane 6 */
	adda.l	%a5,%a4
	lsr.l	#1,%d7
	eor.l	%d7,%d2
	move.l	%d2,(%a4)		/* plane 7 */

	lea	32(%a2),%a2
	lea	4(%a1),%a1
	subq.l	#1,4(%sp)
	bne.w	.Lrn_group

	/* a2 consumed exactly pw bytes and a1 consumed exactly bpr bytes. */
	move.l	12(%sp),%d0
	adda.l	%d0,%a2
	subq.l	#1,8(%sp)
	bne.w	.Lrn_row

.Lrn_done:
	add.l	#16,%sp
	movem.l	(%sp)+,%d2-%d7/%a2-%a6
	rts

	.size	mr_c2p8_riva_native_m68k,.-mr_c2p8_riva_native_m68k
#endif /* MR_M68K_ASM */

#if defined(__ELF__)
	.section .note.GNU-stack,"",@progbits
#endif
