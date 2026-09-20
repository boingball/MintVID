/*
 * Hand-written m68k assembly for H.264 chroma motion-compensation
 * interpolation (spec 8.4.2.2.2, "Chroma sample interpolation process").
 *
 * void mr_ih264_inter_pred_chroma_m68k(UWORD8 *pu1_src, UWORD8 *pu1_dst,
 *     WORD32 src_strd, WORD32 dst_strd, WORD32 dx, WORD32 dy, WORD32 ht,
 *     WORD32 wd);
 *
 * U and V are interleaved (UVUV...); wd counts chroma sample pairs, so
 * there are 2*wd bytes per row and +2 reaches the next same-plane sample.
 * The generic fractional filter computes the exact four-tap weighted sum:
 *   ((8-dx)*(8-dy)*s00 + dx*(8-dy)*s10 +
 *     (8-dx)*dy*s01 + dx*dy*s11 + 32) >> 6.
 * Its four invariant coefficients are calculated once per block.
 *
 * A fractional axis of 4/8 is a particularly useful exact shortcut in
 * motion-heavy video: when dy==4, the expression reduces to
 *   ((8-dx)*(s00+s01) + dx*(s10+s11) + 8) >> 4;
 * when dx==4 it reduces to
 *   ((8-dy)*(s00+s10) + dy*(s01+s11) + 8) >> 4.
 * These cases need TWO per-pixel 16-bit multiplies instead of four. Both
 * axes equal to four take the dy==4 path. The arithmetic is bit-exact:
 * factoring out 4 changes the rounding offset from 32 to 8 and the shift
 * from 6 to 4; all sums remain within an unsigned 16-bit multiplication.
 * dx==0 and/or dy==0 are already handled by ih264_mc_degrade.c's exact
 * chroma fast paths and do not enter this function.
 *
 * A row is addressed using d16(An,Dn.l) and dbf backwards, with a0/a4
 * pointing at adjacent source rows and a1 at the destination. The C ABI
 * passes eight 32-bit args from 4(sp). Saves d2-d7/a2-a4 (36 bytes), so
 * stack argument offsets below include 36; d0/d1/a0/a1 are scratch.
 */
#if defined(MR_M68K_ASM)
	.text
	.even
	.globl	mr_ih264_inter_pred_chroma_m68k

mr_ih264_inter_pred_chroma_m68k:
	movem.l	%d2-%d7/%a2-%a4,-(%sp)
	movea.l	40(%sp),%a0		/* source */
	movea.l	44(%sp),%a1		/* destination */
	movea.l	48(%sp),%a2		/* source stride */
	movea.l	52(%sp),%a3		/* destination stride */
	movea.l	%a0,%a4
	adda.l	%a2,%a4			/* next source row */

	move.l	56(%sp),%d1		/* dx */
	move.l	60(%sp),%d2		/* dy */
	cmp.l	#4,%d2
	beq	.Lchroma_dy4_setup
	cmp.l	#4,%d1
	beq	.Lchroma_dx4_setup

	/* Generic 4-tap filter: hoist all four weights out of the pixel loop. */
	moveq	#8,%d5
	sub.l	%d1,%d5			/* 8-dx */
	moveq	#8,%d4
	sub.l	%d2,%d4			/* 8-dy */
	move.l	%d5,%d3
	mulu.w	%d2,%d3			/* w01 */
	mulu.w	%d4,%d5			/* w00 */
	move.l	%d1,%d6
	mulu.w	%d4,%d6			/* w10 */
	mulu.w	%d2,%d1			/* w11 */

	move.l	64(%sp),%d0		/* ht */
	subq.l	#1,%d0
.Lchroma_row:
	move.l	68(%sp),%d2		/* 2*wd - 1 */
	add.l	%d2,%d2
	subq.l	#1,%d2
.Lchroma_col:
	clr.l	%d4
	clr.l	%d7
	move.b	(0,%a0,%d2.l),%d7
	mulu.w	%d5,%d7
	add.l	%d7,%d4

	clr.l	%d7
	move.b	(2,%a0,%d2.l),%d7
	mulu.w	%d6,%d7
	add.l	%d7,%d4

	clr.l	%d7
	move.b	(0,%a4,%d2.l),%d7
	mulu.w	%d3,%d7
	add.l	%d7,%d4

	clr.l	%d7
	move.b	(2,%a4,%d2.l),%d7
	mulu.w	%d1,%d7
	add.l	%d7,%d4

	add.l	#32,%d4
	lsr.l	#6,%d4
	move.b	%d4,(0,%a1,%d2.l)
	dbf	%d2,.Lchroma_col
	adda.l	%a2,%a0
	adda.l	%a2,%a4
	adda.l	%a3,%a1
	dbf	%d0,.Lchroma_row
	bra	.Lchroma_done

	/* dy=4: pair samples vertically before the weighted sum. */
.Lchroma_dy4_setup:
	moveq	#8,%d5
	sub.l	%d1,%d5			/* inv = 8-dx */
	move.l	%d1,%d6			/* dx */
	move.l	64(%sp),%d0
	subq.l	#1,%d0
.Lchroma_dy4_row:
	move.l	68(%sp),%d2
	add.l	%d2,%d2
	subq.l	#1,%d2
.Lchroma_dy4_col:
	clr.l	%d7
	move.b	(0,%a0,%d2.l),%d7	/* s00 */
	clr.l	%d4
	move.b	(0,%a4,%d2.l),%d4	/* s01 */
	add.w	%d4,%d7
	mulu.w	%d5,%d7			/* (8-dx)*(s00+s01) */
	clr.l	%d4
	move.b	(2,%a0,%d2.l),%d4	/* s10 */
	clr.l	%d3
	move.b	(2,%a4,%d2.l),%d3	/* s11 */
	add.w	%d3,%d4
	mulu.w	%d6,%d4			/* dx*(s10+s11) */
	add.l	%d7,%d4
	addq.l	#8,%d4
	lsr.l	#4,%d4
	move.b	%d4,(0,%a1,%d2.l)
	dbf	%d2,.Lchroma_dy4_col
	adda.l	%a2,%a0
	adda.l	%a2,%a4
	adda.l	%a3,%a1
	dbf	%d0,.Lchroma_dy4_row
	bra	.Lchroma_done

	/* dx=4, dy!=4: pair samples horizontally before the weighted sum. */
.Lchroma_dx4_setup:
	moveq	#8,%d5
	sub.l	%d2,%d5			/* inv = 8-dy */
	move.l	%d2,%d6			/* dy */
	move.l	64(%sp),%d0
	subq.l	#1,%d0
.Lchroma_dx4_row:
	move.l	68(%sp),%d2
	add.l	%d2,%d2
	subq.l	#1,%d2
.Lchroma_dx4_col:
	clr.l	%d7
	move.b	(0,%a0,%d2.l),%d7	/* s00 */
	clr.l	%d4
	move.b	(2,%a0,%d2.l),%d4	/* s10 */
	add.w	%d4,%d7
	mulu.w	%d5,%d7			/* (8-dy)*(s00+s10) */
	clr.l	%d4
	move.b	(0,%a4,%d2.l),%d4	/* s01 */
	clr.l	%d3
	move.b	(2,%a4,%d2.l),%d3	/* s11 */
	add.w	%d3,%d4
	mulu.w	%d6,%d4			/* dy*(s01+s11) */
	add.l	%d7,%d4
	addq.l	#8,%d4
	lsr.l	#4,%d4
	move.b	%d4,(0,%a1,%d2.l)
	dbf	%d2,.Lchroma_dx4_col
	adda.l	%a2,%a0
	adda.l	%a2,%a4
	adda.l	%a3,%a1
	dbf	%d0,.Lchroma_dx4_row

.Lchroma_done:
	movem.l	(%sp)+,%d2-%d7/%a2-%a4
	rts

#endif /* MR_M68K_ASM */

#if defined(__ELF__)
	.section .note.GNU-stack,"",@progbits
#endif
