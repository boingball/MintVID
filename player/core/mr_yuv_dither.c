/*
 * MintVID - direct YUV420P -> indexed conversion (see the header for
 * the geometry contract this relies on).
 *
 * This is a straight algebraic fusion of two existing, independently
 * validated pipelines - core/mr_yuv.c's mr_yuv420_to_rgb24() and
 * core/mr_dither.c's mr_dither_rgb8() - with mr_scale_resize_rgb24()'s
 * nearest-neighbour row/column selection folded in as a source-index
 * computation instead of an intermediate buffer pass. Deliberately does NOT
 * reuse those files' own lookup tables (both are file-static/private) -
 * this rebuilds equivalent tables locally from the same published formulas,
 * so tests/mr_yuv_dither_check.c can compare this function's output against
 * the real three-stage composition (mr_yuv420_to_rgb24 + mr_scale_resize_rgb24
 * + mr_dither_rgb8, called directly) as an independent check that neither
 * copy of the formulas drifted from the other.
 */
#include "mr_yuv_dither.h"

#if defined(MR_M68K_ASM)
/* core/mr_yuv_dither_m68k.S - hand-written m68k version of
 * mr_yuv420_dither8 below, unrolled by pixel pairs (matching the chroma
 * subsampling) with the per-row Bayer tap table and per-pair red/green/
 * blue_add precomputed exactly as the C does - see that file's header. */
void mr_yuv420_dither8_m68k(const uint8_t *y_plane, int y_stride,
                            const uint8_t *u_plane, int u_stride,
                            const uint8_t *v_plane, int v_stride,
                            int width, int dst_h, int vscale,
                            uint8_t *out, int out_stride, int y_base,
                            const int *luma_x298, const int *e_x409,
                            const int *d_xm100, const int *e_xm208,
                            const int *d_x516, const uint8_t *lut_r,
                            const uint8_t *lut_g, const uint8_t *lut_b)
    __asm__("mr_yuv420_dither8_m68k");
#endif

/* ---- YCbCr -> RGB tables (mirrors core/mr_yuv.c's build_tables()) ---- */
static int g_luma_x298[256];
static int g_e_x409[256];
static int g_d_xm100[256];
static int g_e_xm208[256];
static int g_d_x516[256];
static int g_yuv_tables_ready = 0;

static void build_yuv_tables(void)
{
    int i;
    for (i = 0; i < 256; i++) {
        int d = i - 128, e = i - 128;
        g_luma_x298[i] = 298 * i;
        g_e_x409[i] = 409 * e;
        g_d_xm100[i] = -100 * d;
        g_e_xm208[i] = -208 * e;
        g_d_x516[i] = 516 * d;
    }
    g_yuv_tables_ready = 1;
}

static uint8_t clip8(int value)
{
    if (value < 0) return 0;
    if (value > 255) return 255;
    return (uint8_t)value;
}

/* ---- RGB -> 8-bit dither tables (mirrors core/mr_dither.c's build_lut()) */
static const uint8_t bayer4[4][4] = {
    {  0,  8,  2, 10 },
    { 12,  4, 14,  6 },
    {  3, 11,  1,  9 },
    { 15,  7, 13,  5 }
};
static uint8_t lut_r[16][256], lut_g[16][256], lut_b[16][256];
static int     dither_lut_depth = 0;

static void indexed_shape(int depth, int *rl, int *gl, int *bl)
{
    if (depth == 4) { *rl = 2; *gl = 4; *bl = 2; }
    else if (depth == 5) { *rl = 4; *gl = 4; *bl = 2; }
    else { *rl = 6; *gl = 6; *bl = 6; }
}

static void build_dither_lut(int depth)
{
    int rl, gl, bl, t, v;
    indexed_shape(depth, &rl, &gl, &bl);
    for (t = 0; t < 16; t++)
        for (v = 0; v < 256; v++) {
            int rv = v + (t - 8) * (255 / (rl - 1)) / 16;
            int gv = v + (t - 8) * (255 / (gl - 1)) / 16;
            int bv = v + (t - 8) * (255 / (bl - 1)) / 16;
            int rq, gq, bq;
            if (rv < 0) rv = 0; else if (rv > 255) rv = 255;
            if (gv < 0) gv = 0; else if (gv > 255) gv = 255;
            if (bv < 0) bv = 0; else if (bv > 255) bv = 255;
            rq = (rv * (rl - 1) + 127) / 255;
            gq = (gv * (gl - 1) + 127) / 255;
            bq = (bv * (bl - 1) + 127) / 255;
            lut_r[t][v] = (uint8_t)(rq * gl * bl);
            lut_g[t][v] = (uint8_t)(gq * bl);
            lut_b[t][v] = (uint8_t)bq;
        }
    dither_lut_depth = depth;
}

#if defined(MR_M68K_ASM)
/* Tables for the 6x6x6 kernel in mr_yuv_dither_m68k.S (see its comment for
 * the layout). Named with __asm__ so the assembly can reference them without
 * the AmigaOS underscore prefix. */
#define Q6_SUMS 793                     /* channel sums -258..534 */
int32_t mr_yuv_dither_q6_block[0x600] __asm__("mr_yuv_dither_q6_block");
uint8_t mr_yuv_dither_q6_tab[16 * Q6_SUMS * 4]
    __asm__("mr_yuv_dither_q6_tab");
static int g_q6_ready = 0;

static void build_q6_tables(void)
{
    int i, t, s;
    for (i = 0; i < 256; i++) {
        int luma = i - 16 < 0 ? 0 : i - 16;
        mr_yuv_dither_q6_block[i] = g_luma_x298[luma] + 128;
        mr_yuv_dither_q6_block[0x200 + 2 * i] = g_e_x409[i];
        mr_yuv_dither_q6_block[0x200 + 2 * i + 1] = g_e_xm208[i];
        mr_yuv_dither_q6_block[0x400 + 2 * i] = g_d_x516[i];
        mr_yuv_dither_q6_block[0x400 + 2 * i + 1] = g_d_xm100[i];
    }
    /* Same clip, dither and quantise as the C loop below, via lut_b: for
     * depth 8 it is the unweighted 6-level quantiser (lut_r/lut_g are the
     * same level times 36 and 6). */
    for (t = 0; t < 16; t++)
        for (s = -258; s <= 534; s++) {
            uint8_t *e = &mr_yuv_dither_q6_tab[(t * Q6_SUMS + s + 258) * 4];
            unsigned c = clip8(s);
            e[0] = lut_r[t][c];
            e[1] = lut_g[t][c];
            e[2] = lut_b[t][c];
            e[3] = 0;
        }
    g_q6_ready = 1;
}
#endif

void mr_yuv420_dither_indexed(const uint8_t *y_plane, int y_stride,
                              const uint8_t *u_plane, int u_stride,
                              const uint8_t *v_plane, int v_stride,
                              int width, int height, int vscale, int depth,
                              uint8_t *out, int out_stride, int y_base)
{
    int dst_h, oy;
    if (!y_plane || !u_plane || !v_plane || !out ||
        width <= 0 || height <= 0 || vscale <= 0)
        return;
    if (depth != 4 && depth != 5) depth = 8;
    if (!g_yuv_tables_ready) build_yuv_tables();
    if (dither_lut_depth != depth) build_dither_lut(depth);

    dst_h = height / vscale;
#if defined(MR_M68K_ASM)
    if (depth == 8 && !g_q6_ready) build_q6_tables();
    mr_yuv420_dither8_m68k(y_plane, y_stride, u_plane, u_stride, v_plane,
                           v_stride, width, dst_h, vscale, out, out_stride,
                           y_base, g_luma_x298, g_e_x409, g_d_xm100,
                           g_e_xm208, g_d_x516, &lut_r[0][0], &lut_g[0][0],
                           &lut_b[0][0]);
    return;
#endif
    for (oy = 0; oy < dst_h; oy++) {
        int src_row = vscale * oy + vscale / 2;
        int chroma_row = src_row >> 1;
        const uint8_t *src_y = y_plane + (size_t)src_row * y_stride;
        const uint8_t *src_u = u_plane + (size_t)chroma_row * u_stride;
        const uint8_t *src_v = v_plane + (size_t)chroma_row * v_stride;
        uint8_t *dr = out + (size_t)oy * out_stride;
        const uint8_t *threshold = bayer4[(y_base + oy) & 3];
        int x;

        for (x = 0; x + 1 < width; x += 2) {
            unsigned uu = src_u[x >> 1], vv = src_v[x >> 1];
            int red_add = g_e_x409[vv] + 128;
            int green_add = g_d_xm100[uu] + g_e_xm208[vv] + 128;
            int blue_add = g_d_x516[uu] + 128;
            int luma0 = (int)src_y[x] - 16;
            int luma1 = (int)src_y[x + 1] - 16;
            int t0 = threshold[x & 3], t1 = threshold[(x + 1) & 3];
            int scaled_y0, scaled_y1, r, g, b;

            if (luma0 < 0) luma0 = 0;
            scaled_y0 = g_luma_x298[luma0];
            r = clip8((scaled_y0 + red_add) >> 8);
            g = clip8((scaled_y0 + green_add) >> 8);
            b = clip8((scaled_y0 + blue_add) >> 8);
            dr[x] = (uint8_t)(lut_r[t0][r] + lut_g[t0][g] + lut_b[t0][b]);

            if (luma1 < 0) luma1 = 0;
            scaled_y1 = g_luma_x298[luma1];
            r = clip8((scaled_y1 + red_add) >> 8);
            g = clip8((scaled_y1 + green_add) >> 8);
            b = clip8((scaled_y1 + blue_add) >> 8);
            dr[x + 1] = (uint8_t)(lut_r[t1][r] + lut_g[t1][g] + lut_b[t1][b]);
        }
        if (x < width) {
            unsigned uu = src_u[x >> 1], vv = src_v[x >> 1];
            int red_add = g_e_x409[vv] + 128;
            int green_add = g_d_xm100[uu] + g_e_xm208[vv] + 128;
            int blue_add = g_d_x516[uu] + 128;
            int luma0 = (int)src_y[x] - 16;
            int t0 = threshold[x & 3];
            int scaled_y0, r, g, b;

            if (luma0 < 0) luma0 = 0;
            scaled_y0 = g_luma_x298[luma0];
            r = clip8((scaled_y0 + red_add) >> 8);
            g = clip8((scaled_y0 + green_add) >> 8);
            b = clip8((scaled_y0 + blue_add) >> 8);
            dr[x] = (uint8_t)(lut_r[t0][r] + lut_g[t0][g] + lut_b[t0][b]);
        }
    }
}

void mr_yuv420_dither8(const uint8_t *y_plane, int y_stride,
                       const uint8_t *u_plane, int u_stride,
                       const uint8_t *v_plane, int v_stride,
                       int width, int height, int vscale,
                       uint8_t *out, int out_stride, int y_base)
{
    mr_yuv420_dither_indexed(y_plane, y_stride, u_plane, u_stride, v_plane,
                             v_stride, width, height, vscale, 8, out,
                             out_stride, y_base);
}

void mr_yuv420_dither_indexed_resize(const uint8_t *y_plane, int y_stride,
                                     const uint8_t *u_plane, int u_stride,
                                     const uint8_t *v_plane, int v_stride,
                                     int width, int height, int depth,
                                     uint8_t *out, int dst_w, int dst_h,
                                     int out_stride, int y_base)
{
    int xq, xr, sx0, xerr0, yq, yr, sy, yerr, oy;
    if (!y_plane || !u_plane || !v_plane || !out ||
        width <= 0 || height <= 0 || dst_w <= 0 || dst_h <= 0)
        return;
    if (depth != 4 && depth != 5) depth = 8;
    if (!g_yuv_tables_ready) build_yuv_tables();
    if (dither_lut_depth != depth) build_dither_lut(depth);

    xq = width / dst_w; xr = width % dst_w;
    sx0 = (width / 2) / dst_w; xerr0 = (width / 2) % dst_w;
    yq = height / dst_h; yr = height % dst_h;
    sy = (height / 2) / dst_h; yerr = (height / 2) % dst_h;

    for (oy = 0; oy < dst_h; oy++) {
        const uint8_t *src_y = y_plane + (size_t)sy * y_stride;
        const uint8_t *src_u = u_plane + (size_t)(sy >> 1) * u_stride;
        const uint8_t *src_v = v_plane + (size_t)(sy >> 1) * v_stride;
        uint8_t *dr = out + (size_t)oy * out_stride;
        const uint8_t *threshold = bayer4[(y_base + oy) & 3];
        int ox, sx = sx0, xerr = xerr0, cached_sx = -1;
        int r = 0, g = 0, b = 0;

        for (ox = 0; ox < dst_w; ox++) {
            int t = threshold[ox & 3];
            /* Nearest-neighbour upscales repeat each source pixel. Convert a
             * repeated pixel only once, but retain the destination-specific
             * Bayer threshold so output stays bit-exact. The common 256->640
             * AGA fit therefore performs 256 YUV->RGB conversions per row,
             * not 640. Row bases are also hoisted out of the pixel loop. */
            if (sx != cached_sx) {
                unsigned uu = src_u[sx >> 1], vv = src_v[sx >> 1];
                int red_add = g_e_x409[vv] + 128;
                int green_add = g_d_xm100[uu] + g_e_xm208[vv] + 128;
                int blue_add = g_d_x516[uu] + 128;
                int luma = (int)src_y[sx] - 16;
                int scaled_y;
                if (luma < 0) luma = 0;
                scaled_y = g_luma_x298[luma];
                r = clip8((scaled_y + red_add) >> 8);
                g = clip8((scaled_y + green_add) >> 8);
                b = clip8((scaled_y + blue_add) >> 8);
                cached_sx = sx;
            }
            dr[ox] = (uint8_t)(lut_r[t][r] + lut_g[t][g] + lut_b[t][b]);
            sx += xq;
            xerr += xr;
            if (xerr >= dst_w) { xerr -= dst_w; sx++; }
        }

        sy += yq;
        yerr += yr;
        if (yerr >= dst_h) { yerr -= dst_h; sy++; }
    }
}

void mr_yuv420_dither8_resize(const uint8_t *y_plane, int y_stride,
                              const uint8_t *u_plane, int u_stride,
                              const uint8_t *v_plane, int v_stride,
                              int width, int height,
                              uint8_t *out, int dst_w, int dst_h,
                              int out_stride, int y_base)
{
    mr_yuv420_dither_indexed_resize(y_plane, y_stride, u_plane, u_stride,
                                    v_plane, v_stride, width, height, 8, out,
                                    dst_w, dst_h, out_stride, y_base);
}
