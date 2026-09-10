/*
 * MintVID - Amiga audio output (abstract).
 *
 * A compact Paula (audio.device) PCM sink, modelled on MintAMP's proven
 * streaming/teardown patterns but self-contained. The player feeds it source
 * PCM (as the demuxer delivers it); the backend converts to Paula's signed
 * 8-bit mono and double-buffers it out. Its played-milliseconds counter is the
 * A/V master clock.
 *
 * MP2, MP3 and AAC decode to PCM before feeding this same sink.  MP3/AAC use
 * MintAMP/libhelix through the packet adapter in player/audio/.
 */
#ifndef MR_AUDIO_H
#define MR_AUDIO_H

#include <stdint.h>

typedef struct mr_audio mr_audio;
enum mr_audio_request_state {
    AUDIO_REQ_IDLE = 0,
    AUDIO_REQ_QUEUED,
    AUDIO_REQ_PLAYING,
    AUDIO_REQ_COMPLETE
};
typedef struct mr_audio_diagnostics {
    unsigned long hardware_starvations;
    unsigned long minimum_buffered_ms;
    unsigned long minimum_active_ms;
    unsigned long longest_service_gap_ms;
    unsigned long longest_no_active_ms;
    unsigned long fifo_samples;
    unsigned long fifo_dropped_samples;
    unsigned long request_samples[2];
    unsigned char request_state[2]; /* 0 idle, 1 active, 2 completed */
    unsigned char active_requests;
    uint64_t audio_clock_us;
    unsigned long fifo_buffered_ms;
    unsigned long hardware_playing_remaining_ms;
    unsigned long hardware_queued_ms;
    unsigned long total_buffered_ms;
    uint64_t clock_largest_step_us;
    uint64_t startup_clock_largest_step_us;
    uint64_t oldest_request_sequence;
    unsigned char request_timeline_state[2]; /* enum mr_audio_request_state */
    unsigned char timeline_covered;
    uint64_t input_sample_frames;
    uint64_t output_samples_queued;
    uint64_t completed_samples;
} mr_audio_diagnostics;

/* RIFF/WAVE PCM with 8 bits per sample is unsigned, centred at 0x80. */
static inline short audio_pcm_u8_to_s16(unsigned char sample)
{
    return (short)(((int)sample - 128) * 256);
}

/* Open Paula output for source PCM of this rate/channels/bits (8 or 16).
 * Returns NULL if unsupported or on failure. */
mr_audio     *audio_open(unsigned rate, int channels, int bits);

/* Convert and enqueue a source-PCM buffer (a demuxer audio packet). */
void          audio_write(mr_audio *a, const unsigned char *pcm, unsigned bytes);

/* Enqueue native-endian signed 16-bit PCM produced by MintAMP/Helix. */
void          audio_write_s16(mr_audio *a, const short *pcm,
                              unsigned frames, int channels);

/* Pump the double buffer: reap finished writes, submit new ones from the FIFO.
 * Must be called frequently (the player calls it while pacing video). */
void          audio_service(mr_audio *a);

/* Master clock: milliseconds of audio actually played so far. */
unsigned long audio_elapsed_ms(mr_audio *a);
uint64_t      audio_elapsed_us(mr_audio *a);

/* True when the FIFO is empty and nothing is in flight (underrun) - the player
 * uses this to avoid stalling video on an audio clock that has stopped. */
int           audio_starved(mr_audio *a);

/* PCM queued in the software FIFO plus audio.device writes in flight. */
unsigned long audio_buffered_ms(mr_audio *a);
void          audio_diagnostics(mr_audio *a, mr_audio_diagnostics *diag);
int           audio_active_requests(mr_audio *a);
void          audio_set_running(mr_audio *a, int running);
void          audio_set_volume(mr_audio *a, int volume);

/* Drop everything queued in the software FIFO. Used by the live-resync path to
 * throw away stale buffered audio after skipping the backlog; already-submitted
 * hardware buffers still play out (a few ms). The elapsed clock is untouched, so
 * the caller rebases the media clock afterwards. */
void          audio_flush(mr_audio *a);

void          audio_close(mr_audio *a);

/* Off by default: audio_pump() (the worker task's per-wake service routine)
 * calls clock() - a second, separate clock domain from the ReadEClock()-
 * based audio_now_us() it already reads for real request scheduling -
 * solely to feed longest_service_gap/longest_no_active, read only by the
 * --time "audio diagnostics:" report. Skip that second clock() call and its
 * bookkeeping on an ordinary run; see display_set_timing_mode() for the
 * same pattern on the display side. */
void          audio_set_timing_mode(int on);

#endif /* MR_AUDIO_H */
