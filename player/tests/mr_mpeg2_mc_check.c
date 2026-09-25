/*
 * MintVID - differential check for libmpeg2's word-at-a-time motion
 * compensation (vendor/libmpeg2/libmpeg2/motion_comp_swar.c).
 *
 * Runs every entry of mpeg2_mc_swar (put/avg x full/half-pel x/y/xy x 16/8
 * wide) against the matching entry of libmpeg2's own byte-at-a-time
 * mpeg2_mc_c, on random and extreme pixels, over every height 1..16, several
 * strides (odd ones too) and all four reference alignments. The whole
 * destination area, including bytes either side of the block, must match.
 */
#include "config.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "mpeg2.h"
#include "attributes.h"
#include "mpeg2_internal.h"

#define PAD 32
#define MAXSTRIDE 80
#define BUF ((16 + 2) * MAXSTRIDE + 2 * PAD)

static unsigned state = 0x6d706567U;
static uint8_t random_byte (void)
{
    state = state * 1664525U + 1013904223U;
    return (uint8_t) (state >> 24);
}

static uint8_t pick (int mode)
{
    uint8_t b = random_byte ();
    if (mode == 1) return (b & 1) ? 255 : 0;       /* extremes only */
    if (mode == 2) return (uint8_t) (b & 3) | 252; /* 252..255: low-bit carries */
    return b;
}

int main (void)
{
    static const int strides[] = { 16, 23, 32, 48, 65, MAXSTRIDE };
    static const char * const xy_name[4] = { "o", "x", "y", "xy" };
    static uint8_t ref[BUF], d_ref[BUF], d_got[BUF];
    int op, fn, si, height, align, mode, trial, fails = 0;
    long runs = 0;

    for (op = 0; op < 2; op++)
	for (fn = 0; fn < 8; fn++)
	    for (si = 0; si < (int) (sizeof strides / sizeof strides[0]); si++)
		for (height = 1; height <= 16; height++)
		    for (align = 0; align < 4; align++)
			for (mode = 0; mode < 3; mode++)
			    for (trial = 0; trial < 3; trial++) {
				int stride = strides[si], i;
				mpeg2_mc_fct * want = op ? mpeg2_mc_c.avg[fn]
							 : mpeg2_mc_c.put[fn];
				mpeg2_mc_fct * got = op ? mpeg2_mc_swar.avg[fn]
							: mpeg2_mc_swar.put[fn];
				for (i = 0; i < BUF; i++)
				    ref[i] = pick (mode);
				for (i = 0; i < BUF; i++)
				    d_ref[i] = d_got[i] = pick (mode);
				want (d_ref + PAD, ref + PAD + align, stride, height);
				got (d_got + PAD, ref + PAD + align, stride, height);
				runs++;
				if (memcmp (d_ref, d_got, BUF) && fails++ < 10)
				    printf ("FAIL %s_%s_%d stride=%d height=%d "
					    "align=%d mode=%d\n",
					    op ? "avg" : "put", xy_name[fn & 3],
					    fn < 4 ? 16 : 8, stride, height,
					    align, mode);
			    }

    if (fails) {
	printf ("libmpeg2 SWAR motion compensation: %d of %ld runs FAILED\n",
		fails, runs);
	return 1;
    }
    printf ("libmpeg2 SWAR motion compensation matches mpeg2_mc_c "
	    "(%ld runs)\n", runs);
    return 0;
}
