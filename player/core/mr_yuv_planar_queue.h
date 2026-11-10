/* MintVID - negotiated plane-major decode-ahead queue.
 * Direct uses the existing eight-plane assembly kernel; Fused32 uses a
 * portable five-plane converter. Queue slots reserve padded_width*height
 * bytes, enough for either layout. */
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

int mr_yuv_planar_queue_configure32(int visible_width, int height, int padded_width);
int mr_yuv_planar_queue_depth(void);
#endif /* MR_YUV_PLANAR_QUEUE_H */
