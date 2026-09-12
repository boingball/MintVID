#ifndef MR_IH264D_CABAC_PROFILE_H
#define MR_IH264D_CABAC_PROFILE_H

/*
 * Diagnostic-only timing/count breakdown of the H.264 CABAC decode path,
 * complementing ih264d_stage_profile.h's mc/deblock/recon/intra buckets.
 * That file's own stage-profile output could only report the combined
 * "everything else" as a single unattributed remainder (core_us minus
 * mc/deblock/recon/intra) - a real-hardware trace and a host callgrind
 * profile both put that remainder at roughly 56% of total H.264 decode
 * time, about twice motion compensation, but with no further attribution:
 * it covers bitstream/CABAC/CAVLC parsing, MV prediction, and per-MB
 * bookkeeping all lumped together. This file breaks three of those apart
 * into their own buckets:
 *
 *   bin_us/bin_count     - every ih264d_decode_bin() call: mb_type, cbp,
 *                          ref_idx, mvd, intra pred modes, mb_qp_delta and
 *                          every other CABAC-coded macroblock-header
 *                          syntax element. Fed from ih264d_cabac_wrap.c's
 *                          diagnostic trampoline.
 *   coeff_us/coeff_count - residual coefficient parsing
 *                          (ih264d_parse_residual4x4_cabac and the
 *                          standalone ih264d_read_coeff4x4_cabac entry
 *                          point, 4x4 and 8x8 transform sizes alike - both
 *                          routed into this same accumulator). Fed from
 *                          ih264d_parse_cabac_coeff_port.c.
 *   mvpred_us/mvpred_count - motion-vector *prediction*: the median-of-
 *                          neighbours arithmetic that computes the
 *                          predictor a decoded mvd is added to. This is
 *                          not entropy decoding - mvd itself is CABAC-coded
 *                          and already counted under bin_us - so it does
 *                          not overlap with it. Fed from
 *                          ih264d_mvpred_dispatch_port.c.
 *
 * These three do not overlap each other: residual coefficient decode
 * (ih264d_read_coeff4x4/8x8_cabac) and MV prediction are both self-
 * contained arithmetic that never call back into ih264d_decode_bin() - see
 * ih264d_parse_cabac_coeff_port.c's and ih264d_mvpred_dispatch_port.c's own
 * header comments - so bin_us+coeff_us+mvpred_us is a real, additive
 * subtotal, unlike (say) mc_us versus core_us.
 *
 * There is deliberately no fourth "macroblock parsing" bucket of its own.
 * The mb_type/cbp/ref_idx/mvd/intra-mode/mb_qp_delta syntax-element
 * dispatch that drives all of the above - dec_struct_t::pf_parse_inter_mb,
 * assigned to ih264d_parse_pmb_cabac()/ih264d_parse_bmb_cabac() once per
 * slice - cannot be intercepted the same way the three buckets above are:
 * unlike bin/coeff/mvpred (each called from a *different* file than the one
 * defining them, so a normal cross-object relocation exists for --wrap to
 * redirect), that assignment happens in the *same* file that defines the
 * target function, same as the mvpred dispatch's own already-documented
 * same-object pitfall. Confirmed empirically with a minimal repro compiled
 * for m68k: a same-file function-*pointer* assignment does leave a
 * relocation against the target symbol (unlike a same-file direct *call*,
 * which resolves to a branch with no relocation left for the linker to
 * touch), but --wrap still does not redirect it - the reference is
 * satisfied against the object's own local definition before the wrap
 * rename takes effect, so a wrapper installed this way would link cleanly
 * and silently never fire, exactly the failure mode
 * ih264d_mvpred_dispatch_port.c's header warns about for the call case.
 * Reimplementing those two ~200-line dispatchers from scratch (the fix
 * that file applied for a real optimisation) is not justified just to add
 * a diagnostic counter.
 *
 * The remaining "macroblock parsing" cost is still derivable, just not
 * directly measured: treat (core_us - mc_us - deblock_us - recon_us -
 * intra_us - bin_us - coeff_us - mvpred_us) as that combined remainder -
 * the same unattributed-remainder idea ih264d_stage_profile.h already
 * uses, just a much smaller and more useful one now that three of its four
 * components are broken out.
 *
 * Like ih264d_stage_profile.c, this module always compiles in (portable C,
 * no MR_M68K_ASM guard) so mr_h264.c's reset/get calls are unconditionally
 * safe - the accumulators just stay at zero unless something is actually
 * feeding them. Only the three feed sites (gated by MR_H264_CABAC_PROFILE
 * in ih264d_cabac_wrap.c / ih264d_parse_cabac_coeff_port.c /
 * ih264d_mvpred_dispatch_port.c) cost anything, and only in a build that
 * opted in (Makefile.amiga CABAC_PROFILE=1, mirroring STAGE_PROFILE=1). A
 * normal playback build pays nothing for any of this: the bin_us feed site
 * is not just disabled but replaced outright - ih264d_cabac_wrap.c's C
 * trampoline (one extra call/return per decoded bin, the very overhead
 * these counters exist to help quantify) is swapped for a direct asm alias
 * with no C call layer at all (see ih264_m68k_cabac.S).
 */

typedef struct mr_h264_cabac_us {
    unsigned long bin_us, bin_count;
    unsigned long coeff_us, coeff_count;
    unsigned long mvpred_us, mvpred_count;
} mr_h264_cabac_us;

/* Called only from the three MR_H264_CABAC_PROFILE-gated feed sites above -
 * never called at all in a normal build. */
void mr_h264_cabac_profile_add_bin(unsigned long us);
void mr_h264_cabac_profile_add_coeff(unsigned long us);
void mr_h264_cabac_profile_add_mvpred(unsigned long us);

/* Zero the accumulators before a libavc decode sub-call - paired with
 * mr_h264_stage_profile_reset(), called from the same site in mr_h264.c. */
void mr_h264_cabac_profile_reset(void);

/* Read the accumulated per-bucket microseconds/call-counts since the last
 * reset. */
void mr_h264_cabac_profile_get(mr_h264_cabac_us *out);

#endif
