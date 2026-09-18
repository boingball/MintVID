/*
 * MintVID - DV (IEC 61834/SMPTE 314M) video decoder, backed by the vendored
 * libdv decode core (player/vendor/libdv, LGPL-2.1-or-later - see
 * THIRD-PARTY-LICENSES.txt).
 *
 * Handles both real DV frame geometries: 720x576 (DV/PAL, IEC 61834,
 * 4:2:0 - the camcorder AVI this decoder was originally added to fix) and
 * 720x480 (DV/NTSC or PAL/SMPTE 314M DVCPRO, 4:1:1 - libdv has no planar
 * 4:1:1 renderer, so core/mr_dv.c downsamples its packed-YUY2 output
 * itself; see that file's own header). DVCPRO50/HD's larger, faster
 * profiles are a different frame geometry entirely, not just a sampling
 * difference, and are not handled - mr_dv_open() rejects anything whose
 * width/height don't match one of the two shapes above cleanly
 * (MR_EFORMAT) rather than producing wrong pixels.
 */
#ifndef MR_DV_H
#define MR_DV_H

#include "mr_codec.h"

extern const mr_codec mr_codec_dv;

/* Off by default (dec->frame.fmt = MR_PIX_RGB24, same as every other codec
 * here). A caller that wants the raw Y/Cb/Cr planes instead - an Amiga
 * display path avoiding the RGB24 round-trip, mirroring
 * mr_h264_set_yuv_output()/mr_mpeg2_set_yuv_output() - opts in with this;
 * not yet wired into amiga/mrplay.c. */
void mr_dv_set_yuv_output(mr_decoder *dec, int enabled);

/* libdv's own decode-quality knob (dv_set_quality(), DV_QUALITY_* in
 * vendor/libdv/dv_types.h) trades picture detail for real, measured
 * decode cost - not a display-side tweak. DV_QUALITY_DC (this port's
 * MR_DV_SPEED_FAST) makes dv_parse_video_segment() skip the AC
 * coefficient VLC decode entirely (vendor/libdv/parse.c's 3-pass
 * dv_parse_ac_coeffs()/dv_parse_ac_coeffs_pass0() - the same class of
 * per-macroblock bitstream-parsing cost this file's own H.264 CABAC
 * investigation found dominating decode time there) for every block,
 * luma and chroma alike, and dv_decode_macroblock()'s IDCT then has at
 * most one (the DC) nonzero coefficient per block to transform instead
 * of however many AC terms the encoder emitted - both real, per-
 * macroblock costs removed, not merely deferred. Colour is kept (still
 * decodes/places Cb/Cr, just DC-only - flat per-8x8-block chroma, no
 * softer half-measure exists between "quality" and "no colour at all"
 * in libdv's own quality-bit design), so this only ever looks blockier,
 * never wrong or monochrome. Default is MR_DV_SPEED_QUALITY
 * (DV_QUALITY_BEST, dv_open()'s existing unconditional choice) - opt-in
 * only, nothing changes unless a caller asks. */
typedef enum {
    MR_DV_SPEED_QUALITY = 0,  /* DV_QUALITY_BEST: full colour, full AC */
    MR_DV_SPEED_FAST          /* DV_QUALITY_DC|COLOR: DC-only, still colour */
} mr_dv_speed_mode;

void mr_dv_set_speed_mode(mr_decoder *dec, mr_dv_speed_mode mode);

#endif /* MR_DV_H */
