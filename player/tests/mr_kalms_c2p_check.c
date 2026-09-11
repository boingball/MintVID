/* Amiga-side differential checks for MintVID's selected Kalms converters. */
#include "../core/mr_c2p.h"
#include "../core/mr_scale.h"
#ifdef MR_KALMS_040
#include "../vendor/kalms-c2p/normal/c2p1x1_8_c5_040.h"
#include "../vendor/kalms-c2p/bitmap/c2p1x1_8_c5_bm_040.h"
#include "../vendor/kalms-c2p/bitmap/c2p1x1_6_c5_bm_040.h"
#else
#include "../vendor/kalms-c2p/normal/c2p1x1_8_c5_030.h"
#endif
#include "../vendor/kalms-c2p/bitmap/c2p2x2_8_c5_bm.h"

#include <graphics/gfx.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned long random_state = 0x4b616c6dUL;

static unsigned char random_byte(void)
{
    random_state = random_state * 1103515245UL + 12345UL;
    return (unsigned char)(random_state >> 16);
}

static void fill_random(unsigned char *dst, size_t bytes)
{
    size_t i;
    for (i = 0; i < bytes; i++) dst[i] = random_byte();
}

static int check_1x1_case(int width, int height)
{
    int bpr = width / 8, bplsize = bpr * height;
    int split = height / 2;
    size_t total = (size_t)bplsize * 8;
    unsigned char *chunky = (unsigned char *)malloc((size_t)width * height);
    unsigned char *kalms = (unsigned char *)malloc(total);
    unsigned char *reference = (unsigned char *)malloc(total);
    unsigned char *planes[8];
    int p, ok = chunky && kalms && reference;

    if (ok) {
        fill_random(chunky, (size_t)width * height);
        memset(kalms, 0, total);
        memset(reference, 0, total);
        for (p = 0; p < 8; p++) planes[p] = reference + (size_t)p * bplsize;
        mr_c2p8(chunky, width, height, width, 8, planes, bpr, 0, 0);
#ifdef MR_KALMS_040
        c2p1x1_8_c5_040_init(width, split, 0, bplsize);
        c2p1x1_8_c5_040(chunky, kalms);
        c2p1x1_8_c5_040_init(width, height - split, split, bplsize);
        c2p1x1_8_c5_040(chunky + (size_t)split * width, kalms);
#else
        c2p1x1_8_c5_030_smcinit(width, height, 0, bplsize);
        c2p1x1_8_c5_030_init(width, split, 0);
        c2p1x1_8_c5_030(chunky, kalms);
        c2p1x1_8_c5_030_init(width, height - split, split);
        c2p1x1_8_c5_030(chunky + (size_t)split * width, kalms);
#endif
        ok = memcmp(kalms, reference, total) == 0;
        if (!ok) fprintf(stderr, "Kalms 1x1 mismatch at %dx%d\n", width, height);
    }
    free(reference); free(kalms); free(chunky);
    return ok;
}

static int check_2x2(void)
{
    const int width = 160, height = 19, outw = width * 2, outh = height * 2;
    int bpr = outw / 8, bplsize = bpr * outh;
    size_t total = (size_t)bplsize * 8;
    unsigned char *chunky = (unsigned char *)malloc((size_t)width * height);
    unsigned char *scaled = (unsigned char *)malloc((size_t)outw * outh);
    unsigned char *kalms = (unsigned char *)malloc(total);
    unsigned char *reference = (unsigned char *)malloc(total);
    unsigned char *planes[8];
    struct BitMap bm;
    int p, ok = chunky && scaled && kalms && reference;

    if (ok) {
        fill_random(chunky, (size_t)width * height);
        memset(kalms, 0, total);
        memset(reference, 0, total);
        memset(&bm, 0, sizeof bm);
        bm.BytesPerRow = bpr; bm.Rows = outh; bm.Depth = 8;
        for (p = 0; p < 8; p++) {
            bm.Planes[p] = kalms + (size_t)p * bplsize;
            planes[p] = reference + (size_t)p * bplsize;
        }
        mr_scale2x_u8(chunky, width, height, width, scaled, outw);
        mr_c2p8(scaled, outw, outh, outw, 8, planes, bpr, 0, 0);
        c2p2x2_8_c5_bm(width, height, 0, 0, chunky, &bm);
        ok = memcmp(kalms, reference, total) == 0;
        if (!ok) fprintf(stderr, "Kalms fused 2x2 mismatch\n");
    }
    free(reference); free(kalms); free(scaled); free(chunky);
    return ok;
}

static int check_2x2_padded(void)
{
    const int width = 66, padded = 80, height = 19;
    const int screenw = 320, screenh = 48, kernel_x = 80, visible_x = 88;
    int bpr = screenw / 8, bplsize = bpr * screenh;
    size_t total = (size_t)bplsize * 8;
    unsigned char *chunky = (unsigned char *)calloc((size_t)padded, height);
    unsigned char *scaled = (unsigned char *)malloc((size_t)width * 2 * height * 2);
    unsigned char *kalms = (unsigned char *)calloc(total, 1);
    unsigned char *reference = (unsigned char *)calloc(total, 1);
    unsigned char *planes[8];
    struct BitMap bm;
    int p, row, ok = chunky && scaled && kalms && reference;

    if (ok) {
        for (row = 0; row < height; row++)
            fill_random(chunky + (size_t)row * padded + 4, width);
        memset(&bm, 0, sizeof bm);
        bm.BytesPerRow = bpr; bm.Rows = screenh; bm.Depth = 8;
        for (p = 0; p < 8; p++) {
            bm.Planes[p] = kalms + (size_t)p * bplsize;
            planes[p] = reference + (size_t)p * bplsize;
        }
        mr_scale2x_u8(chunky + 4, width, height, padded, scaled, width * 2);
        mr_c2p8(scaled, width * 2, height * 2, width * 2, 8,
                planes, bpr, visible_x / 8, 4);
        c2p2x2_8_c5_bm(padded, height, kernel_x, 4, chunky, &bm);
        ok = memcmp(kalms, reference, total) == 0;
        if (!ok) fprintf(stderr, "Kalms padded fused 2x2 mismatch\n");
    }
    free(reference); free(kalms); free(scaled); free(chunky);
    return ok;
}

#ifdef MR_KALMS_040
static int check_8bit_bitmap(void)
{
    const int width = 160, height = 19, screenw = 320, screenh = 39;
    const int x = 80, y = 10;
    int bpr = screenw / 8, bplsize = bpr * screenh;
    size_t total = (size_t)bplsize * 8;
    unsigned char *chunky = (unsigned char *)malloc((size_t)width * height);
    unsigned char *kalms = (unsigned char *)calloc(total, 1);
    unsigned char *reference = (unsigned char *)calloc(total, 1);
    unsigned char *planes[8];
    struct BitMap bm;
    int p, ok = chunky && kalms && reference;

    if (ok) {
        fill_random(chunky, (size_t)width * height);
        memset(&bm, 0, sizeof bm);
        bm.BytesPerRow = bpr; bm.Rows = screenh; bm.Depth = 8;
        for (p = 0; p < 8; p++) {
            bm.Planes[p] = kalms + (size_t)p * bplsize;
            planes[p] = reference + (size_t)p * bplsize;
        }
        mr_c2p8(chunky, width, height, width, 8, planes, bpr, x / 8, y);
        c2p1x1_8_c5_bm_040(width, height, x, y, chunky, &bm);
        ok = memcmp(kalms, reference, total) == 0;
        if (!ok) fprintf(stderr, "Kalms 8-bit bitmap mismatch\n");
    }
    free(reference); free(kalms); free(chunky);
    return ok;
}

static int check_ham6_bitmap(void)
{
    const int width = 224, height = 19, screenw = 320, screenh = 39;
    const int x = 32, y = 10;
    int bpr = screenw / 8, bplsize = bpr * screenh;
    size_t total = (size_t)bplsize * 6;
    unsigned char *chunky = (unsigned char *)malloc((size_t)width * height);
    unsigned char *kalms = (unsigned char *)malloc(total);
    unsigned char *reference = (unsigned char *)malloc(total);
    unsigned char *planes[6];
    struct BitMap bm;
    int p, ok = chunky && kalms && reference;

    if (ok) {
        fill_random(chunky, (size_t)width * height);
        memset(kalms, 0, total);
        memset(reference, 0, total);
        memset(&bm, 0, sizeof bm);
        bm.BytesPerRow = bpr; bm.Rows = screenh; bm.Depth = 6;
        for (p = 0; p < 6; p++) {
            bm.Planes[p] = kalms + (size_t)p * bplsize;
            planes[p] = reference + (size_t)p * bplsize;
        }
        mr_c2p8(chunky, width, height, width, 6, planes, bpr, x / 8, y);
        c2p1x1_6_c5_bm_040(width, height, x, y, chunky, &bm);
        ok = memcmp(kalms, reference, total) == 0;
        if (!ok) fprintf(stderr, "Kalms HAM6 bitmap mismatch\n");
    }
    free(reference); free(kalms); free(chunky);
    return ok;
}
#endif

int main(void)
{
    static const int widths[] = { 32, 64, 96, 160, 320, 640 };
#ifdef MR_KALMS_040
    static const int heights[] = { 2, 7, 19, 257 };
#else
    static const int heights[] = { 2, 7, 19 };
#endif
    unsigned int w, h;
    for (w = 0; w < sizeof widths / sizeof widths[0]; w++)
        for (h = 0; h < sizeof heights / sizeof heights[0]; h++)
            if (!check_1x1_case(widths[w], heights[h])) return 1;
    if (!check_2x2()) return 1;
    if (!check_2x2_padded()) return 1;
#ifdef MR_KALMS_040
    if (!check_8bit_bitmap()) return 1;
    if (!check_ham6_bitmap()) return 1;
#endif
    puts("Kalms C2P checks passed");
    return 0;
}
