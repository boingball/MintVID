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
