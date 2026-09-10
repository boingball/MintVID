/*
 * Deterministic, host-buildable regression coverage for
 * core/mr_micro_rescue.h - the micro-rescue entry/exit state machine
 * extracted from amiga/mrplay.c specifically so it can be driven and
 * verified without any Amiga dependency.
 *
 * Part 1 exercises mr_micro_rescue_on_packet()/mr_micro_rescue_tick()
 * directly: entry/hysteresis/recovery-exit/timeout-exit in isolation.
 *
 * Part 2 is a small, deterministic simulation of the exact interaction
 * between micro-rescue and mrplay.c's scheduler that produced a real
 * deadlock in the first version of this mechanism: both entry and exit
 * were decided only inside the presentation-gated code, which stops
 * running once the decoded-video queue (qcount) reaches 0 - and
 * micro-rescue's own skip_stale_output composition is exactly what drives
 * qcount to (and keeps it at) 0 once it activates, since every decoded
 * frame is dropped instead of queued. The simulation reproduces that
 * qcount/skip_stale_output interaction faithfully (mirroring mrplay.c's
 * own composition: `qcount >= cap || frame-own-staleness ||
 * micro_rescue.active`) while driving mr_micro_rescue_on_packet()/_tick()
 * exactly as mrplay.c's per-packet decode section and top-of-loop safety
 * check do, and walks the five-step scenario requested in review:
 *
 *   1. qcount == 1, lateness already past the entry threshold
 *   2. micro-rescue activates on that packet
 *   3. the one already-queued frame is presented (qcount 1 -> 0)
 *   4. several subsequent H.264 outputs are skipped (qcount stays 0)
 *   5. rescue exits (recovered) and normal queueing resumes - not stuck
 *      permanently in skip_output mode
 *
 * plus the pure-timeout backstop path (lateness never recovers at all),
 * to prove the mechanism cannot deadlock even in the worst case.
 */
#include <stdio.h>
#include <string.h>
#include "../core/mr_micro_rescue.h"

#define ENTRY_US   700000ULL
#define EXIT_US    200000ULL
#define MAX_US    2500000ULL
#define PERIOD_US   40000ULL /* 25 fps, matching a representative low-res clip */

static int fails = 0;

#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s\n", msg); fails++; } \
} while (0)

/* ---- Part 1: direct state-machine unit tests --------------------- */

static void test_no_entry_below_threshold(void)
{
    mr_micro_rescue_state st;
    mr_micro_rescue_init(&st);
    mr_micro_rescue_result r = mr_micro_rescue_on_packet(
        &st, (int64_t)ENTRY_US - 1, 1000000ULL, (int64_t)ENTRY_US,
        (int64_t)EXIT_US);
    CHECK(!r.entered && !st.active,
          "packet lateness just under entry threshold must not activate");
}

static void test_entry_above_threshold(void)
{
    mr_micro_rescue_state st;
    mr_micro_rescue_init(&st);
    mr_micro_rescue_result r = mr_micro_rescue_on_packet(
        &st, (int64_t)ENTRY_US + 1, 1000000ULL, (int64_t)ENTRY_US,
        (int64_t)EXIT_US);
    CHECK(r.entered && r.active && st.active,
          "packet lateness just over entry threshold must activate");
    CHECK(!r.exited_recovered && !r.exited_timeout,
          "the entering call must not also report an exit");
}

static void test_hysteresis_band_stays_active(void)
{
    /* Between EXIT_US and ENTRY_US, once active, must stay active - this
     * is the "several H.264 outputs are skipped" persistence the review
     * asked to cover: a single high-lateness call must not immediately
     * flip back off just because lateness isn't still climbing. */
    mr_micro_rescue_state st;
    mr_micro_rescue_init(&st);
    mr_micro_rescue_on_packet(&st, (int64_t)ENTRY_US + 1, 1000000ULL,
                              (int64_t)ENTRY_US, (int64_t)EXIT_US);
    CHECK(st.active, "setup: must be active after entry");
    int64_t mid = (int64_t)(ENTRY_US + EXIT_US) / 2;
    for (int i = 0; i < 10; i++) {
        mr_micro_rescue_result r = mr_micro_rescue_on_packet(
            &st, mid, 1000000ULL + (uint64_t)i * PERIOD_US,
            (int64_t)ENTRY_US, (int64_t)EXIT_US);
        CHECK(st.active && !r.exited_recovered && !r.exited_timeout,
              "lateness inside the hysteresis band must not exit early");
    }
}

static void test_recovery_exit(void)
{
    mr_micro_rescue_state st;
    mr_micro_rescue_init(&st);
    mr_micro_rescue_on_packet(&st, (int64_t)ENTRY_US + 1, 1000000ULL,
                              (int64_t)ENTRY_US, (int64_t)EXIT_US);
    mr_micro_rescue_result r = mr_micro_rescue_on_packet(
        &st, (int64_t)EXIT_US - 1, 1300000ULL, (int64_t)ENTRY_US,
        (int64_t)EXIT_US);
    CHECK(r.exited_recovered && !r.exited_timeout && !r.active && !st.active,
          "lateness dropping under the exit threshold must recover-exit");
    CHECK(r.episode_us == 300000ULL, "episode duration must be exact");
}

static void test_timeout_exit_without_any_packet(void)
{
    /* This is the deadlock the review flagged, driven to its worst case:
     * no further PTS-bearing packet ever arrives (on_packet() is never
     * called again after entry) - only the unconditional tick(). Must
     * still exit, deterministically, at exactly max_us. */
    mr_micro_rescue_state st;
    mr_micro_rescue_init(&st);
    mr_micro_rescue_on_packet(&st, (int64_t)ENTRY_US + 1, 0,
                              (int64_t)ENTRY_US, (int64_t)EXIT_US);
    CHECK(st.active, "setup: must be active after entry");
    mr_micro_rescue_result r = mr_micro_rescue_tick(&st, MAX_US, MAX_US);
    CHECK(!r.exited_timeout && st.active,
          "must not exit at exactly max_us (strictly greater required)");
    r = mr_micro_rescue_tick(&st, MAX_US + 1, MAX_US);
    CHECK(r.exited_timeout && !r.active && !st.active,
          "must exit via timeout the instant elapsed exceeds max_us, "
          "with no packets involved at all");
    CHECK(r.episode_us == MAX_US + 1, "timeout episode duration must be exact");
}

/* ---- Part 2: deterministic mini-scheduler simulation -------------- */

#define QUEUE_CAP 4

typedef struct sim {
    mr_micro_rescue_state micro_rescue;
    int qcount;
    uint64_t mono_clock_us;   /* wall-clock time elapsed                  */
    uint64_t decoded_pts_us;  /* position the "decoder" has produced up to */
    unsigned entries, exits_recovered, exits_timeout;
    unsigned frames_queued, frames_skipped;
    int last_skip_stale_output; /* drives next pass's decode speed - see
                                  * sim_pass()'s comment                   */
} sim;

static void sim_init(sim *s)
{
    memset(s, 0, sizeof *s);
    mr_micro_rescue_init(&s->micro_rescue);
}

/*
 * One scheduler pass: decode exactly one packet (mirroring mrplay.c's "at
 * most one packet per scheduler iteration"), run it through the identical
 * skip_stale_output composition mrplay.c uses, then run both micro-rescue
 * entry points exactly where mrplay.c calls them - mr_micro_rescue_tick()
 * unconditionally at the top, mr_micro_rescue_on_packet() from the
 * per-packet decode path.
 *
 * decode_advance_us models how far the "decoder" moves per pass: PERIOD_US
 * normally (real-time paced output), more than that whenever the
 * *previous* pass's skip_stale_output was true for any reason (not only
 * micro-rescue specifically - in real mrplay.c, RGB/YUV conversion and
 * display are skipped whenever skip_stale_output is true regardless of
 * which of its three clauses fired, freeing the same CPU either way).
 * Using the previous pass's decision (rather than this pass's, which
 * would be circular - the decision depends on lateness, which depends on
 * the advance) models the real pipelining: this frame's own cheap-or-not
 * decode is what leaves more or less CPU for the next one. This is what
 * lets the simulation reach a genuine, deterministic recovery rather than
 * asserting it by fiat - including the compounding case where micro-
 * rescue's own recovery-exit threshold (EXIT_US) is well above one frame
 * period, so a frame can still trip the plain per-frame-staleness clause
 * for a little longer even after micro-rescue itself has exited.
 */
static void sim_pass(sim *s)
{
    uint64_t now_us = s->mono_clock_us;

    /* Unconditional safety timeout - runs every pass, matching mrplay.c's
     * top-of-loop placement, regardless of qcount. */
    mr_micro_rescue_result tick_r =
        mr_micro_rescue_tick(&s->micro_rescue, now_us, MAX_US);
    if (tick_r.exited_timeout) s->exits_timeout++;

    /* "Decode" this pass's packet. */
    uint64_t decode_advance = s->last_skip_stale_output ? PERIOD_US * 3
                                                        : PERIOD_US;
    s->decoded_pts_us += decode_advance;
    int64_t pkt_late_us = (int64_t)s->mono_clock_us -
                          (int64_t)s->decoded_pts_us;

    mr_micro_rescue_result pkt_r = mr_micro_rescue_on_packet(
        &s->micro_rescue, pkt_late_us, now_us, (int64_t)ENTRY_US,
        (int64_t)EXIT_US);
    if (pkt_r.entered) s->entries++;
    if (pkt_r.exited_recovered) s->exits_recovered++;

    /* Exact skip_stale_output composition from amiga/mrplay.c (the
     * per-frame-staleness term uses period_us, same as there). */
    int skip_stale_output = s->qcount >= QUEUE_CAP ||
        pkt_late_us > (int64_t)PERIOD_US || s->micro_rescue.active;
    s->last_skip_stale_output = skip_stale_output;

    if (skip_stale_output) {
        s->frames_skipped++;
        /* Not queued - matches mrplay.c's goto drain_decoded_output. */
    } else {
        s->qcount++;
        s->frames_queued++;
    }

    /* Presentation: one frame presented per pass if the queue is
     * non-empty (mirrors mrplay.c's front-of-queue presentation once per
     * scheduler pass while due). */
    if (s->qcount > 0) s->qcount--;

    s->mono_clock_us += PERIOD_US;
}

static void test_scheduler_deadlock_regression(void)
{
    sim s;
    sim_init(&s);

    /* Step 1: seed qcount == 1 and lateness already past the entry
     * threshold, exactly as review requested - a live-resync-adjacent
     * backlog where one frame is already queued but the decoder is
     * running well behind wall-clock. */
    s.qcount = 1;
    s.mono_clock_us = ENTRY_US + PERIOD_US * 2;
    s.decoded_pts_us = 0; /* first packet decodes to pts=PERIOD_US below */

    /* First pass: step 2 (activates) and step 3 (the pre-seeded queued
     * frame gets presented, qcount 1 -> 0) happen together. */
    sim_pass(&s);
    CHECK(s.micro_rescue.active, "step 2: must activate on first late packet");
    CHECK(s.entries == 1, "step 2: exactly one entry recorded");
    CHECK(s.qcount == 0,
          "step 3: the pre-seeded queued frame must have been presented");

    /* Step 4: several subsequent decodes must be skipped (reference-only)
     * while remaining active, with qcount staying at 0 throughout - this
     * is precisely the state that starved the old have_deadline-gated
     * exit logic forever. Run enough passes to be well past what the old
     * code's presentation-block-only exit would have needed, while still
     * comfortably inside MAX_US so recovery (not timeout) is what's being
     * exercised here. */
    int skipped_while_active = 0;
    for (int i = 0; i < 8; i++) {
        unsigned before_skipped = s.frames_skipped;
        sim_pass(&s);
        if (s.frames_skipped != before_skipped) skipped_while_active++;
        CHECK(s.qcount == 0,
              "step 4: qcount must stay at 0 while micro-rescue sheds output");
    }
    CHECK(skipped_while_active > 0,
          "step 4: several H.264 outputs must actually have been skipped");

    /* Step 5: because decode_advance_us runs ahead of real time while
     * active, lateness must eventually recover and exit - proving the
     * mechanism does NOT remain permanently stuck in skip_output mode
     * (the actual deadlock this regression targets). Bound the loop so a
     * regression that reintroduces the deadlock fails the test instead of
     * hanging it. */
    int recovered = 0;
    for (int i = 0; i < 200 && !recovered; i++) {
        sim_pass(&s);
        if (!s.micro_rescue.active && s.exits_recovered > 0) recovered = 1;
    }
    CHECK(recovered,
          "step 5: micro-rescue must exit via recovery within a bounded "
          "number of passes, not remain stuck");
    CHECK(s.exits_timeout == 0,
          "step 5: this scenario must recover on its own, not need the "
          "timeout backstop");

    /* And normal queueing must actually have resumed afterward. */
    unsigned queued_before = s.frames_queued;
    for (int i = 0; i < 5; i++) sim_pass(&s);
    CHECK(s.frames_queued > queued_before,
          "step 5: normal output/queueing must resume after recovery");
}

static void test_scheduler_never_deadlocks_even_without_recovery(void)
{
    /* Worst case: the decoder never gets ahead of real time at all (model
     * it as exactly real-time-paced throughout, no catch-up headroom
     * modeled) - lateness never shrinks, so recovery-by-lateness can
     * never fire and mr_micro_rescue_on_packet() will keep re-entering
     * shortly after every timeout exit (still comfortably above
     * ENTRY_US). That re-entry is not itself the deadlock this regression
     * targets - skip_stale_output stays true either way, so output stays
     * shed exactly as intended, and the real backstop for "this deficit
     * is too big to fix by shedding output alone" is live-resync's own,
     * separate, higher threshold - it is not this test's job to reach
     * that. What this test must prove is liveness: the unconditional
     * tick() timeout keeps firing and completing bounded episodes,
     * proving the mechanism can never get stuck ACTIVE with no further
     * exit event ever firing again (the original deadlock, where the
     * exit logic itself stopped running once qcount hit 0) - not that it
     * ends up permanently inactive, which isn't a meaningful outcome
     * when the underlying lateness genuinely never recovers. */
    sim s;
    sim_init(&s);
    s.qcount = 1;
    s.mono_clock_us = ENTRY_US + PERIOD_US * 2;
    s.decoded_pts_us = 0;

    uint64_t passes;
    unsigned since_last_boundary = 0;
    unsigned max_episode_passes = 0;
    for (passes = 0; passes < 20000; passes++) {
        uint64_t now_us = s.mono_clock_us;
        mr_micro_rescue_result tick_r =
            mr_micro_rescue_tick(&s.micro_rescue, now_us, MAX_US);
        if (tick_r.exited_timeout) {
            s.exits_timeout++;
            if (since_last_boundary > max_episode_passes)
                max_episode_passes = since_last_boundary;
            since_last_boundary = 0;
        }

        s.decoded_pts_us += PERIOD_US; /* no catch-up headroom modeled */
        int64_t pkt_late_us =
            (int64_t)s.mono_clock_us - (int64_t)s.decoded_pts_us;
        mr_micro_rescue_result pkt_r = mr_micro_rescue_on_packet(
            &s.micro_rescue, pkt_late_us, now_us, (int64_t)ENTRY_US,
            (int64_t)EXIT_US);
        if (pkt_r.entered) s.entries++;

        int skip_stale_output = s.qcount >= QUEUE_CAP ||
            pkt_late_us > (int64_t)PERIOD_US || s.micro_rescue.active;
        if (!skip_stale_output) s.qcount++;
        if (s.qcount > 0) s.qcount--;

        s.mono_clock_us += PERIOD_US;
        since_last_boundary++;
    }

    CHECK(s.exits_timeout > 5,
          "worst case: must complete several bounded timeout episodes "
          "rather than getting stuck active with no exit event ever "
          "firing again");
    /* Each completed episode's pass count is a proxy for its wall-clock
     * duration (PERIOD_US per pass); MAX_US/PERIOD_US bounds it, with a
     * couple of passes' slack for the tick-then-on_packet ordering
     * within a single scheduler pass. */
    CHECK(max_episode_passes <= (unsigned)(MAX_US / PERIOD_US) + 2,
          "worst case: no single episode may run longer than MAX_US");
}

int main(void)
{
    test_no_entry_below_threshold();
    test_entry_above_threshold();
    test_hysteresis_band_stays_active();
    test_recovery_exit();
    test_timeout_exit_without_any_packet();
    test_scheduler_deadlock_regression();
    test_scheduler_never_deadlocks_even_without_recovery();

    if (fails) {
        printf("FAILED: %d check(s)\n", fails);
        return 1;
    }
    puts("mr_micro_rescue: state-machine unit tests + deterministic "
         "scheduler deadlock regression passed");
    return 0;
}
