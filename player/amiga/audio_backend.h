/*
 * MintVID - audio output backends behind mr_audio.h (private to player/amiga).
 *
 * The player only ever sees mr_audio.h. audio.c implements those calls once,
 * keeps their NULL-safe defaults, and dispatches through the ops table of the
 * backend that opened the session. Every backend's state struct starts with an
 * mr_audio, so a backend casts the pointer it is handed back to its own type.
 */
#ifndef MR_AUDIO_BACKEND_H
#define MR_AUDIO_BACKEND_H

#include "mr_audio.h"

typedef struct mr_audio_ops {
    const char   *(*name)(mr_audio *a);
    void          (*close)(mr_audio *a);
    void          (*write)(mr_audio *a, const unsigned char *pcm, unsigned bytes);
    void          (*write_s16)(mr_audio *a, const short *pcm,
                               unsigned frames, int channels);
    uint64_t      (*elapsed_us)(mr_audio *a);
    int           (*starved)(mr_audio *a);
    unsigned long (*buffered_ms)(mr_audio *a);
    unsigned long (*capacity_ms)(mr_audio *a);
    void          (*diagnostics)(mr_audio *a, mr_audio_diagnostics *diag);
    int           (*active_requests)(mr_audio *a);
    void          (*set_running)(mr_audio *a, int running);
    void          (*set_volume)(mr_audio *a, int volume);
    void          (*flush)(mr_audio *a);
} mr_audio_ops;

struct mr_audio {
    const mr_audio_ops *ops;
};

/* audio_set_timing_mode()'s flag, read by every backend's worker. */
extern int audio_want_time;

/* Microseconds on the EClock timeline (clock() without timer.device), the
 * time base every backend schedules its requests on. */
uint64_t audio_now_us(void);

mr_audio *paula_audio_open(unsigned rate, int channels, int bits);
mr_audio *ahi_audio_open(int unit, unsigned rate, int channels, int bits);

#endif /* MR_AUDIO_BACKEND_H */
