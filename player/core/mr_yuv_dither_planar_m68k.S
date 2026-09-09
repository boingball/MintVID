/*
 * Experimental wrapper around MintVID's established m68k YUV->indexed
 * converter.  The original source is included verbatim under a private symbol;
 * the public entry point becomes a two-instruction dispatcher so the normal
 * path pays essentially nothing when the planar-queue experiment is disabled.
 *
 * Keeping the original routine as the worker is deliberate: this experiment is
 * about pipeline/layout cost, not changing colour arithmetic at the same time.
 */
#define mr_yuv420_dither8_m68k mr_yuv420_dither8_m68k_base
#include "mr_yuv_dither_m68k.S"
#undef mr_yuv420_dither8_m68k

#if defined(MR_M68K_ASM)
	.text
	.even
	.globl	mr_yuv420_dither8_m68k
	.extern	mr_yuv_planar_queue_active
	.extern	mr_yuv_planar_queue_m68k
	.extern	mr_yuv420_dither8_m68k_base

mr_yuv420_dither8_m68k:
	move.l	mr_yuv_planar_queue_active,%d0
	tst.l	%d0
	bne.b	.Lyd_planar_queue
	jmp	mr_yuv420_dither8_m68k_base
.Lyd_planar_queue:
	jmp	mr_yuv_planar_queue_m68k
#endif

#if defined(__ELF__)
	.section .note.GNU-stack,"",@progbits
#endif
