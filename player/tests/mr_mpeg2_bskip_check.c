/*
 * Proves mr_mpeg2_set_speed_mode(MR_MPEG2_SPEED_FAST)'s B-frame skip (see
 * CLAUDE.md's "MPEG-1/2 B-frame skip" notes) is safe: every displayed frame
 * under Fast mode must be byte-for-byte identical to some frame from a
 * normal (Quality) decode of the same real MPEG-2 elementary stream, in the
 * same order - proving the skip drops exactly the B pictures and nothing
 * else, and never disturbs the I/P reference chain those B pictures (never
 * referenced by anything, by MPEG-2's own design) sit between.
 *
 * This is not a --check-against-ffmpeg test on purpose: --check compares
 * frame N of the decoded output against refdir/fNNN.ppm by sequential
 * index, which only makes sense when every source frame is still present.
 * Fast mode produces fewer frames than the source, so frame N of its output
 * no longer corresponds to reference frame N once even one B has been
 * dropped - comparing that way was tried first and produced a *misleading*
 * failure (steadily growing MAE from index-drift, not real corruption) that
 * this differential design avoids entirely by never touching the ffmpeg
 * reference at all.
 */
#include "../core/mr_demux.h"
#include "../core/mr_mpeg2.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

/* One packed RGB24 frame, width*height*3 bytes, row padding stripped -
 * mirrors mr_decode.c's own write_ppm() byte layout exactly, so frames
 * captured here are directly memcmp-comparable regardless of stride. */
typedef struct {
    uint8_t *data;
    size_t   len;
} captured_frame;

static void capture(captured_frame *out, const mr_frame *fr)
{
    int y;
    out->len = (size_t)fr->width * (size_t)fr->height * 3u;
    out->data = (uint8_t *)malloc(out->len ? out->len : 1);
    assert(out->data);
    for (y = 0; y < fr->height; y++)
        memcpy(out->data + (size_t)y * fr->width * 3,
               fr->data + (size_t)y * fr->stride, (size_t)fr->width * 3);
}

/* Decodes every video packet of `path` through mr_codec_mpeg2 at the given
 * speed mode, capturing every displayed frame in order. Returns the frame
 * count, or -1 on error. */
static int decode_all(const char *path, mr_mpeg2_speed_mode mode,
                      captured_frame **frames_out, int max_frames)
{
    mr_demux *dx = mr_demux_open_file(path);
    const mr_video_info *vi;
    mr_decoder dec;
    mr_packet pkt;
    captured_frame *frames;
    int n = 0;

    if (!dx) { fprintf(stderr, "could not open %s\n", path); return -1; }
    vi = mr_demux_video(dx);
    if (!vi || !vi->valid) { mr_demux_close(dx); return -1; }
    if (mr_decoder_open_config(&dec, &mr_codec_mpeg2, vi->width, vi->height,
                               vi->config, vi->config_len) != MR_OK) {
        mr_demux_close(dx);
        return -1;
    }
    mr_mpeg2_set_speed_mode(&dec, mode);

    frames = (captured_frame *)calloc((size_t)max_frames, sizeof *frames);
    assert(frames);

    while (mr_demux_next_packet(dx, &pkt) == MR_OK) {
        mr_status st;
        if (!pkt.is_video || !pkt.len) continue;
        st = mr_decoder_decode(&dec, pkt.data, pkt.len);
        if (st == MR_EAGAIN) continue;
        if (st != MR_OK) {
            fprintf(stderr, "decode error at frame %d\n", n);
            mr_decoder_close(&dec); mr_demux_close(dx);
            return -1;
        }
        while (1) {
            assert(n < max_frames);
            capture(&frames[n++], &dec.frame);
            if (mr_decoder_drain(&dec) != MR_OK) break;
        }
    }
    while (mr_decoder_flush(&dec) == MR_OK) {
        assert(n < max_frames);
        capture(&frames[n++], &dec.frame);
    }

    mr_decoder_close(&dec);
    mr_demux_close(dx);
    *frames_out = frames;
    return n;
}

static void free_frames(captured_frame *frames, int n)
{
    int i;
    for (i = 0; i < n; i++) free(frames[i].data);
    free(frames);
}

int main(void)
{
    const char *path = "tests/assets/test_mpeg2.ts";
    captured_frame *full, *fast;
    int full_n, fast_n, i, j;

    full_n = decode_all(path, MR_MPEG2_SPEED_QUALITY, &full, 256);
    assert(full_n > 0);
    fast_n = decode_all(path, MR_MPEG2_SPEED_FAST, &fast, 256);
    assert(fast_n > 0);

    /* Pinned against this specific, checked-in fixture (test_mpeg2.ts,
     * "MPEG-TS: MPEG-2 Main Profile + B-frames" - 5 I, 13 P, 32 B per
     * ffprobe) - a regression that starts skipping the wrong pictures, or
     * stops skipping at all, changes this exact count and fails loudly
     * here rather than merely reading as "fewer frames, probably fine". */
    assert(full_n == 50);
    assert(fast_n == 18);

    /* Every Fast-mode frame must appear, in order, as an exact byte match
     * somewhere in the Quality-mode sequence - proving no corruption (a
     * genuinely wrong/stale buffer would not byte-match anything) and no
     * reordering (a dropped B disturbing the I/P chain would desync every
     * frame after it, not just leave gaps). */
    j = 0;
    for (i = 0; i < fast_n; i++) {
        int found = 0;
        for (; j < full_n; j++) {
            if (fast[i].len == full[j].len &&
                memcmp(fast[i].data, full[j].data, fast[i].len) == 0) {
                found = 1;
                j++;
                break;
            }
        }
        if (!found) {
            fprintf(stderr, "fast frame %d not found as an ordered "
                            "subsequence of the full decode\n", i);
            assert(0);
        }
    }

    free_frames(full, full_n);
    free_frames(fast, fast_n);

    printf("mpeg2 B-frame skip: %d frames (quality) -> %d frames (fast), "
          "%d B pictures dropped, every kept frame verified byte-exact "
          "against the full decode: OK\n", full_n, fast_n, full_n - fast_n);
    return 0;
}
