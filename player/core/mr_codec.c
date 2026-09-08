/*
 * MintVID - decoder registry and lifecycle glue.
 */
#include "mr_codec.h"

static const mr_codec *const g_codecs[] = {
    &mr_codec_cinepak,
    &mr_codec_mjpeg,
    &mr_codec_mpeg2,
    &mr_codec_mpeg4,
    &mr_codec_msmpeg4v2,
    &mr_codec_wmv1,
    &mr_codec_wmv2,
    &mr_codec_h263,
    &mr_codec_msvideo1,
    &mr_codec_rle,
    &mr_codec_rawvideo,
#ifdef MR_HAVE_H264
    &mr_codec_h264,
#endif
};

const mr_codec *mr_codec_find(uint32_t fourcc)
{
    size_t i, j;
    for (i = 0; i < sizeof(g_codecs) / sizeof(g_codecs[0]); i++) {
        const mr_codec *c = g_codecs[i];
        for (j = 0; j < sizeof(c->fourcc) / sizeof(c->fourcc[0]); j++) {
            if (c->fourcc[j] && c->fourcc[j] == fourcc)
                return c;
        }
    }
    return NULL;
}

mr_status mr_decoder_open(mr_decoder *dec, const mr_codec *codec,
                          int width, int height)
{
    return mr_decoder_open_config(dec, codec, width, height, NULL, 0);
}

mr_status mr_decoder_open_config(mr_decoder *dec, const mr_codec *codec,
                                 int width, int height,
                                 const uint8_t *config, uint32_t config_len)
{
    if (!dec || !codec || width <= 0 || height <= 0)
        return MR_ERR;
    dec->codec  = codec;
    dec->width  = width;
    dec->height = height;
    dec->config = config;
    dec->config_len = config_len;
    dec->priv   = NULL;
    dec->drain  = NULL;
    dec->frame.data = NULL;
    return codec->open(dec);
}

mr_status mr_decoder_decode(mr_decoder *dec, const uint8_t *data, uint32_t len)
{
    if (!dec || !dec->codec)
        return MR_ERR;
    return dec->codec->decode(dec, data, len);
}

mr_status mr_decoder_drain(mr_decoder *dec)
{
    if (!dec || !dec->codec || !dec->drain)
        return MR_EAGAIN;
    return dec->drain(dec);
}

mr_status mr_decoder_flush(mr_decoder *dec)
{
    if (!dec || !dec->codec || !dec->codec->flush)
        return MR_EAGAIN;
    return dec->codec->flush(dec);
}

mr_status mr_decoder_reset(mr_decoder *dec)
{
    const mr_codec *codec;
    int width, height;
    const uint8_t *config;
    uint32_t config_len;
    if (!dec || !dec->codec) return MR_ERR;
    codec = dec->codec; width = dec->width; height = dec->height;
    config = dec->config; config_len = dec->config_len;
    if (codec->close) codec->close(dec);
    return mr_decoder_open_config(dec, codec, width, height, config, config_len);
}

void mr_decoder_close(mr_decoder *dec)
{
    if (dec && dec->codec && dec->codec->close)
        dec->codec->close(dec);
}
