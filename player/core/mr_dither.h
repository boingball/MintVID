/*
 * MintVID - RGB24 -> indexed palette conversion with ordered dithering.
 *
 * Native Amiga displays are planar, so frames are quantised to one-byte
 * indices for 4-, 5- or 8-plane output. A fixed RGB cube plus 4x4
 * Bayer ordered dithering trades per-pixel error for the absence of banding,
 * is deterministic and cheap (no error-diffusion state), and needs no per-frame
 * palette work. Portable and host-testable; the AGA backend feeds the result to
 * graphics.library WritePixelArray8 for the chunky->planar step.
 */
#ifndef MR_DITHER_H
#define MR_DITHER_H

#include "mr_types.h"

#define MR_PALETTE_COLORS 216   /* 6*6*6 RGB cube; indices 216..255 unused */

/* Fill pal (256*3 bytes) with the fixed 8-plane palette as RGB triplets. */
void mr_dither_palette(uint8_t *pal);

/* Native-planar indexed palettes: depth 4 is a 2x4x2 RGB cube, depth 5 is
 * 4x4x2, and depth 8 is the established 6x6x6 cube. Unused entries are zero. */
void mr_dither_palette_indexed(uint8_t *pal, int depth);

/* Convert an RGB24 top-down frame to 8-bit palette indices with ordered
 * dithering. `out` has out_stride bytes per row. `y_base` is the absolute row
 * index of the first row (so the Bayer pattern stays aligned when encoding only
 * a sub-range of rows); pass 0 for a whole frame. */
void mr_dither_rgb8(const uint8_t *rgb, int w, int h, int rgb_stride,
                    uint8_t *out, int out_stride, int y_base);

/* The same ordered-dither engine for 4-, 5- or 8-plane indexed output. On
 * m68k all three depths use the hand-ASM LUT loop; depth changes only the
 * precomputed table contents, not the hot pixel loop. */
void mr_dither_rgb_indexed(const uint8_t *rgb, int w, int h, int rgb_stride,
                           uint8_t *out, int out_stride, int y_base,
                           int depth);

/* Quantise one RGB pixel at absolute Bayer coordinates (x,y). This is the
 * scalar equivalent of one mr_dither_rgb_indexed() output byte and lets a
 * block codec precompute its small reusable tiles when a codebook changes,
 * instead of running the full-frame dither loop after every decoded frame. */
uint8_t mr_dither_rgb_indexed_pixel(uint8_t r, uint8_t g, uint8_t b,
                                    int x, int y, int depth);

#endif /* MR_DITHER_H */
