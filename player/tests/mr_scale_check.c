#include "../core/mr_scale.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned next_value(unsigned *state)
{
    *state = *state * 1664525U + 1013904223U;
    return *state;
}

/* mr_scale_resize_rgb24_strip() must produce byte-identical output to the
 * corresponding rows of a full-image mr_scale_resize_rgb24() call - that
 * equivalence is what lets display_cgx.c blit a downscaled/upscaled frame a
 * bounded-height strip at a time instead of holding a second full-size
 * destination frame. Cover both directions (down- and upscale), a size where
 * dst_h does not divide evenly by the strip height, and strips that start
 * away from row 0 (so the row DDA's fast-forward is actually exercised). */
static int check_strip_matches_full(int w, int h, int dst_w, int dst_h,
                                    int strip_rows, unsigned seed)
{
    int src_stride = w * 3, dst_stride = dst_w * 3;
    uint8_t *src = (uint8_t *)malloc((size_t)src_stride * h);
    uint8_t *full = (uint8_t *)malloc((size_t)dst_stride * dst_h);
    uint8_t *strip = (uint8_t *)malloc((size_t)dst_stride * strip_rows);
    int y0, ok = 1;
    size_t i;
    if (!src || !full || !strip) { ok = 0; goto done; }
    for (i = 0; i < (size_t)src_stride * h; i++)
        src[i] = (uint8_t)(next_value(&seed) >> 24);
    memset(full, 0xa5, (size_t)dst_stride * dst_h);
    mr_scale_resize_rgb24(src, w, h, src_stride, full, dst_w, dst_h,
                          dst_stride);
    for (y0 = 0; y0 < dst_h; y0 += strip_rows) {
        int rows = dst_h - y0 < strip_rows ? dst_h - y0 : strip_rows;
        memset(strip, 0xa5, (size_t)dst_stride * strip_rows);
        mr_scale_resize_rgb24_strip(src, w, h, src_stride, strip, dst_w,
                                    dst_h, dst_stride, y0, rows);
        if (memcmp(strip, full + (size_t)y0 * dst_stride,
                   (size_t)dst_stride * rows) != 0) {
            fprintf(stderr,
                    "strip mismatch: %dx%d -> %dx%d, strip_rows=%d, y0=%d\n",
                    w, h, dst_w, dst_h, strip_rows, y0);
            ok = 0;
            break;
        }
    }

    /* Full and strip now share the optimized implementation, so comparing
     * them alone could miss identical sampling errors. Check actual source
     * pixels independently using the centre-sampling formula, including the
     * mapped 512-column tile boundary and the final row/column. */
    if (ok) {
        int k;
        for (k = 0; k < 128; k++) {
            int x = k == 0 ? 0 : k == 1 ? dst_w - 1 :
                    k == 2 ? (dst_w > 511 ? 511 : dst_w - 1) :
                    k == 3 ? (dst_w > 512 ? 512 : dst_w - 1) :
                             (int)(next_value(&seed) % (unsigned)dst_w);
            int y = k == 4 ? dst_h - 1 :
                             (int)(next_value(&seed) % (unsigned)dst_h);
            /* These test geometries have dimensions <=1920, so the
             * independent centre numerator fits in unsigned 32-bit. */
            unsigned sx = ((2u * (unsigned)x + 1u) * (unsigned)w) /
                          (2u * (unsigned)dst_w);
            unsigned sy = ((2u * (unsigned)y + 1u) * (unsigned)h) /
                          (2u * (unsigned)dst_h);
            if (memcmp(full + (size_t)y * dst_stride + (size_t)x * 3,
                       src + (size_t)sy * src_stride + (size_t)sx * 3,
                       3) != 0) {
                fprintf(stderr,
                        "centre-sample mismatch: %dx%d -> %dx%d at %d,%d\n",
                        w, h, dst_w, dst_h, x, y);
                ok = 0;
                break;
            }
        }
    }
done:
    free(src); free(full); free(strip);
    return ok;
}

static int check_fit(int w, int h, int max_w, int max_h,
                     int expected_w, int expected_h)
{
    int dw, dh;
    mr_scale_fit_rect(w, h, max_w, max_h, &dw, &dh);
    if (dw != expected_w || dh != expected_h) {
        fprintf(stderr, "fit %dx%d in %dx%d: got %dx%d, expected %dx%d\n",
                w, h, max_w, max_h, dw, dh, expected_w, expected_h);
        return 1;
    }
    return 0;
}

int main(void)
{
    static const uint8_t src[4 * 3] = {
        10, 11, 12,  20, 21, 22,  30, 31, 32,  40, 41, 42
    };
    uint8_t dst[2 * 3];
    int failed = 0;

    failed |= check_fit(854, 480, 320, 256, 320, 180);
    failed |= check_fit(854, 480, 640, 512, 640, 360);
    failed |= check_fit(320, 180, 320, 256, 320, 180);
    failed |= check_fit(240, 320, 320, 256, 192, 256);

    /* Centre sampling when reducing four pixels to two selects 1 and 3. */
    mr_scale_resize_rgb24(src, 4, 1, 4 * 3, dst, 2, 1, 2 * 3);
    if (memcmp(dst, src + 3, 3) || memcmp(dst + 3, src + 9, 3)) {
        fprintf(stderr, "nearest-neighbour centre sampling failed\n");
        failed = 1;
    }

    /* The 640x360 -> 1024x576 upscale from a real fullscreen RTG session,
     * plus a downscale, an odd (non-strip-aligned) height, and a tiny case,
     * each checked against several strip heights including ones that don't
     * evenly divide dst_h. */
    failed |= !check_strip_matches_full(640, 360, 1024, 576, 32, 1);
    failed |= !check_strip_matches_full(640, 360, 1024, 576, 17, 2);
    failed |= !check_strip_matches_full(1920, 1080, 640, 360, 32, 3);
    failed |= !check_strip_matches_full(853, 481, 640, 361, 40, 4);
    failed |= !check_strip_matches_full(7, 5, 3, 11, 4, 5);
    failed |= !check_strip_matches_full(1280, 720, 1002, 564, 64, 6);
    failed |= !check_strip_matches_full(1026, 516, 513, 516, 64, 7);
    failed |= !check_strip_matches_full(1920, 1080, 640, 360, 64, 8);

    if (!failed) puts("scale checks passed");
    return failed;
}