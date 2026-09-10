/*
 * MintVID - Paula (audio.device) PCM output backend.
 *
 * One allocated Paula channel, mono, signed 8-bit, double-buffered CMD_WRITE.
 * Source PCM (8/16-bit, mono/stereo) is downmixed/converted to signed 8-bit
 * mono and pushed into a ring FIFO; audio_service() reaps completed writes and
 * resubmits from the FIFO, so the player just has to call it often.
 *
 * Teardown follows MintAMP's hard-won rule: an in-flight CMD_WRITE must be
 * AbortIO'd and WaitIO'd (reaped) before the chip buffer it points at is freed
 * or the device is closed, so Paula never DMAs freed memory.
 */
#include "mr_audio.h"
#include "mr_audio_rate.h"
#include "../core/mr_muldiv64.h"

#include <exec/types.h>
#include <exec/memory.h>
#include <exec/io.h>
#include <devices/audio.h>
#include <devices/timer.h>
#include <dos/dos.h>
#include <dos/dosextens.h>
#include <dos/dostags.h>
#include <proto/exec.h>
#include <clib/alib_protos.h>   /* BeginIO() prototype (amiga.lib helper) */
#include <proto/timer.h>
#include <proto/dos.h>

#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <limits.h>

#define PAL_CLOCK  3546895UL   /* Paula colour clock (PAL)                  */
#define MIN_PERIOD 124         /* Paula hardware minimum period             */
#define NBUF       2           /* double buffer                            */
#define PAULA_REQUEST_MS 200    /* was 100 (measured validation value) - a
                                 * WinUAE 854x480 TurboGT run still showed
                                 * hw-starvations (both NBUF chip buffers
                                 * empty at once) climbing continuously, ~1
                                 * every 100-150ms for the whole session even
                                 * after mrplay.c's skip_stale_output fix
                                 * reclaimed CPU for late frames - audible as
                                 * a persistent echo/stutter (Paula briefly
                                 * repeating the outgoing buffer's tail on a
                                 * genuine double-buffer underrun is a known
                                 * hardware behaviour, not a software double-
                                 * feed bug). Doubling the buffer gives twice
                                 * the slack against feed-rate jitter per
                                 * refill, at the cost of ~100ms more audio
                                 * latency and 2x chip RAM for these buffers
                                 * (rate * 0.1s more, e.g. ~2.2 KB at 22050 Hz
                                 * mono 8-bit) - re-tune here if a real Amiga/
                                 * WinUAE pass says otherwise. mrplay.c's
                                 * AUDIO_RESCUE_ENTRY_MS/TARGET_MS/
                                 * ONE_REQUEST_MS are defined in terms of one/
                                 * two of these requests and were doubled to
                                 * match - keep them in step with this. */

/* See mr_audio.h's audio_set_timing_mode() comment. */
static int g_audio_want_time = 0;
void audio_set_timing_mode(int on) { g_audio_want_time = on ? 1 : 0; }

typedef struct audio_request_timeline {
    uint64_t sequence;
    unsigned submitted_samples;
    uint64_t scheduled_start_us, scheduled_end_us;
    enum mr_audio_request_state state;
} audio_request_timeline;

extern struct Device *TimerBase;

struct mr_audio {
    struct MsgPort *port;
    struct IOAudio *io[NBUF];
    signed char    *chip[NBUF];
    int             bufsz;         /* samples per chip buffer               */
    int             busy[NBUF];
    int             nsub[NBUF];    /* samples submitted in that buffer      */
    audio_request_timeline request[NBUF];
    int             opened;        /* audio.device open (io[0])             */
    struct Task    *parent_task;
    struct Task    *worker_task;
    BYTE            ready_sig, stopped_sig, wake_sig;
    int             ready_ok;
    volatile int    shutdown;
    unsigned        actual_active;

    unsigned        period;
    unsigned        rate;
    unsigned        output_rate;
    mr_audio_rate_state rate_state;
    int             src_channels;
    int             src_bits;

    signed char    *fifo;          /* ring of converted 8-bit mono samples  */
    unsigned        fifo_size;
    unsigned        head, tail, count;
    unsigned long   fifo_dropped_samples;

    uint64_t        completed_samples;
    uint64_t        next_sequence, timeline_end_us, last_reported_clock_us;
    uint64_t        clock_largest_step_us;
    uint64_t        startup_clock_largest_step_us;
    clock_t         last_service;
    clock_t         longest_service_gap;
    unsigned long   hardware_starvations;
    unsigned long   minimum_buffered_ms;
    unsigned long   minimum_active_ms;
    int             had_buffered_audio;
    int             running;
    int             volume;         /* audio.device range 0..64             */
    int             ever_started;
    int             hardware_starved;
    clock_t         no_active_since;
    clock_t         longest_no_active;
};

static uint64_t audio_now_us(void)
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

static int request_active(mr_audio *a, int i)
{
    return a->busy[i] && a->request[i].state != AUDIO_REQ_COMPLETE;
}

static int active_request_count(mr_audio *a)
{
    int i, n = 0;
    for (i = 0; i < NBUF; i++) if (request_active(a, i)) n++;
    return n;
}

static int oldest_request(mr_audio *a)
{
    int i, oldest = -1;
    for (i = 0; i < NBUF; i++) {
        if (a->request[i].state == AUDIO_REQ_IDLE) continue;
        if (oldest < 0 || a->request[i].sequence < a->request[oldest].sequence)
            oldest = i;
    }
    return oldest;
}

static uint64_t request_duration_us(mr_audio *a, unsigned samples)
{
    /* a->output_rate is a Paula output rate, always well under 65536 (see
     * MIN_PERIOD above) - mr_u64_div_u16 avoids the __muldi3/__divdi3 pair
     * plain `(uint64_t)samples * 1000000ULL / a->output_rate` costs on
     * m68k (see core/mr_muldiv64.h). */
    return mr_u64_div_u16(mr_u64_mul_u32(samples, 1000000u), a->output_rate);
}

static unsigned request_estimated_played(mr_audio *a, int i, uint64_t now)
{
    audio_request_timeline *r = &a->request[i];
    uint64_t elapsed, duration;
    if (r->state == AUDIO_REQ_COMPLETE || now >= r->scheduled_end_us)
        return r->submitted_samples;
    if (now <= r->scheduled_start_us) return 0;
    elapsed = now - r->scheduled_start_us;
    duration = r->scheduled_end_us - r->scheduled_start_us;
    /* elapsed/duration are bounded by one request's span (PAULA_REQUEST_MS,
     * comfortably under mr_u64_div_u24's 2^24us~=16.7s bound) - same
     * libgcc-avoidance as request_duration_us above. */
    return duration ? (unsigned)mr_u64_div_u24(
        mr_u64_mul_u32(elapsed, r->submitted_samples), (uint32_t)duration) : 0;
}

static void audio_worker_entry(void);

/* ---- ring FIFO ---------------------------------------------------------- */

static void fifo_push(mr_audio *a, signed char s)
{
    if (a->count >= a->fifo_size) {
        a->fifo_dropped_samples++;             /* explicit bounded overflow */
        return;
    }
    a->fifo[a->head] = s;
    a->head = (a->head + 1) % a->fifo_size;
    a->count++;
}

static int fifo_pop_into(mr_audio *a, signed char *dst, int n)
{
    int i = 0;
    while (i < n && a->count) {
        dst[i++] = a->fifo[a->tail];
        a->tail = (a->tail + 1) % a->fifo_size;
        a->count--;
    }
    return i;
}

void audio_flush(mr_audio *a)
{
    if (!a) return;
    /* Same Forbid() guard the push/pop paths use, so the worker task cannot be
     * mid-pop when the ring is reset. */
    Forbid();
    a->head = a->tail = 0;
    a->count = 0;
    Permit();
}

/* ---- lifecycle ---------------------------------------------------------- */

mr_audio *audio_open(unsigned rate, int channels, int bits)
{
    mr_audio *a;
    struct Process *worker;
    struct EClockVal eclock_value;
    ULONG eclock_frequency;
    unsigned long paula_clock;

    if (rate == 0 || (bits != 8 && bits != 16) || channels < 1)
        return NULL;

    a = (mr_audio *)calloc(1, sizeof *a);
    if (!a) return NULL;
    a->ready_sig = a->stopped_sig = a->wake_sig = -1;
    a->rate = rate;
    a->src_channels = channels;
    a->src_bits = bits;
    /* Paula's colour clock is exactly five EClock ticks.  Using ReadEClock
     * also selects the right PAL/NTSC clock instead of silently drifting on
     * NTSC machines; retain the PAL value when timer.device is unavailable. */
    eclock_frequency = TimerBase ? ReadEClock(&eclock_value) : 0;
    paula_clock = eclock_frequency ? (unsigned long)eclock_frequency * 5UL
                                   : PAL_CLOCK;
    a->period = (unsigned)(paula_clock / rate);
    a->minimum_buffered_ms = ULONG_MAX;
    a->minimum_active_ms = ULONG_MAX;
    a->volume = 64;
    if (a->period < MIN_PERIOD) a->period = MIN_PERIOD;
    a->output_rate = (unsigned)(paula_clock / a->period);
    mr_audio_rate_init(&a->rate_state, rate, a->output_rate);

    /* libavc is a synchronous API and has no safe slice-yield hook. Keep two
     * genuinely submitted requests, each long enough to span the observed
     * 40-54 ms core decode calls plus scheduler overhead. */
    a->bufsz = (int)(a->output_rate * PAULA_REQUEST_MS / 1000);
    if (a->bufsz < 256) a->bufsz = 256;
    /* Several seconds of cushion: coarsely-interleaved files deliver audio in
     * multi-second chunks (e.g. CDR-Dinner's ~2.3 s), with video-only runs
     * between them. A one-second FIFO dropped the tail of each burst and then
     * starved during the video run - audible as chunky audio and stuttery
     * pacing. Buffer ~4 s so a burst fits and the video run does not starve.
     * Sync is unaffected: pacing follows samples actually played, not queued. */
    a->fifo_size = a->output_rate * 4;          /* ~4 s cushion              */
    if (a->fifo_size < 8192) a->fifo_size = 8192;
    a->fifo = (signed char *)malloc(a->fifo_size);
    if (!a->fifo) { audio_close(a); return NULL; }

    a->parent_task = FindTask(NULL);
    a->ready_sig = AllocSignal(-1);
    a->stopped_sig = AllocSignal(-1);
    if (a->ready_sig < 0 || a->stopped_sig < 0) { audio_close(a); return NULL; }
    worker = CreateNewProcTags(NP_Entry, (ULONG)audio_worker_entry,
                               NP_Name, (ULONG)"MintVID Paula output",
                               NP_Priority, 5,
                               NP_StackSize, 16384UL,
                               TAG_END);
    if (!worker) {
        audio_close(a); return NULL;
    }
    /* NP_UserData is not available in the AmigaOS 3.x NDK used by the
     * 68030 build.  Hand the context to the newly-created process through
     * the standard Task user-data field, then release its private bootstrap
     * wait.  Signals remain pending if the child has not reached Wait() yet.
     * Forbid() makes publishing the pointer and wakeup one atomic handoff. */
    Forbid();
    a->worker_task = &worker->pr_Task;
    worker->pr_Task.tc_UserData = a;
    Signal(a->worker_task, SIGBREAKF_CTRL_F);
    Permit();
    Wait(1UL << a->ready_sig);
    if (!a->ready_ok) { audio_close(a); return NULL; }

    return a;
}

void audio_close(mr_audio *a)
{
    if (!a) return;
    if (a->worker_task) {
        Forbid();
        a->shutdown = 1;
        Permit();
        if (a->wake_sig >= 0) Signal(a->worker_task, 1UL << a->wake_sig);
        Wait(1UL << a->stopped_sig);
    }
    if (a->ready_sig >= 0) FreeSignal(a->ready_sig);
    if (a->stopped_sig >= 0) FreeSignal(a->stopped_sig);
    free(a->fifo);
    free(a);
}

/* ---- streaming ---------------------------------------------------------- */

static void fifo_push_resampled(mr_audio *a, signed char sample)
{
    unsigned outputs = mr_audio_rate_outputs(&a->rate_state);
    while (outputs--) fifo_push(a, sample);
}

void audio_write(mr_audio *a, const unsigned char *pcm, unsigned bytes)
{
    int bps, ch, framebytes;
    unsigned n, k;

    if (!a || !pcm) return;
    bps = a->src_bits / 8;
    ch  = a->src_channels;
    framebytes = bps * ch;
    if (framebytes <= 0) return;
    n = bytes / (unsigned)framebytes;

    Forbid();
    for (k = 0; k < n; k++) {
        const unsigned char *p = pcm + (size_t)k * framebytes;
        int s;
        if (a->src_bits == 16) {
            int l = (int)(short)(p[0] | (p[1] << 8));      /* LE, signed     */
            if (ch >= 2) {
                int r = (int)(short)(p[2] | (p[3] << 8));
                s = (l + r) / 2;
            } else s = l;
            s >>= 8;                                        /* 16 -> 8 bit    */
        } else {
            int l = (int)audio_pcm_u8_to_s16(p[0]);
            if (ch >= 2) {
                int r = (int)audio_pcm_u8_to_s16(p[1]);
                s = (l + r) / 2;
            } else s = l;
            s >>= 8;
        }
        fifo_push_resampled(a, (signed char)s);
    }
    Permit();
    if (a->worker_task && a->wake_sig >= 0)
        Signal(a->worker_task, 1UL << a->wake_sig);
}

void audio_write_s16(mr_audio *a, const short *pcm,
                     unsigned frames, int channels)
{
    unsigned k;
    if (!a || !pcm || channels < 1) return;
    Forbid();
    for (k = 0; k < frames; k++) {
        int s = pcm[(size_t)k * channels];
        if (channels >= 2)
            s = (s + pcm[(size_t)k * channels + 1]) / 2;
        fifo_push_resampled(a, (signed char)(s >> 8));
    }
    Permit();
    if (a->worker_task && a->wake_sig >= 0)
        Signal(a->worker_task, 1UL << a->wake_sig);
}

static void audio_pump(mr_audio *a)
{
    int i;
    /* service_now/last_service/no_active_since/longest_service_gap/
     * longest_no_active only ever feed audio_diagnostics(), read solely by
     * mrplay.c's --time "audio diagnostics:"/"audio hardware:" report -
     * skip the clock() call and its bookkeeping on every ordinary wake of
     * this worker task when nobody is watching. hardware_starved/
     * hardware_starvations below are real rescue-logic inputs and stay
     * unconditional. */
    clock_t service_now = 0;
    uint64_t now;
    unsigned long buffered;
    int active_at_entry;
    if (!a) return;
    now = audio_now_us();
    if (g_audio_want_time) {
        service_now = clock();
        if (a->last_service && service_now - a->last_service > a->longest_service_gap)
            a->longest_service_gap = service_now - a->last_service;
        a->last_service = service_now;
    }
    active_at_entry = active_request_count(a);
    if (g_audio_want_time) {
        if (a->running && active_at_entry == 0) {
            if (!a->no_active_since) a->no_active_since = service_now;
        } else a->no_active_since = 0;
    }

    /* Reap finished writes. */
    for (i = 0; i < NBUF; i++) {
        if (a->busy[i] && CheckIO((struct IORequest *)a->io[i])) {
            WaitIO((struct IORequest *)a->io[i]);
            a->busy[i] = 0;
            a->request[i].state = AUDIO_REQ_COMPLETE;
        }
    }
    /* Commit completed requests strictly in submission order. A later array
     * slot completing first must never advance the master clock past an older
     * outstanding request. */
    for (;;) {
        int oldest = oldest_request(a);
        if (oldest < 0 || a->request[oldest].state != AUDIO_REQ_COMPLETE) break;
        a->completed_samples += a->request[oldest].submitted_samples;
        memset(&a->request[oldest], 0, sizeof a->request[oldest]);
    }
    {
        int oldest = oldest_request(a);
        if (oldest >= 0) a->request[oldest].state = AUDIO_REQ_PLAYING;
    }

    /* `running` is the actual Paula gate, not just a diagnostics flag. During
     * startup/rebuffer/pause keep newly decoded PCM in the software FIFO so
     * mrplay can build its requested cushion before audio.device starts
     * consuming it. Already-submitted requests are allowed to finish cleanly;
     * no new CMD_WRITE is issued until playback is resumed. */
    if (a->running) {
        /* Submit idle buffers from the FIFO. */
        for (i = 0; i < NBUF; i++) {
            if (!a->busy[i] && a->request[i].state == AUDIO_REQ_IDLE) {
                int had_outstanding = oldest_request(a) >= 0;
                int n;
                Forbid();
                n = fifo_pop_into(a, a->chip[i], a->bufsz);
                Permit();
                if (n <= 0) continue;
                a->io[i]->ioa_Request.io_Command = CMD_WRITE;
                a->io[i]->ioa_Request.io_Flags   = ADIOF_PERVOL;
                a->io[i]->ioa_Data   = (UBYTE *)a->chip[i];
                a->io[i]->ioa_Length = (ULONG)n;
                a->io[i]->ioa_Period = a->period;
                a->io[i]->ioa_Volume = (UWORD)a->volume;
                a->io[i]->ioa_Cycles = 1;
                BeginIO((struct IORequest *)a->io[i]);
                a->busy[i] = 1;
                a->nsub[i] = n;
                a->request[i].sequence = a->next_sequence++;
                a->request[i].submitted_samples = (unsigned)n;
                a->request[i].scheduled_start_us = now > a->timeline_end_us
                                                   ? now : a->timeline_end_us;
                a->request[i].scheduled_end_us =
                    a->request[i].scheduled_start_us +
                    request_duration_us(a, (unsigned)n);
                a->timeline_end_us = a->request[i].scheduled_end_us;
                a->request[i].state = had_outstanding
                                    ? AUDIO_REQ_QUEUED : AUDIO_REQ_PLAYING;
            }
        }
    }
    buffered = audio_buffered_ms(a);
    if ((buffered || a->had_buffered_audio) &&
        buffered < a->minimum_buffered_ms)
        a->minimum_buffered_ms = buffered;
    if (buffered) a->had_buffered_audio = 1;
    else if (a->had_buffered_audio) {
        a->had_buffered_audio = 0;
    }
    {
        int active = active_request_count(a);
        unsigned long active_samples = 0;
        for (i = 0; i < NBUF; i++)
            if (request_active(a, i)) active_samples += (unsigned long)a->nsub[i];
        if (a->running) {
            unsigned long active_ms = active_samples * 1000UL / a->rate;
            if (active_ms < a->minimum_active_ms)
                a->minimum_active_ms = active_ms;
        }
        if (a->running && active == 0) {
            if (!a->hardware_starved) {
                a->hardware_starvations++;
                a->hardware_starved = 1;
            }
            if (g_audio_want_time) {
                clock_t gap;
                if (!a->no_active_since) a->no_active_since = service_now;
                gap = service_now - a->no_active_since;
                if (gap > a->longest_no_active) a->longest_no_active = gap;
            }
        } else {
            a->hardware_starved = 0;
            if (g_audio_want_time) a->no_active_since = 0;
        }
    }
}

void audio_set_volume(mr_audio *a, int volume)
{
    if (!a) return;
    if (volume < 0) volume = 0;
    if (volume > 64) volume = 64;
    a->volume = volume;
}

void audio_service(mr_audio *a)
{
    /* Compatibility no-op. PCM writes and audio.device completions signal the
     * worker directly; video scheduling is no longer part of output upkeep. */
    (void)a;
}

static void audio_worker_cleanup(mr_audio *a)
{
    int i;
    for (i = 0; i < NBUF; i++) {
        if (a->busy[i] && a->io[i]) {
            AbortIO((struct IORequest *)a->io[i]);
            WaitIO((struct IORequest *)a->io[i]);
            a->busy[i] = 0;
        }
    }
    if (a->opened && a->io[0]) {
        CloseDevice((struct IORequest *)a->io[0]);
        a->opened = 0;
    }
    for (i = 0; i < NBUF; i++) {
        if (a->io[i]) DeleteIORequest((struct IORequest *)a->io[i]);
        a->io[i] = NULL;
    }
    if (a->port) { DeleteMsgPort(a->port); a->port = NULL; }
    for (i = 0; i < NBUF; i++) {
        if (a->chip[i]) FreeMem(a->chip[i], (ULONG)a->bufsz);
        a->chip[i] = NULL;
    }
    if (a->wake_sig >= 0) { FreeSignal(a->wake_sig); a->wake_sig = -1; }
}

static void audio_worker_entry(void)
{
    static UBYTE anychan[4] = { 1, 2, 4, 8 };
    struct Task *self = FindTask(NULL);
    mr_audio *a;
    int i;
    /* Parent publishes tc_UserData after CreateNewProcTags() returns. */
    Wait(SIGBREAKF_CTRL_F);
    a = (mr_audio *)self->tc_UserData;
    if (!a) return;
    a->worker_task = self;
    a->wake_sig = AllocSignal(-1);
    a->port = CreateMsgPort();
    if (a->wake_sig < 0 || !a->port) goto failed;
    a->io[0] = (struct IOAudio *)CreateIORequest(a->port, sizeof(struct IOAudio));
    if (!a->io[0]) goto failed;
    a->io[0]->ioa_Request.io_Message.mn_Node.ln_Pri = ADALLOC_MAXPREC;
    a->io[0]->ioa_Data = anychan;
    a->io[0]->ioa_Length = sizeof anychan;
    if (OpenDevice((CONST_STRPTR)"audio.device", 0,
                   (struct IORequest *)a->io[0], 0) != 0) goto failed;
    a->opened = 1;
    a->io[1] = (struct IOAudio *)CreateIORequest(a->port, sizeof(struct IOAudio));
    if (!a->io[1]) goto failed;
    {
        struct Message keep = a->io[1]->ioa_Request.io_Message;
        memcpy(a->io[1], a->io[0], sizeof(struct IOAudio));
        a->io[1]->ioa_Request.io_Message = keep;
        a->io[1]->ioa_Request.io_Message.mn_ReplyPort = a->port;
    }
    for (i = 0; i < NBUF; i++) {
        a->chip[i] = (signed char *)AllocMem((ULONG)a->bufsz,
                                             MEMF_CHIP | MEMF_CLEAR);
        if (!a->chip[i]) goto failed;
    }
    a->ready_ok = 1;
    Signal(a->parent_task, 1UL << a->ready_sig);
    for (;;) {
        ULONG signals;
        audio_pump(a);
        if (a->shutdown) break;
        signals = (1UL << a->port->mp_SigBit) | (1UL << a->wake_sig);
        Wait(signals);
    }
    audio_worker_cleanup(a);
    a->worker_task = NULL;
    Signal(a->parent_task, 1UL << a->stopped_sig);
    return;
failed:
    audio_worker_cleanup(a);
    a->worker_task = NULL;
    a->ready_ok = 0;
    Signal(a->parent_task, 1UL << a->ready_sig);
}

uint64_t audio_elapsed_us(mr_audio *a)
{
    uint64_t samples, clock_us, now, step;
    int oldest, used = 0;
    if (!a || !a->output_rate) return 0;
    Forbid();
    now = audio_now_us();
    samples = a->completed_samples;
    oldest = oldest_request(a);
    while (oldest >= 0 && used++ < NBUF) {
        unsigned played = request_estimated_played(a, oldest, now);
        samples += played;
        if (played < a->request[oldest].submitted_samples) break;
        {
            uint64_t seq = a->request[oldest].sequence;
            int i, next = -1;
            for (i = 0; i < NBUF; i++)
                if (a->request[i].state != AUDIO_REQ_IDLE &&
                    a->request[i].sequence > seq &&
                    (next < 0 || a->request[i].sequence <
                               a->request[next].sequence)) next = i;
            oldest = next;
        }
    }
    /* samples is a's completed_samples running total - grows for the whole
     * playback session, so this is a genuine 64-bit dividend divided by a
     * runtime value (a->output_rate) on every call. audio_elapsed_us() is
     * the audio master clock: mrplay.c calls it at least once per displayed
     * video frame while audio is playing (never at all for video-only
     * playback), making this the hottest of the __muldi3/__divdi3 sites in
     * this file (see core/mr_muldiv64.h and the other call sites above). */
    clock_us = mr_u64_div_u16(mr_u64_mul_u32(samples, 1000000u), a->output_rate);
    if (clock_us < a->last_reported_clock_us)
        clock_us = a->last_reported_clock_us;
    step = clock_us - a->last_reported_clock_us;
    if (step > a->clock_largest_step_us) a->clock_largest_step_us = step;
    a->last_reported_clock_us = clock_us;
    Permit();
    return clock_us;
}

unsigned long audio_elapsed_ms(mr_audio *a)
{
    return (unsigned long)(audio_elapsed_us(a) / 1000ULL);
}

unsigned long audio_buffered_ms(mr_audio *a)
{
    unsigned long samples;
    uint64_t now;
    int i;
    if (!a || !a->output_rate) return 0;
    Forbid();
    now = audio_now_us();
    samples = a->count;
    for (i = 0; i < NBUF; i++) {
        audio_request_timeline *r = &a->request[i];
        if (r->state == AUDIO_REQ_PLAYING || r->state == AUDIO_REQ_QUEUED) {
            unsigned played = request_estimated_played(a, i, now);
            if (played < r->submitted_samples) samples += r->submitted_samples - played;
        }
    }
    samples = samples * 1000UL / a->output_rate;
    Permit();
    return samples;
}

void audio_diagnostics(mr_audio *a, mr_audio_diagnostics *diag)
{
    uint64_t now;
    uint64_t clock_snapshot;
    if (!diag) return;
    memset(diag, 0, sizeof *diag);
    if (!a) return;
    clock_snapshot = audio_elapsed_us(a);
    Forbid();
    now = audio_now_us();
    diag->hardware_starvations = a->hardware_starvations;
    diag->minimum_buffered_ms = a->minimum_buffered_ms == ULONG_MAX
                              ? 0 : a->minimum_buffered_ms;
    diag->minimum_active_ms = a->minimum_active_ms == ULONG_MAX
                            ? 0 : a->minimum_active_ms;
    diag->longest_service_gap_ms =
        (unsigned long)(a->longest_service_gap * 1000 / CLOCKS_PER_SEC);
    diag->longest_no_active_ms =
        (unsigned long)(a->longest_no_active * 1000 / CLOCKS_PER_SEC);
    diag->fifo_samples = a->count;
    diag->fifo_dropped_samples = a->fifo_dropped_samples;
    diag->audio_clock_us = clock_snapshot;
    diag->fifo_buffered_ms = a->count * 1000UL / a->output_rate;
    diag->clock_largest_step_us = a->clock_largest_step_us;
    diag->startup_clock_largest_step_us = a->startup_clock_largest_step_us;
    {
        int i;
        for (i = 0; i < NBUF; i++) {
            audio_request_timeline *r = &a->request[i];
            diag->request_samples[i] = a->busy[i] ? (unsigned long)a->nsub[i] : 0;
            diag->request_state[i] = !a->busy[i] ? 0 :
                (r->state == AUDIO_REQ_COMPLETE ? 2 : 1);
            if (diag->request_state[i] == 1) diag->active_requests++;
            diag->request_timeline_state[i] = (unsigned char)r->state;
            if ((r->state == AUDIO_REQ_PLAYING || r->state == AUDIO_REQ_QUEUED) &&
                now >= r->scheduled_start_us && now < r->scheduled_end_us) {
                diag->request_timeline_state[i] = AUDIO_REQ_PLAYING;
                unsigned played = request_estimated_played(a, i, now);
                unsigned remaining = played < r->submitted_samples
                                   ? r->submitted_samples - played : 0;
                diag->hardware_playing_remaining_ms =
                    remaining * 1000UL / a->output_rate;
                diag->oldest_request_sequence = r->sequence;
            } else if (r->state == AUDIO_REQ_QUEUED) {
                diag->hardware_queued_ms +=
                    r->submitted_samples * 1000UL / a->output_rate;
                if (!diag->oldest_request_sequence ||
                    r->sequence < diag->oldest_request_sequence)
                    diag->oldest_request_sequence = r->sequence;
            }
        }
    }
    diag->total_buffered_ms = diag->fifo_buffered_ms +
        diag->hardware_playing_remaining_ms + diag->hardware_queued_ms;
    diag->timeline_covered = (unsigned char)(oldest_request(a) >= 0);
    Permit();
}

int audio_active_requests(mr_audio *a)
{
    int n;
    if (!a) return 0;
    Forbid(); n = active_request_count(a); Permit();
    return n;
}

void audio_set_running(mr_audio *a, int running)
{
    int wake = 0;
    if (!a) return;
    Forbid();
    running = running != 0;
    if (running && !a->ever_started) {
        a->startup_clock_largest_step_us = a->clock_largest_step_us;
        a->clock_largest_step_us = 0;
        a->ever_started = 1;
    }
    if (running && !a->running) wake = 1;
    a->running = running;
    a->hardware_starved = 0;
    a->no_active_since = 0;
    Permit();
    /* The worker may be asleep with a full startup FIFO and no outstanding
     * audio.device request. Wake it when playback/resume opens the gate. */
    if (wake && a->worker_task && a->wake_sig >= 0)
        Signal(a->worker_task, 1UL << a->wake_sig);
}

int audio_starved(mr_audio *a)
{
    int starved;
    if (!a) return 1;
    Forbid(); starved = oldest_request(a) < 0 && a->count == 0; Permit();
    return starved;
}
