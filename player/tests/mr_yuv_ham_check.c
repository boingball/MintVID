/*
 * MintVID - conformance check for the fused YUV420-to-HAM paths.
 *
 * Compares fused YUV420P -> HAM6/HAM8 encoding against the real three-stage
 * composition it replaces - mr_yuv420_to_rgb24() (core/mr_yuv.c) ->
 * mr_scale_resize_rgb24() (core/mr_scale.c) -> mr_ham_encode()
 * (core/mr_ham.c), calling those actual functions directly rather than a
 * reimplementation. vscale==1 is covered as well as the vertical downscale
 * the display side actually routes here: the encoder is correct at 1:1 even
 * though aga_supports_yuv_indexed() does not choose it there.
 *
 * Bit-exactness is the contract here, not "close enough": the fused encoder
 * carries its own copies of the YCbCr->RGB and HAM quantiser tables (see
 * core/mr_yuv_ham.c), and hold-and-modify makes any single-pixel divergence
 * propagate along the rest of the scanline rather than staying local.
 */
#include "../core/mr_yuv.h"
#include "../core/mr_scale.h"
#include "../core/mr_ham.h"
#include "../core/mr_yuv_ham.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned state = 0x68616d38U;
static uint8_t random_byte(void)
{
    state = state * 1664525U + 1013904223U;
    return (uint8_t)(state >> 24);
}

static void fill_planes(uint8_t *y, uint8_t *u, uint8_t *v, int height,
                        int ch, int y_stride, int u_stride, int v_stride,
                        int extreme, int seed)
{
    int yy, xx;
    state = 0x68616d38U ^ (unsigned)seed;
    for (yy = 0; yy < height; yy++)
        for (xx = 0; xx < y_stride; xx++)
            y[(size_t)yy * y_stride + xx] = extreme ? ((xx ^ yy) & 1 ? 255 : 0)
                                                    : random_byte();
    for (yy = 0; yy < ch; yy++)
        for (xx = 0; xx < u_stride; xx++)
            u[(size_t)yy * u_stride + xx] = extreme ? ((xx ^ yy) & 1 ? 255 : 0)
                                                    : random_byte();
    for (yy = 0; yy < ch; yy++)
        for (xx = 0; xx < v_stride; xx++)
            v[(size_t)yy * v_stride + xx] = extreme ? ((xx ^ yy) & 1 ? 0 : 255)
                                                    : random_byte();
}

static int report(const char *what, const uint8_t *ref, const uint8_t *got,
                  int count, int bits, int width, int height, int dst_w,
                  int dst_h, int vscale, int extreme, int seed)
{
    int i;
    if (memcmp(ref, got, (size_t)count) == 0) return 0;
    for (i = 0; i < count; i++)
        if (ref[i] != got[i]) {
            printf("FAIL %s HAM%d w=%d h=%d dst=%dx%d vscale=%d extreme=%d "
                   "seed=%d index=%d (row %d col %d) ref=0x%02x got=0x%02x\n",
                   what, bits, width, height, dst_w, dst_h, vscale, extreme,
                   seed, i, i / dst_w, i % dst_w, ref[i], got[i]);
            return 1;
        }
    return 0;
}

static int run_case(int bits, int width, int height, int vscale,
                    int y_stride, int u_stride, int v_stride, int extreme,
                    int seed)
{
    int ch = (height + 1) / 2;
    int dst_h = height / vscale;
    uint8_t *y = (uint8_t *)malloc((size_t)y_stride * height);
    uint8_t *u = (uint8_t *)malloc((size_t)u_stride * ch);
    uint8_t *v = (uint8_t *)malloc((size_t)v_stride * ch);
    uint8_t *rgb_full = (uint8_t *)malloc((size_t)width * height * 3);
    uint8_t *rgb_resized = (uint8_t *)malloc((size_t)width * dst_h * 3);
    uint8_t *ref_out = (uint8_t *)malloc((size_t)width * dst_h);
    uint8_t *got_out = (uint8_t *)malloc((size_t)width * dst_h);
    int fails;

    fill_planes(y, u, v, height, ch, y_stride, u_stride, v_stride, extreme,
                seed);

    /* Reference: the real three-stage pipeline. */
    mr_yuv420_to_rgb24(rgb_full, width * 3, y, y_stride, u, u_stride,
                       v, v_stride, width, height, NULL, NULL);
    if (vscale == 1)
        memcpy(rgb_resized, rgb_full, (size_t)width * height * 3);
    else
        mr_scale_resize_rgb24(rgb_full, width, height, width * 3,
                              rgb_resized, width, dst_h, width * 3);
    mr_ham_encode(rgb_resized, width, dst_h, width * 3, ref_out, width, bits);

    /* Under test: the direct fused path. */
    mr_yuv420_ham_encode(y, y_stride, u, u_stride, v, v_stride, width,
                         height, vscale, bits, got_out, width);

    fails = report("vscale", ref_out, got_out, width * dst_h, bits, width,
                   height, width, dst_h, vscale, extreme, seed);

    free(y); free(u); free(v);
    free(rgb_full); free(rgb_resized); free(ref_out); free(got_out);
    return fails;
}

int main(void)
{
    static const struct { int w, h, vscale; } geoms[] = {
        { 640, 360, 2 },   /* the shipped shape: HIRES non-laced AGA fit    */
        { 320, 240, 2 },
        {  66,  36, 3 },
        { 320, 180, 1 },   /* vscale 1 - correct, though not routed here    */
        { 256, 144, 1 },
        { 128,  96, 1 },
        {  62,  30, 1 }    /* odd width - exercises the trailing-pixel tail */
    };
    static const int depths[] = { 6, 8 };
    int fails = 0, gi, di, extreme, seed;

    for (di = 0; di < 2; di++)
        for (gi = 0; gi < (int)(sizeof geoms / sizeof geoms[0]); gi++)
            for (extreme = 0; extreme < 2; extreme++)
                for (seed = 0; seed < 3; seed++) {
                    int w = geoms[gi].w, h = geoms[gi].h;
                    /* Padded planes too: the decoder hands over libavc's own
                     * buffers, whose strides exceed the visible width. */
                    int pad = seed & 1 ? 7 : 0;
                    fails |= run_case(depths[di], w, h, geoms[gi].vscale,
                                      w + pad, (w + 1) / 2 + pad,
                                      (w + 1) / 2 + pad, extreme, seed);
                }

    if (fails) { printf("YUV420->HAM fused encode checks FAILED\n"); return 1; }
    printf("YUV420->HAM direct encode checks passed "
           "(HAM6/HAM8 bit-identical to yuv->rgb24->scale->ham_encode)\n");
    return 0;
}
