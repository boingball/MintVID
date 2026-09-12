/*
 * m68k linker-level override for ih264d_parse_residual4x4_cabac() -
 * CABAC residual coefficient parsing's entry point for Luma/Chroma AC
 * residuals (called once per non-skip inter MB and once per intra MB,
 * from ih264d_parse_bslice.c/_islice.c/_pslice.c). See ih264_m68k_cabac_
 * coeff.S for the hand-asm primitive this calls into and its full
 * derivation/verification notes.
 *
 * Same situation as the earlier motion-vector-prediction port
 * (ih264d_mvpred_dispatch_port.c), and for the same underlying reason:
 * ih264d_read_coeff4x4_cabac() - the actual hot CABAC arithmetic, now
 * hand-asm'd - has ~20 real call sites, but all except one are *inside*
 * its own defining file, ih264d_parse_cabac.c. A same-object direct call
 * between two non-static C functions resolves to a branch with no
 * relocation entry left for the linker to redirect - GNU ld's --wrap
 * cannot intercept it (confirmed with a minimal repro during the mvpred
 * port, and the hard way here too: a first attempt wrapping
 * ih264d_read_coeff4x4_cabac directly linked cleanly and simply never
 * executed, caught by a qemu -d exec trace showing 100% of hits still on
 * the original C).
 *
 * The one call site that *is* genuinely cross-file - ih264d_parse_
 * islice.c:687, decoding the Intra16x16 luma DC block - is handled by
 * wrapping ih264d_read_coeff4x4_cabac itself (see libavc.mk); this file
 * only needs to handle the other five, all reachable through
 * ih264d_parse_residual4x4_cabac() and the small orchestrator it calls,
 * ih264d_cabac_parse_8x8block() (also same-file-only called, so also
 * reimplemented here rather than wrapped).
 *
 * mr_cabac_parse_8x8block() below is a straight transcription of
 * ih264d_cabac_parse_8x8block() (vendor/libavc/decoder/ih264d_parse_
 * cabac.c lines 1066-1147) - four sequential 4x4 sub-block decodes with
 * NNZ-neighbour context bookkeeping between them, no CABAC arithmetic of
 * its own. __wrap_ih264d_parse_residual4x4_cabac() is a straight
 * transcription of ih264d_parse_residual4x4_cabac() (same file, lines
 * 1161-1606) - CBP-gated calls into the above (or, for the transform8x8
 * path, into mr_ih264d_read_coeff8x8_cabac_m68k() - see below) plus the
 * Chroma DC/AC decode tail. Both were mechanically diffed against the
 * vendored source (comments/whitespace stripped) to confirm byte-for-byte
 * equivalence except for the three intentional changes: calls to
 * ih264d_read_coeff4x4_cabac / ih264d_cabac_parse_8x8block /
 * ih264d_read_coeff8x8_cabac redirected to the reimplementation/primitives
 * below and in ih264_m68k_cabac_coeff8x8.S.
 *
 * ih264d_read_coeff8x8_cabac() (the transform_8x8_flag / High Profile
 * sibling primitive, ih264d_parse_cabac.c lines 582-1025) was originally
 * left untouched here on the assumption that "this project has no High-
 * Profile/transform8x8 test content" - that assumption turned out to be
 * false (libx264's High Profile preset enables 8x8dct by default,
 * including for this project's own test_h264_high.mp4 fixture) and a
 * stage-profiled run showed real, non-trivial call volume on High Profile
 * content, so it is now ported too: see ih264_m68k_cabac_coeff8x8.S for
 * the primitive and its own derivation notes (in particular, why it is
 * *not* just the 4x4 primitive with a different context-array pointer -
 * the 8x8 significance/last-coefficient context indexing is table-driven,
 * not a flat per-position stride, and getting that specific piece wrong
 * would not have failed loudly).
 *
 * Verification: the primitive itself (mr_ih264d_read_coeff4x4_cabac_
 * m68k) is fuzz-tested directly against the real vendored
 * ih264d_read_coeff4x4_cabac() - see check_read_coeff4x4_cabac() in
 * tests/mr_h264_cabac_coeff_check.c - 20,000 iterations, real m68k/qemu
 * execution, every field of the real dec_struct_t/bitstream/context
 * state this function touches compared bit-exact. The two functions in
 * *this* file were verified by the mechanical source diff described
 * above (their own logic is unchanged from the vendored source; only the
 * primitive underneath changed, and that is independently proven
 * correct). Plus the full make check-m68k H.264-vs-ffmpeg conformance
 * suite (worst per-frame MAE must stay unchanged), and a qemu -d exec
 * trace confirming the asm primitive's own address - not the original
 * vendored C, which should get zero hits from real decode once this is
 * wired in - is what actually executes.
 */
#include "ih264_typedefs.h"
#include "ih264d_structs.h"
#include "ih264d_defs.h"
#include "ih264d_mb_utils.h"
#include "ih264_macros.h"
#include "ih264d_cabac.h"
#include "ih264d_bitstrm.h"
#include "ih264d_parse_cabac.h"
#include "ih264d_tables.h"
#include "ih264d_utils.h"
#include "ih264_m68k_optim.h"

#if defined(MR_H264_CABAC_PROFILE)
#include "ih264d_cabac_profile.h"
#include <time.h>
#endif

#if defined(MR_M68K_ASM)
#define READ_COEFF4X4(bitstrm, ctxcat, sig_ctx, dec, coded_ctx) \
    mr_ih264d_read_coeff4x4_cabac_m68k((void *)(bitstrm), (ctxcat), \
        (UWORD8 *)(sig_ctx), (void *)(dec), (UWORD8 *)(coded_ctx))

/* Transcribed from ih264d_cabac_parse_8x8block() - see file header. */
static UWORD32 mr_cabac_parse_8x8block(WORD16 *pi2_coeff_block,
                                       UWORD32 u4_sub_block_strd,
                                       UWORD32 u4_ctx_cat,
                                       dec_struct_t *ps_dec,
                                       UWORD8 *pu1_top_nnz,
                                       UWORD8 *pu1_left_nnz)
{
    UWORD32 u4_ctxinc, u4_subblock_coded;
    UWORD32 u4_top0, u4_top1;
    UWORD32 u4_csbp = 0;
    UWORD32 u4_idx = 0;
    dec_bit_stream_t *const ps_bitstrm = ps_dec->ps_bitstrm;
    bin_ctxt_model_t *const ps_cbf = ps_dec->p_cbf_t[u4_ctx_cat];
    bin_ctxt_model_t *ps_src_bin_ctxt;
    bin_ctxt_model_t *const ps_sig_coeff_flag =
        ps_dec->p_significant_coeff_flag_t[u4_ctx_cat];

    u4_ctxinc = ((!!pu1_top_nnz[0]) << 1) + (!!pu1_left_nnz[0]);
    ps_src_bin_ctxt = ps_cbf + u4_ctxinc;
    u4_top0 = READ_COEFF4X4(ps_bitstrm, u4_ctx_cat, ps_sig_coeff_flag, ps_dec,
                            ps_src_bin_ctxt);
    INSERT_BIT(u4_csbp, u4_idx, u4_top0);

    u4_idx++;
    pi2_coeff_block += NUM_COEFFS_IN_4x4BLK;
    u4_ctxinc = ((!!pu1_top_nnz[1]) << 1) + u4_top0;
    ps_src_bin_ctxt = ps_cbf + u4_ctxinc;
    u4_top1 = READ_COEFF4X4(ps_bitstrm, u4_ctx_cat, ps_sig_coeff_flag, ps_dec,
                            ps_src_bin_ctxt);
    INSERT_BIT(u4_csbp, u4_idx, u4_top1);
    pu1_left_nnz[0] = (UWORD8)u4_top1;

    u4_idx += (u4_sub_block_strd - 1);
    pi2_coeff_block += ((u4_sub_block_strd - 1) * NUM_COEFFS_IN_4x4BLK);
    u4_ctxinc = (u4_top0 << 1) + (!!pu1_left_nnz[1]);
    ps_src_bin_ctxt = ps_cbf + u4_ctxinc;
    u4_subblock_coded = READ_COEFF4X4(ps_bitstrm, u4_ctx_cat, ps_sig_coeff_flag,
                                      ps_dec, ps_src_bin_ctxt);
    INSERT_BIT(u4_csbp, u4_idx, u4_subblock_coded);
    pu1_top_nnz[0] = (UWORD8)u4_subblock_coded;

    u4_idx++;
    pi2_coeff_block += NUM_COEFFS_IN_4x4BLK;
    u4_ctxinc = (u4_top1 << 1) + u4_subblock_coded;
    ps_src_bin_ctxt = ps_cbf + u4_ctxinc;
    u4_subblock_coded = READ_COEFF4X4(ps_bitstrm, u4_ctx_cat, ps_sig_coeff_flag,
                                      ps_dec, ps_src_bin_ctxt);
    INSERT_BIT(u4_csbp, u4_idx, u4_subblock_coded);
    pu1_top_nnz[1] = pu1_left_nnz[1] = (UWORD8)u4_subblock_coded;

    return u4_csbp;
}

/* Transcribed from ih264d_parse_residual4x4_cabac() - see file header.
 * Named __wrap_... directly in a normal build (the production --wrap
 * target, no extra call layer); renamed to a plain static helper under
 * MR_H264_CABAC_PROFILE, where a thin timing trampoline below takes the
 * public name instead - this function's own body is untouched either way,
 * preserving the "mechanically diffed against the vendored source" claim
 * in the file header. */
#if defined(MR_H264_CABAC_PROFILE)
static WORD32 mr_parse_residual4x4_cabac_impl(dec_struct_t *ps_dec,
                                              dec_mb_info_t *ps_cur_mb_info,
                                              UWORD8 u1_offset)
#else
WORD32 __wrap_ih264d_parse_residual4x4_cabac(dec_struct_t *ps_dec,
                                             dec_mb_info_t *ps_cur_mb_info,
                                             UWORD8 u1_offset)
#endif
{
    UWORD8 u1_cbp = ps_cur_mb_info->u1_cbp;
    UWORD16 ui16_csbp = 0;
    WORD16 *pi2_coeff_block = NULL;
    UWORD8 uc_ctx_cat;
    UWORD8 *pu1_top_nnz = ps_cur_mb_info->ps_curmb->pu1_nnz_y;
    UWORD8 *pu1_left_nnz = ps_dec->pu1_left_nnz_y;
    UWORD8 *pu1_top_nnz_uv = ps_cur_mb_info->ps_curmb->pu1_nnz_uv;
    ctxt_inc_mb_info_t *p_curr_ctxt = ps_dec->ps_curr_ctxt_mb_info;
    ctxt_inc_mb_info_t *ps_top_ctxt = ps_dec->p_top_ctxt_mb_info;
    dec_bit_stream_t *const ps_bitstrm = ps_dec->ps_bitstrm;
    UWORD32 u4_nbr_avail = ps_dec->u1_mb_ngbr_availablity;
    bin_ctxt_model_t *ps_src_bin_ctxt;

    UWORD8 u1_top_dc_csbp = (ps_top_ctxt->u1_yuv_dc_csbp) >> 1;
    UWORD8 u1_left_dc_csbp = (ps_dec->pu1_left_yuv_dc_csbp[0]) >> 1;

    if (!(u4_nbr_avail & TOP_MB_AVAILABLE_MASK)) {
        if (p_curr_ctxt->u1_mb_type & CAB_INTRA_MASK) {
            *(UWORD32 *)pu1_top_nnz = 0;
            u1_top_dc_csbp = 0;
            *(UWORD32 *)pu1_top_nnz_uv = 0;
        } else {
            *(UWORD32 *)pu1_top_nnz = 0x01010101;
            u1_top_dc_csbp = 0x3;
            *(UWORD32 *)pu1_top_nnz_uv = 0x01010101;
        }
    } else {
        UWORD32 *pu4_buf;
        UWORD8 *pu1_buf;
        pu1_buf = ps_cur_mb_info->ps_top_mb->pu1_nnz_y;
        pu4_buf = (UWORD32 *)pu1_buf;
        *(UWORD32 *)(pu1_top_nnz) = *pu4_buf;

        pu1_buf = ps_cur_mb_info->ps_top_mb->pu1_nnz_uv;
        pu4_buf = (UWORD32 *)pu1_buf;
        *(UWORD32 *)(pu1_top_nnz_uv) = *pu4_buf;
    }

    if (!(u4_nbr_avail & LEFT_MB_AVAILABLE_MASK)) {
        if (p_curr_ctxt->u1_mb_type & CAB_INTRA_MASK) {
            UWORD32 *pu4_buf;
            UWORD8 *pu1_buf;
            *(UWORD32 *)pu1_left_nnz = 0;
            u1_left_dc_csbp = 0;
            pu1_buf = ps_dec->pu1_left_nnz_uv;
            pu4_buf = (UWORD32 *)pu1_buf;
            *pu4_buf = 0;
        } else {
            UWORD32 *pu4_buf;
            UWORD8 *pu1_buf;
            *(UWORD32 *)pu1_left_nnz = 0x01010101;
            u1_left_dc_csbp = 0x3;
            pu1_buf = ps_dec->pu1_left_nnz_uv;
            pu4_buf = (UWORD32 *)pu1_buf;
            *pu4_buf = 0x01010101;
        }
    }

    uc_ctx_cat = u1_offset ? LUMA_AC_CTXCAT : LUMA_4X4_CTXCAT;

    ps_cur_mb_info->u1_qp_div6 = ps_dec->u1_qp_y_div6;
    ps_cur_mb_info->u1_qpc_div6 = ps_dec->u1_qp_u_div6;
    ps_cur_mb_info->u1_qp_rem6 = ps_dec->u1_qp_y_rem6;
    ps_cur_mb_info->u1_qpc_rem6 = ps_dec->u1_qp_u_rem6;
    ps_cur_mb_info->u1_qpcr_div6 = ps_dec->u1_qp_v_div6;
    ps_cur_mb_info->u1_qpcr_rem6 = ps_dec->u1_qp_v_rem6;

    if (u1_cbp & 0x0f) {
        if (ps_cur_mb_info->u1_tran_form8x8 == 0) {
            if (!(u1_cbp & 0x1)) {
                *(UWORD16 *)(pu1_top_nnz) = 0;
                *(UWORD16 *)(pu1_left_nnz) = 0;
            } else {
                ui16_csbp = (UWORD16)mr_cabac_parse_8x8block(pi2_coeff_block, 4,
                    uc_ctx_cat, ps_dec, pu1_top_nnz, pu1_left_nnz);
            }

            pi2_coeff_block += (2 * NUM_COEFFS_IN_4x4BLK);
            if (!(u1_cbp & 0x2)) {
                *(UWORD16 *)(pu1_top_nnz + 2) = 0;
                *(UWORD16 *)(pu1_left_nnz) = 0;
            } else {
                UWORD32 u4_temp = mr_cabac_parse_8x8block(pi2_coeff_block, 4,
                    uc_ctx_cat, ps_dec, (pu1_top_nnz + 2), pu1_left_nnz);
                ui16_csbp |= (UWORD16)(u4_temp << 2);
            }

            pi2_coeff_block += (6 * NUM_COEFFS_IN_4x4BLK);
            if (!(u1_cbp & 0x4)) {
                *(UWORD16 *)(pu1_top_nnz) = 0;
                *(UWORD16 *)(pu1_left_nnz + 2) = 0;
            } else {
                UWORD32 u4_temp = mr_cabac_parse_8x8block(pi2_coeff_block, 4,
                    uc_ctx_cat, ps_dec, pu1_top_nnz, (pu1_left_nnz + 2));
                ui16_csbp |= (UWORD16)(u4_temp << 8);
            }

            pi2_coeff_block += (2 * NUM_COEFFS_IN_4x4BLK);
            if (!(u1_cbp & 0x8)) {
                *(UWORD16 *)(pu1_top_nnz + 2) = 0;
                *(UWORD16 *)(pu1_left_nnz + 2) = 0;
            } else {
                UWORD32 u4_temp = mr_cabac_parse_8x8block(pi2_coeff_block, 4,
                    uc_ctx_cat, ps_dec, (pu1_top_nnz + 2), (pu1_left_nnz + 2));
                ui16_csbp |= (UWORD16)(u4_temp << 10);
            }
        } else {
            ui16_csbp = 0;

            if (!(u1_cbp & 0x1)) {
                *(UWORD16 *)(pu1_top_nnz) = 0;
                *(UWORD16 *)(pu1_left_nnz) = 0;
            } else {
                mr_ih264d_read_coeff8x8_cabac_m68k(ps_bitstrm, ps_dec, ps_cur_mb_info);
                pu1_left_nnz[0] = 1;
                pu1_left_nnz[1] = 1;
                pu1_top_nnz[0] = 1;
                pu1_top_nnz[1] = 1;
                ui16_csbp |= 0x0033;
            }

            pi2_coeff_block += 64;
            if (!(u1_cbp & 0x2)) {
                *(UWORD16 *)(pu1_top_nnz + 2) = 0;
                *(UWORD16 *)(pu1_left_nnz) = 0;
            } else {
                mr_ih264d_read_coeff8x8_cabac_m68k(ps_bitstrm, ps_dec, ps_cur_mb_info);
                pu1_left_nnz[0] = 1;
                pu1_left_nnz[1] = 1;
                pu1_top_nnz[2] = 1;
                pu1_top_nnz[3] = 1;
                ui16_csbp |= 0x00CC;
            }

            pi2_coeff_block += 64;
            if (!(u1_cbp & 0x4)) {
                *(UWORD16 *)(pu1_top_nnz) = 0;
                *(UWORD16 *)(pu1_left_nnz + 2) = 0;
            } else {
                mr_ih264d_read_coeff8x8_cabac_m68k(ps_bitstrm, ps_dec, ps_cur_mb_info);
                pu1_left_nnz[2] = 1;
                pu1_left_nnz[3] = 1;
                pu1_top_nnz[0] = 1;
                pu1_top_nnz[1] = 1;
                ui16_csbp |= 0x3300;
            }

            pi2_coeff_block += 64;
            if (!(u1_cbp & 0x8)) {
                *(UWORD16 *)(pu1_top_nnz + 2) = 0;
                *(UWORD16 *)(pu1_left_nnz + 2) = 0;
            } else {
                mr_ih264d_read_coeff8x8_cabac_m68k(ps_bitstrm, ps_dec, ps_cur_mb_info);
                pu1_left_nnz[2] = 1;
                pu1_left_nnz[3] = 1;
                pu1_top_nnz[2] = 1;
                pu1_top_nnz[3] = 1;
                ui16_csbp |= 0xCC00;
            }
        }
    } else {
        *(UWORD32 *)(pu1_top_nnz) = 0;
        *(UWORD32 *)(pu1_left_nnz) = 0;
    }

    ps_cur_mb_info->u2_luma_csbp = ui16_csbp;
    ps_cur_mb_info->ps_curmb->u2_luma_csbp = ui16_csbp;
    {
        WORD8 i;
        UWORD16 u2_chroma_csbp = 0;
        ps_cur_mb_info->u2_chroma_csbp = 0;

        u1_cbp >>= 4;
        pu1_top_nnz = pu1_top_nnz_uv;
        pu1_left_nnz = ps_dec->pu1_left_nnz_uv;

        if (u1_cbp == CBPC_ALLZERO) {
            ps_dec->pu1_left_yuv_dc_csbp[0] &= 0x1;
            *(UWORD32 *)(pu1_top_nnz) = 0;
            *(UWORD32 *)(pu1_left_nnz) = 0;
            p_curr_ctxt->u1_yuv_dc_csbp &= 0x1;
            return 0;
        }

        for (i = 0; i < 2; i++) {
            UWORD8 uc_a = 1, uc_b = 1;
            UWORD32 u4_ctx_inc;
            UWORD8 uc_codedBlockFlag;
            UWORD8 pu1_inv_scan[4] = { 0, 1, 2, 3 };
            WORD32 u4_scale;
            WORD32 i4_mb_inter_inc;
            tu_sblk4x4_coeff_data_t *ps_tu_4x4 =
                (tu_sblk4x4_coeff_data_t *)ps_dec->pv_parse_tu_coeff_data;
            WORD16 *pi2_coeff_data = (WORD16 *)ps_dec->pv_parse_tu_coeff_data;
            WORD16 ai2_dc_coef[4];

            u4_scale = (i) ?
                (ps_dec->pu2_quant_scale_v[0] << ps_dec->u1_qp_v_div6) :
                (ps_dec->pu2_quant_scale_u[0] << ps_dec->u1_qp_u_div6);

            uc_a = (u1_left_dc_csbp >> i) & 0x01;
            uc_b = (u1_top_dc_csbp >> i) & 0x01;
            u4_ctx_inc = (uc_a + (uc_b << 1));

            ps_src_bin_ctxt = (ps_dec->p_cbf_t[CHROMA_DC_CTXCAT]) + u4_ctx_inc;

            uc_codedBlockFlag = (UWORD8)READ_COEFF4X4(ps_bitstrm,
                CHROMA_DC_CTXCAT,
                ps_dec->p_significant_coeff_flag_t[CHROMA_DC_CTXCAT], ps_dec,
                ps_src_bin_ctxt);

            i4_mb_inter_inc = (!((ps_cur_mb_info->ps_curmb->u1_mb_type == I_4x4_MB)
                || (ps_cur_mb_info->ps_curmb->u1_mb_type == I_16x16_MB))) * 3;

            if (ps_dec->s_high_profile.u1_scaling_present) {
                u4_scale *= ps_dec->s_high_profile.i2_scalinglist4x4[
                    i4_mb_inter_inc + 1 + i][0];
            } else {
                u4_scale <<= 4;
            }

            if (uc_codedBlockFlag) {
                WORD32 i_z0, i_z1, i_z2, i_z3;

                SET_BIT(u1_top_dc_csbp, i);
                SET_BIT(u1_left_dc_csbp, i);

                ai2_dc_coef[0] = 0;
                ai2_dc_coef[1] = 0;
                ai2_dc_coef[2] = 0;
                ai2_dc_coef[3] = 0;

                ih264d_unpack_coeff4x4_dc_4x4blk(ps_tu_4x4, ai2_dc_coef,
                                                 pu1_inv_scan);
                i_z0 = (ai2_dc_coef[0] + ai2_dc_coef[2]);
                i_z1 = (ai2_dc_coef[0] - ai2_dc_coef[2]);
                i_z2 = (ai2_dc_coef[1] - ai2_dc_coef[3]);
                i_z3 = (ai2_dc_coef[1] + ai2_dc_coef[3]);

                *pi2_coeff_data++ = (WORD16)(((i_z0 + i_z3) * u4_scale) >> 5);
                *pi2_coeff_data++ = (WORD16)(((i_z0 - i_z3) * u4_scale) >> 5);
                *pi2_coeff_data++ = (WORD16)(((i_z1 + i_z2) * u4_scale) >> 5);
                *pi2_coeff_data++ = (WORD16)(((i_z1 - i_z2) * u4_scale) >> 5);

                ps_dec->pv_parse_tu_coeff_data = (void *)pi2_coeff_data;

                SET_BIT(ps_cur_mb_info->u1_yuv_dc_block_flag, (i + 1));
            } else {
                CLEARBIT(u1_top_dc_csbp, i);
                CLEARBIT(u1_left_dc_csbp, i);
            }
        }

        ps_dec->pu1_left_yuv_dc_csbp[0] &= 0x1;
        p_curr_ctxt->u1_yuv_dc_csbp &= 0x1;
        ps_dec->pu1_left_yuv_dc_csbp[0] |= (u1_left_dc_csbp << 1);
        p_curr_ctxt->u1_yuv_dc_csbp |= (u1_top_dc_csbp << 1);
        if (u1_cbp == CBPC_ACZERO) {
            *(UWORD32 *)(pu1_top_nnz) = 0;
            *(UWORD32 *)(pu1_left_nnz) = 0;
            return 0;
        }

        {
            UWORD32 u4_temp;
            u2_chroma_csbp = (UWORD16)mr_cabac_parse_8x8block(pi2_coeff_block, 2,
                CHROMA_AC_CTXCAT, ps_dec, pu1_top_nnz, pu1_left_nnz);

            pi2_coeff_block += MB_CHROM_SIZE;
            u4_temp = mr_cabac_parse_8x8block(pi2_coeff_block, 2,
                CHROMA_AC_CTXCAT, ps_dec, (pu1_top_nnz + 2),
                (pu1_left_nnz + 2));
            u2_chroma_csbp |= (UWORD16)(u4_temp << 4);
        }
        ps_cur_mb_info->u2_chroma_csbp = u2_chroma_csbp;
    }

    return 0;
}

#if defined(MR_H264_CABAC_PROFILE)
/* Both wrap functions in this file feed the same coeff_us/coeff_count
 * bucket (see ih264d_cabac_profile.h) - residual coefficient parsing is one
 * conceptual stage from a profiling point of view, regardless of which of
 * the two entry points below is on the call stack. */
WORD32 __wrap_ih264d_parse_residual4x4_cabac(dec_struct_t *ps_dec,
                                             dec_mb_info_t *ps_cur_mb_info,
                                             UWORD8 u1_offset)
{
    WORD32 ret;
    clock_t t0 = clock();
    ret = mr_parse_residual4x4_cabac_impl(ps_dec, ps_cur_mb_info, u1_offset);
    mr_h264_cabac_profile_add_coeff(
        (unsigned long)((clock() - t0) * 1000000UL / CLOCKS_PER_SEC));
    return ret;
}
#endif

/*
 * ih264d_read_coeff4x4_cabac()'s one genuinely cross-file call site -
 * ih264d_parse_islice.c:687, decoding the Intra16x16 luma DC block -
 * *is* a normal linker reference this project's own code doesn't
 * originate, so --wrap works on it directly, same as ih264d_decode_bin.
 * Named __wrap_... directly in a normal build; renamed to a plain static
 * helper under MR_H264_CABAC_PROFILE, same split as the function above.
 */
#if defined(MR_H264_CABAC_PROFILE)
static UWORD8 mr_read_coeff4x4_cabac_impl(dec_bit_stream_t *ps_bitstrm,
                                          UWORD32 u4_ctxcat,
                                         bin_ctxt_model_t *ps_ctxt_sig_coeff,
                                         dec_struct_t *ps_dec,
                                         bin_ctxt_model_t *ps_ctxt_coded)
#else
UWORD8 __wrap_ih264d_read_coeff4x4_cabac(dec_bit_stream_t *ps_bitstrm,
                                         UWORD32 u4_ctxcat,
                                         bin_ctxt_model_t *ps_ctxt_sig_coeff,
                                         dec_struct_t *ps_dec,
                                         bin_ctxt_model_t *ps_ctxt_coded)
#endif
{
    return READ_COEFF4X4(ps_bitstrm, u4_ctxcat, ps_ctxt_sig_coeff, ps_dec,
                         ps_ctxt_coded);
}

#if defined(MR_H264_CABAC_PROFILE)
UWORD8 __wrap_ih264d_read_coeff4x4_cabac(dec_bit_stream_t *ps_bitstrm,
                                         UWORD32 u4_ctxcat,
                                         bin_ctxt_model_t *ps_ctxt_sig_coeff,
                                         dec_struct_t *ps_dec,
                                         bin_ctxt_model_t *ps_ctxt_coded)
{
    UWORD8 ret;
    clock_t t0 = clock();
    ret = mr_read_coeff4x4_cabac_impl(ps_bitstrm, u4_ctxcat, ps_ctxt_sig_coeff,
                                      ps_dec, ps_ctxt_coded);
    mr_h264_cabac_profile_add_coeff(
        (unsigned long)((clock() - t0) * 1000000UL / CLOCKS_PER_SEC));
    return ret;
}
#endif
#endif /* MR_M68K_ASM */
