/*
 * MintVID - packet audio decode adapter.
 *
 * This is glue only: the actual MP3/AAC codecs come from MintAMP's fixed-point
 * Helix sources.  The demuxer feeds compressed packets here and receives
 * native-endian signed 16-bit PCM through the sink callback.
 */
#ifndef MR_AUDIO_DECODE_H
#define MR_AUDIO_DECODE_H

#include "../core/mr_demux.h"
#include <stdint.h>

typedef struct mr_audio_decoder mr_audio_decoder;

typedef void (*mr_audio_pcm_sink)(void *user, const int16_t *pcm,
                                  unsigned frames, unsigned channels);

/* Paula's ceiling: the max_rate the Paula output passes. */
#define MR_AUDIO_PAULA_RATE_MAX 28000U

/* max_rate is the output's ceiling: a source above it is decimated 2:1 (48kHz
 * -> 24kHz, 44.1kHz -> 22.05kHz for Paula's MR_AUDIO_PAULA_RATE_MAX). 0 means
 * no ceiling - AHI resamples any rate itself, so it gets the source's own.
 *
 * low_rate halves whatever output rate this would otherwise pick - e.g.
 * 48kHz -> 12kHz instead of 24kHz, 44.1kHz -> 11.025kHz instead of
 * 22.05kHz under Paula's ceiling. A real CPU saving on a heavily-loaded 68k (fewer samples to
 * downmix, convert to 8-bit and queue to Paula) at the cost of noticeably
 * telephone-like audio, especially for music - an explicit opt-in, not a
 * new default.
 *
 * mono collapses a stereo stream to one channel (--audio-mono). Paula output is
 * mono either way, so this is not a change of what the machine can play: it is
 * where the second channel is dropped. Normally both channels are decoded in
 * full and the Paula backend averages them per sample; in mono mode the decoder
 * is asked for one channel instead, which for MP3 (MintAMP's
 * MP3SetOutputMono()/MP3SetMonoMSSideSkip()), MP2 (pl_mpeg's
 * plm_audio_set_mono()) and AC-3 (liba52's A52_MONO downmix) skips roughly half
 * of the per-channel synthesis work. Helix AAC has no such mode, so there mono
 * only saves the decimation copy and the downmix. The audible result is the
 * left channel rather than a centre mix, except where the codec's own mono
 * output is a downmix (AC-3 always, MP3 on mid/side frames). */
mr_audio_decoder *mr_audio_decoder_open(const mr_audio_info *info,
                                        int low_rate, int mono,
                                        unsigned max_rate);
void              mr_audio_decoder_close(mr_audio_decoder *dec);

/* Feed one demuxed packet. Returns PCM sample frames produced, zero when the
 * codec needs more compressed input, or a negative value on a fatal error. */
long mr_audio_decoder_feed(mr_audio_decoder *dec,
                           const uint8_t *data, uint32_t len,
                           mr_audio_pcm_sink sink, void *sink_user);
int  mr_audio_decoder_reset(mr_audio_decoder *dec);

unsigned    mr_audio_decoder_rate(const mr_audio_decoder *dec);
unsigned    mr_audio_decoder_channels(const mr_audio_decoder *dec);
const char *mr_audio_decoder_name(const mr_audio_decoder *dec);

#endif /* MR_AUDIO_DECODE_H */
