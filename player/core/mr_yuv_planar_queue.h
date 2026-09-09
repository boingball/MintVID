/*
 * MintVID - experimental AGA YUV->planar queue mode.
 *
 * This is deliberately kept behind the alternate Makefile.fused build while
 * the real 68060 decides whether the cache/locality win is worth keeping.
 * The normal player and its public display contract remain unchanged.
 */
#ifndef MR_YUV_PLANAR_QUEUE_H
#define MR_YUV_PLANAR_QUEUE_H

/* Configure one process-wide native-planar queue.  padded_width must be a
 * multiple of 32 and >= visible_width; the representation occupies exactly
 * padded_width*height bytes (eight bitplanes, plane-major, padded_width/8
 * bytes per row).  Returns non-zero when the geometry is accepted. */
int mr_yuv_planar_queue_configure(int visible_width, int height,
                                  int padded_width);
void mr_yuv_planar_queue_disable(void);
int mr_yuv_planar_queue_is_active(void);
int mr_yuv_planar_queue_visible_width(void);
int mr_yuv_planar_queue_height(void);
int mr_yuv_planar_queue_padded_width(void);

#endif /* MR_YUV_PLANAR_QUEUE_H */
