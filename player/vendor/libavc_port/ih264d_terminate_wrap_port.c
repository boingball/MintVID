/*
 * m68k linker-level timing wrapper for ih264d_decode_terminate() - the
 * CABAC "termination" bin decode (spec 9.3.3.2.2.3): a small, self-
 * contained arithmetic-decode step used for two syntax elements -
 * end_of_slice_flag, decoded once per macroblock (skip or not) at the end
 * of the shared per-MB loop in ih264d_parse_pslice.c/_islice.c to decide
 * whether the slice has more macroblocks left, and the I16x16-vs-I_PCM
 * bin inside ih264d_parse_mb_type_intra_cabac() (ih264d_parse_mb_
 * header.c). Direct follow-up to the intramb_us/mbparse_us retest (see
 * CLAUDE.md's "intramb_us" section): after both of those landed, ~36% of
 * core_us was still completely unattributed - this and ih264d_mbtype_
 * wrap_port.c's sibling wrap are the next two genuinely wrap-able,
 * previously-untouched per-MB costs found by re-reading the same per-MB
 * loop those two files already live in.
 *
 * Unlike ih264d_decode_bin() (~40+ call sites, almost all inside the same
 * file that defines it - see ih264d_cabac_wrap.c), ih264d_decode_
 * terminate() does its own bypass-like arithmetic inline (CLZ, a range
 * update, conditional renormalisation) rather than delegating to decode_
 * bin - confirmed by reading its body in ih264d_cabac.c before assuming
 * it would be an uncounted cost: it is genuinely self-contained, not
 * already folded into bin_us. Every one of its three real call sites
 * (ih264d_parse_pslice.c, ih264d_parse_islice.c, ih264d_parse_mb_
 * header.c) is in a different file from the one that defines it
 * (ih264d_cabac.c) - a completely ordinary set of cross-object
 * relocations, the same --wrap shape ih264d_intramb_wrap_port.c already
 * uses, not the same-file dead end pf_parse_inter_mb hits.
 *
 * Called once per macroblock regardless of skip/non-skip/inter/intra -
 * the one buildable measurement in this whole chain that reaches every
 * single macroblock the way mbinfo_us does, rather than only a subset the
 * way mbparse_us/intramb_us do. terminate_us is disjoint from bin_us/
 * coeff_us/mvpred_us/mbinfo_us/mbparse_us/intramb_us: none of those call
 * into it, and it never calls into ih264d_decode_bin() itself (confirmed
 * by reading its body - no other call at all, just inline arithmetic), so
 * this is a real, additive sixth-ish bucket, not another overlapping one
 * like mbparse_us/intramb_us.
 *
 * Verification: pure timing pass-through via GNU ld's __real_ symbol, no
 * reimplementation, --wrap flag only added under CABAC_PROFILE=1 (see
 * libavc.mk) - a normal build links none of it.
 */
#include "ih264_typedefs.h"
#include "ih264d_cabac.h"

#if defined(MR_H264_CABAC_PROFILE)
#include "ih264d_cabac_profile.h"
#include <time.h>

extern UWORD8 __real_ih264d_decode_terminate(decoding_envirnoment_t *ps_cab_env,
                                             dec_bit_stream_t *ps_bitstrm);

UWORD8 __wrap_ih264d_decode_terminate(decoding_envirnoment_t *ps_cab_env,
                                      dec_bit_stream_t *ps_bitstrm)
{
    UWORD8 ret;
    clock_t t0 = clock();
    ret = __real_ih264d_decode_terminate(ps_cab_env, ps_bitstrm);
    mr_h264_cabac_profile_add_terminate(
        (unsigned long)((clock() - t0) * 1000000UL / CLOCKS_PER_SEC));
    return ret;
}
#endif
