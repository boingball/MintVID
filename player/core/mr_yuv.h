/* MintVID - shared integer YUV conversion helpers. */
#ifndef MR_YUV_H
#define MR_YUV_H

#include <stdint.h>

typedef void (*mr_yuv_service_fn)(void *opaque);

/* Convert planar limited-range YUV420 to packed RGB24. U and V are sampled
 * once for each horizontal pair, matching 4:2:0 chroma geometry. Strides may
 * include padding. The optional service hook runs after every 16 output rows. */
void mr_yuv420_to_rgb24(uint8_t *dst, int dst_stride,
                        const uint8_t *y_plane, int y_stride,
                        const uint8_t *u_plane, int u_stride,
                        const uint8_t *v_plane, int v_stride,
                        int width, int height,
                        mr_yuv_service_fn service, void *service_opaque);

/* Same conversion and service contract, but packed as B,G,R bytes.  This is
 * useful for native 24-bit Picasso96 screens (RGBFB_B8G8R8): producing BGR at
 * queue time avoids a full per-pixel RGB->BGR shuffle during every blit. */
void mr_yuv420_to_bgr24(uint8_t *dst, int dst_stride,
                        const uint8_t *y_plane, int y_stride,
                        const uint8_t *u_plane, int u_stride,
                        const uint8_t *v_plane, int v_stride,
                        int width, int height,
                        mr_yuv_service_fn service, void *service_opaque);

/* Repack planar 4:2:0 as the packed 4:2:2 byte layout used by RiVA's
 * Picasso96 PIP path: Y0,V0,Y1,U0 for each horizontal pixel pair - real
 * Voodoo3/P96 2.x hardware was confirmed to expect chroma in that order,
 * the opposite of what the RGBFB_Y4U2V2 name and libraries/Picasso96.h's
 * own doc comment suggest. Y/Cb/Cr are also rescaled from the decoder's
 * own studio (limited) range to near-full range (1..254, not the literal
 * 0..255 PC/JPEG span).  Luma is additionally capped against the pair's
 * positive BT.601 chroma contribution: old P96/Voodoo overlay paths can
 * wrap a matrix result above 255 to black even though the individual Y/U/V
 * bytes are below 255.  The cap leaves all in-gamut pixels unchanged.  See
 * mr_yuv.c for the real-hardware history. Hardware PIP surfaces are pair-
 * based, so width must be even. Returns non-zero on success. */
int mr_yuv420_to_y4u2v2(uint8_t *dst, int dst_stride,
                        const uint8_t *y_plane, int y_stride,
                        const uint8_t *u_plane, int u_stride,
                        const uint8_t *v_plane, int v_stride,
                        int width, int height,
                        mr_yuv_service_fn service, void *service_opaque);

/* Inverse of mr_yuv420_to_y4u2v2(): unpack the same swapped-chroma,
 * near-full-range packed 4:2:2 layout back to RGB24. Used only as a
 * software fallback when a display backend that doesn't support
 * show_yuv422 has to take over from one that did (see display.c's
 * switch_to_cgx_fallback()) - the decode side's YUV output choice, made
 * once at startup, does not need to be undone just because the display
 * backend changed underneath it. Each row is self-contained (genuine
 * 4:2:2, unlike planar 4:2:0), so no chroma-row bookkeeping is needed.
 * This is pure software with no hardware-extremes concern of its own, so
 * it decodes with the plain 0..255 full-range matrix.  It is not a bit-
 * exact inverse where the encode-side overlay safety cap had to modify an
 * out-of-gamut highlight, which is fine for this fallback's correctness-
 * not-performance purpose.
 * Returns non-zero on success. */
int mr_y4u2v2_to_rgb24(uint8_t *dst, int dst_stride,
                       const uint8_t *src, int src_stride,
                       int width, int height);

#endif /* MR_YUV_H */
