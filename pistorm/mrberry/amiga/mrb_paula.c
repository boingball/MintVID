/*
 * mrberry - sound through the Amiga's own audio (Paula). See mrb_paula.h.
 *
 * Two media times are kept in microseconds:
 *   cursor   media time of the next sample the decoder hands over. It counts
 *            decoded samples and snaps to a packet's timestamp only when the
 *            two disagree by more than PTS_SNAP_US.
 *   tail     media time just after the last sample put in Paula's queue.
 * Normally tail == cursor. A forward jump in the timestamps is filled with
 * silence; a backward one drops samples until the cursor catches up.
 *
 * Sync looks at the tail. The last queued sample will be heard in
 * audio_buffered_ms() from now; the clock says which media time is due then.
 * The difference is fixed at the tail as well, so a correction counts at
 * once and is never applied twice:
 *   sound ahead  -> add silence (the queue gets longer, the tail is heard later)
 *   sound behind -> drop decoded samples (the tail moves on without them)
 */
#include "mrb_paula.h"
#include "../audio/mr_audio_decode.h"
#include "mr_audio.h"

#include <exec/types.h>
#include <exec/io.h>
#include <devices/timer.h>
#include <proto/exec.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern ULONG ac309i_read_sys_us(void);

#define SYNC_TOLERANCE_US   40000u   /* leave smaller errors alone          */
#define SYNC_STEP_US        20000u   /* small corrections, 20 ms at a time  */
#define SYNC_JUMP_US       300000u   /* larger errors are fixed in one go   */
#define SYNC_INTERVAL_US   500000u   /* check twice a second                */
#define PTS_SNAP_US        100000u
#define GAP_TOLERANCE_US    20000u
#define MAX_FILL_US       1000000u   /* never queue more silence than this  */
#define QUEUE_HEADROOM_MS     500u   /* keep this much of Paula's FIFO free */
#define START_EARLY_US       5000u

/* audio_paula.c reads ReadEClock() through this, like mrplay. */
struct Device *TimerBase;
static struct MsgPort *timer_port;
static struct timerequest *timer_io;

static void timer_close(void)
{
    if (TimerBase && timer_io) CloseDevice((struct IORequest *)timer_io);
    TimerBase = NULL;
    if (timer_io) DeleteIORequest((struct IORequest *)timer_io);
    if (timer_port) DeleteMsgPort(timer_port);
    timer_io = NULL;
    timer_port = NULL;
}

static void timer_open(void)
{
    if (TimerBase) return;
    timer_port = CreateMsgPort();
    if (timer_port)
        timer_io = (struct timerequest *)CreateIORequest(timer_port,
                                                         sizeof *timer_io);
    if (timer_io &&
        OpenDevice((CONST_STRPTR)TIMERNAME, UNIT_MICROHZ,
                   (struct IORequest *)timer_io, 0) == 0)
        TimerBase = timer_io->tr_node.io_Device;
    else
        timer_close();          /* audio_paula.c falls back to clock() */
}

typedef struct pkt_node {
    struct pkt_node *next;
    int has_pts;
    uint64_t pts_us;
    uint32_t len;
    uint8_t data[1];
} pkt_node;

struct mrb_paula {
    mr_audio_decoder *dec;
    mr_audio *out;
    unsigned rate, channels;
    int32_t offset_us;
    char describe[48];

    pkt_node *head, *tail_node;

    /* clock: microseconds played since the anchor, pauses excluded. Kept
     * in 64 bits from 32-bit timer deltas, so the timer's wrap every 71
     * minutes does not matter as long as service runs more often. */
    int anchored;
    uint32_t last_sys;
    uint64_t clock_us;
    uint64_t start_media;
    int paused;
    int running;
    uint64_t next_check;        /* in clock_us */

    /* decode cursor: c_base + c_frames samples */
    int have_cursor;
    uint64_t c_base;
    uint64_t c_frames;

    /* queue tail */
    int started;
    uint64_t head_media;
    uint64_t tail;

    uint64_t drop_frames;       /* pending correction                    */
    unsigned long silence_us, dropped_us, corrections;

    uint64_t decode_us;         /* 68k time in decode_one(), for the log */
    unsigned long decoded_packets;
};

static uint64_t frames_to_us(const mrb_paula *p, uint64_t frames)
{
    return frames * 1000000u / p->rate;
}

static uint64_t us_to_frames(const mrb_paula *p, uint64_t us)
{
    return us * p->rate / 1000000u;
}

static uint64_t cursor(const mrb_paula *p)
{
    return p->c_base + frames_to_us(p, p->c_frames);
}

static void tick(mrb_paula *p)
{
    uint32_t now = (uint32_t)ac309i_read_sys_us();
    if (p->anchored && !p->paused) p->clock_us += (uint32_t)(now - p->last_sys);
    p->last_sys = now;
}

/* Media time the clock shows `ahead_us` from now. It is below start_media
 * while a positive offset still holds the sound back. */
static int64_t media_at(const mrb_paula *p, uint64_t ahead_us)
{
    return (int64_t)p->start_media +
           (int64_t)(p->clock_us + ahead_us) - p->offset_us;
}

static void write_silence(mrb_paula *p, uint64_t frames)
{
    static const short zero[256 * 2];
    while (frames) {
        unsigned n = frames > 256 ? 256 : (unsigned)frames;
        audio_write_s16(p->out, zero, n, (int)p->channels);
        frames -= n;
    }
}

/* Room in Paula's FIFO, in microseconds. */
static uint64_t room_us(mrb_paula *p)
{
    unsigned long cap = audio_capacity_ms(p->out);
    unsigned long buf = audio_buffered_ms(p->out);
    if (buf + QUEUE_HEADROOM_MS >= cap) return 0;
    return (uint64_t)(cap - buf - QUEUE_HEADROOM_MS) * 1000u;
}

static void sink(void *user, const int16_t *pcm, unsigned frames,
                 unsigned channels)
{
    mrb_paula *p = (mrb_paula *)user;
    unsigned skip;

    if (!frames) return;
    if (channels != p->channels) return;      /* decoder layout changed */

    if (!p->started) {
        /* Drop what lies before the first video frame, or before the media
         * time the clock has already reached (a negative offset). */
        uint64_t c = cursor(p), from = p->start_media;
        int64_t now = media_at(p, 0);
        if (now > (int64_t)from) from = (uint64_t)now;
        if (c < from) {
            uint64_t need = us_to_frames(p, from - c);
            if (need >= frames) { p->c_frames += frames; return; }
            pcm += (unsigned)need * channels;
            frames -= (unsigned)need;
            p->c_frames += need;
        }
        p->started = 1;
        p->head_media = p->tail = cursor(p);
    }

    {
        uint64_t c = cursor(p);
        if (c > p->tail + GAP_TOLERANCE_US) {
            /* Timestamps jumped forward: fill the hole. */
            uint64_t gap = c - p->tail, room = room_us(p);
            if (gap > MAX_FILL_US) gap = MAX_FILL_US;
            if (gap > room) gap = room;
            write_silence(p, us_to_frames(p, gap));
            p->tail = c;
        } else if (c + GAP_TOLERANCE_US < p->tail) {
            /* Timestamps went back: drop until the cursor catches up. */
            uint64_t need = us_to_frames(p, p->tail - c);
            if (need >= frames) { p->c_frames += frames; return; }
            pcm += (unsigned)need * channels;
            frames -= (unsigned)need;
            p->c_frames += need;
        }
    }

    skip = 0;
    if (p->drop_frames) {
        skip = p->drop_frames >= frames ? frames : (unsigned)p->drop_frames;
        p->drop_frames -= skip;
    }
    if (frames > skip)
        audio_write_s16(p->out, (const short *)(pcm + skip * channels),
                        frames - skip, (int)channels);
    p->c_frames += frames;
    p->tail = cursor(p);
}

static void decode_one(mrb_paula *p, pkt_node *n)
{
    if (n->has_pts) {
        uint64_t c = cursor(p);
        if (!p->have_cursor ||
            (n->pts_us > c ? n->pts_us - c : c - n->pts_us) > PTS_SNAP_US) {
            p->c_base = n->pts_us;
            p->c_frames = 0;
            p->have_cursor = 1;
        }
    } else if (!p->have_cursor) {
        p->c_base = p->start_media;
        p->c_frames = 0;
        p->have_cursor = 1;
    }
    if (mr_audio_decoder_feed(p->dec, n->data, n->len, sink, p) < 0) {
        /* A broken frame: resynchronise on the next timestamp. */
        p->have_cursor = 0;
    }
}

static void sync_check(mrb_paula *p)
{
    unsigned long buf_ms;
    int64_t due, err;
    if (p->clock_us < p->next_check) return;
    p->next_check = p->clock_us + SYNC_INTERVAL_US;
    buf_ms = audio_buffered_ms(p->out);
    due = media_at(p, (uint64_t)buf_ms * 1000u);
    err = (int64_t)p->tail - due;      /* > 0: sound is ahead */
#ifdef MRB_PAULA_TRACE
    printf("PAULA sync clock=%lums buffered=%lums err=%ldms\n",
           (unsigned long)(p->clock_us / 1000u), buf_ms, (long)(err / 1000));
#endif
    if (err > (int64_t)SYNC_TOLERANCE_US) {
        uint64_t fix = (uint64_t)err > SYNC_JUMP_US ? (uint64_t)err : SYNC_STEP_US;
        uint64_t room = room_us(p);
        if (fix > MAX_FILL_US) fix = MAX_FILL_US;
        if (fix > room) fix = room;
        if (!fix) return;
        write_silence(p, us_to_frames(p, fix));
        p->silence_us += (unsigned long)fix;
        p->corrections++;
        p->next_check = p->clock_us + SYNC_INTERVAL_US + fix;
    } else if (err < -(int64_t)SYNC_TOLERANCE_US && !p->drop_frames) {
        uint64_t fix = (uint64_t)(-err) > SYNC_JUMP_US ? (uint64_t)(-err)
                                                       : SYNC_STEP_US;
        p->drop_frames = us_to_frames(p, fix);
        p->dropped_us += (unsigned long)fix;
        p->corrections++;
    }
}

mrb_paula *mrb_paula_open(const mr_audio_info *ai, int low_rate, int mono,
                          int32_t offset_us, char *why, unsigned why_cap)
{
    mrb_paula *p = (mrb_paula *)calloc(1, sizeof *p);
    if (!p) { snprintf(why, why_cap, "out of memory"); return NULL; }
    p->offset_us = offset_us;
    p->dec = mr_audio_decoder_open(ai, low_rate, mono, MR_AUDIO_PAULA_RATE_MAX);
    if (!p->dec) {
        snprintf(why, why_cap, "this audio format cannot be decoded");
        free(p);
        return NULL;
    }
    p->rate = mr_audio_decoder_rate(p->dec);
    p->channels = mr_audio_decoder_channels(p->dec);
    if (!p->rate || !p->channels) {
        snprintf(why, why_cap, "audio decoder setup failed");
        mrb_paula_close(p);
        return NULL;
    }
    timer_open();
    p->out = audio_open(MR_AUDIO_OUT_PAULA, p->rate, (int)p->channels, 16);
    if (!p->out) {
        snprintf(why, why_cap, "cannot open audio.device");
        mrb_paula_close(p);
        return NULL;
    }
    snprintf(p->describe, sizeof p->describe, "Amiga audio %s %u Hz",
             mr_audio_decoder_name(p->dec), p->rate);
    return p;
}

void mrb_paula_close(mrb_paula *p)
{
    pkt_node *n;
    if (!p) return;
    if (p->out) {
        audio_set_running(p->out, 0);
        audio_close(p->out);
    }
    if (p->dec) mr_audio_decoder_close(p->dec);
    timer_close();
    while ((n = p->head) != NULL) { p->head = n->next; free(n); }
    free(p);
}

const char *mrb_paula_describe(const mrb_paula *p)
{
    return p ? p->describe : "";
}

int mrb_paula_feed(mrb_paula *p, const mr_packet *pkt)
{
    pkt_node *n;
    if (!p || !pkt || !pkt->len) return 1;
    n = (pkt_node *)malloc(sizeof *n + pkt->len);
    if (!n) return 0;
    n->next = NULL;
    n->has_pts = pkt->has_pts;
    n->pts_us = pkt->pts_us;
    n->len = pkt->len;
    memcpy(n->data, pkt->data, pkt->len);
    if (p->tail_node) p->tail_node->next = n; else p->head = n;
    p->tail_node = n;
    mrb_paula_service(p);
    return 1;
}

void mrb_paula_anchor(mrb_paula *p, uint32_t sys_us, uint64_t media_us)
{
    uint32_t now;
    if (!p || p->anchored) return;
    now = (uint32_t)ac309i_read_sys_us();
    p->start_media = media_us;
    p->anchored = 1;
    p->last_sys = now;
    /* Time already gone by since the frame went out counts. */
    p->clock_us = p->paused ? 0 : (uint32_t)(now - sys_us);
    p->next_check = SYNC_INTERVAL_US;
    mrb_paula_service(p);
}

void mrb_paula_service(mrb_paula *p)
{
    if (!p) return;
    tick(p);
    /* Nothing is decoded before the anchor: until then it is not known
     * which samples come before the first frame. */
    while (p->anchored && p->head && room_us(p) > 0) {
        pkt_node *n = p->head;
        p->head = n->next;
        if (!p->head) p->tail_node = NULL;
        {
            uint32_t t0 = (uint32_t)ac309i_read_sys_us();
            decode_one(p, n);
            p->decode_us += (uint32_t)((uint32_t)ac309i_read_sys_us() - t0);
            p->decoded_packets++;
        }
        free(n);
    }
    if (!p->anchored || p->paused || !p->started) return;
    if (!p->running) {
        if (media_at(p, START_EARLY_US) >= (int64_t)p->head_media) {
            audio_set_running(p->out, 1);
            p->running = 1;
            p->next_check = p->clock_us + SYNC_INTERVAL_US;
        }
        return;
    }
    sync_check(p);
}

/* P96W70 (MintVID-pi): change the sound offset while playing; sync_check
 * moves the sound there with silence/dropping, checked at once. */
void mrb_paula_set_offset(mrb_paula *p, int32_t offset_us)
{
    if (!p) return;
    tick(p);
    p->offset_us = offset_us;
    p->next_check = p->clock_us;
}

void mrb_paula_set_mute(mrb_paula *p, int mute)
{
    if (p && p->out) audio_set_volume(p->out, mute ? 0 : 64);
}

void mrb_paula_pause(mrb_paula *p, int paused)
{
    if (!p) return;
    paused = paused != 0;
    if (paused == p->paused) return;
    tick(p);                    /* count time up to now, then freeze */
    p->paused = paused;
    if (p->running) audio_set_running(p->out, !paused);
    if (!paused) p->next_check = p->clock_us + SYNC_INTERVAL_US;
}

int mrb_paula_idle(mrb_paula *p)
{
    if (!p) return 1;
    mrb_paula_service(p);
    return !p->head && (!p->running || audio_starved(p->out));
}

void mrb_paula_stats(const mrb_paula *p, unsigned long *silence_ms,
                     unsigned long *dropped_ms, unsigned long *corrections)
{
    *silence_ms = p ? p->silence_us / 1000u : 0;
    *dropped_ms = p ? p->dropped_us / 1000u : 0;
    *corrections = p ? p->corrections : 0;
}

void mrb_paula_timing(mrb_paula *p, unsigned long *decode_ms,
                      unsigned long *packets, unsigned long *starvations,
                      unsigned long *min_buffered_ms)
{
    mr_audio_diagnostics diag;
    *decode_ms = *packets = *starvations = *min_buffered_ms = 0;
    if (!p) return;
    *decode_ms = (unsigned long)(p->decode_us / 1000u);
    *packets = p->decoded_packets;
    if (p->out) {
        memset(&diag, 0, sizeof diag);
        audio_diagnostics(p->out, &diag);
        *starvations = diag.hardware_starvations;
        *min_buffered_ms = diag.minimum_buffered_ms;
    }
}
