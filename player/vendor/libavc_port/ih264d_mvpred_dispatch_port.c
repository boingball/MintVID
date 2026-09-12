/*
 * m68k linker-level override for ih264d_mvpred_nonmbaff() /
 * ih264d_mvpred_nonmbaffB() - the two "dispatcher" entry points assigned
 * to dec_struct_t::pf_mvpred for non-MBAFF P/B slices (MVPRED_NONMBAFF is
 * never #define'd anywhere in vendor/libavc, so these are the always-
 * active non-MBAFF path; see ih264d_parse_slice.c/ih264d_parse_pslice.c
 * for the by-name assignments this file's --wrap flags redirect).
 * ih264d_mvpred_mbaff (MBAFF slices) is deliberately left untouched and
 * unwrapped - see the bottom of this comment for why.
 *
 * Unlike the CABAC/MC/deblock work, this is NOT a case of "no swap point
 * exists" for the underlying primitive - dec_struct_t::pf_mvpred IS a
 * function pointer, and IS assigned to ih264d_mvpred_nonmbaff/_nonmbaffB
 * by name from a different file than the one that defines them, so
 * -Wl,--wrap works exactly like it does for ih264d_decode_bin (see
 * ih264d_cabac_wrap.c for that base pattern).
 *
 * What's different, and why this file reimplements two ~200-line
 * dispatcher functions instead of just wrapping the small (45-line)
 * arithmetic primitive at their core, is that every one of that
 * primitive's ~20 real call sites is *inside* ih264d_mvpred.c itself -
 * the same file that defines it. A same-object direct call between two
 * non-static C functions in one translation unit gets resolved by the
 * assembler to a fixed intra-section branch with no relocation entry at
 * all - confirmed with a minimal two-function repro (`objdump -r` on the
 * .o shows zero relocations for such a call, and the final linked
 * binary's disassembly branches straight to the original definition
 * regardless of --wrap on that symbol name). GNU ld's --wrap only ever
 * redirects a symbol *reference* the linker still has to resolve, and a
 * same-object direct branch has none left to redirect by the time the
 * linker sees it - wrapping ih264d_get_motion_vector_predictor itself
 * was tried first and is a genuine no-op: it links cleanly and appears
 * to work, but never actually executes. Confirmed the hard way, via a
 * qemu -d exec address trace showing 100% of hits still landing on the
 * original vendored function's address after wiring that wrap up "the
 * normal way" - not assumed from the repro alone.
 *
 * The fix: reimplement the two dispatchers themselves as portable C
 * (below), transcribed line-for-line from ih264d_mvpred_nonmbaff() /
 * ih264d_mvpred_nonmbaffB() in ih264d_mvpred.c, changing only how the
 * primitive is invoked - MV_PRED() calls the hand-asm
 * mr_ih264d_get_motion_vector_predictor_m68k() (ih264_m68k_mvpred.S)
 * instead of the vendored C. Every other line matches the vendored
 * source exactly, including the ordering and side effects of
 * ih264d_non_mbaff_mv_pred() (the neighbour-lookup helper that populates
 * ps_mv_pred[LEFT]/[TOP]/[TOP_R]) - that helper is called here exactly
 * as the vendored dispatchers call it, unmodified, via a completely
 * ordinary cross-file reference (this file is not ih264d_mvpred.c, so
 * there is no same-object-call problem calling *into* it - the problem
 * was only ever with intercepting calls *inside* that file).
 *
 * ih264d_mvpred_mbaff (MBAFF slices) is deliberately left untouched and
 * unwrapped - this project has no MBAFF test content and no reliable way
 * to verify a from-scratch reimplementation of its considerably branchier
 * field/frame neighbour logic against real hardware or ffmpeg right now.
 * Leaving it unwrapped is always safe regardless: any MBAFF-coded stream
 * simply keeps using the original, unmodified, correct C path -
 * unoptimised for that specific case, never incorrect.
 *
 * Verification: check_mvpred_dispatch() in
 * tests/mr_h264_mvpred_dispatch_check.c (a separate test binary, not
 * mr_h264_m68k_check.c - that file is deliberately kept free of libavc
 * headers/linking so it stays a lightweight portable-C-only build; this
 * one links the real vendored decoder, same pattern as mr_h264_cabac_
 * coeff_check.c) fuzzes randomised mv_pred_t neighbour-bank buffers and
 * dec_mb_info_t availability masks and asserts this reimplementation is
 * bit-exact against the real vendored ih264d_mvpred_nonmbaff()/
 * _nonmbaffB() (not a from-spec reference this time - the real functions
 * are directly callable in the same test binary, so there is no need to
 * re-derive one), covering every u1_mb_mc_mode case (PRED_16x8,
 * PRED_8x16, B_DIRECT_SPATIAL, MB_SKIP, default) and both the "neighbour
 * ref_idx already matches" shortcut and the actual median-predictor
 * path. Plus the full make check-m68k H.264-vs-ffmpeg conformance suite
 * (worst per-frame MAE must stay unchanged), and a qemu -d exec address trace
 * confirming the asm primitive's own address is what actually executes
 * during real decode.
 */
#include "ih264_typedefs.h"
#include "ih264d_structs.h"
#include "ih264d_defs.h"
#include "ih264d_mb_utils.h"
#include "ih264_macros.h"
#include "ih264d_mvpred.h"
#include "ih264d_tables.h"
#include "ih264_m68k_optim.h"

#if defined(MR_H264_CABAC_PROFILE)
#include "ih264d_cabac_profile.h"
#include <time.h>
#endif

#if defined(MR_M68K_ASM)
#define MV_PRED(result, pred, ref_idx, b) \
    mr_ih264d_get_motion_vector_predictor_m68k((UWORD8 *)(result), \
        (UWORD8 **)(pred), (ref_idx), (b), \
        (const UWORD8 *)gau1_ih264d_mv_pred_condition)

/* Named __wrap_... directly in a normal build; renamed to a plain static
 * helper under MR_H264_CABAC_PROFILE, where a thin timing trampoline below
 * takes the public name instead - same split as
 * ih264d_parse_cabac_coeff_port.c, and for the same reason: this function's
 * own body stays untouched either way. */
#if defined(MR_H264_CABAC_PROFILE)
static UWORD8 mr_mvpred_nonmbaffB_impl(dec_struct_t *ps_dec,
                                       dec_mb_info_t *ps_cur_mb_info,
                                       mv_pred_t *ps_mv_nmb,
                                       mv_pred_t *ps_mv_ntop,
                                       mv_pred_t *ps_mv_final_pred,
                                       UWORD32 u4_sub_mb_num,
                                       UWORD8 uc_mb_part_width,
                                       UWORD8 u1_lx_start,
                                       UWORD8 u1_lxend,
                                       UWORD8 u1_mb_mc_mode)
#else
UWORD8 __wrap_ih264d_mvpred_nonmbaffB(dec_struct_t *ps_dec,
                                      dec_mb_info_t *ps_cur_mb_info,
                                      mv_pred_t *ps_mv_nmb,
                                      mv_pred_t *ps_mv_ntop,
                                      mv_pred_t *ps_mv_final_pred,
                                      UWORD32 u4_sub_mb_num,
                                      UWORD8 uc_mb_part_width,
                                      UWORD8 u1_lx_start,
                                      UWORD8 u1_lxend,
                                      UWORD8 u1_mb_mc_mode)
#endif
{
    UWORD8 u1_a_in, u1_b_in, uc_temp1, uc_temp2, uc_temp3;
    mv_pred_t *ps_mv_pred[3];
    UWORD8 uc_B2, uc_lx, u1_ref_idx;
    UWORD8 u1_direct_zero_pred_flag = 0;

    ih264d_non_mbaff_mv_pred(ps_mv_pred, u4_sub_mb_num, ps_mv_nmb, ps_mv_ntop,
                             ps_dec, uc_mb_part_width, ps_cur_mb_info);

    for (uc_lx = u1_lx_start; uc_lx < u1_lxend; uc_lx++)
    {
        u1_ref_idx = ps_mv_final_pred->i1_ref_frame[uc_lx];
        uc_B2 = (uc_lx << 1);
        switch (u1_mb_mc_mode)
        {
            case PRED_16x8:
                if (u4_sub_mb_num == 0)
                {
                    if (ps_mv_pred[TOP]->i1_ref_frame[uc_lx] == u1_ref_idx)
                    {
                        ps_mv_final_pred->i2_mv[uc_B2 + 0] =
                                        ps_mv_pred[TOP]->i2_mv[uc_B2 + 0];
                        ps_mv_final_pred->i2_mv[uc_B2 + 1] =
                                        ps_mv_pred[TOP]->i2_mv[uc_B2 + 1];
                    }
                    else
                    {
                        MV_PRED(ps_mv_final_pred, ps_mv_pred, u1_ref_idx, uc_lx);
                    }
                }
                else
                {
                    if (ps_mv_pred[LEFT]->i1_ref_frame[uc_lx] == u1_ref_idx)
                    {
                        ps_mv_final_pred->i2_mv[uc_B2 + 0] =
                                        ps_mv_pred[LEFT]->i2_mv[uc_B2 + 0];
                        ps_mv_final_pred->i2_mv[uc_B2 + 1] =
                                        ps_mv_pred[LEFT]->i2_mv[uc_B2 + 1];
                    }
                    else
                    {
                        MV_PRED(ps_mv_final_pred, ps_mv_pred, u1_ref_idx, uc_lx);
                    }
                }
                break;
            case PRED_8x16:
                if (u4_sub_mb_num == 0)
                {
                    if (ps_mv_pred[LEFT]->i1_ref_frame[uc_lx] == u1_ref_idx)
                    {
                        ps_mv_final_pred->i2_mv[uc_B2 + 0] =
                                        ps_mv_pred[LEFT]->i2_mv[uc_B2 + 0];
                        ps_mv_final_pred->i2_mv[uc_B2 + 1] =
                                        ps_mv_pred[LEFT]->i2_mv[uc_B2 + 1];
                    }
                    else
                    {
                        MV_PRED(ps_mv_final_pred, ps_mv_pred, u1_ref_idx, uc_lx);
                    }
                }
                else
                {
                    if (ps_mv_pred[TOP_R]->i1_ref_frame[uc_lx] == u1_ref_idx)
                    {
                        ps_mv_final_pred->i2_mv[uc_B2 + 0] =
                                        ps_mv_pred[TOP_R]->i2_mv[uc_B2 + 0];
                        ps_mv_final_pred->i2_mv[uc_B2 + 1] =
                                        ps_mv_pred[TOP_R]->i2_mv[uc_B2 + 1];
                    }
                    else
                    {
                        MV_PRED(ps_mv_final_pred, ps_mv_pred, u1_ref_idx, uc_lx);
                    }
                }
                break;
            case B_DIRECT_SPATIAL:
                uc_temp1 = (UWORD8)ps_mv_pred[LEFT]->i1_ref_frame[0];
                uc_temp2 = (UWORD8)ps_mv_pred[TOP]->i1_ref_frame[0];
                uc_temp3 = (UWORD8)ps_mv_pred[TOP_R]->i1_ref_frame[0];

                ps_mv_final_pred->i1_ref_frame[0] = MIN(uc_temp1,
                                                      MIN(uc_temp2, uc_temp3));

                uc_temp1 = (UWORD8)ps_mv_pred[LEFT]->i1_ref_frame[1];
                uc_temp2 = (UWORD8)ps_mv_pred[TOP]->i1_ref_frame[1];
                uc_temp3 = (UWORD8)ps_mv_pred[TOP_R]->i1_ref_frame[1];

                ps_mv_final_pred->i1_ref_frame[1] = MIN(uc_temp1,
                                                      MIN(uc_temp2, uc_temp3));

                if ((ps_mv_final_pred->i1_ref_frame[0] < 0)
                                && (ps_mv_final_pred->i1_ref_frame[1] < 0))
                {
                    u1_direct_zero_pred_flag = 1;
                    ps_mv_final_pred->i1_ref_frame[0] = 0;
                    ps_mv_final_pred->i1_ref_frame[1] = 0;
                }
                MV_PRED(ps_mv_final_pred, ps_mv_pred,
                       ps_mv_final_pred->i1_ref_frame[0], 0);
                MV_PRED(ps_mv_final_pred, ps_mv_pred,
                       ps_mv_final_pred->i1_ref_frame[1], 1);
                break;
            case MB_SKIP:
                u1_a_in = (ps_cur_mb_info->u1_mb_ngbr_availablity &
                LEFT_MB_AVAILABLE_MASK);
                u1_b_in = (ps_cur_mb_info->u1_mb_ngbr_availablity &
                TOP_MB_AVAILABLE_MASK);
                if (((u1_a_in * u1_b_in) == 0)
                                || ((ps_mv_pred[LEFT]->i2_mv[0]
                                                | ps_mv_pred[LEFT]->i2_mv[1]
                                                | ps_mv_pred[LEFT]->i1_ref_frame[0])
                                                == 0)
                                || ((ps_mv_pred[TOP]->i2_mv[0]
                                                | ps_mv_pred[TOP]->i2_mv[1]
                                                | ps_mv_pred[TOP]->i1_ref_frame[0])
                                                == 0))
                {
                    ps_mv_final_pred->i2_mv[0] = 0;
                    ps_mv_final_pred->i2_mv[1] = 0;
                    break;
                }
            default:
                MV_PRED(ps_mv_final_pred, ps_mv_pred, u1_ref_idx, uc_lx);
                break;
        }
    }
    return (u1_direct_zero_pred_flag);
}

#if defined(MR_H264_CABAC_PROFILE)
/* Both mvpred wrap functions in this file feed the same mvpred_us/
 * mvpred_count bucket (see ih264d_cabac_profile.h) - B-slice vs P/B-slice
 * non-MBAFF dispatch is the same conceptual "MV prediction" stage from a
 * profiling point of view. */
UWORD8 __wrap_ih264d_mvpred_nonmbaffB(dec_struct_t *ps_dec,
                                      dec_mb_info_t *ps_cur_mb_info,
                                      mv_pred_t *ps_mv_nmb,
                                      mv_pred_t *ps_mv_ntop,
                                      mv_pred_t *ps_mv_final_pred,
                                      UWORD32 u4_sub_mb_num,
                                      UWORD8 uc_mb_part_width,
                                      UWORD8 u1_lx_start,
                                      UWORD8 u1_lxend,
                                      UWORD8 u1_mb_mc_mode)
{
    UWORD8 ret;
    clock_t t0 = clock();
    ret = mr_mvpred_nonmbaffB_impl(ps_dec, ps_cur_mb_info, ps_mv_nmb,
                                   ps_mv_ntop, ps_mv_final_pred, u4_sub_mb_num,
                                   uc_mb_part_width, u1_lx_start, u1_lxend,
                                   u1_mb_mc_mode);
    mr_h264_cabac_profile_add_mvpred(
        (unsigned long)((clock() - t0) * 1000000UL / CLOCKS_PER_SEC));
    return ret;
}
#endif

#if defined(MR_H264_CABAC_PROFILE)
static UWORD8 mr_mvpred_nonmbaff_impl(dec_struct_t *ps_dec,
                                      dec_mb_info_t *ps_cur_mb_info,
                                      mv_pred_t *ps_mv_nmb,
                                      mv_pred_t *ps_mv_ntop,
                                      mv_pred_t *ps_mv_final_pred,
                                      UWORD32 u4_sub_mb_num,
                                      UWORD8 uc_mb_part_width,
                                      UWORD8 u1_lx_start,
                                      UWORD8 u1_lxend,
                                      UWORD8 u1_mb_mc_mode)
#else
UWORD8 __wrap_ih264d_mvpred_nonmbaff(dec_struct_t *ps_dec,
                                     dec_mb_info_t *ps_cur_mb_info,
                                     mv_pred_t *ps_mv_nmb,
                                     mv_pred_t *ps_mv_ntop,
                                     mv_pred_t *ps_mv_final_pred,
                                     UWORD32 u4_sub_mb_num,
                                     UWORD8 uc_mb_part_width,
                                     UWORD8 u1_lx_start,
                                     UWORD8 u1_lxend,
                                     UWORD8 u1_mb_mc_mode)
#endif
{
    UWORD8 u1_a_in, u1_b_in, uc_temp1, uc_temp2, uc_temp3;
    mv_pred_t *ps_mv_pred[3];
    UWORD8 u1_ref_idx;
    UWORD8 u1_direct_zero_pred_flag = 0;
    UNUSED(u1_lx_start);
    UNUSED(u1_lxend);
    ih264d_non_mbaff_mv_pred(ps_mv_pred, u4_sub_mb_num, ps_mv_nmb, ps_mv_ntop,
                             ps_dec, uc_mb_part_width, ps_cur_mb_info);

    u1_ref_idx = ps_mv_final_pred->i1_ref_frame[0];

    switch (u1_mb_mc_mode)
    {
        case PRED_16x8:
            if (u4_sub_mb_num == 0)
            {
                if (ps_mv_pred[TOP]->i1_ref_frame[0] == u1_ref_idx)
                {
                    ps_mv_final_pred->i2_mv[0] = ps_mv_pred[TOP]->i2_mv[0];
                    ps_mv_final_pred->i2_mv[1] = ps_mv_pred[TOP]->i2_mv[1];
                }
                else
                {
                    MV_PRED(ps_mv_final_pred, ps_mv_pred, u1_ref_idx, 0);
                }
            }
            else
            {
                if (ps_mv_pred[LEFT]->i1_ref_frame[0] == u1_ref_idx)
                {
                    ps_mv_final_pred->i2_mv[0] = ps_mv_pred[LEFT]->i2_mv[0];
                    ps_mv_final_pred->i2_mv[1] = ps_mv_pred[LEFT]->i2_mv[1];
                }
                else
                {
                    MV_PRED(ps_mv_final_pred, ps_mv_pred, u1_ref_idx, 0);
                }
            }
            break;
        case PRED_8x16:
            if (u4_sub_mb_num == 0)
            {
                if (ps_mv_pred[LEFT]->i1_ref_frame[0] == u1_ref_idx)
                {
                    ps_mv_final_pred->i2_mv[0] = ps_mv_pred[LEFT]->i2_mv[0];
                    ps_mv_final_pred->i2_mv[1] = ps_mv_pred[LEFT]->i2_mv[1];
                }
                else
                {
                    MV_PRED(ps_mv_final_pred, ps_mv_pred, u1_ref_idx, 0);
                }
            }
            else
            {
                if (ps_mv_pred[TOP_R]->i1_ref_frame[0] == u1_ref_idx)
                {
                    ps_mv_final_pred->i2_mv[0] = ps_mv_pred[TOP_R]->i2_mv[0];
                    ps_mv_final_pred->i2_mv[1] = ps_mv_pred[TOP_R]->i2_mv[1];
                }
                else
                {
                    MV_PRED(ps_mv_final_pred, ps_mv_pred, u1_ref_idx, 0);
                }
            }
            break;
        case B_DIRECT_SPATIAL:
            uc_temp1 = (UWORD8)ps_mv_pred[LEFT]->i1_ref_frame[0];
            uc_temp2 = (UWORD8)ps_mv_pred[TOP]->i1_ref_frame[0];
            uc_temp3 = (UWORD8)ps_mv_pred[TOP_R]->i1_ref_frame[0];

            ps_mv_final_pred->i1_ref_frame[0] = MIN(uc_temp1,
                                                  MIN(uc_temp2, uc_temp3));

            uc_temp1 = (UWORD8)ps_mv_pred[LEFT]->i1_ref_frame[1];
            uc_temp2 = (UWORD8)ps_mv_pred[TOP]->i1_ref_frame[1];
            uc_temp3 = (UWORD8)ps_mv_pred[TOP_R]->i1_ref_frame[1];

            ps_mv_final_pred->i1_ref_frame[1] = MIN(uc_temp1,
                                                  MIN(uc_temp2, uc_temp3));

            if ((ps_mv_final_pred->i1_ref_frame[0] < 0)
                            && (ps_mv_final_pred->i1_ref_frame[1] < 0))
            {
                u1_direct_zero_pred_flag = 1;
                ps_mv_final_pred->i1_ref_frame[0] = 0;
                ps_mv_final_pred->i1_ref_frame[1] = 0;
            }
            MV_PRED(ps_mv_final_pred, ps_mv_pred,
                   ps_mv_final_pred->i1_ref_frame[0], 0);
            MV_PRED(ps_mv_final_pred, ps_mv_pred,
                   ps_mv_final_pred->i1_ref_frame[1], 1);
            break;
        case MB_SKIP:
            u1_a_in = (ps_cur_mb_info->u1_mb_ngbr_availablity &
            LEFT_MB_AVAILABLE_MASK);
            u1_b_in = (ps_cur_mb_info->u1_mb_ngbr_availablity &
            TOP_MB_AVAILABLE_MASK);
            if (((u1_a_in * u1_b_in) == 0)
                            || ((ps_mv_pred[LEFT]->i2_mv[0]
                                            | ps_mv_pred[LEFT]->i2_mv[1]
                                            | ps_mv_pred[LEFT]->i1_ref_frame[0])
                                            == 0)
                            || ((ps_mv_pred[TOP]->i2_mv[0]
                                            | ps_mv_pred[TOP]->i2_mv[1]
                                            | ps_mv_pred[TOP]->i1_ref_frame[0])
                                            == 0))
            {
                ps_mv_final_pred->i2_mv[0] = 0;
                ps_mv_final_pred->i2_mv[1] = 0;
                break;
            }
        default:
            MV_PRED(ps_mv_final_pred, ps_mv_pred, u1_ref_idx, 0);
            break;
    }

    return (u1_direct_zero_pred_flag);
}

#if defined(MR_H264_CABAC_PROFILE)
UWORD8 __wrap_ih264d_mvpred_nonmbaff(dec_struct_t *ps_dec,
                                     dec_mb_info_t *ps_cur_mb_info,
                                     mv_pred_t *ps_mv_nmb,
                                     mv_pred_t *ps_mv_ntop,
                                     mv_pred_t *ps_mv_final_pred,
                                     UWORD32 u4_sub_mb_num,
                                     UWORD8 uc_mb_part_width,
                                     UWORD8 u1_lx_start,
                                     UWORD8 u1_lxend,
                                     UWORD8 u1_mb_mc_mode)
{
    UWORD8 ret;
    clock_t t0 = clock();
    ret = mr_mvpred_nonmbaff_impl(ps_dec, ps_cur_mb_info, ps_mv_nmb,
                                  ps_mv_ntop, ps_mv_final_pred, u4_sub_mb_num,
                                  uc_mb_part_width, u1_lx_start, u1_lxend,
                                  u1_mb_mc_mode);
    mr_h264_cabac_profile_add_mvpred(
        (unsigned long)((clock() - t0) * 1000000UL / CLOCKS_PER_SEC));
    return ret;
}
#endif
#endif /* MR_M68K_ASM */
