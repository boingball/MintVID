/*
 * m68k linker-level timing wrapper for ih264d_parse_mb_type_cabac() - the
 * mb_type syntax-element dispatch for a P/B-slice macroblock, called once
 * per *non-skip* macroblock (both inter and intra) from the shared per-MB
 * loop in ih264d_parse_pslice.c, strictly before that same macroblock's
 * pf_parse_inter_mb/ih264d_parse_imb_cabac dispatch. See ih264d_
 * terminate_wrap_port.c's header for why this and that file both exist -
 * both are follow-ups to the intramb_us/mbparse_us retest's ~36%
 * still-unattributed remainder.
 *
 * Same cross-object shape as intramb_us, checked the same way before
 * assuming it: ih264d_parse_mb_type_cabac() is *defined* in the separate
 * ih264d_parse_mb_header.c, but *called* from ih264d_parse_pslice.c - an
 * ordinary cross-object relocation, plain --wrap target, not the
 * same-file case pf_parse_inter_mb fails on.
 *
 * mbtype_us is not fully disjoint from bin_us, for the same reason
 * mbparse_us/intramb_us aren't: ih264d_parse_mb_type_cabac() itself calls
 * only ih264d_decode_bin()/ih264d_decode_bins() internally (checked by
 * reading its body - unlike ih264d_parse_mb_type_intra_cabac(), a
 * different, sibling function in the same file, it does *not* call
 * ih264d_decode_terminate() at all, so mbtype_us and terminate_us are
 * disjoint from each other even though neither is disjoint from bin_us).
 * So mbtype_us contains bin_us's contribution during this one call, plus
 * whatever previously-unmeasured C-level context-selection/dispatch glue
 * is in its own body (u1_mb_type computation from decoded bins, the
 * SI/P/B-slice branch structure).
 *
 * Verification: pure timing pass-through via GNU ld's __real_ symbol, no
 * reimplementation, --wrap flag only added under CABAC_PROFILE=1 (see
 * libavc.mk) - a normal build links none of it.
 */
#include "ih264_typedefs.h"
#include "ih264d_structs.h"

#if defined(MR_H264_CABAC_PROFILE)
#include "ih264d_cabac_profile.h"
#include <time.h>

extern UWORD32 __real_ih264d_parse_mb_type_cabac(dec_struct_t *ps_dec);

UWORD32 __wrap_ih264d_parse_mb_type_cabac(dec_struct_t *ps_dec)
{
    UWORD32 ret;
    clock_t t0 = clock();
    ret = __real_ih264d_parse_mb_type_cabac(ps_dec);
    mr_h264_cabac_profile_add_mbtype(
        (unsigned long)((clock() - t0) * 1000000UL / CLOCKS_PER_SEC));
    return ret;
}
#endif
