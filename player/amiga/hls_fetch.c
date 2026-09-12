/*
 * MintVID - background HLS fetch worker (Amiga). See hls_fetch.h for the
 * public contract. Design notes:
 *
 * - A single background process performs every playlist/segment fetch this
 *   session makes, installed as core/mr_http.c's fetch override/prefetch
 *   hint (mr_http_set_fetch_override()/mr_http_set_prefetch_hint()) so it
 *   is the *only* task that ever touches that file's socket/TLS state -
 *   see the design note above connect_socket() in mr_http.c for why that
 *   matters on Amiga. hls_fetch_start() is called before any other network
 *   call this session might make (mrplay.c calls it ahead of YouTube URL
 *   resolution), so the worker is always the session's first opener, never
 *   a second task adopting state another task already opened.
 *
 * - Exactly one request is ever in flight (one reusable struct Message,
 *   reused for every fetch and finally for the quit handshake), but up to
 *   HLS_FETCH_LOOKAHEAD_DEPTH segments of compressed background lookahead
 *   can be queued and ready at once: mr_hls.c's open_seg() hints several
 *   segments ahead of the one currently playing while the worker fetches
 *   them one at a time, in order, into g_slots[] below. A live HLS segment
 *   fetch can stall for hundreds of ms up to well over a second (observed
 *   on YouTube live), so a single segment of lookahead left almost no
 *   margin against jitter - see mrplay.c's video_cap comment for how that
 *   showed up downstream as audio-clock drift. Buffering compressed bytes
 *   this way is far cheaper than the decoded-RGB queue that otherwise has
 *   to absorb the same jitter (a typical live segment is under a MB; a
 *   decoded 720p RGB24 frame alone is ~2.6 MiB), so a few segments of
 *   compressed lookahead buys much more real cushion per byte of RAM.
 *   A miss (nothing hinted for this URL - e.g. the playlist, which is
 *   never hinted, only ever fetched on demand, or a segment the lookahead
 *   window hadn't reached yet) falls back to a blocking round trip through
 *   the same worker via the original single-slot g_ready_ / g_busy path
 *   below - never worse than today's fully-synchronous behaviour, and
 *   unlike it, the main task stays responsive throughout (audio/video/UI
 *   keep moving, ESC/quit is honoured promptly) because every wait here is
 *   a serviced poll, never a raw blocking Wait()/WaitPort() (the one
 *   exception is the one-time worker-startup handshake in
 *   hls_fetch_start(), before any playback state exists to stay responsive
 *   for).
 *
 * - A caller-visible generation counter (hls_fetch_cancel(), bumped on
 *   stream switch/live-reconnect/stop) invalidates whatever the worker is
 *   holding without ever sending it a message: a reply reclaimed under a
 *   stale generation is simply freed instead of cached or handed to a
 *   caller. This can never race the single in-flight slot, because nothing
 *   is ever sent to the worker until the slot is confirmed free (reclaimed,
 *   generation-checked or not) - cancel() only changes what a *future*
 *   reclaim does with whatever comes back.
 *
 * - hls_fetch_stop() never abandons a live worker. Unlike MintAMP's
 *   per-dispatch jobs, this long-lived worker executes a function in
 *   mrplay's own loaded code segment. Letting main() return after a bounded
 *   timeout unloads that code while the worker may still be executing it,
 *   producing an illegal-instruction alert. Teardown therefore stops
 *   promotion of queued lookahead, cancels the one in-flight operation,
 *   and waits for both its reply and the worker's quit acknowledgement.
 *   Underlying network calls are timed out/kicked; if a stack is genuinely
 *   wedged, keeping mrplay alive is still safer than unloading code beneath
 *   an active task.
 *
 * - All allocation on the worker's fetch path already goes through
 *   core/mr_alloc.h (AllocVec/FreeVec on Amiga - task-safe, unlike libnix
 *   malloc/free) via mr_http_fetch_text_direct()/mr_http_post_json_direct().
 *   The result buffer's ownership passes to whichever caller consumes it
 *   (mr_free() required); anything reclaimed but not consumed (a stale
 *   generation, or a mismatched URL when a fresh request pre-empts a
 *   lingering hint result) is freed right there in hls_fetch_reclaim() -
 *   the one place either happens, so there is exactly one path that ever
 *   frees a reply buffer, not one per caller.
 */
#include "hls_fetch.h"

#include "../core/mr_http.h"
#include "../core/mr_source.h"
#include "../core/mr_alloc.h"

#include <exec/types.h>
#include <exec/memory.h>
#include <exec/nodes.h>
#include <exec/ports.h>
#include <dos/dos.h>
#include <dos/dosextens.h>
#include <dos/dostags.h>
#include <proto/exec.h>
#include <proto/dos.h>

#include <stdio.h>
#include <string.h>
#include <time.h>

#define HLS_FETCH_STACK       300000UL  /* HTTPS/AmiSSL needs a deep stack   */
#define HLS_FETCH_JSON_MAX    1024      /* matches mr_youtube.c's json[]     */
#define HLS_FETCH_ERROR_MAX   192       /* matches mr_source.c's error cap   */
#define HLS_FETCH_HINT_MAX    (24UL * 1024 * 1024) /* matches mr_hls.c's own
                                                     * HLS_SEGMENT_MAX - hints
                                                     * are only ever segments */
#define HLS_FETCH_MEM_FLOOR   (4UL * 1024 * 1024)  /* true-exhaustion guard,
                                                     * not a working-set cap */
#define HLS_FETCH_STOP_NOTICE_MS 5000UL /* re-print the "still waiting" note
                                         * at this cadence during teardown   */
/* How many segments of compressed lookahead mr_hls.c's open_seg() is worth
 * hinting for - matched by HLS_LOOKAHEAD_HINT_SEGMENTS there (a hint past
 * this many outstanding is simply dropped, best-effort, by hls_fetch_hint()
 * below, so the two constants drifting apart is harmless, just wasteful in
 * one direction or the other). Kept small deliberately: RAM for a typical
 * live segment (tens to a few hundred KB) is cheap, but this is still a
 * fixed array below (g_slots[]), not a dynamic queue. */
#define HLS_FETCH_LOOKAHEAD_DEPTH 3

/* main <-> worker request (reused; one in flight at a time) */
typedef struct {
    struct Message  msg;
    char            url[MR_HTTP_URL_MAX];
    mr_http_options opts;
    int             has_opts;
    char            post_json[HLS_FETCH_JSON_MAX];
    int             is_post;
    size_t          max_size;
    unsigned        generation;
    int             quit;
    /* result, written by the worker before ReplyMsg (untouched on quit) */
    unsigned char  *buf;
    size_t          len;
    int             ok;
    char            error[HLS_FETCH_ERROR_MAX];
} hls_fetch_request;

/* One queued/in-flight/completed lookahead fetch. Exactly one slot may be
 * INFLIGHT at a time (the single g_req/g_busy request below is shared by
 * both this queue and the anonymous direct-fetch path); the rest sit PENDING
 * until the worker frees up, in the order they were hinted (seq). */
typedef enum {
    HLS_SLOT_EMPTY = 0, HLS_SLOT_PENDING, HLS_SLOT_INFLIGHT, HLS_SLOT_READY
} hls_fetch_slot_state;

typedef struct {
    hls_fetch_slot_state state;
    char            url[MR_HTTP_URL_MAX];
    mr_http_options opts;
    int             has_opts;
    unsigned        seq;             /* hint order, for FIFO promotion        */
    unsigned char  *buf;             /* valid once state == READY             */
    size_t          len;
    int             ok;
    char            error[HLS_FETCH_ERROR_MAX];
} hls_fetch_slot;

static struct Process *g_proc;
static struct MsgPort *g_worker_port;    /* worker's port (worker-owned)      */
static struct MsgPort *g_reply_port;     /* main's port (main-owned)          */
static struct Message  g_ready_msg;      /* worker -> main "I'm up"           */
static hls_fetch_request g_req;
static int              g_busy;          /* g_req sent, reply not reclaimed   */
static hls_fetch_slot   g_slots[HLS_FETCH_LOOKAHEAD_DEPTH]; /* hinted lookahead */
static int              g_inflight_slot; /* index into g_slots, or -1 if the
                                          * in-flight g_req (if any) is the
                                          * anonymous direct-fetch path below  */
static unsigned         g_next_seq;
static int              g_have_ready;    /* anonymous (un-hinted) direct fetch:
                                          * a completed reply is cached        */
static char             g_ready_url[MR_HTTP_URL_MAX];
static unsigned         g_ready_generation;
static unsigned char   *g_ready_buf;
static size_t           g_ready_len;
static int              g_ready_ok;
static char             g_ready_error[HLS_FETCH_ERROR_MAX];
static int              g_active;
static int              g_stopping;         /* suppress lookahead promotion
                                             * while joining the worker      */
static int              g_verbose;
static unsigned         g_generation;
static hls_fetch_service_fn g_service;
static void             *g_service_opaque;
static unsigned long    g_hits, g_misses, g_worst_wait_ms;

/* ---- worker task --------------------------------------------------------- */

static int hls_fetch_download(hls_fetch_request *r)
{
    char *buf = NULL;
    size_t len = 0;
    int ok;
    if (AvailMem(MEMF_ANY) <= HLS_FETCH_MEM_FLOOR) {
        strncpy(r->error, "not enough free memory to fetch",
               sizeof r->error - 1);
        r->error[sizeof r->error - 1] = 0;
        return 0;
    }
    if (r->is_post)
        ok = mr_http_post_json_direct(r->url, r->has_opts ? &r->opts : NULL,
                                      r->post_json, &buf, &len, r->max_size);
    else
        ok = mr_http_fetch_text_direct(r->url, r->has_opts ? &r->opts : NULL,
                                       &buf, &len, r->max_size);
    if (!ok) {
        const char *e = mr_source_last_error();
        strncpy(r->error, e ? e : "fetch failed", sizeof r->error - 1);
        r->error[sizeof r->error - 1] = 0;
        return 0;
    }
    r->buf = (unsigned char *)buf;
    r->len = len;
    return 1;
}

static void hls_fetch_worker(void)
{
    hls_fetch_request *req, *quit_req = NULL;
    /* Create the receive port in this task's context (its signal belongs to
     * us) and tell main we are up - whether or not the port was created. */
    g_worker_port = CreateMsgPort();
    PutMsg(g_reply_port, &g_ready_msg);
    if (!g_worker_port) return;            /* main sees NULL and gives up    */

    for (;;) {
        WaitPort(g_worker_port);
        while ((req = (hls_fetch_request *)GetMsg(g_worker_port)) != NULL) {
            if (req->quit) { quit_req = req; goto done; }
            req->buf = NULL; req->len = 0; req->error[0] = 0;
            req->ok = hls_fetch_download(req);
            ReplyMsg((struct Message *)req);
        }
    }
done:
    /* Consume the cancellation kick before returning through DOS. It is only
     * meaningful while a blocking DNS call has SBTC_BREAKMASK installed. */
    SetSignal(0, SIGBREAKF_CTRL_C);
    /* Release the socket/TLS state this task opened, from this task, before
     * acknowledging the quit - main proceeds to its own mr_http_net_shutdown()
     * as soon as it sees the reply, so ours must be fully done first. */
    mr_http_net_shutdown();
    DeleteMsgPort(g_worker_port);
    g_worker_port = NULL;
    /* Forbid() before the reply so main - woken by it - cannot start tearing
     * down further (freeing state this task's final RemTask still touches)
     * until dos has actually removed this task; only its last Permit-equivalent
     * switches back to main. */
    Forbid();
    ReplyMsg((struct Message *)quit_req);
}

/* ---- main-task helpers --------------------------------------------------- */

/* Forward declaration: hls_fetch_pump() below needs to start a queued
 * lookahead fetch, but hls_fetch_send() (used by every fetch path in this
 * file) is defined further down, alongside the rest of the send/wait
 * helpers it belongs with. */
static void hls_fetch_send(const char *url, const mr_http_options *opts,
                           const char *post_json, size_t max_size);

static void hls_fetch_free_ready(void)
{
    if (g_ready_buf) mr_free(g_ready_buf);
    g_ready_buf = NULL;
    g_have_ready = 0;
}

/* Free any buffered lookahead result and reset the whole queue. Called on
 * teardown (hls_fetch_stop()) and on startup (hls_fetch_start(), in case a
 * previous run's state somehow survived). */
static void hls_fetch_free_slots(void)
{
    int i;
    for (i = 0; i < HLS_FETCH_LOOKAHEAD_DEPTH; i++) {
        if (g_slots[i].state == HLS_SLOT_READY && g_slots[i].buf)
            mr_free(g_slots[i].buf);
        memset(&g_slots[i], 0, sizeof g_slots[i]);
    }
    g_inflight_slot = -1;
}

/* If the worker is free and a lookahead segment is queued, start the
 * earliest-hinted (lowest seq) one. Called after every reclaim so a slot
 * fetch that just completed immediately makes room for the next. */
static void hls_fetch_pump(void)
{
    int i, best = -1;
    if (g_stopping || g_busy) return;      /* no promotion while stopping     */
    for (i = 0; i < HLS_FETCH_LOOKAHEAD_DEPTH; i++)
        if (g_slots[i].state == HLS_SLOT_PENDING &&
            (best < 0 || g_slots[i].seq < g_slots[best].seq))
            best = i;
    if (best < 0) return;
    g_slots[best].state = HLS_SLOT_INFLIGHT;
    g_inflight_slot = best;
    hls_fetch_send(g_slots[best].url,
                  g_slots[best].has_opts ? &g_slots[best].opts : NULL,
                  NULL, HLS_FETCH_HINT_MAX);
}

/* Non-blocking: pull a completed reply into the matching lookahead slot (if
 * the in-flight fetch was one of mr_hls.c's hints) or the anonymous ready
 * cache (a direct, un-hinted fetch), then try to start the next queued
 * lookahead fetch. The one place a reply is ever interpreted - both the
 * quit-ack skip and the stale-generation discard live here, so every caller
 * below (and hls_fetch_stop()'s drain) shares exactly this logic. */
static void hls_fetch_reclaim(void)
{
    struct Message *m;
    if (!g_reply_port) return;
    while ((m = GetMsg(g_reply_port)) != NULL) {
        int slot = g_inflight_slot;
        if (m != &g_req.msg) continue;     /* not our request (can't happen) */
        g_busy = 0;
        g_inflight_slot = -1;
        if (g_req.quit) continue;          /* quit ack: no payload           */
        if (g_req.generation != g_generation) {
            if (g_req.buf) mr_free(g_req.buf);
            if (slot >= 0) g_slots[slot].state = HLS_SLOT_EMPTY;
            continue;
        }
        if (slot >= 0) {
            hls_fetch_slot *s = &g_slots[slot];
            s->state = HLS_SLOT_READY;
            s->buf = g_req.buf;
            s->len = g_req.len;
            s->ok = g_req.ok;
            /* g_req.error is the same fixed size and always already
             * NUL-terminated by whoever filled it (hls_fetch_download()'s
             * own strncpy+explicit-NUL pattern, or req->error[0]=0) - a
             * straight memcpy of the whole buffer avoids a
             * -Wstringop-truncation false positive from strncpy() copying
             * exactly sizeof-1 bytes between two same-sized buffers. */
            memcpy(s->error, g_req.error, sizeof s->error);
        } else {
            hls_fetch_free_ready();
            g_have_ready = 1;
            strncpy(g_ready_url, g_req.url, sizeof g_ready_url - 1);
            g_ready_url[sizeof g_ready_url - 1] = 0;
            g_ready_generation = g_req.generation;
            g_ready_buf = g_req.buf;
            g_ready_len = g_req.len;
            g_ready_ok = g_req.ok;
            memcpy(g_ready_error, g_req.error, sizeof g_ready_error);
        }
    }
    hls_fetch_pump();
}

/* If the ready cache holds `url` at the current generation, consume it
 * (transferring buffer ownership to the caller) and return 1/0 for
 * ok/failed. Returns -1 (cache left untouched) when there is no match. */
static int hls_fetch_take_ready(const char *url, unsigned char **out,
                                size_t *out_len)
{
    int ok;
    if (!g_have_ready || g_ready_generation != g_generation ||
        strcmp(g_ready_url, url) != 0)
        return -1;
    ok = g_ready_ok;
    if (ok) { *out = g_ready_buf; *out_len = g_ready_len; }
    else mr_source_set_error(g_ready_error);
    g_ready_buf = NULL;
    g_have_ready = 0;
    return ok;
}

/* Poll until the outstanding request completes or the service callback asks
 * to abort. Used for a normal (non-teardown) wait: returning early with
 * g_busy still true just means this one call reports failure - the slot
 * stays legitimately busy for whoever asks next. */
static int hls_fetch_wait_busy(void)
{
    clock_t started = g_verbose ? clock() : 0;
    for (;;) {
        hls_fetch_reclaim();
        if (!g_busy) {
            if (g_verbose) {
                unsigned long ms = (unsigned long)
                    ((clock() - started) * 1000UL / CLOCKS_PER_SEC);
                if (ms > g_worst_wait_ms) g_worst_wait_ms = ms;
            }
            return 1;
        }
        if (g_service && g_service(g_service_opaque)) return 0;
        Delay(1);                          /* ~20 ms; stay responsive to ESC */
    }
}

/* Wake a worker that might be wedged inside connect_socket()'s
 * SBTC_BREAKMASK-armed gethostbyname() call (core/mr_http.c) - the one
 * blocking bsdsocket call in this file's whole call chain with no timeout
 * of its own (connect() is bounded by connect_with_timeout(), recv()/send()
 * by SO_RCVTIMEO/SO_SNDTIMEO). Safe to call regardless of what the worker
 * is actually doing right now: its own message loop only ever WaitPort()s
 * on its own port, never on this signal, so this either aborts an in-flight
 * masked DNS lookup immediately or leaves the bit pending harmlessly until
 * connect_socket()'s own SetSignal(0, SIGBREAKF_CTRL_C) clears it before
 * its next attempt. Mirrors vendor/MintAMP/radio_stream.c's
 * Radio_RequestStop()/Radio_RunOnNetWorker(), which use this exact
 * technique for the identical problem. */
static void hls_fetch_kick(void)
{
    if (g_proc) Signal(&g_proc->pr_Task, SIGBREAKF_CTRL_C);
}

/* Poll until the outstanding request completes, servicing playback but
 * IGNORING the service callback's abort request and never abandoning the
 * worker. Used only by hls_fetch_stop(): this worker executes mrplay's own
 * loaded code, so allowing main() to return while it remains live would unload
 * the instructions beneath it. Network operations have their own bounds and
 * hls_fetch_cancel() kicks a masked DNS lookup; the periodic notice keeps a
 * genuinely wedged stack diagnosable without turning it into a delayed crash. */
static void hls_fetch_wait_busy_forever(void)
{
    unsigned long waited = 0, last_notice = 0;
    for (;;) {
        hls_fetch_reclaim();
        if (!g_busy) return;
        if (g_service) g_service(g_service_opaque);
        Delay(1);
        waited += 20;
        if (waited - last_notice >= HLS_FETCH_STOP_NOTICE_MS) {
            last_notice = waited;
            printf("hls-fetch: still waiting for the worker to finish a "
                   "network operation (%lu ms); it will not be abandoned\n",
                   waited);
        }
    }
}

/* Send `url` to the worker (non-blocking). Assumes g_busy is already false -
 * every caller (hls_fetch_pump() and hls_fetch_override()'s direct-fetch
 * fallback) reclaims first, which is what makes that true. post_json NULL
 * sends a GET-shaped fetch. */
static void hls_fetch_send(const char *url, const mr_http_options *opts,
                           const char *post_json, size_t max_size)
{
    strcpy(g_req.url, url);
    if (opts) { g_req.opts = *opts; g_req.has_opts = 1; }
    else g_req.has_opts = 0;
    if (post_json) {
        strncpy(g_req.post_json, post_json, sizeof g_req.post_json - 1);
        g_req.post_json[sizeof g_req.post_json - 1] = 0;
        g_req.is_post = 1;
    } else {
        g_req.post_json[0] = 0;
        g_req.is_post = 0;
    }
    g_req.max_size = max_size;
    g_req.generation = g_generation;
    g_req.quit = 0;
    g_busy = 1;
    PutMsg(g_worker_port, &g_req.msg);
}

/* ---- mr_http backend callbacks (main task) ------------------------------- */

/* Find `url` among the lookahead slots (any state); NULL if untracked. */
static hls_fetch_slot *hls_fetch_find_slot(const char *url)
{
    int i;
    for (i = 0; i < HLS_FETCH_LOOKAHEAD_DEPTH; i++)
        if (g_slots[i].state != HLS_SLOT_EMPTY &&
            strcmp(g_slots[i].url, url) == 0)
            return &g_slots[i];
    return NULL;
}

static int hls_fetch_override(const char *url, const mr_http_options *options,
                              const char *post_json, unsigned char **out,
                              size_t *out_len, size_t max_size)
{
    int r;
    hls_fetch_slot *s;
    if (out) *out = NULL;
    if (out_len) *out_len = 0;
    if (!g_active || !url) return 0;

    hls_fetch_reclaim();

    /* Prefer an already-hinted lookahead slot: READY is an immediate hit.
     * PENDING or INFLIGHT means this exact segment is already queued or
     * being fetched - worth waiting for (pump(), called from every reclaim()
     * below, promotes PENDING to INFLIGHT as soon as the worker frees up)
     * rather than starting a second, competing fetch for the same URL. A
     * sustained backlog (fetches taking longer than segments play out - the
     * jitter this lookahead exists for) is exactly when a slot is still
     * PENDING here, so this has to wait it out rather than skip past it. */
    s = hls_fetch_find_slot(url);
    while (s && (s->state == HLS_SLOT_PENDING || s->state == HLS_SLOT_INFLIGHT)) {
        if (!hls_fetch_wait_busy()) return 0;      /* abort requested        */
        s = hls_fetch_find_slot(url);              /* reclaim() may have
                                                     * moved/freed it above   */
    }
    if (s && s->state == HLS_SLOT_READY) {
        g_hits++;
        r = s->ok;
        if (r) { *out = s->buf; *out_len = s->len; }
        else mr_source_set_error(s->error);
        s->state = HLS_SLOT_EMPTY;
        s->buf = NULL;
        return r;
    }

    /* Untracked (never hinted - e.g. the playlist, which is always fetched
     * on demand, or a segment the lookahead window hadn't reached yet), or
     * its slot fell through above (a stale-generation flush raced it): fall
     * back to the original direct, blocking single-fetch path. */
    r = hls_fetch_take_ready(url, out, out_len);
    if (r >= 0) { g_hits++; return r; }
    hls_fetch_free_ready();                /* stale or mismatched: drop it  */

    if (g_busy) {
        if (!hls_fetch_wait_busy()) return 0;      /* abort requested        */
        r = hls_fetch_take_ready(url, out, out_len);
        if (r >= 0) { g_hits++; return r; }        /* it was already fetching
                                                     * exactly this - a hit  */
        hls_fetch_free_ready();
    }

    g_misses++;
    if (strlen(url) >= sizeof g_req.url) {
        mr_source_set_error("HLS URL too long for the fetch worker");
        return 0;
    }
    if (!max_size || max_size == (size_t)-1) max_size = HLS_FETCH_HINT_MAX;
    hls_fetch_send(url, options, post_json, max_size);
    if (!hls_fetch_wait_busy()) return 0;
    r = hls_fetch_take_ready(url, out, out_len);
    if (r >= 0) return r;
    hls_fetch_free_ready();                /* can't happen, but stay safe   */
    return 0;
}

static void hls_fetch_hint(const char *url, const mr_http_options *options)
{
    int i, slot = -1;
    if (!g_active || !url) return;
    hls_fetch_reclaim();
    if (hls_fetch_find_slot(url)) return;  /* already queued/fetching/ready */
    for (i = 0; i < HLS_FETCH_LOOKAHEAD_DEPTH; i++)
        if (g_slots[i].state == HLS_SLOT_EMPTY) { slot = i; break; }
    if (slot < 0) return;                  /* lookahead window full: best-
                                            * effort, drop this hint         */
    if (strlen(url) >= sizeof g_slots[slot].url) return;
    strcpy(g_slots[slot].url, url);
    if (options) { g_slots[slot].opts = *options; g_slots[slot].has_opts = 1; }
    else g_slots[slot].has_opts = 0;
    g_slots[slot].seq = g_next_seq++;
    g_slots[slot].state = HLS_SLOT_PENDING;
    hls_fetch_pump();
}

/* ---- lifecycle (main task) ------------------------------------------------ */

int hls_fetch_active(void)
{
    return g_active;
}

void hls_fetch_set_service(hls_fetch_service_fn fn, void *opaque)
{
    g_service = fn;
    g_service_opaque = opaque;
}

void hls_fetch_cancel(void)
{
    g_generation++;
    /* Also kick a worker that might be wedged inside connect_socket()'s
     * masked gethostbyname() resolving the very host being cancelled -
     * without this the generation bump above only takes effect once that
     * call eventually returns on its own, which for a slow/unresponsive
     * resolver could be a long time or never. Mirrors
     * vendor/MintAMP/radio_stream.c's Radio_RequestStop(). */
    hls_fetch_kick();
}

void hls_fetch_stats(unsigned long *hits, unsigned long *misses,
                     unsigned long *worst_wait_ms)
{
    if (hits) *hits = g_hits;
    if (misses) *misses = g_misses;
    if (worst_wait_ms) *worst_wait_ms = g_worst_wait_ms;
}

int hls_fetch_start(int verbose)
{
    if (g_active) return 1;
    g_stopping = 0;
    g_verbose = verbose;
    g_generation = 0;
    g_busy = 0; g_have_ready = 0; g_ready_buf = NULL;
    hls_fetch_free_slots();
    g_next_seq = 0;
    g_hits = g_misses = g_worst_wait_ms = 0;
    g_worker_port = NULL;

    g_reply_port = CreateMsgPort();
    if (!g_reply_port) return 0;
    g_ready_msg.mn_Node.ln_Type = NT_MESSAGE;
    g_ready_msg.mn_ReplyPort = NULL;
    g_ready_msg.mn_Length = sizeof g_ready_msg;
    g_req.msg.mn_Node.ln_Type = NT_MESSAGE;
    g_req.msg.mn_ReplyPort = g_reply_port;
    g_req.msg.mn_Length = sizeof g_req;

    g_proc = CreateNewProcTags(
        NP_Entry, (ULONG)hls_fetch_worker,
        NP_Name, (ULONG)"MintVID HLS fetch",
        NP_StackSize, HLS_FETCH_STACK,
        NP_Priority, 0,
        TAG_END);
    if (!g_proc) {
        DeleteMsgPort(g_reply_port);
        g_reply_port = NULL;
        return 0;
    }
    /* One-time startup handshake: safe to block here (raw WaitPort, the one
     * place in this file that isn't a serviced poll) because no playback
     * state exists yet for anything to stay responsive for, and the
     * worker's first action is just CreateMsgPort()+PutMsg - no network I/O
     * is possible before this returns. */
    WaitPort(g_reply_port);
    GetMsg(g_reply_port);
    if (!g_worker_port) {                  /* worker failed and exited       */
        DeleteMsgPort(g_reply_port);
        g_reply_port = NULL;
        g_proc = NULL;
        return 0;
    }
    g_active = 1;
    mr_http_set_fetch_override(hls_fetch_override);
    mr_http_set_prefetch_hint(hls_fetch_hint);
    return 1;
}

void hls_fetch_stop(void)
{
    if (!g_active) {
        if (g_reply_port) { DeleteMsgPort(g_reply_port); g_reply_port = NULL; }
        return;
    }

    /* Freeze the lookahead queue before reclaiming the in-flight request.
     * hls_fetch_reclaim() normally promotes the next pending slot; doing that
     * during ESC teardown needlessly starts more HTTPS work and delays the
     * join. */
    g_stopping = 1;
    hls_fetch_cancel();
    mr_http_set_fetch_override(NULL);
    mr_http_set_prefetch_hint(NULL);

    hls_fetch_reclaim();
    if (g_busy) hls_fetch_wait_busy_forever();
    hls_fetch_free_ready();
    hls_fetch_free_slots();

    g_req.quit = 1;
    g_busy = 1;
    PutMsg(g_worker_port, &g_req.msg);
    hls_fetch_wait_busy_forever();

    g_active = 0;
    g_proc = NULL;
    hls_fetch_free_ready();
    hls_fetch_free_slots();
    DeleteMsgPort(g_reply_port);
    g_reply_port = NULL;
    g_stopping = 0;
}
