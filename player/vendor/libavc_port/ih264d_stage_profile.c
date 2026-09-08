/*
 * Diagnostic timing wrappers - see ih264d_stage_profile.h.  Every wrapper
 * below is a plain, semantics-preserving pass-through: call the captured
 * original function pointer with the exact same arguments, time it with the
 * same clock()/CLOCKS_PER_SEC pattern mr_h264.c already uses for
 * input_us/core_us/output_us, and add the elapsed microseconds to that
 * stage's accumulator. Nothing here changes what gets decoded, only how
 * long each already-existing call takes to measure.
 */
#include "ih264_typedefs.h"
#include "ih264_deblk_edge_filters.h"
#include "ih264_inter_pred_filters.h"
#include "ih264_intra_pred_filters.h"
#include "ih264_trans_quant_itrans_iquant.h"
#include "iv.h"
#include "ivd.h"
#include "ih264d_structs.h"
#include "ih264d_stage_profile.h"

#include <time.h>

static unsigned long g_mc_us;
static unsigned long g_deblock_us;
static unsigned long g_recon_us;
static unsigned long g_intra_us;

static unsigned long stage_elapsed_us(clock_t begin)
{
    return (unsigned long)((clock() - begin) * 1000000UL / CLOCKS_PER_SEC);
}

/* ---- motion compensation: apf_inter_pred_luma[16] ---------------------- */
static ih264_inter_pred_luma_ft *g_mc_orig[16];

#define MR_MC_WRAP(N) \
static void mc_wrap_##N(UWORD8 *pu1_src, UWORD8 *pu1_dst, WORD32 src_strd, \
                        WORD32 dst_strd, WORD32 ht, WORD32 wd, \
                        UWORD8 *pu1_tmp, WORD32 dydx) \
{ \
    clock_t t0 = clock(); \
    g_mc_orig[N](pu1_src, pu1_dst, src_strd, dst_strd, ht, wd, pu1_tmp, dydx); \
    g_mc_us += stage_elapsed_us(t0); \
}
MR_MC_WRAP(0)  MR_MC_WRAP(1)  MR_MC_WRAP(2)  MR_MC_WRAP(3)
MR_MC_WRAP(4)  MR_MC_WRAP(5)  MR_MC_WRAP(6)  MR_MC_WRAP(7)
MR_MC_WRAP(8)  MR_MC_WRAP(9)  MR_MC_WRAP(10) MR_MC_WRAP(11)
MR_MC_WRAP(12) MR_MC_WRAP(13) MR_MC_WRAP(14) MR_MC_WRAP(15)
#undef MR_MC_WRAP

/* Chroma prediction is motion compensation too, and it is not cheap - before
 * ih264_mc_degrade.c's exact dx/dy shortcuts it was the single hottest
 * function in a decode. Leaving it out of mc_us made the on-hardware "h264
 * stages:" line under-report motion compensation and inflate the unattributed
 * remainder. */
static ih264_inter_pred_chroma_ft *g_mc_chroma_orig;
static void mc_chroma_wrap(UWORD8 *pu1_src, UWORD8 *pu1_dst, WORD32 src_strd,
                           WORD32 dst_strd, WORD32 dx, WORD32 dy,
                           WORD32 ht, WORD32 wd)
{
    clock_t t0 = clock();
    g_mc_chroma_orig(pu1_src, pu1_dst, src_strd, dst_strd, dx, dy, ht, wd);
    g_mc_us += stage_elapsed_us(t0);
}

static ih264_inter_pred_luma_ft * const g_mc_wrap[16] = {
    mc_wrap_0,  mc_wrap_1,  mc_wrap_2,  mc_wrap_3,
    mc_wrap_4,  mc_wrap_5,  mc_wrap_6,  mc_wrap_7,
    mc_wrap_8,  mc_wrap_9,  mc_wrap_10, mc_wrap_11,
    mc_wrap_12, mc_wrap_13, mc_wrap_14, mc_wrap_15
};

/* ---- deblocking: 8 non-MBAFF luma/chroma vert/horz edge filters -------- */
#define MR_DEBLK_BS4_WRAP(NAME) \
static ih264_deblk_edge_bs4_ft *g_##NAME##_orig; \
static void NAME##_wrap(UWORD8 *pu1_src, WORD32 src_strd, WORD32 alpha, \
                        WORD32 beta) \
{ \
    clock_t t0 = clock(); \
    g_##NAME##_orig(pu1_src, src_strd, alpha, beta); \
    g_deblock_us += stage_elapsed_us(t0); \
}
MR_DEBLK_BS4_WRAP(deblk_luma_vert_bs4)
MR_DEBLK_BS4_WRAP(deblk_luma_horz_bs4)
#undef MR_DEBLK_BS4_WRAP

#define MR_DEBLK_BSLT4_WRAP(NAME) \
static ih264_deblk_edge_bslt4_ft *g_##NAME##_orig; \
static void NAME##_wrap(UWORD8 *pu1_src, WORD32 src_strd, WORD32 alpha, \
                        WORD32 beta, UWORD32 u4_bs, \
                        const UWORD8 *pu1_cliptab) \
{ \
    clock_t t0 = clock(); \
    g_##NAME##_orig(pu1_src, src_strd, alpha, beta, u4_bs, pu1_cliptab); \
    g_deblock_us += stage_elapsed_us(t0); \
}
MR_DEBLK_BSLT4_WRAP(deblk_luma_vert_bslt4)
MR_DEBLK_BSLT4_WRAP(deblk_luma_horz_bslt4)
#undef MR_DEBLK_BSLT4_WRAP

#define MR_DEBLK_CHROMA_BS4_WRAP(NAME) \
static ih264_deblk_chroma_edge_bs4_ft *g_##NAME##_orig; \
static void NAME##_wrap(UWORD8 *pu1_src, WORD32 src_strd, WORD32 alpha_cb, \
                        WORD32 beta_cb, WORD32 alpha_cr, WORD32 beta_cr) \
{ \
    clock_t t0 = clock(); \
    g_##NAME##_orig(pu1_src, src_strd, alpha_cb, beta_cb, alpha_cr, beta_cr); \
    g_deblock_us += stage_elapsed_us(t0); \
}
MR_DEBLK_CHROMA_BS4_WRAP(deblk_chroma_vert_bs4)
MR_DEBLK_CHROMA_BS4_WRAP(deblk_chroma_horz_bs4)
#undef MR_DEBLK_CHROMA_BS4_WRAP

#define MR_DEBLK_CHROMA_BSLT4_WRAP(NAME) \
static ih264_deblk_chroma_edge_bslt4_ft *g_##NAME##_orig; \
static void NAME##_wrap(UWORD8 *pu1_src, WORD32 src_strd, WORD32 alpha_cb, \
                        WORD32 beta_cb, WORD32 alpha_cr, WORD32 beta_cr, \
                        UWORD32 u4_bs, const UWORD8 *pu1_cliptab_cb, \
                        const UWORD8 *pu1_cliptab_cr) \
{ \
    clock_t t0 = clock(); \
    g_##NAME##_orig(pu1_src, src_strd, alpha_cb, beta_cb, alpha_cr, beta_cr, \
                    u4_bs, pu1_cliptab_cb, pu1_cliptab_cr); \
    g_deblock_us += stage_elapsed_us(t0); \
}
MR_DEBLK_CHROMA_BSLT4_WRAP(deblk_chroma_vert_bslt4)
MR_DEBLK_CHROMA_BSLT4_WRAP(deblk_chroma_horz_bslt4)
#undef MR_DEBLK_CHROMA_BSLT4_WRAP

/* ---- IDCT/reconstruction: 4 luma + 2 chroma variants -------------------- */
#define MR_RECON_LUMA_WRAP(NAME) \
static ih264_iquant_itrans_recon_ft *g_##NAME##_orig; \
static void NAME##_wrap(WORD16 *pi2_src, UWORD8 *pu1_pred, UWORD8 *pu1_out, \
                        WORD32 pred_strd, WORD32 out_strd, \
                        const UWORD16 *pu2_iscale_mat, \
                        const UWORD16 *pu2_weigh_mat, UWORD32 qp_div, \
                        WORD16 *pi2_tmp, WORD32 iq_start_idx, \
                        WORD16 *pi2_dc_ld_addr) \
{ \
    clock_t t0 = clock(); \
    g_##NAME##_orig(pi2_src, pu1_pred, pu1_out, pred_strd, out_strd, \
                    pu2_iscale_mat, pu2_weigh_mat, qp_div, pi2_tmp, \
                    iq_start_idx, pi2_dc_ld_addr); \
    g_recon_us += stage_elapsed_us(t0); \
}
MR_RECON_LUMA_WRAP(recon_luma_4x4)
MR_RECON_LUMA_WRAP(recon_luma_4x4_dc)
MR_RECON_LUMA_WRAP(recon_luma_8x8)
MR_RECON_LUMA_WRAP(recon_luma_8x8_dc)
#undef MR_RECON_LUMA_WRAP

#define MR_RECON_CHROMA_WRAP(NAME) \
static ih264_iquant_itrans_recon_chroma_ft *g_##NAME##_orig; \
static void NAME##_wrap(WORD16 *pi2_src, UWORD8 *pu1_pred, UWORD8 *pu1_out, \
                        WORD32 pred_strd, WORD32 out_strd, \
                        const UWORD16 *pu2_iscal_mat, \
                        const UWORD16 *pu2_weigh_mat, UWORD32 u4_qp_div_6, \
                        WORD16 *pi2_tmp, WORD16 *pi2_dc_src) \
{ \
    clock_t t0 = clock(); \
    g_##NAME##_orig(pi2_src, pu1_pred, pu1_out, pred_strd, out_strd, \
                    pu2_iscal_mat, pu2_weigh_mat, u4_qp_div_6, pi2_tmp, \
                    pi2_dc_src); \
    g_recon_us += stage_elapsed_us(t0); \
}
MR_RECON_CHROMA_WRAP(recon_chroma_4x4)
MR_RECON_CHROMA_WRAP(recon_chroma_4x4_dc)
#undef MR_RECON_CHROMA_WRAP

/* ---- intra prediction: luma 16x16/8x8/4x4 modes + chroma + 8x8 ref-sample
 * filtering. apf_intra_pred_luma_16x16[0]/[1] (vert/horz) may already be
 * m68k asm by the time this installs (ih264d_function_selector_port.c runs
 * first) - captured into g_intra_luma16x16_orig[] like every other slot, so
 * whichever implementation is live gets measured. */
static ih264_intra_pred_luma_ft *g_intra_luma16x16_orig[4];
#define MR_INTRA16_WRAP(N) \
static void intra_luma16x16_wrap_##N(UWORD8 *pu1_src, UWORD8 *pu1_dst, \
                                     WORD32 src_strd, WORD32 dst_strd, \
                                     WORD32 ngbr_avail) \
{ \
    clock_t t0 = clock(); \
    g_intra_luma16x16_orig[N](pu1_src, pu1_dst, src_strd, dst_strd, \
                              ngbr_avail); \
    g_intra_us += stage_elapsed_us(t0); \
}
MR_INTRA16_WRAP(0) MR_INTRA16_WRAP(1) MR_INTRA16_WRAP(2) MR_INTRA16_WRAP(3)
#undef MR_INTRA16_WRAP
static ih264_intra_pred_luma_ft * const g_intra_luma16x16_wrap[4] = {
    intra_luma16x16_wrap_0, intra_luma16x16_wrap_1,
    intra_luma16x16_wrap_2, intra_luma16x16_wrap_3
};

static ih264_intra_pred_luma_ft *g_intra_luma8x8_orig[9];
#define MR_INTRA8_WRAP(N) \
static void intra_luma8x8_wrap_##N(UWORD8 *pu1_src, UWORD8 *pu1_dst, \
                                   WORD32 src_strd, WORD32 dst_strd, \
                                   WORD32 ngbr_avail) \
{ \
    clock_t t0 = clock(); \
    g_intra_luma8x8_orig[N](pu1_src, pu1_dst, src_strd, dst_strd, \
                            ngbr_avail); \
    g_intra_us += stage_elapsed_us(t0); \
}
MR_INTRA8_WRAP(0) MR_INTRA8_WRAP(1) MR_INTRA8_WRAP(2)
MR_INTRA8_WRAP(3) MR_INTRA8_WRAP(4) MR_INTRA8_WRAP(5)
MR_INTRA8_WRAP(6) MR_INTRA8_WRAP(7) MR_INTRA8_WRAP(8)
#undef MR_INTRA8_WRAP
static ih264_intra_pred_luma_ft * const g_intra_luma8x8_wrap[9] = {
    intra_luma8x8_wrap_0, intra_luma8x8_wrap_1, intra_luma8x8_wrap_2,
    intra_luma8x8_wrap_3, intra_luma8x8_wrap_4, intra_luma8x8_wrap_5,
    intra_luma8x8_wrap_6, intra_luma8x8_wrap_7, intra_luma8x8_wrap_8
};

static ih264_intra_pred_luma_ft *g_intra_luma4x4_orig[9];
#define MR_INTRA4_WRAP(N) \
static void intra_luma4x4_wrap_##N(UWORD8 *pu1_src, UWORD8 *pu1_dst, \
                                   WORD32 src_strd, WORD32 dst_strd, \
                                   WORD32 ngbr_avail) \
{ \
    clock_t t0 = clock(); \
    g_intra_luma4x4_orig[N](pu1_src, pu1_dst, src_strd, dst_strd, \
                            ngbr_avail); \
    g_intra_us += stage_elapsed_us(t0); \
}
MR_INTRA4_WRAP(0) MR_INTRA4_WRAP(1) MR_INTRA4_WRAP(2)
MR_INTRA4_WRAP(3) MR_INTRA4_WRAP(4) MR_INTRA4_WRAP(5)
MR_INTRA4_WRAP(6) MR_INTRA4_WRAP(7) MR_INTRA4_WRAP(8)
#undef MR_INTRA4_WRAP
static ih264_intra_pred_luma_ft * const g_intra_luma4x4_wrap[9] = {
    intra_luma4x4_wrap_0, intra_luma4x4_wrap_1, intra_luma4x4_wrap_2,
    intra_luma4x4_wrap_3, intra_luma4x4_wrap_4, intra_luma4x4_wrap_5,
    intra_luma4x4_wrap_6, intra_luma4x4_wrap_7, intra_luma4x4_wrap_8
};

static ih264_intra_pred_chroma_ft *g_intra_chroma_orig[4];
#define MR_INTRA_CHROMA_WRAP(N) \
static void intra_chroma_wrap_##N(UWORD8 *pu1_src, UWORD8 *pu1_dst, \
                                  WORD32 src_strd, WORD32 dst_strd, \
                                  WORD32 ngbr_avail) \
{ \
    clock_t t0 = clock(); \
    g_intra_chroma_orig[N](pu1_src, pu1_dst, src_strd, dst_strd, \
                           ngbr_avail); \
    g_intra_us += stage_elapsed_us(t0); \
}
MR_INTRA_CHROMA_WRAP(0) MR_INTRA_CHROMA_WRAP(1)
MR_INTRA_CHROMA_WRAP(2) MR_INTRA_CHROMA_WRAP(3)
#undef MR_INTRA_CHROMA_WRAP
static ih264_intra_pred_chroma_ft * const g_intra_chroma_wrap[4] = {
    intra_chroma_wrap_0, intra_chroma_wrap_1,
    intra_chroma_wrap_2, intra_chroma_wrap_3
};

static ih264_intra_pred_ref_filtering_ft *g_intra_ref_filt_orig;
static void intra_ref_filt_wrap(UWORD8 *pu1_left, UWORD8 *pu1_topleft,
                                UWORD8 *pu1_top, UWORD8 *pu1_dst,
                                WORD32 left_strd, WORD32 ngbr_avail)
{
    clock_t t0 = clock();
    g_intra_ref_filt_orig(pu1_left, pu1_topleft, pu1_top, pu1_dst, left_strd,
                          ngbr_avail);
    g_intra_us += stage_elapsed_us(t0);
}

/* Set once mr_h264_stage_profile_install() has run, so
 * mr_h264_stage_profile_rewrap_mc() can tell "the table changed under us and
 * needs re-wrapping" from "the profiler is not installed at all". Without it,
 * the rewrap called from mr_h264_port_install_inter_pred() during
 * ih264d_init_function_ptr() would wrap the table before install() got to it,
 * and install() would then capture those wrappers as the originals. */
static int g_installed;

static void wrap_mc_table(dec_struct_t *codec)
{
    int i;
    for (i = 0; i < 16; i++) {
        g_mc_orig[i] = codec->apf_inter_pred_luma[i];
        codec->apf_inter_pred_luma[i] = g_mc_wrap[i];
    }
    g_mc_chroma_orig = codec->pf_inter_pred_chroma;
    codec->pf_inter_pred_chroma = mc_chroma_wrap;
}

/* mr_h264_set_speed_mode() swaps whole inter-prediction filter sets into
 * apf_inter_pred_luma[] (ih264_mc_degrade.c), which overwrites these
 * wrappers. Re-apply them to the new filters so mc_us keeps reporting after a
 * mid-stream mode change instead of silently reading zero. Re-wrapping only
 * the MC slots, rather than calling install() again, is what keeps a wrapper
 * from being captured as another wrapper's original. */
void mr_h264_stage_profile_rewrap_mc(void *codec_v)
{
    if (!g_installed || !codec_v) return;
    wrap_mc_table((dec_struct_t *)codec_v);
}

void mr_h264_stage_profile_install(void *codec_v)
{
    dec_struct_t *codec = (dec_struct_t *)codec_v;
    int i;
    wrap_mc_table(codec);
    g_installed = 1;

    g_deblk_luma_vert_bs4_orig = codec->pf_deblk_luma_vert_bs4;
    codec->pf_deblk_luma_vert_bs4 = deblk_luma_vert_bs4_wrap;
    g_deblk_luma_horz_bs4_orig = codec->pf_deblk_luma_horz_bs4;
    codec->pf_deblk_luma_horz_bs4 = deblk_luma_horz_bs4_wrap;
    g_deblk_luma_vert_bslt4_orig = codec->pf_deblk_luma_vert_bslt4;
    codec->pf_deblk_luma_vert_bslt4 = deblk_luma_vert_bslt4_wrap;
    g_deblk_luma_horz_bslt4_orig = codec->pf_deblk_luma_horz_bslt4;
    codec->pf_deblk_luma_horz_bslt4 = deblk_luma_horz_bslt4_wrap;
    g_deblk_chroma_vert_bs4_orig = codec->pf_deblk_chroma_vert_bs4;
    codec->pf_deblk_chroma_vert_bs4 = deblk_chroma_vert_bs4_wrap;
    g_deblk_chroma_horz_bs4_orig = codec->pf_deblk_chroma_horz_bs4;
    codec->pf_deblk_chroma_horz_bs4 = deblk_chroma_horz_bs4_wrap;
    g_deblk_chroma_vert_bslt4_orig = codec->pf_deblk_chroma_vert_bslt4;
    codec->pf_deblk_chroma_vert_bslt4 = deblk_chroma_vert_bslt4_wrap;
    g_deblk_chroma_horz_bslt4_orig = codec->pf_deblk_chroma_horz_bslt4;
    codec->pf_deblk_chroma_horz_bslt4 = deblk_chroma_horz_bslt4_wrap;

    g_recon_luma_4x4_orig = codec->pf_iquant_itrans_recon_luma_4x4;
    codec->pf_iquant_itrans_recon_luma_4x4 = recon_luma_4x4_wrap;
    g_recon_luma_4x4_dc_orig = codec->pf_iquant_itrans_recon_luma_4x4_dc;
    codec->pf_iquant_itrans_recon_luma_4x4_dc = recon_luma_4x4_dc_wrap;
    g_recon_luma_8x8_orig = codec->pf_iquant_itrans_recon_luma_8x8;
    codec->pf_iquant_itrans_recon_luma_8x8 = recon_luma_8x8_wrap;
    g_recon_luma_8x8_dc_orig = codec->pf_iquant_itrans_recon_luma_8x8_dc;
    codec->pf_iquant_itrans_recon_luma_8x8_dc = recon_luma_8x8_dc_wrap;
    g_recon_chroma_4x4_orig = codec->pf_iquant_itrans_recon_chroma_4x4;
    codec->pf_iquant_itrans_recon_chroma_4x4 = recon_chroma_4x4_wrap;
    g_recon_chroma_4x4_dc_orig = codec->pf_iquant_itrans_recon_chroma_4x4_dc;
    codec->pf_iquant_itrans_recon_chroma_4x4_dc = recon_chroma_4x4_dc_wrap;

    for (i = 0; i < 4; i++) {
        g_intra_luma16x16_orig[i] = codec->apf_intra_pred_luma_16x16[i];
        codec->apf_intra_pred_luma_16x16[i] = g_intra_luma16x16_wrap[i];
    }
    for (i = 0; i < 9; i++) {
        g_intra_luma8x8_orig[i] = codec->apf_intra_pred_luma_8x8[i];
        codec->apf_intra_pred_luma_8x8[i] = g_intra_luma8x8_wrap[i];
    }
    for (i = 0; i < 9; i++) {
        g_intra_luma4x4_orig[i] = codec->apf_intra_pred_luma_4x4[i];
        codec->apf_intra_pred_luma_4x4[i] = g_intra_luma4x4_wrap[i];
    }
    for (i = 0; i < 4; i++) {
        g_intra_chroma_orig[i] = codec->apf_intra_pred_chroma[i];
        codec->apf_intra_pred_chroma[i] = g_intra_chroma_wrap[i];
    }
    g_intra_ref_filt_orig = codec->pf_intra_pred_ref_filtering;
    codec->pf_intra_pred_ref_filtering = intra_ref_filt_wrap;
}

void mr_h264_stage_profile_reset(void)
{
    g_mc_us = 0;
    g_deblock_us = 0;
    g_recon_us = 0;
    g_intra_us = 0;
}

void mr_h264_stage_profile_get(mr_h264_stage_us *out)
{
    out->mc_us = g_mc_us;
    out->deblock_us = g_deblock_us;
    out->recon_us = g_recon_us;
    out->intra_us = g_intra_us;
}
