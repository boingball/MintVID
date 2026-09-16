/*
 * m68k linker-level timing wrapper for ih264d_parse_imb_cabac() - the
 * intra-macroblock mb_type/pred-mode/CBP/mb_qp_delta/residual syntax
 * dispatch for a macroblock decoded as intra, whether inside a whole I
 * slice or embedded in a P/B slice (H.264 allows any slice to carry intra
 * macroblocks - an encoder reaches for one wherever inter prediction would
 * cost more bits, e.g. at a scene cut or a newly-uncovered region).
 *
 * This is the direct follow-up to the mbparse_us retest (see CLAUDE.md's
 * "mbparse_us retest" section, and the correction note at the bottom of
 * ih264d_mbinfo_wrap_port.c/ih264d_cabac_profile.h): mbparse_count (calls
 * that reach pf_parse_inter_mb) averaged only 35% of mbinfo_count (every
 * macroblock) on a real A1200 capture - meaning roughly 65% of
 * macroblocks in that real content were skip *or* intra, neither counted
 * by mbparse_us. ih264d_parse_imb_cabac() is the intra half of that gap -
 * the direct structural analogue of pf_parse_inter_mb/ih264d_parse_
 * pmb_cabac, just for intra macroblocks instead of inter ones.
 *
 * Unlike pf_parse_inter_mb, this one needs no struct-field-swap trick at
 * all: read straight from the per-MB loop that dispatches both
 * (ih264d_parse_inter_slice_data_cabac() in ih264d_parse_pslice.c, shared
 * by P and B slices alike - see that function's own u1_mb_type <
 * u1_mb_threshold branch), the call `ret = ih264d_parse_imb_cabac(ps_dec,
 * ps_cur_mb_info, ...)` is a plain, ordinary direct function call, not an
 * address-of/function-pointer assignment - and ih264d_parse_imb_cabac()
 * itself is *defined* in the separate ih264d_parse_islice.c, not in
 * ih264d_parse_pslice.c where it's called from here. That is a completely
 * ordinary cross-object relocation, the textbook case --wrap exists for -
 * none of the same-file problems that blocked pf_parse_inter_mb or forced
 * the struct-field-swap workaround for it apply here at all.
 *
 * The one call site this does *not* reach: ih264d_parse_islice.c's own
 * per-MB loop (a whole I slice/I frame) calls ih264d_parse_imb_cabac()
 * from within the same file that defines it - the same same-file shape
 * pf_parse_inter_mb hits, unreachable by --wrap. Left unwrapped
 * deliberately rather than chased with another swap: I frames are one per
 * GOP, a small minority of decoded pictures, and the P/B-embedded case
 * this file *does* reach is the one the mbparse_us retest actually needs
 * covered (that capture's "65% skip-or-intra" finding was inside P/B
 * slices, which is exactly where this wrap fires).
 *
 * intramb_us is the intra-side sibling of mbparse_us and shares its same
 * "not a clean additive bucket" shape: ih264d_parse_imb_cabac() itself
 * calls ih264d_decode_bin() (bin_us) and ih264d_parse_residual4x4_cabac()
 * (coeff_us) internally, so intramb_us necessarily overlaps both, on top
 * of whatever previously-unmeasured C-level glue is in its own body
 * (intra 4x4/16x16 pred-mode signalling, CBP, mb_qp_delta bookkeeping -
 * the intra-side counterpart of what mbparse_us's own header already
 * describes for the inter case). Summed with mbparse_us, mbinfo_us and
 * the STAGE_PROFILE buckets, this closes most of the remaining gap the
 * mbparse_us retest found - what's left after all of these is most likely
 * the per-MB loop's own skip-macroblock bookkeeping (inline code, no
 * function boundary at all to hook - see the correction note in
 * ih264d_mbinfo_wrap_port.c for why that one has no equivalent fix yet).
 *
 * Verification: this file changes no behaviour by construction, same as
 * ih264d_mbinfo_wrap_port.c - a pure pass-through to the unmodified
 * vendored function via GNU ld's __real_ symbol, not a reimplementation.
 * The --wrap linker flag is only added under CABAC_PROFILE=1 (see
 * libavc.mk), so a normal build links none of this at all.
 */
#include "ih264_typedefs.h"
#include "ih264d_structs.h"

#if defined(MR_H264_CABAC_PROFILE)
#include "ih264d_cabac_profile.h"
#include <time.h>

extern WORD32 __real_ih264d_parse_imb_cabac(dec_struct_t *ps_dec,
                                            dec_mb_info_t *ps_cur_mb_info,
                                            UWORD8 u1_mb_type);

WORD32 __wrap_ih264d_parse_imb_cabac(dec_struct_t *ps_dec,
                                     dec_mb_info_t *ps_cur_mb_info,
                                     UWORD8 u1_mb_type)
{
    WORD32 ret;
    clock_t t0 = clock();
    ret = __real_ih264d_parse_imb_cabac(ps_dec, ps_cur_mb_info, u1_mb_type);
    mr_h264_cabac_profile_add_intramb(
        (unsigned long)((clock() - t0) * 1000000UL / CLOCKS_PER_SEC));
    return ret;
}
#endif
