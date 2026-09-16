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
 */
#include "ih264_typedefs.h"
#include "ih264d_structs.h"
#include "ih264d_mb_utils.h"

#if defined(MR_H264_CABAC_PROFILE)
#include "ih264d_cabac_profile.h"
#include <time.h>

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
    return ret;
}
#endif
