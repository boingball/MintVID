/*
 * MintVID - mr_audio.h front end: picks the output backend at open and
 * dispatches every later call to it (see audio_backend.h). The NULL checks and
 * the values returned for a NULL session live here, once, so the player can keep
 * passing a NULL audio pointer when a file has no sound.
 */
#include "audio_backend.h"
#include "../core/mr_muldiv64.h"

#include <exec/types.h>
#include <devices/timer.h>
#include <proto/timer.h>

#include <string.h>
#include <time.h>

extern struct Device *TimerBase;

/* See mr_audio.h's audio_set_timing_mode() comment. */
int audio_want_time = 0;
void audio_set_timing_mode(int on) { audio_want_time = on ? 1 : 0; }

uint64_t audio_now_us(void)
{
    struct EClockVal value;
    ULONG frequency = TimerBase ? ReadEClock(&value) : 0;
    uint64_t ticks = ((uint64_t)value.ev_hi << 32) | value.ev_lo;
    /* ticks grows for the whole session and frequency is only known at
     * runtime, so plain `ticks * 1000000ULL / frequency` is a 64-bit
     * multiply-then-divide by a runtime value on every call - GCC always
     * routes that through libgcc's __muldi3/__udivdi3 on m68k, on every CPU
     * tier (see core/mr_muldiv64.h). This is called at least once per
     * audio_elapsed_us()/audio_buffered_ms()/audio_diagnostics() call, i.e.
     * on essentially every displayed video frame while audio is playing -
     * frequency (ReadEClock's tick rate, a few hundred kHz on real Amiga
     * hardware) comfortably fits mr_u64_div_u24's 2^24 bound. The clock()
     * fallback only runs when timer.device is unavailable and divides by
     * the compile-time constant CLOCKS_PER_SEC, which the compiler can
     * already fold into a cheap reciprocal - left alone. */
    return frequency ? mr_u64_div_u24(mr_u64_mul_u32(ticks, 1000000u), frequency) :
           (uint64_t)clock() * 1000000ULL / CLOCKS_PER_SEC;
}

mr_audio *audio_open(mr_audio_output out, unsigned rate, int channels,
                     int bits)
{
    if (out >= MR_AUDIO_OUT_AHI0 &&
        out < MR_AUDIO_OUT_AHI0 + MR_AUDIO_OUT_AHI_UNITS)
        return ahi_audio_open((int)(out - MR_AUDIO_OUT_AHI0), rate, channels,
                              bits);
    return paula_audio_open(rate, channels, bits);
}

const char *audio_name(mr_audio *a)
{
    return a ? a->ops->name(a) : "no audio";
}

void audio_close(mr_audio *a)
{
    if (a) a->ops->close(a);
}

void audio_write(mr_audio *a, const unsigned char *pcm, unsigned bytes)
{
    if (a && pcm) a->ops->write(a, pcm, bytes);
}

void audio_write_s16(mr_audio *a, const short *pcm,
                     unsigned frames, int channels)
{
    if (a && pcm && channels >= 1) a->ops->write_s16(a, pcm, frames, channels);
}

void audio_service(mr_audio *a)
{
    /* Compatibility no-op. PCM writes and device completions signal each
     * backend's worker directly; video scheduling is no longer part of output
     * upkeep. */
    (void)a;
}

uint64_t audio_elapsed_us(mr_audio *a)
{
    return a ? a->ops->elapsed_us(a) : 0;
}

unsigned long audio_elapsed_ms(mr_audio *a)
{
    return (unsigned long)(audio_elapsed_us(a) / 1000ULL);
}

int audio_starved(mr_audio *a)
{
    return a ? a->ops->starved(a) : 1;
}

unsigned long audio_buffered_ms(mr_audio *a)
{
    return a ? a->ops->buffered_ms(a) : 0;
}

unsigned long audio_capacity_ms(mr_audio *a)
{
    return a ? a->ops->capacity_ms(a) : 0;
}

void audio_diagnostics(mr_audio *a, mr_audio_diagnostics *diag)
{
    if (!diag) return;
    if (!a) { memset(diag, 0, sizeof *diag); return; }
    a->ops->diagnostics(a, diag);
}

int audio_active_requests(mr_audio *a)
{
    return a ? a->ops->active_requests(a) : 0;
}

void audio_set_running(mr_audio *a, int running)
{
    if (a) a->ops->set_running(a, running);
}

void audio_set_volume(mr_audio *a, int volume)
{
    if (a) a->ops->set_volume(a, volume);
}

void audio_flush(mr_audio *a)
{
    if (a) a->ops->flush(a);
}
