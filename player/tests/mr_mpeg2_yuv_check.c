/*
 * Verify mr_mpeg2_set_yuv_output() against the adapter's own RGB24 output.
 *
 * The MPEG-1/2 adapter normally converts every displayed picture to RGB24
 * (emit_rgb). Under yuv_output it instead copies libmpeg2's three planes out
 * packed (emit_yuv), so a caller that is going to dither to palette indices
 * never pays for RGB24 at all. Getting that copy wrong is silent: swapping the
 * chroma planes only shifts the colour, and reading rows at the visible width
 * instead of libmpeg2's macroblock-aligned pitch only skews the picture.
 *
 * So decode the same clip twice, once each way, and run the YUV planes back
 * through the very converter emit_rgb() uses. Same transform, same inputs, so
 * the two must come out *bit-identical* - which makes this a far sharper test
 * than a tolerance would be: any error in the plane pointers, the strides or
 * the chroma order shows up immediately. Introducing either mistake takes the
 * mean error from 0 to 50 (visible-width stride) or 99 (swapped chroma).
 *
 * The clip must have a width that is not a multiple of 16, or the aligned
 * pitch equals the visible width and no stride bug can show.
 */
#include "../core/mr_codec.h"
#include "../core/mr_mpeg2.h"
#include "../core/mr_ps.h"
#include "../core/mr_demux.h"
#include "../core/mr_yuv.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Exact: emit_rgb() and this test run the same converter over the same
 * pixels, so any difference at all is a bug in the plane handoff. */
#define MAE_LIMIT     0.0
#define MAXERR_LIMIT  0

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

static int check_display_pts(mr_decoder *dec, int frame,
                             uint64_t *last_pts, int *have_last, int *count)
{
    uint64_t pts;
    if (!mr_mpeg2_output_pts(dec, &pts)) return 1;
    if (*have_last && pts <= *last_pts) {
        fprintf(stderr,
                "FAIL: MPEG display PTS not increasing at frame %d: %llu after %llu\n",
                frame, (unsigned long long)pts,
                (unsigned long long)*last_pts);
        return 0;
    }
    *last_pts = pts;
    *have_last = 1;
    (*count)++;
    return 1;
}

/* Decode every displayed picture, appending each to `out` as RGB24. In YUV
 * mode the planes are converted here with the shared converter. */
static int decode_all(const unsigned char *data, size_t len, int w, int h,
                      int yuv, unsigned char **out, int *frames)
{
    mr_decoder dec;
    mr_ps ps;
    mr_packet pkt;
    size_t frame_bytes = (size_t)w * h * 3;
    size_t cap = 16, n = 0;
    uint64_t last_pts = 0;
    int have_last_pts = 0, tagged_frames = 0;
    unsigned char *buf = (unsigned char *)malloc(cap * frame_bytes);
    if (!buf) return 0;
    if (mr_ps_open(&ps, data, len) != MR_OK) { free(buf); return 0; }
    memset(&dec, 0, sizeof dec);
    dec.codec = &mr_codec_mpeg2;
    dec.width = w; dec.height = h;
    if (mr_codec_mpeg2.open(&dec) != MR_OK) { free(buf); return 0; }
    if (yuv) mr_mpeg2_set_yuv_output(&dec, 1);

    for (;;) {
        mr_status st;
        int got;
        if (mr_ps_next_packet(&ps, &pkt) != MR_OK) break;
        if (!pkt.is_video) continue;
        mr_mpeg2_set_input_pts(&dec, pkt.has_pts, pkt.pts_us);
        st = mr_codec_mpeg2.decode(&dec, pkt.data, pkt.len);
        for (got = (st == MR_OK); got; ) {
            if (!check_display_pts(&dec, (int)n, &last_pts, &have_last_pts,
                                   &tagged_frames)) {
                free(buf); mr_codec_mpeg2.close(&dec); return 0;
            }
            if (n == cap) {
                unsigned char *p;
                cap *= 2;
                p = (unsigned char *)realloc(buf, cap * frame_bytes);
                if (!p) { free(buf); return 0; }
                buf = p;
            }
            if (yuv) {
                if (dec.frame.fmt != MR_PIX_YUV420P) {
                    fprintf(stderr, "FAIL: frame %d is not MR_PIX_YUV420P\n",
                            (int)n);
                    free(buf); return 0;
                }
                if (dec.frame.stride < w) {
                    fprintf(stderr, "FAIL: frame %d luma stride %d under the "
                                    "%d-pixel width\n", (int)n,
                            dec.frame.stride, w);
                    free(buf); return 0;
                }
                mr_yuv420_to_rgb24(buf + n * frame_bytes, w * 3,
                                   dec.frame.data, dec.frame.stride,
                                   dec.frame.u_data, dec.frame.u_stride,
                                   dec.frame.v_data, dec.frame.v_stride,
                                   w, h, NULL, NULL);
            } else {
                memcpy(buf + n * frame_bytes, dec.frame.data, frame_bytes);
            }
            n++;
            got = dec.drain && dec.drain(&dec) == MR_OK;
        }
    }
    while (mr_codec_mpeg2.flush && mr_codec_mpeg2.flush(&dec) == MR_OK) {
        if (!check_display_pts(&dec, (int)n, &last_pts, &have_last_pts,
                               &tagged_frames)) {
            free(buf); mr_codec_mpeg2.close(&dec); return 0;
        }
        if (n == cap) {
            unsigned char *p;
            cap *= 2;
            p = (unsigned char *)realloc(buf, cap * frame_bytes);
            if (!p) { free(buf); return 0; }
            buf = p;
        }
        if (yuv)
            mr_yuv420_to_rgb24(buf + n * frame_bytes, w * 3,
                               dec.frame.data, dec.frame.stride,
                               dec.frame.u_data, dec.frame.u_stride,
                               dec.frame.v_data, dec.frame.v_stride,
                               w, h, NULL, NULL);
        else
            memcpy(buf + n * frame_bytes, dec.frame.data, frame_bytes);
        n++;
    }
    if (tagged_frames < 2) {
        fprintf(stderr, "FAIL: only %d displayed MPEG frames carried PTS tags\n",
                tagged_frames);
        free(buf); mr_codec_mpeg2.close(&dec); return 0;
    }
    mr_codec_mpeg2.close(&dec);
    *out = buf;
    *frames = (int)n;
    return 1;
}

int main(int argc, char **argv)
{
    unsigned char *data, *rgb = NULL, *yuv = NULL;
    size_t len = 0;
    int w = 134, h = 100, nr = 0, ny = 0, i, rc = 0, worst_max = 0;
    double worst_mae = 0.0;

    if (argc < 2) {
        fprintf(stderr, "usage: mr_mpeg2_yuv_check <file.mpg> [w h]\n");
        return 2;
    }
    if (argc == 4) { w = atoi(argv[2]); h = atoi(argv[3]); }
    if (w % 16 == 0) {
        fprintf(stderr, "FAIL: %dx%d has a macroblock-aligned width, so this "
                        "clip cannot detect a plane-stride bug\n", w, h);
        return 1;
    }
    if (!(data = slurp(argv[1], &len))) {
        fprintf(stderr, "cannot read %s\n", argv[1]);
        return 2;
    }
    if (!decode_all(data, len, w, h, 0, &rgb, &nr) ||
        !decode_all(data, len, w, h, 1, &yuv, &ny)) {
        fprintf(stderr, "FAIL: decode failed\n");
        free(data); return 1;
    }
    if (nr != ny || nr == 0) {
        fprintf(stderr, "FAIL: %d RGB frames vs %d YUV frames\n", nr, ny);
        free(rgb); free(yuv); free(data); return 1;
    }
    for (i = 0; i < nr; i++) {
        long j, n = (long)w * h * 3, sum = 0;
        int maxerr = 0;
        double mae;
        const unsigned char *a = rgb + (size_t)i * n;
        const unsigned char *b = yuv + (size_t)i * n;
        for (j = 0; j < n; j++) {
            int d = (int)a[j] - (int)b[j];
            if (d < 0) d = -d;
            if (d > maxerr) maxerr = d;
            sum += d;
        }
        mae = (double)sum / (double)n;
        if (mae > worst_mae) worst_mae = mae;
        if (maxerr > worst_max) worst_max = maxerr;
    }
    printf("mpeg2 yuv output: %d frames at %dx%d, worst MAE=%.2f "
           "worst pixel=%d\n", nr, w, h, worst_mae, worst_max);
    if (worst_mae > MAE_LIMIT || worst_max > MAXERR_LIMIT) {
        fprintf(stderr, "FAIL: YUV and RGB outputs differ (MAE %.2f, worst "
                        "pixel %d; both must be 0 - the same converter over "
                        "the same pixels) - check the plane pointers, the "
                        "strides, and that u is Cb and v is Cr\n",
                worst_mae, worst_max);
        rc = 1;
    } else {
        printf("mpeg2 yuv output: OK\n");
    }
    free(rgb); free(yuv); free(data);
    return rc;
}
