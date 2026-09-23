/*
 * Hand-written m68k YUV420 -> RGB24/BGR24 conversion, bit-identical to the
 * portable C in core/mr_yuv.c. Keep the service callback's caller-saved
 * register preservation intact: real PiStorm hardware previously crashed
 * without it (tests/mr_yuv_check.c's clobbering service holds that).
 *
 * Every table lives in one block built by mr_yuv.c (g_m68k_block), so
 * the whole 2x2 quad runs from a single base register:
 *
 *   luma:       4 * (bbase | y)          one int per Y byte (298*(Y-16))
 *   A pairs:    8 * (abase | a)          { ch2 addend, ch1 addend part }
 *   B pairs:    8 * (bbase | b)          { ch0 addend, ch1 addend part }
 *
 * where bbase = abase + 0x100. d6 is loaded with a slot base and move.b
 * drops the sample into its low byte, so one register indexes both slots;
 * the luma lookups reuse d6 as left by the B-slot load, so no per-pixel
 * clear is needed. The bases come from two stack longwords rather than an
 * add.w/sub.w #256 on d6: same instruction count, but a word-sized result
 * with flags is a partial register write that qemu - and so, roughly, a
 * JIT such as Emu68 - charges several extra host instructions for.
 * RGB24 passes abase 0x300 with A = U, B = V; BGR24 passes abase 0x400
 * with A = V, B = U, which the block stores a second time at 0x500.
 *
 * Clipping is a byte lookup: after the arithmetic shift every channel lies
 * in -258..534, so (clip,d4.l) with clip pointing at the 0 entry saturates
 * without a compare or branch. Per pixel that is 13 instructions: load Y,
 * look up 298*(Y-16), then copy/add/shift/store per channel (the last
 * channel adds in place).
 *
 * ABI: 15 four-byte arguments on the stack, in the order declared by
 * mr_yuv420_to_rgb24_m68k in mr_yuv.c. Saves d2-d7/a2-a6 (44 bytes) and
 * reserves 28 bytes of locals, so argument N sits at 76 + 4*N.
 */
#if defined(MR_M68K_ASM) && !defined(MR_YUV_NO_ASM)
	.text
	.even
	.globl	mr_yuv420_to_rgb24_m68k

/* Locals. */
#define L_ROW	0
#define L_YADV	4
#define L_OADV	8
#define L_AADV	12
#define L_BADV	16
#define L_ASLOT	20
#define L_BSLOT	24
/* Arguments. */
#define A_DST	76
#define A_DSTR	80
#define A_Y	84
#define A_YSTR	88
#define A_A	92
#define A_ASTR	96
#define A_B	100
#define A_BSTR	104
#define A_W	108
#define A_H	112
#define A_SVC	116
#define A_OPQ	120
#define A_TAB	124
#define A_CLIP	128
#define A_ABASE	132

/* Chroma addends shared by both rows and both columns of a 2x2 quad:
 * d0 = ch0, d2 = ch1, d7 = ch2; d6 is left holding bbase | b. */
	.macro	MR_YUV_ADDENDS
	move.l	L_ASLOT(%sp),%d6
	move.b	(%a2)+,%d6
	move.l	(%a5,%d6.l*8),%d7
	move.l	(4,%a5,%d6.l*8),%d2
	move.l	L_BSLOT(%sp),%d6
	move.b	(%a3)+,%d6
	move.l	(%a5,%d6.l*8),%d0
	add.l	(4,%a5,%d6.l*8),%d2
	.endm

/* One pixel: src is the Y byte operand; the three channels go to
 * o(dst), o+1(dst), o+2(dst). Pointers advance once per quad, not per
 * store: under qemu (and so, roughly, a JIT such as Emu68) every
 * postincrement is a separate host add. */
	.macro	MR_YUV_PIXEL src, dst, o
	move.b	\src,%d6
	move.l	(%a5,%d6.l*4),%d5
	move.l	%d5,%d4
	add.l	%d0,%d4
	asr.l	#8,%d4
	move.b	(%a6,%d4.l),\o(\dst)
	move.l	%d5,%d4
	add.l	%d2,%d4
	asr.l	#8,%d4
	move.b	(%a6,%d4.l),\o+1(\dst)
	add.l	%d7,%d5
	asr.l	#8,%d5
	move.b	(%a6,%d5.l),\o+2(\dst)
	.endm

mr_yuv420_to_rgb24_m68k:
	movem.l	%d2-%d7/%a2-%a6,-(%sp)
	sub.l	#28,%sp

	movea.l	A_Y(%sp),%a0		/* row-0 Y */
	movea.l	A_DST(%sp),%a1		/* row-0 output */
	movea.l	A_A(%sp),%a2
	movea.l	A_B(%sp),%a3
	movea.l	A_TAB(%sp),%a5
	lea	-4096(%a5),%a5		/* block starts at virtual index 0x400 */
	movea.l	A_CLIP(%sp),%a6
	move.l	A_YSTR(%sp),%d3
	move.l	A_ABASE(%sp),%d6
	move.l	%d6,L_ASLOT(%sp)
	add.l	#256,%d6
	move.l	%d6,L_BSLOT(%sp)

	/* Per-row-pair pointer advances, measured from where a quad run ends. */
	move.l	A_W(%sp),%d1
	move.l	%d3,%d4
	add.l	%d4,%d4
	sub.l	%d1,%d4
	move.l	%d4,L_YADV(%sp)		/* 2*ystride - w */
	move.l	%d1,%d5
	add.l	%d1,%d5
	add.l	%d1,%d5			/* 3*w */
	move.l	A_DSTR(%sp),%d4
	add.l	%d4,%d4
	sub.l	%d5,%d4
	move.l	%d4,L_OADV(%sp)		/* 2*dstride - 3*w */
	addq.l	#1,%d1
	lsr.l	#1,%d1			/* ceil(w/2) chroma samples per row */
	move.l	A_ASTR(%sp),%d4
	sub.l	%d1,%d4
	move.l	%d4,L_AADV(%sp)
	move.l	A_BSTR(%sp),%d4
	sub.l	%d1,%d4
	move.l	%d4,L_BADV(%sp)
	clr.l	L_ROW(%sp)

/* Process pairs of complete picture rows. */
.Lyuv_pair_row:
	move.l	L_ROW(%sp),%d4
	addq.l	#1,%d4
	cmp.l	A_H(%sp),%d4
	bge	.Lyuv_pairs_done

	movea.l	A_DSTR(%sp),%a4
	adda.l	%a1,%a4			/* row-1 output */
	move.l	A_W(%sp),%d1
	lsr.l	#1,%d1
	beq	.Lyuv_quad_done

.Lyuv_quad_loop:
	MR_YUV_ADDENDS
	MR_YUV_PIXEL (%a0), %a1, 0
	MR_YUV_PIXEL 1(%a0), %a1, 3
	MR_YUV_PIXEL "(0,%a0,%d3.l)", %a4, 0
	MR_YUV_PIXEL "(1,%a0,%d3.l)", %a4, 3
	addq.l	#2,%a0
	addq.l	#6,%a1
	addq.l	#6,%a4
	subq.l	#1,%d1
	bne	.Lyuv_quad_loop

.Lyuv_quad_done:
	btst	#0,A_W+3(%sp)
	beq	.Lyuv_quad_tail_done
	MR_YUV_ADDENDS
	MR_YUV_PIXEL (%a0), %a1, 0
	MR_YUV_PIXEL "(0,%a0,%d3.l)", %a4, 0
	addq.l	#1,%a0
	addq.l	#3,%a1
.Lyuv_quad_tail_done:

	/* Service audio after rows 15,31,... without clobbering live state.
	 * a0/a1 are live; d0/d1/a4 are rebuilt before their next use. */
	move.l	L_ROW(%sp),%d4
	and.l	#15,%d4
	cmp.l	#14,%d4
	bne.b	.Lyuv_pair_no_service
	tst.l	A_SVC(%sp)
	beq.b	.Lyuv_pair_no_service
	movea.l	A_SVC(%sp),%a4
	movem.l	%d0-%d1/%a0-%a1,-(%sp)
	move.l	A_OPQ+16(%sp),-(%sp)
	jsr	(%a4)
	addq.l	#4,%sp
	movem.l	(%sp)+,%d0-%d1/%a0-%a1
.Lyuv_pair_no_service:

	adda.l	L_YADV(%sp),%a0
	adda.l	L_OADV(%sp),%a1
	adda.l	L_AADV(%sp),%a2
	adda.l	L_BADV(%sp),%a3
	addq.l	#2,L_ROW(%sp)
	bra	.Lyuv_pair_row

/* A final unpaired row when height is odd. */
.Lyuv_pairs_done:
	move.l	L_ROW(%sp),%d4
	cmp.l	A_H(%sp),%d4
	bge	.Lyuv_done
	move.l	A_W(%sp),%d1
	lsr.l	#1,%d1
	beq	.Lyuv_last_tail
.Lyuv_last_loop:
	MR_YUV_ADDENDS
	MR_YUV_PIXEL (%a0), %a1, 0
	MR_YUV_PIXEL 1(%a0), %a1, 3
	addq.l	#2,%a0
	addq.l	#6,%a1
	subq.l	#1,%d1
	bne	.Lyuv_last_loop
.Lyuv_last_tail:
	btst	#0,A_W+3(%sp)
	beq	.Lyuv_done
	MR_YUV_ADDENDS
	MR_YUV_PIXEL (%a0), %a1, 0
.Lyuv_done:
	add.l	#28,%sp
	movem.l	(%sp)+,%d2-%d7/%a2-%a6
	rts

#endif /* MR_M68K_ASM */

#if defined(__ELF__)
	.section .note.GNU-stack,"",@progbits
#endif
