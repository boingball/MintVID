/*
 * Regression test for play_mpeg1()'s pacing, run against a real .mpg.
 *
 * amiga/mrplay.c cannot be compiled on the dev host, so the two policy calls
 * it makes per iteration live in core/mr_mpeg1_sched.c and are exercised here
 * inside a model of the loop that surrounds them: a virtual clock, a Paula
 * device with audio_paula.c's 4 s ring FIFO and its played-samples clock, and
 * the real MPEG-1 decoder supplying frames, timestamps and MP2 audio. The
 * `legacy` runs reproduce the loop exactly as it was before this fix, so each
 * assertion below is known to be able to fail.
 *
 * Two defects, both reported on a 134x100 25 fps clip with a 22.05 kHz MP2
 * track played on an A1200/AGA, where a loop iteration costs far more than the
 * stream's 40 ms frame period:
 *
 *  - The picture froze on frame 1 for the whole file. The drop rule was
 *    uncapped, and once the machine falls behind, the audio clock is
 *    permanently more than one frame period ahead of video pts. This path
 *    decodes serially with no queue to skip into, so the condition never
 *    clears: every frame after the first was dropped and never displayed.
 *
 *  - The sound became one short sample repeating forever. The top-up pulled a
 *    fixed 2 MP2 frames per video frame, and at 22.05 kHz an MP2 frame is
 *    52 ms, so it queued 104 ms of audio for every 40 ms of video - 2.6x real
 *    time. The whole track went into the FIFO during the first third of the
 *    clip (peaking at 3.2 s queued here, and overrunning the 4 s ring outright
 *    on anything longer), after which nothing was ever queued again. With the
 *    picture frozen the loop then ground through the remaining frames for
 *    another ten-odd seconds with Paula holding its last buffer. Counting MP2
 *    frames is wrong in both directions; the cushion is in milliseconds.
 */
#include "../core/mr_mpeg1.h"
#include "../core/mr_mpeg1_sched.h"

#include <stdio.h>
#include <stdlib.h>

#define FIFO_MS 4000UL                  /* audio_paula.c: output_rate * 4 */

typedef struct {
    unsigned long now_ms;               /* virtual monotonic clock         */
    unsigned long written_ms;           /* audio accepted into the FIFO    */
    unsigned long dropped_ms;           /* audio the full FIFO discarded   */
    unsigned long play_start_ms;        /* when the playback gate opened   */
    int           started;
} sim_audio;

/* audio_elapsed_ms(): samples actually played. It cannot run past what has
 * been written, so it stalls whenever the device is starved - which is what
 * the repeating Paula buffer sounds like. */
static unsigned long sim_elapsed(const sim_audio *a)
{
    unsigned long e;
    if (!a->started) return 0;
    e = a->now_ms - a->play_start_ms;
    return e > a->written_ms ? a->written_ms : e;
}

static unsigned long sim_buffered(const sim_audio *a)
{
    return a->written_ms - sim_elapsed(a);
}

static int sim_starved(const sim_audio *a)
{
    return a->started && sim_buffered(a) == 0;
}

static void sim_write(sim_audio *a, unsigned long ms)
{
    unsigned long room = FIFO_MS - sim_buffered(a);
    if (ms > room) { a->dropped_ms += ms - room; ms = room; }
    a->written_ms += ms;
}

typedef struct {
    unsigned decoded, presented, dropped;
    unsigned long audio_dropped_ms;     /* lost to FIFO overrun            */
    unsigned long max_buffered_ms;      /* peak audio queued ahead of Paula */
    unsigned long longest_gap_ms;       /* longest freeze between shown frames */
    unsigned      dry_iters;            /* iterations with Paula run dry    */
    unsigned      max_run_pulls;        /* most MP2 frames decoded in one
                                         * iteration once playback started  */
} sim_stats;

/*
 * One playback run. decode_ms/show_ms are what this machine costs per frame.
 * With legacy = 1 the loop body is the pre-fix one: a fixed MP2-frame top-up,
 * an uncapped drop rule, and no fallback when the audio track ends.
 */
static int simulate(const unsigned char *data, size_t len,
                    unsigned long decode_ms, unsigned long show_ms,
                    unsigned long mp2_ms, int legacy, sim_stats *st)
{
    mr_mpeg1 *mp = mr_mpeg1_open(data, len, 0, 0, 0);
    unsigned char *abuf;
    unsigned rate;
    int audio_dry = 0, drop_run = 0;
    unsigned long period, clock_base = 0;
    int64_t pts_us, pts_base_us = 0;
    int have_pts_base = 0;
    sim_audio au;
    mr_frame fr;
    unsigned long last_shown_ms;

    if (!mp) return -1;
    abuf = (unsigned char *)malloc(1152 * 4);
    if (!abuf) { mr_mpeg1_close(mp); return -1; }
    rate = mr_mpeg1_samplerate(mp);
    period = mr_mpeg1_framerate_millihz(mp)
           ? (1000000UL + mr_mpeg1_framerate_millihz(mp) / 2)
             / mr_mpeg1_framerate_millihz(mp)
           : 40;

    au.now_ms = 0; au.written_ms = 0; au.dropped_ms = 0;
    au.play_start_ms = 0; au.started = 0;
    st->decoded = st->presented = st->dropped = 0;
    st->audio_dropped_ms = 0; st->max_buffered_ms = 0; st->dry_iters = 0;
    st->longest_gap_ms = 0; st->max_run_pulls = 0;
    last_shown_ms = 0;

    while (mr_mpeg1_next(mp, &fr, &pts_us)) {
        unsigned long target;
        int n, k = 0, pulled = 0;

        au.now_ms += decode_ms;
        st->decoded++;
        if (!have_pts_base) {
            pts_base_us = pts_us;
            have_pts_base = 1;
            clock_base = sim_elapsed(&au);
        }

        if (rate) {                              /* top up audio             */
            if (sim_starved(&au)) st->dry_iters++;
            while (legacy ? k < (st->decoded == 1 ? 4 : 2)
                          : mr_mpeg1_want_audio(sim_buffered(&au),
                                                au.started, k)) {
                if ((n = mr_mpeg1_audio(mp, abuf)) <= 0) break;
                /* Decoding an MP2 frame is not free: play_mpeg1() uses
                 * pl_mpeg's portable C Layer II decoder, inline between one
                 * shown frame and the next. Charging for it is what makes a
                 * refill burst visible as a freeze. */
                au.now_ms += mp2_ms;
                sim_write(&au, (unsigned long)n * 1000UL / rate);
                pulled = 1;
                k++;
            }
            if (au.started && (unsigned)k > st->max_run_pulls)
                st->max_run_pulls = (unsigned)k;
            if (!au.started) { au.started = 1; au.play_start_ms = au.now_ms;
                               last_shown_ms = au.now_ms; }
            if (sim_buffered(&au) > st->max_buffered_ms)
                st->max_buffered_ms = sim_buffered(&au);
            if (!legacy) {
                if (pulled) audio_dry = 0;
                else if (sim_starved(&au)) audio_dry = 1;
            }
        }

        if (rate && !audio_dry) {                /* pace to the audio clock  */
            target = clock_base + (unsigned long)((pts_us - pts_base_us) / 1000);
            while (sim_elapsed(&au) < target && !sim_starved(&au))
                au.now_ms++;                     /* Delay(1)                 */
            if (legacy ? sim_elapsed(&au) > target + period
                       : mr_mpeg1_drop_frame(sim_elapsed(&au), target, period,
                                             drop_run)) {
                drop_run++;
                st->dropped++;
                continue;
            }
            drop_run = 0;
        } else {
            au.now_ms += period;
        }
        if (st->presented && au.now_ms - last_shown_ms > st->longest_gap_ms)
            st->longest_gap_ms = au.now_ms - last_shown_ms;
        last_shown_ms = au.now_ms;
        st->presented++;
        au.now_ms += show_ms;
    }

    st->audio_dropped_ms = au.dropped_ms;
    free(abuf);
    mr_mpeg1_close(mp);
    return 0;
}

static unsigned char *slurp(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    unsigned char *data;
    long bytes;
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    bytes = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (bytes <= 0) { fclose(f); return NULL; }
    data = (unsigned char *)malloc((size_t)bytes);
    if (!data || fread(data, 1, (size_t)bytes, f) != (size_t)bytes) {
        free(data); fclose(f); return NULL;
    }
    fclose(f);
    *len = (size_t)bytes;
    return data;
}

static void report(const char *what, const sim_stats *s)
{
    printf("%-22s presented=%u/%u dropped=%u peak-queued=%lu ms "
           "fifo-lost=%lu ms dry-iters=%u burst=%u pulls gap=%lu ms\n",
           what, s->presented, s->decoded, s->dropped,
           s->max_buffered_ms, s->audio_dropped_ms, s->dry_iters,
           s->max_run_pulls, s->longest_gap_ms);
}

/* A machine well under the 40 ms frame period - the reported 060/50 decodes
 * this 134x100 clip comfortably, which is why it plays clean once started. */
#define FAST_DECODE_MS 10
#define FAST_SHOW_MS    5
/* A machine that cannot keep up at all, exercising the drop-run cap. */
#define SLOW_DECODE_MS 90
#define SLOW_SHOW_MS   60
/* One MP2 frame through pl_mpeg's portable C Layer II decoder. Not free on
 * 68k, and charged inline between one shown frame and the next - the cost the
 * first version of this test wrongly treated as zero, which is why it did not
 * predict the startup freeze. */
#define MP2_DECODE_MS   8

int main(int argc, char **argv)
{
    unsigned char *data;
    size_t len = 0;
    sim_stats fast, slow, old_slow, old_fast;
    int rc = 0;

    if (argc != 2) {
        fprintf(stderr, "usage: mr_mpeg1_sched_check <file.mpg>\n");
        return 2;
    }
    if (!(data = slurp(argv[1], &len))) {
        fprintf(stderr, "cannot read %s\n", argv[1]);
        return 2;
    }

    if (simulate(data, len, FAST_DECODE_MS, FAST_SHOW_MS, MP2_DECODE_MS, 0, &fast) < 0) {
        fprintf(stderr, "cannot open %s as MPEG-1\n", argv[1]);
        free(data); return 2;
    }
    if (simulate(data, len, SLOW_DECODE_MS, SLOW_SHOW_MS, MP2_DECODE_MS, 0, &slow) < 0 ||
        simulate(data, len, SLOW_DECODE_MS, SLOW_SHOW_MS, MP2_DECODE_MS, 1, &old_slow) < 0 ||
        simulate(data, len, FAST_DECODE_MS, FAST_SHOW_MS, MP2_DECODE_MS, 1, &old_fast) < 0) {
        free(data); return 2;
    }
    report("fast machine", &fast);
    report("slow machine", &slow);
    report("legacy, slow machine", &old_slow);
    report("legacy, fast machine", &old_fast);

    /* A machine that keeps up shows every frame and never starves Paula. */
    if (fast.presented != fast.decoded || fast.dropped != 0) {
        fprintf(stderr, "FAIL: frames dropped on a machine that keeps up\n");
        rc = 1;
    }
    if (fast.dry_iters != 0 || fast.audio_dropped_ms != 0) {
        fprintf(stderr, "FAIL: audio mis-fed on a machine that keeps up\n");
        rc = 1;
    }

    /* A machine that cannot keep up shows a slower picture, but a picture:
     * the drop run is capped, so at least one frame in MPEG1_MAX_DROP_RUN + 1
     * reaches the screen instead of the display freezing on frame 1. */
    if (slow.presented < slow.decoded / (MPEG1_MAX_DROP_RUN + 1)) {
        fprintf(stderr, "FAIL: only %u of %u frames reached the screen\n",
                slow.presented, slow.decoded);
        rc = 1;
    }

    /* The cushion is primed before the playback gate opens, not ramped up
     * after it. Once Paula is playing, a machine that keeps up only replaces
     * what has drained - about one MP2 frame per video frame. A post-gate ramp
     * shows up here as a burst that runs into MPEG1_AUDIO_MAX_PULLS, and on
     * hardware as the startup freeze and one-frame-in-three stepping this
     * bound exists to prevent. */
    if (fast.max_run_pulls > 2) {
        fprintf(stderr, "FAIL: %u MP2 frames decoded in one iteration with "
                        "Paula playing; the cushion is being ramped after the "
                        "gate opens, not primed before it\n",
                fast.max_run_pulls);
        rc = 1;
    }
    if (slow.max_run_pulls > MPEG1_AUDIO_MAX_PULLS ||
        fast.max_run_pulls > MPEG1_AUDIO_MAX_PULLS) {
        fprintf(stderr, "FAIL: post-gate refill burst exceeded the cap\n");
        rc = 1;
    }
    /* No freeze at the start on a machine that keeps up: consecutive shown
     * frames stay within a couple of frame periods of each other. */
    if (fast.longest_gap_ms > 120) {
        fprintf(stderr, "FAIL: %lu ms between consecutive shown frames on a "
                        "machine that keeps up\n", fast.longest_gap_ms);
        rc = 1;
    }

    /* Neither machine queues far past the cushion. The bound is the cushion
     * plus the one MP2 frame that crosses it (52 ms at 22.05 kHz); leaving
     * headroom for a longer frame at a lower rate. */
    if (fast.max_buffered_ms > MPEG1_AUDIO_CUSHION_MS + 200 ||
        slow.max_buffered_ms > MPEG1_AUDIO_CUSHION_MS + 200) {
        fprintf(stderr, "FAIL: audio queued %lu/%lu ms ahead, cushion is "
                        "%lu ms\n", fast.max_buffered_ms,
                slow.max_buffered_ms, MPEG1_AUDIO_CUSHION_MS);
        rc = 1;
    }
    if (fast.audio_dropped_ms != 0 || slow.audio_dropped_ms != 0) {
        fprintf(stderr, "FAIL: audio overran the FIFO\n");
        rc = 1;
    }

    /* The legacy runs must still exhibit the defects, or the assertions above
     * would pass without the fix and pin down nothing. */
    if (old_slow.presented > 2) {
        fprintf(stderr, "FAIL: the legacy drop rule no longer freezes the "
                        "picture on a slow machine (%u frames shown)\n",
                old_slow.presented);
        rc = 1;
    }
    /* The legacy top-up runs the whole 5 s track into the FIFO in the first
     * third of the clip. On a file long enough to reach it that is an outright
     * FIFO overrun; here it shows up as a queue several seconds deep, followed
     * by a Paula left with nothing to play for the rest of the file. */
    if (old_fast.max_buffered_ms < 2500) {
        fprintf(stderr, "FAIL: the legacy top-up no longer over-queues "
                        "(peak %lu ms)\n", old_fast.max_buffered_ms);
        rc = 1;
    }
    /* dry-iters is reported but not asserted on: once the MP2 track ends, a
     * machine this slow still has most of the file left to walk, and no
     * top-up policy can conjure audio that is not there. That tail is why
     * play_mpeg1() now closes the Paula gate when the track runs out, rather
     * than leaving the device sitting on its last buffer. */

    free(data);
    if (!rc) printf("mpeg1 scheduler: OK\n");
    return rc;
}
