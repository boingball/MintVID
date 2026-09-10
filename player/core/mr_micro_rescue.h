/*
 * Micro-rescue's entry/exit state machine, pulled out of amiga/mrplay.c as
 * pure, host-testable logic (no Amiga dependency at all - just integer
 * timestamp arithmetic), so it can be driven deterministically from
 * tests/mr_micro_rescue_check.c instead of only from inside mrplay.c's
 * full scheduler loop.
 *
 * Background (see amiga/mrplay.c's own MICRO_RESCUE_* comment for the full
 * story): catastrophic live-resync only fires once a stall is already
 * severe (multi-second), and its catch-up burst pays only H.264 core-decode
 * cost. The instant it exits, every subsequent frame resumes paying core
 * decode + RGB/YUV conversion + display + AAC decode all at once, which can
 * produce a "temporarily smooth, then gradually falls behind again" cycle
 * on hardware where that full stack only marginally fits the frame budget.
 * Micro-rescue is a milder, earlier tier: once a decoded packet's own PTS
 * is found more than entry_us behind the mono media clock, decode
 * reference-only (skip RGB/YUV conversion and display) for as long as it
 * takes, while leaving audio decode and playback completely untouched.
 * This is a sustained absolute-lateness condition checked packet-by-packet,
 * not a rate-of-change/derivative trend detector.
 *
 * The state machine is split into two independent entry points on purpose:
 *
 *   mr_micro_rescue_on_packet() - call once per decoded video packet that
 *   carries a PTS, with pkt_late_us = (current mono media clock) - (this
 *   packet's own PTS). Decides both entry and the lateness-recovery exit.
 *
 *   mr_micro_rescue_tick() - call once per scheduler iteration,
 *   unconditionally, regardless of whether a packet was decoded or the
 *   presentation queue is empty. This is the last-resort safety timeout.
 *
 * This split exists because of a real deadlock in the first version of
 * this mechanism: both entry and exit were decided only inside the
 * presentation/deadline-drop code, which stops running entirely once the
 * decoded-video queue is empty (qcount == 0, front == NULL). But once
 * micro-rescue itself starts skipping RGB/YUV output, decoded frames stop
 * being queued at all - qcount drops to (and stays at) 0, so the
 * presentation code that would have cleared the flag never runs again,
 * and micro-rescue gets stuck active forever with a frozen last picture.
 * mr_micro_rescue_on_packet() runs from the per-packet decode path, which
 * keeps running regardless of queue depth, so recovery no longer depends
 * on anything being queued; mr_micro_rescue_tick() is an unconditional
 * backstop in case no further PTS-bearing packet ever arrives to trigger
 * that recovery check.
 */
#ifndef MR_MICRO_RESCUE_H
#define MR_MICRO_RESCUE_H

#include <stdint.h>

typedef struct mr_micro_rescue_state {
    int active;
    uint64_t entered_at_us;
} mr_micro_rescue_state;

typedef struct mr_micro_rescue_result {
    int active;           /* state after this call                        */
    int entered;          /* true iff this call transitioned to active    */
    int exited_recovered; /* true iff this call exited via lateness recovery */
    int exited_timeout;   /* true iff this call exited via the safety timeout */
    uint64_t episode_us;  /* episode duration; valid only when entered==0 and
                            * (exited_recovered || exited_timeout)         */
} mr_micro_rescue_result;

static inline void mr_micro_rescue_init(mr_micro_rescue_state *st)
{
    st->active = 0;
    st->entered_at_us = 0;
}

/*
 * Call once per decoded video packet that carries a usable PTS.
 * pkt_late_us is (int64_t)mono_media_clock_us - (int64_t)pkt_pts_us - the
 * same signal amiga/mrplay.c's skip_stale_output already computes for its
 * own per-frame staleness check. now_us must be a monotonic microsecond
 * timestamp (the same clock mr_micro_rescue_tick() is driven from).
 */
static inline mr_micro_rescue_result mr_micro_rescue_on_packet(
    mr_micro_rescue_state *st, int64_t pkt_late_us, uint64_t now_us,
    int64_t entry_us, int64_t exit_us)
{
    mr_micro_rescue_result r;
    r.entered = 0;
    r.exited_recovered = 0;
    r.exited_timeout = 0;
    r.episode_us = 0;
    if (!st->active && pkt_late_us > entry_us) {
        st->active = 1;
        st->entered_at_us = now_us;
        r.entered = 1;
    } else if (st->active && pkt_late_us < exit_us) {
        r.episode_us = now_us - st->entered_at_us;
        st->active = 0;
        r.exited_recovered = 1;
    }
    r.active = st->active;
    return r;
}

/*
 * Call once per scheduler iteration, unconditionally - with no dependency
 * on queue depth, presentation state, or whether a packet was decoded this
 * pass. Bounds how long the state can stay active if
 * mr_micro_rescue_on_packet() never sees another PTS-bearing packet.
 */
static inline mr_micro_rescue_result mr_micro_rescue_tick(
    mr_micro_rescue_state *st, uint64_t now_us, uint64_t max_us)
{
    mr_micro_rescue_result r;
    r.entered = 0;
    r.exited_recovered = 0;
    r.exited_timeout = 0;
    r.episode_us = 0;
    if (st->active && now_us - st->entered_at_us > max_us) {
        r.episode_us = now_us - st->entered_at_us;
        st->active = 0;
        r.exited_timeout = 1;
    }
    r.active = st->active;
    return r;
}

#endif /* MR_MICRO_RESCUE_H */
