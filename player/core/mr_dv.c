/*
 * MintVID - DV (IEC 61834/SMPTE 314M) video decoder plugin.
 *
 * Backed by the vendored libdv decode core (player/vendor/libdv). See
 * mr_dv.h for the current scope (DV/PAL 720x576 4:2:0 only).
 */
#include "mr_dv.h"
#include "mr_yuv.h"
#include "../vendor/libdv/dv.h"
#include <stdlib.h>
#include <string.h>

typedef struct {
    dv_decoder_t *dv;
    int width, height;
    uint8_t *y, *u, *v;
    int y_stride, uv_stride;
    uint8_t *rgb;
    int rgb_stride;
    int yuv_output;
} dv_ctx;

/* One DV video frame is a fixed number of 80-byte DIF blocks - 12 DIF
 * sequences * 150 blocks * 80 bytes for 625/50 (PAL), 10*150*80 for 525/60
 * (NTSC). dv_parse_header() only reads the first 6 DIF blocks (the header
 * block plus subcode/VAUX), so the exact frame size isn't known until after
 * that call - mr_avi.c hands us one full 'dc'/'db' chunk per frame already,
 * so len is trusted rather than recomputed here. */
#define DV_PAL_FRAME_BYTES  (12 * 150 * 80)
#define DV_NTSC_FRAME_BYTES (10 * 150 * 80)

static mr_status dv_open(mr_decoder *dec)
{
    dv_ctx *c;
    size_t y_bytes, uv_bytes;

    /* Scope check (see mr_dv.h): only DV/PAL 720x576 4:2:0 is supported so
     * far. 720x480 (NTSC, 4:1:1) is a real, common format this decoder does
     * not yet handle - failing cleanly here (mr_decoder_open_config()
     * returning MR_ERR -> mrplay.c's existing "decoder init failed" path)
     * beats silently producing wrong pixels. */
    if (dec->width != 720 || dec->height != 576)
        return MR_ERR;

    c = (dv_ctx *)calloc(1, sizeof *c);
    if (!c) return MR_ENOMEM;

    c->dv = dv_decoder_new(0, 1, 1);
    if (!c->dv) { free(c); return MR_ENOMEM; }
    /* Best (colour, full AC) quality - this is a v1 CPU-decode integration,
     * not yet tuned for slower Amiga targets; see mr_dv.h. */
    dv_set_quality(c->dv, DV_QUALITY_BEST);

    c->width = dec->width;
    c->height = dec->height;
    c->y_stride = c->width;
    c->uv_stride = c->width / 2;
    y_bytes  = (size_t)c->y_stride * c->height;
    uv_bytes = (size_t)c->uv_stride * (c->height / 2);

    c->y = (uint8_t *)malloc(y_bytes);
    c->u = (uint8_t *)malloc(uv_bytes);
    c->v = (uint8_t *)malloc(uv_bytes);
    c->rgb_stride = c->width * 3;
    c->rgb = (uint8_t *)malloc((size_t)c->rgb_stride * c->height);
    if (!c->y || !c->u || !c->v || !c->rgb) {
        free(c->y); free(c->u); free(c->v); free(c->rgb);
        dv_decoder_free(c->dv);
        free(c);
        return MR_ENOMEM;
    }

    dec->priv = c;
    dec->frame.width  = c->width;
    dec->frame.height = c->height;
    /* Default to RGB24 - the host test harness (tests/mr_decode.c's
     * write_ppm()/check_ppm()) only ever reads dec->frame.data as packed
     * RGB24, same as every other codec here; mr_dv_set_yuv_output() below
     * (mirroring mr_h264_set_yuv_output()/mr_mpeg2_set_yuv_output()) is the
     * opt-in an Amiga display path would use once wired up, to skip this
     * conversion and consume the Y/Cb/Cr planes directly. */
    dec->frame.fmt    = MR_PIX_RGB24;
    dec->frame.stride = c->rgb_stride;
    dec->frame.data   = c->rgb;
    dec->frame.u_data = NULL;
    dec->frame.v_data = NULL;
    dec->frame.u_stride = 0;
    dec->frame.v_stride = 0;
    return MR_OK;
}

void mr_dv_set_yuv_output(mr_decoder *dec, int enabled)
{
    dv_ctx *c;
    if (!dec || dec->codec != &mr_codec_dv || !dec->priv) return;
    c = (dv_ctx *)dec->priv;
    c->yuv_output = enabled != 0;
    if (c->yuv_output) {
        dec->frame.fmt      = MR_PIX_YUV420P;
        dec->frame.stride   = c->y_stride;
        dec->frame.data     = c->y;
        dec->frame.u_data   = c->u;
        dec->frame.v_data   = c->v;
        dec->frame.u_stride = c->uv_stride;
        dec->frame.v_stride = c->uv_stride;
    } else {
        dec->frame.fmt      = MR_PIX_RGB24;
        dec->frame.stride   = c->rgb_stride;
        dec->frame.data     = c->rgb;
        dec->frame.u_data   = NULL;
        dec->frame.v_data   = NULL;
        dec->frame.u_stride = 0;
        dec->frame.v_stride = 0;
    }
}

static mr_status dv_decode(mr_decoder *dec, const uint8_t *data, uint32_t len)
{
    dv_ctx *c = (dv_ctx *)dec->priv;
    uint8_t *pixels[3];
    uint16_t pitches[3];

    if (!data || len < DV_PAL_FRAME_BYTES) return MR_EFORMAT;
    if (dv_parse_header(c->dv, data) < 0) return MR_EFORMAT;
    /* dv_parse_header() derives system/sampling from *this* frame's own
     * header block - a stream this decoder opened as 720x576 could still
     * (in principle) hand it an NTSC frame. Reject rather than misdecode. */
    if (c->dv->system != e_dv_system_625_50 ||
        c->dv->sampling != e_dv_sample_420)
        return MR_EFORMAT;

    dv_parse_packs(c->dv, data);

    pixels[0] = c->y;
    pixels[1] = c->v;
    pixels[2] = c->u;
    pitches[0] = (uint16_t)c->y_stride;
    pitches[1] = (uint16_t)c->uv_stride;
    pitches[2] = (uint16_t)c->uv_stride;

    dv_decode_full_frame(c->dv, data, e_dv_color_yuv, pixels, pitches);

    if (!c->yuv_output)
        mr_yuv420_to_rgb24(c->rgb, c->rgb_stride,
                            c->y, c->y_stride, c->u, c->uv_stride,
                            c->v, c->uv_stride, c->width, c->height,
                            NULL, NULL);

    dec->frame.dirty_y0 = 0;
    dec->frame.dirty_y1 = c->height;
    return MR_OK;
}

static void dv_close(mr_decoder *dec)
{
    dv_ctx *c = (dv_ctx *)dec->priv;
    if (!c) return;
    if (c->dv) dv_decoder_free(c->dv);
    free(c->y); free(c->u); free(c->v); free(c->rgb);
    free(c);
    dec->priv = NULL;
}

/* AVI fccHandler/biCompression tags seen in the wild for DV video (see
 * core/mr_codec.c's case-insensitive matching note - a decoder lists each
 * tag once, muxers stamp whichever case they like):
 *   dvsd/DVSD - standard consumer DV ("type-1"/"type-2" DV-AVI)
 *   dvc /DVC  - the space-padded FourCC some capture tools use
 *   CDVC/cdvc - Canopus's own DV FourCC (same bitstream)
 *   dvsl/DVSL - DVCPRO (SMPTE 314M, still 4:2:0 at 625/50)
 * dvhd/DVHD (DVCPRO50/HD) are deliberately not listed: those are always
 * 4:1:1 or higher-resolution profiles this decoder does not yet support,
 * and mr_codec_find() matching them to this decoder would just turn a
 * clean "no decoder" report into a confusing "decoder init failed" one. */
const mr_codec mr_codec_dv = {
    "DV (IEC 61834/SMPTE 314M)",
    { MR_FOURCC('d','v','s','d'), MR_FOURCC('D','V','S','D'),
      MR_FOURCC('d','v','c',' '), MR_FOURCC('D','V','C',' '),
      MR_FOURCC('C','D','V','C'), MR_FOURCC('c','d','v','c'),
      MR_FOURCC('d','v','s','l'), MR_FOURCC('D','V','S','L'), 0 },
    dv_open, dv_decode, dv_close, NULL
};
