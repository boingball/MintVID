/*
 * MintVID - MPEG-1/MPEG-2 video decoder adapter using libmpeg2.
 */
#ifndef MR_MPEG2_H
#define MR_MPEG2_H

#include "mr_codec.h"

extern const mr_codec mr_codec_mpeg2;

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
