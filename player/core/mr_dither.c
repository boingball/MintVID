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

/* The 64 colours an EHB screen can actually show: the 32 real registers,
 * plus the same 32 halved by the hardware for free. Built once and cached,
 * mirroring build_lut()'s own lut_depth-guarded caching. */
static uint8_t ehb_vr[64], ehb_vg[64], ehb_vb[64];
static int ehb_built = 0;

static void build_ehb_virtual(void)
{
    uint8_t pal[32 * 3];
    int i;
    mr_dither_palette_ehb(pal);
    for (i = 0; i < 32; i++) {
        ehb_vr[i] = pal[i*3+0]; ehb_vg[i] = pal[i*3+1]; ehb_vb[i] = pal[i*3+2];
        ehb_vr[32+i] = (uint8_t)(ehb_vr[i] >> 1);
        ehb_vg[32+i] = (uint8_t)(ehb_vg[i] >> 1);
        ehb_vb[32+i] = (uint8_t)(ehb_vb[i] >> 1);
    }
    ehb_built = 1;
}

static uint8_t ehb_nearest(int r, int g, int b)
{
    int best = 0, best_d = 0x7fffffff, i;
    for (i = 0; i < 64; i++) {
        int dr = r - ehb_vr[i], dg = g - ehb_vg[i], db = b - ehb_vb[i];
        int d = dr*dr + dg*dg + db*db;
        if (d < best_d) { best_d = d; best = i; }
    }
    return (uint8_t)best;
}

void mr_dither_rgb_ehb(const uint8_t *rgb, int w, int h, int rgb_stride,
                       uint8_t *out, int out_stride, int y_base)
{
    int x, y;
    if (!ehb_built) build_ehb_virtual();
    for (y = 0; y < h; y++) {
        const uint8_t *sr = rgb + (size_t)y * rgb_stride;
        uint8_t       *dr = out + (size_t)y * out_stride;
        const uint8_t *br = bayer4[(y_base + y) & 3];
        for (x = 0; x < w; x++) {
            const uint8_t *p = sr + x * 3;
            int t = br[x & 3];
            /* Perturb by the base cube's own per-channel step (matching
             * mr_dither_rgb_indexed()'s (t-8)*step/16 offset) before the
             * nearest-of-64 search - the half-brite bit scales all three
             * channels together, so unlike the plain indexed cube this
             * can't be reduced to three independent per-channel LUTs. */
            int rv = p[0] + (t - 8) * (255 / 3) / 16;
            int gv = p[1] + (t - 8) * (255 / 3) / 16;
            int bv = p[2] + (t - 8) * 255 / 16;
            if (rv < 0) rv = 0; else if (rv > 255) rv = 255;
            if (gv < 0) gv = 0; else if (gv > 255) gv = 255;
            if (bv < 0) bv = 0; else if (bv > 255) bv = 255;
            dr[x] = ehb_nearest(rv, gv, bv);
        }
    }
}
