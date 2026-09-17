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

#endif /* MR_DV_H */
