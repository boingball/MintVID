/*
 * MintVID - 2x nearest-neighbour upscale.
 */
#include "mr_scale.h"

void mr_scale2x_rgb24(const uint8_t *src, int w, int h, int src_stride,
                      uint8_t *dst, int dst_stride)
{
    int x, y;
    for (y = 0; y < h; y++) {
        const uint8_t *sr = src + (size_t)y * src_stride;
        uint8_t       *d0 = dst + (size_t)(y * 2)     * dst_stride;
        uint8_t       *d1 = dst + (size_t)(y * 2 + 1) * dst_stride;
        for (x = 0; x < w; x++) {
            uint8_t r = sr[x * 3 + 0], g = sr[x * 3 + 1], b = sr[x * 3 + 2];
            int o = x * 6;
            d0[o+0]=r; d0[o+1]=g; d0[o+2]=b; d0[o+3]=r; d0[o+4]=g; d0[o+5]=b;
            d1[o+0]=r; d1[o+1]=g; d1[o+2]=b; d1[o+3]=r; d1[o+4]=g; d1[o+5]=b;
        }
    }
}

void mr_scale2x_u8(const uint8_t *src, int w, int h, int src_stride,
                   uint8_t *dst, int dst_stride)
{
    int x, y;
    for (y = 0; y < h; y++) {
        const uint8_t *sr = src + (size_t)y * src_stride;
        uint8_t       *d0 = dst + (size_t)(y * 2)     * dst_stride;
        uint8_t       *d1 = dst + (size_t)(y * 2 + 1) * dst_stride;
        for (x = 0; x < w; x++) {
            uint8_t v = sr[x];
            d0[x * 2] = v; d0[x * 2 + 1] = v;
            d1[x * 2] = v; d1[x * 2 + 1] = v;
        }
    }
}

void mr_scale2x_u8_horiz(const uint8_t *src, int w, int h, int src_stride,
                         uint8_t *dst, int dst_stride)
{
    int x, y;
    for (y = 0; y < h; y++) {
        const uint8_t *sr = src + (size_t)y * src_stride;
        uint8_t       *dr = dst + (size_t)y * dst_stride;
        for (x = 0; x < w; x++) {
            uint8_t v = sr[x];
            dr[x * 2] = v; dr[x * 2 + 1] = v;
        }
    }
}

void mr_scale_down_rgb24(const uint8_t *src, int w, int h, int src_stride,
                         uint8_t *dst, int dst_stride, int factor)
{
    int dw = w / factor, dh = h / factor;
    int area = factor * factor;
    int ox, oy, ix, iy;
    for (oy = 0; oy < dh; oy++) {
        uint8_t *dr = dst + (size_t)oy * dst_stride;
        for (ox = 0; ox < dw; ox++) {
            unsigned r = 0, g = 0, b = 0;
            for (iy = 0; iy < factor; iy++) {
                const uint8_t *sr = src + (size_t)(oy * factor + iy) * src_stride
                                        + (size_t)(ox * factor) * 3;
                for (ix = 0; ix < factor; ix++) {
                    r += sr[0]; g += sr[1]; b += sr[2];
                    sr += 3;
                }
            }
            dr[ox * 3 + 0] = (uint8_t)(r / area);
            dr[ox * 3 + 1] = (uint8_t)(g / area);
            dr[ox * 3 + 2] = (uint8_t)(b / area);
        }
    }
}

void mr_scale_fit_rect(int w, int h, int max_w, int max_h,
                       int *dst_w, int *dst_h)
{
    int dw = w, dh = h;

    if (w > 0 && h > 0 && max_w > 0 && max_h > 0 &&
        (w > max_w || h > max_h)) {
        /* Video dimensions are small enough that these products stay within
         * 32 bits; avoiding 64-bit division matters on a 68020/68030. */
        if ((uint32_t)w * (uint32_t)max_h >
            (uint32_t)max_w * (uint32_t)h) {
            dw = max_w;
            dh = (int)(((uint32_t)h * (uint32_t)max_w +
                        (uint32_t)w / 2) / (uint32_t)w);
        } else {
            dh = max_h;
            dw = (int)(((uint32_t)w * (uint32_t)max_h +
                        (uint32_t)h / 2) / (uint32_t)h);
        }
        if (dw < 1) dw = 1;
        if (dh < 1) dh = 1;
    }
    if (dst_w) *dst_w = dw;
    if (dst_h) *dst_h = dh;
}

void mr_scale_resize_rgb24_strip(const uint8_t *src, int w, int h,
                                 int src_stride, uint8_t *dst, int dst_w,
                                 int dst_h, int dst_stride, int y0, int rows)
{
    int y, sy, yq, yr, yerr;
    int xq, xr, sx0, xerr0;
    int i;

    if (!src || !dst || w <= 0 || h <= 0 || dst_w <= 0 || dst_h <= 0 ||
        y0 < 0 || y0 >= dst_h || rows <= 0)
        return;
    if (rows > dst_h - y0) rows = dst_h - y0;

    /*
     * Sample at destination pixel centres:
     *   source = floor(((2 * destination + 1) * source_size) /
     *                  (2 * destination_size))
     * Quotient/remainder stepping turns that into additions and an occasional
     * carry in the hot loop - the same DDA a full-image resize uses, just
     * fast-forwarded to start at row y0 instead of row 0.
     */
    xq = w / dst_w;
    xr = w % dst_w;
    sx0 = (w / 2) / dst_w;
    xerr0 = (w / 2) % dst_w;
    yq = h / dst_h;
    yr = h % dst_h;
    sy = (h / 2) / dst_h;
    yerr = (h / 2) % dst_h;

    /* Advancing the row DDA here is at most dst_h integer steps, once per
     * strip - negligible next to the O(dst_w) pixel work every row does
     * regardless, and it keeps the per-row state below self-contained rather
     * than threaded through a caller-owned struct across strip calls. */
    for (i = 0; i < y0; i++) {
        sy += yq;
        yerr += yr;
        if (yerr >= dst_h) {
            yerr -= dst_h;
            sy++;
        }
    }

    for (y = 0; y < rows; y++) {
        const uint8_t *sr = src + (size_t)sy * src_stride;
        uint8_t *dr = dst + (size_t)y * dst_stride;
        int x, sx = sx0, xerr = xerr0;

        for (x = 0; x < dst_w; x++) {
            const uint8_t *sp = sr + (size_t)sx * 3;
            dr[x * 3 + 0] = sp[0];
            dr[x * 3 + 1] = sp[1];
            dr[x * 3 + 2] = sp[2];
            sx += xq;
            xerr += xr;
            if (xerr >= dst_w) {
                xerr -= dst_w;
                sx++;
            }
        }

        sy += yq;
        yerr += yr;
        if (yerr >= dst_h) {
            yerr -= dst_h;
            sy++;
        }
    }
}

void mr_scale_resize_rgb24(const uint8_t *src, int w, int h, int src_stride,
                           uint8_t *dst, int dst_w, int dst_h, int dst_stride)
{
    mr_scale_resize_rgb24_strip(src, w, h, src_stride, dst, dst_w, dst_h,
                                dst_stride, 0, dst_h);
}
