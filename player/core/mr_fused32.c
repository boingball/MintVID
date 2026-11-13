/* MintVID - Fused32: YUV420 or RGB24 to the five-plane AGA layout. */
#include <stdlib.h>
#include <string.h>
#include "mr_dither.h"
#include "mr_c2p.h"
#include "mr_yuv_dither.h"

/* Nearest-neighbour scale, dither and C2P one output row at a time, so the
 * result equals mr_scale_resize_rgb24 + mr_dither_rgb_indexed + mr_c2p8 (see
 * tests/mr_planar_modes_check.c). The source column of each output pixel is
 * worked out once per call with a DDA, never with a per-pixel divide: on m68k
 * a 64-bit divide is a libgcc call, and the old per-pixel version cost ten
 * times the ECS32 path. */
void mr_rgb_fused32(const uint8_t *rgb, int w, int h, int stride,
                    uint8_t *out, int dw, int dh, int pw)
{
    static uint8_t *row_rgb, *row_idx;
    static uint32_t *sx_off;
    static int cap_dw, cap_pw;
    size_t plane_size = (size_t)(pw / 8) * dh;
    uint8_t *planes[5];
    int x, y, p, left = (pw - dw) / 2;
    int sx, xerr, sy, yerr;
    if (!rgb || !out || w <= 0 || h <= 0 || dw <= 0 || dh <= 0 ||
        pw < dw || (pw & 31)) return;
    if (dw > cap_dw) {
        uint8_t *nr = (uint8_t *)realloc(row_rgb, (size_t)dw * 3);
        uint32_t *ns;
        if (!nr) return;
        row_rgb = nr;
        ns = (uint32_t *)realloc(sx_off, (size_t)dw * sizeof(*ns));
        if (!ns) return;
        sx_off = ns;
        cap_dw = dw;
    }
    if (pw > cap_pw) {
        uint8_t *ni = (uint8_t *)realloc(row_idx, (size_t)pw);
        if (!ni) return;
        row_idx = ni;
        cap_pw = pw;
    }
    /* source = floor(((2 * d + 1) * size) / (2 * dsize)), stepped. */
    sx = (w / 2) / dw; xerr = (w / 2) % dw;
    for (x = 0; x < dw; x++) {
        sx_off[x] = (uint32_t)sx * 3;
        sx += w / dw; xerr += w % dw;
        if (xerr >= dw) { xerr -= dw; sx++; }
    }
    memset(row_idx, 0, (size_t)pw);
    for (p = 0; p < 5; p++) planes[p] = out + (size_t)p * plane_size;
    sy = (h / 2) / dh; yerr = (h / 2) % dh;
    for (y = 0; y < dh; y++) {
        const uint8_t *sr = rgb + (size_t)sy * stride;
        if (dw != w) {
            uint8_t *d = row_rgb;
            for (x = 0; x < dw; x++, d += 3) {
                const uint8_t *c = sr + sx_off[x];
                d[0] = c[0]; d[1] = c[1]; d[2] = c[2];
            }
            sr = row_rgb;
        }
        mr_dither_rgb_indexed(sr, dw, 1, dw * 3, row_idx + left, pw, y, 5);
        mr_c2p8_riva32(row_idx, pw, 1, pw, 5, planes, pw / 8, 0, y);
        sy += h / dh; yerr += h % dh;
        if (yerr >= dh) { yerr -= dh; sy++; }
    }
}

/* YUV input. The hand kernel covers 1:1 widths that are a multiple of 32.
 * Every other shape used to run mr_yuv420_fused32_c(), which packs five
 * plane bits per pixel and measured about twice the ECS32 path's cost. The
 * same result comes from ECS32's own resize-dither into a chunky frame
 * followed by the 32-pixel C2P, so do that. The portable fused function
 * stays as the reference, and as the fallback if the frame cannot be
 * allocated. */
void mr_yuv420_fused32(const uint8_t *y, int ys, const uint8_t *u, int us,
                       const uint8_t *v, int vs, int w, int h,
                       uint8_t *out, int dw, int dh, int pw, int y_base)
{
    static uint8_t *chunky;
    static size_t cap;
    static int last_pw, last_dw, last_dh;
    uint8_t *planes[5];
    size_t need = (size_t)pw * (dh > 0 ? dh : 0);
    int p;
    if (mr_yuv420_fused32_asm(y, ys, u, us, v, vs, w, h, out, dw, dh, pw, y_base))
        return;
    if (!y || !u || !v || !out || w <= 0 || h <= 0 || dw <= 0 || dh <= 0 ||
        pw < dw || (pw & 31))
        return;
    if (need > cap) {
        uint8_t *n = (uint8_t *)realloc(chunky, need);
        if (!n) {
            mr_yuv420_fused32_c(y, ys, u, us, v, vs, w, h, out, dw, dh, pw, y_base);
            return;
        }
        chunky = n;
        cap = need;
        last_pw = 0;
    }
    if (pw != last_pw || dw != last_dw || dh != last_dh) {
        memset(chunky, 0, need);     /* the side margins stay zero */
        last_pw = pw; last_dw = dw; last_dh = dh;
    }
    if (dw == w && dh == h)          /* 1:1: the table-driven kernel */
        mr_yuv420_dither_indexed(y, ys, u, us, v, vs, w, h, 1, 5,
                                 chunky + (pw - dw) / 2, pw, y_base);
    else
        mr_yuv420_dither_indexed_resize(y, ys, u, us, v, vs, w, h, 5,
                                        chunky + (pw - dw) / 2, dw, dh, pw,
                                        y_base);
    for (p = 0; p < 5; p++) planes[p] = out + (size_t)p * (pw / 8) * dh;
    mr_c2p8_riva32(chunky, pw, dh, pw, 5, planes, pw / 8, 0, 0);
}
