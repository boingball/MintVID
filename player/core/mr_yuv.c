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
 */
#include "mr_yuv.h"

#include <stddef.h>

/*
 * MR_YUV_NO_ASM selects the portable C below even on an Amiga build, so the
 * hand-written kernel can be A/B'd against it on real hardware without
 * disabling every other hand-asm path in the project (Makefile.amiga wires
 * MR_M68K_ASM on for all of them at once). Build with YUV_ASM=0.
 *
 * That switch exists because which one is faster is genuinely open. The asm
 * was written when GCC was emitting an indirect jsr to emit_pixel twice per
 * pixel with five stack-pushed arguments; emit_pixel is now always_inline and
 * both paths step 2x2 quads, so that gap is gone. Measured under qemu-m68k on
 * 352x288 the C is about 23% faster - but qemu costs instructions rather than
 * cycles and models neither the 68030's memory system nor its lack of branch
 * prediction, and this kernel's tuning is largely about exactly those. Only a
 * real 68030/68040/PiStorm A/B settles it.
 */
#if defined(MR_M68K_ASM) && !defined(MR_YUV_NO_ASM)
/* core/mr_yuv_m68k.S - hand-written m68k loop over the same formula and
 * tables below. __asm__ binds to the bare .globl name (m68k-amigaos-gcc
 * decorates C symbols but not hand-written asm labels), matching every
 * other hand-asm entry point in this project. */
void mr_yuv420_to_rgb24_m68k(uint8_t *dst, int dst_stride,
                             const uint8_t *y_plane, int y_stride,
                             const uint8_t *u_plane, int u_stride,
                             const uint8_t *v_plane, int v_stride,
                             int width, int height,
                             mr_yuv_service_fn service, void *service_opaque,
                             const int *luma_x298, const int *e_x409,
                             const int *d_xm100, const int *e_xm208,
                             const int *d_x516)
    __asm__("mr_yuv420_to_rgb24_m68k");
#endif

static int g_luma_x298[256];
static int g_e_x409[256];
static int g_d_xm100[256];
static int g_e_xm208[256];
static int g_d_x516[256];
static int g_tables_ready = 0;

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
    g_tables_ready = 1;
}

static uint8_t clip8(int value)
{
    if (value < 0) return 0;
    if (value > 255) return 255;
    return (uint8_t)value;
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
    dst[ri] = clip8((scaled_y + red_add) >> 8);
    dst[1]  = clip8((scaled_y + green_add) >> 8);
    dst[bi] = clip8((scaled_y + blue_add) >> 8);
}

/*
 * 4:2:0 means one chroma sample per 2x2 luma quad, so the three
 * chroma-derived addends below are shared by four output pixels. Stepping the
 * picture a row *pair* at a time computes them once for all four; stepping it
 * one row at a time - which this did until now, and which the m68k kernel in
 * mr_yuv_m68k.S still does - recomputes them for the second row of every
 * pair, doubling the chroma work for no change in output.
 *
 * That is not a small share of the total. Colour conversion, not decoding, is
 * where most of an MPEG-1 or MPEG-2 frame's time goes on 68k, and measured
 * under qemu-m68k on 352x288 this quad-stepped C is 27% faster than the
 * hand-written single-row assembly it is the reference for.
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
    /* A real-Pistorm run of an earlier version of this dispatch crashed
     * (illegal instruction, error 80000004, plus visible corruption of the
     * mouse pointer and clock gadget - a wild write) the first time it ran
     * with real audio servicing active: mr_yuv420_to_rgb24_m68k was
     * clobbering d0/a0/a1 across the periodic service() callback, since
     * those registers are only safe from *our own caller's* perspective,
     * not across a call this function makes itself - see
     * core/mr_yuv_m68k.S for the fix and tests/mr_yuv_check.c's
     * check_yuv_service_clobber() for the regression test (which
     * reproduces the crash against the unfixed asm before confirming the
     * fix). Fixed and confirmed clean on real Pistorm hardware with audio
     * servicing active (`make -f Makefile.amiga mrplay YUV_ASM=1`, since
     * folded back into the default build here). */
    mr_yuv420_to_rgb24_m68k(dst, dst_stride, y_plane, y_stride, u_plane,
                            u_stride, v_plane, v_stride, width, height,
                            service, service_opaque, g_luma_x298, g_e_x409,
                            g_d_xm100, g_e_xm208, g_d_x516);
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
    /*
     * Reuse the bit-exact hand-written RGB kernel without adding a branch to
     * its hot pixel loop.  Swap U/V and permute the coefficient tables:
     *
     *   RGB kernel R slot = 409 * V' = 516 * U  -> B
     *   RGB kernel G slot = -100*U' -208*V'     -> unchanged G
     *   RGB kernel B slot = 516 * U' = 409 * V  -> R
     *
     * with U'=V and V'=U.  The kernel therefore writes B,G,R directly, with
     * exactly the same clipping/service behaviour and no post-conversion
     * channel shuffle.
     */
    mr_yuv420_to_rgb24_m68k(dst, dst_stride, y_plane, y_stride,
                            v_plane, v_stride, u_plane, u_stride,
                            width, height, service, service_opaque,
                            g_luma_x298,
                            g_d_x516,   /* kernel e_x409:  B from original U */
                            g_e_xm208,  /* kernel d_xm100: V green term      */
                            g_d_xm100,  /* kernel e_xm208: U green term      */
                            g_e_x409);  /* kernel d_x516:  R from original V */
    return;
#endif

    yuv420_to_packed24(dst, dst_stride, y_plane, y_stride, u_plane, u_stride,
                       v_plane, v_stride, width, height, service,
                       service_opaque, 2, 0);
}

/*
 * A studio(limited)->full(PC) range rescale was added here, then reverted,
 * then re-added again with a change - all three real-hardware findings are
 * worth keeping on record since each one narrowed the actual problem:
 *
 * 1. A synthetic colour-bar A/B (WritePixel correct/saturated, overlay
 *    pastel on the same pattern) suggested the PIP's own YUV->RGB decode
 *    treats this buffer as full range, so passing studio-range bytes
 *    through unscaled under-saturates. Rescaling studio->full (0..255) at
 *    pack time fixed it.
 * 2. On real video (not a flat synthetic pattern), that same 0..255 rescale
 *    produced large solid white/black blocks - confirmed via a controlled
 *    A/B (WritePixel and CGX fullscreen both clean on the same clip, only
 *    this function's own output affected) to be *worse* than the pastel
 *    colours it fixed, so it was reverted outright back to a plain
 *    passthrough.
 * 3. But the user's own read of finding 2 - "clipping past black and going
 *    solid white" - describes black wrapping to white, not merely
 *    aggressive saturation (an ordinary clamp floors at black, it cannot
 *    produce white from a dark input). That points at the PIP hardware/
 *    driver's own internal YUV decoder not safely handling the true
 *    numeric extremes (0 and 255) of a full-range signal once actually fed
 *    them - a known category of limitation in simpler video-overlay
 *    silicon that expects input to never quite reach the rails, distinct
 *    from finding 1's separate, real range-interpretation mismatch.
 *
 * The fix combining all three: keep the rescale (finding 1 said it was
 * needed, and the user confirmed on their own follow-up that it did fix
 * real video's colours, not just the synthetic pattern), but never emit
 * literal 0 or 255 - clamp to [FULL_LO, FULL_HI] (1..254) instead of the
 * full 0..255 span, trading a fraction of a percent of contrast range for
 * insurance against whatever the hardware does at the true rails. Chroma's
 * neutral value 128 is unaffected (still maps to exactly 128), so grey/
 * neutral colours are untouched; only extreme black/white and fully
 * saturated chroma lose their last count or two of range. This is an
 * empirical workaround for a suspected hardware limitation, not a fully
 * root-caused fix - unconfirmed until a real Voodoo3/WinUAE retest.
 */
enum { MR_YUV_FULL_LO = 1, MR_YUV_FULL_HI = 254 };

static int g_y_full[256];
static int g_c_full[256];
static int g_full_enc_ready = 0;

static int round_div_signed(int num, int den)
{
    if (num >= 0) return (num + den / 2) / den;
    return -(((-num) + den / 2) / den);
}

static void build_full_range_encode_tables(void)
{
    enum { SPAN = MR_YUV_FULL_HI - MR_YUV_FULL_LO };
    int i;
    for (i = 0; i < 256; i++) {
        int y = round_div_signed((i - 16) * SPAN, 219) + MR_YUV_FULL_LO;
        int c = round_div_signed((i - 128) * SPAN, 224) + 128;
        if (y < MR_YUV_FULL_LO) y = MR_YUV_FULL_LO;
        else if (y > MR_YUV_FULL_HI) y = MR_YUV_FULL_HI;
        if (c < MR_YUV_FULL_LO) c = MR_YUV_FULL_LO;
        else if (c > MR_YUV_FULL_HI) c = MR_YUV_FULL_HI;
        g_y_full[i] = y;
        g_c_full[i] = c;
    }
    g_full_enc_ready = 1;
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
    if (!g_full_enc_ready) build_full_range_encode_tables();

    /* Real Voodoo3/P96 2.x hardware (the case this format exists for - see
     * display_p96pip.c's file header) was confirmed on real hardware to
     * show swapped colour: the format name and libraries/Picasso96.h both
     * read "Y4U2V2" as Y0,U0,Y1,V0, but the driver's actual chroma order is
     * the other way round. Slots 1 and 3 below are therefore V then U, not
     * U then V - matching what the hardware actually consumes, not the
     * nominal name. mr_y4u2v2_to_rgb24() (the software fallback decode this
     * format also needs - see display.c's switch_to_cgx_fallback()) and
     * display_p96pip.c's write_rgb_rows() (the RGB-source encode path) both
     * use the identical swapped convention, so every producer/consumer of
     * this buffer agrees. Both also emit near-full range (see this
     * function's own leading comment for why not the literal full 0..255
     * span). */
    for (row = 0; row < height; row++) {
        const uint8_t *sy = y_plane + (size_t)row * (size_t)y_stride;
        const uint8_t *su = u_plane + (size_t)(row >> 1) * (size_t)u_stride;
        const uint8_t *sv = v_plane + (size_t)(row >> 1) * (size_t)v_stride;
        uint8_t *out = dst + (size_t)row * (size_t)dst_stride;
        int x;
        for (x = 0; x < width; x += 2) {
            out[0] = (uint8_t)g_y_full[sy[x]];
            out[1] = (uint8_t)g_c_full[sv[x >> 1]];
            out[2] = (uint8_t)g_y_full[sy[x + 1]];
            out[3] = (uint8_t)g_c_full[su[x >> 1]];
            out += 4;
        }
        if (service && (row & 15) == 15) service(service_opaque);
    }
    return 1;
}

/* Inverse of the full-range (well, near-full: see MR_YUV_FULL_LO/HI above)
 * encode tables, for mr_y4u2v2_to_rgb24()'s own decode: R = Y +
 * 1.402*(Cr-128), G = Y - 0.344136*(Cb-128) - 0.714136*(Cr-128), B = Y +
 * 1.772*(Cb-128) - the standard full-range (not studio-range) YCbCr->RGB
 * matrix, since that is what this buffer now holds. This is pure software
 * (the CGX fallback path only, never the actual overlay hardware, which
 * decodes this buffer itself and is not something this file's own code
 * ever touches) so it uses the plain 0..255 matrix with no equivalent
 * safety margin - there is no hardware-extremes concern to work around on
 * this side. Y is used directly with no scale (full range needs none,
 * unlike g_luma_x298's studio-range 298/256 expansion) - only the two
 * chroma contributions are tabulated. Named _dec_ to keep them visually
 * distinct from mr_yuv420_to_rgb24()'s own studio-range g_e_x409/
 * g_d_xm100/etc tables above, which must not be reused here (they assume
 * the wrong input range entirely). */
static int g_full_dec_r_cr[256];
static int g_full_dec_g_cb[256];
static int g_full_dec_g_cr[256];
static int g_full_dec_b_cb[256];
static int g_full_dec_ready = 0;

static void build_full_range_decode_tables(void)
{
    int i;
    for (i = 0; i < 256; i++) {
        int d = i - 128;
        g_full_dec_r_cr[i] = 359 * d + 128;
        g_full_dec_g_cb[i] = -88 * d + 128;
        g_full_dec_g_cr[i] = -183 * d;
        g_full_dec_b_cb[i] = 454 * d + 128;
    }
    g_full_dec_ready = 1;
}

int mr_y4u2v2_to_rgb24(uint8_t *dst, int dst_stride,
                       const uint8_t *src, int src_stride,
                       int width, int height)
{
    int row;
    if (!dst || !src || width <= 0 || height <= 0 || (width & 1) ||
        dst_stride < width * 3 || src_stride < width * 2)
        return 0;
    if (!g_full_dec_ready) build_full_range_decode_tables();

    /* Software fallback for switch_to_cgx_fallback() (display.c): a
     * backend that doesn't implement show_yuv422 (CGX/AGA) still needs a
     * correct picture, so unpack the packed Y4U2V2 buffer back to RGB24
     * here rather than making the caller re-thread its whole decode-side
     * format choice mid-session. Unlike planar 4:2:0, this format is
     * genuinely 4:2:2 (a fresh chroma pair every row), so each row decodes
     * independently with no chroma-row bookkeeping. Slot 1 is V and slot 3
     * is U - see mr_yuv420_to_y4u2v2()'s own comment for why - and both are
     * (near-)full range, not studio range - see this file's g_full_dec_*
     * tables above for why. */
    for (row = 0; row < height; row++) {
        const uint8_t *in = src + (size_t)row * (size_t)src_stride;
        uint8_t *out = dst + (size_t)row * (size_t)dst_stride;
        int x;
        for (x = 0; x < width; x += 2) {
            unsigned vv = in[1], uu = in[3];
            int red_add = g_full_dec_r_cr[vv];
            int green_add = g_full_dec_g_cb[uu] + g_full_dec_g_cr[vv];
            int blue_add = g_full_dec_b_cb[uu];
            int y0 = in[0], y1 = in[2];

            out[0] = clip8(y0 + (red_add >> 8));
            out[1] = clip8(y0 + (green_add >> 8));
            out[2] = clip8(y0 + (blue_add >> 8));
            out[3] = clip8(y1 + (red_add >> 8));
            out[4] = clip8(y1 + (green_add >> 8));
            out[5] = clip8(y1 + (blue_add >> 8));
            in += 4;
            out += 6;
        }
    }
    return 1;
}
