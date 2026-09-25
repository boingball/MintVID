/*
 * MintVID - differential check for libmpeg2's IDCT with MintVID's
 * sparse-block shortcuts (vendor/libmpeg2/libmpeg2/idct.c).
 *
 * mpeg2_idct_copy/mpeg2_idct_add are run against the IDCT exactly as it was
 * imported from libmpeg2 0.5.1 (tests/mr_mpeg2_idct_reference.h) on blocks
 * shaped like the shortcuts' cases (only storage row 0, only rows 0 and 4,
 * DC-only, rows with a DC term of 1 that the row pass turns into zeros) and
 * on random sparse and dense blocks, across the legal dequantised range
 * -2048..2047. The destination (with a margin either side), and the block,
 * which both versions must leave all zero, have to match exactly.
 */
#include "config.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "mpeg2.h"
#include "attributes.h"
#include "mpeg2_internal.h"

#include "mr_mpeg2_idct_reference.h"

/* idct.c permutes these in mpeg2_idct_init(); header.c, which owns them,
 * is not linked here. */
uint8_t mpeg2_scan_norm[64];
uint8_t mpeg2_scan_alt[64];

/* Declared by slice.c itself, not by a header. */
extern void (* mpeg2_idct_copy) (int16_t * block, uint8_t * dest, int stride);
extern void (* mpeg2_idct_add) (int last, int16_t * block,
				uint8_t * dest, int stride);

#define STRIDE 24
#define MARGIN 8
#define DEST (8 * STRIDE + 2 * MARGIN)

static unsigned state = 0x69646374U;
static unsigned rnd (void)
{
    state = state * 1664525U + 1013904223U;
    return state >> 8;
}

static int16_t coeff (int kind)
{
    switch (kind) {
    case 0: return (int16_t) ((int) (rnd () % 4096) - 2048);
    case 1: return (rnd () & 1) ? 2047 : -2048;
    case 2: return (int16_t) ((int) (rnd () % 9) - 4);      /* tiny */
    default: return (int16_t) ((int) (rnd () % 257) - 128);
    }
}

/* Fill block in the permuted storage layout according to shape. */
static void make_block (int16_t * b, int shape, int kind)
{
    int i, r;

    memset (b, 0, 64 * sizeof *b);
    switch (shape) {
    case 0:                               /* DC only */
	b[0] = coeff (kind);
	break;
    case 1:                               /* storage row 0 only */
	for (i = 0; i < 8; i++) if (rnd () & 1) b[i] = coeff (kind);
	break;
    case 2:                               /* storage rows 0 and 4 only */
	for (i = 0; i < 8; i++) {
	    if (rnd () & 1) b[i] = coeff (kind);
	    if (rnd () & 1) b[32 + i] = coeff (kind);
	}
	break;
    case 3:                               /* row 4 only */
	for (i = 0; i < 8; i++) if (rnd () & 1) b[32 + i] = coeff (kind);
	break;
    case 4:                               /* rows whose only term is DC 1 or -1 */
	for (r = 0; r < 8; r++) if (rnd () & 1) b[8 * r] = (rnd () & 1) ? 1 : -1;
	if (rnd () & 1) b[0] = coeff (kind);
	break;
    case 5:                               /* random sparse */
	for (i = 0, r = 1 + (int) (rnd () % 6); i < r; i++)
	    b[rnd () % 64] = coeff (kind);
	break;
    default:                              /* dense */
	for (i = 0; i < 64; i++) b[i] = coeff (kind);
	break;
    }
}

int main (void)
{
    static const int lasts[] = { 129, 63, 0 };
    int16_t in[64], a[64], b[64];
    uint8_t da[DEST], db[DEST];
    int shape, kind, trial, op, li, i, fails = 0;
    long runs = 0;

    ref_idct_init ();
    mpeg2_idct_init (0);

    for (shape = 0; shape < 7; shape++)
	for (kind = 0; kind < 4; kind++)
	    for (trial = 0; trial < 400; trial++)
		for (op = 0; op < 2; op++)
		    for (li = 0; li < (op ? 3 : 1); li++) {
			make_block (in, shape, kind);
			for (i = 0; i < DEST; i++)
			    da[i] = db[i] = (uint8_t) rnd ();
			memcpy (a, in, sizeof in);
			memcpy (b, in, sizeof in);
			if (op) {
			    ref_idct_add (lasts[li], a, da + MARGIN, STRIDE);
			    mpeg2_idct_add (lasts[li], b, db + MARGIN, STRIDE);
			} else {
			    ref_idct_copy (a, da + MARGIN, STRIDE);
			    mpeg2_idct_copy (b, db + MARGIN, STRIDE);
			}
			runs++;
			if ((memcmp (da, db, DEST) || memcmp (a, b, sizeof a)) &&
			    fails++ < 10)
			    printf ("FAIL %s shape=%d kind=%d last=%d trial=%d "
				    "(%s differs)\n", op ? "add" : "copy",
				    shape, kind, op ? lasts[li] : -1, trial,
				    memcmp (da, db, DEST) ? "dest" : "block");
		    }

    if (fails) {
	printf ("libmpeg2 IDCT shortcuts: %d of %ld runs FAILED\n",
		fails, runs);
	return 1;
    }
    printf ("libmpeg2 IDCT shortcuts match the imported IDCT (%ld runs)\n",
	    runs);
    return 0;
}
