/*
 * MintVID - HAM8 (and HAM6) encoding for AGA near-truecolour output.
 *
 * HAM (Hold-And-Modify) gets far more than 256 on-screen colours out of a
 * planar display: each pixel either loads a fresh colour from a small base
 * palette or *modifies* one R/G/B channel of the pixel to its left. HAM8 (AGA,
 * 8 planes) modifies with 6-bit precision -> ~262k colours, near-photographic.
 *
 * The encoder is a cheap per-scanline greedy pass (4 options per pixel, no
 * search - the base palette is a regular cube), so it is portable and
 * host-testable. Output is 8-bit HAM pixel bytes fed to WritePixelArray8 on a
 * HAM screen, exactly like the 256-colour path. A decoder is provided so the
 * host harness can reconstruct RGB and measure error.
 */
#ifndef MR_HAM_H
#define MR_HAM_H

#include "mr_types.h"

/* Base "set" palette for the given HAM depth: HAM8 -> 64-entry 4x4x4 RGB cube;
 * HAM6 -> 16-entry grey ramp. pal must hold (1<<(bits-2))*3 bytes. */
void mr_ham_palette(uint8_t *pal, int bits);

/* Encode an RGB24 top-down frame to HAM8 pixel bytes (control<<6 | data6).
 * `bits` is 8 (HAM8, 6-bit modify) or 6 (HAM6, 4-bit modify). */
void mr_ham_encode(const uint8_t *rgb, int w, int h, int rgb_stride,
                   uint8_t *out, int out_stride, int bits);

/* The same, optionally with ordered dither ("HAM8 (Dither)"/"HAM6
 * (Dither)"). Only the value a pixel writes when it modifies one channel is
 * dithered: a 4x4 Bayer threshold (0..3 for HAM8's 6-bit steps, 0..15 for
 * HAM6's 4-bit ones) is added before the truncation, so over each 4x4 block
 * the written values average to the true colour instead of always rounding
 * down. Channel choice and base-colour picks are unchanged, so edges fringe
 * exactly as in the plain encoder. y_base is the row index of rgb's first
 * row in the whole picture, keeping the pattern continuous when a frame is
 * encoded in row bands. dither == 0 is bit-identical to mr_ham_encode(). */
void mr_ham_encode_ex(const uint8_t *rgb, int w, int h, int rgb_stride,
                      uint8_t *out, int out_stride, int bits, int y_base,
                      int dither);

/* Reconstruct RGB24 from HAM bytes (hardware simulation) - for validation. */
void mr_ham_decode(const uint8_t *ham, int w, int h, int in_stride,
                   const uint8_t *pal, uint8_t *rgb, int rgb_stride, int bits);

#endif /* MR_HAM_H */
