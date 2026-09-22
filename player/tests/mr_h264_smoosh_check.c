/*
 * Pins Smoosh (mr_h264_set_drop_nonsync(), the "datamosh" VQ mode) on real
 * H.264 streams:
 *
 *  - dropping every droppable access unit leaves exactly the keyframes, and
 *    each is byte-identical to the same picture from a normal decode - so
 *    the classifier never drops a keyframe and never lets a P/B slice
 *    through on either the AVCC (MP4) or the Annex-B (MPEG-TS) input path;
 *  - a partial drop pattern (the shape a late player produces) decodes with
 *    no errors, really drops pictures, and every keyframe after the smeared
 *    stretch is again byte-identical to the normal decode - proving the
 *    damage heals at the next keyframe instead of accumulating.
 *
 * Differential against our own full decode rather than ffmpeg: Smoosh's
 * surviving P pictures are deliberately wrong, and output count differs, so
 * only the keyframes have a meaningful reference.
 */
#include "../core/mr_demux.h"
#include "../core/mr_h264.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_FRAMES 128

typedef struct {
    uint64_t pts;
    uint8_t *data;
    size_t   len;
} frame_copy;

typedef struct {
    frame_copy f[MAX_FRAMES];
    int n;
    int skipped;
    int errors;
} decode_run;

static void keep(decode_run *run, mr_decoder *dec)
{
    const mr_frame *fr = &dec->frame;
    frame_copy *c;
    uint64_t pts = 0;
    int y;
    assert(run->n < MAX_FRAMES);
    assert(mr_h264_output_pts(dec, &pts));
    c = &run->f[run->n++];
    c->pts = pts;
    c->len = (size_t)fr->width * (size_t)fr->height * 3u;
    c->data = (uint8_t *)malloc(c->len);
    assert(c->data);
    for (y = 0; y < fr->height; y++)
        memcpy(c->data + (size_t)y * fr->width * 3u,
               fr->data + (size_t)y * fr->stride, (size_t)fr->width * 3u);
}

/* drop_mode: 0 = never, 1 = every packet, 2 = two packets in three. */
static void decode(const char *path, mr_h264_speed_mode speed, int drop_mode,
                   decode_run *run)
{
    mr_demux *dx = mr_demux_open_file(path);
    const mr_video_info *vi;
    mr_decoder dec;
    mr_packet pkt;
    unsigned long index = 0;

    memset(run, 0, sizeof *run);
    if (!dx) { fprintf(stderr, "cannot open %s\n", path); exit(1); }
    vi = mr_demux_video(dx);
    assert(vi && vi->valid);
    assert(mr_decoder_open_config(&dec, &mr_codec_h264, vi->width, vi->height,
                                  vi->config, vi->config_len) == MR_OK);
    assert(mr_h264_set_speed_mode(&dec, speed));
    while (mr_demux_next_packet(dx, &pkt) == MR_OK) {
        mr_status st;
        if (!pkt.is_video || !pkt.len) continue;
        mr_h264_set_input_annexb(&dec, pkt.is_annexb);
        mr_h264_set_input_pts(&dec, 1, (uint64_t)index * 1000u);
        mr_h264_set_drop_nonsync(&dec, drop_mode == 1 ||
                                       (drop_mode == 2 && index % 3u != 0));
        index++;
        st = mr_decoder_decode(&dec, pkt.data, pkt.len);
        if (st == MR_SKIPPED) { run->skipped++; continue; }
        if (st == MR_EAGAIN) continue;
        /* A P/B picture whose references Smoosh dropped may be refused by
         * libavc outright (seen on B-frame streams, never on Baseline).
         * mrplay logs it and carries on exactly like this; the keyframe
         * checks below prove the decoder resynchronises afterwards. */
        if (st == MR_EFORMAT && drop_mode == 2) { run->errors++; continue; }
        if (st != MR_OK) {
            fprintf(stderr, "%s: decode error %d at packet %lu (drop mode %d)\n",
                    path, (int)st, index - 1, drop_mode);
            exit(1);
        }
        do keep(run, &dec); while (mr_decoder_drain(&dec) == MR_OK);
    }
    while (mr_decoder_flush(&dec) == MR_OK) keep(run, &dec);
    mr_decoder_close(&dec);
    mr_demux_close(dx);
}

static const frame_copy *find(const decode_run *run, uint64_t pts)
{
    int i;
    for (i = 0; i < run->n; i++)
        if (run->f[i].pts == pts) return &run->f[i];
    return NULL;
}

static int same(const frame_copy *a, const frame_copy *b)
{
    return a && b && a->len == b->len && !memcmp(a->data, b->data, a->len);
}

static void release(decode_run *run)
{
    int i;
    for (i = 0; i < run->n; i++) free(run->f[i].data);
}

static void check_file(const char *path, int expect_keys, int baseline)
{
    decode_run full, keys, mosh;
    int i, keys_matched = 0;

    decode(path, MR_H264_SPEED_QUALITY, 0, &full);
    decode(path, MR_H264_SPEED_QUALITY, 1, &keys);
    decode(path, MR_H264_SPEED_QUALITY, 2, &mosh);
    assert(full.skipped == 0);

    /* Drop everything droppable: only keyframes remain, bit-exact. */
    if (keys.n != expect_keys) {
        fprintf(stderr, "%s: %d keyframes survived, expected %d\n",
                path, keys.n, expect_keys);
        exit(1);
    }
    assert(keys.skipped == full.n - expect_keys);
    for (i = 0; i < keys.n; i++) {
        if (!same(&keys.f[i], find(&full, keys.f[i].pts))) {
            fprintf(stderr, "%s: keyframe pts=%lu differs from full decode\n",
                    path, (unsigned long)keys.f[i].pts);
            exit(1);
        }
    }

    /* Partial drops: real drops, keyframes heal exactly. Baseline (every
     * picture a P, the YouTube case) must never be refused at all. */
    assert(mosh.skipped > 0);
    if (baseline) assert(mosh.errors == 0);
    assert(mosh.n + mosh.skipped + mosh.errors == full.n);
    for (i = 0; i < keys.n; i++) {
        const frame_copy *m = find(&mosh, keys.f[i].pts);
        if (!same(m, &keys.f[i])) {
            fprintf(stderr, "%s: keyframe pts=%lu not restored after "
                            "smoosh drops\n",
                    path, (unsigned long)keys.f[i].pts);
            exit(1);
        }
        keys_matched++;
    }
    printf("  %s: full=%d keyframes=%d smoosh kept=%d dropped=%d "
           "refused=%d (keyframes exact: %d)\n", path, full.n, keys.n,
           mosh.n, mosh.skipped, mosh.errors, keys_matched);
    release(&full); release(&keys); release(&mosh);
}

int main(void)
{
    decode_run turbo;
    /* Baseline, no B-frames, keyframe every 12 of 36 frames: the YouTube
     * itag 18 shape Smoosh exists for. MP4 = AVCC input, TS = Annex-B. */
    check_file("tests/assets/test_h264_gop.mp4", 3, 1);
    check_file("tests/assets/test_h264_gop.ts", 3, 1);
    /* High profile with B-frames (CABAC, reordering): 2 keyframes. */
    check_file("tests/assets/test_h264_high.mp4", 2, 0);
    check_file("tests/assets/test_h264_aac.ts", 2, 0);

    /* The Smoosh speed mode itself is accepted and decodes like Turbo. */
    decode("tests/assets/test_h264_gop.mp4", MR_H264_SPEED_SMOOSH, 2, &turbo);
    assert(turbo.n > 3 && turbo.skipped > 0);
    release(&turbo);
    printf("mr_h264_smoosh_check: OK\n");
    return 0;
}
