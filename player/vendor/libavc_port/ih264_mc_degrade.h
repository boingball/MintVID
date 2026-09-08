#ifndef MR_IH264_MC_DEGRADE_H
#define MR_IH264_MC_DEGRADE_H

/*
 * Inter-prediction filter selection for MintVID's H.264 speed modes.
 *
 * libavc advertises three inter-prediction qualities through its
 * IH264D_CMD_CTL_DEGRADE control (ih264d.h, i4_degrade_type):
 *
 *     bit 1 : disable deblocking
 *     bit 2 : "faster inter prediction filters"
 *     bit 3 : "fastest inter prediction filters"
 *
 * Only bit 1 still does anything in the vendored decoder.  Bits 2 and 3 set
 * ps_dec->i4_mv_frac_mask to 0 in ih264d_parse_slice.c, but nothing in the
 * tree ever *reads* that field again - ih264d_form_mb_part_info_*() extracts
 * the fractional MV with a hardcoded `& 0x3`, so every picture is
 * interpolated at full six-tap quality no matter what the application asks
 * for.  MintVID's Fast/Turbo/TurboGT modes were therefore only ever buying
 * the deblocking half of what they requested.
 *
 * Rather than patch the vendored submodule, this module supplies the missing
 * filters and selects them the same way the rest of the port swaps in m68k
 * assembly: by rewriting dec_struct_t's apf_inter_pred_luma[] /
 * pf_inter_pred_chroma function-pointer slots.
 *
 *   MR_MC_QUALITY_FULL      spec-exact six-tap luma, eighth-pel chroma.
 *   MR_MC_QUALITY_BILINEAR  quarter-pel luma by bilinear interpolation of
 *                           the four surrounding integer samples instead of
 *                           the separable six-tap filter (bit 2's intent).
 *                           Chroma is untouched - it is already bilinear.
 *
 * Bit 3's literal intent - truncating motion vectors to whole samples so
 * prediction becomes a block copy - was implemented and measured, and is
 * deliberately not offered: on a 320x180 CABAC stream it bought 3-4% of
 * decode time over bilinear while costing 17 dB PSNR (23.1 dB against
 * bilinear's 40.5 dB, and worse still - 17.8 dB - on low-detail content,
 * where full-sample snapping has no high-frequency texture to hide behind).
 * Bilinear is where the speed actually is.
 *
 * MR_MC_QUALITY_FULL is not merely "do nothing": it installs exact fast
 * paths for the chroma cases libavc's general routine handles with four
 * multiplies per sample even when three of the weights are zero.  See
 * chroma_dispatch() in the .c file.
 */

typedef enum {
    MR_MC_QUALITY_FULL = 0,
    MR_MC_QUALITY_BILINEAR = 1
} mr_mc_quality;

/*
 * Install the inter-prediction filter set for `quality` into `codec`, a bare
 * dec_struct_t * passed as void * so mr_h264.c - portable core code - never
 * has to include libavc's ih264d_structs.h.  Safe to call repeatedly; each
 * call rewrites all sixteen luma slots and the chroma slot from scratch
 * rather than layering on whatever was there before.
 *
 * Called once from ih264d_init_function_ptr() with MR_MC_QUALITY_FULL (so the
 * baseline table lives in exactly one place) and again from
 * mr_h264_set_speed_mode() whenever the speed mode changes.
 */

void mr_h264_port_install_inter_pred(void *codec, mr_mc_quality quality);

/*
 * As above, but takes the public iv_obj_t * handle that mr_h264.c holds and
 * unwraps pv_codec_handle itself.  Returns 0 if `handle` is NULL.
 */
int mr_h264_port_set_mc_quality(void *handle, mr_mc_quality quality);

#endif
