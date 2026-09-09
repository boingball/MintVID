/*
 * Verify that the MPEG program-stream demuxer hands out real PES timestamps.
 *
 * mr_ps used to walk straight past the PTS fields in the PES header and leave
 * mr_packet::has_pts alone, so every MPEG-PS clip reached the player untimed:
 * mr_mpeg2_set_input_pts() was always told "no timestamp",
 * mr_mpeg2_output_pts() always answered 0, and A/V sync silently fell back to
 * the synthetic display-order clock. Nothing failed - it just drifted, and the
 * audio stream had no anchor at all.
 *
 * tests/mr_ps_check.c pins the header-walking rules on synthetic streams. This
 * one pins the numbers against a real fixture, taken from ffprobe:
 *
 *   ffprobe -select_streams v:0 -show_entries packet=pts_time \
 *           -of csv=p=0 tests/assets/test_mpeg1_odd.mpg
 *
 * so a plausible-but-wrong parse (a dropped marker bit, a 90 kHz/µs mixup)
 * cannot pass by producing merely monotonic garbage.
 */
#include "../core/mr_ps.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* PES timestamps are 90 kHz ticks, so the microsecond value is exact only to
 * within one truncated tick (11.2 us). */
#define PTS_TOLERANCE_US  12

static int close_enough(uint64_t got, uint64_t want)
{
    uint64_t d = got > want ? got - want : want - got;
    return d <= PTS_TOLERANCE_US;
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

int main(int argc, char **argv)
{
    mr_ps ps;
    mr_packet pkt;
    unsigned char *data;
    size_t len = 0;
    uint64_t first_video = 0, prev_video = 0, first_audio = 0;
    uint64_t want_first_video, want_period, want_first_audio = 0;
    int have_first_video = 0, have_first_audio = 0, want_audio = 0;
    int timed_video = 0, untimed_video = 0, rc = 0;

    if (argc != 4 && argc != 5) {
        fprintf(stderr, "usage: mr_ps_pts_check <file.mpg> <first_video_us> "
                        "<period_us> [first_audio_us]\n");
        return 2;
    }
    want_first_video = strtoul(argv[2], NULL, 10);
    want_period      = strtoul(argv[3], NULL, 10);
    if (argc == 5) {
        want_first_audio = strtoul(argv[4], NULL, 10);
        want_audio = 1;
    }
    if (!(data = slurp(argv[1], &len))) {
        fprintf(stderr, "cannot read %s\n", argv[1]);
        return 2;
    }
    if (mr_ps_open(&ps, data, len) != MR_OK) {
        fprintf(stderr, "cannot open %s as MPEG-PS\n", argv[1]);
        free(data);
        return 2;
    }
    memset(&pkt, 0, sizeof pkt);
    while (mr_ps_next_packet(&ps, &pkt) == MR_OK) {
        if (!pkt.is_video) {
            if (pkt.has_pts && !have_first_audio) {
                first_audio = pkt.pts_us;
                have_first_audio = 1;
            }
            continue;
        }
        if (!pkt.has_pts) { untimed_video++; continue; }
        timed_video++;
        if (!have_first_video) {
            first_video = prev_video = pkt.pts_us;
            have_first_video = 1;
            continue;
        }
        /* Video PES packets arrive in decode order, but these fixtures have no
         * B-frames, so display order is decode order and the stamps must climb
         * by whole frame periods. Not necessarily *one* period each: MPEG only
         * requires a PTS on the first access unit that begins in a PES packet,
         * so where one PES starts two pictures the second carries none and the
         * next stamp is two periods on. (ffprobe interpolates those; the
         * container really does not hold them.) */
        {
            uint64_t delta = pkt.pts_us > prev_video ? pkt.pts_us - prev_video
                                                     : 0;
            uint64_t periods = (delta + want_period / 2) / want_period;
            if (!periods || !close_enough(delta, periods * want_period)) {
                fprintf(stderr, "FAIL: video PTS %lu follows %lu; %lu us is "
                                "not a whole number of %lu us frame periods\n",
                        (unsigned long)pkt.pts_us, (unsigned long)prev_video,
                        (unsigned long)delta, (unsigned long)want_period);
                rc = 1;
                break;
            }
        }
        prev_video = pkt.pts_us;
    }
    if (!rc && !have_first_video) {
        fprintf(stderr, "FAIL: no video packet carried a PTS - the PES header "
                        "parse is dropping the timestamp\n");
        rc = 1;
    }
    if (!rc && !close_enough(first_video, want_first_video)) {
        fprintf(stderr, "FAIL: first video PTS %lu us, expected %lu us\n",
                (unsigned long)first_video, (unsigned long)want_first_video);
        rc = 1;
    }
    /* Timed packets must dominate. A PES that starts no picture legitimately
     * carries none, but a parse that only ever catches the odd one is broken. */
    if (!rc && timed_video <= untimed_video) {
        fprintf(stderr, "FAIL: only %d of %d video packets carried a PTS\n",
                timed_video, timed_video + untimed_video);
        rc = 1;
    }
    if (!rc && want_audio) {
        if (!have_first_audio) {
            fprintf(stderr, "FAIL: no audio packet carried a PTS\n");
            rc = 1;
        } else if (!close_enough(first_audio, want_first_audio)) {
            fprintf(stderr, "FAIL: first audio PTS %lu us, expected %lu us\n",
                    (unsigned long)first_audio,
                    (unsigned long)want_first_audio);
            rc = 1;
        }
    }
    if (!rc)
        printf("mpeg-ps PTS: %s: %d timed video packets from %lu us, step "
               "%lu us%s: OK\n", argv[1], timed_video,
               (unsigned long)first_video, (unsigned long)want_period,
               want_audio ? ", audio anchored" : "");
    mr_ps_close(&ps);
    free(data);
    return rc;
}
