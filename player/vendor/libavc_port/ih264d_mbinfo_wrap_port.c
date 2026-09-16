/*
 * m68k linker-level timing wrapper for ih264d_get_mb_info_cabac_nonmbaff()
 * - the per-macroblock neighbour-availability/CABAC-context setup function
 * assigned to dec_struct_t::pf_get_mb_info for non-MBAFF CABAC slices (see
 * ih264d_parse_pslice.c/_islice.c/_bslice.c for the by-name assignments
 * this file's --wrap flag redirects). Called once for *every* macroblock,
 * skip or not - unlike the mb_type/cbp/ref_idx/mvd/intra-mode/mb_qp_delta
 * syntax dispatch (pf_parse_inter_mb), this one is a genuine cross-object
 * relocation: it is assigned from ih264d_parse_pslice.c/_islice.c/
 * _bslice.c but *defined* in the separate ih264d_mb_utils.c, so --wrap
 * works on it exactly like it does for ih264d_decode_bin/ih264d_mvpred_
 * nonmbaff/ih264d_parse_residual4x4_cabac - see ih264d_cabac_profile.h for
 * the full "why this one and not pf_parse_inter_mb" story.
 *
 * Unlike every other --wrap site in this port, this one is NOT an asm
 * replacement - there is no m68k kernel for this function, only a real-
 * hardware finding (see CLAUDE.md's H.264 CABAC notes) that it might be a
 * meaningful chunk of the previously-unattributed "everything else"
 * remainder in core_us, worth measuring directly instead of guessing about
 * further. So __wrap_ih264d_get_mb_info_cabac_nonmbaff below is a plain
 * timing pass-through to the real, completely unmodified vendored
 * function via GNU ld's __real_ symbol (automatically defined for any
 * --wrap=X target) - no reimplementation, so no risk of changing what
 * gets decoded, only of adding two clock() calls' worth of overhead.
 *
 * Because there is nothing to gain from wrapping this function outside of
 * profiling, the --wrap=ih264d_get_mb_info_cabac_nonmbaff linker flag
 * itself is only added when CABAC_PROFILE=1 (see libavc.mk), unlike the
 * always-on wraps for bin/mvpred/coeff/update_qp. A normal playback build
 * never links against this symbol at all - not even the call/return layer
 * this file adds under profiling - so mr_h264_get_mb_info_cabac_nonmbaff's
 * original cost is completely unaffected outside of a one-off diagnostic
 * build, matching Makefile.amiga's existing STAGE_PROFILE=1 precedent.
 *
 * Only the non-MBAFF CABAC variant is wrapped, matching ih264d_mvpred_
 * dispatch_port.c's own "MBAFF left untouched" precedent - this project
 * has no MBAFF (interlaced-field-coded) test content, and the CAVLC
 * variants are unreached by every fixture this project decodes (all use
 * CABAC entropy coding).
 *
 * Verification: this file changes no behaviour by construction - it is a
 * pure pass-through to the unmodified vendored function, not a
 * reimplementation, so there is no new bit-exactness claim to prove.
 * Correctness of the *wiring* (the wrap fires, with the real function's
 * return value and side effects intact) is exercised by the full make
 * check-m68k H.264-vs-ffmpeg conformance suite run through
 * mr_decode_cabac_profile.m68k (tests/run_m68k_check.sh), the same build
 * that already proves ih264d_cabac_wrap.c/ih264d_parse_cabac_coeff_port.c/
 * ih264d_mvpred_dispatch_port.c's own CABAC_PROFILE-gated timing paths.
 *
 * ---------------------------------------------------------------------
 *
 * A real A1200 retest of mbinfo_us (see CLAUDE.md) found it a genuine but
 * modest ~8% of core_us - it does not explain the ~48% that is still left
 * over after mc/deblock/recon/intra/bin/coeff/mvpred/mbinfo, which is
 * still dominated by pf_parse_inter_mb itself (ih264d_parse_pmb_cabac()/
 * ih264d_parse_bmb_cabac()) - the one function pointer in this whole chain
 * that --wrap genuinely cannot reach (see ih264d_cabac_profile.h): its
 * assignment happens in the *same* file that defines it
 * (ih264d_parse_pslice.c/_bslice.c), so a --wrap=ih264d_parse_pmb_cabac/
 * ih264d_parse_bmb_cabac flag would link cleanly and silently never fire,
 * exactly like the already-documented --wrap=ih264d_get_motion_vector_
 * predictor and --wrap=ih264d_read_coeff4x4_cabac dead ends.
 *
 * mr_wrap_parse_inter_mb() below reaches it anyway, through a completely
 * different mechanism that sidesteps the same-file problem instead of
 * fighting it: __wrap_ih264d_get_mb_info_cabac_nonmbaff() above already
 * runs, with a live dec_struct_t*, on *every* macroblock - skip or not -
 * strictly *before* that same macroblock's ps_dec->pf_parse_inter_mb gets
 * looked up and called (confirmed by reading ih264d_parse_pslice_data_
 * cabac()'s per-MB loop: pf_get_mb_info() is called first each iteration,
 * pf_parse_inter_mb() - only for non-skip MBs - after). So instead of
 * trying to intercept libavc's own address-of assignment (impossible, per
 * above), this hooks in one step later: every time the mbinfo wrapper
 * runs, it captures whatever real function ps_dec->pf_parse_inter_mb
 * currently holds (ih264d_parse_pmb_cabac for a P slice,
 * ih264d_parse_bmb_cabac for a B slice - libavc reassigns this pointer
 * fresh at the start of every slice header, so "currently holds" is
 * always correct for the slice in progress) into g_real_parse_inter_mb,
 * then overwrites the field with mr_wrap_parse_inter_mb - a plain C
 * struct-field write, no linker trick needed at all. The very next call
 * through that field (for this or a later MB in the same slice) runs our
 * wrapper, which times the call through to whatever was captured and
 * restores nothing else - completely transparent to the real decode.
 *
 * Idempotent by construction, not by luck: the `!=` check means a slice
 * boundary (a fresh real assignment, P or B, CABAC only - CAVLC and MBAFF
 * are not captured here for the same "no test content, no verified
 * primitive to compare against" reason ih264d_mvpred_dispatch_port.c
 * leaves those variants alone) gets captured and re-wrapped on the very
 * next macroblock, while every other macroblock in the same slice is a
 * no-op (the field already holds our own wrapper, so the check fails and
 * nothing is rewritten) - no risk of the wrapper capturing itself and
 * recursing, and no assumption about how many slices a picture has.
 *
 * mbparse_us is *not* a clean fourth additive bucket the way bin/coeff/
 * mvpred/mbinfo are documented to be: it is the *whole* wall-clock cost of
 * pf_parse_inter_mb, which itself calls ih264d_decode_bin() (already
 * counted in bin_us), ih264d_parse_residual4x4_cabac() (already counted
 * in coeff_us) and the mv-predictor dispatch (already counted in
 * mvpred_us) - so mbparse_us necessarily overlaps all three, on top of
 * timing genuinely new, previously-unmeasured C-level cost (the sub-MB-
 * type/ref-idx/partition-loop/CBP/transform8x8-flag/mb_qp_delta glue that
 * has no wrap of its own). That overlap is the whole point here, not a
 * flaw to fix: mbparse_us, summed over a frame, is a *direct* measurement
 * of the same wall-clock quantity the "core_us minus every other bucket"
 * remainder has only ever been able to *derive* by subtraction - a real
 * hardware capture with this wired in can finally check whether that
 * remainder genuinely *is* pf_parse_inter_mb (mbparse_us tracks it
 * closely) or whether there is a further, still-unattributed cost outside
 * it (mbparse_us reads meaningfully smaller than the remainder).
 */
#include "ih264_typedefs.h"
#include "ih264d_structs.h"
#include "ih264d_mb_utils.h"

#if defined(MR_H264_CABAC_PROFILE)
#include "ih264d_cabac_profile.h"
#include <time.h>

typedef WORD32 (*mr_parse_inter_mb_fn)(dec_struct_t *ps_dec,
                                       dec_mb_info_t *ps_cur_mb_info,
                                       UWORD32 u4_mb_num,
                                       UWORD32 u4_num_mbsNby2);

static mr_parse_inter_mb_fn g_real_parse_inter_mb;

static WORD32 mr_wrap_parse_inter_mb(dec_struct_t *ps_dec,
                                     dec_mb_info_t *ps_cur_mb_info,
                                     UWORD32 u4_mb_num,
                                     UWORD32 u4_num_mbsNby2)
{
    WORD32 ret;
    clock_t t0 = clock();
    ret = g_real_parse_inter_mb(ps_dec, ps_cur_mb_info, u4_mb_num,
                                u4_num_mbsNby2);
    mr_h264_cabac_profile_add_mbparse(
        (unsigned long)((clock() - t0) * 1000000UL / CLOCKS_PER_SEC));
    return ret;
}

extern UWORD32 __real_ih264d_get_mb_info_cabac_nonmbaff(
    dec_struct_t *ps_dec, const UWORD16 u2_cur_mb_address,
    dec_mb_info_t *ps_cur_mb_info, UWORD32 u4_mbskip_run);

UWORD32 __wrap_ih264d_get_mb_info_cabac_nonmbaff(
    dec_struct_t *ps_dec, const UWORD16 u2_cur_mb_address,
    dec_mb_info_t *ps_cur_mb_info, UWORD32 u4_mbskip_run)
{
    UWORD32 ret;
    clock_t t0 = clock();
    ret = __real_ih264d_get_mb_info_cabac_nonmbaff(ps_dec, u2_cur_mb_address,
                                                    ps_cur_mb_info,
                                                    u4_mbskip_run);
    mr_h264_cabac_profile_add_mbinfo(
        (unsigned long)((clock() - t0) * 1000000UL / CLOCKS_PER_SEC));

    if ((mr_parse_inter_mb_fn)ps_dec->pf_parse_inter_mb !=
        mr_wrap_parse_inter_mb) {
        g_real_parse_inter_mb =
            (mr_parse_inter_mb_fn)ps_dec->pf_parse_inter_mb;
        ps_dec->pf_parse_inter_mb = mr_wrap_parse_inter_mb;
    }

    return ret;
}
#endif
