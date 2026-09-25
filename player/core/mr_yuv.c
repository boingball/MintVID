/*
 * MintVID - portable integer YUV420 to RGB24 conversion.
 *
 * Each U/V sample belongs to two neighbouring luma pixels. Calculating its
 * red/green/blue contribution once per pair removes four multiplications per
 * pixel pair while remaining byte-identical to the original scalar formula.
 *
 * The five remaining multiplications (298*luma, 409*e, -100*d, -208*e,
 * 516*d) are themselves replaced with 256-entry lookup tables, built once on
 * first use.  The luma table is indexed by the raw Y byte and folds in the
 * limited-range Y-16 clamp; the chroma tables fold in the three +128 rounding
 * biases.  That keeps both corrections out of the per-pixel/per-quad loops.
 * A 68030 without a fast integer multiplier spends far more cycles on MULU.L
 * than on a table read, and every coefficient here is applied to an 8-bit
 * input, so the table approach is exact - not an approximation - for every
 * legal sample value.
 *
 * Saturation is a byte table too (g_clip): after the >>8 every channel lies
 * in -258..534, so one indexed load replaces two compares and branches per
 * channel. Measured with callgrind over qemu-m68k (68040 flags, 640x360,
 * code page-aligned - see the note below), that took the C from 34.5M to
 * 21.8M host instructions a frame, and the half-size converter from 13.1M
 * to 10.4M.
 */
#include "mr_yuv.h"

#include <stddef.h>

/*
 * MR_YUV_NO_ASM selects the portable C below even on an Amiga build, so the
 * hand-written kernel can be A/B'd against it on real hardware without
 * disabling every other hand-asm path in the project (Makefile.amiga wires
 * MR_M68K_ASM on for all of them at once). Build with YUV_ASM=0.
 *
 * Same measurement as above: the kernel costs 20.0M host instructions a
 * frame against 21.8M for the C and 34.6M for the kernel it replaced (which
 * clipped with compares and branches and reloaded four table pointers per
 * 2x2 quad). qemu counts instructions, not cycles, so this is a fair proxy
 * for a JIT such as PiStorm's Emu68 and for "less work", not for a real
 * 68030's memory system; the half-size converter stays C because a kernel
 * for it measured 2% slower than the C there.
 *
 * Measure page-aligned: qemu does not chain translated blocks across a 4 KB
 * page, so a hot loop that happens to straddle one costs a TB lookup per
 * iteration. An unaligned build read 45M for this same kernel; build the
 * benchmark with -falign-functions=4096 and a .balign 4096 before the asm
 * entry point before comparing numbers.
 */
#if defined(MR_M68K_ASM) && !defined(MR_YUV_NO_ASM)
/* core/mr_yuv_m68k.S - hand-written m68k loop over the same formula, reading
 * every coefficient from one table block (g_m68k_block below). a_plane
 * feeds the block's slot at a_base, b_plane the slot at a_base + 0x100.
 * __asm__ binds to the bare .globl name (m68k-amigaos-gcc decorates C symbols
 * but not hand-written asm labels), matching every other hand-asm entry point
 * in this project. */
void mr_yuv420_to_rgb24_m68k(uint8_t *dst, int dst_stride,
                             const uint8_t *y_plane, int y_stride,
                             const uint8_t *a_plane, int a_stride,
                             const uint8_t *b_plane, int b_stride,
                             int width, int height,
                             mr_yuv_service_fn service, void *service_opaque,
                             const int32_t *block, const uint8_t *clip,
                             int a_base)
    __asm__("mr_yuv420_to_rgb24_m68k");
#endif

static int g_luma_x298[256];
static int g_e_x409[256];
static int g_d_xm100[256];
static int g_e_xm208[256];
static int g_d_x516[256];
/* Saturation table: index -MR_YUV_CLIP_LO..MR_YUV_CLIP_HI-1 through g_clip. */
#define MR_YUV_CLIP_LO 260
#define MR_YUV_CLIP_HI 540
static uint8_t g_clip_store[MR_YUV_CLIP_LO + MR_YUV_CLIP_HI];
#define g_clip (g_clip_store + MR_YUV_CLIP_LO)
/* RGB565: the saturated channel already shifted into its field, same index
 * range as g_clip, so a pixel is three lookups ORed together. */
static uint16_t g_r565_store[MR_YUV_CLIP_LO + MR_YUV_CLIP_HI];
static uint16_t g_g565_store[MR_YUV_CLIP_LO + MR_YUV_CLIP_HI];
static uint16_t g_b565_store[MR_YUV_CLIP_LO + MR_YUV_CLIP_HI];
#define g_r565 (g_r565_store + MR_YUV_CLIP_LO)
#define g_g565 (g_g565_store + MR_YUV_CLIP_LO)
#define g_b565 (g_b565_store + MR_YUV_CLIP_LO)
#if defined(MR_M68K_ASM) && !defined(MR_YUV_NO_ASM)
/* Layout, in ints, of the block mr_yuv_m68k.S indexes (its virtual index
 * minus 0x400, which the kernel subtracts once):
 *     0..255   luma, reached as 4*(0x400|y) by the RGB24 call
 *   256..511   luma again, reached as 4*(0x500|y) by the BGR24 call
 *   512..1023  U pairs at 8*(0x300|u): { 516d+128, -100d+128 }
 *  1024..1535  V pairs at 8*(0x400|v): { 409e+128, -208e }
 *  1536..2047  U pairs again at 8*(0x500|u)
 * RGB24 uses A = U at 0x300 and B = V at 0x400, so the kernel's channel 0 is
 * red and channel 2 blue; BGR24 uses A = V at 0x400 and B = U at 0x500, which
 * swaps them. 8 KB in all, of which one call reads 5 KB. */
static int32_t g_m68k_block[2048];
#endif
static int g_tables_ready = 0;
/* Session-specific packed P96 PIP byte order; default preserves YVYU. */
static int g_p96_yuyv = 0;

void mr_yuv_set_p96_format(int yuyv)
{
    g_p96_yuyv = yuyv != 0;
}

int mr_yuv_get_p96_format(void)
{
    return g_p96_yuyv;
}

static void build_tables(void)
{
    int i;
    for (i = 0; i < 256; i++) {
        int y = i - 16;
        int d = i - 128, e = i - 128;
        if (y < 0) y = 0;
        g_luma_x298[i] = 298 * y;
        g_e_x409[i] = 409 * e + 128;
        g_d_xm100[i] = -100 * d + 128;
        g_e_xm208[i] = -208 * e;
        g_d_x516[i] = 516 * d + 128;
    }
    for (i = -MR_YUV_CLIP_LO; i < MR_YUV_CLIP_HI; i++) {
        unsigned c = i < 0 ? 0 : i > 255 ? 255 : (unsigned)i;
        g_clip[i] = (uint8_t)c;
        g_r565[i] = (uint16_t)((c & 0xF8u) << 8);
        g_g565[i] = (uint16_t)((c & 0xFCu) << 3);
        g_b565[i] = (uint16_t)(c >> 3);
    }
#if defined(MR_M68K_ASM) && !defined(MR_YUV_NO_ASM)
    for (i = 0; i < 256; i++) {
        g_m68k_block[i] = g_m68k_block[256 + i] = g_luma_x298[i];
        g_m68k_block[512 + 2 * i] = g_m68k_block[1536 + 2 * i] = g_d_x516[i];
        g_m68k_block[513 + 2 * i] = g_m68k_block[1537 + 2 * i] = g_d_xm100[i];
        g_m68k_block[1024 + 2 * i] = g_e_x409[i];
        g_m68k_block[1025 + 2 * i] = g_e_xm208[i];
    }
#endif
    g_tables_ready = 1;
}

#if defined(__GNUC__)
#define MR_YUV_INLINE static inline __attribute__((always_inline))
#else
#define MR_YUV_INLINE static inline
#endif

/* ri/bi are the red and blue byte offsets within the packed triple - 0,2 for
 * RGB24 and 2,0 for BGR24. Both callers pass literals, so after inlining the
 * two orderings compile to separate straight-line stores with no branch. */
MR_YUV_INLINE void emit_pixel(uint8_t *dst, int luma, int red_add,
                              int green_add, int blue_add, int ri, int bi)
{
    int scaled_y = g_luma_x298[(unsigned)luma];
    dst[ri] = g_clip[(scaled_y + red_add) >> 8];
    dst[1]  = g_clip[(scaled_y + green_add) >> 8];
    dst[bi] = g_clip[(scaled_y + blue_add) >> 8];
}

/*
 * 4:2:0 means one chroma sample per 2x2 luma quad, so the three
 * chroma-derived addends below are shared by four output pixels. Stepping the
 * picture a row *pair* at a time computes them once for all four; stepping it
 * one row at a time recomputes them for the second row of every pair,
 * doubling the chroma work for no change in output. mr_yuv_m68k.S steps
 * quads the same way.
 *
 * That is not a small share of the total: colour conversion, not decoding,
 * is where most of an MPEG-1 or MPEG-2 frame's time goes on 68k.
 *
 * The addends depend only on (row>>1, x>>1), so hoisting them across the pair
 * is exact: output is bit-identical to the row-at-a-time form, and
 * tests/mr_yuv_check.c holds that.
 */
MR_YUV_INLINE void yuv420_to_packed24(uint8_t *dst, int dst_stride,
                                      const uint8_t *y_plane, int y_stride,
                                      const uint8_t *u_plane, int u_stride,
                                      const uint8_t *v_plane, int v_stride,
                                      int width, int height,
                                      mr_yuv_service_fn service,
                                      void *service_opaque, int ri, int bi)
{
    int row;

    for (row = 0; row + 1 < height; row += 2) {
        const uint8_t *y0 = y_plane + (size_t)row * y_stride;
        const uint8_t *y1 = y0 + y_stride;
        const uint8_t *su = u_plane + (size_t)(row >> 1) * u_stride;
        const uint8_t *sv = v_plane + (size_t)(row >> 1) * v_stride;
        uint8_t *o0 = dst + (size_t)row * dst_stride;
        uint8_t *o1 = o0 + dst_stride;
        int x;

        for (x = 0; x + 1 < width; x += 2) {
            unsigned uu = su[x >> 1], vv = sv[x >> 1];
            int red_add = g_e_x409[vv];
            int green_add = g_d_xm100[uu] + g_e_xm208[vv];
            int blue_add = g_d_x516[uu];
            emit_pixel(o0 + x * 3, y0[x],
                       red_add, green_add, blue_add, ri, bi);
            emit_pixel(o0 + (x + 1) * 3, y0[x + 1],
                       red_add, green_add, blue_add, ri, bi);
            emit_pixel(o1 + x * 3, y1[x],
                       red_add, green_add, blue_add, ri, bi);
            emit_pixel(o1 + (x + 1) * 3, y1[x + 1],
                       red_add, green_add, blue_add, ri, bi);
        }
        if (x < width) {
            unsigned uu = su[x >> 1], vv = sv[x >> 1];
            int red_add = g_e_x409[vv];
            int green_add = g_d_xm100[uu] + g_e_xm208[vv];
            int blue_add = g_d_x516[uu];
            emit_pixel(o0 + x * 3, y0[x],
                       red_add, green_add, blue_add, ri, bi);
            emit_pixel(o1 + x * 3, y1[x],
                       red_add, green_add, blue_add, ri, bi);
        }
        /* Unchanged service cadence: the old loop could only fire on an odd
         * row, which is always the second of a pair. */
        if (service && ((row + 1) & 15) == 15) service(service_opaque);
    }

    if (row < height) {                     /* odd height: unpaired last row */
        const uint8_t *src_y = y_plane + (size_t)row * y_stride;
        const uint8_t *su = u_plane + (size_t)(row >> 1) * u_stride;
        const uint8_t *sv = v_plane + (size_t)(row >> 1) * v_stride;
        uint8_t *out = dst + (size_t)row * dst_stride;
        int x;
        for (x = 0; x < width; x++) {
            unsigned uu = su[x >> 1], vv = sv[x >> 1];
            emit_pixel(out + x * 3, src_y[x],
                       g_e_x409[vv],
                       g_d_xm100[uu] + g_e_xm208[vv],
                       g_d_x516[uu], ri, bi);
        }
        if (service && (row & 15) == 15) service(service_opaque);
    }
}

void mr_yuv420_to_rgb24(uint8_t *dst, int dst_stride,
                        const uint8_t *y_plane, int y_stride,
                        const uint8_t *u_plane, int u_stride,
                        const uint8_t *v_plane, int v_stride,
                        int width, int height,
                        mr_yuv_service_fn service, void *service_opaque)
{
    if (!dst || !y_plane || !u_plane || !v_plane ||
        width <= 0 || height <= 0)
        return;
    if (!g_tables_ready) build_tables();

#if defined(MR_M68K_ASM) && !defined(MR_YUV_NO_ASM)
    /* The kernel's service call preserves the caller-saved registers it
     * still needs: an early version did not, and crashed real PiStorm
     * hardware (illegal instruction, wild writes) the first time audio
     * servicing was active - tests/mr_yuv_check.c's clobbering callback
     * keeps that fixed. */
    mr_yuv420_to_rgb24_m68k(dst, dst_stride, y_plane, y_stride, u_plane,
                            u_stride, v_plane, v_stride, width, height,
                            service, service_opaque, g_m68k_block, g_clip,
                            0x300);
    return;
#endif

    yuv420_to_packed24(dst, dst_stride, y_plane, y_stride, u_plane, u_stride,
                       v_plane, v_stride, width, height, service,
                       service_opaque, 0, 2);
}

void mr_yuv420_to_bgr24(uint8_t *dst, int dst_stride,
                        const uint8_t *y_plane, int y_stride,
                        const uint8_t *u_plane, int u_stride,
                        const uint8_t *v_plane, int v_stride,
                        int width, int height,
                        mr_yuv_service_fn service, void *service_opaque)
{
    if (!dst || !y_plane || !u_plane || !v_plane ||
        width <= 0 || height <= 0)
        return;
    if (!g_tables_ready) build_tables();

#if defined(MR_M68K_ASM) && !defined(MR_YUV_NO_ASM)
    /* Same kernel with the planes and table slots swapped (see
     * g_m68k_block): it writes B,G,R directly, with no branch added to its
     * pixel loop and no post-conversion channel shuffle. */
    mr_yuv420_to_rgb24_m68k(dst, dst_stride, y_plane, y_stride, v_plane,
                            v_stride, u_plane, u_stride, width, height,
                            service, service_opaque, g_m68k_block, g_clip,
                            0x400);
    return;
#endif

    yuv420_to_packed24(dst, dst_stride, y_plane, y_stride, u_plane, u_stride,
                       v_plane, v_stride, width, height, service,
                       service_opaque, 2, 0);
}

/*
 * Half-size conversion for the RTG "Half" display mode. Each output pixel is
 * one 2x2 luma block (rounded average) plus the 4:2:0 chroma sample that
 * already covers exactly that block, so no chroma is shared or interpolated
 * and the colour math is the same table-driven formula as above. Compared
 * with converting the full picture this computes one output pixel per four
 * source pixels and writes a quarter of the bytes - the conversion itself
 * and the WritePixelArray blit that follows both shrink by about 4x. A
 * trailing odd source column/row is dropped (the output is floor(w/2) x
 * floor(h/2)); the service hook runs every 8 output rows, i.e. every 16
 * source rows, matching the full-size converter's cadence.
 */
MR_YUV_INLINE void yuv420_to_packed24_half(uint8_t *dst, int dst_stride,
                                           const uint8_t *y_plane, int y_stride,
                                           const uint8_t *u_plane, int u_stride,
                                           const uint8_t *v_plane, int v_stride,
                                           int width, int height,
                                           mr_yuv_service_fn service,
                                           void *service_opaque, int ri, int bi)
{
    int out_w = width >> 1, out_h = height >> 1, row;

    for (row = 0; row < out_h; row++) {
        const uint8_t *y0 = y_plane + (size_t)(row * 2) * y_stride;
        const uint8_t *y1 = y0 + y_stride;
        const uint8_t *su = u_plane + (size_t)row * u_stride;
        const uint8_t *sv = v_plane + (size_t)row * v_stride;
        uint8_t *o = dst + (size_t)row * dst_stride;
        int x;
        for (x = 0; x < out_w; x++) {
            unsigned uu = su[x], vv = sv[x];
            int luma = ((int)y0[2 * x] + y0[2 * x + 1] +
                        y1[2 * x] + y1[2 * x + 1] + 2) >> 2;
            emit_pixel(o, luma, g_e_x409[vv],
                       g_d_xm100[uu] + g_e_xm208[vv], g_d_x516[uu], ri, bi);
            o += 3;
        }
        if (service && (row & 7) == 7) service(service_opaque);
    }
}

void mr_yuv420_to_rgb24_half(uint8_t *dst, int dst_stride,
                             const uint8_t *y_plane, int y_stride,
                             const uint8_t *u_plane, int u_stride,
                             const uint8_t *v_plane, int v_stride,
                             int width, int height,
                             mr_yuv_service_fn service, void *service_opaque)
{
    if (!dst || !y_plane || !u_plane || !v_plane || width < 2 || height < 2)
        return;
    if (!g_tables_ready) build_tables();
    yuv420_to_packed24_half(dst, dst_stride, y_plane, y_stride, u_plane,
                            u_stride, v_plane, v_stride, width, height,
                            service, service_opaque, 0, 2);
}

void mr_yuv420_to_bgr24_half(uint8_t *dst, int dst_stride,
                             const uint8_t *y_plane, int y_stride,
                             const uint8_t *u_plane, int u_stride,
                             const uint8_t *v_plane, int v_stride,
                             int width, int height,
                             mr_yuv_service_fn service, void *service_opaque)
{
    if (!dst || !y_plane || !u_plane || !v_plane || width < 2 || height < 2)
        return;
    if (!g_tables_ready) build_tables();
    yuv420_to_packed24_half(dst, dst_stride, y_plane, y_stride, u_plane,
                            u_stride, v_plane, v_stride, width, height,
                            service, service_opaque, 2, 0);
}

/* One RGB565 pixel: the same sums as emit_pixel(), clipped and packed by
 * the three field tables. */
MR_YUV_INLINE uint16_t pixel565(int luma, int red_add, int green_add,
                                int blue_add)
{
    int scaled_y = g_luma_x298[(unsigned)luma];
    return (uint16_t)(g_r565[(scaled_y + red_add) >> 8] |
                      g_g565[(scaled_y + green_add) >> 8] |
                      g_b565[(scaled_y + blue_add) >> 8]);
}

/* yuv420_to_packed24() with 16-bit output: row pairs share one chroma
 * row, pixel pairs one chroma sample. */
void mr_yuv420_to_rgb565(uint8_t *dst, int dst_stride,
                         const uint8_t *y_plane, int y_stride,
                         const uint8_t *u_plane, int u_stride,
                         const uint8_t *v_plane, int v_stride,
                         int width, int height,
                         mr_yuv_service_fn service, void *service_opaque)
{
    int row;

    if (!dst || !y_plane || !u_plane || !v_plane ||
        width <= 0 || height <= 0)
        return;
    if (!g_tables_ready) build_tables();

    for (row = 0; row < height; row += 2) {
        const uint8_t *y0 = y_plane + (size_t)row * y_stride;
        const uint8_t *y1 = y0 + y_stride;
        const uint8_t *su = u_plane + (size_t)(row >> 1) * u_stride;
        const uint8_t *sv = v_plane + (size_t)(row >> 1) * v_stride;
        uint16_t *o0 = (uint16_t *)(void *)(dst + (size_t)row * dst_stride);
        uint16_t *o1 = (uint16_t *)(void *)((uint8_t *)o0 + dst_stride);
        int pair = row + 1 < height, x;

        for (x = 0; x + 1 < width; x += 2) {
            unsigned uu = su[x >> 1], vv = sv[x >> 1];
            int red_add = g_e_x409[vv];
            int green_add = g_d_xm100[uu] + g_e_xm208[vv];
            int blue_add = g_d_x516[uu];
            o0[x] = pixel565(y0[x], red_add, green_add, blue_add);
            o0[x + 1] = pixel565(y0[x + 1], red_add, green_add, blue_add);
            if (pair) {
                o1[x] = pixel565(y1[x], red_add, green_add, blue_add);
                o1[x + 1] = pixel565(y1[x + 1], red_add, green_add, blue_add);
            }
        }
        if (x < width) {
            unsigned uu = su[x >> 1], vv = sv[x >> 1];
            int red_add = g_e_x409[vv];
            int green_add = g_d_xm100[uu] + g_e_xm208[vv];
            int blue_add = g_d_x516[uu];
            o0[x] = pixel565(y0[x], red_add, green_add, blue_add);
            if (pair)
                o1[x] = pixel565(y1[x], red_add, green_add, blue_add);
        }
        if (service && ((row + 1) & 15) == 15) service(service_opaque);
    }
}

void mr_yuv420_to_rgb565_half(uint8_t *dst, int dst_stride,
                              const uint8_t *y_plane, int y_stride,
                              const uint8_t *u_plane, int u_stride,
                              const uint8_t *v_plane, int v_stride,
                              int width, int height,
                              mr_yuv_service_fn service, void *service_opaque)
{
    int out_w = width >> 1, out_h = height >> 1, row;

    if (!dst || !y_plane || !u_plane || !v_plane || width < 2 || height < 2)
        return;
    if (!g_tables_ready) build_tables();

    for (row = 0; row < out_h; row++) {
        const uint8_t *y0 = y_plane + (size_t)(row * 2) * y_stride;
        const uint8_t *y1 = y0 + y_stride;
        const uint8_t *su = u_plane + (size_t)row * u_stride;
        const uint8_t *sv = v_plane + (size_t)row * v_stride;
        uint16_t *o = (uint16_t *)(void *)(dst + (size_t)row * dst_stride);
        int x;
        for (x = 0; x < out_w; x++) {
            unsigned uu = su[x], vv = sv[x];
            int luma = ((int)y0[2 * x] + y0[2 * x + 1] +
                        y1[2 * x] + y1[2 * x + 1] + 2) >> 2;
            o[x] = pixel565(luma, g_e_x409[vv],
                            g_d_xm100[uu] + g_e_xm208[vv], g_d_x516[uu]);
        }
        if (service && (row & 7) == 7) service(service_opaque);
    }
}

/*
 * Keep legal studio-range Y/Cb/Cr bytes unchanged. Expanding them to full
 * range caused severe white/black clipping in the P96 overlay. Passing all
 * 0..255 decoder output through unchanged fixed the broad clipping but left
 * isolated white/black flecks on high-contrast edges: video inverse-transform
 * ringing may legally reach the byte rails even though nominal studio range
 * is Y=16..235 and Cb/Cr=16..240, and this old overlay does not saturate those
 * excursions cleanly. Clamp only samples outside the nominal ranges; there is
 * still no scale or matrix conversion and every legal sample is unchanged.
 */

MR_YUV_INLINE uint8_t clamp_studio_y(uint8_t value)
{
    if (value < 16) return 16;
    if (value > 235) return 235;
    return value;
}

MR_YUV_INLINE uint8_t clamp_studio_c(uint8_t value)
{
    if (value < 16) return 16;
    if (value > 240) return 240;
    return value;
}

int mr_yuv420_to_y4u2v2(uint8_t *dst, int dst_stride,
                        const uint8_t *y_plane, int y_stride,
                        const uint8_t *u_plane, int u_stride,
                        const uint8_t *v_plane, int v_stride,
                        int width, int height,
                        mr_yuv_service_fn service, void *service_opaque)
{
    int row;
    if (!dst || !y_plane || !u_plane || !v_plane ||
        width <= 0 || height <= 0 || (width & 1) ||
        dst_stride < width * 2)
        return 0;

    /* PIP hardware/driver versions may interpret the source chroma pair
     * differently. The preference selects YVYU or YUYV on the packed producer;
     * the RGB-only producer and software fallback use the same choice.
     * Only out-of-range Y/Cr/Cb ringing is clamped to studio rails. */
    for (row = 0; row < height; row++) {
        const uint8_t *sy = y_plane + (size_t)row * (size_t)y_stride;
        const uint8_t *su = u_plane + (size_t)(row >> 1) * (size_t)u_stride;
        const uint8_t *sv = v_plane + (size_t)(row >> 1) * (size_t)v_stride;
        /* Choose the two planes once per row, never inside the pixel loop. */
        const uint8_t *c1 = g_p96_yuyv ? su : sv;
        const uint8_t *c3 = g_p96_yuyv ? sv : su;
        uint8_t *out = dst + (size_t)row * (size_t)dst_stride;
        int x;
        for (x = 0; x < width; x += 2) {
            out[0] = clamp_studio_y(sy[x]);
            out[1] = clamp_studio_c(c1[x >> 1]);
            out[2] = clamp_studio_y(sy[x + 1]);
            out[3] = clamp_studio_c(c3[x >> 1]);
            out += 4;
        }
        if (service && (row & 15) == 15) service(service_opaque);
    }
    return 1;
}

int mr_y4u2v2_to_rgb24(uint8_t *dst, int dst_stride,
                       const uint8_t *src, int src_stride,
                       int width, int height)
{
    int row;
    const int vi = g_p96_yuyv ? 3 : 1;
    const int ui = g_p96_yuyv ? 1 : 3;
    if (!dst || !src || width <= 0 || height <= 0 || (width & 1) ||
        dst_stride < width * 3 || src_stride < width * 2)
        return 0;
    if (!g_tables_ready) build_tables();

    /* Software fallback for switch_to_cgx_fallback() (display.c): a
     * backend that doesn't implement show_yuv422 (CGX/AGA) still needs a
     * correct picture, so unpack the packed Y4U2V2 buffer back to RGB24
     * here rather than making the caller re-thread its whole decode-side
     * format choice mid-session. Unlike planar 4:2:0, this format is
     * genuinely 4:2:2 (a fresh chroma pair every row), so each row decodes
     * independently with no chroma-row bookkeeping. The saved P96
     * preference defines which packed chroma slot is V/U. The packed
     * samples remain studio-range, so this reuses the normal conversion
     * tables and emit_pixel(). */
    for (row = 0; row < height; row++) {
        const uint8_t *in = src + (size_t)row * (size_t)src_stride;
        uint8_t *out = dst + (size_t)row * (size_t)dst_stride;
        int x;
        for (x = 0; x < width; x += 2) {
            unsigned vv = in[vi], uu = in[ui];
            int red_add = g_e_x409[vv];
            int green_add = g_d_xm100[uu] + g_e_xm208[vv];
            int blue_add = g_d_x516[uu];
            emit_pixel(out, in[0], red_add, green_add, blue_add, 0, 2);
            emit_pixel(out + 3, in[2], red_add, green_add, blue_add, 0, 2);
            in += 4;
            out += 6;
        }
    }
    return 1;
}
