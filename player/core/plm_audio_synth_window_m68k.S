/* MP2 synthesis window: one 32-sample output block, 16 signed 32x32
 * products per lane. ABI: (d, v, v_pos, u). The C implementation remains
 * the reference and is used on 68060 until a trap-free long multiply kernel
 * is available for that CPU. */
	.text
	.even
	.globl	plm_audio_synth_window_m68k
	.globl	_plm_audio_synth_window_m68k
plm_audio_synth_window_m68k:
_plm_audio_synth_window_m68k:
	link.w	%a6,#-8
	movem.l	%d2-%d7/%a2-%a5,-(%sp)
	movea.l	8(%a6),%a2		/* D */
	movea.l	12(%a6),%a3		/* V */
	move.l	16(%a6),%d7		/* v_pos */
	movea.l	20(%a6),%a4		/* U */
	move.l	%d7,%d6
	lsr.l	#1,%d6			/* d_start = 512 - (v_pos >> 1) */
	move.l	#512,%d5
	sub.l	%d6,%d5
	move.l	%d5,%d3
	move.l	%d7,%d6
	andi.l	#127,%d6
	lsr.l	#1,%d6			/* v_start */

	moveq	#0,%d7			/* lane i */
.Lsw_lane:
	moveq	#0,%d4			/* 64-bit sum d4:d5 */
	moveq	#0,%d5
	move.l	%d3,%d2
	add.l	%d7,%d2
	lsl.l	#2,%d2
	movea.l	%a2,%a0
	adda.l	%d2,%a0
	move.l	%d6,%d2
	add.l	%d7,%d2
	lsl.l	#2,%d2
	movea.l	%a3,%a1
	adda.l	%d2,%a1
	moveq	#7,%d2
.Lsw_tap1:
	move.l	(%a0),%d1
	muls.l	(%a1),%d0:%d1
	add.l	%d1,%d5
	addx.l	%d0,%d4
	adda.w	#256,%a0
	adda.w	#512,%a1
	dbra	%d2,.Lsw_tap1

	move.l	%d3,%d2
	add.l	#32,%d2
	add.l	%d7,%d2
	lsl.l	#2,%d2
	movea.l	%a2,%a0
	adda.l	%d2,%a0
	moveq	#96,%d2
	sub.l	%d6,%d2
	add.l	%d7,%d2
	lsl.l	#2,%d2
	movea.l	%a3,%a1
	adda.l	%d2,%a1
	moveq	#7,%d2
.Lsw_tap2:
	move.l	(%a0),%d1
	muls.l	(%a1),%d0:%d1
	add.l	%d1,%d5
	addx.l	%d0,%d4
	adda.w	#256,%a0
	adda.w	#512,%a1
	dbra	%d2,.Lsw_tap2
	move.l	%d4,(%a4)+
	move.l	%d5,(%a4)+
	addq.l	#1,%d7
	cmpi.l	#32,%d7
	bne	.Lsw_lane
	movem.l	(%sp)+,%d2-%d7/%a2-%a5
	unlk	%a6
	rts
