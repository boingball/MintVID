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
 * There is deliberately no bucket of its own for the mb_type/cbp/ref_idx/
 * mvd/intra-mode/mb_qp_delta syntax-element *dispatch* itself -
 * dec_struct_t::pf_parse_inter_mb, assigned to ih264d_parse_pmb_cabac()/
 * ih264d_parse_bmb_cabac() once per slice. That one genuinely cannot be
 * intercepted the same way the buckets above are: unlike bin/coeff/mvpred
 * (each called from a *different* file than the one defining them, so a
 * normal cross-object relocation exists for --wrap to redirect), that
 * assignment happens in the *same* file that defines the target function.
 * Confirmed empirically with a minimal repro compiled for m68k: a same-file
 * function-*pointer* assignment does leave a relocation against the target
 * symbol (unlike a same-file direct *call*, which resolves to a branch with
 * no relocation left for the linker to touch), but --wrap still does not
 * redirect it - the reference is satisfied against the object's own local
 * definition before the wrap rename takes effect, so a wrapper installed
 * this way would link cleanly and silently never fire. Reimplementing those
 * two ~200-line dispatchers from scratch (the fix ih264d_mvpred_dispatch_
 * port.c/ih264d_parse_cabac_coeff_port.c applied, for a real optimisation)
 * is not justified just to add a diagnostic counter.
 *
 *   mbinfo_us/mbinfo_count - per-macroblock neighbour-availability/CABAC-
 *                          context setup, called once for *every* MB
 *                          (skip or not) via dec_struct_t::pf_get_mb_info.
 *                          Unlike pf_parse_inter_mb above, this dispatcher
 *                          IS interceptable: it is assigned from
 *                          ih264d_parse_pslice.c/_islice.c/_bslice.c, but
 *                          *defined* in the separate ih264d_mb_utils.c, so
 *                          the assignment is a genuine cross-object
 *                          relocation --wrap can redirect - the same shape
 *                          that already works for bin/coeff/mvpred, not the
 *                          same-file case pf_parse_inter_mb fails on. Only
 *                          the non-MBAFF CABAC variant
 *                          (ih264d_get_mb_info_cabac_nonmbaff) is wrapped,
 *                          matching ih264d_mvpred_dispatch_port.c's own
 *                          "MBAFF left untouched" precedent - this project
 *                          has no MBAFF (interlaced-field-coded) test
 *                          content. Fed from ih264d_mbinfo_wrap_port.c,
 *                          a pure timing pass-through (calls straight
 *                          through to the real, unmodified vendored
 *                          function via GNU ld's __real_ symbol - no
 *                          reimplementation, so no risk of behaviour
 *                          change). Small, known overlap: when the current
 *                          MB is itself a P/B-skip run, this function
 *                          decodes the one mb_skip_flag CABAC bin inline
 *                          (see ih264d_mb_utils.c) - already counted under
 *                          bin_us too - so mbinfo_us is not perfectly
 *                          disjoint from bin_us the way bin/coeff/mvpred
 *                          are from each other, though one bin's cost is
 *                          negligible next to the rest of the function.
 *
 * The remaining "macroblock parsing" cost is still derivable, just not
 * directly measured: treat (core_us - mc_us - deblock_us - recon_us -
 * intra_us - bin_us - coeff_us - mvpred_us - mbinfo_us) as that combined
 * remainder - the same unattributed-remainder idea ih264d_stage_profile.h
 * already uses, just smaller and more useful now that four of its buckets
 * are broken out. A real-hardware STAGE_PROFILE+CABAC_PROFILE capture
 * (YouTube 360p, Turbo, A1200 68060/50 - see CLAUDE.md) found this
 * remainder at 52-55% of core_us before mbinfo_us existed - bigger than
 * mc+deblock+recon+intra or bin+coeff+mvpred combined - which is what
 * motivated singling out pf_get_mb_info as the next thing to try to
 * measure directly rather than guess about further.
 *
 * Like ih264d_stage_profile.c, this module always compiles in (portable C,
 * no MR_M68K_ASM guard) so mr_h264.c's reset/get calls are unconditionally
 * safe - the accumulators just stay at zero unless something is actually
 * feeding them. Only the four feed sites (gated by MR_H264_CABAC_PROFILE
 * in ih264d_cabac_wrap.c / ih264d_parse_cabac_coeff_port.c /
 * ih264d_mvpred_dispatch_port.c / ih264d_mbinfo_wrap_port.c) cost anything,
 * and only in a build that opted in (Makefile.amiga CABAC_PROFILE=1,
 * mirroring STAGE_PROFILE=1). A normal playback build pays nothing for any
 * of this: the bin_us feed site is not just disabled but replaced outright
 * - ih264d_cabac_wrap.c's C trampoline (one extra call/return per decoded
 * bin, the very overhead these counters exist to help quantify) is swapped
 * for a direct asm alias with no C call layer at all (see
 * ih264_m68k_cabac.S) - and the mbinfo_us feed site's --wrap linker flag
 * itself is only added to the link when CABAC_PROFILE=1 (see libavc.mk),
 * unlike bin/coeff/mvpred/update_qp's always-on wraps, since there is no
 * asm replacement to gain from wrapping pf_get_mb_info outside of
 * profiling - a normal build never links __wrap_ih264d_get_mb_info_cabac_
 * nonmbaff at all, so it costs literally nothing, not even a call/return.
 */

typedef struct mr_h264_cabac_us {
    unsigned long bin_us, bin_count;
    unsigned long coeff_us, coeff_count;
    unsigned long mvpred_us, mvpred_count;
    unsigned long mbinfo_us, mbinfo_count;
} mr_h264_cabac_us;

/* Called only from the four MR_H264_CABAC_PROFILE-gated feed sites above -
 * never called at all in a normal build. */
void mr_h264_cabac_profile_add_bin(unsigned long us);
void mr_h264_cabac_profile_add_coeff(unsigned long us);
void mr_h264_cabac_profile_add_mvpred(unsigned long us);
void mr_h264_cabac_profile_add_mbinfo(unsigned long us);

/* Zero the accumulators before a libavc decode sub-call - paired with
 * mr_h264_stage_profile_reset(), called from the same site in mr_h264.c. */
void mr_h264_cabac_profile_reset(void);

/* Read the accumulated per-bucket microseconds/call-counts since the last
 * reset. */
void mr_h264_cabac_profile_get(mr_h264_cabac_us *out);

#endif
