/*
 * MintVID - simple portable image scaling.
 *
 * The AGA backend uses the fixed 2x path for small clips and the arbitrary
 * nearest-neighbour path when compensating for HIRES/interlace pixel aspect.
 * Everything is integer-only and deterministic.
 */
#ifndef MR_SCALE_H
#define MR_SCALE_H

#include "mr_types.h"

/* Double an RGB24 top-down frame into a 2w x 2h destination. */
void mr_scale2x_rgb24(const uint8_t *src, int w, int h, int src_stride,
                      uint8_t *dst, int dst_stride);

/* Double an 8-bit (1 byte/pixel) frame into 2w x 2h. Used to scale the encoded
 * chunky buffer - far cheaper than scaling RGB and re-encoding. Correct for HAM
 * because a modify sets an absolute channel value and scanlines are
 * independent, so a duplicated pixel/row reconstructs the same colour. */
void mr_scale2x_u8(const uint8_t *src, int w, int h, int src_stride,
                   uint8_t *dst, int dst_stride);

/* Same doubling as mr_scale2x_u8(), width only: 2w x h out of w x h, no row
 * duplicated. For the AGA backend's copper-assisted vertical doubling
 * (display_set_copper_vdouble()), where a copper list repeats each row on
 * the real raster instead of software duplicating it here. */
void mr_scale2x_u8_horiz(const uint8_t *src, int w, int h, int src_stride,
                         uint8_t *dst, int dst_stride);

/* Integer downscale an RGB24 frame by `factor` (>=1) with a box average, so an
 * oversized clip fits a small (AGA) screen. Output is (w/factor)x(h/factor). */
void mr_scale_down_rgb24(const uint8_t *src, int w, int h, int src_stride,
                         uint8_t *dst, int dst_stride, int factor);

/* Fit a rectangle inside max_w x max_h without changing its aspect ratio or
 * enlarging it.  Dimensions are rounded to the nearest whole pixel. */
void mr_scale_fit_rect(int w, int h, int max_w, int max_h,
                       int *dst_w, int *dst_h);

/* Resize RGB24 with nearest-neighbour sampling.  The inner loop uses an integer
 * DDA: there is no floating point and no division per pixel. */
void mr_scale_resize_rgb24(const uint8_t *src, int w, int h, int src_stride,
                           uint8_t *dst, int dst_w, int dst_h, int dst_stride);

/* Same resize, restricted to destination rows [y0, y0+rows). `dst` is the
 * strip buffer itself (row-relative: its own row 0 is destination row y0),
 * not the full-size destination. Produces byte-identical rows to what
 * mr_scale_resize_rgb24() would write at [y0, y0+rows) of the full image -
 * for a caller (e.g. a bounded-height RTG blit strip) that scales and
 * transfers a downscaled/upscaled frame a few rows at a time instead of
 * holding a second full-size frame buffer. */
void mr_scale_resize_rgb24_strip(const uint8_t *src, int w, int h,
                                 int src_stride, uint8_t *dst, int dst_w,
                                 int dst_h, int dst_stride, int y0, int rows);

#endif /* MR_SCALE_H */
