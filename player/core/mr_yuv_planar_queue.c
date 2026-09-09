/*
 * MintVID - experimental small-tile-fused YUV420P -> AGA planar queue.
 *
 * Four scanlines at a time are dithered into a small aligned scratch tile and
 * immediately converted into the queue slot's FINAL plane-major representation.
 * The C2P step now uses an adaptation of RiVA's actual GrayC2P 32-pixel register
 * network rather than MintVID's earlier generic transpose8-based "RiVA-style"
 * path.  Presentation therefore performs no C2P at all: display_aga_fused.c
 * only copies the eight already-planar row bands from Fast RAM to the AGA bitmap.
 *
 * This remains bit-exact with MintVID's established 6x6x6/Bayer palette.  It is
 * not RiVA's DHAM/STORM colour model; the experiment borrows the real RiVA C2P
 * schedule while keeping MintVID's existing colour output unchanged.
 */
#include "mr_yuv_planar_queue.h"

#include <stdint.h>
#include <stddef.h>
#include <string.h>

#define MR_PLANAR_MAX_WIDTH 640
#define MR_PLANAR_TILE_ROWS 4

/* Bare symbol names are intentional.  The hand-written .S wrapper references
 * these directly; m68k-amigaos-gcc otherwise decorates C symbols. */
#if defined(__GNUC__)
volatile int mr_yuv_planar_queue_active
    __asm__("mr_yuv_planar_queue_active") = 0;
#else
volatile int mr_yuv_planar_queue_active = 0;
#endif

static int g_visible_width;
static int g_height;
static int g_padded_width;

int mr_yuv_planar_queue_configure(int visible_width, int height,
                                  int padded_width)
{
    if (visible_width <= 0 || height <= 0 ||
        padded_width < visible_width || padded_width > MR_PLANAR_MAX_WIDTH ||
        (padded_width & 31) != 0) {
        mr_yuv_planar_queue_active = 0;
        g_visible_width = g_height = g_padded_width = 0;
        return 0;
    }
    g_visible_width = visible_width;
    g_height = height;
    g_padded_width = padded_width;
    mr_yuv_planar_queue_active = 1;
    return 1;
}

void mr_yuv_planar_queue_disable(void)
{
    mr_yuv_planar_queue_active = 0;
    g_visible_width = g_height = g_padded_width = 0;
}

int mr_yuv_planar_queue_is_active(void)
{
    return mr_yuv_planar_queue_active != 0;
}

int mr_yuv_planar_queue_visible_width(void) { return g_visible_width; }
int mr_yuv_planar_queue_height(void)        { return g_height; }
int mr_yuv_planar_queue_padded_width(void)  { return g_padded_width; }

#if defined(MR_M68K_ASM)
/* The wrapper .S file renames MintVID's established implementation to this
 * bare symbol, then publishes the normal name as a tiny dispatcher. */
void mr_yuv420_dither8_m68k_base(
    const uint8_t *y_plane, int y_stride,
    const uint8_t *u_plane, int u_stride,
    const uint8_t *v_plane, int v_stride,
    int width, int dst_h, int vscale, uint8_t *out,
    int out_stride, int y_base, const int *luma_x298, const int *e_x409,
    const int *d_xm100, const int *e_xm208, const int *d_x516,
    const uint8_t *lut_r, const uint8_t *lut_g, const uint8_t *lut_b)
    __asm__("mr_yuv420_dither8_m68k_base");

void mr_yuv_planar_queue_m68k(
    const uint8_t *y_plane, int y_stride,
    const uint8_t *u_plane, int u_stride,
    const uint8_t *v_plane, int v_stride,
    int width, int dst_h, int vscale, uint8_t *out,
    int out_stride, int y_base, const int *luma_x298, const int *e_x409,
    const int *d_xm100, const int *e_xm208, const int *d_x516,
    const uint8_t *lut_r, const uint8_t *lut_g, const uint8_t *lut_b)
    __asm__("mr_yuv_planar_queue_m68k");

/* Adapted from RiVA's actual GrayC2P register-scheduled 32-pixel kernel.
 * Destination is eight consecutive plane-major buffers rather than RiVA's
 * patched screen displacements. */
void mr_c2p8_riva_native_m68k(const uint8_t *chunky, int pw, int h,
                              int chunky_stride, uint8_t *planar,
                              int plane_size, int bpr, int y0)
    __asm__("mr_c2p8_riva_native_m68k");

/* Called by the assembly dispatcher with the *same* ABI as the original
 * dither routine.  The configured AGA capability guarantees identity scaling,
 * depth 8 and a padded queue stride.  Keep validation here anyway: if that
 * contract is ever violated, disable the experiment and produce the ordinary
 * chunky result rather than corrupting memory. */
void mr_yuv_planar_queue_m68k(
    const uint8_t *y_plane, int y_stride,
    const uint8_t *u_plane, int u_stride,
    const uint8_t *v_plane, int v_stride,
    int width, int dst_h, int vscale, uint8_t *out,
    int out_stride, int y_base, const int *luma_x298, const int *e_x409,
    const int *d_xm100, const int *e_xm208, const int *d_x516,
    const uint8_t *lut_r, const uint8_t *lut_g, const uint8_t *lut_b)
{
    uint8_t raw_tile[MR_PLANAR_MAX_WIDTH * MR_PLANAR_TILE_ROWS + 15];
    uint8_t *tile = (uint8_t *)(((uintptr_t)(raw_tile + 15)) & ~(uintptr_t)15);
    size_t plane_size;
    int pw = g_padded_width;
    int bpr, left, oy;

    if (!y_plane || !u_plane || !v_plane || !out || !lut_b ||
        !mr_yuv_planar_queue_active || width != g_visible_width ||
        dst_h != g_height || vscale != 1 || out_stride != pw ||
        pw <= 0 || pw > MR_PLANAR_MAX_WIDTH || (pw & 31) != 0 ||
        /* Same unambiguous depth-8 discriminator used by the compact-LUT
         * assembly: threshold 8 has zero dither offset, and a six-level blue
         * quantiser maps 128 -> 3 (the 4/5-bit two-level blue maps it -> 1). */
        lut_b[8 * 256 + 128] != 3) {
        size_t bytes = (out && out_stride > 0 && dst_h > 0)
                     ? (size_t)out_stride * (size_t)dst_h : 0;
        mr_yuv_planar_queue_active = 0;
        if (bytes) memset(out, 0, bytes);
        mr_yuv420_dither8_m68k_base(
            y_plane, y_stride, u_plane, u_stride, v_plane, v_stride,
            width, dst_h, vscale, out, out_stride, y_base,
            luma_x298, e_x409, d_xm100, e_xm208, d_x516,
            lut_r, lut_g, lut_b);
        return;
    }

    bpr = pw >> 3;
    plane_size = (size_t)bpr * (size_t)dst_h;

    /* Centre the real image inside the 32-pixel C2P padding. Dither phase is
     * still source-relative because the base converter writes at x=0 into each
     * row's subspan tile+left; black padding never participates in Bayer x. */
    left = (pw - width) >> 1;

    /* Prevent the tile calls below from re-entering this dispatcher. Playback
     * is single-tasked and the queue is not published until conversion returns,
     * so this process-wide switch cannot race a second conversion. */
    mr_yuv_planar_queue_active = 0;
    for (oy = 0; oy < dst_h; oy += MR_PLANAR_TILE_ROWS) {
        int rows = dst_h - oy;
        const uint8_t *ry;
        const uint8_t *ru;
        const uint8_t *rv;
        if (rows > MR_PLANAR_TILE_ROWS) rows = MR_PLANAR_TILE_ROWS;

        ry = y_plane + (size_t)oy * (size_t)y_stride;
        ru = u_plane + (size_t)(oy >> 1) * (size_t)u_stride;
        rv = v_plane + (size_t)(oy >> 1) * (size_t)v_stride;

        /* oy advances by four, so every full tile starts on an even 4:2:0
         * chroma pair boundary. The final short tile starts on one too. */
        memset(tile, 0, (size_t)pw * (size_t)rows);

        /* Dither all rows in the tile with one asm entry/exit. */
        mr_yuv420_dither8_m68k_base(
            ry, y_stride, ru, u_stride, rv, v_stride,
            width, rows, 1, tile + left, pw, y_base + oy,
            luma_x298, e_x409, d_xm100, e_xm208, d_x516,
            lut_r, lut_g, lut_b);

        /* RiVA's actual 32-pixel register transpose writes directly into the
         * queue's final plane-major representation.  Keeping this immediately
         * after the dither means the <=2.5 KB tile is still hot while avoiding
         * the four transpose8 sub-calls per 32-pixel group used previously. */
        mr_c2p8_riva_native_m68k(tile, pw, rows, pw, out,
                                 (int)plane_size, bpr, oy);
    }
    mr_yuv_planar_queue_active = 1;
}
#endif /* MR_M68K_ASM */
