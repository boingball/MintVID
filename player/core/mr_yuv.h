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
 * Picasso96 PIP path: Y0,U0,Y1,V0 for each horizontal pixel pair.  No colour
 * conversion is performed; each 4:2:0 chroma row is reused for both of its
 * luma rows.  Hardware PIP surfaces are pair-based, so width must be even.
 * Returns non-zero on success. */
int mr_yuv420_to_y4u2v2(uint8_t *dst, int dst_stride,
                        const uint8_t *y_plane, int y_stride,
                        const uint8_t *u_plane, int u_stride,
                        const uint8_t *v_plane, int v_stride,
                        int width, int height,
                        mr_yuv_service_fn service, void *service_opaque);

#endif /* MR_YUV_H */
