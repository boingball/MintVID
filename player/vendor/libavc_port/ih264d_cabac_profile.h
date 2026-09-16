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
 * A real A1200 retest with mbinfo_us wired in (see CLAUDE.md) found the
 * remainder only dropped from that 52-55% to ~48% - consistent with
 * mbinfo_us (a real ~8% of core_us) simply having been carved out of what
 * was previously lumped into the remainder, not with it having explained
 * most of that remainder. pf_parse_inter_mb itself is still the largest
 * single cost by a wide margin.
 *
 *   mbparse_us/mbparse_count - the *entire* wall-clock cost of
 *                          pf_parse_inter_mb (ih264d_parse_pmb_cabac() for
 *                          a P slice, ih264d_parse_bmb_cabac() for a B
 *                          slice), reached without --wrap at all:
 *                          ih264d_mbinfo_wrap_port.c's mbinfo wrapper
 *                          already runs, with a live dec_struct_t*, on
 *                          every macroblock strictly before that same
 *                          macroblock's pf_parse_inter_mb is looked up and
 *                          called - so it captures whatever real function
 *                          libavc just assigned for the slice in progress
 *                          and overwrites the struct field with a timing
 *                          wrapper, a plain C pointer write with no linker
 *                          trick needed. See that file's own header for
 *                          why this reaches pf_parse_inter_mb despite the
 *                          same-file --wrap dead end described above, and
 *                          for the idempotency argument (a fresh real
 *                          assignment at every slice boundary is always
 *                          re-captured on the very next macroblock).
 *
 *                          Unlike bin/coeff/mvpred/mbinfo, mbparse_us is
 *                          *not* disjoint from the others - pf_parse_
 *                          inter_mb itself calls ih264d_decode_bin()
 *                          (bin_us), ih264d_parse_residual4x4_cabac()
 *                          (coeff_us) and the mv-predictor dispatch
 *                          (mvpred_us), so it necessarily contains all
 *                          three plus whatever previously-unmeasured C
 *                          glue exists between them (partition loops,
 *                          sub_mb_type/ref_idx/CBP/transform8x8-flag/
 *                          mb_qp_delta bookkeeping). That overlap is the
 *                          point: mbparse_us is a *direct* measurement of
 *                          the same quantity the remainder above only
 *                          derives by subtraction, so comparing the two on
 *                          a real capture settles whether the remainder
 *                          genuinely *is* pf_parse_inter_mb (mbparse_us
 *                          tracks it closely) or there is further,
 *                          still-unattributed cost beyond it (mbparse_us
 *                          reads meaningfully smaller).
 *
 * Like ih264d_stage_profile.c, this module always compiles in (portable C,
 * no MR_M68K_ASM guard) so mr_h264.c's reset/get calls are unconditionally
 * safe - the accumulators just stay at zero unless something is actually
 * feeding them. Only the five feed sites (gated by MR_H264_CABAC_PROFILE
 * in ih264d_cabac_wrap.c / ih264d_parse_cabac_coeff_port.c /
 * ih264d_mvpred_dispatch_port.c / ih264d_mbinfo_wrap_port.c, the last of
 * which feeds both mbinfo_us and mbparse_us) cost anything, and only in a
 * build that opted in (Makefile.amiga CABAC_PROFILE=1, mirroring
 * STAGE_PROFILE=1). A normal playback build pays nothing for any of this:
 * the bin_us feed site is not just disabled but replaced outright -
 * ih264d_cabac_wrap.c's C trampoline (one extra call/return per decoded
 * bin, the very overhead these counters exist to help quantify) is swapped
 * for a direct asm alias with no C call layer at all (see
 * ih264_m68k_cabac.S) - and both the mbinfo_us --wrap linker flag and the
 * mbparse_us struct-field-swap code live entirely inside ih264d_mbinfo_
 * wrap_port.c's own MR_H264_CABAC_PROFILE guard, so a normal build neither
 * links the wrap nor ever touches ps_dec->pf_parse_inter_mb from this file
 * at all - not even a branch to check.
 */

typedef struct mr_h264_cabac_us {
    unsigned long bin_us, bin_count;
    unsigned long coeff_us, coeff_count;
    unsigned long mvpred_us, mvpred_count;
    unsigned long mbinfo_us, mbinfo_count;
    unsigned long mbparse_us, mbparse_count;
} mr_h264_cabac_us;

/* Called only from the five MR_H264_CABAC_PROFILE-gated feed sites above -
 * never called at all in a normal build. */
void mr_h264_cabac_profile_add_bin(unsigned long us);
void mr_h264_cabac_profile_add_coeff(unsigned long us);
void mr_h264_cabac_profile_add_mvpred(unsigned long us);
void mr_h264_cabac_profile_add_mbinfo(unsigned long us);
void mr_h264_cabac_profile_add_mbparse(unsigned long us);

/* Zero the accumulators before a libavc decode sub-call - paired with
 * mr_h264_stage_profile_reset(), called from the same site in mr_h264.c. */
void mr_h264_cabac_profile_reset(void);

/* Read the accumulated per-bucket microseconds/call-counts since the last
 * reset. */
void mr_h264_cabac_profile_get(mr_h264_cabac_us *out);

#endif
