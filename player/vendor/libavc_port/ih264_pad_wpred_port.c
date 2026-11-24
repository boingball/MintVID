/*
 * Two exact shortcuts around libavc function pointers, installed by
 * ih264d_init_function_ptr() after every other primitive is chosen.
 *
 * Weighted prediction with default weights
 * ----------------------------------------
 * The slice-level check in ih264d_parse_pred_weight_table()
 * (u1_wts_ofst_trivial) only fires when *every* reference of a slice has the
 * default weight and zero offset.  The per-block weighted routines still run
 * for the default-weight references of a slice where some other reference is
 * weighted, and implicit bi-prediction (weighted_bipred_idc == 2, x264's
 * default for B slices) derives its weights per block pair from POC
 * distances, so a B picture midway between its references gets w0 = w1 = 32
 * with log_wd = 5 on every bi-predicted block.  Substituting the defaults into
 * libavc's own formulas (common/ih264_weighted_pred.c):
 *
 *   single list, wt == 1 << log_wd, ofst == 0:
 *     log_wd >= 1: (wt*a + (1 << (log_wd-1))) >> log_wd == a
 *     log_wd == 0: wt*a + ofst == a
 *   bi, wt1 == wt2 == 1 << log_wd, ofst1 == ofst2 == 0:
 *     (a*2^L + b*2^L + 2^L) >> (L+1) == (a + b + 1) >> 1
 *
 * which is the plain average pf_default_weighted_pred_* computes, and the
 * single-list case is the identity (the caller always passes src == dst).
 * CLIP_U8 never fires on either.  So the result is bit-identical; only the
 * multiply-per-sample routines are skipped.
 *
 * Frame-edge padding
 * ------------------
 * ih264_pad_left_luma()/ih264_pad_right_luma() call memset() once per row for
 * a 32-byte run (PAD_LEN_Y_H), i.e. about 2 * height library calls per
 * picture.  A memset call costs far more than eight longword stores of the
 * replicated byte.  Same bytes written.
 */
#include <stdint.h>
#include <string.h>

#include "ih264_typedefs.h"
#include "iv.h"
#include "ivd.h"
#include "ih264d_structs.h"
#include "ih264_pad_wpred_port.h"

static ih264_weighted_pred_ft *g_wpred_luma, *g_wpred_chroma;
static ih264_weighted_bi_pred_ft *g_wbipred_luma, *g_wbipred_chroma;
static ih264_default_weighted_pred_ft *g_dpred_luma, *g_dpred_chroma;

static void wpred_luma(UWORD8 *src, UWORD8 *dst, WORD32 src_strd,
                       WORD32 dst_strd, WORD32 log_wd, WORD32 wt,
                       WORD32 ofst, WORD32 ht, WORD32 wd)
{
    if((WORD16)(wt & 0xffff) == (1 << log_wd) && !(WORD8)(ofst & 0xff) &&
       src == dst && src_strd == dst_strd)
        return;
    g_wpred_luma(src, dst, src_strd, dst_strd, log_wd, wt, ofst, ht, wd);
}

static void wpred_chroma(UWORD8 *src, UWORD8 *dst, WORD32 src_strd,
                         WORD32 dst_strd, WORD32 log_wd, WORD32 wt,
                         WORD32 ofst, WORD32 ht, WORD32 wd)
{
    if((WORD16)(wt & 0xffff) == (1 << log_wd) &&
       (WORD16)(wt >> 16) == (1 << log_wd) && !(ofst & 0xffff) &&
       src == dst && src_strd == dst_strd)
        return;
    g_wpred_chroma(src, dst, src_strd, dst_strd, log_wd, wt, ofst, ht, wd);
}

static void wbipred_luma(UWORD8 *src1, UWORD8 *src2, UWORD8 *dst,
                         WORD32 src_strd1, WORD32 src_strd2, WORD32 dst_strd,
                         WORD32 log_wd, WORD32 wt1, WORD32 wt2, WORD32 ofst1,
                         WORD32 ofst2, WORD32 ht, WORD32 wd)
{
    if((WORD16)(wt1 & 0xffff) == (1 << log_wd) &&
       (WORD16)(wt2 & 0xffff) == (1 << log_wd) &&
       !(WORD8)(ofst1 & 0xff) && !(WORD8)(ofst2 & 0xff))
    {
        g_dpred_luma(src1, src2, dst, src_strd1, src_strd2, dst_strd, ht, wd);
        return;
    }
    g_wbipred_luma(src1, src2, dst, src_strd1, src_strd2, dst_strd, log_wd,
                   wt1, wt2, ofst1, ofst2, ht, wd);
}

static void wbipred_chroma(UWORD8 *src1, UWORD8 *src2, UWORD8 *dst,
                           WORD32 src_strd1, WORD32 src_strd2,
                           WORD32 dst_strd, WORD32 log_wd, WORD32 wt1,
                           WORD32 wt2, WORD32 ofst1, WORD32 ofst2,
                           WORD32 ht, WORD32 wd)
{
    if((WORD16)(wt1 & 0xffff) == (1 << log_wd) &&
       (WORD16)(wt1 >> 16) == (1 << log_wd) &&
       (WORD16)(wt2 & 0xffff) == (1 << log_wd) &&
       (WORD16)(wt2 >> 16) == (1 << log_wd) &&
       !(ofst1 & 0xffff) && !(ofst2 & 0xffff))
    {
        g_dpred_chroma(src1, src2, dst, src_strd1, src_strd2, dst_strd, ht,
                       wd);
        return;
    }
    g_wbipred_chroma(src1, src2, dst, src_strd1, src_strd2, dst_strd, log_wd,
                     wt1, wt2, ofst1, ofst2, ht, wd);
}

/* Fill n bytes at p with v; n is a multiple of 4 on every call below. */
static inline void fill_run(UWORD8 *p, UWORD32 v, WORD32 n)
{
    if(!((uintptr_t)p & 3) && !(n & 3))
    {
        UWORD32 *q = (UWORD32 *)p;
        WORD32 i;
        for(i = n >> 2; i > 0; i--)
            *q++ = v;
        return;
    }
    memset(p, (int)(v & 0xff), (size_t)n);
}

static void pad_left_luma(UWORD8 *src, WORD32 src_strd, WORD32 ht,
                          WORD32 pad_size)
{
    WORD32 row;
    for(row = 0; row < ht; row++, src += src_strd)
        fill_run(src - pad_size, 0x01010101u * src[0], pad_size);
}

static void pad_right_luma(UWORD8 *src, WORD32 src_strd, WORD32 ht,
                           WORD32 pad_size)
{
    WORD32 row;
    for(row = 0; row < ht; row++, src += src_strd)
        fill_run(src, 0x01010101u * src[-1], pad_size);
}

/* Chroma rows are interleaved U,V: replicate the UV pair, in memory order, so
 * the result does not depend on byte order. */
static UWORD32 uv_pair(const UWORD8 *p)
{
    UWORD32 v;
    UWORD8 b[4];
    b[0] = b[2] = p[0];
    b[1] = b[3] = p[1];
    memcpy(&v, b, 4);
    return v;
}

static void pad_left_chroma(UWORD8 *src, WORD32 src_strd, WORD32 ht,
                            WORD32 pad_size)
{
    WORD32 row;
    for(row = 0; row < ht; row++, src += src_strd)
    {
        UWORD8 *p = src - pad_size;
        UWORD32 v = uv_pair(src);
        if(!((uintptr_t)p & 3) && !(pad_size & 3))
        {
            fill_run(p, v, pad_size);
        }
        else
        {
            WORD32 c;
            for(c = 0; c < pad_size; c += 2)
            {
                p[c] = src[0];
                p[c + 1] = src[1];
            }
        }
    }
}

static void pad_right_chroma(UWORD8 *src, WORD32 src_strd, WORD32 ht,
                             WORD32 pad_size)
{
    WORD32 row;
    for(row = 0; row < ht; row++, src += src_strd)
    {
        UWORD32 v = uv_pair(src - 2);
        if(!((uintptr_t)src & 3) && !(pad_size & 3))
        {
            fill_run(src, v, pad_size);
        }
        else
        {
            WORD32 c;
            for(c = 0; c < pad_size; c += 2)
            {
                src[c] = src[-2];
                src[c + 1] = src[-1];
            }
        }
    }
}

void mr_h264_port_install_pad_wpred(dec_struct_t *codec)
{
    g_wpred_luma = codec->pf_weighted_pred_luma;
    g_wpred_chroma = codec->pf_weighted_pred_chroma;
    g_wbipred_luma = codec->pf_weighted_bi_pred_luma;
    g_wbipred_chroma = codec->pf_weighted_bi_pred_chroma;
    g_dpred_luma = codec->pf_default_weighted_pred_luma;
    g_dpred_chroma = codec->pf_default_weighted_pred_chroma;
    codec->pf_weighted_pred_luma = wpred_luma;
    codec->pf_weighted_pred_chroma = wpred_chroma;
    codec->pf_weighted_bi_pred_luma = wbipred_luma;
    codec->pf_weighted_bi_pred_chroma = wbipred_chroma;

    codec->pf_pad_left_luma = pad_left_luma;
    codec->pf_pad_right_luma = pad_right_luma;
    codec->pf_pad_left_chroma = pad_left_chroma;
    codec->pf_pad_right_chroma = pad_right_chroma;
}
