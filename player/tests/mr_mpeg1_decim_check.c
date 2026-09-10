/*
 * Proves plm_audio_set_decim()'s Fast MP2 mode (pl_mpeg.h - see CLAUDE.md's
 * "MPEG-1/2 (libmpeg2) notes") is bit-exact: for every decoded frame, the
 * decim-N output must equal a full decim=1 decode of the same frame with
 * every N-th sample kept - the same relationship mr_mpeg1_audio() used to
 * compute itself, by decoding everything and discarding samples afterward
 * (see CLAUDE.md's "genuine MP2 fast/superfast path" note). This is not a
 * test of whether the reduced-work derivation is sound in the abstract -
 * that argument is in the comment beside plm_audio_synth_window_decim() in
 * pl_mpeg.h - it is a direct check that skipping the discarded lanes'
 * synthesis work never changes a single kept sample, for stereo, mono, and
 * both decim=2 and decim=4, against a real MP2 elementary stream extracted
 * from an existing test fixture.
 */
#include "../core/mr_demux.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* pl_mpeg's implementation (PL_MPEG_IMPLEMENTATION) is compiled once, into
 * core/mr_mpeg1.c, which this test links against - defining it again here
 * would duplicate every symbol at link time. This translation unit only
 * needs the declarations. */
#include "../core/pl_mpeg.h"

static uint8_t *extract_mp2_es(const char *path, long *len_out)
{
    mr_demux *dx = mr_demux_open_file(path);
    const mr_audio_info *ai;
    mr_packet pkt;
    uint8_t *buf = NULL;
    size_t len = 0, cap = 0;

    if (!dx) {
        fprintf(stderr, "could not open %s\n", path);
        return NULL;
    }
    ai = mr_demux_audio(dx);
    if (!ai || ai->format_tag != MR_AUDIO_FORMAT_MP2) {
        fprintf(stderr, "%s: not an MP2 track\n", path);
        mr_demux_close(dx);
        return NULL;
    }
    while (mr_demux_next_packet(dx, &pkt) == MR_OK) {
        if (pkt.is_video || !pkt.len) continue;
        if (len + pkt.len > cap) {
            cap = (len + pkt.len) * 2 + 4096;
            buf = realloc(buf, cap);
        }
        memcpy(buf + len, pkt.data, pkt.len);
        len += pkt.len;
    }
    mr_demux_close(dx);
    *len_out = (long)len;
    return buf;
}

/* Decodes every frame of `data` at the given decim/mono setting, appending
 * each frame's interleaved samples (already at the reduced per-frame count
 * when decim > 1) as one entry in `frames_out` (caller-owned array of
 * pointers, each frame malloc'd here). Returns the number of frames
 * decoded, or -1 on error. `channels_out` receives the channel count. */
static int decode_all(const uint8_t *data, long len, int decim, int mono,
                      int16_t **frames_out, unsigned *counts_out, int max_frames,
                      int *channels_out)
{
    plm_buffer_t *buf = plm_buffer_create_with_capacity((size_t)len);
    plm_audio_t *dec;
    int n = 0;
    plm_samples_t *s;

    if (!buf) return -1;
    plm_buffer_write(buf, (uint8_t *)data, (size_t)len);
    dec = plm_audio_create_with_buffer(buf, 1);
    if (!dec) return -1;
    plm_audio_set_mono(dec, mono);
    plm_audio_set_decim(dec, decim);
    *channels_out = plm_audio_get_channels(dec);

    while ((s = plm_audio_decode(dec)) != NULL) {
        size_t total = (size_t)s->count * (size_t)*channels_out;
        if (n >= max_frames) break;
        frames_out[n] = malloc(total * sizeof(int16_t));
        memcpy(frames_out[n], s->interleaved, total * sizeof(int16_t));
        counts_out[n] = s->count;
        n++;
    }
    plm_audio_destroy(dec);
    return n;
}

static int check_variant(const uint8_t *data, long len, int decim, int mono,
                         const char *label)
{
#define MAX_FRAMES 256
    static int16_t *ref_frames[MAX_FRAMES];
    static int16_t *fast_frames[MAX_FRAMES];
    static unsigned ref_counts[MAX_FRAMES], fast_counts[MAX_FRAMES];
    int ref_n, fast_n, i, ch_ref, ch_fast, fail = 0;

    ref_n = decode_all(data, len, 1, mono, ref_frames, ref_counts, MAX_FRAMES, &ch_ref);
    fast_n = decode_all(data, len, decim, mono, fast_frames, fast_counts, MAX_FRAMES, &ch_fast);

    if (ref_n <= 0 || fast_n != ref_n || ch_ref != ch_fast) {
        fprintf(stderr, "%s: frame/channel count mismatch (ref=%d fast=%d ch=%d/%d)\n",
                label, ref_n, fast_n, ch_ref, ch_fast);
        return 1;
    }

    for (i = 0; i < ref_n; i++) {
        unsigned expect = ref_counts[i] / (unsigned)decim;
        unsigned j, ch;
        if (fast_counts[i] != expect) {
            fprintf(stderr, "%s: frame %d count %u, expected %u\n",
                    label, i, fast_counts[i], expect);
            fail = 1;
            continue;
        }
        for (j = 0; j < fast_counts[i]; j++) {
            for (ch = 0; ch < (unsigned)ch_ref; ch++) {
                int16_t r = ref_frames[i][(size_t)(j * (unsigned)decim) * ch_ref + ch];
                int16_t f = fast_frames[i][(size_t)j * ch_fast + ch];
                if (r != f) {
                    fprintf(stderr,
                            "%s: frame %d sample %u ch %u differs: ref=%d fast=%d\n",
                            label, i, j, ch, r, f);
                    fail = 1;
                }
            }
        }
    }

    for (i = 0; i < ref_n; i++) free(ref_frames[i]);
    for (i = 0; i < fast_n; i++) free(fast_frames[i]);

    if (!fail) {
        printf("%s: %d frames, decim=%d, bit-exact\n", label, ref_n, decim);
    }
    return fail;
#undef MAX_FRAMES
}

int main(int argc, char **argv)
{
    const char *path = argc > 1 ? argv[1] : "tests/assets/test_mp2_stereo.ts";
    long len;
    uint8_t *data = extract_mp2_es(path, &len);
    int fail = 0;

    if (!data || len <= 0) {
        fprintf(stderr, "no MP2 elementary stream extracted from %s\n", path);
        return 2;
    }

    fail |= check_variant(data, len, 2, 0, "stereo decim=2");
    fail |= check_variant(data, len, 4, 0, "stereo decim=4");
    fail |= check_variant(data, len, 2, 1, "mono decim=2");
    fail |= check_variant(data, len, 4, 1, "mono decim=4");

    free(data);
    return fail ? 1 : 0;
}
