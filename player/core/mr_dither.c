/* MintVID - RGB24 -> native indexed palette conversion. */
#include "mr_dither.h"

#if defined(MR_M68K_ASM)
/* The assembly is LUT-generic: despite its historical rgb8 name, the output
 * depth is entirely encoded in the three tables passed to it. The same loop
 * therefore accelerates 4-, 5- and 8-plane modes byte-for-byte. */
void mr_dither_rgb8_m68k(const uint8_t *rgb, int w, int h, int rgb_stride,
                         uint8_t *out, int out_stride, int y_base,
                         const uint8_t *lut_r, const uint8_t *lut_g,
                         const uint8_t *lut_b)
    __asm__("mr_dither_rgb8_m68k");
#endif

/* Normalised 4x4 Bayer matrix, values 0..15. */
static const uint8_t bayer4[4][4] = {
    {  0,  8,  2, 10 },
    { 12,  4, 14,  6 },
    {  3, 11,  1,  9 },
    { 15,  7, 13,  5 }
};

static void indexed_shape(int depth, int *rl, int *gl, int *bl)
{
    if (depth == 4) { *rl = 2; *gl = 4; *bl = 2; }
    else if (depth == 5) { *rl = 4; *gl = 4; *bl = 2; }
    else { *rl = 6; *gl = 6; *bl = 6; }
}

void mr_dither_palette_indexed(uint8_t *pal, int depth)
{
    int rl, gl, bl, r, g, b, i = 0;
    indexed_shape(depth, &rl, &gl, &bl);
    for (r = 0; r < rl; r++)
        for (g = 0; g < gl; g++)
            for (b = 0; b < bl; b++) {
                pal[i * 3 + 0] = (uint8_t)(r * 255 / (rl - 1));
                pal[i * 3 + 1] = (uint8_t)(g * 255 / (gl - 1));
                pal[i * 3 + 2] = (uint8_t)(b * 255 / (bl - 1));
                i++;
            }
    for (; i < 256; i++)
        pal[i * 3 + 0] = pal[i * 3 + 1] = pal[i * 3 + 2] = 0;
}

void mr_dither_palette(uint8_t *pal)
{
    mr_dither_palette_indexed(pal, 8);
}

/* Per-(threshold,value) tables giving each channel's pre-weighted cube
 * contribution, so the hot loop is three lookups + two adds - no per-pixel
 * multiply or divide (both are very slow on a 68030). Built once; the values
 * are identical to the old quant6()*weight arithmetic. */
static uint8_t lut_r[16][256], lut_g[16][256], lut_b[16][256];
static int     lut_depth = 0;

static void build_lut(int depth)
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
    lut_depth = depth;
}

uint8_t mr_dither_rgb_indexed_pixel(uint8_t r, uint8_t g, uint8_t b,
                                    int x, int y, int depth)
{
    int t;
    if (depth != 4 && depth != 5) depth = 8;
    if (lut_depth != depth) build_lut(depth);
    t = bayer4[y & 3][x & 3];
    return (uint8_t)(lut_r[t][r] + lut_g[t][g] + lut_b[t][b]);
}

void mr_dither_rgb_indexed(const uint8_t *rgb, int w, int h, int rgb_stride,
                           uint8_t *out, int out_stride, int y_base,
                           int depth)
{
    int x, y;
    if (depth != 4 && depth != 5) depth = 8;
    if (lut_depth != depth) build_lut(depth);
#if defined(MR_M68K_ASM)
    mr_dither_rgb8_m68k(rgb, w, h, rgb_stride, out, out_stride, y_base,
                        &lut_r[0][0], &lut_g[0][0], &lut_b[0][0]);
    return;
#endif
    for (y = 0; y < h; y++) {
        const uint8_t *sr = rgb + (size_t)y * rgb_stride;
        uint8_t       *dr = out + (size_t)y * out_stride;
        const uint8_t *br = bayer4[(y_base + y) & 3];
        for (x = 0; x < w; x++) {
            const uint8_t *p = sr + x * 3;
            int t = br[x & 3];
            dr[x] = (uint8_t)(lut_r[t][p[0]] + lut_g[t][p[1]] + lut_b[t][p[2]]);
        }
    }
}

void mr_dither_rgb8(const uint8_t *rgb, int w, int h, int rgb_stride,
                    uint8_t *out, int out_stride, int y_base)
{
    mr_dither_rgb_indexed(rgb, w, h, rgb_stride, out, out_stride, y_base, 8);
}

/* Extra Half-Brite: 32-entry base cube, same 4x4x2 shape as the depth-5
 * ECS/OCS cube (indexed_shape(5,...)), inlined directly rather than routed
 * through mr_dither_palette_indexed() since that function's own contract
 * assumes and zero-fills a 256-entry buffer. */
void mr_dither_palette_ehb(uint8_t *pal32)
{
    int r, g, b, i = 0;
    for (r = 0; r < 4; r++)
        for (g = 0; g < 4; g++)
            for (b = 0; b < 2; b++) {
                pal32[i*3+0] = (uint8_t)(r * 255 / 3);
                pal32[i*3+1] = (uint8_t)(g * 255 / 3);
                pal32[i*3+2] = (uint8_t)(b * 255 / 1);
                i++;
            }
}

/*
 * The 64 colours an EHB screen can actually show split into two
 * independent, separable RGB cubes: the 32 real registers, and the same 32
 * values halved by the hardware for free. That separability is the whole
 * optimisation - for an axis-aligned Cartesian-product grid, the point
 * nearest any target is always the one found by quantising each channel
 * independently (there are no cross-channel terms to trade off), so the
 * true nearest-of-64 is exactly min(nearest-in-bright-cube,
 * nearest-in-half-cube). Each of those two nearest-in-cube lookups costs
 * three table reads instead of a 32-way scan, so the whole search drops
 * from 64 full RGB-distance evaluations to 2 - not an approximation of the
 * brute-force answer, the same answer, proven by tests/mr_dither_check.c's
 * independent 64-way reference (unchanged, still the oracle this is
 * checked against).
 *
 * The four (or two, for blue) levels in each cube are exactly the values
 * mr_dither_palette_ehb() computes and their >>1 halves - listed explicitly
 * here rather than re-derived by formula so the per-channel quantiser below
 * can't drift out of step with the palette actually loaded into hardware.
 */
static const uint8_t ehb_bright_r[4] = { 0, 85, 170, 255 };
static const uint8_t ehb_bright_g[4] = { 0, 85, 170, 255 };
static const uint8_t ehb_bright_b[2] = { 0, 255 };
static const uint8_t ehb_half_r[4]   = { 0, 42, 85, 127 };
static const uint8_t ehb_half_g[4]   = { 0, 42, 85, 127 };
static const uint8_t ehb_half_b[2]   = { 0, 127 };

/* [0..255] -> index of the nearest level in the matching array above. Built
 * once by brute-force over the (at most 4) real levels - cheap at build
 * time, and exact regardless of whether a level list happens to be evenly
 * spaced (the half-brite levels, being >>1 of the bright ones, are not
 * quite: 42/43/42 apart, not 42.5/42.5). */
static uint8_t ehb_q_br[256], ehb_q_bg[256], ehb_q_bb[256];
static uint8_t ehb_q_hr[256], ehb_q_hg[256], ehb_q_hb[256];
static int ehb_lut_built = 0;

static void build_ehb_channel_quant(uint8_t *lut, const uint8_t *levels, int n)
{
    int v, i;
    for (v = 0; v < 256; v++) {
        int best = 0, best_d = 0x7fffffff;
        for (i = 0; i < n; i++) {
            int d = v - levels[i];
            d = d < 0 ? -d : d;
            if (d < best_d) { best_d = d; best = i; }
        }
        lut[v] = (uint8_t)best;
    }
}

static void build_ehb_luts(void)
{
    build_ehb_channel_quant(ehb_q_br, ehb_bright_r, 4);
    build_ehb_channel_quant(ehb_q_bg, ehb_bright_g, 4);
    build_ehb_channel_quant(ehb_q_bb, ehb_bright_b, 2);
    build_ehb_channel_quant(ehb_q_hr, ehb_half_r, 4);
    build_ehb_channel_quant(ehb_q_hg, ehb_half_g, 4);
    build_ehb_channel_quant(ehb_q_hb, ehb_half_b, 2);
    ehb_lut_built = 1;
}

void mr_dither_rgb_ehb(const uint8_t *rgb, int w, int h, int rgb_stride,
                       uint8_t *out, int out_stride, int y_base)
{
    int x, y;
    if (!ehb_lut_built) build_ehb_luts();
    for (y = 0; y < h; y++) {
        const uint8_t *sr = rgb + (size_t)y * rgb_stride;
        uint8_t       *dr = out + (size_t)y * out_stride;
        const uint8_t *br = bayer4[(y_base + y) & 3];
        for (x = 0; x < w; x++) {
            const uint8_t *p = sr + x * 3;
            int t = br[x & 3];
            /* One dithered target per pixel, same as before - both cubes'
             * quantisers are evaluated against this same (rv,gv,bv), which
             * is exactly what the brute-force 64-way search also compared
             * every candidate against. */
            int rv = p[0] + (t - 8) * (255 / 3) / 16;
            int gv = p[1] + (t - 8) * (255 / 3) / 16;
            int bv = p[2] + (t - 8) * 255 / 16;
            int rq, gq, bq, hr, hg, hb;
            int br_r, br_g, br_b, hf_r, hf_g, hf_b;
            int dr_r, dr_g, dr_b;
            long d_bright, d_half;
            if (rv < 0) rv = 0; else if (rv > 255) rv = 255;
            if (gv < 0) gv = 0; else if (gv > 255) gv = 255;
            if (bv < 0) bv = 0; else if (bv > 255) bv = 255;

            rq = ehb_q_br[rv]; gq = ehb_q_bg[gv]; bq = ehb_q_bb[bv];
            hr = ehb_q_hr[rv]; hg = ehb_q_hg[gv]; hb = ehb_q_hb[bv];
            br_r = ehb_bright_r[rq]; br_g = ehb_bright_g[gq]; br_b = ehb_bright_b[bq];
            hf_r = ehb_half_r[hr];   hf_g = ehb_half_g[hg];   hf_b = ehb_half_b[hb];

            dr_r = rv - br_r; dr_g = gv - br_g; dr_b = bv - br_b;
            d_bright = (long)dr_r*dr_r + (long)dr_g*dr_g + (long)dr_b*dr_b;
            dr_r = rv - hf_r; dr_g = gv - hf_g; dr_b = bv - hf_b;
            d_half = (long)dr_r*dr_r + (long)dr_g*dr_g + (long)dr_b*dr_b;

            /* Ties go to the bright cube, matching the brute-force search's
             * own ascending-index/strict-< scan (bright entries 0..31 are
             * visited, and would win any tie, before half-bright 32..63). */
            dr[x] = (d_bright <= d_half)
                        ? (uint8_t)(rq * 8 + gq * 2 + bq)
                        : (uint8_t)(32 + hr * 8 + hg * 2 + hb);
        }
    }
}
