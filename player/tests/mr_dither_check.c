/*
 * Bit-exactness check for 4/5/8-plane mr_dither_rgb_indexed() output. On host
 * build this exercises the portable C fallback; on a real m68k build
 * (tests/run_m68k_check.sh) mr_dither_rgb8() itself dispatches to the hand
 * asm mr_dither_rgb8_m68k() (core/mr_dither_m68k.S), so the same comparison
 * exercises the real asm there.
 *
 * ref_dither() below independently derives the selected cube quantisation
 * (core/mr_dither.c), computed fresh per pixel rather than via a shared
 * precomputed LUT - so a bug in either the LUT build or the asm's LUT
 * indexing would not be masked by testing against itself.
 */
#include "../core/mr_dither.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_failures = 0;

static uint32_t xrand(uint32_t *state)
{
    *state = *state * 1103515245u + 12345u;
    return (*state >> 16) & 0x7fffu;
}

static const uint8_t ref_bayer4[4][4] = {
    {  0,  8,  2, 10 },
    { 12,  4, 14,  6 },
    {  3, 11,  1,  9 },
    { 15,  7, 13,  5 },
};

static uint8_t ref_quant(int val, int levels)
{
    int q;
    if (val < 0) val = 0; else if (val > 255) val = 255;
    q = (val * (levels - 1) + 127) / 255;
    return (uint8_t)q;
}

static void ref_dither(const uint8_t *rgb, int w, int h, int rgb_stride,
                       uint8_t *out, int out_stride, int y_base, int depth)
{
    int rl = depth == 4 ? 2 : depth == 5 ? 4 : 6;
    int gl = depth == 4 || depth == 5 ? 4 : 6;
    int bl = depth == 4 || depth == 5 ? 2 : 6;
    int x, y;
    for (y = 0; y < h; y++) {
        const uint8_t *sr = rgb + (size_t)y * rgb_stride;
        uint8_t       *dr = out + (size_t)y * out_stride;
        const uint8_t *br = ref_bayer4[(y_base + y) & 3];
        for (x = 0; x < w; x++) {
            const uint8_t *p = sr + x * 3;
            int t = br[x & 3];
            int ro = (t - 8) * (255 / (rl - 1)) / 16;
            int go = (t - 8) * (255 / (gl - 1)) / 16;
            int bo = (t - 8) * (255 / (bl - 1)) / 16;
            uint8_t qr = ref_quant(p[0] + ro, rl);
            uint8_t qg = ref_quant(p[1] + go, gl);
            uint8_t qb = ref_quant(p[2] + bo, bl);
            dr[x] = (uint8_t)(qr * gl * bl + qg * bl + qb);
        }
    }
}

static void check_size(int depth, int w, int h, uint32_t *seed, int extreme)
{
    int pad_w = w + 5, pad_h = h + 3; /* mismatched rgb_stride/out_stride */
    int rgb_stride = pad_w * 3 + 7, out_stride = pad_w + 11;
    unsigned char *rgb = (unsigned char *)malloc((size_t)rgb_stride * pad_h);
    uint8_t *out_asm = (uint8_t *)malloc((size_t)out_stride * pad_h);
    uint8_t *out_ref = (uint8_t *)malloc((size_t)out_stride * pad_h);
    int y_base, row, col;

    if (!rgb || !out_asm || !out_ref) {
        fprintf(stderr, "mr_dither_check: allocation failed\n");
        exit(1);
    }
    for (row = 0; row < rgb_stride * pad_h; row++)
        rgb[row] = extreme ? (uint8_t)((xrand(seed) & 1) ? 255 : 0)
                           : (uint8_t)(xrand(seed) & 0xff);

    for (y_base = 0; y_base < 5; y_base++) {
        memset(out_asm, 0xAA, (size_t)out_stride * pad_h);
        memset(out_ref, 0xAA, (size_t)out_stride * pad_h);
        mr_dither_rgb_indexed(rgb, w, h, rgb_stride, out_asm, out_stride,
                              y_base, depth);
        ref_dither(rgb, w, h, rgb_stride, out_ref, out_stride, y_base, depth);
        for (row = 0; row < h; row++)
            for (col = 0; col < w; col++) {
                int idx = row * out_stride + col;
                if (out_asm[idx] != out_ref[idx]) {
                    if (g_failures < 20)
                        fprintf(stderr,
                                "FAIL dither: depth=%d w=%d h=%d y_base=%d row=%d "
                                "col=%d expected=%d got=%d\n",
                                depth, w, h, y_base, row, col, out_ref[idx],
                                out_asm[idx]);
                    g_failures++;
                }
            }
        /* Padding past the visible width/height must be left untouched. */
        for (row = 0; row < pad_h; row++)
            for (col = 0; col < out_stride; col++) {
                if (row < h && col < w) continue;
                int idx = row * out_stride + col;
                if (out_asm[idx] != 0xAA) {
                    if (g_failures < 20)
                        fprintf(stderr,
                                "FAIL dither padding: depth=%d w=%d h=%d y_base=%d "
                                "row=%d col=%d wrote=%d\n",
                                depth, w, h, y_base, row, col, out_asm[idx]);
                    g_failures++;
                }
            }
    }
    free(rgb); free(out_asm); free(out_ref);
}

/*
 * Extra Half-Brite: independent re-derivation of the 32-entry base cube and
 * the 64 achievable colours (32 real + 32 hardware-halved), so a bug in
 * mr_dither_palette_ehb()'s own formula or mr_dither_rgb_ehb()'s nearest
 * search would not be masked by testing against itself.
 */
static void ref_ehb_palette(uint8_t pal[32 * 3])
{
    int r, g, b, i = 0;
    for (r = 0; r < 4; r++)
        for (g = 0; g < 4; g++)
            for (b = 0; b < 2; b++) {
                pal[i*3+0] = (uint8_t)(r * 255 / 3);
                pal[i*3+1] = (uint8_t)(g * 255 / 3);
                pal[i*3+2] = (uint8_t)(b * 255 / 1);
                i++;
            }
}

static uint8_t ref_ehb_nearest(int r, int g, int b, const uint8_t vr[64],
                               const uint8_t vg[64], const uint8_t vb[64])
{
    int best = 0, best_d = 0x7fffffff, i;
    for (i = 0; i < 64; i++) {
        int dr = r - vr[i], dg = g - vg[i], db = b - vb[i];
        int d = dr*dr + dg*dg + db*db;
        if (d < best_d) { best_d = d; best = i; }
    }
    return (uint8_t)best;
}

static void check_ehb_palette(void)
{
    uint8_t pal_ref[32 * 3], pal_got[32 * 3];
    int i;
    ref_ehb_palette(pal_ref);
    mr_dither_palette_ehb(pal_got);
    for (i = 0; i < 32 * 3; i++)
        if (pal_ref[i] != pal_got[i]) {
            if (g_failures < 20)
                fprintf(stderr, "FAIL ehb palette: entry byte %d expected=%d "
                                "got=%d\n", i, pal_ref[i], pal_got[i]);
            g_failures++;
        }
}

static void check_ehb_size(int w, int h, uint32_t *seed, int extreme)
{
    int pad_w = w + 5, pad_h = h + 3;
    int rgb_stride = pad_w * 3 + 7, out_stride = pad_w + 11;
    unsigned char *rgb = (unsigned char *)malloc((size_t)rgb_stride * pad_h);
    uint8_t *out_got = (uint8_t *)malloc((size_t)out_stride * pad_h);
    uint8_t pal[32 * 3], vr[64], vg[64], vb[64];
    int y_base, row, col, i;

    if (!rgb || !out_got) {
        fprintf(stderr, "mr_dither_check: allocation failed\n");
        exit(1);
    }
    ref_ehb_palette(pal);
    for (i = 0; i < 32; i++) {
        vr[i] = pal[i*3+0]; vg[i] = pal[i*3+1]; vb[i] = pal[i*3+2];
        vr[32+i] = (uint8_t)(vr[i] >> 1);
        vg[32+i] = (uint8_t)(vg[i] >> 1);
        vb[32+i] = (uint8_t)(vb[i] >> 1);
    }
    for (row = 0; row < rgb_stride * pad_h; row++)
        rgb[row] = extreme ? (uint8_t)((xrand(seed) & 1) ? 255 : 0)
                           : (uint8_t)(xrand(seed) & 0xff);

    for (y_base = 0; y_base < 5; y_base++) {
        memset(out_got, 0xAA, (size_t)out_stride * pad_h);
        mr_dither_rgb_ehb(rgb, w, h, rgb_stride, out_got, out_stride, y_base);
        for (row = 0; row < h; row++)
            for (col = 0; col < w; col++) {
                int idx = row * out_stride + col;
                const uint8_t *p = rgb + (size_t)row * rgb_stride + col * 3;
                int t = ref_bayer4[(y_base + row) & 3][col & 3];
                int rv = p[0] + (t - 8) * (255 / 3) / 16;
                int gv = p[1] + (t - 8) * (255 / 3) / 16;
                int bv = p[2] + (t - 8) * 255 / 16;
                uint8_t expected;
                if (rv < 0) rv = 0; else if (rv > 255) rv = 255;
                if (gv < 0) gv = 0; else if (gv > 255) gv = 255;
                if (bv < 0) bv = 0; else if (bv > 255) bv = 255;
                expected = ref_ehb_nearest(rv, gv, bv, vr, vg, vb);
                if (out_got[idx] > 63) {
                    if (g_failures < 20)
                        fprintf(stderr, "FAIL ehb range: w=%d h=%d row=%d "
                                        "col=%d idx=%d out of range\n",
                                w, h, row, col, out_got[idx]);
                    g_failures++;
                } else if (out_got[idx] != expected) {
                    if (g_failures < 20)
                        fprintf(stderr, "FAIL ehb: w=%d h=%d y_base=%d "
                                        "row=%d col=%d expected=%d got=%d\n",
                                w, h, y_base, row, col, expected, out_got[idx]);
                    g_failures++;
                }
            }
        for (row = 0; row < pad_h; row++)
            for (col = 0; col < out_stride; col++) {
                if (row < h && col < w) continue;
                int idx = row * out_stride + col;
                if (out_got[idx] != 0xAA) {
                    if (g_failures < 20)
                        fprintf(stderr, "FAIL ehb padding: w=%d h=%d "
                                        "y_base=%d row=%d col=%d wrote=%d\n",
                                w, h, y_base, row, col, out_got[idx]);
                    g_failures++;
                }
            }
    }
    free(rgb); free(out_got);
}

int main(void)
{
    static const int widths[] = { 1, 2, 3, 4, 5, 7, 8, 15, 16, 17, 63, 64,
                                  65, 160, 320, 640 };
    static const int heights[] = { 1, 2, 3, 4, 5, 11, 96, 180, 360 };
    static const int depths[] = { 4, 5, 8 };
    uint32_t seed = 3;
    unsigned di, wi, hi;
    for (di = 0; di < sizeof depths / sizeof depths[0]; di++)
        for (wi = 0; wi < sizeof widths / sizeof widths[0]; wi++)
            for (hi = 0; hi < sizeof heights / sizeof heights[0]; hi++) {
                check_size(depths[di], widths[wi], heights[hi], &seed, 0);
                check_size(depths[di], widths[wi], heights[hi], &seed, 1);
            }

    check_ehb_palette();
    for (wi = 0; wi < sizeof widths / sizeof widths[0]; wi++)
        for (hi = 0; hi < sizeof heights / sizeof heights[0]; hi++) {
            check_ehb_size(widths[wi], heights[hi], &seed, 0);
            check_ehb_size(widths[wi], heights[hi], &seed, 1);
        }

    if (g_failures) {
        fprintf(stderr, "mr_dither_check: %d mismatches\n", g_failures);
        return 1;
    }
    printf("dither checks passed\n");
    return 0;
}
