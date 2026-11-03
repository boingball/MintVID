/*
 * MintVID - AHI (ahi.device) PCM output backend.
 *
 * Plays through one of ahi.device's four units, on whichever card and mode
 * AHI's preferences give that unit, at the source's own rate: AHI resamples
 * to the mode itself, so nothing here is held to Paula's ~28kHz ceiling. The
 * FIFO holds native signed 16-bit stereo frames (AHIST_S16S); mono is heard
 * in both speakers, as with Paula.
 *
 * The shape is the Paula backend's (audio_paula.c), so mrplay's rescue logic
 * reads the same diagnostics: a worker process at the same priority, two
 * requests of AHI_REQUEST_MS, completions committed in submission order, and
 * the clock interpolated across the request in flight on the EClock timeline.
 * Each new request is linked (ahir_Link) to the one still playing, so AHI
 * starts it the moment that one ends.
 *
 * A completed write means AHI's mixer has taken the samples, not that they
 * have been heard: the clock leads the speaker by the driver's own buffer,
 * tens of milliseconds on most cards.
 *
 * Teardown follows the Paula backend: every request in flight is AbortIO'd
 * and WaitIO'd before its buffer is freed or the device is closed.
 */
#include "audio_backend.h"
#include "../core/mr_muldiv64.h"

#include <exec/types.h>
#include <exec/memory.h>
#include <exec/io.h>
#include <devices/ahi.h>
#include <dos/dos.h>
#include <dos/dosextens.h>
#include <dos/dostags.h>
#include <proto/exec.h>
#include <proto/dos.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <limits.h>

#define NBUF           2
#define AHI_REQUEST_MS 200      /* PAULA_REQUEST_MS: mrplay.c's
                                 * AUDIO_RESCUE_ENTRY_MS/TARGET_MS/
                                 * ONE_REQUEST_MS are one or two requests of
                                 * this size, whichever output plays */

typedef struct ahi_timeline {
    uint64_t sequence;
    unsigned submitted_samples;
    uint64_t scheduled_start_us, scheduled_end_us;
    enum mr_audio_request_state state;
} ahi_timeline;

typedef struct ahi_audio {
    mr_audio           base;       /* first: audio.c dispatches through it  */
    int                unit;
    char               name[16];
    struct MsgPort    *port;
    struct AHIRequest *io[NBUF];
    short             *buf[NBUF];  /* interleaved S16 L/R frames            */
    int                bufsz;      /* frames per request                    */
    int                busy[NBUF];
    int                nsub[NBUF]; /* frames submitted in that request      */
    int                last;       /* slot sent last (ahir_Link), -1 none   */
    ahi_timeline       request[NBUF];
    int                opened;     /* ahi.device open (io[0])               */
    struct Task       *parent_task;
    struct Task       *worker_task;
    BYTE               ready_sig, stopped_sig, wake_sig;
    int                ready_ok;
    volatile int       shutdown;

    unsigned           rate;
    int                src_channels;
    int                src_bits;

    short             *fifo;       /* interleaved S16 L/R frames            */
    unsigned           fifo_size;  /* in frames                             */
    unsigned           head, tail, count;
    unsigned long      fifo_dropped_samples;

    uint64_t           completed_samples;
    uint64_t           next_sequence, timeline_end_us, last_reported_clock_us;
    uint64_t           clock_largest_step_us;
    uint64_t           startup_clock_largest_step_us;
    clock_t            last_service;
    clock_t            longest_service_gap;
    unsigned long      hardware_starvations;
    unsigned long      minimum_buffered_ms;
    unsigned long      minimum_active_ms;
    int                had_buffered_audio;
    int                running;
    int                volume;     /* mr_audio.h range 0..64                */
    int                ever_started;
    int                hardware_starved;
    clock_t            no_active_since;
    clock_t            longest_no_active;
} ahi_audio;

static const mr_audio_ops ahi_ops;
static void ahi_close(mr_audio *base);
static unsigned long ahi_buffered_ms(mr_audio *base);

static int request_active(ahi_audio *a, int i)
{
    return a->busy[i] && a->request[i].state != AUDIO_REQ_COMPLETE;
}

static int active_request_count(ahi_audio *a)
{
    int i, n = 0;
    for (i = 0; i < NBUF; i++) if (request_active(a, i)) n++;
    return n;
}

static int oldest_request(ahi_audio *a)
{
    int i, oldest = -1;
    for (i = 0; i < NBUF; i++) {
        if (a->request[i].state == AUDIO_REQ_IDLE) continue;
        if (oldest < 0 || a->request[i].sequence < a->request[oldest].sequence)
            oldest = i;
    }
    return oldest;
}

static uint64_t request_duration_us(ahi_audio *a, unsigned samples)
{
    /* AHI rates can pass 65535 Hz (96 kHz sources), so mr_u64_div_u24 rather
     * than Paula's mr_u64_div_u16 - see core/mr_muldiv64.h. */
    return mr_u64_div_u24(mr_u64_mul_u32(samples, 1000000u), a->rate);
}

static unsigned request_estimated_played(ahi_audio *a, int i, uint64_t now)
{
    ahi_timeline *r = &a->request[i];
    uint64_t elapsed, duration;
    if (r->state == AUDIO_REQ_COMPLETE || now >= r->scheduled_end_us)
        return r->submitted_samples;
    if (now <= r->scheduled_start_us) return 0;
    elapsed = now - r->scheduled_start_us;
    duration = r->scheduled_end_us - r->scheduled_start_us;
    return duration ? (unsigned)mr_u64_div_u24(
        mr_u64_mul_u32(elapsed, r->submitted_samples), (uint32_t)duration) : 0;
}

static void ahi_worker_entry(void);

/* ---- ring FIFO ---------------------------------------------------------- */

static void fifo_push(ahi_audio *a, short left, short right)
{
    if (a->count >= a->fifo_size) {
        a->fifo_dropped_samples++;             /* explicit bounded overflow */
        return;
    }
    a->fifo[a->head * 2] = left;
    a->fifo[a->head * 2 + 1] = right;
    a->head = (a->head + 1) % a->fifo_size;
    a->count++;
}

static int fifo_pop_into(ahi_audio *a, short *out, int n)
{
    int i = 0;
    while (i < n && a->count) {
        out[i * 2] = a->fifo[a->tail * 2];
        out[i * 2 + 1] = a->fifo[a->tail * 2 + 1];
        a->tail = (a->tail + 1) % a->fifo_size;
        a->count--;
        i++;
    }
    return i;
}

static void ahi_flush(mr_audio *base)
{
    ahi_audio *a = (ahi_audio *)base;
    Forbid();
    a->head = a->tail = 0;
    a->count = 0;
    Permit();
}

/* ---- lifecycle ---------------------------------------------------------- */

mr_audio *ahi_audio_open(int unit, unsigned rate, int channels, int bits)
{
    ahi_audio *a;
    struct Process *worker;

    if (unit < 0 || unit >= MR_AUDIO_OUT_AHI_UNITS || rate == 0 ||
        (bits != 8 && bits != 16) || channels < 1)
        return NULL;

    a = (ahi_audio *)calloc(1, sizeof *a);
    if (!a) return NULL;
    a->base.ops = &ahi_ops;
    a->unit = unit;
    snprintf(a->name, sizeof a->name, "AHI unit %d", unit);
    a->ready_sig = a->stopped_sig = a->wake_sig = -1;
    a->last = -1;
    a->rate = rate;
    a->src_channels = channels;
    a->src_bits = bits;
    a->minimum_buffered_ms = ULONG_MAX;
    a->minimum_active_ms = ULONG_MAX;
    a->volume = 64;
    a->bufsz = (int)(rate * AHI_REQUEST_MS / 1000);
    if (a->bufsz < 256) a->bufsz = 256;
    /* The same ~4 s cushion as Paula's FIFO, for the same coarsely
     * interleaved files. */
    a->fifo_size = rate * 4;
    if (a->fifo_size < 8192) a->fifo_size = 8192;
    a->fifo = (short *)malloc((size_t)a->fifo_size * 2 * sizeof(short));
    if (!a->fifo) { ahi_close(&a->base); return NULL; }

    a->parent_task = FindTask(NULL);
    a->ready_sig = AllocSignal(-1);
    a->stopped_sig = AllocSignal(-1);
    if (a->ready_sig < 0 || a->stopped_sig < 0) { ahi_close(&a->base); return NULL; }
    /* Priority 5, as the Paula worker: see audio_paula.c for why. */
    worker = CreateNewProcTags(NP_Entry, (ULONG)ahi_worker_entry,
                               NP_Name, (ULONG)"MintVID AHI output",
                               NP_Priority, 5,
                               NP_StackSize, 16384UL,
                               TAG_END);
    if (!worker) {
        ahi_close(&a->base); return NULL;
    }
    /* The same tc_UserData + Ctrl-F hand-over as the Paula worker. */
    Forbid();
    a->worker_task = &worker->pr_Task;
    worker->pr_Task.tc_UserData = a;
    Signal(a->worker_task, SIGBREAKF_CTRL_F);
    Permit();
    Wait(1UL << a->ready_sig);
    if (!a->ready_ok) { ahi_close(&a->base); return NULL; }

    return &a->base;
}

static void ahi_close(mr_audio *base)
{
    ahi_audio *a = (ahi_audio *)base;
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

static const char *ahi_name(mr_audio *base)
{
    return ((ahi_audio *)base)->name;
}

/* ---- streaming ---------------------------------------------------------- */

static void wake_worker(ahi_audio *a)
{
    if (a->worker_task && a->wake_sig >= 0)
        Signal(a->worker_task, 1UL << a->wake_sig);
}

static void ahi_write(mr_audio *base, const unsigned char *pcm, unsigned bytes)
{
    ahi_audio *a = (ahi_audio *)base;
    int bps = a->src_bits / 8, ch = a->src_channels;
    int framebytes = bps * ch;
    unsigned n, k;

    if (framebytes <= 0) return;
    n = bytes / (unsigned)framebytes;
    Forbid();
    for (k = 0; k < n; k++) {
        const unsigned char *p = pcm + (size_t)k * framebytes;
        short l, r;
        if (a->src_bits == 16) {
            l = (short)(p[0] | (p[1] << 8));            /* LE, signed */
            r = ch >= 2 ? (short)(p[2] | (p[3] << 8)) : l;
        } else {
            l = audio_pcm_u8_to_s16(p[0]);
            r = ch >= 2 ? audio_pcm_u8_to_s16(p[1]) : l;
        }
        fifo_push(a, l, r);
    }
    Permit();
    wake_worker(a);
}

static void ahi_write_s16(mr_audio *base, const short *pcm,
                          unsigned frames, int channels)
{
    ahi_audio *a = (ahi_audio *)base;
    unsigned k;
    Forbid();
    for (k = 0; k < frames; k++) {
        short l = pcm[(size_t)k * channels];
        short r = channels >= 2 ? pcm[(size_t)k * channels + 1] : l;
        fifo_push(a, l, r);
    }
    Permit();
    wake_worker(a);
}

static void ahi_pump(ahi_audio *a)
{
    int i;
    /* As in the Paula backend: the clock() bookkeeping only feeds --time's
     * report; hardware_starved/hardware_starvations are rescue inputs and
     * stay unconditional. */
    clock_t service_now = 0;
    uint64_t now = audio_now_us();
    unsigned long buffered;
    if (audio_want_time) {
        service_now = clock();
        if (a->last_service && service_now - a->last_service > a->longest_service_gap)
            a->longest_service_gap = service_now - a->last_service;
        a->last_service = service_now;
        if (a->running && active_request_count(a) == 0) {
            if (!a->no_active_since) a->no_active_since = service_now;
        } else a->no_active_since = 0;
    }

    for (i = 0; i < NBUF; i++) {
        if (a->busy[i] && CheckIO((struct IORequest *)a->io[i])) {
            WaitIO((struct IORequest *)a->io[i]);
            a->busy[i] = 0;
            a->request[i].state = AUDIO_REQ_COMPLETE;
        }
    }
    /* Commit completed requests strictly in submission order, as Paula
     * does. */
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

    /* `running` gates new writes exactly as in the Paula backend; requests
     * already sent finish. */
    if (a->running) {
        for (i = 0; i < NBUF; i++) {
            if (!a->busy[i] && a->request[i].state == AUDIO_REQ_IDLE) {
                struct AHIRequest *io = a->io[i];
                int had_outstanding = oldest_request(a) >= 0;
                int n;
                Forbid();
                n = fifo_pop_into(a, a->buf[i], a->bufsz);
                Permit();
                if (n <= 0) continue;
                /* A completed CMD_WRITE leaves every field but io_Device,
                 * io_Unit and io_Command trashed: set them all each time. */
                io->ahir_Std.io_Command = CMD_WRITE;
                io->ahir_Std.io_Data = a->buf[i];
                io->ahir_Std.io_Length = (ULONG)n * 2 * sizeof(short);
                io->ahir_Std.io_Offset = 0;
                io->ahir_Type = AHIST_S16S;
                io->ahir_Frequency = a->rate;
                io->ahir_Volume = (Fixed)a->volume << 10;  /* 64 -> 0x10000 */
                io->ahir_Position = 0x8000;                 /* centre */
                io->ahir_Link = a->last >= 0 && a->busy[a->last]
                              ? a->io[a->last] : NULL;
                SendIO((struct IORequest *)io);
                a->busy[i] = 1;
                a->last = i;
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
    buffered = ahi_buffered_ms(&a->base);
    if ((buffered || a->had_buffered_audio) &&
        buffered < a->minimum_buffered_ms)
        a->minimum_buffered_ms = buffered;
    a->had_buffered_audio = buffered != 0;
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
            if (audio_want_time) {
                clock_t gap;
                if (!a->no_active_since) a->no_active_since = service_now;
                gap = service_now - a->no_active_since;
                if (gap > a->longest_no_active) a->longest_no_active = gap;
            }
        } else {
            a->hardware_starved = 0;
            if (audio_want_time) a->no_active_since = 0;
        }
    }
}

static void ahi_set_volume(mr_audio *base, int volume)
{
    ahi_audio *a = (ahi_audio *)base;
    if (volume < 0) volume = 0;
    if (volume > 64) volume = 64;
    a->volume = volume;
}

static void ahi_worker_cleanup(ahi_audio *a)
{
    int i;
    /* The request sent last first, so aborting the one playing cannot start
     * the one linked to it. */
    for (i = 0; i < NBUF; i++) {
        int slot = a->last >= 0 ? (a->last + NBUF - i) % NBUF : i;
        if (a->busy[slot]) {
            AbortIO((struct IORequest *)a->io[slot]);
            WaitIO((struct IORequest *)a->io[slot]);
            a->busy[slot] = 0;
        }
    }
    if (a->opened) {
        CloseDevice((struct IORequest *)a->io[0]);
        a->opened = 0;
    }
    for (i = 0; i < NBUF; i++) {
        if (a->io[i]) DeleteIORequest((struct IORequest *)a->io[i]);
        a->io[i] = NULL;
        if (a->buf[i])
            FreeMem(a->buf[i], (ULONG)a->bufsz * 2 * sizeof(short));
        a->buf[i] = NULL;
    }
    if (a->port) { DeleteMsgPort(a->port); a->port = NULL; }
    if (a->wake_sig >= 0) { FreeSignal(a->wake_sig); a->wake_sig = -1; }
}

static void ahi_worker_entry(void)
{
    struct Task *self = FindTask(NULL);
    ahi_audio *a;
    int i;
    /* Parent publishes tc_UserData after CreateNewProcTags() returns. */
    Wait(SIGBREAKF_CTRL_F);
    a = (ahi_audio *)self->tc_UserData;
    if (!a) return;
    a->worker_task = self;
    a->wake_sig = AllocSignal(-1);
    a->port = CreateMsgPort();
    if (a->wake_sig < 0 || !a->port) goto failed;
    for (i = 0; i < NBUF; i++) {
        a->io[i] = (struct AHIRequest *)CreateIORequest(
            a->port, sizeof(struct AHIRequest));
        if (!a->io[i]) goto failed;
        a->buf[i] = (short *)AllocMem((ULONG)a->bufsz * 2 * sizeof(short),
                                      MEMF_PUBLIC | MEMF_CLEAR);
        if (!a->buf[i]) goto failed;
    }
    a->io[0]->ahir_Version = 4;
    if (OpenDevice((CONST_STRPTR)AHINAME, (ULONG)a->unit,
                   (struct IORequest *)a->io[0], 0) != 0) goto failed;
    a->opened = 1;
    /* The second request plays on the same open unit: a copy of the opened
     * one (as AHI's own double-buffering example does), keeping its own
     * message header. */
    for (i = 1; i < NBUF; i++) {
        struct Message keep = a->io[i]->ahir_Std.io_Message;
        memcpy(a->io[i], a->io[0], sizeof(struct AHIRequest));
        a->io[i]->ahir_Std.io_Message = keep;
    }
    a->ready_ok = 1;
    Signal(a->parent_task, 1UL << a->ready_sig);
    for (;;) {
        ahi_pump(a);
        if (a->shutdown) break;
        Wait((1UL << a->port->mp_SigBit) | (1UL << a->wake_sig));
    }
    ahi_worker_cleanup(a);
    /* The same exit handshake as the Paula worker: no Permit() after this
     * Forbid(), so the parent cannot unload this code before the task has
     * gone. */
    Forbid();
    a->worker_task = NULL;
    Signal(a->parent_task, 1UL << a->stopped_sig);
    return;
failed:
    ahi_worker_cleanup(a);
    Forbid();
    a->worker_task = NULL;
    a->ready_ok = 0;
    Signal(a->parent_task, 1UL << a->ready_sig);
}

/* ---- clock and diagnostics (as in the Paula backend) -------------------- */

static uint64_t ahi_elapsed_us(mr_audio *base)
{
    ahi_audio *a = (ahi_audio *)base;
    uint64_t samples, clock_us, now, step;
    int oldest, used = 0;
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
    clock_us = mr_u64_div_u24(mr_u64_mul_u32(samples, 1000000u), a->rate);
    if (clock_us < a->last_reported_clock_us)
        clock_us = a->last_reported_clock_us;
    step = clock_us - a->last_reported_clock_us;
    if (step > a->clock_largest_step_us) a->clock_largest_step_us = step;
    a->last_reported_clock_us = clock_us;
    Permit();
    return clock_us;
}

static unsigned long ahi_buffered_ms(mr_audio *base)
{
    ahi_audio *a = (ahi_audio *)base;
    unsigned long samples;
    uint64_t now;
    int i;
    Forbid();
    now = audio_now_us();
    samples = a->count;
    for (i = 0; i < NBUF; i++) {
        ahi_timeline *r = &a->request[i];
        if (r->state == AUDIO_REQ_PLAYING || r->state == AUDIO_REQ_QUEUED) {
            unsigned played = request_estimated_played(a, i, now);
            if (played < r->submitted_samples) samples += r->submitted_samples - played;
        }
    }
    samples = samples * 1000UL / a->rate;
    Permit();
    return samples;
}

static unsigned long ahi_capacity_ms(mr_audio *base)
{
    ahi_audio *a = (ahi_audio *)base;
    return (unsigned long)a->fifo_size * 1000UL / a->rate;
}

static void ahi_diagnostics(mr_audio *base, mr_audio_diagnostics *diag)
{
    ahi_audio *a = (ahi_audio *)base;
    uint64_t now;
    uint64_t clock_snapshot;
    int i;
    memset(diag, 0, sizeof *diag);
    clock_snapshot = ahi_elapsed_us(base);
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
    diag->fifo_buffered_ms = a->count * 1000UL / a->rate;
    diag->clock_largest_step_us = a->clock_largest_step_us;
    diag->startup_clock_largest_step_us = a->startup_clock_largest_step_us;
    for (i = 0; i < NBUF; i++) {
        ahi_timeline *r = &a->request[i];
        diag->request_samples[i] = a->busy[i] ? (unsigned long)a->nsub[i] : 0;
        diag->request_state[i] = !a->busy[i] ? 0 :
            (r->state == AUDIO_REQ_COMPLETE ? 2 : 1);
        if (diag->request_state[i] == 1) diag->active_requests++;
        diag->request_timeline_state[i] = (unsigned char)r->state;
        if ((r->state == AUDIO_REQ_PLAYING || r->state == AUDIO_REQ_QUEUED) &&
            now >= r->scheduled_start_us && now < r->scheduled_end_us) {
            unsigned played = request_estimated_played(a, i, now);
            unsigned remaining = played < r->submitted_samples
                               ? r->submitted_samples - played : 0;
            diag->request_timeline_state[i] = AUDIO_REQ_PLAYING;
            diag->hardware_playing_remaining_ms =
                remaining * 1000UL / a->rate;
            diag->oldest_request_sequence = r->sequence;
        } else if (r->state == AUDIO_REQ_QUEUED) {
            diag->hardware_queued_ms +=
                r->submitted_samples * 1000UL / a->rate;
            if (!diag->oldest_request_sequence ||
                r->sequence < diag->oldest_request_sequence)
                diag->oldest_request_sequence = r->sequence;
        }
    }
    diag->total_buffered_ms = diag->fifo_buffered_ms +
        diag->hardware_playing_remaining_ms + diag->hardware_queued_ms;
    diag->timeline_covered = (unsigned char)(oldest_request(a) >= 0);
    Permit();
}

static int ahi_active_requests(mr_audio *base)
{
    ahi_audio *a = (ahi_audio *)base;
    int n;
    Forbid(); n = active_request_count(a); Permit();
    return n;
}

static void ahi_set_running(mr_audio *base, int running)
{
    ahi_audio *a = (ahi_audio *)base;
    int wake = 0;
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
    if (wake) wake_worker(a);
}

static int ahi_starved(mr_audio *base)
{
    ahi_audio *a = (ahi_audio *)base;
    int starved;
    Forbid(); starved = oldest_request(a) < 0 && a->count == 0; Permit();
    return starved;
}

static const mr_audio_ops ahi_ops = {
    ahi_name,
    ahi_close,
    ahi_write,
    ahi_write_s16,
    ahi_elapsed_us,
    ahi_starved,
    ahi_buffered_ms,
    ahi_capacity_ms,
    ahi_diagnostics,
    ahi_active_requests,
    ahi_set_running,
    ahi_set_volume,
    ahi_flush
};
