#ifndef MR_IH264D_STAGE_PROFILE_H
#define MR_IH264D_STAGE_PROFILE_H

/*
 * Diagnostic-only timing breakdown of libavc decode, added to find out
 * where a real-hardware Pistorm frame's ~150-300ms libavc-core cost
 * actually goes: motion compensation, deblocking, IDCT/reconstruction, and
 * intra prediction each sit behind their own function-pointer table in
 * dec_struct_t, so each can be wrapped with a clock()-based timer without
 * touching a single line of the vendored libavc source - the same pattern
 * this port already uses to swap in m68k asm
 * (ih264d_function_selector_port.c), just measuring instead of replacing.
 *
 * There is no function pointer for bitstream/CABAC/CAVLC parsing or MV
 * prediction, so those are not broken out here; treat core_us minus
 * (mc_us+deblock_us+recon_us+intra_us) as that combined remainder.
 *
 * mr_h264_stage_profile_install() below is only ever called when
 * MR_H264_STAGE_PROFILE is defined (see ih264d_function_selector_port.c
 * and Makefile.amiga's STAGE_PROFILE variable) - every wrapped slot costs
 * two clock() calls per invocation, on leaf pixel filters called tens of
 * thousands of times a frame, which is real overhead on the actual m68k
 * target this instrumentation exists to help speed up. A normal playback
 * build never installs these wrappers, so mc_us/deblock_us/recon_us/
 * intra_us stay at zero and cost nothing.
 */

typedef struct mr_h264_stage_us {
    unsigned long mc_us;
    unsigned long deblock_us;
    unsigned long recon_us;
    unsigned long intra_us;
} mr_h264_stage_us;

/* Wrap this codec instance's apf_inter_pred_luma[]/pf_inter_pred_chroma/
 * deblock/recon function pointers with timing instrumentation. mc_us covers
 * chroma prediction as well as luma - chroma is motion compensation too, and
 * was the hottest single function in a decode before ih264_mc_degrade.c's
 * exact dx/dy shortcuts. Call once, after
 * ih264d_init_function_ptr_generic() and any MR_M68K_ASM overrides have set
 * up the real implementations to measure - this captures whatever is
 * sitting in each slot at that point (generic C or our asm) and installs a
 * thin pass-through wrapper in its place.
 *
 * Takes a bare dec_struct_t* as void* so this header - included from
 * mr_h264.c, which is portable core code - never has to pull in libavc's
 * ih264d_structs.h just to spell the parameter type. */
void mr_h264_stage_profile_install(void *codec);

/* Re-apply the motion-compensation wrappers after something else has
 * rewritten apf_inter_pred_luma[] - mr_h264_set_speed_mode() switching filter
 * sets, via ih264_mc_degrade.c. No-op until mr_h264_stage_profile_install()
 * has run, so a normal playback build (and the install-time call order) costs
 * nothing. */
void mr_h264_stage_profile_rewrap_mc(void *codec);

/* Zero the accumulators before a libavc decode sub-call. */
void mr_h264_stage_profile_reset(void);

/* Read the accumulated per-stage microseconds since the last reset. */
void mr_h264_stage_profile_get(mr_h264_stage_us *out);

#endif
