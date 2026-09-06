/*
 * Hand-written m68k assembly for YUV420 -> RGB24 conversion
 * (core/mr_yuv.c's mr_yuv420_to_rgb24), the "rgb-output" stage that runs
 * once per decoded frame before display. A WinUAE/Pistorm log measured
 * this at 6-16ms/frame - real, recurring CPU cost, and unlike the H.264
 * decode path this one is this project's own code (not vendored), so it
 * can be edited directly rather than needing a submodule workaround.
 *
 * m68k-linux-gnu-gcc -O2 -S showed the C loop calling emit_pixel() via an
 * indirect jsr (through a register holding its address) *twice per pixel*
 * with five stack-pushed arguments each time, not inlined despite being a
 * small static function - a real, measurable per-pixel call overhead this
 * asm removes by inlining the same arithmetic directly into the loop body.
 *
 * Algorithm (unchanged from the C - see mr_yuv.c's own header comment):
 * U/V are 4:2:0 subsampled, so each chroma sample pair (uu, vv) is shared
 * by two horizontally-adjacent luma samples AND by two picture rows
 * (chroma row = luma row >> 1). Per pixel:
 *   scaled_y = 298 * (Y - 16), Y-16 clamped to >= 0
 *   R = clip8((scaled_y + 409*(V-128)          + 128) >> 8)
 *   G = clip8((scaled_y - 100*(U-128) - 208*(V-128) + 128) >> 8)
 *   B = clip8((scaled_y + 516*(U-128)          + 128) >> 8)
 * All five products are pre-multiplied into 256-entry tables by
 * build_tables() in mr_yuv.c (no fast integer multiplier on 68030 - a
 * table read beats MULU.L/MULS.L for values applied to an 8-bit input);
 * this function only ever indexes those tables, never multiplies.
 *
 * This walks the picture a ROW PAIR at a time, emitting the whole 2x2 quad
 * a chroma sample covers before moving on. It used to walk single rows,
 * which meant the red/green/blue addend block below - six table-base loads,
 * six indexed reads, the adds and three scratch stores - ran twice for every
 * chroma sample instead of once, for output that was identical either way.
 * The addends depend only on (row>>1, x>>1), so hoisting them over the pair
 * is exact; tests/mr_yuv_check.c holds that against the C, which steps the
 * same way.
 *
 * Register budget for the quad. The pair loop streams row 0 through a0/a1
 * with postincrement, which is the cheap addressing mode and the one used
 * for three quarters of all traffic; row 1 is then reached from those same
 * two pointers with the strides held in d3/d7 and a small negative
 * displacement, costing one extension word per access and no extra address
 * register - there is none free, a0-a6 are all live. The luma and d_x516
 * table bases stay resident in a5/a6 for the entire conversion; a4 is the
 * rotating base for the other three tables.
 *
 * Channel clipping keeps the common 0..255 case to one unsigned compare
 * and branch.  Values outside that range are then split by sign into 0 or
 * 255.  This replaces a pair of signed comparisons on all three channels
 * of every output pixel without changing a single result byte.
 *
 * void mr_yuv420_to_rgb24_m68k(uint8_t *dst, int dst_stride,
 *     const uint8_t *y_plane, int y_stride, const uint8_t *u_plane,
 *     int u_stride, const uint8_t *v_plane, int v_stride, int width,
 *     int height, mr_yuv_service_fn service, void *service_opaque,
 *     const int *luma_x298, const int *e_x409, const int *d_xm100,
 *     const int *e_xm208, const int *d_x516);
 *
 * Only ever called from mr_yuv420_to_rgb24() itself (mr_yuv.c), which
 * already validates the null/size preconditions the C version checks -
 * this function trusts its caller and does not re-check them.
 *
 * m68k C ABI: all seventeen arguments are 4 bytes, passed on the stack in
 * order starting at 4(%sp). d2-d7 and a2-a6 are callee-saved; this saves
 * all eleven (44 bytes) plus a fixed 12-byte local scratch area
 * (red/green/blue_add), so stack offsets below are +56. %d0/%d1/%a0/%a1 do
 * not need saving *for our caller's sake* (d0=row, d1=quad countdown,
 * a0=row-0 Y stream, a1=row-0 dst stream) - but this function is also a
 * caller itself (the periodic service() callback below), and values held in
 * caller-saved registers that are still needed *after* that call must be
 * preserved around it. See the .Lyuv_tail_done comment - missing this the
 * first time caused a real-hardware crash.
 */
#if defined(MR_M68K_ASM) && !defined(MR_YUV_NO_ASM)
	.text
	.even
	.globl	mr_yuv420_to_rgb24_m68k

/* One channel of one pixel: scaled_y (d5) + the addend at \aoff(%sp),
 * arithmetic-shifted down 8 and clipped to a byte. Two forms, because row 0
 * streams through (%a1)+ while row 1 is reached from that same pointer with
 * the destination stride in d7 - the macro argument list cannot carry a
 * whole addressing mode, its commas would be read as argument separators. */
	.macro	MR_YUV_CLIP aoff
	move.l	%d5,%d4
	add.l	\aoff(%sp),%d4
	asr.l	#8,%d4
	cmp.l	#255,%d4
	bls.b	9f
	tst.l	%d4
	bmi.b	7f
	move.l	#255,%d4
	bra.b	9f
7:	moveq	#0,%d4
9:
	.endm

	.macro	MR_YUV_CHAN_S aoff		/* store to (%a1)+ */
	MR_YUV_CLIP \aoff
	move.b	%d4,(%a1)+
	.endm

	.macro	MR_YUV_CHAN_I aoff, disp	/* store to (disp,%a1,%d7.l) */
	MR_YUV_CLIP \aoff
	move.b	%d4,(\disp,%a1,%d7.l)
	.endm

/* scaled_y = luma_x298[clamp(Y-16, 0, ..)] into d5. */
	.macro	MR_YUV_SCALED
	sub.l	#16,%d6
	bge.b	8f
	moveq	#0,%d6
8:	move.l	(%a5,%d6.l*4),%d5
	.endm

/* One RGB triple of row 0: Y from (%a0)+, R/G/B to (%a1)+. */
	.macro	MR_YUV_PIXEL_S
	clr.l	%d6
	move.b	(%a0)+,%d6
	MR_YUV_SCALED
	MR_YUV_CHAN_S 0
	MR_YUV_CHAN_S 4
	MR_YUV_CHAN_S 8
	.endm

/* One RGB triple of row 1, addressed off the already-advanced row-0
 * pointers: Y from (ydisp,%a0,%d3.l), R/G/B to (dr|dg|db,%a1,%d7.l). */
	.macro	MR_YUV_PIXEL_I ydisp, dr, dg, db
	clr.l	%d6
	move.b	(\ydisp,%a0,%d3.l),%d6
	MR_YUV_SCALED
	MR_YUV_CHAN_I 0, \dr
	MR_YUV_CHAN_I 4, \dg
	MR_YUV_CHAN_I 8, \db
	.endm

/* red/green/blue_add for the chroma sample pair at (%a2)+/(%a3)+. */
	.macro	MR_YUV_ADDENDS
	clr.l	%d6
	move.b	(%a2)+,%d6		/* uu */
	move.l	(%a6,%d6.l*4),%d4	/* d_x516[uu] */
	add.l	#128,%d4
	move.l	%d4,8(%sp)		/* blue_add */
	movea.l	116(%sp),%a4		/* d_xm100 */
	move.l	(%a4,%d6.l*4),%d4
	clr.l	%d6
	move.b	(%a3)+,%d6		/* vv */
	movea.l	120(%sp),%a4		/* e_xm208 */
	add.l	(%a4,%d6.l*4),%d4
	add.l	#128,%d4
	move.l	%d4,4(%sp)		/* green_add */
	movea.l	112(%sp),%a4		/* e_x409 */
	move.l	(%a4,%d6.l*4),%d4
	add.l	#128,%d4
	move.l	%d4,0(%sp)		/* red_add */
	.endm

mr_yuv420_to_rgb24_m68k:
	movem.l	%d2-%d7/%a2-%a6,-(%sp)
	sub.l	#12,%sp			/* 0(sp)=red_add 4(sp)=green_add 8(sp)=blue_add */

	movea.l	68(%sp),%a0		/* a0 = row-0 Y stream   (12+56) */
	movea.l	60(%sp),%a1		/* a1 = row-0 dst stream (4+56)  */
	movea.l	76(%sp),%a2		/* a2 = u stream         (20+56) */
	movea.l	84(%sp),%a3		/* a3 = v stream         (28+56) */
	move.l	92(%sp),%d2		/* d2 = width            (36+56) */
	move.l	72(%sp),%d3		/* d3 = y_stride         (16+56) */
	move.l	64(%sp),%d7		/* d7 = dst_stride       (8+56)  */
	movea.l	108(%sp),%a5		/* a5 = luma_x298        (52+56) */
	movea.l	124(%sp),%a6		/* a6 = d_x516           (68+56) */

	clr.l	%d0			/* row = 0 */

/* ---------------- row pairs: rows d0 and d0+1 together ---------------- */
.Lyuv_pair_row:
	move.l	%d0,%d4
	addq.l	#1,%d4
	cmp.l	96(%sp),%d4		/* row + 1 < height ? (40+56) */
	bge	.Lyuv_pairs_done

	move.l	%d2,%d1
	lsr.l	#1,%d1			/* d1 = complete 2x2 quads in this pair */
	beq	.Lyuv_quad_done

.Lyuv_quad_loop:
	MR_YUV_ADDENDS
	/* Row 0 streams forward; row 1 is the same columns one stride down,
	 * addressed off the already-advanced pointers. */
	MR_YUV_PIXEL_S
	MR_YUV_PIXEL_S
	MR_YUV_PIXEL_I -2, -6, -5, -4
	MR_YUV_PIXEL_I -1, -3, -2, -1

	subq.l	#1,%d1
	bne	.Lyuv_quad_loop

.Lyuv_quad_done:
	/* trailing odd column, if any: one more Y and U/V sample, both rows */
	btst	#0,%d2
	beq	.Lyuv_quad_tail_done
	MR_YUV_ADDENDS
	MR_YUV_PIXEL_S
	MR_YUV_PIXEL_I -1, -3, -2, -1
.Lyuv_quad_tail_done:

	/* The C fires service() after any row where (row & 15) == 15, which is
	 * always the odd row of a pair - so here, when ((row + 1) & 15) == 15. */
	move.l	%d0,%d4
	and.l	#15,%d4
	cmp.l	#14,%d4
	bne.b	.Lyuv_pair_no_service
	tst.l	100(%sp)		/* service (44+56) */
	beq.b	.Lyuv_pair_no_service
	movea.l	100(%sp),%a4
	/* d0/d3/d7/a0/a1 are caller-saved and this is a call: row, the two
	 * strides and both stream pointers are all read again right after it
	 * returns, so they must be preserved across an ordinary C service()
	 * that is fully entitled to clobber them. Missing this (for d0/a0/a1)
	 * caused a real-hardware crash - illegal instruction plus visible
	 * screen corruption - the very first time this ran with audio
	 * servicing actually wired up instead of NULL or a trivial test stub.
	 * See check_yuv_service_clobber() in tests/mr_yuv_check.c. */
	movem.l	%d0/%d3/%d7/%a0-%a1,-(%sp)
	move.l	124(%sp),-(%sp)		/* service_opaque: 104 + 20 (movem push) */
	jsr	(%a4)
	addq.l	#4,%sp
	movem.l	(%sp)+,%d0/%d3/%d7/%a0-%a1
.Lyuv_pair_no_service:

	/* a0/a1 point just past row 0's width Y bytes / width*3 RGB bytes;
	 * advance both by two whole rows. */
	move.l	%d3,%d4
	add.l	%d3,%d4
	sub.l	%d2,%d4			/* 2*y_stride - width */
	adda.l	%d4,%a0
	move.l	%d2,%d5
	add.l	%d2,%d5
	add.l	%d2,%d5			/* d5 = width*3 */
	move.l	%d7,%d4
	add.l	%d7,%d4
	sub.l	%d5,%d4			/* 2*dst_stride - width*3 */
	adda.l	%d4,%a1

	/* One chroma row served both picture rows, and a2/a3 consumed
	 * ceil(width/2) of it - step to the next chroma row. */
	move.l	%d2,%d5
	addq.l	#1,%d5
	lsr.l	#1,%d5
	move.l	80(%sp),%d4		/* u_stride (24+56) */
	sub.l	%d5,%d4
	adda.l	%d4,%a2
	move.l	88(%sp),%d4		/* v_stride (32+56) */
	sub.l	%d5,%d4
	adda.l	%d4,%a3

	addq.l	#2,%d0
	bra	.Lyuv_pair_row

/* ------------- odd height: one unpaired final row, if any ------------- */
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
	/* This row is the even one of an incomplete pair, so (row & 15) == 15
	 * cannot hold and the C fires no service() call here either. */

.Lyuv_done:
	add.l	#12,%sp
	movem.l	(%sp)+,%d2-%d7/%a2-%a6
	rts

#endif /* MR_M68K_ASM */

#if defined(__ELF__)
	.section .note.GNU-stack,"",@progbits
#endif
