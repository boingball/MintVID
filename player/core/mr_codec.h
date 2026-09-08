/*
 * MintVID - video decoder plugin interface
 *
 * The whole point of this project is a *codec-agnostic* player: one demux +
 * sync + render skeleton, with decoders that plug in behind this vtable. That
 * is what lets the same player scale from Cinepak on a bare A600/AGA up to
 * heavier codecs on a fast PiStorm/RTG machine - you add a decoder, not a
 * player.
 */
#ifndef MR_CODEC_H
#define MR_CODEC_H

#include "mr_types.h"

typedef struct mr_decoder mr_decoder;

typedef struct mr_codec {
    const char *name;
    /* AVI fourcc(s) this decoder claims (BITMAPINFOHEADER biCompression).
     * Unused slots are 0. Codecs like MPEG-4 have many aliases. */
    uint32_t    fourcc[24];
    /* Allocate decoder state for a w*h stream. */
    mr_status (*open)(mr_decoder *dec);
    /* Decode one compressed frame into dec->frame. May reuse/patch the
     * previous frame buffer (inter frames), so the buffer persists across
     * calls until close(). */
    mr_status (*decode)(mr_decoder *dec, const uint8_t *data, uint32_t len);
    void      (*close)(mr_decoder *dec);
    /* Optional (may be NULL): emit one buffered/reordered frame at end of
     * stream. Returns MR_OK while frames remain, MR_EAGAIN when drained.
     * Used by codecs with display reordering (MPEG-4 B-VOPs). */
    mr_status (*flush)(mr_decoder *dec);
} mr_codec;

struct mr_decoder {
    const mr_codec *codec;
    int             width;
    int             height;
    const uint8_t  *config;  /* borrowed container decoder setup (e.g. avcC) */
    uint32_t        config_len;
    mr_frame        frame;   /* filled in by decode()                       */
    void           *priv;    /* decoder-private state                       */
    /* Optional codec-installed callback for extra frames made ready by the
     * last decode(), without signalling end of stream. */
    mr_status     (*drain)(mr_decoder *dec);
};

/* Registry: decoders self-select by fourcc. */
const mr_codec *mr_codec_find(uint32_t fourcc);

mr_status mr_decoder_open(mr_decoder *dec, const mr_codec *codec,
                          int width, int height);
mr_status mr_decoder_open_config(mr_decoder *dec, const mr_codec *codec,
                                 int width, int height,
                                 const uint8_t *config, uint32_t config_len);
mr_status mr_decoder_decode(mr_decoder *dec, const uint8_t *data, uint32_t len);
/* Drain one additional frame made ready by the last decode() call. */
mr_status mr_decoder_drain(mr_decoder *dec);
/* Drain one reordered frame at end of stream (MR_EAGAIN when none left). */
mr_status mr_decoder_flush(mr_decoder *dec);
/* Re-open the codec with the same stream setup, discarding reference frames. */
mr_status mr_decoder_reset(mr_decoder *dec);
void      mr_decoder_close(mr_decoder *dec);

/* Individual codec descriptors (defined in their .c files). */
extern const mr_codec mr_codec_cinepak;
extern const mr_codec mr_codec_mjpeg;
extern const mr_codec mr_codec_mpeg2;
extern const mr_codec mr_codec_mpeg4;
extern const mr_codec mr_codec_msmpeg4v2;
extern const mr_codec mr_codec_wmv1;
extern const mr_codec mr_codec_wmv2;
extern const mr_codec mr_codec_h263;
extern const mr_codec mr_codec_h263;
extern const mr_codec mr_codec_msvideo1;
extern const mr_codec mr_codec_rle;
extern const mr_codec mr_codec_rawvideo;
#ifdef MR_HAVE_H264
extern const mr_codec mr_codec_h264;
#endif

#endif /* MR_CODEC_H */
