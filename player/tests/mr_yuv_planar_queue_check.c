/*
 * Differential check for the experimental YUV->planar queue.
 *
 * The expected result is deliberately composed from MintVID's two established
 * public operations: YUV420P -> INDEX8, then 32-pixel C2P.  The experimental
 * dispatcher must produce the exact same eight plane bytes directly.
 */
#include "../core/mr_yuv_dither.h"
#include "../core/mr_yuv_planar_queue.h"
#include "../core/mr_c2p.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint32_t rng_state = 0x4d495654u;
static uint8_t rnd8(void)
{
    rng_state = rng_state * 1664525u + 1013904223u;
    return (uint8_t)(rng_state >> 24);
}

static int run_case(int width, int height)
{
    int pw = (width + 31) & ~31;
    int uvw = (width + 1) >> 1;
    int uvh = (height + 1) >> 1;
    int bpr = pw >> 3;
    int left = (pw - width) >> 1;
    size_t ybytes = (size_t)width * (size_t)height;
    size_t uvbytes = (size_t)uvw * (size_t)uvh;
    size_t chunky_bytes = (size_t)width * (size_t)height;
    size_t padded_bytes = (size_t)pw * (size_t)height;
    size_t plane_size = (size_t)bpr * (size_t)height;
    uint8_t *y = NULL, *u = NULL, *v = NULL;
    uint8_t *chunky = NULL, *padded = NULL;
    uint8_t *expected = NULL, *actual = NULL;
    uint8_t *planes[8];
    size_t i;
    int row, p, ok = 0;

    y = (uint8_t *)malloc(ybytes);
    u = (uint8_t *)malloc(uvbytes);
    v = (uint8_t *)malloc(uvbytes);
    chunky = (uint8_t *)malloc(chunky_bytes);
    padded = (uint8_t *)calloc(padded_bytes, 1);
    expected = (uint8_t *)calloc(padded_bytes, 1);
    actual = (uint8_t *)calloc(padded_bytes, 1);
    if (!y || !u || !v || !chunky || !padded || !expected || !actual)
        goto out;

    for (i = 0; i < ybytes; i++) y[i] = rnd8();
    for (i = 0; i < uvbytes; i++) { u[i] = rnd8(); v[i] = rnd8(); }

    /* Reference: established INDEX8 output, centred inside the same 32-pixel
     * padded row, followed by the established RiVA-style C2P. */
    mr_yuv_planar_queue_disable();
    mr_yuv420_dither_indexed(y, width, u, uvw, v, uvw,
                             width, height, 1, 8, chunky, width, 0);
    for (row = 0; row < height; row++)
        memcpy(padded + (size_t)row * pw + left,
               chunky + (size_t)row * width, (size_t)width);
    for (p = 0; p < 8; p++) planes[p] = expected + (size_t)p * plane_size;
    mr_c2p8_riva32(padded, pw, height, pw, 8, planes, bpr, 0, 0);

    /* Experimental route: same public dither call, but the m68k dispatcher
     * writes final plane-major bytes directly into `actual`. */
    if (!mr_yuv_planar_queue_configure(width, height, pw)) goto out;
    mr_yuv420_dither_indexed(y, width, u, uvw, v, uvw,
                             width, height, 1, 8, actual, pw, 0);
    mr_yuv_planar_queue_disable();

    if (memcmp(expected, actual, padded_bytes) != 0) {
        for (i = 0; i < padded_bytes; i++) {
            if (expected[i] != actual[i]) {
                fprintf(stderr,
                        "planar mismatch %dx%d pw=%d at byte %lu: expected=%02x actual=%02x\n",
                        width, height, pw, (unsigned long)i,
                        (unsigned)expected[i], (unsigned)actual[i]);
                break;
            }
        }
        goto out;
    }

    printf("planar queue %dx%d -> pw=%d: bit-exact (%lu bytes)\n",
           width, height, pw, (unsigned long)padded_bytes);
    ok = 1;
out:
    mr_yuv_planar_queue_disable();
    free(actual); free(expected); free(padded); free(chunky);
    free(v); free(u); free(y);
    return ok;
}

int main(void)
{
    /* The real regression geometry first, then awkward odd/padding/Bayer
     * shapes so a lucky 134x100 alignment cannot hide a bug. */
    if (!run_case(134, 100)) return 1;
    if (!run_case(127, 73)) return 1;
    if (!run_case(319, 101)) return 1;
    if (!run_case(640, 17)) return 1;
    puts("fused YUV planar queue: all cases passed");
    return 0;
}
