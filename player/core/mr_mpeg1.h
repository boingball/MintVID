/*
 * MintVID - MPEG-1 program-stream source (wraps pl_mpeg).
 *
 * MPEG-1 .mpg is a self-contained stream (its own demux + video + MP2 audio),
 * so it does not fit the mr_demux -> mr_codec split; it gets this small source
 * wrapper instead. pl_mpeg (Dominic Szablewski, MIT) does the heavy lifting;
 * this exposes just what the player/harness need: open a memory buffer, pull
 * decoded RGB frames, rewind, close. (MP2 audio comes later.)
 */
#ifndef MR_MPEG1_H
#define MR_MPEG1_H

#include "mr_types.h"

typedef struct mr_mpeg1 mr_mpeg1;

/* True only for an MPEG program stream containing MPEG-1 video. */
int        mr_mpeg1_probe(const uint8_t *buf, size_t len);

/* True for an MPEG program stream whose video has an MPEG-2 sequence
 * extension. Such streams use the MPEG-PS demuxer and libmpeg2 rather than
 * pl_mpeg, even when the accompanying audio happens to be MP2. */
int        mr_mpeg2_ps_probe(const uint8_t *buf, size_t len);

/* Open over a borrowed buffer (must outlive the source). NULL on failure.
 * low_rate halves the effective audio rate mr_mpeg1_samplerate() reports
 * again, on top of the always-on >28kHz halving (mirrors
 * audio/mr_audio_decode.c's compute_stride() and mr_audio_decoder_open()'s
 * own low_rate - this MP2 path is a separate, self-contained source that
 * doesn't route through that adapter, so it needs its own copy of the same
 * --audio-rate=low policy). no_audio skips demuxing/decoding the MP2 track
 * entirely (mr_mpeg1_samplerate() then reports 0, same as a video-only
 * stream, so callers need no separate no_audio check of their own).
 * mono decodes only the first channel: the Layer II bitstream still has to be
 * parsed in full, but the polyphase synthesis - the dominant cost of MP2
 * decode - runs once per frame instead of twice, and mr_mpeg1_audio() then
 * emits one channel per sample frame (mr_mpeg1_channels()). */
mr_mpeg1  *mr_mpeg1_open(const uint8_t *buf, size_t len, int low_rate,
                         int no_audio, int mono);

int        mr_mpeg1_width(mr_mpeg1 *m);
int        mr_mpeg1_height(mr_mpeg1 *m);
unsigned   mr_mpeg1_framerate_millihz(mr_mpeg1 *m);

/* Audio: the effective output sample rate (0 = no audio track). MP2 is decoded
 * as stereo (mono when mr_mpeg1_open() was given mono); the rate is halved
 * internally if the stream is above Paula's reach (~28 kHz), so this is the
 * rate to open the audio backend with. */
unsigned   mr_mpeg1_samplerate(mr_mpeg1 *m);

/* Channels per sample frame in mr_mpeg1_audio()'s output: 2 normally, 1 in
 * mono mode. Open the audio backend with this. */
int        mr_mpeg1_channels(mr_mpeg1 *m);

/* Decode the next video frame into `out` (RGB24, owned by the source); *pts (if
 * non-NULL) gets its presentation time in microseconds. Returns 1 on a frame, 0 at
 * end of stream. */
int        mr_mpeg1_next(mr_mpeg1 *m, mr_frame *out, int64_t *pts_us);

/* As mr_mpeg1_next(), but hands back the decoder's own YUV420P planes instead
 * of converting to RGB24. For a caller that is going to dither to palette
 * indices anyway (the display_supports_yuv_indexed() route), the RGB24 buffer
 * is pure overhead: on real m68k the conversion alone is ~21% of decode, and
 * the RGB->indexed pass that follows it costs about as much again.
 *
 * The returned planes are borrowed from the decoder and are valid only until
 * the next mr_mpeg1_next*() or mr_mpeg1_rewind() call. */
int        mr_mpeg1_next_yuv(mr_mpeg1 *m, mr_frame *out, int64_t *pts_us);

/* Decode one audio frame into `dst` as little-endian signed-16 interleaved
 * bytes, mr_mpeg1_channels() per sample frame (room for 1152*4 bytes needed;
 * explicit LE so it is correct on the big-endian 68k). Returns the output
 * sample-frame count, or 0 if none is available right now. */
int        mr_mpeg1_audio(mr_mpeg1 *m, unsigned char *dst);

void       mr_mpeg1_rewind(mr_mpeg1 *m);
void       mr_mpeg1_close(mr_mpeg1 *m);

#endif /* MR_MPEG1_H */
