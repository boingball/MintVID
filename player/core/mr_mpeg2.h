/*
 * MintVID - MPEG-1/MPEG-2 video decoder adapter using libmpeg2.
 */
#ifndef MR_MPEG2_H
#define MR_MPEG2_H

#include "mr_codec.h"

extern const mr_codec mr_codec_mpeg2;

typedef void (*mr_mpeg2_service_fn)(void *opaque);
/* Called periodically during decode/convert (once per completed picture
 * inside pump()'s parse loop, and threaded into mr_yuv420_to_rgb24() the same
 * way mr_h264_set_service() threads it into libavc's own RGB conversion) so
 * the caller can keep feeding Paula's software FIFO during a run of work
 * this adapter cannot otherwise yield out of. Matters more here than it
 * might look: a single demuxed packet can hand pump() more than one
 * complete picture (common for small/low-bitrate MPEG-1 GOPs), and every
 * one of them gets converted to RGB24 before pump() ever returns control to
 * the caller's own scheduler loop - with no service callback, that whole
 * run happens with Paula unfed. NULL disables (the default). */
void mr_mpeg2_set_service(mr_decoder *dec, mr_mpeg2_service_fn fn,
                          void *opaque);

/* Emit MR_PIX_YUV420P planes rather than converting each displayed picture to
 * RGB24. Worth taking whenever the caller dithers to palette indices anyway
 * (display_supports_yuv_indexed()): on real m68k the RGB24 conversion is about
 * a third of this adapter's decode time, and the RGB->indexed pass that
 * follows it costs about as much again.
 *
 * Call it immediately after open, as the H.264 path does with
 * mr_h264_set_yuv_output(); it discards anything already queued. A no-op on a
 * decoder that is not this codec, so callers need not test first. */
void mr_mpeg2_set_yuv_output(mr_decoder *dec, int enabled);

/* Preserve PES timestamps across libmpeg2 display reordering. */
void mr_mpeg2_set_input_pts(mr_decoder *dec, int has_pts, uint64_t pts_us);
int mr_mpeg2_output_pts(mr_decoder *dec, uint64_t *pts_us);

#endif /* MR_MPEG2_H */
