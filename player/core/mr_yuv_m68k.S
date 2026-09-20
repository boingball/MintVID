/*
 * Hand-written m68k YUV420 -> RGB24 conversion. The coefficient tables and
 * 2x2 chroma sharing match core/mr_yuv.c exactly; BGR24 also dispatches here
 * with permuted tables. Keep the service callback's caller-saved register
 * preservation intact: real PiStorm hardware previously crashed without it.
 *
 * RGB24 store optimisation: a4 is only needed for coefficient lookups while
 * loading a chroma pair. After row 0 is emitted, reuse it as a streaming
 * row-1 output pointer. One indexed LEA replaces six indexed byte stores
 * per 2x2 quad; output remains three postincrement byte stores per pixel.
 * Packing RGB24 into wider writes requires extra shifts/merges, risks
 * unaligned/overlapping writes, and has not been shown faster on PiStorm.
 *
 * Clipping optimisation: after the arithmetic shift the maximum possible
 * channel range for any 8-bit Y/U/V input is -258..534, so the signed value
 * fits a word. Word comparisons are sufficient and moveq #-1 produces the
 * same output byte (255) as a longword immediate. The in-range branch is
 * still the common path, with the original exact clipping behaviour.
 *
 * ABI: 17 four-byte arguments on the stack, in the order declared by
 * mr_yuv420_to_rgb24_m68k in mr_yuv.c. Save d2-d7/a2-a6 (44 bytes) and 12
 * bytes of red/green/blue scratch: argument offsets are original +56.
 */
#if defined(MR_M68K_ASM) && !defined(MR_YUV_NO_ASM)
	.text
	.even
	.globl	mr_yuv420_to_rgb24_m68k

/* d5 = scaled Y; scratch addend in 0/4/8(sp). Return clipped byte in d4. */
	.macro	MR_YUV_CLIP aoff
	move.l	%d5,%d4
	add.l	\aoff(%sp),%d4
	asr.l	#8,%d4
	cmp.w	#255,%d4
	bls.b	9f
	tst.w	%d4
	bmi.b	7f
	moveq	#-1,%d4		/* low byte = 255 */
	bra.b	9f
7:	moveq	#0,%d4
9:
	.endm

	.macro	MR_YUV_CHAN_S aoff
	MR_YUV_CLIP \aoff
	move.b	%d4,(%a1)+
	.endm

/* a4 streams along the second output row within this chroma quad. */
	.macro	MR_YUV_CHAN_1 aoff
	MR_YUV_CLIP \aoff
	move.b	%d4,(%a4)+
	.endm

	.macro	MR_YUV_SCALED
	move.l	(%a5,%d6.l*4),%d5
	.endm

/* Row 0 Y and destination advance together. */
	.macro	MR_YUV_PIXEL_S
	clr.l	%d6
	move.b	(%a0)+,%d6
	MR_YUV_SCALED
	MR_YUV_CHAN_S 0
	MR_YUV_CHAN_S 4
	MR_YUV_CHAN_S 8
	.endm

/* Row 1 Y is indexed from the advanced row-0 pointer; a4 advances over RGB. */
	.macro	MR_YUV_PIXEL_1 ydisp
	clr.l	%d6
	move.b	(\ydisp,%a0,%d3.l),%d6
	MR_YUV_SCALED
	MR_YUV_CHAN_1 0
	MR_YUV_CHAN_1 4
	MR_YUV_CHAN_1 8
	.endm

/* Chroma addends shared by both rows and both columns of a 2x2 quad. */
	.macro	MR_YUV_ADDENDS
	clr.l	%d6
	move.b	(%a2)+,%d6		/* uu */
	move.l	(%a6,%d6.l*4),%d4	/* blue */
	move.l	%d4,8(%sp)
	movea.l	116(%sp),%a4		/* d_xm100 */
	move.l	(%a4,%d6.l*4),%d4
	clr.l	%d6
	move.b	(%a3)+,%d6		/* vv */
	movea.l	120(%sp),%a4		/* e_xm208 */
	add.l	(%a4,%d6.l*4),%d4
	move.l	%d4,4(%sp)
	movea.l	112(%sp),%a4		/* e_x409 */
	move.l	(%a4,%d6.l*4),%d4
	move.l	%d4,0(%sp)
	.endm

mr_yuv420_to_rgb24_m68k:
	movem.l	%d2-%d7/%a2-%a6,-(%sp)
	sub.l	#12,%sp

	movea.l	68(%sp),%a0		/* row-0 Y */
	movea.l	60(%sp),%a1		/* row-0 RGB */
	movea.l	76(%sp),%a2		/* U */
	movea.l	84(%sp),%a3		/* V */
	move.l	92(%sp),%d2		/* width */
	move.l	72(%sp),%d3		/* Y stride */
	move.l	64(%sp),%d7		/* RGB stride */
	movea.l	108(%sp),%a5		/* luma table */
	movea.l	124(%sp),%a6		/* blue table */
	clr.l	%d0			/* row = 0 */

/* Process pairs of complete picture rows. */
.Lyuv_pair_row:
	move.l	%d0,%d4
	addq.l	#1,%d4
	cmp.l	96(%sp),%d4
	bge	.Lyuv_pairs_done

	move.l	%d2,%d1
	lsr.l	#1,%d1
	beq	.Lyuv_quad_done

.Lyuv_quad_loop:
	MR_YUV_ADDENDS
	MR_YUV_PIXEL_S
	MR_YUV_PIXEL_S
	/* a1 is now six bytes after this quad's row-0 RGB start. Derive
	 * row-1 start once instead of indexing all six channel stores. */
	lea	(-6,%a1,%d7.l),%a4
	MR_YUV_PIXEL_1 -2
	MR_YUV_PIXEL_1 -1
	subq.l	#1,%d1
	bne	.Lyuv_quad_loop

.Lyuv_quad_done:
	btst	#0,%d2
	beq	.Lyuv_quad_tail_done
	MR_YUV_ADDENDS
	MR_YUV_PIXEL_S
	/* A trailing odd column occupies only three RGB bytes. */
	lea	(-3,%a1,%d7.l),%a4
	MR_YUV_PIXEL_1 -1
.Lyuv_quad_tail_done:

	/* Service audio after rows 15,31,... without clobbering live state. */
	move.l	%d0,%d4
	and.l	#15,%d4
	cmp.l	#14,%d4
	bne.b	.Lyuv_pair_no_service
	tst.l	100(%sp)
	beq.b	.Lyuv_pair_no_service
	movea.l	100(%sp),%a4
	/* C callbacks may clobber d0/d3/d7/a0/a1. The old omission of
	 * these saves caused an actual illegal-instruction/hardware crash. */
	movem.l	%d0/%d3/%d7/%a0-%a1,-(%sp)
	move.l	124(%sp),-(%sp)	/* opaque arg: 104 + 20 saved bytes */
	jsr	(%a4)
	addq.l	#4,%sp
	movem.l	(%sp)+,%d0/%d3/%d7/%a0-%a1
.Lyuv_pair_no_service:

	/* Advance stream pointers by two full rows from the end of row 0. */
	move.l	%d3,%d4
	add.l	%d3,%d4
	sub.l	%d2,%d4
	adda.l	%d4,%a0
	move.l	%d2,%d5
	add.l	%d2,%d5
	add.l	%d2,%d5
	move.l	%d7,%d4
	add.l	%d7,%d4
	sub.l	%d5,%d4
	adda.l	%d4,%a1

	/* Move U/V to the next chroma row after ceil(width/2) samples. */
	move.l	%d2,%d5
	addq.l	#1,%d5
	lsr.l	#1,%d5
	move.l	80(%sp),%d4
	sub.l	%d5,%d4
	adda.l	%d4,%a2
	move.l	88(%sp),%d4
	sub.l	%d5,%d4
	adda.l	%d4,%a3
	addq.l	#2,%d0
	bra	.Lyuv_pair_row

/* A final unpaired row when height is odd. */
.Lyuv_pairs_done:
	cmp.l	96(%sp),%d0
	bge	.Lyuv_done
	move.l	%d2,%d1
	lsr.l	#1,%d1
	beq	.Lyuv_last_tail
.Lyuv_last_loop:
	MR_YUV_ADDENDS
	MR_YUV_PIXEL_S
	MR_YUV_PIXEL_S
	subq.l	#1,%d1
	bne	.Lyuv_last_loop
.Lyuv_last_tail:
	btst	#0,%d2
	beq	.Lyuv_last_done
	MR_YUV_ADDENDS
	MR_YUV_PIXEL_S
.Lyuv_last_done:
.Lyuv_done:
	add.l	#12,%sp
	movem.l	(%sp)+,%d2-%d7/%a2-%a6
	rts
#endif /* MR_M68K_ASM */

#if defined(__ELF__)
	.section .note.GNU-stack,"",@progbits
#endif
