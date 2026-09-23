/*
 * Inter-prediction filter sets for MintVID's H.264 speed modes.
 * See ih264_mc_degrade.h for why these live here and not in the vendored
 * libavc submodule.
 *
 * Written as portable C in the style of ih264_m68k_optim.c: where a whole
 * row of samples is combined with the same operation, it is done four bytes
 * at a time through a 32-bit register with carries masked out of the byte
 * lanes.  The Amiga builds require a 68030, where the unaligned longword
 * accesses that prediction source positions force are legal.
 */
#include "ih264_mc_degrade.h"

#include "ih264_typedefs.h"
#include "iv.h"
#include "ivd.h"
#include "ih264d_structs.h"
#include "ih264_inter_pred_filters.h"
#include "ih264_m68k_optim.h"
#include "ih264d_stage_profile.h"

#include <stdint.h>

#define AVG_MASK UINT32_C(0xfefefefe)
/* Two 16-bit lanes, each holding one byte - see luma_bilinear(). */
#define LANE_MASK UINT32_C(0x00ff00ff)

#if defined(__GNUC__)
#define MR_FORCE_INLINE static inline __attribute__((always_inline))
#else
#define MR_FORCE_INLINE static inline
#endif

MR_FORCE_INLINE uint32_t load_u32(const UWORD8 *p)
{
    return *(const uint32_t *)(const void *)p;
}

MR_FORCE_INLINE void store_u32(UWORD8 *p, uint32_t value)
{
    *(uint32_t *)(void *)p = value;
}

/* Rounded average (a+b+1)>>1 of four independent unsigned bytes.  The mask
 * keeps a carry out of one lane from reaching the next. */
MR_FORCE_INLINE uint32_t avg_u8x4(uint32_t a, uint32_t b)
{
    return (a | b) - (((a ^ b) & AVG_MASK) >> 1);
}

/* Every H.264 prediction block is 4, 8 or 16 samples wide, and chroma's
 * interleaved U/V rows are 2*wd = 4, 8 or 16 bytes - so a row is always a
 * whole number of longwords. */
MR_FORCE_INLINE void copy_row_u8(UWORD8 *dst, const UWORD8 *src, WORD32 bytes)
{
    WORD32 c;
    for(c = 0; c + 4 <= bytes; c += 4)
        store_u32(dst + c, load_u32(src + c));
    for(; c < bytes; c++)
        dst[c] = src[c];
}

MR_FORCE_INLINE void avg_row_u8(UWORD8 *dst, const UWORD8 *a, const UWORD8 *b,
                                WORD32 bytes)
{
    WORD32 c;
    for(c = 0; c + 4 <= bytes; c += 4)
        store_u32(dst + c, avg_u8x4(load_u32(a + c), load_u32(b + c)));
    for(; c < bytes; c++)
        dst[c] = (UWORD8)((a[c] + b[c] + 1) >> 1);
}

/* ------------------------------------------------------------------ */
/* Chroma: exact fast paths                                            */
/* ------------------------------------------------------------------ */
/*
 * ih264_inter_pred_chroma() evaluates the full four-term eighth-pel average
 * of spec equation (8-266) for every sample, and so does this port's
 * hand-written ih264_m68k_chroma_mc.S.  Both keep doing that when dx or dy
 * is zero and two or three of the four weights are therefore zero.
 *
 * Substituting into (8-266) shows those cases collapse exactly, with no
 * rounding difference at all:
 *
 *   dx==0 && dy==0 : (64*s00 + 32) >> 6            == s00
 *   dy==0          : (8*((8-dx)*s00 + dx*s10) + 32) >> 6
 *                                                  == ((8-dx)*s00 + dx*s10 + 4) >> 3
 *   dx==0          : likewise, vertically
 *
 * and the CLIP_U8 in the general routine can never fire, because a weighted
 * average of bytes whose weights sum to 64 is itself a byte.  So these are
 * bit-identical shortcuts, not approximations, and they are installed for
 * every speed mode including Quality.  Chroma MVs are the luma MVs, so
 * dx==dy==0 covers all the zero-motion background a typical stream is mostly
 * made of.
 */
static void chroma_copy(UWORD8 *src, UWORD8 *dst, WORD32 src_strd,
                        WORD32 dst_strd, WORD32 ht, WORD32 wd)
{
    WORD32 row;
    for(row = 0; row < ht; row++)
    {
        copy_row_u8(dst, src, 2 * wd);
        src += src_strd;
        dst += dst_strd;
    }
}

/*
 * The remaining cases, exact, two samples per register the same way
 * luma_bilinear() does it.  A chroma row is interleaved U,V,U,V, so the
 * horizontal neighbour of byte k is byte k+2 and a load at src+2 lines it up
 * in the same lane - U and V never mix.  Separably, a horizontal tap sum is
 * at most 8*255 = 2040 and the vertical one at most 8*2040 + 32 = 16352, so
 * both fit a 16-bit lane.  Substituting shows the separable form equals
 * (8-266) exactly; this replaces the per-sample four-multiply filter
 * (Ittiam's C, or ih264_m68k_chroma_mc.S on m68k) with about half the
 * instructions under Emu68/qemu.
 */
MR_FORCE_INLINE uint32_t chroma_lanes(uint32_t e, uint32_t o, int shift)
{
    return (((e >> shift) & LANE_MASK) << 8) | ((o >> shift) & LANE_MASK);
}

/* One axis: dy == 0 (step 2) or dx == 0 (step = stride); w is 1..7. */
static void chroma_1d(UWORD8 *src, UWORD8 *dst, WORD32 src_strd,
                      WORD32 dst_strd, WORD32 step, uint32_t w,
                      WORD32 ht, WORD32 wd)
{
    const uint32_t inv = 8 - w;
    const WORD32 groups = wd >> 1;   /* 2*wd bytes, four per group */
    WORD32 row, g;
    if(w == 4)
    {
        for(row = 0; row < ht; row++, src += src_strd, dst += dst_strd)
            avg_row_u8(dst, src, src + step, 2 * wd);
        return;
    }
    for(row = 0; row < ht; row++, src += src_strd, dst += dst_strd)
        for(g = 0; g < groups; g++)
        {
            uint32_t a = load_u32(src + 4 * g), b = load_u32(src + 4 * g + step);
            uint32_t e = inv * ((a >> 8) & LANE_MASK) + w * ((b >> 8) & LANE_MASK)
                       + UINT32_C(0x00040004);
            uint32_t o = inv * (a & LANE_MASK) + w * (b & LANE_MASK)
                       + UINT32_C(0x00040004);
            store_u32(dst + 4 * g, chroma_lanes(e, o, 3));
        }
}

MR_FORCE_INLINE void chroma_h_swar(uint32_t *ev, uint32_t *od,
                                   const UWORD8 *src, WORD32 groups,
                                   uint32_t inv, uint32_t dx)
{
    WORD32 g;
    for(g = 0; g < groups; g++, src += 4)
    {
        uint32_t a = load_u32(src), b = load_u32(src + 2);
        ev[g] = inv * ((a >> 8) & LANE_MASK) + dx * ((b >> 8) & LANE_MASK);
        od[g] = inv * (a & LANE_MASK) + dx * (b & LANE_MASK);
    }
}

static void chroma_general(UWORD8 *src, UWORD8 *dst, WORD32 src_strd,
                           WORD32 dst_strd, WORD32 dx, WORD32 dy,
                           WORD32 ht, WORD32 wd)
{
    const uint32_t inv_x = 8 - (uint32_t)dx, inv_y = 8 - (uint32_t)dy;
    const WORD32 groups = wd >> 1;
    uint32_t buf[4][4];
    uint32_t *top_e = buf[0], *top_o = buf[1];
    uint32_t *bot_e = buf[2], *bot_o = buf[3];
    WORD32 row, g;

    chroma_h_swar(top_e, top_o, src, groups, inv_x, (uint32_t)dx);
    for(row = 0; row < ht; row++)
    {
        uint32_t *swap;
        src += src_strd;
        chroma_h_swar(bot_e, bot_o, src, groups, inv_x, (uint32_t)dx);
        for(g = 0; g < groups; g++)
        {
            uint32_t e = inv_y * top_e[g] + (uint32_t)dy * bot_e[g] + UINT32_C(0x00200020);
            uint32_t o = inv_y * top_o[g] + (uint32_t)dy * bot_o[g] + UINT32_C(0x00200020);
            store_u32(dst + 4 * g, chroma_lanes(e, o, 6));
        }
        swap = top_e; top_e = bot_e; bot_e = swap;
        swap = top_o; top_o = bot_o; bot_o = swap;
        dst += dst_strd;
    }
}

static void chroma_dispatch(UWORD8 *src, UWORD8 *dst, WORD32 src_strd,
                            WORD32 dst_strd, WORD32 dx, WORD32 dy,
                            WORD32 ht, WORD32 wd)
{
    if(dx == 0)
    {
        if(dy == 0) chroma_copy(src, dst, src_strd, dst_strd, ht, wd);
        else        chroma_1d(src, dst, src_strd, dst_strd, src_strd,
                              (uint32_t)dy, ht, wd);
        return;
    }
    if(dy == 0)
    {
        chroma_1d(src, dst, src_strd, dst_strd, 2, (uint32_t)dx, ht, wd);
        return;
    }
    chroma_general(src, dst, src_strd, dst_strd, dx, dy, ht, wd);
}

/* ------------------------------------------------------------------ */
/* Luma: bilinear quarter-pel                                          */
/* ------------------------------------------------------------------ */
/*
 * ih264d_form_mb_part_info_*() splits the MV into an integer part and a
 * quarter-pel remainder dydx = (dy<<2)|dx, offsets the reference pointer
 * back by two samples in each fractional dimension so the six-tap filter has
 * its window, and ih264d_motion_compensate_*() then adds those two samples
 * back before the call.  So pu1_src as seen here is exactly the integer
 * sample position, and the four samples bilinear interpolation needs are
 * src[0], src[1], src[src_strd] and src[src_strd+1] - all well inside the
 * window the six-tap path already had clipped and padded for it.
 *
 *   dst = ((4-dx)(4-dy)A + dx(4-dy)B + (4-dx)dy C + dx dy D + 8) >> 4
 *
 * evaluated separably: a horizontal pass, then a vertical pass.  Substituting
 * shows the separable form is the same value, not an approximation of it,
 * and the half-sample cases with the other axis whole (dx or dy == 2, the
 * other 0) reduce to the rounded byte average avg_u8x4() computes four lanes
 * at a time.
 *
 * Everything else runs two samples per 32-bit register.  A longword load
 * puts four source bytes in lanes 0-3; masking with 0x00ff00ff (after an
 * 8-bit shift for the other pair) spreads two of them into 16-bit lanes, and
 * a second load one byte further on gives each sample its right-hand
 * neighbour in the same lane.  Every intermediate fits its 16-bit lane - a
 * horizontal tap sum is at most 4*255 = 1020, the weighted vertical sum at
 * most 4*1020 + 8 = 4088 - so ordinary 32-bit multiplies and adds act on
 * both lanes at once without a carry reaching the neighbour, and one
 * shift-and-mask finishes both.  Lanes are recombined with the same shift
 * that split them, so this is independent of byte order.
 *
 * On a 640x360 Baseline clip with heavy motion this roughly halved Turbo's
 * luma MC and took 16% off the whole decode (m68k code, instruction counts
 * under qemu).  Constant-weight per-slot versions of the old per-sample loop,
 * where GCC turns the multiplies into shifts and adds, measured *slower*:
 * under qemu - and Emu68 on a PiStorm - a multiply is one translated
 * instruction, so strength reduction only adds work.
 */
MR_FORCE_INLINE void bilinear_h_swar(uint32_t *ev, uint32_t *od,
                                     const UWORD8 *src, WORD32 groups,
                                     uint32_t inv, uint32_t dx)
{
    WORD32 g;
    for(g = 0; g < groups; g++, src += 4)
    {
        uint32_t a = load_u32(src), b = load_u32(src + 1);
        ev[g] = inv * ((a >> 8) & LANE_MASK) + dx * ((b >> 8) & LANE_MASK);
        od[g] = inv * (a & LANE_MASK) + dx * (b & LANE_MASK);
    }
}

static void luma_bilinear(UWORD8 *src, UWORD8 *dst, WORD32 src_strd,
                               WORD32 dst_strd, WORD32 ht, WORD32 wd,
                               UWORD8 *tmp, WORD32 dydx)
{
    const uint32_t dx = (uint32_t)(dydx & 3), dy = (uint32_t)((dydx >> 2) & 3);
    const uint32_t inv_x = 4 - dx, inv_y = 4 - dy;
    const WORD32 groups = wd >> 2;
    WORD32 row, g;
    (void)tmp;

    if(dy == 0 || dx == 0)
    {
        /* One axis only: taps are the next sample in that axis.  Whole and
         * half-sample offsets keep their exact copy/average fast paths. */
        const WORD32 step = dy == 0 ? 1 : src_strd;
        const uint32_t w = dy == 0 ? dx : dy, inv = 4 - w;
        if(w == 0)
        {
            for(row = 0; row < ht; row++, src += src_strd, dst += dst_strd)
                copy_row_u8(dst, src, wd);
            return;
        }
        if(w == 2)
        {
            for(row = 0; row < ht; row++, src += src_strd, dst += dst_strd)
                avg_row_u8(dst, src, src + step, wd);
            return;
        }
        for(row = 0; row < ht; row++, src += src_strd, dst += dst_strd)
            for(g = 0; g < groups; g++)
            {
                uint32_t a = load_u32(src + 4 * g), b = load_u32(src + 4 * g + step);
                uint32_t e = inv * ((a >> 8) & LANE_MASK)
                           + w * ((b >> 8) & LANE_MASK) + UINT32_C(0x00020002);
                uint32_t o = inv * (a & LANE_MASK)
                           + w * (b & LANE_MASK) + UINT32_C(0x00020002);
                store_u32(dst + 4 * g, (((e >> 2) & LANE_MASK) << 8)
                                       | ((o >> 2) & LANE_MASK));
            }
        return;
    }

    {
        /* Two rolling rows of horizontally filtered lane pairs; wd <= 16. */
        uint32_t buf[4][4];
        uint32_t *top_e = buf[0], *top_o = buf[1];
        uint32_t *bot_e = buf[2], *bot_o = buf[3];
        bilinear_h_swar(top_e, top_o, src, groups, inv_x, dx);
        for(row = 0; row < ht; row++)
        {
            uint32_t *swap;
            src += src_strd;
            bilinear_h_swar(bot_e, bot_o, src, groups, inv_x, dx);
            for(g = 0; g < groups; g++)
            {
                uint32_t e = inv_y * top_e[g] + dy * bot_e[g] + UINT32_C(0x00080008);
                uint32_t o = inv_y * top_o[g] + dy * bot_o[g] + UINT32_C(0x00080008);
                store_u32(dst + 4 * g, (((e >> 4) & LANE_MASK) << 8)
                                       | ((o >> 4) & LANE_MASK));
            }
            swap = top_e; top_e = bot_e; bot_e = swap;
            swap = top_o; top_o = bot_o; bot_o = swap;
            dst += dst_strd;
        }
    }
}

/* Slot 0 - the whole-sample position - in every filter set. */
MR_FORCE_INLINE ih264_inter_pred_luma_ft *luma_copy_fn(void)
{
#if defined(MR_M68K_ASM)
    return mr_ih264_inter_pred_luma_copy_m68k;
#else
    return ih264_inter_pred_luma_copy;
#endif
}

/* ------------------------------------------------------------------ */
/* Installation                                                        */
/* ------------------------------------------------------------------ */

static void install_full_luma(dec_struct_t *codec)
{
    codec->apf_inter_pred_luma[0] = luma_copy_fn();
#if defined(MR_M68K_ASM)
    codec->apf_inter_pred_luma[2] = mr_ih264_inter_pred_luma_horz_m68k;
    codec->apf_inter_pred_luma[8] = mr_ih264_inter_pred_luma_vert_m68k;
    codec->apf_inter_pred_luma[5] =
        mr_ih264_inter_pred_luma_horz_qpel_vert_qpel_m68k;
    codec->apf_inter_pred_luma[7] =
        mr_ih264_inter_pred_luma_horz_qpel_vert_qpel_m68k;
    codec->apf_inter_pred_luma[13] =
        mr_ih264_inter_pred_luma_horz_qpel_vert_qpel_m68k;
    codec->apf_inter_pred_luma[15] =
        mr_ih264_inter_pred_luma_horz_qpel_vert_qpel_m68k;
    codec->apf_inter_pred_luma[1] = mr_ih264_inter_pred_luma_horz_qpel_m68k;
    codec->apf_inter_pred_luma[3] = mr_ih264_inter_pred_luma_horz_qpel_m68k;
    codec->apf_inter_pred_luma[4] = mr_ih264_inter_pred_luma_vert_qpel_m68k;
    codec->apf_inter_pred_luma[12] = mr_ih264_inter_pred_luma_vert_qpel_m68k;
    codec->apf_inter_pred_luma[10] =
        mr_ih264_inter_pred_luma_horz_hpel_vert_hpel_m68k;
    codec->apf_inter_pred_luma[9] =
        mr_ih264_inter_pred_luma_horz_qpel_vert_hpel_m68k;
    codec->apf_inter_pred_luma[11] =
        mr_ih264_inter_pred_luma_horz_qpel_vert_hpel_m68k;
    codec->apf_inter_pred_luma[6] =
        mr_ih264_inter_pred_luma_horz_hpel_vert_qpel_m68k;
    codec->apf_inter_pred_luma[14] =
        mr_ih264_inter_pred_luma_horz_hpel_vert_qpel_m68k;
#else
    codec->apf_inter_pred_luma[2] = ih264_inter_pred_luma_horz;
    codec->apf_inter_pred_luma[8] = ih264_inter_pred_luma_vert;
    codec->apf_inter_pred_luma[5] = ih264_inter_pred_luma_horz_qpel_vert_qpel;
    codec->apf_inter_pred_luma[7] = ih264_inter_pred_luma_horz_qpel_vert_qpel;
    codec->apf_inter_pred_luma[13] = ih264_inter_pred_luma_horz_qpel_vert_qpel;
    codec->apf_inter_pred_luma[15] = ih264_inter_pred_luma_horz_qpel_vert_qpel;
    codec->apf_inter_pred_luma[1] = ih264_inter_pred_luma_horz_qpel;
    codec->apf_inter_pred_luma[3] = ih264_inter_pred_luma_horz_qpel;
    codec->apf_inter_pred_luma[4] = ih264_inter_pred_luma_vert_qpel;
    codec->apf_inter_pred_luma[12] = ih264_inter_pred_luma_vert_qpel;
    codec->apf_inter_pred_luma[10] = ih264_inter_pred_luma_horz_hpel_vert_hpel;
    codec->apf_inter_pred_luma[9] = ih264_inter_pred_luma_horz_qpel_vert_hpel;
    codec->apf_inter_pred_luma[11] = ih264_inter_pred_luma_horz_qpel_vert_hpel;
    codec->apf_inter_pred_luma[6] = ih264_inter_pred_luma_horz_hpel_vert_qpel;
    codec->apf_inter_pred_luma[14] = ih264_inter_pred_luma_horz_hpel_vert_qpel;
#endif
}

void mr_h264_port_install_inter_pred(void *handle, mr_mc_quality quality)
{
    dec_struct_t *codec = (dec_struct_t *)handle;
    WORD32 i;

    if(!codec) return;

    switch(quality)
    {
        case MR_MC_QUALITY_BILINEAR:
            codec->apf_inter_pred_luma[0] = luma_copy_fn();
            for(i = 1; i < 16; i++)
                codec->apf_inter_pred_luma[i] = luma_bilinear;
            codec->pf_inter_pred_chroma = chroma_dispatch;
            break;

        case MR_MC_QUALITY_FULL:
        default:
            install_full_luma(codec);
            codec->pf_inter_pred_chroma = chroma_dispatch;
            break;
    }

#if defined(MR_H264_STAGE_PROFILE)
    /* A profiling build times MC through wrappers sitting in these same
     * slots; the rewrite above just removed them. */
    mr_h264_stage_profile_rewrap_mc(codec);
#endif
}

int mr_h264_port_set_mc_quality(void *handle, mr_mc_quality quality)
{
    iv_obj_t *obj = (iv_obj_t *)handle;
    if(!obj || !obj->pv_codec_handle) return 0;
    mr_h264_port_install_inter_pred(obj->pv_codec_handle, quality);
    return 1;
}
