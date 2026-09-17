/*
 * MintVID - DV (IEC 61834/SMPTE 314M) video decoder, backed by the vendored
 * libdv decode core (player/vendor/libdv, LGPL-2.1-or-later - see
 * THIRD-PARTY-LICENSES.txt).
 *
 * Scope for now: DV/PAL, IEC 61834 (consumer "type-2" DV-AVI), 720x576,
 * 4:2:0 sampling - the common camcorder case, and the one this decoder was
 * added to fix (a DV-PAL AVI that MintVID could not play at all). NTSC/
 * DVCPRO's 4:1:1 sampling is not yet supported; mr_dv_open() rejects it
 * cleanly (MR_EFORMAT) rather than producing wrong pixels. See
 * player/vendor/libdv/dv.c's own adaptation note for why.
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
