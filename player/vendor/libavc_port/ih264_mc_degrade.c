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

/* dy == 0, dx != 0.  The two taps are the interleaved-neighbour pair
 * src[col] and src[col+2]. */
static void chroma_horz(UWORD8 *src, UWORD8 *dst, WORD32 src_strd,
                        WORD32 dst_strd, WORD32 dx, WORD32 ht, WORD32 wd)
{
    WORD32 row, col, cols = 2 * wd, inv = 8 - dx;
    if(dx == 4)
    {
        for(row = 0; row < ht; row++)
        {
            avg_row_u8(dst, src, src + 2, cols);
            src += src_strd;
            dst += dst_strd;
        }
        return;
    }
    for(row = 0; row < ht; row++)
    {
        for(col = 0; col < cols; col++)
            dst[col] = (UWORD8)((inv * src[col] + dx * src[col + 2] + 4) >> 3);
        src += src_strd;
        dst += dst_strd;
    }
}

/* dx == 0, dy != 0.  The two taps are vertical neighbours. */
static void chroma_vert(UWORD8 *src, UWORD8 *dst, WORD32 src_strd,
                        WORD32 dst_strd, WORD32 dy, WORD32 ht, WORD32 wd)
{
    WORD32 row, col, cols = 2 * wd, inv = 8 - dy;
    if(dy == 4)
    {
        for(row = 0; row < ht; row++)
        {
            avg_row_u8(dst, src, src + src_strd, cols);
            src += src_strd;
            dst += dst_strd;
        }
        return;
    }
    for(row = 0; row < ht; row++)
    {
        const UWORD8 *next = src + src_strd;
        for(col = 0; col < cols; col++)
            dst[col] = (UWORD8)((inv * src[col] + dy * next[col] + 4) >> 3);
        src += src_strd;
        dst += dst_strd;
    }
}

/* The general eighth-pel filter: hand-written assembly on m68k builds,
 * Ittiam's reference C everywhere else. */
MR_FORCE_INLINE void chroma_general(UWORD8 *src, UWORD8 *dst, WORD32 src_strd,
                                    WORD32 dst_strd, WORD32 dx, WORD32 dy,
                                    WORD32 ht, WORD32 wd)
{
#if defined(MR_M68K_ASM)
    mr_ih264_inter_pred_chroma_m68k(src, dst, src_strd, dst_strd, dx, dy,
                                    ht, wd);
#else
    ih264_inter_pred_chroma(src, dst, src_strd, dst_strd, dx, dy, ht, wd);
#endif
}

static void chroma_dispatch(UWORD8 *src, UWORD8 *dst, WORD32 src_strd,
                            WORD32 dst_strd, WORD32 dx, WORD32 dy,
                            WORD32 ht, WORD32 wd)
{
    if(dx == 0)
    {
        if(dy == 0) chroma_copy(src, dst, src_strd, dst_strd, ht, wd);
        else        chroma_vert(src, dst, src_strd, dst_strd, dy, ht, wd);
        return;
    }
    if(dy == 0)
    {
        chroma_horz(src, dst, src_strd, dst_strd, dx, ht, wd);
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
 * evaluated separably: a horizontal pass into 16-bit intermediates, then a
 * vertical pass.  Substituting shows the separable form is the same value,
 * not an approximation of it, and the half-sample cases (dx or dy == 2)
 * reduce to the rounded byte average avg_u8x4() computes four lanes at a
 * time.
 */
MR_FORCE_INLINE void bilinear_h_row(WORD32 *out, const UWORD8 *src,
                                    WORD32 wd, WORD32 dx)
{
    WORD32 c, inv = 4 - dx;
    if(dx == 0)
    {
        for(c = 0; c < wd; c++) out[c] = 4 * src[c];
        return;
    }
    for(c = 0; c < wd; c++)
        out[c] = inv * src[c] + dx * src[c + 1];
}

static void luma_bilinear(UWORD8 *src, UWORD8 *dst, WORD32 src_strd,
                          WORD32 dst_strd, WORD32 ht, WORD32 wd,
                          UWORD8 *tmp, WORD32 dydx)
{
    WORD32 dx = dydx & 3, dy = (dydx >> 2) & 3;
    WORD32 row, col;
    (void)tmp;

    if(dy == 0)
    {
        WORD32 inv = 4 - dx;
        if(dx == 0)
        {
            for(row = 0; row < ht; row++, src += src_strd, dst += dst_strd)
                copy_row_u8(dst, src, wd);
            return;
        }
        if(dx == 2)
        {
            for(row = 0; row < ht; row++, src += src_strd, dst += dst_strd)
                avg_row_u8(dst, src, src + 1, wd);
            return;
        }
        for(row = 0; row < ht; row++, src += src_strd, dst += dst_strd)
            for(col = 0; col < wd; col++)
                dst[col] = (UWORD8)((inv * src[col] + dx * src[col + 1] + 2) >> 2);
        return;
    }

    if(dx == 0)
    {
        WORD32 inv = 4 - dy;
        if(dy == 2)
        {
            for(row = 0; row < ht; row++, src += src_strd, dst += dst_strd)
                avg_row_u8(dst, src, src + src_strd, wd);
            return;
        }
        for(row = 0; row < ht; row++, src += src_strd, dst += dst_strd)
        {
            const UWORD8 *next = src + src_strd;
            for(col = 0; col < wd; col++)
                dst[col] = (UWORD8)((inv * src[col] + dy * next[col] + 2) >> 2);
        }
        return;
    }

    {
        /* Two rolling horizontally-filtered rows; wd is at most 16. */
        WORD32 buf[2][16];
        WORD32 *top = buf[0], *bot = buf[1];
        WORD32 inv = 4 - dy;
        bilinear_h_row(top, src, wd, dx);
        for(row = 0; row < ht; row++)
        {
            WORD32 *swap;
            bilinear_h_row(bot, src + src_strd, wd, dx);
            for(col = 0; col < wd; col++)
                dst[col] = (UWORD8)((inv * top[col] + dy * bot[col] + 8) >> 4);
            swap = top; top = bot; bot = swap;
            src += src_strd;
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
