/*
 * idct.c
 * Copyright (C) 2000-2003 Michel Lespinasse <walken@zoy.org>
 * Copyright (C) 1999-2000 Aaron Holtzman <aholtzma@ess.engr.uvic.ca>
 *
 * This file is part of mpeg2dec, a free MPEG-2 video stream decoder.
 * See http://libmpeg2.sourceforge.net/ for updates.
 *
 * mpeg2dec is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * mpeg2dec is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with mpeg2dec; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#include "config.h"

#include <stdlib.h>
#include <string.h>
#include <inttypes.h>

#include "mpeg2.h"
#include "attributes.h"
#include "mpeg2_internal.h"

#define W1 2841 /* 2048 * sqrt (2) * cos (1 * pi / 16) */
#define W2 2676 /* 2048 * sqrt (2) * cos (2 * pi / 16) */
#define W3 2408 /* 2048 * sqrt (2) * cos (3 * pi / 16) */
#define W5 1609 /* 2048 * sqrt (2) * cos (5 * pi / 16) */
#define W6 1108 /* 2048 * sqrt (2) * cos (6 * pi / 16) */
#define W7 565  /* 2048 * sqrt (2) * cos (7 * pi / 16) */

/* idct main entry point  */
void (* mpeg2_idct_copy) (int16_t * block, uint8_t * dest, int stride);
void (* mpeg2_idct_add) (int last, int16_t * block,
			 uint8_t * dest, int stride);

/*
 * In legal streams, the IDCT output should be between -384 and +384.
 * In corrupted streams, it is possible to force the IDCT output to go
 * to +-3826 - this is the worst case for a column IDCT where the
 * column inputs are 16-bit values.
 */
uint8_t mpeg2_clip[3840 * 2 + 256];
#define CLIP(i) ((mpeg2_clip + 3840)[i])

#if 0
#define BUTTERFLY(t0,t1,W0,W1,d0,d1)	\
do {					\
    t0 = W0 * d0 + W1 * d1;		\
    t1 = W0 * d1 - W1 * d0;		\
} while (0)
#else
#define BUTTERFLY(t0,t1,W0,W1,d0,d1)	\
do {					\
    int tmp = W0 * (d0 + d1);		\
    t0 = tmp + (W1 - W0) * d1;		\
    t1 = tmp - (W1 + W0) * d0;		\
} while (0)
#endif

static inline void idct_row (int16_t * const block)
{
    int d0, d1, d2, d3;
    int a0, a1, a2, a3, b0, b1, b2, b3;
    int t0, t1, t2, t3;

    /* shortcut */
    if (likely (!(block[1] | ((int32_t *)block)[1] | ((int32_t *)block)[2] |
		  ((int32_t *)block)[3]))) {
	uint32_t tmp = (uint16_t) (block[0] >> 1);
	tmp |= tmp << 16;
	((int32_t *)block)[0] = tmp;
	((int32_t *)block)[1] = tmp;
	((int32_t *)block)[2] = tmp;
	((int32_t *)block)[3] = tmp;
	return;
    }

    d0 = (block[0] << 11) + 2048;
    d1 = block[1];
    d2 = block[2] << 11;
    d3 = block[3];
    t0 = d0 + d2;
    t1 = d0 - d2;
    BUTTERFLY (t2, t3, W6, W2, d3, d1);
    a0 = t0 + t2;
    a1 = t1 + t3;
    a2 = t1 - t3;
    a3 = t0 - t2;

    d0 = block[4];
    d1 = block[5];
    d2 = block[6];
    d3 = block[7];
    BUTTERFLY (t0, t1, W7, W1, d3, d0);
    BUTTERFLY (t2, t3, W3, W5, d1, d2);
    b0 = t0 + t2;
    b3 = t1 + t3;
    t0 -= t2;
    t1 -= t3;
    b1 = ((t0 + t1) >> 8) * 181;
    b2 = ((t0 - t1) >> 8) * 181;

    block[0] = (a0 + b0) >> 12;
    block[1] = (a1 + b1) >> 12;
    block[2] = (a2 + b2) >> 12;
    block[3] = (a3 + b3) >> 12;
    block[4] = (a3 - b3) >> 12;
    block[5] = (a2 - b2) >> 12;
    block[6] = (a1 - b1) >> 12;
    block[7] = (a0 - b0) >> 12;
}

static inline void idct_col (int16_t * const block)
{
    int d0, d1, d2, d3;
    int a0, a1, a2, a3, b0, b1, b2, b3;
    int t0, t1, t2, t3;

    d0 = (block[8*0] << 11) + 65536;
    d1 = block[8*1];
    d2 = block[8*2] << 11;
    d3 = block[8*3];
    t0 = d0 + d2;
    t1 = d0 - d2;
    BUTTERFLY (t2, t3, W6, W2, d3, d1);
    a0 = t0 + t2;
    a1 = t1 + t3;
    a2 = t1 - t3;
    a3 = t0 - t2;

    d0 = block[8*4];
    d1 = block[8*5];
    d2 = block[8*6];
    d3 = block[8*7];
    BUTTERFLY (t0, t1, W7, W1, d3, d0);
    BUTTERFLY (t2, t3, W3, W5, d1, d2);
    b0 = t0 + t2;
    b3 = t1 + t3;
    t0 -= t2;
    t1 -= t3;
    b1 = ((t0 + t1) >> 8) * 181;
    b2 = ((t0 - t1) >> 8) * 181;

    block[8*0] = (a0 + b0) >> 17;
    block[8*1] = (a1 + b1) >> 17;
    block[8*2] = (a2 + b2) >> 17;
    block[8*3] = (a3 + b3) >> 17;
    block[8*4] = (a3 - b3) >> 17;
    block[8*5] = (a2 - b2) >> 17;
    block[8*6] = (a1 - b1) >> 17;
    block[8*7] = (a0 - b0) >> 17;
}

/*
 * MintVID: sparse-block shortcuts, all exact (tests/mr_mpeg2_idct_check.c
 * compares against the imported code in tests/mr_mpeg2_idct_reference.h).
 *
 * The coefficient layout is permuted (see mpeg2_idct_init): storage row r
 * holds vertical frequency 0,2,4,6,1,3,5,7 for r = 0..7. An all-zero input
 * row stays zero through idct_row, so rows_pass() skips it and returns a
 * mask of the rows it did transform. Most blocks at VCD bitrates only use
 * vertical frequencies 0 and 1 (storage rows 0 and 4); for those the column
 * pass reduces algebraically. Skipping multiplications by zero changes no
 * value, so the output is bit-identical. The idea of skipping empty lines
 * and picking a cheaper transform per block follows RiVA's 68k IDCT
 * (MacrosIDCT68k.m), without its non-IEEE-1180 AAN arithmetic.
 */
static inline int rows_pass (int16_t * const block)
{
    int i, mask = 0;

    for (i = 0; i < 8; i++) {
	int32_t * const row = (int32_t *)(block + 8 * i);
	if (row[0] | row[1] | row[2] | row[3]) {
	    idct_row (block + 8 * i);
	    mask |= 1 << i;
	}
    }
    return mask;
}

/* idct_col when only storage rows 0 and 4 can be non-zero. */
static inline void idct_col_r04 (int16_t * const block)
{
    int d0 = (block[8*0] << 11) + 65536;
    int x = block[8*4];
    int b0 = W1 * x;
    int b3 = W7 * x;
    int b1 = ((b0 + b3) >> 8) * 181;
    int b2 = ((b0 - b3) >> 8) * 181;

    block[8*0] = (d0 + b0) >> 17;
    block[8*1] = (d0 + b1) >> 17;
    block[8*2] = (d0 + b2) >> 17;
    block[8*3] = (d0 + b3) >> 17;
    block[8*4] = (d0 - b3) >> 17;
    block[8*5] = (d0 - b2) >> 17;
    block[8*6] = (d0 - b1) >> 17;
    block[8*7] = (d0 - b0) >> 17;
}

static inline void cols_pass (int16_t * const block, const int mask)
{
    int i;

    if (!(mask & ~0x11))
	for (i = 0; i < 8; i++)
	    idct_col_r04 (block + i);
    else
	for (i = 0; i < 8; i++)
	    idct_col (block + i);
}

/* Only storage row 0 is non-zero: idct_col's output is
 * ((v << 11) + 65536) >> 17 = (v + 32) >> 6 down the whole column. Returns
 * the eight column values in col[] and clears row 0 (the rest is zero). */
static inline void row0_only (int16_t * const block, int * const col)
{
    int i;

    for (i = 0; i < 8; i++)
	col[i] = (block[i] + 32) >> 6;
    ((int32_t *)block)[0] = 0;	((int32_t *)block)[1] = 0;
    ((int32_t *)block)[2] = 0;	((int32_t *)block)[3] = 0;
}

static void mpeg2_idct_copy_c (int16_t * block, uint8_t * dest,
			       const int stride)
{
    int i;
    int mask = rows_pass (block);

    if (!(mask & ~1)) {
	int col[8];
	uint8_t line[8];

	row0_only (block, col);
	for (i = 0; i < 8; i++)
	    line[i] = CLIP (col[i]);
	i = 8;
	do {
	    memcpy (dest, line, 8);
	    dest += stride;
	} while (--i);
	return;
    }
    cols_pass (block, mask);
    i = 8;
    do {
	dest[0] = CLIP (block[0]);
	dest[1] = CLIP (block[1]);
	dest[2] = CLIP (block[2]);
	dest[3] = CLIP (block[3]);
	dest[4] = CLIP (block[4]);
	dest[5] = CLIP (block[5]);
	dest[6] = CLIP (block[6]);
	dest[7] = CLIP (block[7]);

	((int32_t *)block)[0] = 0;	((int32_t *)block)[1] = 0;
	((int32_t *)block)[2] = 0;	((int32_t *)block)[3] = 0;

	dest += stride;
	block += 8;
    } while (--i);
}

static void mpeg2_idct_add_c (const int last, int16_t * block,
			      uint8_t * dest, const int stride)
{
    int i;

    if (last != 129 || (block[0] & (7 << 4)) == (4 << 4)) {
	int mask = rows_pass (block);

	if (!(mask & ~1)) {
	    int col[8];

	    row0_only (block, col);
	    i = 8;
	    do {
		dest[0] = CLIP (col[0] + dest[0]);
		dest[1] = CLIP (col[1] + dest[1]);
		dest[2] = CLIP (col[2] + dest[2]);
		dest[3] = CLIP (col[3] + dest[3]);
		dest[4] = CLIP (col[4] + dest[4]);
		dest[5] = CLIP (col[5] + dest[5]);
		dest[6] = CLIP (col[6] + dest[6]);
		dest[7] = CLIP (col[7] + dest[7]);
		dest += stride;
	    } while (--i);
	    return;
	}
	cols_pass (block, mask);
	i = 8;
	do {
	    dest[0] = CLIP (block[0] + dest[0]);
	    dest[1] = CLIP (block[1] + dest[1]);
	    dest[2] = CLIP (block[2] + dest[2]);
	    dest[3] = CLIP (block[3] + dest[3]);
	    dest[4] = CLIP (block[4] + dest[4]);
	    dest[5] = CLIP (block[5] + dest[5]);
	    dest[6] = CLIP (block[6] + dest[6]);
	    dest[7] = CLIP (block[7] + dest[7]);

	    ((int32_t *)block)[0] = 0;	((int32_t *)block)[1] = 0;
	    ((int32_t *)block)[2] = 0;	((int32_t *)block)[3] = 0;

	    dest += stride;
	    block += 8;
	} while (--i);
    } else {
	int DC;

	DC = (block[0] + 64) >> 7;
	block[0] = block[63] = 0;
	i = 8;
	do {
	    dest[0] = CLIP (DC + dest[0]);
	    dest[1] = CLIP (DC + dest[1]);
	    dest[2] = CLIP (DC + dest[2]);
	    dest[3] = CLIP (DC + dest[3]);
	    dest[4] = CLIP (DC + dest[4]);
	    dest[5] = CLIP (DC + dest[5]);
	    dest[6] = CLIP (DC + dest[6]);
	    dest[7] = CLIP (DC + dest[7]);
	    dest += stride;
	} while (--i);
    }
}

void mpeg2_idct_init (uint32_t accel)
{
#ifdef ARCH_X86
    if (accel & MPEG2_ACCEL_X86_SSE2) {
	mpeg2_idct_copy = mpeg2_idct_copy_sse2;
	mpeg2_idct_add = mpeg2_idct_add_sse2;
	mpeg2_idct_mmx_init ();
    } else if (accel & MPEG2_ACCEL_X86_MMXEXT) {
	mpeg2_idct_copy = mpeg2_idct_copy_mmxext;
	mpeg2_idct_add = mpeg2_idct_add_mmxext;
	mpeg2_idct_mmx_init ();
    } else if (accel & MPEG2_ACCEL_X86_MMX) {
	mpeg2_idct_copy = mpeg2_idct_copy_mmx;
	mpeg2_idct_add = mpeg2_idct_add_mmx;
	mpeg2_idct_mmx_init ();
    } else
#endif
#ifdef ARCH_PPC
    if (accel & MPEG2_ACCEL_PPC_ALTIVEC) {
	mpeg2_idct_copy = mpeg2_idct_copy_altivec;
	mpeg2_idct_add = mpeg2_idct_add_altivec;
	mpeg2_idct_altivec_init ();
    } else
#endif
#ifdef ARCH_ALPHA
    if (accel & MPEG2_ACCEL_ALPHA_MVI) {
	mpeg2_idct_copy = mpeg2_idct_copy_mvi;
	mpeg2_idct_add = mpeg2_idct_add_mvi;
	mpeg2_idct_alpha_init ();
    } else if (accel & MPEG2_ACCEL_ALPHA) {
	int i;

	mpeg2_idct_copy = mpeg2_idct_copy_alpha;
	mpeg2_idct_add = mpeg2_idct_add_alpha;
	mpeg2_idct_alpha_init ();
	for (i = -3840; i < 3840 + 256; i++)
	    CLIP(i) = (i < 0) ? 0 : ((i > 255) ? 255 : i);
    } else
#endif
    {
	int i, j;

	mpeg2_idct_copy = mpeg2_idct_copy_c;
	mpeg2_idct_add = mpeg2_idct_add_c;
	for (i = -3840; i < 3840 + 256; i++)
	    CLIP(i) = (i < 0) ? 0 : ((i > 255) ? 255 : i);
	for (i = 0; i < 64; i++) {
	    j = mpeg2_scan_norm[i];
	    mpeg2_scan_norm[i] = ((j & 0x36) >> 1) | ((j & 0x09) << 2);
	    j = mpeg2_scan_alt[i];
	    mpeg2_scan_alt[i] = ((j & 0x36) >> 1) | ((j & 0x09) << 2);
	}
    }
}
