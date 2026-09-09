/*
 * Verify mr_mpeg1_next_yuv() hands back the decoder's planes correctly.
 *
 * play_mpeg1() takes this route when the display backend accepts YUV420P
 * directly, skipping the RGB24 buffer entirely. Getting the planes wrong there
 * is silent in a way an RGB path is not: swapping u and v (MR_PIX_YUV420P is
 * Y, Cb, Cr; pl_mpeg names them cb and cr) only shifts the colour, and using
 * the visible width as the stride only skews the picture. Neither crashes.
 *
 * So decode the same clip twice - once through mr_mpeg1_next()'s RGB24
 * conversion, once through mr_mpeg1_next_yuv() plus MintVID's own shared
 * YUV->RGB converter - and require the two to agree closely, frame by frame.
 * The two converters are independent implementations with different rounding,
 * so a small mean error is expected and a large one means the planes, the
 * strides or the chroma order are wrong.
 *
 * The clip matters: it must be one whose width is not a multiple of 16, so the
 * macroblock-aligned plane pitch (144 for a 134-wide frame) differs from the
 * visible width. On a 128x96 clip the two are equal and a stride bug cannot
 * show at all.
 */
#include "../core/mr_mpeg1.h"
#include "../core/mr_yuv.h"

#include <stdio.h>
#include <stdlib.h>

/* Independent converters, so allow rounding differences but nothing more. */
#define MAE_LIMIT      3.0    /* mean abs error per channel byte, of 255 */
#define MAXERR_LIMIT   40     /* worst single channel byte              */

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
    unsigned char *data;
    size_t len = 0;
    mr_mpeg1 *rgb_dec, *yuv_dec;
    unsigned char *converted;
    mr_frame rgb_fr, yuv_fr;
    int w, h, frame = 0, rc = 0, worst_max = 0;
    double worst_mae = 0.0;

    if (argc != 2) {
        fprintf(stderr, "usage: mr_mpeg1_yuv_check <file.mpg>\n");
        return 2;
    }
    if (!(data = slurp(argv[1], &len))) {
        fprintf(stderr, "cannot read %s\n", argv[1]);
        return 2;
    }
    rgb_dec = mr_mpeg1_open(data, len, 0, 1, 0);
    yuv_dec = mr_mpeg1_open(data, len, 0, 1, 0);
    if (!rgb_dec || !yuv_dec) {
        fprintf(stderr, "cannot open %s as MPEG-1\n", argv[1]);
        free(data); return 2;
    }
    w = mr_mpeg1_width(rgb_dec);
    h = mr_mpeg1_height(rgb_dec);
    if (w % 16 == 0) {
        fprintf(stderr, "FAIL: %dx%d has a macroblock-aligned width, so this "
                        "clip cannot detect a plane-stride bug\n", w, h);
        mr_mpeg1_close(rgb_dec); mr_mpeg1_close(yuv_dec); free(data);
        return 1;
    }
    if (!(converted = (unsigned char *)malloc((size_t)w * h * 3))) {
        mr_mpeg1_close(rgb_dec); mr_mpeg1_close(yuv_dec); free(data);
        return 2;
    }

    while (mr_mpeg1_next(rgb_dec, &rgb_fr, NULL)) {
        long i, n = (long)w * h * 3, sum = 0;
        int maxerr = 0;
        double mae;

        if (!mr_mpeg1_next_yuv(yuv_dec, &yuv_fr, NULL)) {
            fprintf(stderr, "FAIL: YUV decode ended at frame %d, RGB did not\n",
                    frame);
            rc = 1; break;
        }
        if (yuv_fr.fmt != MR_PIX_YUV420P) {
            fprintf(stderr, "FAIL: frame %d is not MR_PIX_YUV420P\n", frame);
            rc = 1; break;
        }
        if (yuv_fr.width != w || yuv_fr.height != h) {
            fprintf(stderr, "FAIL: frame %d is %dx%d, expected %dx%d\n",
                    frame, yuv_fr.width, yuv_fr.height, w, h);
            rc = 1; break;
        }
        /* The pitch must be the aligned allocation width, not the visible one.
         * This is the check the 128x96 clip cannot make. */
        if (yuv_fr.stride < w) {
            fprintf(stderr, "FAIL: frame %d luma stride %d is under the %d-pixel "
                            "width\n", frame, yuv_fr.stride, w);
            rc = 1; break;
        }
        mr_yuv420_to_rgb24(converted, w * 3,
                           yuv_fr.data, yuv_fr.stride,
                           yuv_fr.u_data, yuv_fr.u_stride,
                           yuv_fr.v_data, yuv_fr.v_stride,
                           w, h, NULL, NULL);
        for (i = 0; i < n; i++) {
            int d = (int)converted[i] - (int)rgb_fr.data[i];
            if (d < 0) d = -d;
            if (d > maxerr) maxerr = d;
            sum += d;
        }
        mae = (double)sum / (double)n;
        if (mae > worst_mae) worst_mae = mae;
        if (maxerr > worst_max) worst_max = maxerr;
        frame++;
    }

    if (!rc && frame == 0) {
        fprintf(stderr, "FAIL: no frames decoded\n");
        rc = 1;
    }
    if (!rc) {
        printf("mpeg1 yuv planes: %d frames at %dx%d (luma stride %d), "
               "worst MAE=%.2f worst pixel=%d\n",
               frame, w, h, yuv_fr.stride, worst_mae, worst_max);
        if (worst_mae > MAE_LIMIT || worst_max > MAXERR_LIMIT) {
            fprintf(stderr, "FAIL: YUV and RGB paths disagree (MAE %.2f limit "
                            "%.2f, worst pixel %d limit %d) - check the plane "
                            "pointers, the strides, and that u is Cb and v is "
                            "Cr\n",
                    worst_mae, MAE_LIMIT, worst_max, MAXERR_LIMIT);
            rc = 1;
        } else {
            printf("mpeg1 yuv planes: OK\n");
        }
    }

    free(converted);
    mr_mpeg1_close(rgb_dec);
    mr_mpeg1_close(yuv_dec);
    free(data);
    return rc;
}
