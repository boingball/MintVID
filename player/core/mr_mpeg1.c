/*
 * MintVID - MPEG-1 source, wrapping pl_mpeg.
 *
 * This translation unit carries pl_mpeg's implementation (PL_MPEG_IMPLEMENTATION),
 * so it is the one place its integer-only MP2 code is compiled.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PL_MPEG_IMPLEMENTATION
#include "pl_mpeg.h"

#include "mr_mpeg1.h"

struct mr_mpeg1 {
    plm_t   *plm;
    uint8_t *fb;                 /* persistent RGB24 output                 */
    int      w, h;
    int      decim;              /* audio decimation (1, 2 or 4) for Paula  */
    int      channels;           /* PCM channels emitted (1 in mono mode)   */
    unsigned rate_eff;           /* effective audio rate after decimation   */
};

enum mr_mpeg_ps_kind {
    MR_MPEG_PS_UNKNOWN,
    MR_MPEG_PS_MPEG1_VIDEO,
    MR_MPEG_PS_MPEG2_VIDEO
};

static enum mr_mpeg_ps_kind mpeg_ps_kind(const uint8_t *buf, size_t len)
{
    size_t i;
    int saw_sequence = 0;

    /* A pack header alone identifies the container, not its video codec.
     * In particular, DVD-style MPEG-2 program streams use the same start
     * code and must not be handed to pl_mpeg's MPEG-1 video decoder. */
    if (!buf || len < 4 || buf[0] != 0x00 || buf[1] != 0x00 ||
        buf[2] != 0x01 || buf[3] != 0xBA)
        return MR_MPEG_PS_UNKNOWN;

    for (i = 4; i + 4 < len; i++) {
        unsigned code;
        if (buf[i] != 0x00 || buf[i + 1] != 0x00 || buf[i + 2] != 0x01)
            continue;
        code = buf[i + 3];
        if (code == 0xB3) {              /* sequence header */
            saw_sequence = 1;
        } else if (saw_sequence && code == 0xB5 &&
                   (buf[i + 4] >> 4) == 1) { /* MPEG-2 sequence extension */
            return MR_MPEG_PS_MPEG2_VIDEO;
        } else if (saw_sequence && code == 0x00) { /* first picture */
            return MR_MPEG_PS_MPEG1_VIDEO;
        }
    }
    return MR_MPEG_PS_UNKNOWN;
}

int mr_mpeg1_probe(const uint8_t *buf, size_t len)
{
    return mpeg_ps_kind(buf, len) == MR_MPEG_PS_MPEG1_VIDEO;
}

int mr_mpeg2_ps_probe(const uint8_t *buf, size_t len)
{
    return mpeg_ps_kind(buf, len) == MR_MPEG_PS_MPEG2_VIDEO;
}

mr_mpeg1 *mr_mpeg1_open(const uint8_t *buf, size_t len, int low_rate,
                        int no_audio, int mono)
{
    mr_mpeg1 *m = (mr_mpeg1 *)calloc(1, sizeof *m);
    if (!m) return NULL;
    /* free_when_done = 0: the caller keeps ownership of buf. */
    m->plm = plm_create_with_memory((uint8_t *)buf, len, 0);
    if (!m->plm) { free(m); return NULL; }
    plm_set_loop(m->plm, 0);
    plm_set_audio_mono(m->plm, mono);
    plm_set_audio_enabled(m->plm,
                          !no_audio && plm_get_num_audio_streams(m->plm) > 0);
    m->channels = mono ? 1 : 2;
    m->w = plm_get_width(m->plm);
    m->h = plm_get_height(m->plm);
    if (m->w <= 0 || m->h <= 0) { plm_destroy(m->plm); free(m); return NULL; }
    m->fb = (uint8_t *)calloc((size_t)m->w * m->h * 3, 1);
    if (!m->fb) { plm_destroy(m->plm); free(m); return NULL; }
    { /* Paula tops out near ~28 kHz; halve higher rates (44.1/48) to fit,
       * doubled again under low_rate. no_audio (plm_set_audio_enabled(0)
       * above) makes raw 0 here, so rate_eff comes out 0 exactly like a
       * video-only stream - mr_mpeg1_samplerate() needs no separate
       * no_audio check. */
        int raw = (!no_audio && plm_get_num_audio_streams(m->plm) > 0)
                ? plm_get_samplerate(m->plm) : 0;
        m->decim = (raw > 28000) ? 2 : 1;
        if (low_rate) m->decim *= 2;
        m->rate_eff = (unsigned)(raw / m->decim);
    }
    return m;
}

int mr_mpeg1_width(mr_mpeg1 *m)  { return m ? m->w : 0; }
int mr_mpeg1_height(mr_mpeg1 *m) { return m ? m->h : 0; }
unsigned mr_mpeg1_framerate_millihz(mr_mpeg1 *m) { return m ? plm_get_framerate(m->plm) : 0; }

unsigned mr_mpeg1_samplerate(mr_mpeg1 *m)
{
    return m ? m->rate_eff : 0;
}

int mr_mpeg1_channels(mr_mpeg1 *m)
{
    return m ? m->channels : 0;
}

int mr_mpeg1_next(mr_mpeg1 *m, mr_frame *out, int64_t *pts_us)
{
    plm_frame_t *fr;
    if (!m) return 0;
    fr = plm_decode_video(m->plm);
    if (!fr) return 0;
    /*
     * Deliberately pl_mpeg's own converter, not MintVID's shared
     * mr_yuv420_to_rgb24(), even though every other codec uses the shared
     * one. Switching this over was tried and measured worse: both are
     * multiply-free (GCC strength-reduces pl_mpeg's constant multiplies into
     * shift/add chains - checking the disassembly rather than the source is
     * what settled that) and both step 2x2 quads, but on a 352x288 frame
     * under qemu-m68k pl_mpeg's costs 649ms/300 against the shared
     * converter's 821ms with core/mr_yuv_m68k.S active, and its output is
     * also marginally closer to the ffmpeg reference (worst MAE 0.963 vs
     * 1.161). Revisit if the YUV_ASM=0 question settles in the portable C's
     * favour on real hardware - the shared C form measures 661ms, a wash with
     * this, and then one validated converter for everything would win on
     * merit rather than only on tidiness.
     */
    plm_frame_to_rgb(fr, m->fb, m->w * 3);
    out->width  = m->w;
    out->height = m->h;
    out->fmt    = MR_PIX_RGB24;
    out->stride = m->w * 3;
    out->data   = m->fb;
    out->dirty_y0 = 0;                          /* inter frames, but simplest */
    out->dirty_y1 = m->h;                       /* is a full repaint          */
    if (pts_us) *pts_us = fr->time;
    return 1;
}

int mr_mpeg1_next_yuv(mr_mpeg1 *m, mr_frame *out, int64_t *pts_us)
{
    plm_frame_t *fr;
    if (!m) return 0;
    fr = plm_decode_video(m->plm);
    if (!fr) return 0;
    /*
     * The planes as pl_mpeg decoded them, with no RGB24 buffer in between.
     * plm_frame_t's plane `width` is the macroblock-aligned allocation pitch
     * (144 for a 134-wide clip), not the visible width, which is exactly the
     * stride mr_frame wants; frame->width/height stay the display size. Note
     * the naming: MR_PIX_YUV420P is Y, Cb, Cr, so u is pl_mpeg's cb and v is
     * its cr - the one thing here that is silent if swapped, and shows up
     * only as wrong colour.
     *
     * Borrowed, not copied: these point into the decoder's own frame and stay
     * valid only until the next mr_mpeg1_next*() call, which is all the
     * caller needs - it converts straight to chunky pixels before decoding
     * again.
     */
    out->width  = m->w;
    out->height = m->h;
    out->fmt    = MR_PIX_YUV420P;
    out->data   = fr->y.data;
    out->stride = (int)fr->y.width;
    out->u_data = fr->cb.data;
    out->v_data = fr->cr.data;
    out->u_stride = (int)fr->cb.width;
    out->v_stride = (int)fr->cr.width;
    out->dirty_y0 = 0;
    out->dirty_y1 = m->h;
    if (pts_us) *pts_us = fr->time;
    return 1;
}

int mr_mpeg1_audio(mr_mpeg1 *m, unsigned char *dst)
{
    plm_samples_t *s;
    unsigned j, out = 0;
    int decim, channels;
    if (!m) return 0;
    s = plm_decode_audio(m->plm);
    if (!s) return 0;
    decim = m->decim;
    channels = m->channels;
    /* Take every `decim`-th sample frame, emit little-endian signed-16 so it
     * is correct on the big-endian 68k regardless of host byte order. In mono
     * mode pl_mpeg has already packed one channel at the front of the buffer
     * (see plm_set_audio_mono()), so the frame stride is one sample. */
    for (j = 0; j < s->count; j += decim) {
        int ch;
        for (ch = 0; ch < channels; ch++) {
            int v = s->interleaved[j * channels + ch];
            *dst++ = (unsigned char)(v & 0xff);
            *dst++ = (unsigned char)((v >> 8) & 0xff);
        }
        out++;
    }
    return (int)out;
}

void mr_mpeg1_rewind(mr_mpeg1 *m) { if (m) plm_rewind(m->plm); }

void mr_mpeg1_close(mr_mpeg1 *m)
{
    if (!m) return;
    if (m->plm) plm_destroy(m->plm);
    free(m->fb);
    free(m);
}
