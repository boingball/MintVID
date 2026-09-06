/*
 * MintVID - Amiga player.
 *
 * Ties the proven portable core (demux + decoder) to the Amiga display + audio
 * backends: load file -> auto-detect container -> decode frames / enqueue audio
 * -> blit, with audio as the A/V master clock (video frames are held until
 * Paula playback reaches their timestamp). Falls back to frame-rate pacing when
 * there is no audio. ESC or the close gadget quits.
 *
 * AVI, MOV/MP4 and MPEG-TS containers are file-backed: only metadata and the
 * current compressed packet live in RAM. Raw elementary streams and MPEG-1
 * retain the original whole-file fallback.
 *
 *   mrplay <file.avi|file.mov>
 */
#include "../core/mr_demux.h"
#include "../core/mr_hls.h"
#include "../core/mr_http.h"
#include "../core/mr_youtube.h"
#include "hls_fetch.h"
#include "../core/mr_codec.h"
#include "../core/mr_rawvideo.h"
#include "../core/mr_mpeg1.h"
#include "../core/mr_h264.h"
#include "../core/mr_dither.h"
#include "../core/mr_media_clock.h"
#include "../core/mr_yuv.h"
#include "../core/mr_yuv_dither.h"
#include "../core/mr_yuv_ham.h"
#include "../audio/mr_audio_decode.h"
#include "../iptv/mr_iptv.h"
#include "amiga_display.h"
#include "mr_audio.h"
#include "mr_player_status.h"
#include "mintvid_version.h"

MINTVID_DECLARE_VERSION(mrplay_version_tag, "mrplay");

#include <proto/dos.h>
#include <proto/exec.h>
#include <proto/timer.h>
#include <clib/alib_protos.h>
#include <devices/timer.h>
#include <exec/memory.h>
#include <exec/nodes.h>
#include <exec/ports.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/*
 * libavc's P/B-slice reference-list setup has stack frames above 20 KiB
 * before its callers and the AmigaOS libraries are accounted for.  Classic
 * Shells commonly provide only 4 KiB, which corrupts memory during H.264
 * playback and makes the eventual EOF/ESC teardown appear to crash.  AmigaOS
 * versions with stack-cookie support raise the process stack to this minimum;
 * older systems can use "Stack 320000" before launching mrplay.
 */
static const char mr_min_stack[] __attribute__((used)) = "$STACK:320000";

/*
 * mrplay handles Ctrl-C / Ctrl-F itself: the IPTV controller signals them to
 * ask the player to stop, and the main loop (control_signal_event) turns them
 * into a clean quit that closes the display and Paula. Left to libnix, a
 * pending Ctrl-C during any stdio call - frequent when --time logging is on -
 * fires its "***Break" handler and exit()s the process mid-printf, skipping
 * CloseWindow() and leaving an orphaned RTG window stuck on screen. An empty
 * __chkabort() disables that automatic abort; the break signals still arrive as
 * exec signals and are serviced by our own event loop.
 */
void __chkabort(void) { }

/* Upper ceiling on the decoded-frame ring - the size of the vq[] array only.
 * The ACTIVE ring is video_cap (chosen at runtime from the audio cushion and
 * free RAM, see main), and video_cap is the modulus for all qhead arithmetic,
 * so only video_cap slots are ever touched and the RGB footprint is exactly
 * video_cap * frame_bytes. (Modulo the whole array instead and qhead would cycle
 * every slot, allocating an RGB buffer in each - the footprint would be this
 * ceiling * frame_bytes regardless of video_cap, which once ate all of Fast RAM.)
 * The array structs are tiny; only the lazily-allocated RGB buffers cost. */
#define VIDEO_QUEUE_CAP 48
/* Default decoded-frame depths, clamped to free RAM and raisable by --net-queue.
 * In the video-ahead regime the presented rate is about depth / cushion_seconds
 * (topping up the audio cushion discards frames decoded past the cap), so a
 * deeper queue presents more before each gap: measured ~5 fps at depth 14 vs
 * ~2 fps at depth 4 against the 2.5 s cushion. Keep it as deep as RAM allows. */
#define VIDEO_QUEUE_NET_DEPTH  16
#define VIDEO_QUEUE_DISK_DEPTH 16
/* Never let the decoded queue eat RAM below this: it must leave room for the
 * segment fetch, decoder and TLS. An over-deep queue once filled Fast RAM to
 * the safety floor and starved playback into an endless reconnect (see the
 * runtime sizing in main). */
#define VIDEO_QUEUE_MEM_FLOOR (8UL * 1024 * 1024)
#define STATS_INTERVAL_US 3000000ULL
/* clock-trace fires on every clock-source flip/large-delta/multi-drop frame,
 * which during a genuine overload can mean every single frame. On real Amiga
 * hardware each printf is a DOS Write() that competes with the same 33 ms
 * budget it is trying to diagnose, so cap it to a few lines/second - still
 * enough to see the trend without adding load while --time is capturing it. */
#define CLOCK_TRACE_MIN_INTERVAL_US 200000ULL
#define PRESENTATION_GUARD_US 4000ULL
#define MR_SEEK_STEP_US 10000000LL  /* cursor-left/right: jump 10 s          */
#define AUDIO_REFILL_WARNING_MS 120UL
/* ENTRY/TARGET/ONE_REQUEST are "one request's worth" / "both hardware
 * requests' worth" of buffered audio - defined in terms of Paula's
 * PAULA_REQUEST_MS (audio_paula.c), currently 200ms per request, NBUF=2.
 * Keep these at 1x/2x/1x that value if it ever changes again. */
#define AUDIO_RESCUE_ENTRY_MS 200UL
#define AUDIO_RESCUE_TARGET_MS 400UL
#define AUDIO_RESCUE_FIFO_NEAR_EMPTY_MS 20UL
#define AUDIO_RESCUE_ONE_REQUEST_MS 200UL
#define AUDIO_STARTUP_TARGET_MS 400UL
#define AUDIO_CUSHION_MIN_MS 400UL
/* 2.5 s is the legacy/direct-path ceiling: enough to ride the ~1.7 s stalls
 * seen in hardware logs, but a decoded video ring shallower than the
 * corresponding number of frames cannot stay smooth while filling that much
 * audio. For synchronous network playback the active cushion is therefore
 * reduced after video_cap is known to roughly (video_cap - 2) frame periods,
 * clamped between AUDIO_CUSHION_MIN_MS and this ceiling. Local files keep the
 * ceiling. */
#define AUDIO_CUSHION_TARGET_MS 2500UL
/* Live-resync (opt-in, --live-resync, network sources only). A multi-second
 * network stall can leave a live stream many seconds behind the wall clock with
 * the audio clock unable to climb back; these bound the catch-up-to-live burst.
 * The trigger is far above any jitter normal playback produces. */
#define LIVE_RESYNC_BEHIND_US 4000000ULL  /* >4 s behind wall clock: catch up  */
#define LIVE_RESYNC_TARGET_US 2500000ULL  /* aim to land ~2.5 s behind the edge:
                                           * a deliberate latency penalty (vs.
                                           * the old ~1 s) buys real margin
                                           * against the next HLS segment-fetch
                                           * stall instead of landing right back
                                           * at the edge with none - a judder-
                                           * free 2-3 s behind live beats a
                                           * tight chase that resyncs again a
                                           * few segments later               */
#define LIVE_RESYNC_MAX_US    3000000ULL  /* hard cap on one catch-up burst     */
#define LIVE_RESYNC_EDGE_US   2000000ULL  /* a read slower than any normal fetch
                                           * means we have caught the frontier   */
#define LIVE_RECONNECT_TRIES  200         /* reopen attempts before giving up;
                                           * with backoff this is ~15 min, enough
                                           * to ride out a flaky mobile link      */
#define LIVE_RECONNECT_STALL_LIMIT 3      /* consecutive reopens with no playback
                                           * before giving up (avoids a spin)     */
#define AUDIO_RESCUE_MAX_PACKETS 16U
/* Was 100000 (100ms) - left unscaled when AUDIO_RESCUE_ENTRY_MS/TARGET_MS
 * doubled to match audio_paula.c's PAULA_REQUEST_MS going to 200ms, which
 * was a mistake: a WinUAE log with that mismatch still showed every rescue
 * episode exiting via "limit" (exit(target/limit/eof)=0/26/0, same as
 * before) and hw-starvations climbing just as fast as pre-doubling (~15/s)
 * - widening AUDIO_RESCUE_TARGET_MS to 400ms without widening the time
 * allowed to reach it left rescue no better able to actually fill the
 * bigger buffers; the real buffered-audio level in that log never moved
 * off the old ~100-150ms range. Doubled to match, so one episode has the
 * same proportional chance of reaching the new target it had of reaching
 * the old one. Trade-off: a rescue episode can now hold the CPU for up to
 * 200ms instead of 100ms, competing more with video decode/demux during
 * that window - re-tune here if a real Amiga/WinUAE pass shows that
 * costing more than the starvations it fixes. */
#define AUDIO_RESCUE_MAX_US 200000ULL

static struct MsgPort *timer_port;
static struct timerequest *timer_request;
struct Device *TimerBase;

/* Control port: published on exec's public port list so the IPTV controller can
 * discover this player and signal it (Ctrl-F clean stop, Ctrl-C forced stop)
 * before launching another stream. We never receive messages through it, only
 * signals aimed at mp_SigTask, so it is created PA_IGNORE. The port carries an
 * embedded status block (see mr_player_status.h) so the controller can read
 * back the codec that is playing, or why a stream refused to play. */
static mr_player_control_port *control_block;
static mr_audio *control_audio;
static int control_volume = 64;
static int deferred_player_event;
/* Set once, first thing in main(). service_player_during_io() checks this
 * to refuse to touch display/audio/queue state when called from any task
 * other than this one - see that function's comment for why that check is
 * required, not optional, once the HLS fetch worker exists. */
static struct Task *g_main_task;
static ULONG status_file_seq;
static int playing_status_published;
static char playing_status_codec[MR_PLAYER_CODEC_MAX];
static char playing_status_text[MR_PLAYER_STATUS_TEXT_MAX];
/* Elapsed playback position last folded into the published status text, in
 * the current frame's own timeline (pts_us) - not wall-clock time, so it
 * still tracks correctly under seeks and the Turbo/Turbo+ H.264 speed
 * modes. -1 means "nothing published yet". */
static int64_t position_status_last_pts_us = -1;

/* One-shot high-resolution pipeline breadcrumb. It advances only when a new
 * stage is reached, so diagnostics cannot turn every 1080p frame into DOS I/O. */
static int h264_pipeline_diag_enabled;
static int h264_pipeline_stage;
static int h264_pipeline_width, h264_pipeline_height;

static void h264_pipeline_checkpoint_player(const char *stage, int qcount,
                                            int playback_started)
{
    BPTR fh;
    char buf[256];
    int n;
    ULONG fast_total, fast_largest;

    if (!h264_pipeline_diag_enabled || !stage) return;
    fh = Open((CONST_STRPTR)"RAM:MintVID-H264.pipeline", MODE_NEWFILE);
    if (!fh) return;
    fast_total = AvailMem(MEMF_FAST);
    fast_largest = AvailMem(MEMF_FAST | MEMF_LARGEST);
    n = snprintf(buf, sizeof buf,
                 "stage=%s res=%dx%d qcount=%d playback=%d fast=%lu "
                 "fast_largest=%lu\n",
                 stage, h264_pipeline_width, h264_pipeline_height, qcount,
                 playback_started, (unsigned long)fast_total,
                 (unsigned long)fast_largest);
    if (n > 0) {
        LONG bytes = n < (int)sizeof buf ? (LONG)n : (LONG)(sizeof buf - 1);
        Write(fh, (APTR)buf, bytes);
    }
    Close(fh);
}

static void control_port_close(void)
{
    if (control_block) {
        RemPort(&control_block->port);
        FreeMem(control_block, sizeof *control_block);
        control_block = NULL;
    }
}

static void control_port_open(void)
{
    mr_player_control_port *block;
    /* Only one player should ever be live. If a port already exists another
     * player is still shutting down; skip rather than publish a duplicate. */
    Forbid();
    if (FindPort((CONST_STRPTR)MR_IPTV_PLAYER_PORT)) { Permit(); return; }
    Permit();
    block = (mr_player_control_port *)AllocMem(sizeof *block,
                                               MEMF_PUBLIC | MEMF_CLEAR);
    if (!block) return;
    block->port.mp_Node.ln_Type = NT_MSGPORT;
    block->port.mp_Node.ln_Name = (char *)MR_IPTV_PLAYER_PORT;
    block->port.mp_Flags = PA_IGNORE;
    block->port.mp_SigTask = FindTask(NULL);
    NewList(&block->port.mp_MsgList);
    block->status.state = MR_PLAYER_STATE_STARTING;
    block->status.magic = MR_PLAYER_STATUS_MAGIC;
    AddPort(&block->port);
    control_block = block;
    atexit(control_port_close);
}

/* Publish a status update for the IPTV controller (no-op if the port was not
 * created). Thin wrapper so call sites read cleanly. */
static void player_status(LONG state, const char *codec, const char *text)
{
    mr_player_status_snapshot snapshot;
    BPTR file;
    mr_player_status_set(control_block, state, codec, text);
    memset(&snapshot, 0, sizeof snapshot);
    snapshot.status.magic = MR_PLAYER_STATUS_MAGIC;
    snapshot.status.seq = ++status_file_seq;
    snapshot.status.state = state;
    mr_player_status_copy(snapshot.status.codec,
                          sizeof snapshot.status.codec, codec ? codec : "");
    mr_player_status_copy(snapshot.status.text,
                          sizeof snapshot.status.text, text ? text : "");
    snapshot.trailer_magic = MR_PLAYER_STATUS_MAGIC;
    file = Open((CONST_STRPTR)MR_PLAYER_STATUS_TMP, MODE_NEWFILE);
    if (file) {
        LONG written = Write(file, &snapshot, (LONG)sizeof snapshot);
        Close(file);
        if (written == (LONG)sizeof snapshot) {
            DeleteFile((CONST_STRPTR)MR_PLAYER_STATUS_FILE);
            if (!Rename((CONST_STRPTR)MR_PLAYER_STATUS_TMP,
                        (CONST_STRPTR)MR_PLAYER_STATUS_FILE))
                DeleteFile((CONST_STRPTR)MR_PLAYER_STATUS_TMP);
        } else {
            DeleteFile((CONST_STRPTR)MR_PLAYER_STATUS_TMP);
        }
    }
}

/* Same as player_status() but skips the T: snapshot's Open/Write/Close/
 * Rename - that disk I/O only exists so a terminal state (error/unsupported/
 * ended) survives this process exiting before the controller reads it. A
 * periodic position update while playback is healthy has no such need, and
 * doing that disk round trip once a second during decode is exactly the
 * kind of avoidable tax on a slow floppy/HDD-based Amiga this player is
 * otherwise careful not to add. */
static void player_status_live(const char *codec, const char *text)
{
    mr_player_status_set(control_block, MR_PLAYER_STATE_PLAYING, codec, text);
}

static void player_prepare_playing_status(const char *codec, const char *text)
{
    mr_player_status_copy(playing_status_codec,
                          sizeof playing_status_codec, codec ? codec : "");
    mr_player_status_copy(playing_status_text,
                          sizeof playing_status_text, text ? text : "");
    playing_status_published = 0;
    position_status_last_pts_us = -1;
}

/* Folds an "H:MM:SS"/"M:SS" playhead onto the end of the fixed stream
 * description and republishes it (live only, see player_status_live()) for
 * the IPTV/YouTube/main controllers' status line, and onto the video
 * window's own title bar via display_set_status() - so the playhead is
 * visible even with the controller window elsewhere. Throttled to about
 * once a second of the video's own timeline, so this is a once-a-second
 * string format plus a Forbid()/Permit() and one SetWindowTitles(), not a
 * per-frame cost. pts_us < 0 means the caller has no usable timestamp here
 * (e.g. the H.264 EOF drain loop, which only ever handles the last frame or
 * two) and just wants the one-time "now playing" latch below. */
static void player_update_position(amiga_display *disp, int64_t pts_us)
{
    int64_t diff;
    unsigned long secs, h, m, s;
    /* Wider than MR_PLAYER_STATUS_TEXT_MAX on purpose: gcc's -Wformat-
     * truncation can't see that m/s are always 0-59, so it sizes its worst
     * case off the full width of an unsigned long - give it enough headroom
     * to prove this can't truncate. player_status_live() -> mr_player_
     * status_set() safely truncates again to the real 208-byte field, same
     * as every other status publish in this file. */
    char line[MR_PLAYER_STATUS_TEXT_MAX + 32];
    char title[48];

    if (pts_us < 0) return;
    diff = pts_us - position_status_last_pts_us;
    if (diff < 0) diff = -diff;
    if (position_status_last_pts_us >= 0 && diff < 1000000) return;
    position_status_last_pts_us = pts_us;

    secs = (unsigned long)(pts_us / 1000000);
    h = secs / 3600; m = (secs / 60) % 60; s = secs % 60;
    if (h) {
        snprintf(line, sizeof line, "%s | %lu:%02lu:%02lu",
                 playing_status_text, h, m, s);
        snprintf(title, sizeof title, "MintVID - %lu:%02lu:%02lu", h, m, s);
    } else {
        snprintf(line, sizeof line, "%s | %lu:%02lu",
                 playing_status_text, m, s);
        snprintf(title, sizeof title, "MintVID - %lu:%02lu", m, s);
    }
    player_status_live(playing_status_codec, line);
    /* A no-op on AGA (no .status hook, and no window title bar to update in
     * the first place) and harmless if a transient message (Buffering...,
     * Seeking...) is showing - this only ever runs right after a frame was
     * actually presented, which by definition means any such stall already
     * cleared, so there is nothing to clobber. */
    display_set_status(disp, title);
}

static void player_first_frame_presented(amiga_display *disp, int64_t pts_us)
{
    if (!playing_status_published) {
        playing_status_published = 1;
        player_status(MR_PLAYER_STATE_PLAYING, playing_status_codec,
                      playing_status_text);
    }
    player_update_position(disp, pts_us);
}

/* Keep a failure status readable by the IPTV controller for a moment before we
 * exit, but bail out promptly if the controller asks us to stop. The controller
 * polls a few times a second, so a short hold is enough to be seen. */
static void status_hold(void)
{
    int i;
    for (i = 0; i < 20; i++) { /* up to ~2 s at 1/10 s ticks */
        if (SetSignal(0, 0) & (SIGBREAKF_CTRL_C | SIGBREAKF_CTRL_F))
            break;
        Delay(5);
    }
}

static void playback_timer_close(void);

static int playback_timer_open(void)
{
    timer_port = CreateMsgPort();
    if (!timer_port) return 0;
    timer_request = (struct timerequest *)CreateIORequest(timer_port,
                                                          sizeof *timer_request);
    if (!timer_request) {
        playback_timer_close();
        return 0;
    }
    if (OpenDevice((CONST_STRPTR)TIMERNAME, UNIT_MICROHZ,
                   (struct IORequest *)timer_request, 0) != 0) {
        playback_timer_close();
        return 0;
    }
    TimerBase = timer_request->tr_node.io_Device;
    return 1;
}

static void playback_timer_close(void)
{
    if (TimerBase && timer_request)
        CloseDevice((struct IORequest *)timer_request);
    TimerBase = NULL;
    if (timer_request) DeleteIORequest((struct IORequest *)timer_request);
    if (timer_port) DeleteMsgPort(timer_port);
    timer_request = NULL; timer_port = NULL;
}

/* Every main() exit, including resolver/stream failures, must release Amiga
 * device and library state explicitly. Repeated failed launches otherwise
 * leave timer.device requests and, on some libnix setups, socket/AmiSSL state
 * behind until reboot. All three shutdowns are idempotent. */
static int mrplay_exit(int code)
{
    playback_timer_close();
    /* Stop and join the fetch worker before releasing this task's own
     * bsdsocket/AmiSSL state. hls_fetch_stop() cannot abandon the worker:
     * its entry point lives in this executable's segment, so main() must not
     * return until that task has acknowledged a complete owner-task cleanup. */
    hls_fetch_stop();
    mr_http_net_shutdown();
    control_port_close();
    return code;
}

/* Clamp a signed microsecond value to a non-negative uint64_t. */
static uint64_t s64_to_us(int64_t v) { return v > 0 ? (uint64_t)v : 0; }

typedef struct queued_video {
    unsigned char *rgb;
    size_t capacity;
    int width, height, stride, dirty_y0, dirty_y1;
    uint64_t pts_us;
    uint64_t decoded_at_us;
} queued_video;

typedef struct playback_stats {
    uint64_t since_us, network_us, demux_us, audio_decode_us, video_decode_us;
    uint64_t convert_us, scale_us, display_us, sleep_requested_us, sleep_actual_us;
    uint64_t latency_us, refill_block_us, refill_delayed_ready_us;
    unsigned long video_decode_max_us, display_max_us, sleep_max_error_us;
    unsigned decoded, presented, late, dropped, samples;
    uint64_t rtg_prepare_us, rtg_scale_us, rtg_convert_us, rtg_copy_us;
    uint64_t rtg_blit_us, rtg_clip_us, rtg_total_us;
    unsigned long rtg_prepare_max_us, rtg_blit_max_us;
    uint64_t h264_input_us, h264_core_us, h264_output_us;
    unsigned long h264_input_max_us, h264_core_max_us, h264_output_max_us;
    uint64_t h264_mc_us, h264_deblock_us, h264_recon_us, h264_intra_us;
    unsigned long h264_mc_max_us, h264_deblock_max_us, h264_recon_max_us;
    unsigned long h264_intra_max_us;
    uint64_t rescue_us;
    unsigned rescue_entries, rescue_packets, rescue_audio_packets;
    unsigned rescue_video_decoded, rescue_video_queued, rescue_video_skipped;
    unsigned rescue_critical, rescue_noncritical, rescue_video_replaced;
    uint64_t rescue_newest_retained_pts_us;
    int64_t rescue_post_lateness_us;
    unsigned long rescue_max_us;
    unsigned rescue_exit_target, rescue_exit_limit, rescue_exit_eof;
    long rescue_min_margin_ms;
    unsigned rescue_negative_margin, rescue_hw_starvations;
    unsigned dropped_after_scale;
    unsigned long audio_before, audio_after;
    uint64_t frame_pts_us, audio_clock_us;
    int64_t calculated_lateness_us;
    unsigned queue_head, dropped_in_pass, timing_rebases;
    mr_display_timing last_rtg;
    uint64_t yuv_indexed_us;
    unsigned long yuv_indexed_max_us;
    unsigned yuv_indexed_frames;
    uint64_t yuv_rgb_us;
    unsigned long yuv_rgb_max_us;
    unsigned yuv_rgb_frames;
} playback_stats;

/*
 * Present-during-fetch context.
 *
 * The player is single-threaded: while it is parked in a blocking segment fetch
 * (mr_demux_next_packet -> HLS segment open/read) the main loop cannot reach its
 * own presentation path, so without help the picture freezes for the whole
 * fetch. The demux/decoder/display service hooks already pump
 * service_audio_for_display() throughout that stall to keep Paula fed; this
 * struct lets that same callback also advance video, presenting frames from the
 * already-decoded queue as they fall due against the audio clock.
 *
 * This is NOT a second presenter racing the loop. The main loop hands the queue
 * to the callback only for the duration of the blocking read (vp->released) and
 * re-reads qhead/qcount afterwards, so exactly one side ever touches the queue
 * at a time - single task, no lock, nothing to kill on teardown. `presenting`
 * guards the reentrancy a blit's own service pump would otherwise cause. All
 * fields point at the scheduler's live locals so the two paths share one queue.
 */
typedef struct video_presenter {
    int                  released;      /* main loop blocked: callback may present */
    int                  presenting;    /* reentrancy guard (a blit re-enters us)  */
    amiga_display       *disp;
    mr_audio            *audio;
    media_clock         *mc;
    const mr_video_info *vi;
    queued_video        *vq;
    int                  video_cap;        /* ring modulus - see main()'s sizing     */
    int                 *qhead, *qcount;
    int                 *playback_started;
    uint64_t            *mono_base_us;
    int                 *frames;
    playback_stats      *stats;
    int                  want_time;
    /* Set once from display_supports_indexed() after display_open(). When set,
     * the queue holds pre-dithered one-byte palette indices (queue_copy_indexed)
     * instead of RGB24, and this presenter must blit them with
     * display_show_indexed() instead of display_show_rgb(). */
    int                  use_indexed;
    /* P96 native BGR queue: present with display_show_bgr24(). */
    int                  use_bgr;
} video_presenter;

typedef struct scheduler_trace {
    mr_audio *audio;
    const char *phase, *previous_phase;
    uint64_t phase_started_us, previous_duration_us, last_service_us;
    uint64_t sleep_requested_us, sleep_actual_us;
    uint64_t last_clock_trace_us;
    uint64_t last_rescue_print_us;
    unsigned long delay_ticks;
    int enabled;
    video_presenter *presenter;        /* NULL until the scheduler wires it up   */
} scheduler_trace;

static uint64_t monotonic_us(void);

static void trace_phase(scheduler_trace *trace, const char *phase)
{
    uint64_t now;
    /* trace->phase/phase_started_us/previous_phase/previous_duration_us are
     * only ever read by service_audio_for_display's --time audio-gap
     * message - never by real scheduling/pacing logic - so there is no
     * reason to pay for a monotonic_us() (ReadEClock + 64-bit divide) on
     * every one of this function's many per-frame call sites when --time
     * was not requested. */
    if (!trace || !trace->enabled) return;
    now = monotonic_us();
    if (trace->phase) {
        trace->previous_phase = trace->phase;
        trace->previous_duration_us = now - trace->phase_started_us;
    }
    trace->phase = phase;
    trace->phase_started_us = now;
}

/*
 * Present the queue's front frame if it is due, from inside a service pump.
 *
 * Runs only while the main loop has released the queue (vp->released) for a
 * blocking fetch, so it never contends with the loop's own presentation path.
 * It mirrors the scheduler's media-clock derivation and its "drop frames more
 * than a period overdue, keep the newest" policy, so presenting here looks
 * identical to presenting from the loop - it just happens while the one task is
 * otherwise stuck in recv(). At most one frame is blitted per call; the loop
 * re-reads qhead/qcount when the fetch returns. `presenting` swallows the nested
 * service call display_show_rgb's own pump triggers.
 */
static void present_service_frame(video_presenter *vp)
{
    media_clock *mc;
    uint64_t period_us, master_clock_us, now;
    int64_t late_us;
    queued_video *front;

    if (!vp || !vp->released || vp->presenting) return;
    if (!*vp->playback_started || *vp->qcount <= 0) return;
    vp->presenting = 1;

    mc = vp->mc;
    now = monotonic_us();
    period_us = vp->vi->rate
        ? (uint64_t)(vp->vi->scale ? vp->vi->scale : 1) * 1000000ULL / vp->vi->rate
        : 83333ULL;

    /* Same master clock the scheduler uses: the audio clock when present (with
     * its starvation holdover), the monotonic fallback otherwise. */
    if (vp->audio)
        master_clock_us = current_media_clock_us(mc, 1, audio_starved(vp->audio),
                                                 audio_elapsed_us(vp->audio),
                                                 vp->want_time);
    else {
        mc->source = MCLOCK_MONO;
        master_clock_us = now - *vp->mono_base_us;
    }

    front = &vp->vq[*vp->qhead];
    late_us = (int64_t)master_clock_us - (int64_t)front->pts_us;

    /* Front not due yet: leave it for a later service tick or the loop. */
    if (late_us < -(int64_t)PRESENTATION_GUARD_US) { vp->presenting = 0; return; }

    /* Resync to the clock rather than replay a backlog when the stall clears:
     * drop frames a whole period or more overdue, keeping the newest. */
    while (late_us > (int64_t)period_us && *vp->qcount > 1) {
        vp->stats->dropped++;
        *vp->qhead = (*vp->qhead + 1) % vp->video_cap;
        (*vp->qcount)--;
        front = &vp->vq[*vp->qhead];
        late_us = (int64_t)master_clock_us - (int64_t)front->pts_us;
    }
    if (late_us > 0) vp->stats->late++;

    if (h264_pipeline_diag_enabled && h264_pipeline_stage < 4) {
        h264_pipeline_checkpoint_player("pre-display-service",
                                        *vp->qcount, *vp->playback_started);
        h264_pipeline_stage = 4;
    }
    if (vp->use_indexed)
        display_show_indexed(vp->disp, front->rgb, front->width, front->height,
                             front->stride, front->dirty_y0, front->dirty_y1);
    else if (vp->use_bgr)
        display_show_bgr24(vp->disp, front->rgb, front->width, front->height,
                           front->stride, front->dirty_y0, front->dirty_y1);
    else
        display_show_rgb(vp->disp, front->rgb, front->width, front->height,
                         front->stride, front->dirty_y0, front->dirty_y1);
    player_first_frame_presented(vp->disp, (int64_t)front->pts_us);
    if (h264_pipeline_diag_enabled && h264_pipeline_stage < 5) {
        h264_pipeline_checkpoint_player("post-display-service",
                                        *vp->qcount, *vp->playback_started);
        h264_pipeline_stage = 5;
    }
    /* latency_us only ever feeds the --time report's "latency=" figure. */
    if (vp->want_time)
        vp->stats->latency_us += monotonic_us() - front->decoded_at_us;
    vp->stats->presented++;
    if (vp->frames) (*vp->frames)++;
    *vp->qhead = (*vp->qhead + 1) % vp->video_cap;
    (*vp->qcount)--;
    vp->presenting = 0;
}

static void service_audio_for_display(void *opaque)
{
    scheduler_trace *trace = (scheduler_trace *)opaque;
    /* now/last_service_us only ever feed the --time audio-gap diagnostic
     * below - monotonic_us() is a ReadEClock() call plus a 64-bit tick-to-
     * microsecond divide, and this callback runs from deep inside the YUV
     * conversion loop (about 22 times per decoded frame at 360p), so paying
     * for two of those per call on a normal (non---time) run is a real,
     * pointless tax on a 50 MHz 060. Skip both when nobody is watching. */
    if (trace->enabled) {
        uint64_t now = monotonic_us();
        /* monotonic_us() can land one tick's worth of rounding behind its
         * own previous reading (EClock ticks converted with integer
         * division). Guard the subtraction so that never-quite-monotonic
         * hair does not underflow into an "impossible" multi-day gap - as
         * seen in the field as audio-gap=1271310319 ms, i.e. (uint64_t)-1us
         * truncated to 32 bits. */
        if (trace->last_service_us && now > trace->last_service_us &&
            now - trace->last_service_us > 40000ULL) {
            printf("audio-gap=%lu ms phase=%s phase-duration=%lu ms previous-phase=%s "
                   "previous-duration=%lu ms sleep-request=%lu ms "
                   "sleep-actual=%lu ms delay-ticks=%lu\n",
                   (unsigned long)((now - trace->last_service_us) / 1000),
                   trace->phase ? trace->phase : "unknown",
                   (unsigned long)((now - trace->phase_started_us) / 1000),
                   trace->previous_phase ? trace->previous_phase : "none",
                   (unsigned long)(trace->previous_duration_us / 1000),
                   (unsigned long)(trace->sleep_requested_us / 1000),
                   (unsigned long)(trace->sleep_actual_us / 1000),
                   trace->delay_ticks);
        }
        audio_service(trace->audio);
        trace->last_service_us = monotonic_us();
    } else {
        audio_service(trace->audio);
    }
    /* Audio first (it is the master clock), then advance video against it if the
     * scheduler has released the queue for a blocking fetch. */
    if (trace->presenter) present_service_frame(trace->presenter);
}

/* EClock is per-machine monotonic and normally much finer than the 20 ms DOS
 * tick. Keeping all scheduling in integer microseconds avoids truncating a
 * 25 fps period into alternating/coarse Delay() ticks. */
static uint64_t monotonic_us(void)
{
    struct EClockVal value;
    ULONG frequency = TimerBase ? ReadEClock(&value) : 0;
    uint64_t ticks = ((uint64_t)value.ev_hi << 32) | value.ev_lo;
    return frequency ? ticks * 1000000ULL / frequency :
           (uint64_t)clock() * 1000000ULL / CLOCKS_PER_SEC;
}

static void paced_sleep(uint64_t usec, scheduler_trace *trace,
                        playback_stats *st)
{
    uint64_t begin, end;
    if (!usec) return;
    begin = monotonic_us();
    trace_phase(trace, "paced-sleep");
    trace->sleep_requested_us = usec;
    trace->sleep_actual_us = 0;
    trace->delay_ticks = 0;
    st->sleep_requested_us += usec;
    /* Delay is only the coarse backoff. Recheck the absolute deadline so an
     * oversleep is measured rather than carried into the next frame.
     * service_audio_for_display() calls used to bracket this spin, but
     * presenter.released (the only thing that makes that call do anything
     * beyond a no-op audio_service()) is set only around the one blocking
     * mr_demux_next_packet() call far below - never while pacing a sleep -
     * so they never presented a frame or fed audio here; Paula's own
     * worker task keeps running regardless. See trace_phase()'s own
     * comment for the matching monotonic_us() reasoning. */
    while ((end = monotonic_us()) - begin < usec) {
        uint64_t left = usec - (end - begin);
        /* A one-tick Delay is a forced 20 ms oversleep for short deadlines.
         * Spin on EClock until at least two ticks remain. */
        if (left > 40000) {
            LONG ticks = (LONG)((left - 20000) / 20000);
            uint64_t delay_begin = monotonic_us();
            if (ticks < 1) ticks = 1;
            trace->delay_ticks = (unsigned long)ticks;
            Delay(ticks);
            trace->sleep_actual_us += monotonic_us() - delay_begin;
        }
    }
    end = monotonic_us();
    st->sleep_actual_us += end - begin;
    trace->sleep_actual_us = end - begin;
    if (end - begin > usec && end - begin - usec > st->sleep_max_error_us)
        st->sleep_max_error_us = (unsigned long)(end - begin - usec);
}

static int queue_copy(queued_video *q, const mr_frame *fr, uint64_t pts,
                      uint64_t decoded_at)
{
    size_t bytes = (size_t)fr->stride * fr->height;
    if (q->capacity < bytes) {
        unsigned char *p = (unsigned char *)realloc(q->rgb, bytes);
        if (!p) return 0;
        q->rgb = p; q->capacity = bytes;
    }
    memcpy(q->rgb, fr->data, bytes);
    q->width = fr->width; q->height = fr->height; q->stride = fr->stride;
    q->dirty_y0 = fr->dirty_y0; q->dirty_y1 = fr->dirty_y1;
    q->pts_us = pts; q->decoded_at_us = decoded_at;
    return 1;
}

/* H.264 RGB-display path: convert libavc's borrowed YUV420P planes directly
 * into the queue slot.  The old path converted into mr_h264.c's persistent
 * full-frame RGB buffer and immediately memcpy'd that whole buffer here.
 * Writing the conversion result straight to q->rgb removes that extra RGB
 * framebuffer and one complete RGB24 read+write pass per queued frame while
 * preserving the display-facing queue format and all CGX/P96 code unchanged.
 * It also defers conversion until after the scheduler's retain/drop decision,
 * so an overload frame discarded before queueing never pays RGB conversion.
 * The service hook is the same audio/presenter callback formerly used inside
 * mr_h264.c's conversion and remains safe here: the queue is not published
 * (qcount is not incremented) until this function returns. */
static int queue_copy_yuv_rgb24(queued_video *q, const mr_frame *fr,
                                uint64_t pts, uint64_t decoded_at, int bgr,
                                mr_yuv_service_fn service,
                                void *service_opaque)
{
    size_t stride, bytes;
    if (!q || !fr || fr->fmt != MR_PIX_YUV420P ||
        !fr->data || !fr->u_data || !fr->v_data ||
        fr->width <= 0 || fr->height <= 0)
        return 0;
    if ((size_t)fr->width > (size_t)-1 / 3u) return 0;
    stride = (size_t)fr->width * 3u;
    if ((size_t)fr->height > (size_t)-1 / stride) return 0;
    bytes = stride * (size_t)fr->height;
    if (q->capacity < bytes) {
        unsigned char *p = (unsigned char *)realloc(q->rgb, bytes);
        if (!p) return 0;
        q->rgb = p; q->capacity = bytes;
    }
    if (bgr)
        mr_yuv420_to_bgr24(q->rgb, (int)stride,
                           fr->data, fr->stride,
                           fr->u_data, fr->u_stride,
                           fr->v_data, fr->v_stride,
                           fr->width, fr->height, service, service_opaque);
    else
        mr_yuv420_to_rgb24(q->rgb, (int)stride,
                           fr->data, fr->stride,
                           fr->u_data, fr->u_stride,
                           fr->v_data, fr->v_stride,
                           fr->width, fr->height, service, service_opaque);
    q->width = fr->width; q->height = fr->height; q->stride = (int)stride;
    q->dirty_y0 = fr->dirty_y0; q->dirty_y1 = fr->dirty_y1;
    q->pts_us = pts; q->decoded_at_us = decoded_at;
    return 1;
}

/*
 * Same contract as queue_copy(), but for a display that can accept an
 * indexed frame directly (display_supports_indexed() - see display_backend.h
 * and aga_supports_indexed()). Dithers straight into the queue slot instead
 * of memcpy'ing an RGB24 frame that display_show_rgb's AGA backend would only
 * dither (again) at presentation time - cutting the ~w*h*3-byte RGB24 copy
 * out of this pipeline stage entirely and leaving the display side a cheap
 * per-row byte copy instead of a per-pixel LUT pass.
 *
 * Dithers the whole frame every call, exactly like queue_copy()'s full-buffer
 * memcpy above: fr->data is the decoder's persistent framebuffer (every row
 * holds valid pixel data, not just the dirty ones), and this queue slot is
 * reused by unrelated frames between visits, so dithering only [dirty_y0,
 * dirty_y1) would leave stale rows from whatever frame last occupied it.
 */
static int queue_copy_indexed(queued_video *q, const mr_frame *fr, uint64_t pts,
                              uint64_t decoded_at, int indexed_depth)
{
    size_t bytes = (size_t)fr->width * (size_t)fr->height;
    if (q->capacity < bytes) {
        unsigned char *p = (unsigned char *)realloc(q->rgb, bytes);
        if (!p) return 0;
        q->rgb = p; q->capacity = bytes;
    }
    mr_dither_rgb_indexed(fr->data, fr->width, fr->height, fr->stride,
                          q->rgb, fr->width, 0, indexed_depth);
    q->width = fr->width; q->height = fr->height; q->stride = fr->width;
    q->dirty_y0 = fr->dirty_y0; q->dirty_y1 = fr->dirty_y1;
    q->pts_us = pts; q->decoded_at_us = decoded_at;
    return 1;
}

/*
 * Same contract as queue_copy_indexed(), but for the AGA resize case
 * (display_supports_yuv_indexed() - see display_backend.h and
 * aga_supports_yuv_indexed()): fr must be a MR_PIX_YUV420P frame
 * (mr_h264_set_yuv_output() enabled), and dst_w/dst_h/vscale/ham are
 * display_supports_yuv_indexed()'s own output for this session. Converts
 * straight from the decoder's YUV planes to chunky pixels at the display's
 * fitted size in one pass - no RGB24 buffer, full resolution or resized,
 * ever exists on this path.
 *
 * ham is 0 for palette indices (core/mr_yuv_dither.h) or 6/8 for HAM6/HAM8
 * pixel bytes (core/mr_yuv_ham.h); both fill the same one-byte-per-pixel
 * slot and are presented identically by display_show_indexed(). HAM is only
 * ever reported for the exact vertical downscale.
 *
 * vscale > 0 selects the exact vertical-only downscale (hand-tuned m68k
 * assembly on the indexed side); vscale == 0 selects the general 2D
 * nearest-neighbour path for every other resize shape, including an upscale
 * (e.g. a 192x108 mobile HLS variant fitted up to a 320x180 AGA screen).
 */
static int queue_copy_yuv_indexed(queued_video *q, const mr_frame *fr,
                                  uint64_t pts, uint64_t decoded_at,
                                  int dst_w, int dst_h, int vscale,
                                  int indexed_depth, int ham)
{
    size_t bytes = (size_t)dst_w * (size_t)dst_h;
    if (q->capacity < bytes) {
        unsigned char *p = (unsigned char *)realloc(q->rgb, bytes);
        if (!p) return 0;
        q->rgb = p; q->capacity = bytes;
    }
    if (ham)
        /* aga_supports_yuv_indexed() only reports ham for the exact vertical
         * downscale, so vscale is always > 1 here - see core/mr_yuv_ham.h for
         * why the other shapes stay on aga_show()'s three-stage path. */
        mr_yuv420_ham_encode(fr->data, fr->stride, fr->u_data,
                             fr->u_stride, fr->v_data, fr->v_stride,
                             fr->width, fr->height, vscale, ham,
                             q->rgb, dst_w);
    else if (vscale > 0)
        mr_yuv420_dither_indexed(fr->data, fr->stride, fr->u_data,
                                 fr->u_stride, fr->v_data, fr->v_stride,
                                 fr->width, fr->height, vscale, indexed_depth,
                                 q->rgb, dst_w, 0);
    else
        mr_yuv420_dither_indexed_resize(
            fr->data, fr->stride, fr->u_data, fr->u_stride, fr->v_data,
            fr->v_stride, fr->width, fr->height, indexed_depth, q->rgb,
            dst_w, dst_h, dst_w, 0);
    q->width = dst_w; q->height = dst_h; q->stride = dst_w;
    q->dirty_y0 = 0; q->dirty_y1 = dst_h;
    q->pts_us = pts; q->decoded_at_us = decoded_at;
    return 1;
}

static unsigned long average_hundredths(uint64_t usec, unsigned count)
{
    return count ? (unsigned long)(usec / ((uint64_t)count * 10ULL)) : 0;
}

static unsigned long rate_hundredths(unsigned count, uint64_t elapsed_us)
{
    return elapsed_us
         ? (unsigned long)((uint64_t)count * 100000000ULL / elapsed_us) : 0;
}

static void report_stats(playback_stats *st, mr_audio *audio, mr_demux *demux,
                         scheduler_trace *trace, int depth, uint64_t now)
{
    uint64_t elapsed_us = now - st->since_us;
    unsigned long vd = average_hundredths(st->video_decode_us, st->decoded);
    unsigned long dm = average_hundredths(st->demux_us, st->samples);
    unsigned long ad = average_hundredths(st->audio_decode_us, st->samples);
    unsigned long cv = average_hundredths(st->convert_us, st->presented);
    unsigned long sc = average_hundredths(st->scale_us, st->presented);
    unsigned long yi = average_hundredths(st->yuv_indexed_us,
                                          st->yuv_indexed_frames);
    unsigned long yr = average_hundredths(st->yuv_rgb_us,
                                          st->yuv_rgb_frames);
    unsigned long ds = average_hundredths(st->display_us, st->presented);
    unsigned long la = average_hundredths(st->latency_us, st->presented);
    unsigned long pf = rate_hundredths(st->presented, elapsed_us);
    unsigned long df = rate_hundredths(st->decoded, elapsed_us);
    mr_source_timing io;
    mr_audio_diagnostics audio_diag;
    mr_demux_timing demux_timing;
    mr_source_timing_get(&io);
    audio_diagnostics(audio, &audio_diag);
    mr_demux_timing_get(demux, &demux_timing, 1);
    printf("rtg timing: vdecode=%lu.%02lu/%lu ms network-blocked=%lu ms "
           "hls-segment=%lu ms demux=%lu.%02lu ms adecode=%lu.%02lu ms "
           "convert=%lu.%02lu ms scale=%lu.%02lu ms "
           "yuv-indexed=%lu.%02lu/%lu ms yuv-rgb=%lu.%02lu/%lu ms "
           "display=%lu.%02lu/%lu ms "
           "audio-buffered=%lu ms vqueue=%d late=%u dropped=%u "
           "presented=%lu.%02lu fps decoded=%lu.%02lu fps sleep=%lu/%lu ms "
           "sleep-max-error=%lu us latency=%lu.%02lu ms "
           "refill-blocked=%lu ms ready-delayed-by-refill=%lu ms\n",
           vd / 100, vd % 100, st->video_decode_max_us / 1000,
           (unsigned long)(st->network_us / 1000) + io.network_ms,
           io.hls_segment_ms, dm / 100, dm % 100, ad / 100, ad % 100,
           cv / 100, cv % 100, sc / 100, sc % 100,
           yi / 100, yi % 100, st->yuv_indexed_max_us / 1000,
           yr / 100, yr % 100, st->yuv_rgb_max_us / 1000,
           ds / 100, ds % 100, st->display_max_us / 1000,
           audio ? audio_buffered_ms(audio) : 0, depth, st->late, st->dropped,
           pf / 100, pf % 100, df / 100, df % 100,
           (unsigned long)(st->sleep_requested_us / 1000),
           (unsigned long)(st->sleep_actual_us / 1000), st->sleep_max_error_us,
           la / 100, la % 100,
           (unsigned long)(st->refill_block_us / 1000),
           (unsigned long)(st->refill_delayed_ready_us / 1000));
    if (audio) service_audio_for_display(trace);
    printf("audio diagnostics: hw-starvations=%lu minimum-buffered=%lu ms "
           "minimum-active=%lu ms "
           "longest-service-gap=%lu ms longest-no-active=%lu ms "
           "fifo=%lu fifo-dropped=%lu req0=%u/%lu req1=%u/%lu active=%u\n",
           audio_diag.hardware_starvations, audio_diag.minimum_buffered_ms,
           audio_diag.minimum_active_ms,
           audio_diag.longest_service_gap_ms, audio_diag.longest_no_active_ms,
           audio_diag.fifo_samples, audio_diag.fifo_dropped_samples,
           (unsigned)audio_diag.request_state[0],
           audio_diag.request_samples[0], (unsigned)audio_diag.request_state[1],
           audio_diag.request_samples[1], (unsigned)audio_diag.active_requests);
    if (audio) service_audio_for_display(trace);
    printf("audio timeline: clock=%lu us fifo=%lu ms playing-remain=%lu ms "
           "queued=%lu ms total=%lu ms max-step=%lu us oldest=%lu "
           "startup-max-step=%lu us req0=%u req1=%u\n",
           (unsigned long)audio_diag.audio_clock_us,
           audio_diag.fifo_buffered_ms,
           audio_diag.hardware_playing_remaining_ms,
           audio_diag.hardware_queued_ms, audio_diag.total_buffered_ms,
           (unsigned long)audio_diag.clock_largest_step_us,
           (unsigned long)audio_diag.oldest_request_sequence,
           (unsigned long)audio_diag.startup_clock_largest_step_us,
           (unsigned)audio_diag.request_timeline_state[0],
           (unsigned)audio_diag.request_timeline_state[1]);
    printf("audio hardware: timeline-covered=%u actual-active-requests=%u "
           "actual-no-active-duration=%lu ms\n",
           (unsigned)audio_diag.timeline_covered,
           (unsigned)audio_diag.active_requests,
           audio_diag.longest_no_active_ms);
    if (audio) service_audio_for_display(trace);
    printf("demux timing: calls=%lu max-call=%lu us max-scanned=%lu "
           "service=%lu\n", demux_timing.calls, demux_timing.call_max_us,
           demux_timing.scanned_max, demux_timing.service_calls);
    if (audio) service_audio_for_display(trace);
    if (hls_fetch_active()) {
        unsigned long hits, misses, worst_wait_ms;
        hls_fetch_stats(&hits, &misses, &worst_wait_ms);
        printf("hls fetch: hits=%lu misses=%lu worst-wait=%lu ms\n",
               hits, misses, worst_wait_ms);
    }
    if (audio) service_audio_for_display(trace);
    if (st->decoded) {
#if defined(MR_H264_STAGE_PROFILE)
        printf("h264 stages: input=%lu/%lu us libavc-core=%lu/%lu us "
               "rgb-output=%lu/%lu us mc=%lu/%lu us deblock=%lu/%lu us "
               "recon=%lu/%lu us intra=%lu/%lu us\n",
               (unsigned long)(st->h264_input_us / st->decoded),
               st->h264_input_max_us,
               (unsigned long)(st->h264_core_us / st->decoded),
               st->h264_core_max_us,
               (unsigned long)(st->h264_output_us / st->decoded),
               st->h264_output_max_us,
               (unsigned long)(st->h264_mc_us / st->decoded),
               st->h264_mc_max_us,
               (unsigned long)(st->h264_deblock_us / st->decoded),
               st->h264_deblock_max_us,
               (unsigned long)(st->h264_recon_us / st->decoded),
               st->h264_recon_max_us,
               (unsigned long)(st->h264_intra_us / st->decoded),
               st->h264_intra_max_us);
#else
        /* mc/deblock/recon/intra are only wrapped with per-call timing
         * (ih264d_stage_profile.c) when built with -DMR_H264_STAGE_PROFILE
         * - see ih264d_function_selector_port.c. That instrumentation adds
         * real overhead on this target, so it is opt-in, not part of a
         * normal playback build; without it these four stay at zero and
         * are not worth printing. */
        printf("h264 stages: input=%lu/%lu us libavc-core=%lu/%lu us "
               "rgb-output=%lu/%lu us mc/deblock/recon/intra breakdown "
               "disabled (build with -DMR_H264_STAGE_PROFILE=1)\n",
               (unsigned long)(st->h264_input_us / st->decoded),
               st->h264_input_max_us,
               (unsigned long)(st->h264_core_us / st->decoded),
               st->h264_core_max_us,
               (unsigned long)(st->h264_output_us / st->decoded),
               st->h264_output_max_us);
#endif
        if (audio) service_audio_for_display(trace);
    }
    if (st->rescue_entries) {
        printf("audio rescue: entries=%u critical=%u noncritical=%u packets=%u "
               "audio=%u video=%u retained=%u replaced=%u not-copied=%u "
               "newest-pts=%lu post-late=%ld us duration-avg=%lu us max=%lu us "
               "min-margin=%ld ms negative=%u rescue-starvations=%u "
               "exit(target/limit/eof)=%u/%u/%u\n",
               st->rescue_entries, st->rescue_critical,
               st->rescue_noncritical, st->rescue_packets,
               st->rescue_audio_packets, st->rescue_video_decoded,
               st->rescue_video_queued, st->rescue_video_replaced,
               st->rescue_video_skipped,
               (unsigned long)st->rescue_newest_retained_pts_us,
               (long)st->rescue_post_lateness_us,
               (unsigned long)(st->rescue_us / st->rescue_entries),
               st->rescue_max_us, st->rescue_min_margin_ms,
               st->rescue_negative_margin, st->rescue_hw_starvations,
               st->rescue_exit_target, st->rescue_exit_limit, st->rescue_exit_eof);
        if (audio) service_audio_for_display(trace);
    }
    if (st->last_rtg.src_w) {
        unsigned n = st->presented ? st->presented : 1;
        printf("rtg src=%ux%u dst=%ux%u srcfmt=%s dstfmt=%s "
               "prepare=%lu us scale=%lu us convert=%lu us copy=%lu us "
               "prepare-max=%lu us cgx-blit=%lu us cgx-blit-max=%lu us "
               "clip=%lu us display-total=%lu us "
               "audio-before=%lu ms audio-after=%lu ms "
               "pixels=%lu bytes=%lu copies=%u displayed=%u "
               "dropped-before-scale=%u dropped-after-scale=%u "
               "frame_pts=%lu audio_clock=%lu lateness=%ld us "
               "queue-head=%u dropped-pass=%u timing-rebases=%u\n",
               st->last_rtg.src_w, st->last_rtg.src_h,
               st->last_rtg.dst_w, st->last_rtg.dst_h,
               st->last_rtg.src_format, st->last_rtg.dst_format,
               (unsigned long)(st->rtg_prepare_us / n),
               (unsigned long)(st->rtg_scale_us / n),
               (unsigned long)(st->rtg_convert_us / n),
               (unsigned long)(st->rtg_copy_us / n),
               st->rtg_prepare_max_us,
               (unsigned long)(st->rtg_blit_us / n),
               st->rtg_blit_max_us,
               (unsigned long)(st->rtg_clip_us / n),
               (unsigned long)(st->rtg_total_us / n),
               st->audio_before, st->audio_after,
               st->last_rtg.pixels, st->last_rtg.bytes,
               st->last_rtg.copies, st->presented, st->dropped,
               st->dropped_after_scale,
               (unsigned long)st->frame_pts_us,
               (unsigned long)st->audio_clock_us,
               (long)st->calculated_lateness_us, st->queue_head,
               st->dropped_in_pass, st->timing_rebases);
        if (audio) service_audio_for_display(trace);
    }
    memset(st, 0, sizeof *st); st->since_us = now;
    mr_source_timing_reset();
}

static unsigned char *slurp(const char *path, long *out_len)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    unsigned char *b = (unsigned char *)malloc((size_t)n);
    if (b && fread(b, 1, (size_t)n, f) != (size_t)n) { free(b); b = NULL; }
    fclose(f);
    if (out_len) *out_len = n;
    return b;
}

/* Ticks are 1/50 s (dos Delay). frame period = 50*scale/rate, min 1. */
static long frame_ticks(unsigned long rate, unsigned long scale)
{
    long t;
    if (!rate) return 4;
    t = (long)((50UL * scale + rate / 2) / rate);
    return t < 1 ? 1 : t;
}

static void decoded_audio_sink(void *user, const int16_t *pcm,
                               unsigned frames, unsigned channels)
{
    audio_write_s16((mr_audio *)user, (const short *)pcm, frames,
                    (int)channels);
}

static mr_h264_speed_mode effective_h264_speed(int requested)
{
    if (requested == MR_H264_SPEED_QUALITY ||
        requested == MR_H264_SPEED_BALANCED ||
        requested == MR_H264_SPEED_FAST ||
        requested == MR_H264_SPEED_TURBO ||
        requested == MR_H264_SPEED_TURBO_PLUS ||
        requested == MR_H264_SPEED_TURBO_GT)
        return (mr_h264_speed_mode)requested;
    /* Auto follows the release's throughput-first default. TurboGT preserves
     * the P-frame reference chain, unlike Turbo+, while skipping B pictures
     * and applying libavc's strongest practical degradation policy to every
     * decoded picture - which is now also exactly Turbo's policy, TurboGT
     * being kept as a name rather than a distinct setting (see
     * mr_h264_set_speed_mode()). Explicit Fast remains available for users
     * who prefer to keep every frame, and Quality/Balanced remain deliberate
     * opt-ins. */
    return MR_H264_SPEED_TURBO_GT;
}

static int apply_h264_speed(mr_decoder *dec, int requested, int verbose)
{
    mr_h264_speed_mode mode;
    const char *name;
    if (!dec || dec->codec != &mr_codec_h264) return 1;
    mode = effective_h264_speed(requested);
    name = mode == MR_H264_SPEED_TURBO_GT ? "TurboGT (B-skip, bilinear MC)" :
           mode == MR_H264_SPEED_TURBO_PLUS ? "Turbo+ (PB-skip, keyframes only)" :
           mode == MR_H264_SPEED_TURBO ? "Turbo (B-skip, bilinear MC)" :
           mode == MR_H264_SPEED_FAST ? "Fast (bilinear MC)" :
           mode == MR_H264_SPEED_BALANCED ? "Balanced" : "Quality";
    if (!mr_h264_set_speed_mode(dec, mode)) return 0;
    if (verbose || requested < 0)
        printf("H.264 performance: %s%s\n", name,
               requested < 0 ? " (automatic)" : "");
    return 1;
}

/* Shared by the on-screen Vol -/+ controller commands and the display
 * window's own cursor up/down keys. */
static void apply_volume_step(int delta)
{
    control_volume += delta;
    if (control_volume < 0) control_volume = 0;
    if (control_volume > 64) control_volume = 64;
    if (control_audio) audio_set_volume(control_audio, control_volume);
    printf("volume: %d%%\n", control_volume * 100 / 64);
}

/* The ReAction controller uses Ctrl-F for a normal stop so AmigaDOS does not
 * abort the CLI process before display/audio cleanup runs.  Shell Ctrl-C is
 * still accepted when mrplay sees it itself; Ctrl-D toggles pause and Ctrl-E
 * toggles fast-forward. */
static int control_signal_event(amiga_display *disp)
{
    ULONG sig = SetSignal(0, SIGBREAKF_CTRL_C | SIGBREAKF_CTRL_D |
                             SIGBREAKF_CTRL_E | SIGBREAKF_CTRL_F);
    ULONG commands = 0;
    if (sig & (SIGBREAKF_CTRL_C | SIGBREAKF_CTRL_F)) return MR_EV_QUIT;
    if (sig & SIGBREAKF_CTRL_D) return MR_EV_PAUSE;
    if (sig & SIGBREAKF_CTRL_E) {
        if (control_block) {
            Forbid();
            commands = control_block->commands;
            control_block->commands = 0;
            Permit();
        }
        if (commands & MR_PLAYER_COMMAND_FULLSCREEN)
            display_toggle_fullscreen(disp);
        if (commands & MR_PLAYER_COMMAND_VOLUME_DOWN) apply_volume_step(-8);
        if (commands & MR_PLAYER_COMMAND_VOLUME_UP) apply_volume_step(8);
        if (commands & MR_PLAYER_COMMAND_PAUSE) return MR_EV_PAUSE;
        if (commands & MR_PLAYER_COMMAND_FAST) return MR_EV_SEEK_FWD;
        /* Compatibility with older controllers that used bare Ctrl-E. */
        if (!commands) return MR_EV_SEEK_FWD;
    }
    return MR_EV_NONE;
}

static int player_event(amiga_display *disp)
{
    int ev;
    if (deferred_player_event != MR_EV_NONE) {
        ev = deferred_player_event;
        deferred_player_event = MR_EV_NONE;
    } else {
        ev = control_signal_event(disp);
    }
    if (ev == MR_EV_NONE) ev = display_poll_event(disp);
    /* Volume is applied here, not returned to the many playback loops below -
     * none of them need to know a volume key was pressed, only that nothing
     * else happened this poll. */
    if (ev == MR_EV_VOLUME_UP) { apply_volume_step(8); ev = MR_EV_NONE; }
    else if (ev == MR_EV_VOLUME_DOWN) { apply_volume_step(-8); ev = MR_EV_NONE; }
    return ev;
}

/* Minimal service hook for the fetch worker during the very first HLS
 * fetch(es) - before any display/decoder/trace state exists for the full
 * service_player_during_io() below to safely touch (see the call site in
 * main() for why). Only checks for the quit signal, nothing else: no
 * fullscreen/pause/volume handling, no trace dereference. opaque is unused
 * (always NULL here) - the signature matches hls_fetch_service_fn only so
 * it can be installed the same way. */
static int service_early_quit_check(void *opaque)
{
    (void)opaque;
    return (SetSignal(0, SIGBREAKF_CTRL_C | SIGBREAKF_CTRL_F) &
            (SIGBREAKF_CTRL_C | SIGBREAKF_CTRL_F)) != 0;
}

/* HTTP's synchronous reader runs on the player task.  Pump fresh Intuition
 * input while it waits, but leave pause/seek/quit queued for the scheduler to
 * perform at its normal safe point.  Fullscreen is handled immediately by the
 * display backend, so its replacement window starts being drained at once.
 *
 * MUST NOT run on any task but the main one. core/mr_http.c's own internal
 * service hook (mr_http_set_service()) and this file's hls_fetch_set_
 * service() are BOTH set to this same function with the same &trace, but
 * they are consulted from two different tasks once the HLS fetch worker is
 * active: mr_http.c's hook fires from inside whichever task is currently
 * calling into its connect/read code - which, for every HLS fetch, is the
 * worker task, not this one - while hls_fetch_wait_busy() calls it from
 * this (main) task while polling for that same worker's reply. Without this
 * check both tasks could call display_poll_event()/present_service_frame()/
 * service_audio_for_display() concurrently, on state (the video queue in
 * particular) that is explicitly documented as single-task with no lock.
 * Refuse to do any of that unless FindTask(NULL) is the main task; the
 * worker's own calls become a safe no-op instead. */
static int service_player_during_io(void *opaque)
{
    scheduler_trace *trace = (scheduler_trace *)opaque;
    amiga_display *disp;
    int ev = MR_EV_NONE;
    if (FindTask(NULL) != g_main_task) return 0;
    disp = trace && trace->presenter ? trace->presenter->disp : NULL;
    if (disp && (!trace->presenter || !trace->presenter->presenting)) {
        ev = control_signal_event(disp);
        if (ev == MR_EV_NONE) ev = display_poll_event(disp);
        if (ev != MR_EV_NONE &&
            (deferred_player_event == MR_EV_NONE || ev == MR_EV_QUIT))
            deferred_player_event = ev;
    }
    if (trace) {
        if (trace->audio) service_audio_for_display(trace);
        else if (trace->presenter) present_service_frame(trace->presenter);
    }
    return ev == MR_EV_QUIT || deferred_player_event == MR_EV_QUIT;
}

/*
 * Wait hook for the HLS live-playlist re-fetch loop (mr_hls_set_wait).
 *
 * When playback reaches the live edge the reader must poll the playlist for new
 * segments. That poll used to spin without pause and without servicing anything,
 * so a stalled edge froze the single task for tens of seconds - audio and video
 * stopped and even ESC/Close could not be seen until the spin ended. This paces
 * each poll and, throughout the wait, pumps the same service hook the scheduler
 * uses: audio keeps playing, already-decoded video keeps presenting (the queue
 * is released for the blocking fetch), and a quit request is honoured promptly.
 * opaque is the scheduler_trace, which carries the audio handle and presenter.
 */
static int hls_wait_service(void *opaque, unsigned wait_ms)
{
    scheduler_trace *trace = (scheduler_trace *)opaque;
    amiga_display *disp = trace && trace->presenter ? trace->presenter->disp
                                                    : NULL;
    uint64_t begin = monotonic_us();
    uint64_t target = (uint64_t)wait_ms * 1000ULL;
    for (;;) {
        if (disp && player_event(disp) == MR_EV_QUIT) return 1;
        if (trace->audio) service_audio_for_display(trace);
        else if (trace->presenter) present_service_frame(trace->presenter);
        if (monotonic_us() - begin >= target) return 0;
        Delay(1);                            /* 20 ms; stay responsive to ESC  */
    }
}

/* MPEG-1 program streams (.mpg/.mpeg) play through pl_mpeg (video + MP2 audio),
 * reusing the display and Paula audio backends. Separate from the AVI/MOV +
 * codec path because .mpg is a self-contained stream. */
static int play_mpeg1(const unsigned char *buf, long len, int loop, int want_time,
                      int audio_low_rate, int no_audio)
{
    mr_mpeg1      *mp;
    amiga_display *disp;
    mr_audio      *audio = NULL;
    unsigned       sr;
    int            w, h, frames = 0, paused = 0, quit = 0, fast_forward = 0;
    unsigned long  period, clock_base = 0;
    long           ntick;
    unsigned char *abuf;                         /* heap, not stack (4.6 KB)  */
    clock_t        t_dec = 0, t_show = 0;
    mr_frame       fr;
    int64_t        pts_us;
    unsigned       fps_millihz;

    mp = mr_mpeg1_open((const uint8_t *)buf, (size_t)len, audio_low_rate,
                       no_audio);
    if (!mp) { printf("cannot open MPEG-1 stream\n");
               player_status(MR_PLAYER_STATE_ERROR, "MPEG-1",
                             "cannot open MPEG-1 stream");
               status_hold(); return 10; }
    abuf = (unsigned char *)malloc(1152 * 4);    /* max: 1152 frames stereo16 */
    if (!abuf) { mr_mpeg1_close(mp); return 10; }
    w = mr_mpeg1_width(mp); h = mr_mpeg1_height(mp);
    printf("mpeg1: %dx%d, opening display...\n", w, h);
    {
        char line[MR_PLAYER_STATUS_TEXT_MAX];
        snprintf(line, sizeof line, "%dx%d, MPEG-1", w, h);
        player_status(MR_PLAYER_STATE_PLAYING, "MPEG-1", line);
    }
    disp = display_open(w, h, "MintVID");
    if (!disp) { printf("cannot open a display\n");
                 player_status(MR_PLAYER_STATE_ERROR, "MPEG-1",
                               "cannot open a display");
                 status_hold(); mr_mpeg1_close(mp); return 10; }
    printf("display backend: %s\n", display_backend_name(disp));

    sr = mr_mpeg1_samplerate(mp);
    if (sr) {
        audio = audio_open(sr, 2, 16);
        control_audio = audio;
        if (audio) audio_set_volume(audio, control_volume);
        printf(audio ? "audio: Paula out, %u Hz (MP2 stereo)\n"
                     : "audio: Paula open failed, silent\n", sr);
    }
    fps_millihz = mr_mpeg1_framerate_millihz(mp);
    period = fps_millihz ? (1000000UL + fps_millihz / 2) / fps_millihz : 40;
    if (period < 1) period = 1;
    ntick = (long)((period + 19) / 20);
    if (ntick < 1) ntick = 1;

    printf("playing: space=pause, ESC=quit%s...\n", loop ? ", loop on" : "");

    while (!quit) {
        int got;
        while (paused && !quit) {
            int ev = player_event(disp);
            if (ev == MR_EV_QUIT) quit = 1;
            else if (ev == MR_EV_PAUSE) {
                paused = 0;
                if (audio) audio_set_running(audio, 1);
            }
            Delay(2);
        }
        if (quit) break;

        {
            clock_t a = 0;
            if (want_time) a = clock();
            got = mr_mpeg1_next(mp, &fr, &pts_us);
            if (want_time) t_dec += clock() - a;
        }
        if (!got) {
            if (loop) { mr_mpeg1_rewind(mp); frames = 0;
                        clock_base = audio ? audio_elapsed_ms(audio) : 0; continue; }
            break;
        }
        if (audio) {                             /* top up audio (bounded)    */
            int n, k = 0;
            /* Build one complete Paula request before opening its playback
             * gate.  The generic streaming player does this explicitly, but
             * this older MPEG-1 path pre-dates the gate.  Leaving it closed
             * queues PCM forever and deadlocks on the second video frame while
             * waiting for an audio clock that cannot advance. */
            int limit = frames == 0 ? 4 : 2;
            while (k < limit && (n = mr_mpeg1_audio(mp, abuf)) > 0) {
                audio_write(audio, abuf, (unsigned)(n * 4));
                audio_service(audio);
                k++;
            }
            if (frames == 0) audio_set_running(audio, 1);
        }

        if (audio) {                             /* pace to the audio clock   */
            unsigned long target = clock_base + (unsigned long)frames * period;
            for (;;) {
                int ev = player_event(disp);
                if (ev == MR_EV_QUIT)  { quit = 1; break; }
                if (ev == MR_EV_PAUSE) {
                    paused = 1;
                    audio_set_running(audio, 0);
                    break;
                }
                if (ev == MR_EV_SEEK_FWD) fast_forward = !fast_forward;
                audio_service(audio);
                if (fast_forward) break;
                if (audio_elapsed_ms(audio) >= target) break;
                if (audio_starved(audio)) break;
                Delay(1);
            }
        } else {
            int ev = player_event(disp);
            if (ev == MR_EV_QUIT) quit = 1;
            else if (ev == MR_EV_PAUSE) paused = 1;
            else if (ev == MR_EV_SEEK_FWD) fast_forward = !fast_forward;
            if (!fast_forward) Delay(ntick);
        }
        if (quit) break;

        {
            clock_t a = 0;
            if (want_time) a = clock();
            display_show_rgb(disp, fr.data, fr.width, fr.height, fr.stride,
                             fr.dirty_y0, fr.dirty_y1);
            if (want_time) t_show += clock() - a;
        }
        frames++;
        if (audio) audio_service(audio);
    }

    if (want_time && frames > 0) {
        unsigned long e = 0, bl = 0;
        display_aga_timing(&e, &bl);
        printf("timing/%d frames: decode=%lu ms, display=%lu ms (encode=%lu, blit=%lu)\n",
               frames, (unsigned long)(t_dec * 1000 / CLOCKS_PER_SEC),
               (unsigned long)(t_show * 1000 / CLOCKS_PER_SEC), e, bl);
        if (display_aga_kalms_timing(&bl))
            printf("Kalms conversion: %lu ms\n", bl);
    }
    if (audio) {
        int g = 0;
        while (!audio_starved(audio) && g++ < 4000) {
            if (player_event(disp) == MR_EV_QUIT) {
                quit = 1;
                break;
            }
            audio_service(audio);
            Delay(1);
        }
    }
    if (!quit) printf("played %d frames\n", frames);
    player_status(MR_PLAYER_STATE_ENDED, "MPEG-1", "stream ended");
    control_audio = NULL;
    if (audio) audio_close(audio);
    display_close(disp);
    mr_mpeg1_close(mp);
    free(abuf);
    return 0;
}

int main(int argc, char **argv)
{
    g_main_task = FindTask(NULL);
    long len = 0;
    unsigned char *buf = NULL;
    mr_demux *dx;
    const mr_video_info *vi;
    const mr_audio_info *ai;
    const mr_codec *codec;
    mr_decoder dec;
    amiga_display *disp;
    mr_audio *audio = NULL;
    mr_audio_decoder *audio_dec = NULL;
    mr_packet pkt;
    long ticks;
    int frames = 0;
    int want_time = 0, loop = 0, paused = 0, quit = 0, fast_forward = 0;
    int fullscreen = 0;
    int raw_diag_printed = 0;
    int hls_low = 0;
    unsigned hls_max_width = 0, hls_max_height = 0, hls_max_fps = 0;
    int net_queue = 0;  /* 0 = built-in default (network depth 1)             */
    int live_resync = 0;  /* --live-resync: catch up after a big stall, and
                           * reconnect a live stream that drops out            */
    int auto_close_eof = 0; /* finite GUI media should release its window      */
    int audio_unavailable = 0;
    const char *audio_failure = NULL;
    int h264_speed = -1; /* automatic: TurboGT - see effective_h264_speed() */
    int audio_low_rate = 0; /* --audio-rate=low: halve the output rate again */
    int no_audio = 0;       /* --no-audio: skip the decoder/Paula entirely   */
    const char *media_path = NULL;
    const char *user_agent = NULL;
    const char *referer = NULL;
    char youtube_media[MR_HTTP_URL_MAX];
    char video_description[96], audio_description[96];
    char playing_detail[MR_PLAYER_STATUS_TEXT_MAX];
    mr_youtube_media_kind youtube_kind = MR_YOUTUBE_MEDIA_NONE;
    mr_http_options http_options;
    int have_http_options = 0;
    media_clock mc;
    memset(&mc, 0, sizeof mc);
    int64_t container_pts_adjust_us = 0;
    uint64_t last_container_pts_us = 0;
    int have_container_pts = 0;
    /* Lifetime totals for the final "timing/N frames" summary. These are
     * separate from stats.video_decode_us/display_us, which report_stats()
     * resets every STATS_INTERVAL_US: without an independent accumulator the
     * final summary only ever saw whatever the periodic reset last left
     * behind, printing decode=0 ms/display=0 ms when a report just fired. */
    uint64_t total_decode_us = 0, total_display_us = 0;
    queued_video vq[VIDEO_QUEUE_CAP];
    int qhead = 0, qcount = 0, input_eof = 0;
    int oom_warned = 0; /* set after first queue_copy OOM is logged */
    int rescue_active = 0;
    int rescue_priority = 0;
    unsigned rescue_cooldown = 0;
    unsigned rescue_episode_packets = 0, rescue_episode_audio = 0;
    unsigned rescue_episode_video = 0, rescue_episode_queued = 0;
    unsigned rescue_episode_skipped = 0;
    unsigned rescue_episode_replaced = 0;
    int rescue_episode_critical = 0;
    uint64_t rescue_newest_retained_pts_us = 0;
    unsigned long rescue_buffer_before = 0;
    unsigned long rescue_entry_threshold = 0;
    unsigned long rescue_min_buffer = 0;
    unsigned long rescue_hw_before = 0;
    uint64_t rescue_started_us = 0;
    uint64_t decoded_index = 0, mono_base_us = 0;
    /* Bridges the seek origin across the gap a seek itself creates: a
     * successful seek empties the video queue (qcount = 0), so front is
     * NULL until a new frame is decoded and queued - real time, spent on a
     * network fetch plus H.264 decode. A seek key pressed again inside that
     * gap must still originate from where the previous seek actually
     * landed, not from a stale/zero fallback, or repeated presses (holding
     * the key, or just pressing faster than the queue refills) all land on
     * the same target instead of accumulating. front->pts_us is preferred
     * over this the moment it is available again (see cur_pts_us below). */
    int64_t last_seek_pts_us = -1;
    playback_stats stats;
    scheduler_trace trace;

    /* Unbuffered so every diagnostic reaches the shell immediately, even if a
     * later step hangs or crashes (libnix stdout can otherwise block-buffer). */
    setvbuf(stdout, NULL, _IONBF, 0);
    control_port_open();
    DeleteFile((CONST_STRPTR)MR_PLAYER_STATUS_FILE);
    DeleteFile((CONST_STRPTR)MR_PLAYER_STATUS_TMP);
    player_status(MR_PLAYER_STATE_STARTING, "", "Starting player...");
    if (!playback_timer_open())
        printf("warning: timer.device unavailable; pacing timer is coarse\n");

    if (argc < 2) {
        printf("usage: mrplay [--user-agent <value>] [--referer <value>] "
               "<file.avi|file.mov|file.ts|file.m2ts|"
               "file.mjpeg|file.m4v> "
               "[--aga] [--ham] [--ham6] [--p96] "
               "[--2x] [--lace] [--ecs-fast] [--ecs32] [--loop] "
               "[--wpa|--c2p|--riva-c2p|--kalms-c2p] "
               "[--cd32] [--fullscreen] [--hls-low] [--net-queue=N] [--live-resync] "
               "[--h264-speed=auto|quality|balanced|fast|turbo|turbo+|turbogt] "
               "[--audio-rate=normal|low] [--no-audio] "
               "[--time]\n");
        return mrplay_exit(5);
    }
    {   /* display options anywhere on the command line */
        int i;
        for (i = 1; i < argc; i++) {
            if (!strcmp(argv[i], "--user-agent")) {
                if (++i >= argc) {
                    printf("--user-agent requires a value\n");
                    return mrplay_exit(5);
                }
                user_agent = argv[i];
            } else if (!strcmp(argv[i], "--referer")) {
                if (++i >= argc) {
                    printf("--referer requires a value\n");
                    return mrplay_exit(5);
                }
                referer = argv[i];
            }
            else if (!strcmp(argv[i], "--aga"))  display_set_force_aga(1);
            else if (!strcmp(argv[i], "--p96"))  display_set_force_p96(1);
            else if (!strcmp(argv[i], "--ham"))  display_set_ham(8);
            else if (!strcmp(argv[i], "--ham6")) display_set_ham(6);
            else if (!strcmp(argv[i], "--2x"))   display_set_scale(2);
            else if (!strcmp(argv[i], "--wpa"))  display_set_c2p(0);
            else if (!strcmp(argv[i], "--c2p"))  display_set_c2p(1);
            else if (!strcmp(argv[i], "--riva-c2p")) display_set_riva_c2p(1);
            else if (!strcmp(argv[i], "--kalms-c2p")) display_set_kalms_c2p(1);
            else if (!strcmp(argv[i], "--loop")) loop = 1;
            else if (!strcmp(argv[i], "--lace")) display_set_lace(1);
            else if (!strcmp(argv[i], "--ecs-fast")) display_set_ecs_fast(1);
            else if (!strcmp(argv[i], "--ecs32")) display_set_ecs32(1);
            else if (!strcmp(argv[i], "--cd32")) display_set_akiko(1);
            else if (!strcmp(argv[i], "--time")) {
                want_time = 1;
                display_set_timing_mode(1);
                audio_set_timing_mode(1);
            }
            else if (!strcmp(argv[i], "--fullscreen")) fullscreen = 1;
            else if (!strcmp(argv[i], "--hls-low")) hls_low = 1;
            else if (!strncmp(argv[i], "--hls-max-width=", 16))
                hls_max_width = (unsigned)strtoul(argv[i] + 16, NULL, 10);
            else if (!strncmp(argv[i], "--hls-max-height=", 17))
                hls_max_height = (unsigned)strtoul(argv[i] + 17, NULL, 10);
            else if (!strncmp(argv[i], "--hls-max-fps=", 14))
                hls_max_fps = (unsigned)strtoul(argv[i] + 14, NULL, 10);
            else if (!strncmp(argv[i], "--net-queue=", 12))
                net_queue = (int)strtoul(argv[i] + 12, NULL, 10);
            else if (!strcmp(argv[i], "--live-resync")) live_resync = 1;
            else if (!strncmp(argv[i], "--h264-speed=", 13)) {
                const char *mode = argv[i] + 13;
                if (!strcmp(mode, "auto")) h264_speed = -1;
                else if (!strcmp(mode, "quality")) h264_speed = MR_H264_SPEED_QUALITY;
                else if (!strcmp(mode, "balanced")) h264_speed = MR_H264_SPEED_BALANCED;
                else if (!strcmp(mode, "fast")) h264_speed = MR_H264_SPEED_FAST;
                else if (!strcmp(mode, "turbo")) h264_speed = MR_H264_SPEED_TURBO;
                else if (!strcmp(mode, "turbo+") || !strcmp(mode, "turbo-plus"))
                    h264_speed = MR_H264_SPEED_TURBO_PLUS;
                else if (!strcmp(mode, "turbogt") || !strcmp(mode, "turbo-gt"))
                    h264_speed = MR_H264_SPEED_TURBO_GT;
                else {
                    printf("invalid H.264 speed mode: %s\n", mode);
                    return mrplay_exit(5);
                }
            }
            else if (!strncmp(argv[i], "--audio-rate=", 13)) {
                const char *mode = argv[i] + 13;
                if (!strcmp(mode, "normal")) audio_low_rate = 0;
                else if (!strcmp(mode, "low")) audio_low_rate = 1;
                else {
                    printf("invalid audio rate mode: %s\n", mode);
                    return mrplay_exit(5);
                }
            }
            else if (!strcmp(argv[i], "--no-audio")) no_audio = 1;
            else if (argv[i][0] != '-' && !media_path) media_path = argv[i];
        }
    }
    if (!media_path) {
        printf("no media URL or filename supplied\n");
        return mrplay_exit(5);
    }
    display_set_fullscreen(fullscreen);
    if (!mr_http_options_init(&http_options, user_agent, referer)) {
        printf("invalid HTTP options: %s\n", mr_source_last_error());
        return mrplay_exit(5);
    }
    http_options.hls_low = hls_low;
    http_options.hls_max_width = hls_max_width;
    http_options.hls_max_height = hls_max_height;
    http_options.hls_max_fps = hls_max_fps;
    have_http_options = user_agent || referer || hls_low || hls_max_width ||
                        hls_max_height || hls_max_fps;
    /* Start the background fetch worker before any network call this session
     * might make (including YouTube URL resolution just below) so it is
     * always the first task to open bsdsocket/AmiSSL state, never a second
     * task adopting state another task already opened - see hls_fetch.c's
     * design note. No-op (and harmless) for local files.
     *
     * The playlist/first-segment fetch this can trigger (via mr_youtube_
     * resolve_media() below, or directly once mr_demux_open_file_ex() is
     * reached further down) happens before any display/decoder/trace state
     * exists - service_player_during_io() is not safe to hand a service
     * hook this early (it can reach display_toggle_fullscreen(disp) with a
     * NULL disp, and its scheduler_trace pointer would be dangling before
     * that local is ever initialised). Use the minimal quit-only check
     * below instead, purely so a stuck initial connect can actually be
     * interrupted rather than only ever timing out on its own; the full
     * hook takes over once real playback state exists (see the
     * hls_fetch_set_service() call further down). */
    if (mr_source_is_url(media_path)) {
        hls_fetch_start(want_time);
        hls_fetch_set_service(service_early_quit_check, NULL);
    }
    if (mr_youtube_is_url(media_path)) {
        mr_http_options youtube_options;
        if (!mr_youtube_http_options_init(&youtube_options, &http_options)) {
            printf("invalid YouTube HTTP options: %s\n",
                   mr_source_last_error());
            return mrplay_exit(5);
        }
        http_options = youtube_options;
        have_http_options = 1;
    }
    if (want_time) {
        printf("HTTP User-Agent: %s\n", http_options.user_agent);
        if (http_options.referer[0])
            printf("HTTP Referer: %s\n", http_options.referer);
        if (hls_low)
            printf("HLS preference: low bandwidth (max %ux%u @ %u fps)\n",
                   hls_max_width, hls_max_height, hls_max_fps);
    }
    if (mr_youtube_is_url(media_path)) {
        printf("YouTube: resolving...\n");
        player_status(MR_PLAYER_STATE_OPENING, "",
                      "Resolving YouTube media...");
        if (!mr_youtube_resolve_media(media_path,
                                      have_http_options ? &http_options : NULL,
                                      youtube_media, sizeof youtube_media,
                                      &youtube_kind) ||
            !mr_youtube_media_http_options_init(
                &http_options, have_http_options ? &http_options : NULL)) {
            char reason[MR_PLAYER_STATUS_TEXT_MAX];
            const char *why = mr_source_last_error();
            printf("cannot resolve YouTube media: %s\n", why);
            snprintf(reason, sizeof reason, "YouTube: %s", why);
            player_status(MR_PLAYER_STATE_UNSUPPORTED, "", reason);
            status_hold();
            return mrplay_exit(10);
        }
        have_http_options = 1;
        media_path = youtube_media;
        if (youtube_kind == MR_YOUTUBE_MEDIA_HLS)
            printf("YouTube: live HLS manifest found\n");
        else if (youtube_kind == MR_YOUTUBE_MEDIA_PROGRESSIVE_720P)
            printf("YouTube: progressive 720p MP4 found\n");
        else
            printf("YouTube: progressive 360p MP4 found\n");
        /* A progressive YouTube URL is a finite MP4.  The GUI enables
         * --live-resync for IPTV by default, but carrying that policy into a
         * VOD turns its clean EOF into a bogus "Reconnecting..." cycle. */
        if (youtube_kind != MR_YOUTUBE_MEDIA_HLS) {
            if (live_resync && want_time)
                printf("YouTube: finite video; live reconnect disabled\n");
            live_resync = 0;
            auto_close_eof = 1;
        }
        if (want_time)
            printf("YouTube: source client %s\n",
                   mr_youtube_last_client());
    }
    /* Resolution (if any) is done, so we now know definitively whether this
     * session is HLS. If it is not, the worker started above (for "any
     * URL", before we could know) is not actually going to be used: stop it
     * now, before mr_demux_open_file_ex() below opens its own connection on
     * this task for progressive/direct playback. Otherwise that connection
     * would adopt whatever bsdsocket/AmiSSL state the worker already opened
     * (e.g. during YouTube resolution above) instead of being this
     * session's sole, fresh owner of it - see hls_fetch.c's design note on
     * why a second task touching that state is unsafe. Safe/idempotent even
     * when the worker was never started (a local file, or mr_source_is_url()
     * was already false). */
    if (!mr_source_is_hls(media_path))
        hls_fetch_stop();

    printf("mrplay: opening %s\n", media_path);
    player_status(MR_PLAYER_STATE_OPENING, "", "Connecting to stream...");
    mr_hls_set_verbose(want_time);

    dx = mr_demux_open_file_ex(media_path,
                               have_http_options ? &http_options : NULL);
    if (dx) {
        printf("streaming %s from %s\n", mr_demux_container_name(dx),
               !strncmp(media_path, "http://", 7) ||
               !strncmp(media_path, "https://", 8) ? "network" : "disk");
    } else {
        if (mr_demux_is_file_backed_container(media_path)) {
            char reason[MR_PLAYER_STATUS_TEXT_MAX];
            const char *why = mr_demux_last_open_error();
            /* A "not supported"/"no decoder" reason is a codec/container we
             * can't play (e.g. HEVC); anything else is a connection/read fault.
             * Report them as different states so the browser can distinguish
             * "wrong codec" from "bad link". */
            int unsupported = why && (strstr(why, "not supported") ||
                                      strstr(why, "unsupported") ||
                                      strstr(why, "no decoder"));
            printf("cannot open stream: %s\n",
                   why ? why : "connection failed");
            if (unsupported) {
                snprintf(reason, sizeof reason, "%s", why);
                player_status(MR_PLAYER_STATE_UNSUPPORTED, "", reason);
            } else {
                snprintf(reason, sizeof reason, "cannot open stream: %s",
                         why ? why : "connection failed");
                player_status(MR_PLAYER_STATE_ERROR, "", reason);
            }
            status_hold();
            return mrplay_exit(10);
        }
        /* MPEG-1 and raw elementary streams still require a contiguous input
         * buffer because their current decoders parse directly from it. */
        buf = slurp(media_path, &len);
        if (!buf) { printf("cannot read %s\n", media_path);
                    player_status(MR_PLAYER_STATE_ERROR, "",
                                  "cannot read stream data");
                    status_hold(); return mrplay_exit(10); }
        printf("loaded %ld bytes\n", len);

        if (mr_mpeg1_probe(buf, (size_t)len)) {  /* .mpg via pl_mpeg         */
            int rc = play_mpeg1(buf, len, loop, want_time, audio_low_rate,
                                no_audio);
            free(buf);
            return mrplay_exit(rc);
        }
        dx = mr_demux_open(buf, (size_t)len);
        if (!dx) {
            printf("unsupported container (need AVI, MOV/MP4, MKV, "
                   "MPEG-TS/PS, raw MJPEG/M4V or MPEG-1)\n");
            player_status(MR_PLAYER_STATE_UNSUPPORTED, "",
                          "unsupported container (not AVI/MOV/MP4/MKV/TS/PS/"
                          "MJPEG/M4V/MPEG-1)");
            status_hold();
            free(buf);
            return mrplay_exit(10);
        }
    }

    vi = mr_demux_video(dx);
    ai = mr_demux_audio(dx);
    mr_demux_describe_video_codec(dx, video_description,
                                  sizeof video_description);
    mr_demux_describe_audio_codec(dx, audio_description,
                                  sizeof audio_description);
    printf("codec probe: container=%s, video=%s, audio=%s\n",
           mr_demux_container_name(dx), video_description, audio_description);
    codec = mr_codec_find(vi->fourcc);
    if (!codec) { char reason[MR_PLAYER_STATUS_TEXT_MAX];
                  snprintf(reason, sizeof reason, "%s has no decoder",
                           video_description);
                  printf("no decoder: %s\n", reason);
                  player_status(MR_PLAYER_STATE_UNSUPPORTED, "", reason);
                  status_hold();
                  mr_demux_close(dx);
                  free(buf); return mrplay_exit(10); }

    if (want_time)
        printf("video fourcc='%c%c%c%c'\n", (int)(vi->fourcc & 255),
               (int)((vi->fourcc >> 8) & 255),
               (int)((vi->fourcc >> 16) & 255),
               (int)((vi->fourcc >> 24) & 255));

    if (mr_decoder_open_config(&dec, codec, vi->width, vi->height,
                               vi->config, vi->config_len) != MR_OK) {
        char reason[MR_PLAYER_STATUS_TEXT_MAX];
        printf("decoder init failed\n");
        snprintf(reason, sizeof reason, "%s decoder failed to initialise",
                 codec->name);
        player_status(MR_PLAYER_STATE_ERROR, codec->name, reason);
        status_hold();
        mr_demux_close(dx);
        free(buf); return mrplay_exit(10);
    }
    if (!apply_h264_speed(&dec, h264_speed, want_time)) {
        player_status(MR_PLAYER_STATE_ERROR, codec->name,
                      "H.264 performance mode was rejected by decoder");
        status_hold();
        mr_decoder_close(&dec); mr_demux_close(dx);
        free(buf); return mrplay_exit(10);
    }

    /* No-ops for a non-H.264 codec (mr_h264_set_timing_enabled checks
     * dec->codec internally) - only worth turning on when --time is
     * actually going to print the h264-stages breakdown it feeds. */
    mr_h264_set_timing_enabled(&dec, want_time);

    h264_pipeline_diag_enabled = want_time && codec == &mr_codec_h264 &&
        (uint64_t)vi->width * (uint64_t)vi->height >= 1280ULL * 720ULL;
    h264_pipeline_width = vi->width;
    h264_pipeline_height = vi->height;
    h264_pipeline_stage = 0;
    if (h264_pipeline_diag_enabled)
        h264_pipeline_checkpoint_player("decoder-open", 0, 0);

    printf("media: file=%s, container=%s, video=%s (%c%c%c%c), "
           "%dx%d, %lu.%03lu fps\n", media_path, mr_demux_container_name(dx),
           codec->name, (int)(vi->fourcc & 255), (int)((vi->fourcc >> 8) & 255),
           (int)((vi->fourcc >> 16) & 255), (int)((vi->fourcc >> 24) & 255),
           vi->width, vi->height,
           (unsigned long)(vi->rate / (vi->scale ? vi->scale : 1)),
           (unsigned long)(((vi->rate % (vi->scale ? vi->scale : 1)) * 1000) /
                           (vi->scale ? vi->scale : 1)));
    printf("%dx%d, opening display...\n", vi->width, vi->height);
    /* Prepare the eventual first-frame status, but do not claim Playing until
     * a frame has actually reached the display. */
    snprintf(playing_detail, sizeof playing_detail, "%dx%d, %s",
             vi->width, vi->height, mr_demux_container_name(dx));
    player_prepare_playing_status(codec->name, playing_detail);
    {
        char line[MR_PLAYER_STATUS_TEXT_MAX];
        snprintf(line, sizeof line, "Opening display for %dx%d %s...",
                 vi->width, vi->height, codec->name);
        player_status(MR_PLAYER_STATE_OPENING, codec->name, line);
    }
    disp = display_open(vi->width, vi->height, "MintVID");
    if (!disp) { printf("cannot open a display (RTG or AGA)\n");
                 player_status(MR_PLAYER_STATE_ERROR, codec->name,
                               "cannot open a display (RTG or AGA)");
                 status_hold();
                 mr_decoder_close(&dec); mr_demux_close(dx);
                 free(buf); return mrplay_exit(10); }
    printf("display backend: %s\n", display_backend_name(disp));
    player_status(MR_PLAYER_STATE_OPENING, codec->name,
                  "Display open; buffering first frame...");
    /* True whenever this (H.264-only) session can go straight from the
     * decoder's YUV420P planes to indexed pixels (aga_supports_yuv_indexed()
     * - core/mr_yuv_dither.h), which now covers three shapes: 1:1 identity
     * (yuv_vscale==1, skips mr_dither_rgb8() entirely - strictly cheaper
     * than decoding through RGB24 first even with no resize needed), the
     * common non-laced-HIRES vertical-only downscale (e.g. 640x360 -> 640x180,
     * yuv_vscale>1, hand-tuned m68k asm), and every other resize shape
     * including an upscale (e.g. 192x108 -> 320x180, yuv_vscale==0, portable
     * C - see queue_copy_yuv_indexed()). Checked first, ahead of
     * use_indexed_queue below: at 1:1 both would otherwise apply, and this
     * is strictly the cheaper of the two. */
    int use_yuv_indexed_queue = 0, yuv_dst_w = 0, yuv_dst_h = 0;
    int yuv_vscale = 1, indexed_depth = 8, yuv_ham = 0;
    if (codec == &mr_codec_h264)
        use_yuv_indexed_queue = display_supports_yuv_indexed(
            disp, vi->width, vi->height, &yuv_dst_w, &yuv_dst_h, &yuv_vscale,
            &indexed_depth, &yuv_ham);
    /* True for a plain 4-, 5- or 8-plane native indexed configuration (see
     * display_backend.h / aga_supports_indexed()) when the YUV path above
     * doesn't already cover this (non-H.264, or an AGA mode
     * aga_supports_yuv_indexed() itself rejects) session. Queried once: the
     * display mode is fixed for the life of this session, so every queue
     * slot below uses the same format throughout. */
    int use_indexed_queue = !use_yuv_indexed_queue &&
                            display_supports_indexed(disp, &indexed_depth);
    /* Every remaining H.264 display path consumes RGB24.  Keep libavc's
     * output planar and convert it directly into each queue slot instead of
     * first allocating/filling the decoder's private RGB framebuffer and
     * immediately copying that full frame into the queue.  This is primarily
     * the RTG CGX/P96 path, but is equally correct for an AGA RGB fallback. */
    int use_yuv_rgb_queue = codec == &mr_codec_h264 &&
                            !use_yuv_indexed_queue && !use_indexed_queue;
    int use_yuv_bgr_queue = use_yuv_rgb_queue &&
                            display_supports_bgr24(disp);
    if (use_yuv_indexed_queue || use_yuv_rgb_queue)
        mr_h264_set_yuv_output(&dec, 1);
    if (want_time) {
        int diag_depth, diag_ham, diag_scale, diag_resize;
        const char *diag_c2p;
        display_aga_describe(&diag_depth, &diag_ham, &diag_scale,
                             &diag_resize, &diag_c2p);
        if (diag_depth >= 0)
            printf("AGA path: depth=%d ham=%d scale=%d resize=%d c2p=%s "
                   "yuv=%s\n", diag_depth, diag_ham, diag_scale, diag_resize,
                   diag_c2p, use_yuv_indexed_queue ? "supported"
                                                   : "unsupported");
        if (use_yuv_indexed_queue && yuv_ham)
            printf("video path: YUV420P %dx%d -> HAM%d %dx%d %s\n",
                   vi->width, vi->height, yuv_ham, yuv_dst_w, yuv_dst_h,
                   yuv_vscale > 1 ? "(vscale path)" : "(unexpected shape)");
        else if (use_yuv_indexed_queue)
            printf("video path: YUV420P %dx%d -> INDEX%d %dx%d %s\n",
                   vi->width, vi->height, indexed_depth, yuv_dst_w, yuv_dst_h,
                   yuv_vscale == 1 ? "(1:1 identity)" :
                   yuv_vscale > 1  ? "(vscale asm path)" :
                                     "(general resize path)");
        else if (use_indexed_queue)
            printf("video path: RGB24 %dx%d -> INDEX%d (queue_copy_indexed)\n",
                   vi->width, vi->height, indexed_depth);
        else if (use_yuv_bgr_queue)
            printf("video path: YUV420P %dx%d -> BGR24 "
                   "(direct-to-queue; P96 native)\n", vi->width, vi->height);
        else if (use_yuv_rgb_queue)
            printf("video path: YUV420P %dx%d -> RGB24 "
                   "(direct-to-queue)\n", vi->width, vi->height);
        else
            printf("video path: RGB24 %dx%d (queue_copy)\n",
                   vi->width, vi->height);
    }

    /* Every decoder feeds signed S16 to the common Paula sink.  In particular,
     * PCM byte signedness is resolved before downmixing and S16-to-S8 output. */
    {
        if (no_audio) {
            printf("audio: disabled (--no-audio)\n");
        } else if (ai->valid && ai->format_tag == MR_AUDIO_FORMAT_PCM) {
            audio_dec = mr_audio_decoder_open(ai, audio_low_rate);
            if (want_time && audio_dec) {
                if (ai->codec_tag > 0xffff)
                    printf("audio: %s %s %lu Hz (%c%c%c%c)\n",
                           mr_audio_decoder_name(audio_dec),
                           ai->channels == 1 ? "mono" : "stereo",
                           (unsigned long)ai->sample_rate,
                           (int)(ai->codec_tag & 255),
                           (int)((ai->codec_tag >> 8) & 255),
                           (int)((ai->codec_tag >> 16) & 255),
                           (int)((ai->codec_tag >> 24) & 255));
                else
                    printf("audio: %s %s %lu Hz\n",
                           mr_audio_decoder_name(audio_dec),
                           ai->channels == 1 ? "mono" : "stereo",
                           (unsigned long)ai->sample_rate);
            }
            if (audio_dec)
                audio = audio_open(mr_audio_decoder_rate(audio_dec),
                                   (int)mr_audio_decoder_channels(audio_dec), 16);
            if (audio && audio_dec)
                printf("audio: Paula out, %u Hz (%s, %u ch)\n",
                       mr_audio_decoder_rate(audio_dec),
                       mr_audio_decoder_name(audio_dec),
                       mr_audio_decoder_channels(audio_dec));
            else {
                printf("audio: unsupported PCM layout or Paula open failed, "
                       "playing silent\n");
                audio_unavailable = 1;
                audio_failure = !audio_dec ? "decoder rejected stream setup"
                                           : "Paula/audio.device open failed";
                if (audio_dec) {
                    mr_audio_decoder_close(audio_dec);
                    audio_dec = NULL;
                }
            }
        } else if (ai->valid &&
                   (ai->format_tag == MR_AUDIO_FORMAT_MP2 ||
                    ai->format_tag == MR_AUDIO_FORMAT_MP3 ||
                    ai->format_tag == MR_AUDIO_FORMAT_AAC ||
                    ai->format_tag == MR_AUDIO_FORMAT_AC3)) {
            audio_dec = mr_audio_decoder_open(ai, audio_low_rate);
            if (audio_dec)
                audio = audio_open(mr_audio_decoder_rate(audio_dec),
                                   (int)mr_audio_decoder_channels(audio_dec), 16);
            if (audio && audio_dec)
                printf("audio: Paula out, %u Hz (%s, %u ch)\n",
                       mr_audio_decoder_rate(audio_dec),
                       mr_audio_decoder_name(audio_dec),
                       mr_audio_decoder_channels(audio_dec));
            else {
                printf("audio: unsupported %s setup or Paula open failed, "
                       "playing silent\n",
                       ai->format_tag == MR_AUDIO_FORMAT_MP2 ? "MP2" :
                       ai->format_tag == MR_AUDIO_FORMAT_MP3 ? "MP3" :
                       ai->format_tag == MR_AUDIO_FORMAT_AC3 ? "AC-3" : "AAC");
                audio_unavailable = 1;
                audio_failure = !audio_dec ? "decoder rejected stream setup"
                                           : "Paula/audio.device open failed";
                if (audio_dec) {
                    mr_audio_decoder_close(audio_dec);
                    audio_dec = NULL;
                }
            }
        } else if (strcmp(audio_description, "none detected")) {
            printf("audio: %s is not supported; playing silent\n",
                   audio_description);
            audio_unavailable = 1;
            audio_failure = "codec unsupported";
        }
    }
    {
        size_t used = strlen(playing_detail);
        if (no_audio)
            snprintf(playing_detail + used, sizeof playing_detail - used,
                     "; audio disabled (--no-audio)");
        else if (audio_unavailable)
            snprintf(playing_detail + used, sizeof playing_detail - used,
                     "; audio %s unavailable: %s (silent)", audio_description,
                     audio_failure ? audio_failure : "initialisation failed");
        else
            snprintf(playing_detail + used, sizeof playing_detail - used,
                     "; audio %s",
                     strcmp(audio_description, "none detected")
                         ? audio_description : "none detected (silent)");
        player_prepare_playing_status(codec->name, playing_detail);
    }
    control_audio = audio;
    if (audio) audio_set_volume(audio, control_volume);

    ticks = frame_ticks(vi->rate, vi->scale);
    memset(&trace, 0, sizeof trace);
    trace.audio = audio; trace.enabled = want_time;
    trace.phase = "startup"; trace.phase_started_us = monotonic_us();
    display_set_service(disp, audio ? service_audio_for_display : NULL, &trace);
    mr_demux_set_service(dx, audio ? service_audio_for_display : NULL, &trace);
    mr_h264_set_service(&dec, audio ? service_audio_for_display : NULL, &trace);
    /* Off by default: mr_ts_next_packet() (several clock() reads per
     * 188/192-byte TS packet) and mr_source_read_at()/the HLS playlist and
     * segment fetch timers (two clock() reads per source read) only ever
     * feed the --time report. No-ops for any non-TS container / non-network
     * source. */
    mr_demux_set_timing_enabled(dx, want_time);
    mr_source_set_timing_enabled(want_time);
    /* Keep audio/video/UI alive (and quit responsive) while the HLS live path
     * polls for new segments at the edge; see hls_wait_service. */
    mr_hls_set_wait(hls_wait_service, &trace);
    /* Present queued video / feed audio between the socket reads of a blocking
     * segment fetch, so an ordinary ~350 ms download no longer freezes video for
     * its whole duration. Fires while released is set around the demux read. */
    mr_http_set_service(service_player_during_io, &trace);
    /* Same contract, for whichever fetches the background worker performs on
     * our behalf while we wait on it (see hls_fetch_wait_busy()). */
    hls_fetch_set_service(service_player_during_io, &trace);
    {
        unsigned long period = vi->rate ? (1000UL * (vi->scale ? vi->scale : 1)
                                           / vi->rate) : 83;
        if (period < 1) period = 1;

    printf("playing: space=pause, F=fullscreen, %s, ESC=quit%s...\n",
           mr_demux_can_seek(dx) ? "left/right=seek 10s"
                                 : "right=fast (no seek index for this file)",
           loop ? ", loop on" : "");

    memset(vq, 0, sizeof vq);
    memset(&stats, 0, sizeof stats);
    stats.since_us = monotonic_us();
    mr_source_timing_reset();
    {
        struct EClockVal ev;
        ULONG hz = TimerBase ? ReadEClock(&ev) : 0;
        if (want_time)
            {
                unsigned long gran_ns = hz ? 1000000000UL / hz : 0;
                printf("timer: EClock=%lu Hz (%lu.%03lu us nominal), "
                       "DOS tick=20.000 ms\n", (unsigned long)hz,
                       gran_ns / 1000, gran_ns % 1000);
            }
    }

    {
        int playback_started = 0;
        int network_source = mr_source_is_url(media_path);
        int frames_at_last_reconnect = 0, reconnects_without_progress = 0;
        int startup_depth = network_source ? 1 : 2;
        /* Network sources default to a single decoded frame (see the comment on
         * the decode gate below). --net-queue=N opts into a read-ahead cushion.
         * A few frames smooth per-frame decode jitter (e.g. a GOP-boundary
         * I-frame); a deep buffer (tens of frames) additionally lets video sit
         * ahead of the audio clock and present in PTS order as frames fall due,
         * keeps the loop demuxing so the audio FIFO stays fed, and carries the
         * picture across a segment-boundary refetch. Clamped to VIDEO_QUEUE_CAP;
         * startup latency is unchanged because startup_depth still gates when
         * playback begins. */
        int net_target = net_queue > 0
            ? (net_queue > VIDEO_QUEUE_CAP ? VIDEO_QUEUE_CAP : net_queue) : 1;
        int target_depth = network_source ? net_target : 3;
        /* Decoded-frame queue depth. As deep as RAM allows: while video runs
         * ahead of the audio clock, topping up the audio cushion decodes past
         * the queue cap and discards those frames, so the presented rate is
         * roughly depth / cushion_seconds - a deeper queue shows more frames
         * before each gap. A stall longer than the queue drains it and flips
         * playback into the smooth present-as-decoded regime (video trailing the
         * clock, no discards); a deeper queue also rides short stalls outright.
         * The clamps below keep the footprint within a safe slice of free RAM,
         * and video_cap is the ring modulus so the queue's footprint is
         * exactly video_cap * frame_bytes (RGB24, indexed or YUV-indexed,
         * whichever this session actually queues - direct YUV->RGB still
         * produces an ordinary RGB24 queue slot; see frame_bytes below). */
        unsigned long cushion_ms;
        /* Must match whichever queue_copy* the copy_ok dispatch below picks
         * for this session (queue_copy_yuv_indexed()/queue_copy_yuv_rgb24()/
         * queue_copy_indexed()/queue_copy() - see their declarations), or this
         * budget either
         * overestimates and needlessly clamps video_cap (RGB24 assumed on an
         * indexed path is a 3x-6x overestimate), or underestimates and lets
         * the queue overrun its RAM budget. */
        size_t frame_bytes = use_yuv_indexed_queue
                            ? (size_t)yuv_dst_w * (size_t)yuv_dst_h
                            : use_indexed_queue
                            ? (size_t)vi->width * (size_t)vi->height
                            : (size_t)vi->width * (size_t)vi->height * 3;
        ULONG free_any = AvailMem(MEMF_ANY);
        /* Only a third of the (post-floor) free pool is a safety ceiling; the
         * shallow default is far below it, but it protects a tight machine and
         * an explicit --net-queue. video_cap is the ring modulus below, so
         * the queue's footprint is exactly video_cap * frame_bytes. */
        size_t budget = free_any > VIDEO_QUEUE_MEM_FLOOR
                      ? (size_t)(free_any - VIDEO_QUEUE_MEM_FLOOR) / 3 : 0;
        int budget_frames = frame_bytes ? (int)(budget / frame_bytes) : 0;
        int video_cap = network_source ? VIDEO_QUEUE_NET_DEPTH
                                        : VIDEO_QUEUE_DISK_DEPTH;
        /* VIDEO_QUEUE_NET_DEPTH above is only a starting default, not a
         * floor - the budget_frames clamp below can still pull it under 16
         * on a tight machine (e.g. a large frame size at 720p+). A live HLS
         * segment fetch can stall for hundreds of ms up to well over a
         * second (observed on YouTube live - one segment of lookahead is
         * all the single-TLS-connection design in hls_fetch.c allows), so
         * 16 frames (~466 ms of cushion at 30fps) is nowhere near enough
         * headroom on a machine with room to spare, and every stall shows up
         * as audio-clock drift that compounds until a hard live-resync jump.
         * Let a healthy RAM budget grow the network queue past that default
         * instead of requiring an explicit --net-queue for every live
         * stream; the budget_frames/VIDEO_QUEUE_CAP clamps below still
         * apply either way. */
        if (network_source && budget_frames > video_cap)
            video_cap = budget_frames;
        if (net_queue > 0 && video_cap < net_target) video_cap = net_target;
        if (video_cap < target_depth) video_cap = target_depth;
        if (budget_frames > 0 && video_cap > budget_frames)
            video_cap = budget_frames;
        if (video_cap < 2) video_cap = 2;          /* ring needs >= 2 slots    */
        if (video_cap > VIDEO_QUEUE_CAP) video_cap = VIDEO_QUEUE_CAP;
        if (network_source) {
            uint64_t frame_period_us = vi->rate
                ? (uint64_t)(vi->scale ? vi->scale : 1) * 1000000ULL / vi->rate
                : 83333ULL;
            uint64_t queue_cushion_us =
                (uint64_t)(video_cap - 2) * frame_period_us;
            cushion_ms = (unsigned long)(queue_cushion_us / 1000ULL);
            if (cushion_ms < AUDIO_CUSHION_MIN_MS)
                cushion_ms = AUDIO_CUSHION_MIN_MS;
            if (cushion_ms > AUDIO_CUSHION_TARGET_MS)
                cushion_ms = AUDIO_CUSHION_TARGET_MS;
        } else {
            cushion_ms = AUDIO_CUSHION_TARGET_MS;
        }
        int prev_master_source = -1;
        if (want_time)
            printf("video-queue: cap=%d frames (%s, cushion=%lu ms, "
                   "frame=%lu KB, free=%lu KB)\n",
                   video_cap, network_source ? "network" : "disk", cushion_ms,
                   (unsigned long)(frame_bytes / 1024),
                   (unsigned long)(free_any / 1024));

        /* Wire the present-during-fetch context onto the shared service callback.
         * It touches the queue only while `released` is set around the blocking
         * demux read below, so it can advance video through a segment stall even
         * while the single playback loop is blocked in the synchronous reader. */
        video_presenter presenter;
        presenter.released = 0;
        presenter.presenting = 0;
        presenter.disp = disp;
        presenter.audio = audio;
        presenter.mc = &mc;
        presenter.vi = vi;
        presenter.vq = vq;
        presenter.video_cap = video_cap;   /* same ring modulus the loop uses  */
        presenter.qhead = &qhead;
        presenter.qcount = &qcount;
        presenter.playback_started = &playback_started;
        presenter.mono_base_us = &mono_base_us;
        presenter.frames = &frames;
        presenter.stats = &stats;
        presenter.want_time = want_time;
        /* present_service_frame() only needs to know whether front->rgb
         * holds indexed data or RGB24 - front->width/height already carry
         * whichever path's real dimensions (source size for
         * use_indexed_queue, the fitted downscaled size for
         * use_yuv_indexed_queue - see queue_copy_indexed()/
         * queue_copy_yuv_indexed()), so one flag covers both. */
        presenter.use_indexed = use_indexed_queue || use_yuv_indexed_queue;
        presenter.use_bgr = use_yuv_bgr_queue;
        trace.presenter = &presenter;

    /* The live-resync term keeps the loop alive on EOF so the reconnect block in
     * the body can run; without it the loop would exit here (empty queue, no
     * --loop) before reconnect ever gets a chance. That block owns its own exits
     * (give-up, mismatch, no-progress backstop). */
    while (!quit && (!input_eof || qcount || loop ||
                     (network_source && live_resync))) {
        queued_video *front = qcount ? &vq[qhead] : NULL;
        uint64_t now = monotonic_us();
        uint64_t period_us = vi->rate
            ? (uint64_t)(vi->scale ? vi->scale : 1) * 1000000ULL / vi->rate
            : 83333ULL;
        int64_t late_us = 0;
        uint64_t master_clock_us = 0;
        unsigned long audio_ms = 0;
        int startup_refill = 0;
        int have_deadline = 0;
        int starved = 1;
        uint64_t audio_elapsed_raw_us = 0;
        uint64_t audio_media_clock_us = 0;
        uint64_t mono_media_clock_us = now - mono_base_us;

        /* Build the startup cushion in the software FIFO before starting
         * Paula; otherwise each small packet starts playing immediately and
         * the prebuffer can never grow to the warning threshold. */
        trace_phase(&trace, "scheduler");
        if (audio) audio_ms = audio_buffered_ms(audio);
        startup_refill = audio && !playback_started &&
                         audio_ms < AUDIO_STARTUP_TARGET_MS;
        if (rescue_cooldown) rescue_cooldown--;
        {
            mr_audio_diagnostics rd;
            int rescue_critical = 0;
            int rescue_needed = 0;
            memset(&rd, 0, sizeof rd);
            if (audio) {
                unsigned long hardware_ms;
                audio_diagnostics(audio, &rd);
                hardware_ms = rd.hardware_playing_remaining_ms +
                              rd.hardware_queued_ms;
                rescue_critical =
                    rd.fifo_buffered_ms < AUDIO_RESCUE_FIFO_NEAR_EMPTY_MS &&
                    hardware_ms < AUDIO_RESCUE_ONE_REQUEST_MS;
                /* With two live requests, a useful FIFO cushion is healthy;
                 * the output task no longer needs scheduler intervention.
                 * Rescue only a genuinely short PCM horizon or a request
                 * queue which has already fallen below two active writes. */
                rescue_needed = rescue_critical ||
                    (audio_ms < AUDIO_RESCUE_ENTRY_MS &&
                     (rd.active_requests < 2 ||
                      rd.fifo_buffered_ms < AUDIO_RESCUE_FIFO_NEAR_EMPTY_MS));
            }
        if (audio && playback_started && !rescue_active && !input_eof &&
            !rescue_cooldown && (rescue_priority || rescue_needed)) {
            rescue_active = 1;
            rescue_priority = 1;
            rescue_episode_critical = rescue_critical;
            rescue_started_us = now;
            rescue_buffer_before = audio_ms;
            rescue_entry_threshold = AUDIO_RESCUE_ENTRY_MS;
            rescue_min_buffer = audio_ms;
            rescue_hw_before = rd.hardware_starvations;
            rescue_episode_packets = rescue_episode_audio = 0;
            rescue_episode_video = rescue_episode_queued = 0;
            rescue_episode_skipped = rescue_episode_replaced = 0;
            rescue_newest_retained_pts_us = 0;
            stats.rescue_entries++;
            if (rescue_critical) stats.rescue_critical++;
            else stats.rescue_noncritical++;
        }
        }
        if (rescue_active) {
            const char *reason = NULL;
            uint64_t rescue_elapsed = now - rescue_started_us;
            mr_audio_diagnostics current_audio;
            int ev = player_event(disp);
            memset(&current_audio, 0, sizeof current_audio);
            audio_diagnostics(audio, &current_audio);
            if (audio_ms < rescue_min_buffer) rescue_min_buffer = audio_ms;
            if (ev == MR_EV_QUIT) { quit = 1; break; }
            if (ev == MR_EV_PAUSE) { paused = 1; reason = "limit"; }
            if (rescue_episode_audio && current_audio.active_requests == 2 &&
                audio_ms >= AUDIO_RESCUE_ENTRY_MS) {
                reason = "audio"; stats.rescue_exit_target++;
            } else if (audio_ms >= AUDIO_RESCUE_TARGET_MS) {
                reason = "target"; stats.rescue_exit_target++;
            } else if (rescue_episode_packets >= AUDIO_RESCUE_MAX_PACKETS ||
                       rescue_elapsed >= AUDIO_RESCUE_MAX_US || paused) {
                reason = "limit"; stats.rescue_exit_limit++;
            } else if (input_eof) {
                reason = "eof"; stats.rescue_exit_eof++;
            }
            if (reason) {
                long margin_ms = (long)rescue_buffer_before -
                                 (long)(rescue_elapsed / 1000);
                mr_audio_diagnostics rd;
                audio_diagnostics(audio, &rd);
                rescue_active = 0;
                rescue_priority = 0;
                if (reason[0] == 'l') rescue_cooldown = 2;
                stats.rescue_us += rescue_elapsed;
                if (rescue_elapsed > stats.rescue_max_us)
                    stats.rescue_max_us = (unsigned long)rescue_elapsed;
                if (stats.rescue_entries == 1 || margin_ms < stats.rescue_min_margin_ms)
                    stats.rescue_min_margin_ms = margin_ms;
                if (margin_ms < 0) stats.rescue_negative_margin++;
                stats.rescue_hw_starvations +=
                    rd.hardware_starvations - rescue_hw_before;
                stats.rescue_newest_retained_pts_us =
                    rescue_newest_retained_pts_us;
                if (qcount && playback_started) {
                    uint64_t rescue_clock =
                        media_clock_rescue_estimate(&mc,
                                                    audio_elapsed_us(audio));
                    queued_video *new_front = &vq[qhead];
                    int64_t post_late = (int64_t)rescue_clock -
                                        (int64_t)new_front->pts_us;
                    while (post_late > (int64_t)period_us && qcount > 1) {
                        stats.dropped++;
                        qhead = (qhead + 1) % video_cap;
                        qcount--;
                        new_front = &vq[qhead];
                        post_late = (int64_t)rescue_clock -
                                    (int64_t)new_front->pts_us;
                    }
                    stats.rescue_post_lateness_us = post_late;
                }
                front = qcount ? &vq[qhead] : NULL;
                /* One rescue episode is ~200ms by design (AUDIO_RESCUE_MAX_US),
                 * so a machine where rescue keeps failing back-to-back can
                 * produce one of these lines roughly every scheduler pass -
                 * 910 of them in one ~67 s constrained-hardware run. Rate-limit
                 * like clock-trace: still enough samples to see the pattern,
                 * without --time logging adding print overhead on nearly every
                 * iteration of an already CPU-starved playback loop. */
                if (want_time &&
                    now - trace.last_rescue_print_us >= CLOCK_TRACE_MIN_INTERVAL_US) {
                    trace.last_rescue_print_us = now;
                    printf("audio-rescue reason=%s urgency=%s packets=%u audio=%u "
                           "video=%u retained=%u replaced=%u reference-only=%u "
                           "newest-pts=%lu post-late=%ld us duration=%lu us "
                           "buffer=%lu->%lu ms min=%lu ms consumed=%lu ms "
                           "entry-threshold=%lu ms margin=%ld ms "
                           "hw-starvations=%lu\n", reason,
                           rescue_episode_critical ? "critical" : "warning",
                           rescue_episode_packets, rescue_episode_audio,
                           rescue_episode_video, rescue_episode_queued,
                           rescue_episode_replaced, rescue_episode_skipped,
                           (unsigned long)rescue_newest_retained_pts_us,
                           (long)stats.rescue_post_lateness_us,
                           (unsigned long)rescue_elapsed,
                           rescue_buffer_before, audio_ms, rescue_min_buffer,
                           (unsigned long)(rescue_elapsed / 1000),
                           rescue_entry_threshold, margin_ms,
                           rd.hardware_starvations - rescue_hw_before);
                }
            }
        }
        if (playback_started && front) {
            starved = audio ? audio_starved(audio) : 1;
            audio_elapsed_raw_us = audio ? audio_elapsed_us(audio) : 0;
            mono_media_clock_us = now - mono_base_us;
            if (audio) {
                audio_media_clock_us = current_media_clock_us(
                    &mc, 1, starved, audio_elapsed_raw_us, want_time);
                master_clock_us = audio_media_clock_us;
            } else {
                audio_media_clock_us = 0;
                mc.source = MCLOCK_MONO;
                master_clock_us = mono_media_clock_us;
            }
            late_us = (int64_t)master_clock_us - (int64_t)front->pts_us;
            have_deadline = 1;
        }

        /* Catastrophic fall-behind recovery. A multi-second network stall can
         * leave a live stream many seconds behind the wall clock, the audio
         * clock unable to climb back. Fast-consume the buffered backlog (decode
         * video reference-only, discard audio) up to the download frontier, then
         * flush audio and re-prime exactly like first start - landing near the
         * live edge. Opt-in, network-only; the trigger sits far above the ~1 s
         * lag ordinary boundary stalls produce, so it never fires in normal
         * playback. have_deadline guarantees both clocks are valid here. */
        if (live_resync && network_source && audio && have_deadline &&
            mono_media_clock_us >
                audio_media_clock_us + (uint64_t)LIVE_RESYNC_BEHIND_US) {
            uint64_t behind = mono_media_clock_us - audio_media_clock_us;
            uint64_t want = behind > LIVE_RESYNC_TARGET_US
                          ? behind - LIVE_RESYNC_TARGET_US : behind;
            uint64_t cu_start = monotonic_us();
            uint64_t base_pts = 0;
            int have_base = 0;
            if (want_time)
                printf("live-resync: %lu ms behind live, catching up\n",
                       (unsigned long)(behind / 1000));
            audio_set_running(audio, 0);
            display_set_status(disp, "Buffering...");
            qcount = 0; qhead = 0;               /* stale pictures, far behind */
            mr_h264_set_skip_output(&dec, 1);    /* reference-only: fast/no RGB */
            for (;;) {
                uint64_t r0, rdt;
                mr_status ns;
                if (player_event(disp) == MR_EV_QUIT) { quit = 1; break; }
                r0 = monotonic_us();
                ns = mr_demux_next_packet(dx, &pkt);
                rdt = monotonic_us() - r0;
                if (ns != MR_OK) { input_eof = 1; break; }
                if (rdt > LIVE_RESYNC_EDGE_US) break;   /* reached the frontier */
                if (pkt.is_video && pkt.len) {
                    mr_h264_set_input_pts(&dec, pkt.has_pts, pkt.pts_us);
                    mr_h264_set_input_annexb(&dec, pkt.is_annexb);
                    mr_decoder_decode(&dec, pkt.data, pkt.len);
                }
                /* audio packets are discarded: the FIFO is flushed below */
                if (pkt.has_pts) {
                    if (!have_base) { base_pts = pkt.pts_us; have_base = 1; }
                    else if (pkt.pts_us > base_pts &&
                             pkt.pts_us - base_pts >= want) break;
                }
                if (monotonic_us() - cu_start > LIVE_RESYNC_MAX_US) break;
            }
            mr_h264_set_skip_output(&dec, 0);
            audio_flush(audio);
            /* Re-prime from the new position; the startup path below refills the
             * queue and audio cushion and restarts playback near the edge. */
            playback_started = 0;
            decoded_index = 0; mono_base_us = 0;
            have_container_pts = 0; last_container_pts_us = 0;
            container_pts_adjust_us = 0;
            media_clock_rebase(&mc, audio_elapsed_us(audio), 0);
            stats.timing_rebases++;
            continue;
        }

        /* Presentation used to be gated on !rescue_active, blacking out the
         * screen for the entire duration of every audio-rescue episode. That
         * made sense if rescue reliably fixed the underrun quickly, but on a
         * bandwidth-starved network source it does not: a WinUAE log showed
         * 96% of episodes (393/408) exiting via the AUDIO_RESCUE_MAX_US/
         * AUDIO_RESCUE_MAX_PACKETS timeout ("limit"), not because audio
         * recovered, each one still blanking the screen for up to its ~100ms
         * budget (longer still when it overruns via a blocking demux read -
         * observed rescue durations past 500ms). Because the deadline-drop
         * loop below - the only place that prunes stale queued frames - lived
         * inside this same gated block, the backlog was never trimmed while
         * rescue ran either: post-late (the front frame's lateness after
         * rescue's own catch-up attempt) grew every reporting interval,
         * 7.6ms up to 7.87s over one session, and presented fps collapsed
         * to a fraction of decoded fps even during stretches with negligible
         * network blocking. Letting presentation run unconditionally lets
         * the drop loop keep the queue near-live every iteration regardless
         * of whether a rescue is concurrently in flight, and RTG/CGX blits
         * do not contend with Paula's audio DMA the way a custom-chip
         * blitter would, so there is no hardware reason to hold it back. */
        if (have_deadline &&
            late_us >= -(int64_t)PRESENTATION_GUARD_US) {
            int ev;
            trace_phase(&trace, "event-processing");
            ev = player_event(disp);
            if (ev == MR_EV_QUIT) { quit = 1; break; }
            if (ev == MR_EV_PAUSE) {
                paused = 1;
                if (audio) audio_set_running(audio, 0);
            }
            if (ev == MR_EV_SEEK_FWD || ev == MR_EV_SEEK_BACK)
                printf("seek: received %s, can_seek=%d\n",
                       ev == MR_EV_SEEK_FWD ? "FWD" : "BACK",
                       mr_demux_can_seek(dx));
            if ((ev == MR_EV_SEEK_FWD || ev == MR_EV_SEEK_BACK) &&
                mr_demux_can_seek(dx)) {
                /* Real offline-file seek: jump the demux index straight to
                 * the nearest keyframe, then re-prime exactly like the
                 * loop-restart/live-reconnect paths above do - same reset
                 * calls, same "let the startup logic re-derive timing from
                 * the next real packet" idiom, just landing mid-file instead
                 * of at 0. Network sources fall through to the old
                 * fast-forward toggle below; they have no keyframe index. */
                uint64_t out_us;
                int64_t cur_pts_us = front ? (int64_t)front->pts_us
                                    : last_seek_pts_us >= 0 ? last_seek_pts_us
                                    : (have_deadline ? (int64_t)master_clock_us : 0);
                int64_t target_us = cur_pts_us +
                    (ev == MR_EV_SEEK_FWD ? MR_SEEK_STEP_US : -MR_SEEK_STEP_US);
                if (target_us < 0) target_us = 0;
                if (mr_demux_seek(dx, (uint64_t)target_us, &out_us) == MR_OK) {
                    last_seek_pts_us = (int64_t)out_us;
                    if (audio) { audio_set_running(audio, 0); audio_flush(audio); }
                    display_set_status(disp, "Seeking...");
                    if (mr_decoder_reset(&dec) != MR_OK ||
                        !apply_h264_speed(&dec, h264_speed, 0)) break;
                    mr_h264_set_timing_enabled(&dec, want_time);
                    if (use_yuv_indexed_queue || use_yuv_rgb_queue)
                        mr_h264_set_yuv_output(&dec, 1);
                    if (audio_dec) mr_audio_decoder_reset(audio_dec);
                    qcount = 0; qhead = 0;
                    playback_started = 0;
                    /* decoded_index drives synthetic_pts (= decoded_index *
                     * period_us, the same product either way period_us is
                     * derived) below, used whenever a decoded frame's own
                     * container PTS isn't available yet - e.g. while the
                     * H.264 reorder buffer is still refilling right after
                     * mr_decoder_reset() above, which every seek forces.
                     * Resetting it to 0 here (as loop-restart/live-reconnect
                     * correctly do, since those really do restart at time 0)
                     * made those early post-seek frames report a small
                     * near-zero pts even once the have_container_pts fix
                     * below stopped clobbering real container timestamps -
                     * seed it from the seek target instead, same as
                     * last_container_pts_us. */
                    decoded_index = period_us ? out_us / period_us : 0;
                    mono_base_us = 0;
                    /* Unlike a loop-restart/live-reconnect (which genuinely
                     * begins again at container time 0, so a zero baseline is
                     * correct), a seek lands mid-file: seed the discontinuity
                     * baseline at the seek target itself, not 0. Leaving
                     * have_container_pts/last_container_pts_us zeroed here
                     * made the very first frame decoded after a seek look
                     * like a discontinuity against a synthetic zero clock,
                     * which set container_pts_adjust_us = 0 - frame_pts_us -
                     * forcing every displayed pts to 0 - (frame_pts_us) = 0
                     * regardless of where mr_demux_seek() actually landed.
                     * That is what made cur_pts_us (the next seek's origin)
                     * read back near 0 even once front was populated again,
                     * so repeated presses kept retargeting the same ~10s
                     * mark instead of accumulating - last_seek_pts_us above
                     * only covered the gap before front existed at all. */
                    have_container_pts = 1; last_container_pts_us = out_us;
                    container_pts_adjust_us = 0;
                    input_eof = 0;
                    if (audio) media_clock_rebase(&mc, audio_elapsed_us(audio), out_us);
                    else memset(&mc, 0, sizeof mc);
                    stats.timing_rebases++;
                    continue;
                }
                printf("seek: mr_demux_seek() failed for target %lld us "
                       "(container reports seekable but couldn't reposition)\n",
                       (long long)target_us);
                display_set_status(disp, "Seek failed");
            } else if (ev == MR_EV_SEEK_FWD) fast_forward = !fast_forward;
            while (paused && !quit) {
                ev = player_event(disp);
                if (ev == MR_EV_QUIT) quit = 1;
                else if (ev == MR_EV_PAUSE) {
                    paused = 0; mono_base_us = monotonic_us() - front->pts_us;
                    if (audio) {
                        media_clock_rebase(&mc, audio_elapsed_us(audio),
                                           front->pts_us);
                        stats.timing_rebases++;
                        audio_set_running(audio, 1);
                    }
                }
                {
                    uint64_t delay_begin = monotonic_us();
                    Delay(1);
                    trace.delay_ticks = 1;
                    trace.sleep_actual_us = monotonic_us() - delay_begin;
                }
            }
            if (quit) break;
            now = monotonic_us();
            trace_phase(&trace, "deadline-drop");
            starved = audio ? audio_starved(audio) : 1;
            audio_elapsed_raw_us = audio ? audio_elapsed_us(audio) : 0;
            mono_media_clock_us = now - mono_base_us;
            if (audio) {
                audio_media_clock_us = current_media_clock_us(
                    &mc, 1, starved, audio_elapsed_raw_us, want_time);
                master_clock_us = audio_media_clock_us;
            } else {
                audio_media_clock_us = 0;
                mc.source = MCLOCK_MONO;
                master_clock_us = mono_media_clock_us;
            }
            late_us = (int64_t)master_clock_us - (int64_t)front->pts_us;
            if (late_us > 0) stats.late++;
            stats.dropped_in_pass = 0;
            /* Select once per scheduler pass. Re-evaluate each newer queued
             * PTS against the same audio-clock sample, stopping as soon as it
             * is useful or only the newest decoded frame remains. */
            while (!fast_forward && late_us > (int64_t)period_us && qcount > 1) {
                stats.dropped++; stats.dropped_in_pass++;
                qhead = (qhead + 1) % video_cap; qcount--;
                front = &vq[qhead];
                late_us = (int64_t)master_clock_us - (int64_t)front->pts_us;
            }
            {
                int source_changed = prev_master_source >= 0 &&
                                     prev_master_source != mc.source;
                int64_t delta_clocks_us = (int64_t)audio_media_clock_us -
                                          (int64_t)mono_media_clock_us;
                uint64_t abs_delta_us = delta_clocks_us < 0
                    ? (uint64_t)(-delta_clocks_us) : (uint64_t)delta_clocks_us;
                int big_delta = abs_delta_us > period_us;
                int multi_drop = stats.dropped_in_pass > 1;
                if (want_time && (source_changed || big_delta || multi_drop) &&
                    now - trace.last_clock_trace_us >=
                        CLOCK_TRACE_MIN_INTERVAL_US) {
                    trace.last_clock_trace_us = now;
                    printf("clock-trace src=%c->%c starved=%d "
                           "audio-elapsed=%lu offset=%ld "
                           "audio-clock=%lu mono-clock=%lu delta=%ld "
                           "front-pts=%lu late=%ld qcount=%d drops=%u\n",
                           prev_master_source == MCLOCK_AUDIO ? 'A' :
                               prev_master_source == MCLOCK_HOLDOVER ? 'H' :
                               prev_master_source == MCLOCK_MONO ? 'M' : '?',
                           mc.source == MCLOCK_AUDIO ? 'A' :
                               mc.source == MCLOCK_HOLDOVER ? 'H' : 'M',
                           starved,
                           (unsigned long)audio_elapsed_raw_us,
                           (long)mc.audio_to_media_offset_us,
                           (unsigned long)audio_media_clock_us,
                           (unsigned long)mono_media_clock_us,
                           (long)delta_clocks_us,
                           (unsigned long)(front ? front->pts_us : 0),
                           (long)late_us,
                           qcount,
                           stats.dropped_in_pass);
                }
                prev_master_source = mc.source;
            }
            stats.frame_pts_us = front->pts_us;
            stats.audio_clock_us = master_clock_us;
            stats.calculated_lateness_us = late_us;
            stats.queue_head = (unsigned)qhead;
            if (late_us < -(int64_t)PRESENTATION_GUARD_US) continue;
            /* now/audio_before here only ever feed the --time show_us/
             * rtg-timing bookkeeping below - skip both otherwise. */
            if (want_time) now = monotonic_us();
            {
                unsigned long audio_before =
                    want_time && audio ? audio_buffered_ms(audio) : 0;
            trace_phase(&trace, "cgx-prepare/transfer");
            if (h264_pipeline_diag_enabled && h264_pipeline_stage < 4) {
                h264_pipeline_checkpoint_player("pre-display-main",
                                                qcount, playback_started);
                h264_pipeline_stage = 4;
            }
            /* front->width/height already hold whichever path's real
             * dimensions - source size for use_indexed_queue, the fitted
             * downscaled size for use_yuv_indexed_queue (see
             * queue_copy_indexed()/queue_copy_yuv_indexed()) - so both
             * take the same display_show_indexed() call. */
            if (use_indexed_queue || use_yuv_indexed_queue)
                display_show_indexed(disp, front->rgb, front->width, front->height,
                                     front->stride, front->dirty_y0, front->dirty_y1);
            else if (use_yuv_bgr_queue)
                display_show_bgr24(disp, front->rgb, front->width, front->height,
                                   front->stride, front->dirty_y0,
                                   front->dirty_y1);
            else
                display_show_rgb(disp, front->rgb, front->width, front->height,
                                 front->stride, front->dirty_y0, front->dirty_y1);
            player_first_frame_presented(disp, (int64_t)front->pts_us);
            if (h264_pipeline_diag_enabled && h264_pipeline_stage < 5) {
                h264_pipeline_checkpoint_player("post-display-main",
                                                qcount, playback_started);
                h264_pipeline_stage = 5;
            }
                if (want_time) {
                    mr_display_timing rt;
                    if (display_rtg_frame_timing(disp, &rt)) {
                        stats.rtg_prepare_us += rt.prepare_us;
                        stats.rtg_scale_us += rt.scale_us;
                        stats.rtg_convert_us += rt.convert_us;
                        stats.rtg_copy_us += rt.copy_us;
                        stats.rtg_blit_us += rt.blit_us;
                        stats.rtg_clip_us += rt.clip_us;
                        stats.rtg_total_us += rt.total_us;
                        if (rt.prepare_us > stats.rtg_prepare_max_us)
                            stats.rtg_prepare_max_us = rt.prepare_us;
                        if (rt.blit_us > stats.rtg_blit_max_us)
                            stats.rtg_blit_max_us = rt.blit_us;
                        stats.last_rtg = rt;
                        stats.audio_before = audio_before;
                        stats.audio_after = audio ? audio_buffered_ms(audio) : 0;
                    }
                }
            }
            /* show_us/display_us/display_max_us/convert_us/latency_us only
             * ever feed the --time report (the periodic "rtg timing:" print
             * and the final "timing/N frames:" summary) - skip the
             * monotonic_us() calls and display_aga_frame_timing() query on a
             * normal run. */
            if (want_time) {
                unsigned long show_us = (unsigned long)(monotonic_us() - now);
                unsigned long enc_ms = 0, blit_ms = 0;
                display_aga_frame_timing(&enc_ms, &blit_ms);
                stats.convert_us += (uint64_t)enc_ms * 1000;
                stats.display_us += show_us;
                total_display_us += show_us;
                if (show_us > stats.display_max_us) stats.display_max_us = show_us;
                stats.latency_us += monotonic_us() - front->decoded_at_us;
            }
            stats.presented++; frames++;
            qhead = (qhead + 1) % video_cap; qcount--;
            if (want_time) now = monotonic_us();
            if (want_time && now - stats.since_us >= STATS_INTERVAL_US) {
                trace_phase(&trace, "scheduler-diagnostics");
                if (audio) service_audio_for_display(&trace);
                report_stats(&stats, audio, dx, &trace, qcount, now);
                if (audio) service_audio_for_display(&trace);
            }
            continue;
        }

        if (have_deadline) {
            int source_changed = prev_master_source >= 0 &&
                                 prev_master_source != mc.source;
            int64_t delta_clocks_us = (int64_t)audio_media_clock_us -
                                      (int64_t)mono_media_clock_us;
            uint64_t abs_delta_us = delta_clocks_us < 0
                ? (uint64_t)(-delta_clocks_us) : (uint64_t)delta_clocks_us;
            int big_delta = abs_delta_us > period_us;
            if (want_time && (source_changed || big_delta) &&
                now - trace.last_clock_trace_us >= CLOCK_TRACE_MIN_INTERVAL_US) {
                trace.last_clock_trace_us = now;
                printf("clock-trace src=%c->%c starved=%d "
                       "audio-elapsed=%lu offset=%ld "
                       "audio-clock=%lu mono-clock=%lu delta=%ld "
                       "front-pts=%lu late=%ld qcount=%d drops=%u\n",
                       prev_master_source == MCLOCK_AUDIO ? 'A' :
                           prev_master_source == MCLOCK_HOLDOVER ? 'H' :
                           prev_master_source == MCLOCK_MONO ? 'M' : '?',
                       mc.source == MCLOCK_AUDIO ? 'A' :
                           mc.source == MCLOCK_HOLDOVER ? 'H' : 'M',
                       starved,
                       (unsigned long)audio_elapsed_raw_us,
                       (long)mc.audio_to_media_offset_us,
                       (unsigned long)audio_media_clock_us,
                       (unsigned long)mono_media_clock_us,
                       (long)delta_clocks_us,
                       (unsigned long)(front ? front->pts_us : 0),
                       (long)late_us,
                       qcount,
                       0U);
            }
            prev_master_source = mc.source;
        }

        if (input_eof && !qcount && loop) {
            if (audio) audio_set_running(audio, 0);
            mr_demux_rewind(dx);
            if (mr_decoder_reset(&dec) != MR_OK ||
                !apply_h264_speed(&dec, h264_speed, 0)) break;
            /* mr_decoder_reset() closes and reopens the codec, so a fresh
             * h264_state comes back with timing_enabled at its default
             * (off) - reapply, same as apply_h264_speed just above. */
            mr_h264_set_timing_enabled(&dec, want_time);
            if (use_yuv_indexed_queue || use_yuv_rgb_queue)
                mr_h264_set_yuv_output(&dec, 1);
            if (audio_dec) mr_audio_decoder_reset(audio_dec);
            input_eof = 0; decoded_index = 0; mono_base_us = 0;
            have_container_pts = 0; last_container_pts_us = 0;
            container_pts_adjust_us = 0;
            playback_started = 0;
            if (audio)
                media_clock_rebase(&mc, audio_elapsed_us(audio), 0);
            else
                memset(&mc, 0, sizeof mc);
            continue;
        }

        /* A live network stream lost its source: a dropout exhausted the HLS
         * retry budget or the connection dropped, and mr_demux_next_packet
         * reported end of stream. Rather than ending playback, reopen the URL -
         * which resolves to the current live edge - and resume. Bounded, and a
         * mismatched restream stops cleanly. Gated with --live-resync; the
         * default still ends on EOF. */
        if (input_eof && !qcount && !loop && network_source && live_resync) {
            int tries, backoff = 12;                 /* ~0.5 s, grows to ~4 s   */
            const mr_video_info *nvi;
            /* If an unhealthy TLS drop has disabled HTTPS for this process, no
             * reopen can ever succeed - end cleanly instead of spinning through
             * every retry. (This is the AmiSSL "relaunch to resume" limitation.) */
            if (mr_http_tls_disabled()) {
                display_set_status(disp, "Connection lost - relaunch");
                if (want_time) printf("live-reconnect: HTTPS disabled, ending\n");
                break;
            }
            /* Give up if we keep reopening but never actually play: a stream that
             * reconnects and immediately ends again would otherwise spin on the
             * network. Any presented frame since the last reopen counts as
             * progress and resets the tally. */
            if (frames != frames_at_last_reconnect) {
                reconnects_without_progress = 0;
                frames_at_last_reconnect = frames;
            } else if (++reconnects_without_progress > LIVE_RECONNECT_STALL_LIMIT) {
                break;
            }
            if (audio) { audio_set_running(audio, 0); audio_flush(audio); }
            display_set_status(disp, "Reconnecting...");
            /* Discard any in-flight/cached fetch for the stream we're leaving
             * before it can be handed to (or cached for) the reopened one. */
            hls_fetch_cancel();
            mr_demux_close(dx);
            dx = NULL;
            for (tries = 0; tries < LIVE_RECONNECT_TRIES && !quit; tries++) {
                int d;
                if (player_event(disp) == MR_EV_QUIT) { quit = 1; break; }
                if (want_time)
                    printf("live-reconnect: attempt %d/%d\n",
                           tries + 1, LIVE_RECONNECT_TRIES);
                dx = mr_demux_open_file_ex(media_path,
                        have_http_options ? &http_options : NULL);
                if (dx) break;
                /* Back off between attempts so a sustained outage is not hammered
                 * every half second; stay responsive to ESC throughout. */
                for (d = 0; d < backoff && !quit; d++) {
                    if (player_event(disp) == MR_EV_QUIT) { quit = 1; break; }
                    if (audio) audio_service(audio);
                    Delay(2);
                }
                backoff = backoff < 100 ? backoff * 2 : 100;   /* cap ~4 s */
            }
            if (quit) break;
            if (!dx) break;                     /* gave up: end playback */
            nvi = mr_demux_video(dx);
            if (!nvi || !nvi->valid || nvi->width != vi->width ||
                nvi->height != vi->height)
                break;                          /* different shape: stop cleanly */
            vi = nvi;
            if (mr_decoder_reset(&dec) != MR_OK ||
                !apply_h264_speed(&dec, h264_speed, 0)) break;
            /* mr_decoder_reset() closes and reopens the codec, so a fresh
             * h264_state comes back with timing_enabled at its default
             * (off) - reapply, same as apply_h264_speed just above. */
            mr_h264_set_timing_enabled(&dec, want_time);
            if (use_yuv_indexed_queue || use_yuv_rgb_queue)
                mr_h264_set_yuv_output(&dec, 1);
            if (audio_dec) mr_audio_decoder_reset(audio_dec);
            input_eof = 0; decoded_index = 0; mono_base_us = 0;
            have_container_pts = 0; last_container_pts_us = 0;
            container_pts_adjust_us = 0;
            playback_started = 0;
            qcount = 0; qhead = 0;
            if (audio) media_clock_rebase(&mc, audio_elapsed_us(audio), 0);
            else memset(&mc, 0, sizeof mc);
            stats.timing_rebases++;
            continue;
        }

        /* At most one packet per scheduler iteration. URL sources default to
         * depth one: without an asynchronous reader, a blocking HLS fetch
         * cannot be allowed to delay a frame already queued for presentation.
         * When --net-queue raises the network depth, read-ahead is instead
         * governed by the same lateness margin the disk path uses, so a frame
         * is only decoded ahead while the queue front is comfortably far from
         * its deadline. */
        {
            int refill_audio = audio && audio_ms < AUDIO_REFILL_WARNING_MS;
            /* With the video queue already full the loop would otherwise sleep,
             * which stops the audio FIFO being refilled - after a long stall the
             * cushion then never rebuilds and playback settles into a degraded
             * state. Keep demuxing to top the cushion back up; the audio lands
             * in the cheap PCM FIFO, and with the video queue shallow the video
             * decoded alongside it is presented as it comes (present-as-decoded)
             * rather than piling into a deep buffer.
             *
         * The cushion rides segment-fetch stalls for audio; it deliberately
         * does not drive the decoded-video queue deep, which previously caused
         * visible judder and excessive RGB memory use. */
            int feed_audio_full = audio && qcount >= target_depth &&
                                  audio_ms < cushion_ms;
            int can_decode = !input_eof && !(!rescue_active && rescue_priority) &&
                             (rescue_active || startup_refill ||
                              qcount < target_depth || feed_audio_full);
            if (can_decode && playback_started && qcount) {
                uint64_t margin = stats.video_decode_max_us +
                                  PRESENTATION_GUARD_US + 2000ULL;
                if (!refill_audio && !feed_audio_full &&
                    ((network_source && target_depth <= 1) ||
                     late_us > -(int64_t)margin))
                    can_decode = 0;
            }
            if (can_decode) {
                int ready_before = qcount > 0;
                int64_t due_before = late_us;
                /* refill_started/a/blocked only ever feed demux_us/
                 * refill_block_us/network_us/refill_delayed_ready_us, all
                 * --time-report-only (see report_stats() and the
                 * ready_before/due_before check below) - skip the
                 * monotonic_us() calls around an ordinary demux read
                 * otherwise. */
                uint64_t refill_started = want_time ? monotonic_us() : 0;
                uint64_t a = 0;
                trace_phase(&trace, "demux-read");
                if (want_time) a = monotonic_us();
                /* Hand the queue to the service callback for the duration of the
                 * fetch: this is the one place the single loop blocks long enough
                 * (a segment boundary can stall ~1.7 s) to freeze video. While
                 * released, the demux service hook presents due frames from the
                 * decode queue - via mr_demux_set_service()'s registered callback,
                 * invoked from deep inside mr_ts.c/mr_http.c during their own
                 * blocking reads, not from any direct call here (this function's
                 * own service_audio_for_display() calls all run outside this
                 * window, before released is set or after it is cleared, so they
                 * were provably no-ops - see present_service_frame()'s released
                 * guard). We reclaim the queue the instant the read returns and
                 * read qhead/qcount fresh below. Network sources only - a local
                 * read never stalls, so nothing is released and behaviour there
                 * is unchanged. */
                if (network_source) presenter.released = 1;
                mr_status next = mr_demux_next_packet(dx, &pkt);
                presenter.released = 0;
                if (want_time) {
                    uint64_t blocked = monotonic_us() - a;
                    stats.demux_us += blocked; stats.refill_block_us += blocked;
                    if (network_source) stats.network_us += blocked;
                }
                stats.samples++;
                if (rescue_active) {
                    rescue_episode_packets++; stats.rescue_packets++;
                }
                if (next != MR_OK) input_eof = 1;
                else if (!pkt.is_video) {
                    if (audio && audio_dec) {
                        trace_phase(&trace, "audio-decode");
                        /* stats.audio_decode_us only ever feeds the --time
                         * report; skip both monotonic_us() calls (each a
                         * ReadEClock() + 64-bit divide) around every
                         * decoded audio packet when nobody asked for it. */
                        if (want_time) {
                            uint64_t audio_end;
                            a = monotonic_us();
                            mr_audio_decoder_feed(audio_dec, pkt.data, pkt.len,
                                                  decoded_audio_sink, audio);
                            audio_end = monotonic_us();
                            stats.audio_decode_us += audio_end - a;
                        } else {
                            mr_audio_decoder_feed(audio_dec, pkt.data, pkt.len,
                                                  decoded_audio_sink, audio);
                        }
                        if (rescue_active) {
                            rescue_episode_audio++;
                            stats.rescue_audio_packets++;
                        }
                    }
                } else if (pkt.len) {
                    mr_status decode_status;
                    uint64_t decode_end;
                    int skip_stale_output;
                    trace_phase(&trace, "h264-decode");
                    /* A frame decoded into a full queue is dropped (newest-out),
                     * so decode it reference-only: keep the reference chain
                     * intact without spending time producing RGB we discard.
                     * This is what makes the audio-cushion top-up above cheap.
                     *
                     * A network source's queue is deliberately kept shallow
                     * (target_depth as low as 1 - see its declaration above),
                     * so "queue full" alone rarely fires even when the whole
                     * decode+convert+display pipeline is costing more than one
                     * frame period and audio is being starved of CPU time for
                     * it (observed: 854x480 TurboGT averaging ~36-39ms of
                     * decode+yuv-rgb+display against a 33.3ms/30fps budget,
                     * with audio-rescue firing almost continuously as a
                     * result). Falling behind wall-clock by more than one
                     * frame period on this packet's own PTS is a second,
                     * independent signal of the same problem that doesn't
                     * need the queue to be full first. mono_media_clock_us is
                     * wall-clock elapsed since the mono_base_us anchor
                     * established when playback_started first went true (see
                     * below) - cheap, always up to date this iteration, and
                     * safe to read again here, unlike current_media_clock_us()
                     * (already called above when have_deadline was set: a
                     * second call this iteration would spuriously re-enter/
                     * exit clock-holdover). Gated on playback_started because
                     * mono_base_us is meaningless before that anchor exists.
                     * Must stay exactly the condition the queue-full check
                     * below uses too - skip_output with no matching drop
                     * would let a stale, un-converted RGB buffer (emit_rgb()
                     * never ran) get copied into the queue as if it were this
                     * frame's real picture. */
                    skip_stale_output = qcount >= video_cap ||
                        (playback_started && pkt.has_pts &&
                         (int64_t)mono_media_clock_us - (int64_t)pkt.pts_us >
                             (int64_t)period_us);
                    mr_h264_set_skip_output(&dec, skip_stale_output);
                    mr_h264_set_input_pts(&dec, pkt.has_pts, pkt.pts_us);
                    mr_h264_set_input_annexb(&dec, pkt.is_annexb);
                    a = monotonic_us();
                    decode_status = mr_decoder_decode(&dec, pkt.data, pkt.len);
                    decode_end = monotonic_us();
                    /* mr_h264_frame_timing()'s fields only ever feed the
                     * --time stats report below; when timing isn't enabled
                     * h264_decode() never populates them (they read back as
                     * zero), so fetching and accumulating them every frame
                     * is a wasted struct copy plus a dozen adds/compares for
                     * numbers nobody asked for. */
                    if (want_time) {
                        mr_h264_timing ht;
                        mr_h264_frame_timing(&dec, &ht);
                        stats.h264_input_us += ht.input_us;
                        stats.h264_core_us += ht.core_us;
                        stats.h264_output_us += ht.output_us;
                        if (ht.input_us > stats.h264_input_max_us)
                            stats.h264_input_max_us = ht.input_us;
                        if (ht.core_us > stats.h264_core_max_us)
                            stats.h264_core_max_us = ht.core_us;
                        if (ht.output_us > stats.h264_output_max_us)
                            stats.h264_output_max_us = ht.output_us;
                        stats.h264_mc_us += ht.mc_us;
                        stats.h264_deblock_us += ht.deblock_us;
                        stats.h264_recon_us += ht.recon_us;
                        stats.h264_intra_us += ht.intra_us;
                        if (ht.mc_us > stats.h264_mc_max_us)
                            stats.h264_mc_max_us = ht.mc_us;
                        if (ht.deblock_us > stats.h264_deblock_max_us)
                            stats.h264_deblock_max_us = ht.deblock_us;
                        if (ht.recon_us > stats.h264_recon_max_us)
                            stats.h264_recon_max_us = ht.recon_us;
                        if (ht.intra_us > stats.h264_intra_max_us)
                            stats.h264_intra_max_us = ht.intra_us;
                    }
                    if (decode_status == MR_ENOMEM) {
                        printf("h264-decode-oom: packet %lu len=%lu - "
                               "stopping playback\n",
                               (unsigned long)decoded_index,
                               (unsigned long)pkt.len);
                        display_set_status(disp, "H.264 decoder out of memory");
                        /* libavc marks IVD_MEM_ALLOC_FAILED fatal. Do not keep
                         * demuxing into a dead decoder: take mrplay's existing
                         * quit/cleanup path so RTG, Paula and decoder buffers are
                         * released immediately. */
                        quit = 1;
                        continue;
                    }
                    if (decode_status == MR_SKIPPED) {
                        /*
                         * Turbo deliberately omitted this source picture.
                         * No display frame is queued, but its media-time slot
                         * still existed. Advance the fallback timeline so the
                         * next surviving picture cannot inherit a compressed
                         * synthetic PTS and race ahead of audio.
                         *
                         * MR_EAGAIN is different: it is ordinary libavc
                         * reordering/no-output and consumes no extra fallback
                         * display interval here.
                         */
                        decoded_index++;
                        stats.dropped++;
                    }
                    if (decode_status == MR_EFORMAT) {
                        printf("h264-decode-error: packet %lu len=%lu\n",
                               (unsigned long)decoded_index,
                               (unsigned long)pkt.len);
                    }
                    if (decode_status == MR_OK) {
                        if (h264_pipeline_diag_enabled && h264_pipeline_stage < 1) {
                            h264_pipeline_checkpoint_player("decoder-return",
                                                            qcount, playback_started);
                            h264_pipeline_stage = 1;
                        }
                        unsigned long decode_us =
                            (unsigned long)(decode_end - a);
                        uint64_t synthetic_pts = vi->rate
                            ? decoded_index *
                              (uint64_t)(vi->scale ? vi->scale : 1) *
                              1000000ULL / vi->rate
                            : decoded_index * 83333ULL;
                        uint64_t frame_pts_us = pkt.pts_us;
                        int frame_has_pts = pkt.has_pts;
                        uint64_t pts = synthetic_pts;
                        /* Libavc emits pictures in display order, which may
                         * differ from the current packet's decode order when
                         * B-frames are present.  Use the PTS mapped from the
                         * decoder's returned AU timestamp.  If that mapping is
                         * unavailable, synthetic output-order timing is safer
                         * than attaching the current packet's wrong PTS. */
                        if (codec == &mr_codec_h264)
                            frame_has_pts = mr_h264_output_pts(
                                &dec, &frame_pts_us);
                        if (frame_has_pts) {
                            int discontinuity = have_container_pts &&
                                (frame_pts_us + period_us * 10 < last_container_pts_us ||
                                 frame_pts_us > last_container_pts_us + period_us * 10);
                            if (!have_container_pts || discontinuity) {
                                container_pts_adjust_us =
                                    (int64_t)synthetic_pts - (int64_t)frame_pts_us;
                                if (have_container_pts) stats.timing_rebases++;
                            }
                            pts = (uint64_t)((int64_t)frame_pts_us +
                                             container_pts_adjust_us);
                            last_container_pts_us = frame_pts_us;
                            have_container_pts = 1;
                        }
                        stats.video_decode_us += decode_us; stats.decoded++;
                        total_decode_us += decode_us;
                        if (decode_us > stats.video_decode_max_us)
                            stats.video_decode_max_us = decode_us;
                        {
                            int retain = !startup_refill ||
                                         qcount < video_cap ||
                                         rescue_active;
                            if (rescue_active) {
                                rescue_episode_video++;
                                stats.rescue_video_decoded++;
                            }
                            if (!retain) {
                                if (rescue_active) {
                                    rescue_episode_skipped++;
                                    stats.rescue_video_skipped++;
                                }
                                stats.dropped++;
                                decoded_index++;
                                continue;
                            }
                            if (skip_stale_output) {
                                /* Queue full, or this frame already fell more
                                 * than a period behind wall-clock before it
                                 * was even decoded (see skip_stale_output's
                                 * declaration above) - either way emit_rgb()
                                 * never ran for it, so dec.frame still holds
                                 * whatever a previous frame last wrote there.
                                 * Must match mr_h264_set_skip_output()'s
                                 * condition exactly: copying that stale buffer
                                 * into the queue under this frame's own pts
                                 * would show a duplicated/frozen picture. When
                                 * it's specifically the full-queue case, every
                                 * queued frame is also closer to its deadline
                                 * than this freshly decoded one, which sits
                                 * furthest in the future, so keep them and
                                 * drop the newcomer - evicting the oldest
                                 * instead (the previous behaviour) left the
                                 * queue front permanently ahead of the audio
                                 * clock, so presentation stalled and never
                                 * recovered after a long stall. The packet was
                                 * still consumed, so audio rescue makes
                                 * progress regardless. */
                                if (rescue_active) {
                                    rescue_episode_skipped++;
                                    stats.rescue_video_skipped++;
                                }
                                stats.dropped++;
                                decoded_index++;
                                continue;
                            }
                            queued_video *tail =
                                &vq[(qhead + qcount) % video_cap];
                            trace_phase(&trace, "frame-copy");
                            /* decoded_at_us only ever feeds the --time
                             * latency stat (both readers are want_time-
                             * gated) - skip the monotonic_us() call
                             * otherwise. The three specialised queue modes
                             * are mutually exclusive (see their declarations
                             * above). */
                            uint64_t decoded_at = want_time ? monotonic_us() : 0;
                            int copy_ok;
                            if (use_yuv_indexed_queue) {
                                /* Only the fused YUV420->chunky path gets its
                                 * own timing bucket: it is real per-frame work
                                 * (mr_yuv420_dither8(), or mr_yuv420_ham_
                                 * encode() on a HAM screen), not folded into
                                 * any existing stat, and is the next ASM
                                 * candidate - this is how its cost gets
                                 * measured. */
                                uint64_t yt0 = want_time ? monotonic_us() : 0;
                                copy_ok = queue_copy_yuv_indexed(
                                    tail, &dec.frame, pts, decoded_at,
                                    yuv_dst_w, yuv_dst_h, yuv_vscale,
                                    indexed_depth, yuv_ham);
                                if (want_time) {
                                    unsigned long us =
                                        (unsigned long)(monotonic_us() - yt0);
                                    stats.yuv_indexed_us += us;
                                    stats.yuv_indexed_frames++;
                                    if (us > stats.yuv_indexed_max_us)
                                        stats.yuv_indexed_max_us = us;
                                }
                            } else if (use_indexed_queue)
                                copy_ok = queue_copy_indexed(tail, &dec.frame,
                                                             pts, decoded_at,
                                                             indexed_depth);
                            else if (use_yuv_rgb_queue) {
                                uint64_t yr0 = want_time ? monotonic_us() : 0;
                                copy_ok = queue_copy_yuv_rgb24(
                                    tail, &dec.frame, pts, decoded_at,
                                    use_yuv_bgr_queue,
                                    audio ? service_audio_for_display : NULL,
                                    &trace);
                                if (want_time) {
                                    unsigned long us =
                                        (unsigned long)(monotonic_us() - yr0);
                                    stats.yuv_rgb_us += us;
                                    stats.yuv_rgb_frames++;
                                    if (us > stats.yuv_rgb_max_us)
                                        stats.yuv_rgb_max_us = us;
                                }
                            }
                            else
                                copy_ok = queue_copy(tail, &dec.frame, pts,
                                                     decoded_at);
                            if (!copy_ok) {
                                /* Out of RAM for another RGB slot (a backstop -
                                 * the queue is sized to fit; see main). Never
                                 * quit: drop this frame exactly as a full queue
                                 * would and keep recycling the slots we own. The
                                 * cap is the ring modulus so it must not change;
                                 * the slot simply stays empty and is retried.
                                 * Log once so hardware tests can confirm whether
                                 * a grey window is caused by this OOM path. */
                                if (!oom_warned) {
                                    printf("warning: queue_copy OOM - "
                                           "frame %ux%u dropped; "
                                           "further OOM drops will be silent\n",
                                           dec.frame.width, dec.frame.height);
                                    oom_warned = 1;
                                }
                                stats.dropped++;
                                decoded_index++;
                                continue;
                            }
                            qcount++;
                            if (h264_pipeline_diag_enabled && h264_pipeline_stage < 2) {
                                h264_pipeline_checkpoint_player("queue-copy",
                                                                qcount, playback_started);
                                h264_pipeline_stage = 2;
                            }
                            if (rescue_active) {
                                rescue_episode_queued++;
                                stats.rescue_video_queued++;
                                rescue_newest_retained_pts_us = pts;
                            }
                        }
                        decoded_index++;
                    }
                }
                if (want_time && ready_before && due_before < 0) {
                    uint64_t refill_elapsed = monotonic_us() - refill_started;
                    if (refill_elapsed > (uint64_t)(-due_before))
                        stats.refill_delayed_ready_us +=
                            refill_elapsed - (uint64_t)(-due_before);
                }
                if (!playback_started &&
                    (qcount >= startup_depth || input_eof) &&
                    (!audio || audio_buffered_ms(audio) >=
                               AUDIO_STARTUP_TARGET_MS || input_eof)) {
                    playback_started = qcount > 0;
                    if (playback_started) {
                        now = monotonic_us();
                        mono_base_us = now - vq[qhead].pts_us;
                        /* Rebase the audio-derived media clock to the same
                         * front-of-queue pts mono_base_us just used, not a
                         * hardcoded 0. This "playback just (re)started"
                         * priming runs after a real session start, a loop
                         * restart, live-reconnect, AND a seek (which reuses
                         * playback_started = 0 on purpose to let this same
                         * logic re-derive timing) - the first three really
                         * do begin at media time 0, where vq[qhead].pts_us
                         * is already ~0 too, so this is a no-op for them.
                         * A seek does not: rebasing to 0 here silently
                         * overwrote the seek's own correct
                         * media_clock_rebase(..., out_us) the moment the
                         * queue refilled past startup_depth, leaving the
                         * audio clock thinking playback was at ~0 while
                         * mono_base_us (and every video frame's real pts)
                         * correctly read the seeked-to position - a
                         * permanent split between the two clocks that Audio,
                         * as the master clock, never recovers from: the
                         * video looked frozen (perpetually "not due yet")
                         * while audio kept playing normally. */
                        if (audio)
                            media_clock_rebase(&mc, audio_elapsed_us(audio),
                                               vq[qhead].pts_us);
                        if (audio) audio_set_running(audio, 1);
                        display_set_status(disp, NULL);  /* clear Buffering... */
                        if (h264_pipeline_diag_enabled && h264_pipeline_stage < 3) {
                            h264_pipeline_checkpoint_player("playback-start",
                                                            qcount, playback_started);
                            h264_pipeline_stage = 3;
                        }
                    }
                }
                continue;
            }
        }

        if (audio && audio_buffered_ms(audio) < AUDIO_REFILL_WARNING_MS &&
            !input_eof && qcount < target_depth) {
            /* Do not burn a 20 ms DOS tick while audio is in refill mode. */
            continue;
        } else if (qcount && playback_started) {
            uint64_t wait_us = late_us < -(int64_t)PRESENTATION_GUARD_US
                ? (uint64_t)(-late_us) - PRESENTATION_GUARD_US : 0;
            if (wait_us > 20000) wait_us = 20000;
            paced_sleep(wait_us, &trace, &stats);
        } else {
            int ev = player_event(disp);
            if (ev == MR_EV_QUIT) quit = 1;
            {
                uint64_t delay_begin = monotonic_us();
                Delay(1);
                trace.delay_ticks = 1;
                trace.sleep_actual_us = monotonic_us() - delay_begin;
            }
        }
    }
    }
    /* `presenter` lived in the scheduler block just closed; the display service
     * hook is still installed and fires during the teardown flush's blits, so
     * drop the now-dangling pointer before any of that can dereference it. */
    trace.presenter = NULL;
    /* The HLS wait and HTTP service hooks hold &trace too; the teardown flush no
     * longer reads the demux, so retire them here before the scheduler locals
     * die. */
    mr_hls_set_wait(NULL, NULL);
    mr_http_set_service(NULL, NULL);
    hls_fetch_set_service(NULL, NULL);

    /* This drain loop calls display_show_rgb() directly on dec.frame.data,
     * bypassing the queue (and so bypassing queue_copy_yuv_indexed() and
     * queue_copy_yuv_rgb24()) - h264_flush() runs frames through the same
     * emit_rgb() the normal decode path does, so with yuv_output still
     * enabled dec.frame would be MR_PIX_YUV420P and dec.frame.data would
     * be the Y plane alone, which display_show_rgb() would misread as
     * RGB24. Only ever drains the last frame or two at EOF, so falling
     * back to the ordinary (always-correct) RGB24 conversion here costs
     * nothing worth avoiding. */
    if (use_yuv_indexed_queue || use_yuv_rgb_queue)
        mr_h264_set_yuv_output(&dec, 0);

    /* MPEG-4 B-frame/display reordering holds the final anchor until EOF.
     * Drain it through the same pacing and display path so the player does not
     * silently finish one frame short (e.g. 129/130 on legacy OpenDivX). */
    while (!quit) {
        uint64_t a = monotonic_us();
        mr_status ds = mr_decoder_flush(&dec);
        total_decode_us += monotonic_us() - a;
        if (ds != MR_OK) break;

        if (audio) {
            /* Target audio raw time for frame N: invert the signed offset.
             * audio_raw = media_target - audio_to_media_offset_us */
            int64_t flush_media_us = (int64_t)frames * (int64_t)(period * 1000UL);
            int64_t flush_audio_us = flush_media_us - mc.audio_to_media_offset_us;
            unsigned long target = (unsigned long)(s64_to_us(flush_audio_us) / 1000ULL);
            while (audio_elapsed_ms(audio) < target &&
                   !audio_starved(audio)) {
                int ev = player_event(disp);
                if (ev == MR_EV_QUIT) { quit = 1; break; }
                audio_service(audio);
                Delay(1);
            }
        } else {
            int ev = player_event(disp);
            if (ev == MR_EV_QUIT) quit = 1;
            Delay(ticks);
        }
        if (quit) break;

        /* total_display_us only ever feeds the --time "timing/N frames:"
         * summary below - skip both monotonic_us() calls otherwise. */
        if (want_time) {
            a = monotonic_us();
            display_show_rgb(disp, dec.frame.data, dec.frame.width,
                             dec.frame.height, dec.frame.stride,
                             dec.frame.dirty_y0, dec.frame.dirty_y1);
            player_first_frame_presented(disp, -1);
            total_display_us += monotonic_us() - a;
        } else {
            display_show_rgb(disp, dec.frame.data, dec.frame.width,
                             dec.frame.height, dec.frame.stride,
                             dec.frame.dirty_y0, dec.frame.dirty_y1);
            player_first_frame_presented(disp, -1);
        }
        frames++;
    }
    }
    if (want_time && frames > 0) {
        unsigned long enc_ms = 0, blit_ms = 0;
        display_aga_timing(&enc_ms, &blit_ms);
        printf("timing/%d frames: decode=%lu ms, display=%lu ms"
               " (encode=%lu ms, blit=%lu ms)\n", frames,
               (unsigned long)(total_decode_us  / 1000),
               (unsigned long)(total_display_us / 1000),
               enc_ms, blit_ms);
        if (display_aga_kalms_timing(&blit_ms))
            printf("Kalms conversion: %lu ms\n", blit_ms);
    }
    /* Let any queued audio drain (bounded, so a wedged clock can't loop). */
    if (audio && !quit) {
        int guard = 0;
        while (!audio_starved(audio) && guard++ < 4000) {
            if (player_event(disp) == MR_EV_QUIT) {
                quit = 1;
                break;
            }
            audio_service(audio);
            Delay(1);
        }
    }
    if (audio) audio_set_running(audio, 0);

    if (!quit && !auto_close_eof) {
        printf("played %d frames - press ESC or close the window to exit\n",
               frames);
        while (player_event(disp) != MR_EV_QUIT) {
            if (audio) audio_service(audio);
            Delay(2);
        }
    }

    player_status(MR_PLAYER_STATE_ENDED, codec->name, "stream ended");
    { int qi; for (qi = 0; qi < VIDEO_QUEUE_CAP; qi++) free(vq[qi].rgb); }
    if (audio_dec) mr_audio_decoder_close(audio_dec);
    control_audio = NULL;
    if (audio) audio_close(audio);
    display_close(disp);
    mr_decoder_close(&dec);
    mr_demux_close(dx);
    free(buf);
    return mrplay_exit(0);
}
