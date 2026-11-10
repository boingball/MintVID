/*
 * MintVID - direct YUV420P -> indexed conversion, fused with an exact
 * integer vertical decimation.
 *
 * For AGA playback where the fitted display height is an exact integer
 * fraction of the source height (typical: a 640x360 H.264 frame on a
 * non-laced AGA screen needs 640x180, a clean 2:1 vertical fit with no
 * horizontal change - see display_aga.c's aga_open()), this replaces the
 * three-stage mr_yuv420_to_rgb24() -> mr_scale_resize_rgb24() ->
 * mr_dither_rgb_indexed() pipeline with one pass over only the rows that survive
 * the downscale, entirely skipping the intermediate RGB24 buffers (both the
 * full-resolution one and the resized one).
 */
#ifndef MR_YUV_DITHER_H
#define MR_YUV_DITHER_H

#include "mr_types.h"

/*
 * Convert width x height YUV420P (2x2-subsampled Cb/Cr) directly to 8-bit
 * palette indices (mr_dither_rgb8()'s fixed 6x6x6 cube + 4x4 Bayer ordered
 * dither), selecting one source row out of every `vscale` via the same
 * nearest-neighbour rule mr_scale_resize_rgb24() uses for an exact-fit
 * downscale: source row = vscale*out_row + vscale/2 (destination pixel
 * centres). Pass vscale=1 for no vertical scaling (every row kept).
 *
 * Precondition (caller's responsibility - not checked here): this is only
 * bit-identical to the three-stage pipeline above when the destination
 * width equals `width` (no horizontal resize - mr_scale_resize_rgb24's own
 * DDA reduces to a 1:1 column copy in exactly that case, see mr_scale.c)
 * and `vscale` evenly divides `height`.
 *
 * out has out_stride bytes per row and (height / vscale) rows of `width`
 * indices each. y_base is the absolute output row of the first row written
 * (Bayer phase, matching mr_dither_rgb8()'s y_base) - pass 0 for a full
 * frame.
 */
void mr_yuv420_dither8(const uint8_t *y_plane, int y_stride,
                       const uint8_t *u_plane, int u_stride,
                       const uint8_t *v_plane, int v_stride,
                       int width, int height, int vscale,
                       uint8_t *out, int out_stride, int y_base);

/* Depth-generic form used by native planar output. depth=4 selects the
 * 2x4x2 16-colour cube, depth=5 selects the 4x4x2 32-colour cube, and depth=8
 * retains the established 6x6x6 palette. All three dispatch through the same
 * LUT-driven m68k assembly for identity/integer vertical scaling. */
void mr_yuv420_dither_indexed(const uint8_t *y_plane, int y_stride,
                              const uint8_t *u_plane, int u_stride,
                              const uint8_t *v_plane, int v_stride,
                              int width, int height, int vscale, int depth,
                              uint8_t *out, int out_stride, int y_base);

/*
 * General nearest-neighbour resize + dither fusion: dst_w/dst_h may differ
 * from width/height in either direction (upscale or downscale, horizontal
 * or vertical or both) - not just mr_yuv420_dither8()'s exact vertical-only
 * integer downscale. Uses the same destination-pixel-centre DDA as
 * mr_scale_resize_rgb24() (source = floor(((2*dest+1)*src_size) /
 * (2*dest_size)), stepped via quotient/remainder rather than per-pixel
 * division), applied independently on each axis, so this is bit-identical
 * to mr_yuv420_to_rgb24() -> mr_scale_resize_rgb24() -> mr_dither_rgb8()
 * for any width/height -> dst_w/dst_h combination (see
 * tests/mr_yuv_dither_check.c). The Bayer dither phase is keyed by the
 * *destination* row/column (y_base+oy, ox), matching mr_dither_rgb8()'s own
 * convention of dithering the already-resized frame.
 *
 * Portable C only for now: unlike mr_yuv420_dither8()'s pair-unrolled m68k
 * asm, a horizontally-resizing DDA can't assume adjacent destination
 * columns share a chroma sample, so it has no hand-tuned asm yet.
 */
void mr_yuv420_dither8_resize(const uint8_t *y_plane, int y_stride,
                              const uint8_t *u_plane, int u_stride,
                              const uint8_t *v_plane, int v_stride,
                              int width, int height,
                              uint8_t *out, int dst_w, int dst_h,
                              int out_stride, int y_base);

void mr_yuv420_dither_indexed_resize(const uint8_t *y_plane, int y_stride,
                                     const uint8_t *u_plane, int u_stride,
                                     const uint8_t *v_plane, int v_stride,
                                     int width, int height, int depth,
                                     uint8_t *out, int dst_w, int dst_h,
                                     int out_stride, int y_base);

/* Five-plane, plane-major fused conversion. Output needs
 * (padded_width/8)*dst_h*5 bytes. padded_width is a multiple of 32;
 * visible dst_w pixels are centred in it, with zero padding. */
void mr_yuv420_fused32(const uint8_t *y_plane, int y_stride,
                       const uint8_t *u_plane, int u_stride,
                       const uint8_t *v_plane, int v_stride,
                       int width, int height, uint8_t *out,
                       int dst_w, int dst_h, int padded_width, int y_base);

#endif /* MR_YUV_DITHER_H */
