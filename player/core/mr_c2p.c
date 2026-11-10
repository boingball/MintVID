/*
 * MintVID - chunky-to-planar via 8x8 bit transpose.
 */
#include "mr_c2p.h"
#include <string.h>

#if defined(MR_M68K_ASM)
/* core/mr_c2p_m68k.S - hand-written m68k version of mr_c2p8_riva32 below,
 * using native 32-bit register-pair arithmetic instead of the uint64_t the
 * portable version needs gcc to emulate on a target with no 64-bit ALU.
 * __asm__ binds to the bare .globl name (m68k-amigaos-gcc decorates C
 * symbols but not hand-written asm labels), matching every other hand-asm
 * entry point in this project. */
void mr_c2p8_riva32_m68k(const uint8_t *chunky, int pw, int h,
                         int chunky_stride, int nplanes,
                         uint8_t *const planes[], int bpr,
                         int x0byte, int y0)
    __asm__("mr_c2p8_riva32_m68k");
#endif

/* Transpose 8 chunky bytes (pixels, pixel 0 -> MSB of each plane byte) into 8
 * plane bytes. Host-verified identical to the naive per-bit reference. */
static void transpose8(const uint8_t *in, uint8_t out[8])
{
    uint64_t x = 0;
    int i;
    for (i = 0; i < 8; i++) x = (x << 8) | in[i];
    x = (x & 0xAA55AA55AA55AA55ULL)
      | ((x & 0x00AA00AA00AA00AAULL) << 7)
      | ((x >> 7) & 0x00AA00AA00AA00AAULL);
    x = (x & 0xCCCC3333CCCC3333ULL)
      | ((x & 0x0000CCCC0000CCCCULL) << 14)
      | ((x >> 14) & 0x0000CCCC0000CCCCULL);
    x = (x & 0xF0F0F0F00F0F0F0FULL)
      | ((x & 0x00000000F0F0F0F0ULL) << 28)
      | ((x >> 28) & 0x00000000F0F0F0F0ULL);
    for (i = 0; i < 8; i++) { out[i] = (uint8_t)x; x >>= 8; }
}

void mr_c2p8(const uint8_t *chunky, int pw, int h, int chunky_stride,
             int nplanes, uint8_t *const planes[], int bpr,
             int x0byte, int y0)
{
    int groups = pw >> 3;                 /* pw is a multiple of 8          */
    int y, g, k;
    for (y = 0; y < h; y++) {
        const uint8_t *row = chunky + (size_t)y * chunky_stride;
        size_t base = (size_t)(y0 + y) * bpr + x0byte;
        for (g = 0; g < groups; g++) {
            uint8_t pl[8];
            transpose8(row + (g << 3), pl);
            for (k = 0; k < nplanes; k++)
                planes[k][base + g] = pl[k];
        }
    }
}

/* Build each plane from left to right: pixel zero becomes bit 31, exactly as
 * four consecutive transpose8 calls make it the MSB of the first plane byte. */
static void transpose32(const uint8_t *in, uint32_t out[8])
{
    int pixel, plane;
    for (plane = 0; plane < 8; plane++) out[plane] = 0;
    for (pixel = 0; pixel < 32; pixel++) {
        uint32_t value = in[pixel];
        out[0] = (out[0] << 1) | ( value        & 1U);
        out[1] = (out[1] << 1) | ((value >> 1) & 1U);
        out[2] = (out[2] << 1) | ((value >> 2) & 1U);
        out[3] = (out[3] << 1) | ((value >> 3) & 1U);
        out[4] = (out[4] << 1) | ((value >> 4) & 1U);
        out[5] = (out[5] << 1) | ((value >> 5) & 1U);
        out[6] = (out[6] << 1) | ((value >> 6) & 1U);
        out[7] = (out[7] << 1) | ((value >> 7) & 1U);
    }
}

static void store_plane_word(uint8_t *dst, uint32_t value)
{
#if defined(AMIGA_M68K) && !defined(MR_HOST_BUILD)
    /* Callers guarantee alignment; m68k's big-endian store writes four plane
     * bytes with one longword Chip RAM transaction. */
    *(uint32_t *)dst = value;
#else
    /* Keep differential tests independent of the host's byte order. */
    dst[0] = (uint8_t)(value >> 24);
    dst[1] = (uint8_t)(value >> 16);
    dst[2] = (uint8_t)(value >> 8);
    dst[3] = (uint8_t)value;
#endif
}

void mr_c2p8_riva32(const uint8_t *chunky, int pw, int h, int chunky_stride,
                    int nplanes, uint8_t *const planes[], int bpr,
                    int x0byte, int y0)
{
    int y, group, plane;
#if defined(MR_M68K_ASM)
    mr_c2p8_riva32_m68k(chunky, pw, h, chunky_stride, nplanes, planes, bpr,
                        x0byte, y0);
    return;
#endif
    for (y = 0; y < h; y++) {
        const uint8_t *row = chunky + (size_t)y * chunky_stride;
        size_t base = (size_t)(y0 + y) * bpr + x0byte;
        for (group = 0; group < (pw >> 5); group++) {
            uint32_t words[8];
            transpose32(row + (group << 5), words);
            for (plane = 0; plane < nplanes; plane++)
                store_plane_word(planes[plane] + base + (group << 2),
                                 words[plane]);
        }
    }
}

unsigned long mr_planar_shadow_copy(const uint8_t *src, uint8_t *dst,
                                    uint8_t *shadow, unsigned long bytes, int valid)
{
    unsigned long i = 0, written = 0;
    for (; i + 4 <= bytes; i += 4) {
        uint32_t a, b = 0;
        memcpy(&a, src + i, 4);
        if (valid) memcpy(&b, shadow + i, 4);
        if (!valid || a != b) {
            memcpy(dst + i, &a, 4); memcpy(shadow + i, &a, 4); written += 4;
        }
    }
    for (; i < bytes; i++) if (!valid || src[i] != shadow[i]) {
        dst[i] = shadow[i] = src[i]; written++;
    }
    return written;
}
