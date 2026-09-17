/* 
 *  weighting.c
 *
 *     Copyright (C) Charles 'Buck' Krasic - April 2000
 *     Copyright (C) Erik Walthinsen - April 2000
 *
 *  This file is part of libdv, a free DV (IEC 61834/SMPTE 314M)
 *  codec.
 *
 *  libdv is free software; you can redistribute it and/or modify it
 *  under the terms of the GNU Lesser Public License as published by
 *  the Free Software Foundation; either version 2.1, or (at your
 *  option) any later version.
 *   
 *  libdv is distributed in the hope that it will be useful, but
 *  WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *  Lesser Public License for more details.
 *   
 *  You should have received a copy of the GNU Lesser Public License
 *  along with libdv; see the file COPYING.  If not, write to
 *  the Free Software Foundation, 675 Mass Ave, Cambridge, MA 02139, USA. 
 *
 *  The libdv homepage is http://libdv.sourceforge.net/.  
 */

/** @file
 *  @ingroup weighting
 *  @brief Implementation for @link weighting Coefficient Weighting @endlink
 */

/** @weakgroup weighting Coefficient Weighting
 *
 *  DCT coeffictions are stored with 10 bits of precision, although
 *  there is the possibility that the DCT transform will produce
 *  values outside of this range.  The weighting operation ensures
 *  that all values fit into the allocated 10 bit range.
 *  
 *  @{
 */

#if HAVE_CONFIG_H
# include <config.h>
#endif

#include "weighting.h"

dv_coeff_t preSC[64] ALIGN32 = {
	16384,22725,21407,19266, 16384,12873,8867,4520,
	22725,31521,29692,26722, 22725,17855,12299,6270,
	21407,29692,27969,25172, 21407,16819,11585,5906,
	19266,26722,25172,22654, 19266,15137,10426,5315,

	16384,22725,21407,19266, 16384,12873,8867,4520,
	12873,17855,16819,15137, 25746,20228,13933,7103,
	17734,24598,23170,20853, 17734,13933,9597,4892,
	18081,25080,23624,21261, 18081,14206,9785,4988
};

/* MintVID adaptation: postSC88/postSC248 are consumed only by dct.c's
 * postscale88()/postscale248(), which are themselves only reached from
 * the forward-DCT AAN encoder path (_dv_dct_88()/_dv_dct_248()) - dead
 * code in this decode-only tree (nothing calls either function; see
 * core/mr_dv.c's own header). Left declared (implicitly zero-initialised)
 * so dct.c's extern reference still links, but never filled or read. */
dv_coeff_t postSC88[64] ALIGN32;
dv_coeff_t postSC248[64] ALIGN32;

/* MintVID adaptation: dv_weight_inverse_88_matrix was computed at
 * dv_init() time via cos()/rint() on a W[8] table itself built from
 * cos(). On this target (68k Amiga, host build's own m68k-amigaos-gcc
 * cross-compile) that is a real crash, not just a slow path: 68040/68060
 * hardware FPUs do not implement transcendental instructions at all
 * (FCOS etc. trap to the "Line 1111 emulator" exception - AmigaOS Guru
 * 8000000B - unless fpsp040.library is loaded to emulate them in
 * software, which a real playback system may not have), and MintVID's
 * own design intent for the m68k build is integer-only, no FPU
 * dependency at all (see CLAUDE.md's DV decoder section). The matrix is
 * a pure function of the DV standard's own constants with no runtime
 * decoder state, so it is precomputed once, offline, from the exact
 * original formula (CS(m)=cos(m*pi/16), W[]/weight_88_inverse_float() as
 * upstream defines them, then dv_coeff_t(rint(...)) matching the
 * original's own rounding) and hardcoded here - see
 * tools/gen_dv_tables.py for the generator. dv_weight_inverse_248_matrix
 * and W[] themselves are no longer needed at runtime: their only other
 * consumer, idct_248.c's dv_dct_248_init(), is precomputed the same way
 * (see that file). postscale88_init()/postscale248_init()/
 * weight_88_float()/weight_248_float() (the encode-only postSC88/248 and
 * dv_weight_88_matrix/dv_weight_248_matrix computations) are dropped
 * entirely along with them - dead code in this decode-only tree, per the
 * postSC88/248 note above. */
#if (!ARCH_X86) && (!ARCH_X86_64)
static const dv_coeff_t dv_weight_inverse_88_matrix[64] = {
	4, 2, 2, 2, 2, 2, 3, 3,
	2, 2, 2, 2, 2, 2, 3, 3,
	2, 2, 2, 2, 2, 3, 3, 3,
	2, 2, 2, 2, 3, 3, 3, 3,
	2, 2, 2, 3, 3, 3, 3, 3,
	2, 2, 3, 3, 3, 3, 3, 3,
	3, 3, 3, 3, 3, 3, 3, 4,
	3, 3, 3, 3, 3, 3, 4, 4,
};
#endif

void _dv_weight_init(void)
{
	/* No runtime work left to do - see dv_weight_inverse_88_matrix's own
	 * comment above. Kept as a callable no-op since dv.c's dv_init()
	 * calls it unconditionally. */
}

void _dv_weight_88(dv_coeff_t *block)
{
	/* Forward-weight (encode-only); dead code in this decode-only tree -
	 * see postSC88's own comment above. Never called. */
	(void)block;
}

void _dv_weight_248(dv_coeff_t *block)
{
	/* Forward-weight (encode-only); dead code in this decode-only tree -
	 * see postSC88's own comment above. Never called. */
	(void)block;
}

void _dv_weight_88_inverse(dv_coeff_t *block)
{
	/* When we're using MMX assembler, weights are applied in the 8x8
	   iDCT prescale */
#if (!ARCH_X86) && (!ARCH_X86_64)
	int i;

	for (i=0;i<64;i++) {
		block[i] *= dv_weight_inverse_88_matrix[i];
	}
#endif
}

void _dv_weight_248_inverse(dv_coeff_t *block) 
{
	/* These weights are now folded into the idct prescalers - so this
	   function doesn't do anything. */
#if 0
	int x,z;
	dv_coeff_t dc;
	
	dc = block[0];
	for (z=0;z<4;z++) {
		for (x=0;x<8;x++) {
			block[z*8+x] /= (W[x] * W[2*z] / 2);
			block[(z+4)*8+x] /= (W[x] * W[2*z] / 2);
		}
	}
	block[0] = dc * 4;
#endif
}

/*@}*/
