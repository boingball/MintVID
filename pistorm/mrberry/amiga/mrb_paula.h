/*
 * mrberry - sound through the Amiga's own audio (Paula).
 *
 * The VideoCore shows the picture; this module decodes the soundtrack on the
 * 68k with MintVID's decoders (MintAMP AAC/MP3, pl_mpeg MP2, liba52 AC-3,
 * PCM) and plays it through MintVID's Paula backend (amiga/audio_paula.c).
 * It needs no BerryCore kernel.
 *
 * Timing: the Pi's 1-MHz system timer is the clock. The first video frame
 * sent to the VideoCore anchors it: media time start_media is due at that
 * timer value plus offset_us. Sound is held until then, and afterwards the
 * end of the audio queue is compared with where the clock will be when it
 * plays. When they drift apart, silence is added or decoded samples are
 * dropped there.
 */
#ifndef MRB_PAULA_H
#define MRB_PAULA_H

#include "../core/mr_demux.h"
#include <stdint.h>

typedef struct mrb_paula mrb_paula;

/* NULL if the track cannot be decoded or Paula cannot be opened; *why then
 * says which. offset_us delays the sound (negative: earlier). */
mrb_paula *mrb_paula_open(const mr_audio_info *ai, int low_rate, int mono,
                          int32_t offset_us, char *why, unsigned why_cap);
void        mrb_paula_close(mrb_paula *p);

/* Short text for the status line, e.g. "Amiga audio 22050 Hz". */
const char *mrb_paula_describe(const mrb_paula *p);

/* Take one audio packet (timestamps already rebased). The packet is copied.
 * Returns 0 only when out of memory. */
int  mrb_paula_feed(mrb_paula *p, const mr_packet *pkt);

/* The first video frame went out at sys_us; its media time is media_us. */
void mrb_paula_anchor(mrb_paula *p, uint32_t sys_us, uint64_t media_us);

/* Decode queued packets, start Paula on time, keep it in step. Call often. */
void mrb_paula_service(mrb_paula *p);

/* P96W70: change the sound offset while playing (negative: earlier). */
void        mrb_paula_set_offset(mrb_paula *p, int32_t offset_us);

/* Pause or continue sound and the audio clock. */
void mrb_paula_pause(mrb_paula *p, int paused);
/* v96.87: key M - mute (Paula volume 0, else 64) */
void mrb_paula_set_mute(mrb_paula *p, int mute);

/* Nothing left to decode or play. */
int  mrb_paula_idle(mrb_paula *p);

/* How many sync corrections were made (for the log). */
void mrb_paula_stats(const mrb_paula *p, unsigned long *silence_ms,
                     unsigned long *dropped_ms, unsigned long *corrections);

/* Where the 68k time went, for the log: time spent decoding and queueing
 * sound, packets decoded, Paula underruns and the fewest milliseconds that
 * were ever queued. */
void mrb_paula_timing(mrb_paula *p, unsigned long *decode_ms,
                      unsigned long *packets, unsigned long *starvations,
                      unsigned long *min_buffered_ms);

#endif
