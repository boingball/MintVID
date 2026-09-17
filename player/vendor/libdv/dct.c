/* 
 *  dct.c
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

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <string.h>

#include "dct.h"
#include "weighting.h"

#if ARCH_X86 || ARCH_X86_64
#include "mmx.h"
#endif /* ARCH_X86 || ARCH_X86_64 */

/** @file
 *  @ingroup dct
 *  @brief Implementation for @link dct Discrete Cosine Transform @endlink
 */

/** @weakgroup dct Discrete Cosine Transform
 *
 *  The Discrete Cosine Transform is the basis of most popular video
 *  and image coding standards.  To oversimplify, the power of the
 *  transform is that for most video, energy gets concentrated around
 *  the lower frequency coefficients.  The human visual system is less
 *  sensitive to higher frequency components.  Because of this it is
 *  possible to achieve significant compression ratios in a hybrid
 *  coder, where by reducing the precision of higher frequency terms,
 *  causing many terms towards zero.  The result is highly amenable to
 *  losseless entropy coders such as huffman encoding.
 *
 *  @{
 */

typedef short var;

#if BRUTE_FORCE_DCT_248 || BRUTE_FORCE_DCT_88
static double KC248[8][4][4][8];
#endif /* BRUTE_FORCE_DCT_248 || BRUTE_FORCE_DCT_88 */

/* MintVID adaptation: _dv_idct_88()'s non-x86 branch used to be a brute
 * force reference IDCT running in double precision, built from a
 * KC88[x][y][h][v] = cos(pi*v*(2y+1)/16) * cos(pi*h*(2x+1)/16) table
 * computed at dv_init() time via cos() (_dv_dct_init(), below) - a real
 * crash on this target (68040/68060 FPUs trap on transcendental
 * instructions with no fpsp040.library loaded - see idct_248.c's own
 * comment for the mechanism), and floating point in the per-macroblock
 * decode hot path even where it happens not to trap. KC88 is separable
 * (KC88[x][y][h][v] = COS8[v][y]*COS8[h][x]), so the whole thing reduces
 * to the standard two-pass A^T*block*A form with A[k][n] =
 * C(k)*cos(pi*k*(2n+1)/16) - one 8x8 basis table instead of a 4096-entry
 * one, folded into a single Q14 (1<<DCT88_SCALE_BITS) fixed-point table
 * precomputed once, offline, on the host - see tools/gen_dv_tables.py for
 * the generator. _dv_idct_88() itself (below) does the multiply-
 * accumulate in int64_t and rounds on the way out; see its own comment
 * for the overflow/precision argument. */
#if (!ARCH_X86) && (!ARCH_X86_64)
#define DCT88_SCALE_BITS 14
static const int32_t dv_idct88_basis[8][8] = {
  { 5793, 5793, 5793, 5793, 5793, 5793, 5793, 5793 },
  { 8035, 6811, 4551, 1598, -1598, -4551, -6811, -8035 },
  { 7568, 3135, -3135, -7568, -7568, -3135, 3135, 7568 },
  { 6811, -1598, -8035, -4551, 4551, 8035, 1598, -6811 },
  { 5793, -5793, -5793, 5793, 5793, -5793, -5793, 5793 },
  { 4551, -8035, 1598, 6811, -6811, -1598, 8035, -4551 },
  { 3135, -7568, 7568, -3135, -3135, 7568, -7568, 3135 },
  { 1598, -4551, 6811, -8035, 8035, -6811, 4551, -1598 },
};
#endif /* (!ARCH_X86) && (!ARCH_X86_64) */

#if ARCH_X86_64
void _dv_dct_88_block_mmx_x86_64(int16_t* block);
void _dv_dct_block_mmx_x86_64_postscale_88(int16_t* block, int16_t* postscale_matrix);
void _dv_dct_block_mmx_x86_64_postscale_248(int16_t* block, int16_t* postscale_matrix);
void _dv_dct_248_block_mmx_x86_64(int16_t* block);
void _dv_dct_248_block_mmx_x86_64_post_sum(int16_t* out_block);
void _dv_idct_block_mmx_x86_64(dv_coeff_t *block);
void _dv_transpose_mmx_x86_64(short * dst);
#endif
#if ARCH_X86
void _dv_idct_block_mmx(dv_coeff_t *block);
void _dv_dct_88_block_mmx(int16_t* block);
void _dv_dct_block_mmx_postscale_88(int16_t* block, int16_t* postscale_matrix);
void _dv_dct_block_mmx_postscale_248(int16_t* block, int16_t* postscale_matrix);
void _dv_dct_248_block_mmx(int16_t* block);
void _dv_dct_248_block_mmx_post_sum(int16_t* out_block);
void _dv_transpose_mmx(short * dst);
#endif /* ARCH_X86 */

/* MintVID adaptation: this used to fill KC88[]/C[] (and, under the
 * never-defined BRUTE_FORCE_DCT_* macros, KC248[]) via cos()/sqrt() -
 * see dv_idct88_basis's own comment above for why that's a real crash on
 * this target and where its replacement lives. Nothing here is reachable
 * any more: dv_idct88_basis replaces KC88/C for _dv_idct_88() (the only
 * non-dead consumer). Kept as a callable no-op since dv.c's dv_init()
 * calls it unconditionally. */
void _dv_dct_init(void) {
}

void _dv_idct_88(dv_coeff_t *block)
{
#if ARCH_X86_64

  _dv_idct_block_mmx_x86_64(block);
  emms();

#elif ARCH_X86

  _dv_idct_block_mmx(block);
  emms();

#else /* ARCH_X86 */

  /* MintVID adaptation: fixed-point replacement for the brute-force
   * double-precision reference IDCT - see dv_idct88_basis's own comment
   * above. temp[y*8+x] = sum_v sum_h block[v*8+h] * A[v][y] * A[h][x],
   * with A[] scaled by 2^DCT88_SCALE_BITS, so each product is scaled by
   * 2^(2*DCT88_SCALE_BITS); the final shift (with round-to-nearest,
   * handled sign-correctly since >> on a negative operand is an
   * arithmetic-shift-right/floor, not a truncating divide) recovers the
   * unscaled integer result. Overflow check: block[] is at most 16 bits
   * signed, each A[][] factor at most 14 bits signed (see the table's own
   * generation - max magnitude 8035), so a single term is at most
   * 16+14+14 = 44 bits and the 64-term sum at most 44+6 = 50 bits -
   * comfortably inside int64_t (63 usable bits) with no intermediate
   * overflow, computed left-to-right in 64-bit once the first operand is
   * cast. */
  int v,h,y,x,i;
  int64_t temp[64];

  memset(temp,0,sizeof(temp));
  for (v=0;v<8;v++) {
    for (h=0;h<8;h++) {
      int32_t bvh = block[v*8+h];
      if (!bvh) continue;
      for (y=0;y<8;y++){
	int64_t partial = (int64_t)bvh * dv_idct88_basis[v][y];
	for (x=0;x<8;x++) {
	  temp[y*8+x] += partial * dv_idct88_basis[h][x];
	}
      }
    }
  }

  for (i=0;i<64;i++) {
    int64_t t = temp[i];
    int64_t half = (int64_t)1 << (2*DCT88_SCALE_BITS - 1);
    block[i] = (dv_coeff_t)(t >= 0
			     ? (t + half) >> (2*DCT88_SCALE_BITS)
			     : -(((-t) + half) >> (2*DCT88_SCALE_BITS)));
  }
#endif
}

#if BRUTE_FORCE_248

void _dv_idct_248(double *block) 
{
  int u,h,z,x,i;
  double temp[64];
  double temp2[64];
  double b,c;
  double (*in)[8][8], (*out)[8][8]; /* don't really need storage 
				       -- fixup later */

#if 0
  /* This is to identify visually where 248 blocks are... */
  for(i=0;i<64;i++) {
    block[i] = 235 - 128;
  }
  return;
#endif

  memset(temp,0,sizeof(temp));

  out = &temp;
  in = &temp2;
  
  for(z=0;z<8;z++) {
    for(x=0;x<8;x++)
      (*in)[z][x] = block[z*8+x];
  }
	
  for (x = 0; x < 8; x++) {
    for (z = 0; z < 4; z++) {
      for (u = 0; u < 4; u++) {
	for (h = 0; h < 8; h++) {
	  b = (double)(*in)[u][h];  
	  c = (double)(*in)[u+4][h];
	  (*out)[2*z][x] += C[u] * C[h] * (b + c) * KC248[x][z][u][h];
	  (*out)[2*z+1][x] += C[u] * C[h] * (b - c) * KC248[x][z][u][h];
	}                       /* for h */
      }                         /* for u */
    }                           /* for z */
  }                             /* for x */

#if 0
  for (u=0;u<4;u++) {
    for (h=0;h<8;h++) {
      for (z=0;z<4;z++) {
        for (x=0;x<8;x++) {
          b = block[u*8+h];
          c = block[(u+4)*8+h];
          temp[(2*u)*8+h] += C[h] * C[u] * (b + c) * KC248[x][z][h][u];
          temp[(2*u+1)*8+h] += C[h] * C[u] * (b - c) * KC248[x][z][h][u];
        }
      }
    }
  }
#endif

  for(z=0;z<8;z++) {
    for(x=0;x<8;x++)
      block[z*8+x] = (*out)[z][x];
  }
}


#endif /* BRUTE_FORCE_248 */

/*@}*/
