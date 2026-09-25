/*
 * MintVID - check for the RGB565 output path used by 16-bit Picasso96
 * screens: mr_yuv420_to_rgb565()/_half() must equal the existing RGB24
 * converters' output packed to RGB565, and mr_scale_resize_u16_strip() must
 * pick the same source pixels as mr_scale_resize_rgb24_strip(). Covers odd
 * sizes, padded strides (padding must stay untouched), random and extreme
 * samples, and every U x V pair at the Y extremes (the clip tables' ends).
 */
#include "../core/mr_yuv.h"
#include "../core/mr_scale.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned state;
static uint8_t rnd8(void)
{
    state = state * 1664525U + 1013904223U;
    return (uint8_t)(state >> 24);
}

static uint16_t pack(const uint8_t *p)
{
    return (uint16_t)(((p[0] & 0xF8u) << 8) | ((p[1] & 0xFCu) << 3) |
                      (p[2] >> 3));
}

/* dst565 rows vs rgb rows packed; bytes past w*2 in each row must still be
 * the 0xA5 fill. */
static int compare(const uint8_t *rgb, int rgb_stride, const uint8_t *d565,
                   int d_stride, int w, int h, const char *what)
{
    int x, y;
    for (y = 0; y < h; y++) {
        const uint16_t *row = (const uint16_t *)(const void *)
                              (d565 + (size_t)y * d_stride);
        for (x = 0; x < w; x++) {
            uint16_t want = pack(rgb + (size_t)y * rgb_stride + x * 3);
            if (row[x] != want) {
                fprintf(stderr, "%s: %dx%d pixel %d,%d got %04x want %04x\n",
                        what, w, h, x, y, row[x], want);
                return 0;
            }
        }
        for (x = w * 2; x < d_stride; x++)
            if (d565[(size_t)y * d_stride + x] != 0xA5) {
                fprintf(stderr, "%s: %dx%d wrote padding at row %d\n",
                        what, w, h, y);
                return 0;
            }
    }
    return 1;
}

static int run_convert(int w, int h, int mode, unsigned seed)
{
    int cw = (w + 1) / 2, ch = (h + 1) / 2;
    int ys = w + 5, cs = cw + 3;
    int ow = w, oh = h, ds, rs, ok = 1, half;
    uint8_t *yp = malloc((size_t)ys * h), *up = malloc((size_t)cs * ch);
    uint8_t *vp = malloc((size_t)cs * ch);
    uint8_t *rgb, *d;
    int i;

    state = seed;
    for (i = 0; i < ys * h; i++)
        yp[i] = mode == 1 ? ((i & 1) ? 255 : 0) : rnd8();
    for (i = 0; i < cs * ch; i++) {
        up[i] = mode == 1 ? ((i & 2) ? 255 : 0) : rnd8();
        vp[i] = mode == 1 ? ((i & 1) ? 0 : 255) : rnd8();
    }
    for (half = 0; half < 2 && ok; half++) {
        if (half) {
            if (w < 2 || h < 2) break;
            ow = w / 2; oh = h / 2;
        }
        rs = ow * 3 + 7;
        ds = ow * 2 + 6;
        rgb = malloc((size_t)rs * oh);
        d = malloc((size_t)ds * oh);
        memset(d, 0xA5, (size_t)ds * oh);
        if (half) {
            mr_yuv420_to_rgb24_half(rgb, rs, yp, ys, up, cs, vp, cs, w, h,
                                    NULL, NULL);
            mr_yuv420_to_rgb565_half(d, ds, yp, ys, up, cs, vp, cs, w, h,
                                     NULL, NULL);
        } else {
            mr_yuv420_to_rgb24(rgb, rs, yp, ys, up, cs, vp, cs, w, h,
                               NULL, NULL);
            mr_yuv420_to_rgb565(d, ds, yp, ys, up, cs, vp, cs, w, h,
                                NULL, NULL);
        }
        ok = compare(rgb, rs, d, ds, ow, oh, half ? "rgb565_half" : "rgb565");
        free(rgb); free(d);
    }
    free(yp); free(up); free(vp);
    return ok;
}

/* Every U x V pair at Y = 0, 16, 235 and 255: all clip-table ends. */
static int run_extremes(void)
{
    static const uint8_t ys[] = { 0, 16, 235, 255 };
    enum { W = 512, H = 2 };
    static uint8_t yp[W * H], up[W / 2], vp[W / 2], rgb[W * H * 3];
    static uint8_t d[W * H * 2];
    unsigned k, u, v;
    for (k = 0; k < sizeof ys; k++) {
        memset(yp, ys[k], sizeof yp);
        for (u = 0; u < 256; u++) {
            for (v = 0; v < 256; v++) {
                up[v] = (uint8_t)u;
                vp[v] = (uint8_t)v;
            }
            mr_yuv420_to_rgb24(rgb, W * 3, yp, W, up, W / 2, vp, W / 2, W, H,
                               NULL, NULL);
            mr_yuv420_to_rgb565(d, W * 2, yp, W, up, W / 2, vp, W / 2, W, H,
                                NULL, NULL);
            if (!compare(rgb, W * 3, d, W * 2, W, H, "rgb565 extremes"))
                return 0;
        }
    }
    return 1;
}

static int run_scale(int sw, int sh, int dw, int dh, unsigned seed)
{
    int ss24 = sw * 3 + 5, ss16 = sw * 2 + 4, ds24 = dw * 3, ds16 = dw * 2 + 2;
    uint8_t *s24 = malloc((size_t)ss24 * sh), *s16 = malloc((size_t)ss16 * sh);
    uint8_t *d24 = malloc((size_t)ds24 * 32), *d16 = malloc((size_t)ds16 * 32);
    int x, y, y0, ok = 1;

    state = seed;
    for (y = 0; y < sh; y++)
        for (x = 0; x < sw; x++) {
            uint8_t *p = s24 + (size_t)y * ss24 + x * 3;
            p[0] = rnd8(); p[1] = rnd8(); p[2] = rnd8();
            *(uint16_t *)(void *)(s16 + (size_t)y * ss16 + x * 2) = pack(p);
        }
    /* In 32-row strips, as the P96 backend calls it. */
    for (y0 = 0; y0 < dh && ok; y0 += 32) {
        int rows = dh - y0 < 32 ? dh - y0 : 32;
        memset(d16, 0xA5, (size_t)ds16 * 32);
        mr_scale_resize_rgb24_strip(s24, sw, sh, ss24, d24, dw, dh, ds24,
                                    y0, rows);
        mr_scale_resize_u16_strip(s16, sw, sh, ss16, d16, dw, dh, ds16,
                                  y0, rows);
        ok = compare(d24, ds24, d16, ds16, dw, rows, "u16 scale");
    }
    free(s24); free(s16); free(d24); free(d16);
    return ok;
}

int main(void)
{
    static const int sizes[] = { 1, 2, 3, 7, 16, 17, 33 };
    static const int scales[][4] = {
        { 1280, 720, 1024, 576 }, { 1280, 720, 640, 360 },
        { 640, 360, 1024, 576 }, { 320, 240, 320, 240 },
        { 17, 13, 31, 19 }, { 31, 19, 17, 13 }, { 2, 2, 5, 5 }
    };
    unsigned i, j, mode;
    for (mode = 0; mode < 2; mode++)
        for (i = 0; i < sizeof sizes / sizeof sizes[0]; i++)
            for (j = 0; j < sizeof sizes / sizeof sizes[0]; j++)
                if (!run_convert(sizes[i], sizes[j], (int)mode,
                                 0x35363521U + i * 37 + j * 5 + mode))
                    return 1;
    if (!run_convert(640, 360, 0, 0x2d2d2d2dU)) return 1;
    if (!run_extremes()) return 1;
    for (i = 0; i < sizeof scales / sizeof scales[0]; i++)
        if (!run_scale(scales[i][0], scales[i][1], scales[i][2], scales[i][3],
                       0x5343414cU + i))
            return 1;
    printf("RGB565 conversion and 16-bit scaling match the RGB24 paths\n");
    return 0;
}
