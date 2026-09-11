/*
 * Microsoft Video 1 (MSVC/CRAM), 8-bit paletted and RGB555.
 *
 * The format is bottom-up in both dimensions: the bottom row of 4x4 blocks is
 * coded first, and inside a block the bottom pixel row comes first. Getting
 * that flip wrong mirrors every block vertically - fine horizontal banding
 * rather than an obvious failure - so it is easy to miss without a reference.
 *
 * The second header byte `b` selects how a block is coded:
 *   (b & 0xfc) == 0x84   run of skipped blocks (keep the previous frame)
 *   b < 0x80             2-colour block (16-bit: 2- or 8-colour, see below)
 *   b >= 0x90            8-colour block (8-bit mode only)
 *   otherwise            flat 1-colour block
 * In 16-bit mode the 8-colour case is not signalled by `b` at all but by bit
 * 15 of the *first* colour word. Those paths must stay distinct: treating an
 * 8-colour block as a flat one leaves its six extra colour words in the
 * stream, so the frame decodes correctly up to the first such block and is
 * noise from there on - and because the frame is built bottom-up, that reads
 * as a correct strip along the bottom under a screen of garbage.
 *
 * Blocks cover width/4 by height/4 whole blocks; any partial block at the
 * right or bottom edge is simply not coded by the format. The framebuffer
 * persists across frames, so skip runs and inter frames patch it in place.
 */
#include "mr_msvideo1.h"
#include "mr_dither.h"
#include <stdlib.h>
#include <string.h>

typedef struct {
    int      w, h;          /* frame size in pixels                        */
    int      bw, bh;        /* frame size in whole 4x4 blocks              */
    int      stride, bits;
    int      indexed_depth; /* 0 => RGB24; otherwise 4, 5 or 8             */
    int      decoded_any;
    uint8_t *fb;            /* persistent RGB24 or INDEX8 framebuffer      */
    uint8_t  pal[768];
    uint8_t  indexed_pal[16][256]; /* 8-bit palette at each Bayer phase   */
} ms1_ctx;

static void rgb555(unsigned v, uint8_t *d)
{
    unsigned r = (v >> 10) & 31, g = (v >> 5) & 31, b = v & 31;
    d[0] = (uint8_t)((r << 3) | (r >> 2));
    d[1] = (uint8_t)((g << 3) | (g >> 2));
    d[2] = (uint8_t)((b << 3) | (b >> 2));
}

/* Paint one 4x4 block at block coordinates (bx,by). `flags` carries one
 * selector bit per pixel, consumed LSB-first from the block's bottom-left
 * pixel, running left to right and then upwards. `nc` is 1, 2 or 8 colours;
 * the 8-colour layout pairs two colours per 2x2 quadrant, indexed in the same
 * bottom-up pixel space. Note the selector bit is *inverted*: a set bit
 * selects the lower-numbered colour of the pair. */
static void put_block(ms1_ctx *c, int bx, int by, unsigned flags,
                      const unsigned *cols, int nc)
{
    uint8_t rgb[8][3];
    int px, py;
    int i;

    if (!c->indexed_depth) {
        for (i = 0; i < nc; i++) {
            if (c->bits == 8)
                memcpy(rgb[i], c->pal + (cols[i] & 255) * 3, 3);
            else
                rgb555(cols[i], rgb[i]);
        }
    } else if (c->bits == 16) {
        for (i = 0; i < nc; i++) rgb555(cols[i], rgb[i]);
    }

    for (py = 0; py < 4; py++) {
        int y = by * 4 + 3 - py;
        for (px = 0; px < 4; px++, flags >>= 1) {
            int x = bx * 4 + px;
            if (nc == 1)      i = 0;
            else if (nc == 2) i = (int)(flags & 1) ^ 1;
            else              i = ((py & 2) << 1) + (px & 2) +
                                  ((int)(flags & 1) ^ 1);
            /* bx/by enumerate only complete 4x4 blocks, so this hot loop
             * needs no per-pixel clipping. */
            if (c->indexed_depth) {
                uint8_t value;
                if (c->bits == 8)
                    value = c->indexed_pal[((y & 3) << 2) | (x & 3)]
                                          [cols[i] & 255];
                else
                    value = mr_dither_rgb_indexed_pixel(
                                rgb[i][0], rgb[i][1], rgb[i][2], x, y,
                                c->indexed_depth);
                c->fb[(size_t)y * c->stride + x] = value;
            } else {
                uint8_t *dst = c->fb + (size_t)y * c->stride + x * 3;
                dst[0] = rgb[i][0]; dst[1] = rgb[i][1]; dst[2] = rgb[i][2];
            }
        }
    }
}

static mr_status ms1_open(mr_decoder *d)
{
    ms1_ctx *c;
    size_t n;
    if (d->width < 4 || d->height < 4) return MR_EUNSUPPORTED;
    c = (ms1_ctx *)calloc(1, sizeof(*c));
    if (!c) return MR_ENOMEM;
    c->w = d->width; c->h = d->height; c->stride = c->w * 3;
    c->bw = c->w / 4; c->bh = c->h / 4;
    c->bits = (d->config_len >= 2) ? mr_rl16(d->config) : 16;
    if (c->bits != 8 && c->bits != 16) { free(c); return MR_EUNSUPPORTED; }
    if (c->bits == 8 && d->config_len > 2) {
        uint32_t z = d->config_len - 2;
        if (z > sizeof c->pal) z = sizeof c->pal;
        memcpy(c->pal, d->config + 2, z);
    }
    n = (size_t)c->stride * c->h;
    c->fb = (uint8_t *)calloc(1, n);
    if (!c->fb) { free(c); return MR_ENOMEM; }
    d->priv = c;
    d->frame.data = c->fb;
    d->frame.width = c->w; d->frame.height = c->h;
    d->frame.stride = c->stride;
    d->frame.fmt = MR_PIX_RGB24;
    return MR_OK;
}

int mr_msvideo1_set_indexed_output(mr_decoder *d, int depth)
{
    ms1_ctx *c;
    uint8_t *fb;
    size_t bytes;
    int phase, i;

    if (!d || d->codec != &mr_codec_msvideo1 || !d->priv ||
        (depth != 4 && depth != 5 && depth != 8))
        return 0;
    c = (ms1_ctx *)d->priv;
    if (c->decoded_any) return 0;

    bytes = (size_t)c->w * c->h;
    fb = (uint8_t *)calloc(bytes, 1);
    if (!fb) return 0;

    if (c->bits == 8) {
        for (phase = 0; phase < 16; phase++)
            for (i = 0; i < 256; i++) {
                const uint8_t *rgb = c->pal + i * 3;
                c->indexed_pal[phase][i] = mr_dither_rgb_indexed_pixel(
                    rgb[0], rgb[1], rgb[2], phase & 3, phase >> 2, depth);
            }
    }

    free(c->fb);
    c->fb = fb;
    c->stride = c->w;
    c->indexed_depth = depth;
    d->frame.data = c->fb;
    d->frame.stride = c->stride;
    d->frame.fmt = MR_PIX_INDEX8;
    d->frame.u_data = d->frame.v_data = NULL;
    d->frame.u_stride = d->frame.v_stride = 0;
    return 1;
}

static mr_status ms1_decode(mr_decoder *d, const uint8_t *p, uint32_t len)
{
    ms1_ctx *c = (ms1_ctx *)d->priv;
    const uint8_t *e = p + len;
    int total = c->bw * c->bh;
    int skip = 0, bx, by;
    int changed0 = c->h, changed1 = 0;

    if (!p) return MR_EFORMAT;
    c->decoded_any = 1;

    for (by = c->bh - 1; by >= 0; by--) {
        for (bx = 0; bx < c->bw; bx++) {
            unsigned cols[8], flags, a, b;
            int nc, i;

            if (skip) { skip--; total--; continue; }
            /* Running out mid-frame keeps what was decoded, as the reference
             * decoder does, rather than dropping the whole stream. */
            if (e - p < 2) goto done;
            a = *p++; b = *p++;
            if (a == 0 && b == 0 && total == 0) goto done;

            if ((b & 0xfc) == 0x84) {
                /* Skip run; the current block is the first one skipped. */
                skip = (int)(((b - 0x84) << 8) + a) - 1;
                if (skip < 0) skip = total;   /* zero-length run: skip rest */
            } else {
                if (b < 0x80) {
                    flags = (b << 8) | a;
                    nc = 2;
                    if (c->bits == 8) {
                        if (e - p < 2) goto done;
                        cols[0] = *p++; cols[1] = *p++;
                    } else {
                        if (e - p < 4) goto done;
                        cols[0] = mr_rl16(p); cols[1] = mr_rl16(p + 2);
                        p += 4;
                        if (cols[0] & 0x8000) {   /* 16-bit 8-colour flag */
                            if (e - p < 12) goto done;
                            for (i = 2; i < 8; i++, p += 2) cols[i] = mr_rl16(p);
                            nc = 8;
                        }
                    }
                } else if (c->bits == 8 && b >= 0x90) {
                    flags = (b << 8) | a;
                    nc = 8;
                    if (e - p < 8) goto done;
                    for (i = 0; i < 8; i++) cols[i] = *p++;
                } else {
                    flags = 0;
                    nc = 1;
                    cols[0] = (c->bits == 8) ? a : ((b << 8) | a);
                }
                put_block(c, bx, by, flags, cols, nc);
                if (by * 4 < changed0) changed0 = by * 4;
                if (by * 4 + 4 > changed1) changed1 = by * 4 + 4;
            }
            total--;
        }
    }
done:
    if (changed1 > c->h) changed1 = c->h;
    d->frame.dirty_y0 = changed0;
    d->frame.dirty_y1 = changed1;
    return MR_OK;
}

static void ms1_close(mr_decoder *d)
{
    ms1_ctx *c = (ms1_ctx *)d->priv;
    if (c) { free(c->fb); free(c); }
    d->priv = NULL;
}

const mr_codec mr_codec_msvideo1 = {
    "Microsoft Video 1 (MSVC)",
    { MR_FOURCC('M','S','V','C'), MR_FOURCC('m','s','v','c'),
      MR_FOURCC('C','R','A','M'), MR_FOURCC('c','r','a','m'),
      MR_FOURCC('W','H','A','M'), 0 },
    ms1_open, ms1_decode, ms1_close, NULL
};
