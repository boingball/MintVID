/*
 * Portable libavc function selector for architectures without a specialised
 * backend.  In particular this keeps the m68k build on libavc's integer C
 * implementations instead of pulling in x86/ARM assembly.
 */
#include "ih264_typedefs.h"
#include "iv.h"
#include "ivd.h"
#include "ih264d_structs.h"
#include "ih264d_function_selector.h"
#include "ih264_m68k_optim.h"
#include "ih264_m68k_bs.h"
#include "ih264_mc_degrade.h"
#include "ih264d_stage_profile.h"

void ih264d_init_function_ptr(dec_struct_t *codec)
{
    ih264d_init_function_ptr_generic(codec);
    /* Inter prediction - the sixteen-entry six-tap luma table and the chroma
     * filter - is owned by ih264_mc_degrade.c, which also supplies the
     * reduced-quality filter sets mr_h264_set_speed_mode() switches to when
     * the application asks libavc to degrade (see that file for why libavc's
     * own i4_degrade_type inter-prediction bits no longer do anything).
     * Installed unconditionally rather than under MR_M68K_ASM, so the exact
     * chroma fast paths run - and the host conformance suite validates them -
     * on every build, not only the Amiga one. */
    mr_h264_port_install_inter_pred(codec, MR_MC_QUALITY_FULL);
    /* MR_M68K_ASM is an explicit build flag (set by Makefile.amiga and
     * tests/run_m68k_check.sh), not GCC's own __mc68000__ predefine: a real
     * m68k-amigaos-gcc build hit an undefined-reference link error against
     * ih264_m68k_interp.S's functions even though this file's __mc68000__
     * guard clearly *did* activate (the call sites below were compiled in) -
     * meaning that predefine did not reach the .S file identically on that
     * toolchain, for reasons not reproducible on the m68k-linux-gnu test
     * toolchain here. An explicit, build-system-controlled flag removes the
     * dependency on that predefine matching across every GCC fork/version
     * this project might be built with, on both a real AmigaOS target and
     * the m68k-linux-gnu test build `make check-m68k` uses to exercise this
     * through the real decode pipeline under qemu-m68k instead of only in
     * unit-test isolation. Deliberately narrower than AMIGA_M68K: it says
     * nothing about dos.h/exec.h being available, unlike mr_h264.c's
     * diagnostic hooks. */
#if defined(MR_M68K_ASM)
    /* Replace only bit-exact leaf primitives.  Keeping selection here, rather
     * than modifying the imported libavc tree, makes the port auditable and
     * leaves every non-m68k build on Ittiam's reference C implementation. */
    codec->pf_default_weighted_pred_luma =
        mr_ih264_default_weighted_pred_luma_m68k;
    codec->pf_default_weighted_pred_chroma =
        mr_ih264_default_weighted_pred_chroma_m68k;
    codec->pf_weighted_pred_luma = mr_ih264_weighted_pred_luma_m68k;
    codec->pf_weighted_pred_chroma = mr_ih264_weighted_pred_chroma_m68k;
    codec->pf_weighted_bi_pred_luma = mr_ih264_weighted_bi_pred_luma_m68k;
    codec->pf_weighted_bi_pred_chroma = mr_ih264_weighted_bi_pred_chroma_m68k;
    codec->apf_intra_pred_luma_16x16[0] =
        mr_ih264_intra_pred_luma_16x16_vert_m68k;
    codec->apf_intra_pred_luma_16x16[1] =
        mr_ih264_intra_pred_luma_16x16_horz_m68k;
    codec->apf_intra_pred_luma_16x16[2] =
        mr_ih264_intra_pred_luma_16x16_dc_m68k;
    codec->apf_intra_pred_luma_16x16[3] =
        mr_ih264_intra_pred_luma_16x16_plane_m68k;
    codec->apf_intra_pred_luma_4x4[0] = mr_ih264_intra_pred_luma_4x4_vert_m68k;
    codec->apf_intra_pred_luma_4x4[1] = mr_ih264_intra_pred_luma_4x4_horz_m68k;
    codec->apf_intra_pred_luma_4x4[2] = mr_ih264_intra_pred_luma_4x4_dc_m68k;
    codec->apf_intra_pred_luma_8x8[0] = mr_ih264_intra_pred_luma_8x8_vert_m68k;
    codec->apf_intra_pred_luma_8x8[1] = mr_ih264_intra_pred_luma_8x8_horz_m68k;
    codec->apf_intra_pred_luma_8x8[2] = mr_ih264_intra_pred_luma_8x8_dc_m68k;
    codec->apf_intra_pred_luma_8x8[3] =
        mr_ih264_intra_pred_luma_8x8_diag_dl_m68k;
    codec->apf_intra_pred_luma_8x8[4] =
        mr_ih264_intra_pred_luma_8x8_diag_dr_m68k;
    codec->apf_intra_pred_luma_8x8[5] =
        mr_ih264_intra_pred_luma_8x8_vert_r_m68k;
    codec->apf_intra_pred_luma_8x8[6] =
        mr_ih264_intra_pred_luma_8x8_horz_d_m68k;
    codec->apf_intra_pred_luma_8x8[7] =
        mr_ih264_intra_pred_luma_8x8_vert_l_m68k;
    codec->apf_intra_pred_luma_8x8[8] =
        mr_ih264_intra_pred_luma_8x8_horz_u_m68k;
    codec->pf_intra_pred_ref_filtering =
        mr_ih264_intra_pred_luma_8x8_ref_filtering_m68k;
    codec->apf_intra_pred_chroma[0] = mr_ih264_intra_pred_chroma_8x8_vert_m68k;
    codec->apf_intra_pred_chroma[1] = mr_ih264_intra_pred_chroma_8x8_horz_m68k;
    codec->apf_intra_pred_chroma[2] = mr_ih264_intra_pred_chroma_8x8_dc_m68k;
    codec->apf_intra_pred_chroma[3] =
        mr_ih264_intra_pred_chroma_8x8_plane_m68k;
    codec->pf_deblk_luma_vert_bs4 = mr_ih264_deblk_luma_vert_bs4_m68k;
    codec->pf_deblk_luma_horz_bs4 = mr_ih264_deblk_luma_horz_bs4_m68k;
    codec->pf_deblk_luma_vert_bslt4 = mr_ih264_deblk_luma_vert_bslt4_m68k;
    codec->pf_deblk_luma_horz_bslt4 = mr_ih264_deblk_luma_horz_bslt4_m68k;
    codec->pf_deblk_chroma_vert_bs4 = mr_ih264_deblk_chroma_vert_bs4_m68k;
    codec->pf_deblk_chroma_horz_bs4 = mr_ih264_deblk_chroma_horz_bs4_m68k;
    codec->pf_deblk_chroma_vert_bslt4 = mr_ih264_deblk_chroma_vert_bslt4_m68k;
    codec->pf_deblk_chroma_horz_bslt4 = mr_ih264_deblk_chroma_horz_bslt4_m68k;
    codec->pf_iquant_itrans_recon_luma_4x4 =
        mr_ih264_iquant_itrans_recon_4x4_m68k;
    codec->pf_iquant_itrans_recon_luma_4x4_dc =
        mr_ih264_iquant_itrans_recon_4x4_dc_m68k;
    codec->pf_iquant_itrans_recon_chroma_4x4 =
        mr_ih264_iquant_itrans_recon_chroma_4x4_m68k;
    codec->pf_iquant_itrans_recon_chroma_4x4_dc =
        mr_ih264_iquant_itrans_recon_chroma_4x4_dc_m68k;
    codec->pf_iquant_itrans_recon_luma_8x8 =
        mr_ih264_iquant_itrans_recon_8x8_m68k;
    codec->pf_iquant_itrans_recon_luma_8x8_dc =
        mr_ih264_iquant_itrans_recon_8x8_dc_m68k;
    codec->pf_fill_bs1[0][0] =
        mr_ih264d_fill_bs1_16x16mb_pslice_m68k;
    codec->pf_fill_bs1[0][1] =
        mr_ih264d_fill_bs1_non16x16mb_pslice_m68k;
    codec->pf_fill_bs1[1][0] =
        mr_ih264d_fill_bs1_16x16mb_bslice_m68k;
    codec->pf_fill_bs1[1][1] =
        mr_ih264d_fill_bs1_non16x16mb_bslice_m68k;
#endif
#if defined(MR_H264_STAGE_PROFILE)
    /* Diagnostic-only, opt-in: wraps whatever is now sitting in the
     * MC/deblock/recon/intra function-pointer slots above (m68k asm or
     * Ittiam's generic C) with clock()-based timing, reported via
     * mrplay.c's "h264 stages:" line. See ih264d_stage_profile.h.
     *
     * Deliberately NOT installed unless MR_H264_STAGE_PROFILE is defined:
     * every wrapped slot costs two clock() calls per invocation, and these
     * are leaf pixel filters called tens of thousands of times a frame -
     * on the actual m68k target this instrumentation exists to help speed
     * up, that overhead is not something a normal playback build should
     * pay. Enable explicitly for a profiling build (Makefile.amiga
     * STAGE_PROFILE=1; already on for tests/run_m68k_check.sh, which wants
     * this code path exercised as real regression coverage even though the
     * conformance suite itself doesn't consume the timing output). */
    mr_h264_stage_profile_install(codec);
#endif
}

void ih264d_init_arch(dec_struct_t *codec)
{
    codec->e_processor_arch = ARCH_X86_GENERIC;
}
