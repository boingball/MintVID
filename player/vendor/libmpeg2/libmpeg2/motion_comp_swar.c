/*
 * motion_comp_swar.c - MintVID addition to libmpeg2
 * Copyright (C) 2026 MintVID contributors
 *
 * Portable C motion compensation that works on four pixels per 32-bit word
 * ("SIMD within a register") instead of one byte at a time. It computes the
 * same values as motion_comp.c's C functions, bit for bit
 * (tests/mr_mpeg2_mc_check.c compares every entry against mpeg2_mc_c).
 *
 * The four-way half-pel average splits each byte into its upper six and
 * lower two bits so no lane can carry into its neighbour, the technique
 * Henryk Richter's MacrosInterpol68k.m uses in RiVA (GPL-2.0-or-later):
 *
 *   (A+B+C+D+2)>>2 = A>>2 + B>>2 + C>>2 + D>>2 + ((A&3 + B&3 + C&3 + D&3 + 2)>>2)
 *
 * Changed from RiVA: written in C rather than 68k assembly, and the
 * vertical and four-way cases walk each 4-pixel column down the block so
 * every source row is loaded, and its horizontal pair summed, only once.
 *
 * This file is part of mpeg2dec, a free MPEG-2 video stream decoder.
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

#include <inttypes.h>
#include <string.h>

#include "mpeg2.h"
#include "attributes.h"
#include "mpeg2_internal.h"

/* Reference rows are at arbitrary byte offsets. memcpy compiles to a single
 * longword move on 68020+ and x86, which both allow unaligned access. The
 * byte lanes are independent, so the host's byte order does not matter. */
static inline uint32_t ld4 (const uint8_t * p)
{
    uint32_t v;
    memcpy (&v, p, 4);
    return v;
}

static inline void st4 (uint8_t * p, uint32_t v)
{
    memcpy (p, &v, 4);
}

/* (a+b+1)>>1 in every byte. */
static inline uint32_t avg2_4 (uint32_t a, uint32_t b)
{
    return (a | b) - (((a ^ b) & 0xfefefefeU) >> 1);
}

/* Horizontal pair a, b split for the four-way average: *hi gets
 * a>>2 + b>>2 (at most 126 a byte), *lo gets (a&3) + (b&3) (at most 6). */
static inline void pair4 (uint32_t a, uint32_t b, uint32_t * hi, uint32_t * lo)
{
    *hi = ((a >> 2) & 0x3f3f3f3fU) + ((b >> 2) & 0x3f3f3f3fU);
    *lo = (a & 0x03030303U) + (b & 0x03030303U);
}

/* (sum of four + 2)>>2 from two split pairs. */
static inline uint32_t avg4_4 (uint32_t hi0, uint32_t lo0,
			       uint32_t hi1, uint32_t lo1)
{
    return hi0 + hi1 + (((lo0 + lo1 + 0x02020202U) >> 2) & 0x03030303U);
}

#define OP_put(d,v) st4 (d, v)
#define OP_avg(d,v) st4 (d, avg2_4 (v, ld4 (d)))

/* One statement per 4-pixel column, spelled out: GCC does not unroll a
 * 16-wide "for (i = 0; i < 16; i += 4)" and pays an index loop per word. */
#define EACH4_16(X) X (0) X (4) X (8) X (12)
#define EACH4_8(X)  X (0) X (4)
#define COPY_put(i) OP_put (dest + i, ld4 (ref + i));
#define COPY_avg(i) OP_avg (dest + i, ld4 (ref + i));
#define HALF_X_put(i) OP_put (dest + i, avg2_4 (ld4 (ref + i), ld4 (ref + i + 1)));
#define HALF_X_avg(i) OP_avg (dest + i, avg2_4 (ld4 (ref + i), ld4 (ref + i + 1)));

#define MC_SWAR(op,w)							\
static void MC_##op##_o_##w##_swar (uint8_t * dest, const uint8_t * ref, \
				    const int stride, int height)	\
{									\
    do {								\
	EACH4_##w (COPY_##op)						\
	ref += stride;							\
	dest += stride;							\
    } while (--height);							\
}									\
static void MC_##op##_x_##w##_swar (uint8_t * dest, const uint8_t * ref, \
				    const int stride, int height)	\
{									\
    do {								\
	EACH4_##w (HALF_X_##op)						\
	ref += stride;							\
	dest += stride;							\
    } while (--height);							\
}									\
static void MC_##op##_y_##w##_swar (uint8_t * dest, const uint8_t * ref, \
				    const int stride, int height)	\
{									\
    int i;								\
    for (i = 0; i < w; i += 4) {					\
	const uint8_t * r = ref + i;					\
	uint8_t * d = dest + i;						\
	uint32_t prev = ld4 (r);					\
	int h = height;							\
	do {								\
	    uint32_t cur = ld4 (r + stride);				\
	    OP_##op (d, avg2_4 (prev, cur));				\
	    prev = cur;							\
	    r += stride;						\
	    d += stride;						\
	} while (--h);							\
    }									\
}									\
static void MC_##op##_xy_##w##_swar (uint8_t * dest, const uint8_t * ref, \
				     const int stride, int height)	\
{									\
    int i;								\
    for (i = 0; i < w; i += 4) {					\
	const uint8_t * r = ref + i;					\
	uint8_t * d = dest + i;						\
	uint32_t hi0, lo0;						\
	int h = height;							\
	pair4 (ld4 (r), ld4 (r + 1), &hi0, &lo0);			\
	do {								\
	    uint32_t hi1, lo1;						\
	    r += stride;						\
	    pair4 (ld4 (r), ld4 (r + 1), &hi1, &lo1);			\
	    OP_##op (d, avg4_4 (hi0, lo0, hi1, lo1));			\
	    hi0 = hi1;							\
	    lo0 = lo1;							\
	    d += stride;						\
	} while (--h);							\
    }									\
}

MC_SWAR (put, 16)
MC_SWAR (put, 8)
MC_SWAR (avg, 16)
MC_SWAR (avg, 8)

MPEG2_MC_EXTERN (swar)
