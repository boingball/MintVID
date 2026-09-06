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
#include "mr_yuv.h"

struct mr_mpeg1 {
    plm_t   *plm;
    uint8_t *fb;                 /* persistent RGB24 output                 */
    int      w, h;
    int      decim;              /* audio decimation (1, 2 or 4) for Paula  */
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
                        int no_audio)
{
    mr_mpeg1 *m = (mr_mpeg1 *)calloc(1, sizeof *m);
    if (!m) return NULL;
    /* free_when_done = 0: the caller keeps ownership of buf. */
    m->plm = plm_create_with_memory((uint8_t *)buf, len, 0);
    if (!m->plm) { free(m); return NULL; }
    plm_set_loop(m->plm, 0);
    plm_set_audio_enabled(m->plm,
                          !no_audio && plm_get_num_audio_streams(m->plm) > 0);
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

int mr_mpeg1_next(mr_mpeg1 *m, mr_frame *out, int64_t *pts_us)
{
    plm_frame_t *fr;
    if (!m) return 0;
    fr = plm_decode_video(m->plm);
    if (!fr) return 0;
    /*
     * pl_mpeg ships its own YUV->RGB pass (plm_frame_to_rgb), and MPEG-1 was
     * the one codec left using it: the 1.2.0 change that moved H.263, MPEG-2,
     * MPEG-4 Part 2, MP42/DIV2, WMV1 and WMV2 onto MintVID's shared converter
     * missed this call site. That mattered more here than anywhere else,
     * because plm_frame_to_rgb() is scalar C with no m68k path while
     * mr_yuv420_to_rgb24() dispatches to core/mr_yuv_m68k.S on Amiga builds -
     * and colour conversion, not decoding, is where an MPEG-1 frame's time
     * actually goes (44% of executed instructions on a 352x288 clip).
     *
     * pl_mpeg rounds each plane up to a whole macroblock, so the plane widths
     * are the strides and may exceed the display width; the converter is told
     * both, and reads only m->w/m->h of them.
     */
    mr_yuv420_to_rgb24(m->fb, m->w * 3,
                       fr->y.data, (int)fr->y.width,
                       fr->cb.data, (int)fr->cb.width,
                       fr->cr.data, (int)fr->cr.width,
                       m->w, m->h, NULL, NULL);
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

int mr_mpeg1_audio(mr_mpeg1 *m, unsigned char *dst)
{
    plm_samples_t *s;
    unsigned j, out = 0;
    int decim;
    if (!m) return 0;
    s = plm_decode_audio(m->plm);
    if (!s) return 0;
    decim = m->decim;
    /* Take every `decim`-th stereo sample, emit little-endian signed-16 so it
     * is correct on the big-endian 68k regardless of host byte order. */
    for (j = 0; j < s->count; j += decim) {
        int ch;
        for (ch = 0; ch < 2; ch++) {
            int v = s->interleaved[j * 2 + ch];
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
