/*
 * MintVID - oracle for tests/mr_mpeg2_idct_check.c: libmpeg2 0.5.1's
 * portable C IDCT (libmpeg2/idct.c) exactly as imported, before MintVID's
 * sparse-block shortcuts, with its symbols renamed. GPL-2.0-or-later:
 *
 * Copyright (C) 2000-2003 Michel Lespinasse <walken@zoy.org>
 * Copyright (C) 1999-2000 Aaron Holtzman <aholtzma@ess.engr.uvic.ca>
 *
 * This file is part of mpeg2dec, a free MPEG-2 video stream decoder.
 * mpeg2dec is free software; you can redistribute it and/or modify it under
 * the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 */
#ifndef MR_MPEG2_IDCT_REFERENCE_H
#define MR_MPEG2_IDCT_REFERENCE_H

#define REF_W1 2841 /* 2048 * sqrt (2) * cos (1 * pi / 16) */
#define REF_W2 2676 /* 2048 * sqrt (2) * cos (2 * pi / 16) */
#define REF_W3 2408 /* 2048 * sqrt (2) * cos (3 * pi / 16) */
#define REF_W5 1609 /* 2048 * sqrt (2) * cos (5 * pi / 16) */
#define REF_W6 1108 /* 2048 * sqrt (2) * cos (6 * pi / 16) */
#define REF_W7 565  /* 2048 * sqrt (2) * cos (7 * pi / 16) */


/*
 * In legal streams, the IDCT output should be between -384 and +384.
 * In corrupted streams, it is possible to force the IDCT output to go
 * to +-3826 - this is the worst case for a column IDCT where the
 * column inputs are 16-bit values.
 */
static uint8_t ref_clip[3840 * 2 + 256];
#define REF_CLIP(i) ((ref_clip + 3840)[i])

#if 0
#define REF_BUTTERFLY(t0,t1,W0,REF_W1,d0,d1)	\
do {					\
    t0 = W0 * d0 + REF_W1 * d1;		\
    t1 = W0 * d1 - REF_W1 * d0;		\
} while (0)
#else
#define REF_BUTTERFLY(t0,t1,W0,REF_W1,d0,d1)	\
do {					\
    int tmp = W0 * (d0 + d1);		\
    t0 = tmp + (REF_W1 - W0) * d1;		\
    t1 = tmp - (REF_W1 + W0) * d0;		\
} while (0)
#endif

static inline void ref_idct_row (int16_t * const block)
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
    REF_BUTTERFLY (t2, t3, REF_W6, REF_W2, d3, d1);
    a0 = t0 + t2;
    a1 = t1 + t3;
    a2 = t1 - t3;
    a3 = t0 - t2;

    d0 = block[4];
    d1 = block[5];
    d2 = block[6];
    d3 = block[7];
    REF_BUTTERFLY (t0, t1, REF_W7, REF_W1, d3, d0);
    REF_BUTTERFLY (t2, t3, REF_W3, REF_W5, d1, d2);
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

static inline void ref_idct_col (int16_t * const block)
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
    REF_BUTTERFLY (t2, t3, REF_W6, REF_W2, d3, d1);
    a0 = t0 + t2;
    a1 = t1 + t3;
    a2 = t1 - t3;
    a3 = t0 - t2;

    d0 = block[8*4];
    d1 = block[8*5];
    d2 = block[8*6];
    d3 = block[8*7];
    REF_BUTTERFLY (t0, t1, REF_W7, REF_W1, d3, d0);
    REF_BUTTERFLY (t2, t3, REF_W3, REF_W5, d1, d2);
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

static void ref_idct_copy (int16_t * block, uint8_t * dest,
			       const int stride)
{
    int i;

    for (i = 0; i < 8; i++)
	ref_idct_row (block + 8 * i);
    for (i = 0; i < 8; i++)
	ref_idct_col (block + i);
    do {
	dest[0] = REF_CLIP (block[0]);
	dest[1] = REF_CLIP (block[1]);
	dest[2] = REF_CLIP (block[2]);
	dest[3] = REF_CLIP (block[3]);
	dest[4] = REF_CLIP (block[4]);
	dest[5] = REF_CLIP (block[5]);
	dest[6] = REF_CLIP (block[6]);
	dest[7] = REF_CLIP (block[7]);

	((int32_t *)block)[0] = 0;	((int32_t *)block)[1] = 0;
	((int32_t *)block)[2] = 0;	((int32_t *)block)[3] = 0;

	dest += stride;
	block += 8;
    } while (--i);
}

static void ref_idct_add (const int last, int16_t * block,
			      uint8_t * dest, const int stride)
{
    int i;

    if (last != 129 || (block[0] & (7 << 4)) == (4 << 4)) {
	for (i = 0; i < 8; i++)
	    ref_idct_row (block + 8 * i);
	for (i = 0; i < 8; i++)
	    ref_idct_col (block + i);
	do {
	    dest[0] = REF_CLIP (block[0] + dest[0]);
	    dest[1] = REF_CLIP (block[1] + dest[1]);
	    dest[2] = REF_CLIP (block[2] + dest[2]);
	    dest[3] = REF_CLIP (block[3] + dest[3]);
	    dest[4] = REF_CLIP (block[4] + dest[4]);
	    dest[5] = REF_CLIP (block[5] + dest[5]);
	    dest[6] = REF_CLIP (block[6] + dest[6]);
	    dest[7] = REF_CLIP (block[7] + dest[7]);

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
	    dest[0] = REF_CLIP (DC + dest[0]);
	    dest[1] = REF_CLIP (DC + dest[1]);
	    dest[2] = REF_CLIP (DC + dest[2]);
	    dest[3] = REF_CLIP (DC + dest[3]);
	    dest[4] = REF_CLIP (DC + dest[4]);
	    dest[5] = REF_CLIP (DC + dest[5]);
	    dest[6] = REF_CLIP (DC + dest[6]);
	    dest[7] = REF_CLIP (DC + dest[7]);
	    dest += stride;
	} while (--i);
    }
}

static void ref_idct_init (void)
{
    int i;
    for (i = -3840; i < 3840 + 256; i++)
	REF_CLIP (i) = (i < 0) ? 0 : ((i > 255) ? 255 : i);
}

#endif /* MR_MPEG2_IDCT_REFERENCE_H */
