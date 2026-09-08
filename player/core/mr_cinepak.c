/*
 * MintVID - Cinepak (CVID) video decoder.
 *
 * Why Cinepak is the base-tier codec: it was designed to play from CD-ROM on
 * 386/68030-class CPUs. Decoding is vector-quantisation - codebook lookups and
 * block copies, no DCT and no per-pixel arithmetic beyond a YUV->RGB at output.
 * That is exactly what keeps it real-time on a bare A600/AGA while still being
 * a portable ~single-file C decoder in the MintAMP spirit.
 *
 * Format reference: Tim Ferguson's CVID description. Frames are built from
 * horizontal strips; each strip carries V1 (4x4-from-2x2) and V4 (four 2x2)
 * codebooks plus a vector map over 4x4 macroblocks. Inter frames patch the
 * persistent framebuffer in place and may selectively update codebook entries,
 * so codebooks and the output buffer are decoder state that survives frames.
 *
 * RGB24 remains the default for host validation and RTG/HAM display. Native
 * indexed AGA can opt into MR_PIX_INDEX8 before the first frame: each codebook
 * update then precomputes the exact 4x4-Bayer palette bytes its V1/V4 vectors
 * will use, and vector decode writes those bytes straight to a persistent
 * chunky framebuffer. That removes both the three-byte RGB framebuffer and
 * the full-frame RGB-to-indexed dither pass from the AGA hot path.
 */
#include "mr_codec.h"
#include "mr_cinepak.h"
#include "mr_dither.h"
#include <stdlib.h>
#include <string.h>

/* A codebook entry represents four source colours. RGB mode stores those four
 * triplets. Indexed mode instead stores the complete output tile:
 *
 *   V1: one 4x4 tile (each source colour doubled in X and Y)
 *   V4: four 2x2 variants, one for each quadrant of the containing 4x4 block
 *
 * Cinepak blocks are 4x4-aligned, so those V4 quadrants also fully determine
 * the 4x4 Bayer phase. Both representations fit in the same 16-byte union. */
typedef struct {
    union {
        uint8_t rgb[4][3];
        uint8_t indexed[16];
    } pixels;
} cvid_cb;

typedef struct {
    cvid_cb  v1[256];
    cvid_cb  v4[256];
} cvid_strip_cb;

typedef struct {
    int            width, height;
    uint8_t       *fb;          /* persistent RGB24 or INDEX8 framebuffer  */
    int            stride;
    int            indexed_depth; /* 0 => RGB24; otherwise 4, 5 or 8       */
    int            decoded_any;
    cvid_strip_cb *strips;      /* per-strip codebooks (persist for inter) */
    int            num_strip_cb;
    int            dy0, dy1;    /* changed-row span this frame             */
} cvid_ctx;

/* Cinepak YUV->RGB. u,v are signed offsets stored in the codebook bytes.
 * Matches the conventional CVID reconstruction (r=y+2v, b=y+2u). */
static inline void yuv2rgb(int y, int u, int v, uint8_t *out)
{
    int r = y + (v << 1);
    int g = y - (u >> 1) - v;
    int b = y + (u << 1);
    if (r < 0) r = 0; else if (r > 255) r = 255;
    if (g < 0) g = 0; else if (g > 255) g = 255;
    if (b < 0) b = 0; else if (b > 255) b = 255;
    out[0] = (uint8_t)r; out[1] = (uint8_t)g; out[2] = (uint8_t)b;
}

/* Build one codebook entry's four RGB pixels from raw bytes. If gray, u=v=0. */
static void cb_build(cvid_ctx *c, cvid_cb *cb, const uint8_t *d,
                     int is_color, int is_v1, int phase_y)
{
    int u = 0, v = 0;
    uint8_t rgb[4][3];
    int i;
    if (is_color) {
        u = (int8_t)d[4];
        v = (int8_t)d[5];
    }
    for (i = 0; i < 4; i++) yuv2rgb(d[i], u, v, rgb[i]);

    if (!c->indexed_depth) {
        memcpy(cb->pixels.rgb, rgb, sizeof rgb);
    } else if (is_v1) {
        int x, y;
        for (y = 0; y < 4; y++)
            for (x = 0; x < 4; x++) {
                const uint8_t *p = rgb[(y >> 1) * 2 + (x >> 1)];
                cb->pixels.indexed[y * 4 + x] =
                    mr_dither_rgb_indexed_pixel(p[0], p[1], p[2], x,
                                                phase_y + y,
                                                c->indexed_depth);
            }
    } else {
        int q, x, y;
        for (q = 0; q < 4; q++) {
            int qx = (q & 1) * 2, qy = (q >> 1) * 2;
            for (y = 0; y < 2; y++)
                for (x = 0; x < 2; x++) {
                    const uint8_t *p = rgb[y * 2 + x];
                    cb->pixels.indexed[q * 4 + y * 2 + x] =
                        mr_dither_rgb_indexed_pixel(p[0], p[1], p[2],
                                                    qx + x, phase_y + qy + y,
                                                    c->indexed_depth);
                }
        }
    }
}

/* Load/patch a codebook chunk. chunk_id low nibble selects color vs gray and
 * full vs selective update:
 *   0x20/0x22 full color (6 bytes/entry)   0x24/0x26 full gray (4 bytes/entry)
 *   0x21/0x23 sel  color                   0x25/0x27 sel  gray
 * Selective updates carry 32-bit MSB-first flag words; a set bit means the
 * corresponding entry is present. */
static void load_codebook(cvid_ctx *c, cvid_cb *cb, int chunk_id, int is_v1,
                          int phase_y, const uint8_t *data, uint32_t size)
{
    int is_color = !(chunk_id & 0x0400);
    int selective = (chunk_id & 0x0100);
    int entry_sz = is_color ? 6 : 4;
    const uint8_t *p = data;
    const uint8_t *end = data + size;
    int i;

    if (!selective) {
        for (i = 0; i < 256 && p + entry_sz <= end; i++, p += entry_sz)
            cb_build(c, &cb[i], p, is_color, is_v1, phase_y);
        return;
    }

    i = 0;
    while (i < 256 && p + 4 <= end) {
        uint32_t flags = mr_rb32(p);
        p += 4;
        int bit;
        for (bit = 31; bit >= 0 && i < 256; bit--, i++) {
            if (flags & (1u << bit)) {
                if (p + entry_sz > end) return;
                cb_build(c, &cb[i], p, is_color, is_v1, phase_y);
                p += entry_sz;
            }
        }
    }
}

/* MSB-first bit reader over a chunk's vector-flag stream. */
typedef struct { const uint8_t *p, *end; uint32_t acc; int bits; } bitrdr;

static void br_init(bitrdr *b, const uint8_t *p, const uint8_t *end)
{ b->p = p; b->end = end; b->acc = 0; b->bits = 0; }

static int br_bit(bitrdr *b)
{
    if (b->bits == 0) {
        uint32_t w = 0; int i;
        for (i = 0; i < 4; i++) w = (w << 8) | (b->p < b->end ? *b->p++ : 0);
        b->acc = w; b->bits = 32;
    }
    b->bits--;
    return (int)((b->acc >> b->bits) & 1);
}

/* Copy a codebook entry's 2x2 RGB pixels, upscaled by `scale` (1 => a 2x2
 * pixel target for V4 sub-blocks, 2 => a 4x4 target for V1). */
static void put_vector(cvid_ctx *c, const cvid_cb *cb, int x, int y, int scale,
                       int variant)
{
    if (c->indexed_depth) {
        if (scale == 2) {
            int py;
            if (x >= 0 && y >= 0 && x + 4 <= c->width &&
                y + 4 <= c->height) {
                const uint8_t *src = cb->pixels.indexed;
                uint8_t *dst = c->fb + (size_t)y * c->stride + x;
                memcpy(dst, src, 4); dst += c->stride; src += 4;
                memcpy(dst, src, 4); dst += c->stride; src += 4;
                memcpy(dst, src, 4); dst += c->stride; src += 4;
                memcpy(dst, src, 4);
                return;
            }
            for (py = 0; py < 4 && y + py < c->height; py++) {
                int count = c->width - x;
                if (count > 4) count = 4;
                if (count > 0)
                    memcpy(c->fb + (size_t)(y + py) * c->stride + x,
                           cb->pixels.indexed + py * 4, (size_t)count);
            }
        } else {
            int q = variant & 3;
            int py;
            if (x >= 0 && y >= 0 && x + 2 <= c->width &&
                y + 2 <= c->height) {
                const uint8_t *src = cb->pixels.indexed + q * 4;
                uint8_t *dst = c->fb + (size_t)y * c->stride + x;
                dst[0] = src[0]; dst[1] = src[1];
                dst += c->stride;
                dst[0] = src[2]; dst[1] = src[3];
                return;
            }
            for (py = 0; py < 2 && y + py < c->height; py++) {
                int count = c->width - x;
                if (count > 2) count = 2;
                if (count > 0)
                    memcpy(c->fb + (size_t)(y + py) * c->stride + x,
                           cb->pixels.indexed + q * 4 + py * 2,
                           (size_t)count);
            }
        }
        return;
    }

    int sy, sx;
    for (sy = 0; sy < 2; sy++) {
        for (sx = 0; sx < 2; sx++) {
            const uint8_t *rgb = cb->pixels.rgb[sy * 2 + sx];
            int py, px;
            for (py = 0; py < scale; py++) {
                int oy = y + sy * scale + py;
                if (oy >= c->height) continue;
                uint8_t *row = c->fb + (size_t)oy * c->stride;
                for (px = 0; px < scale; px++) {
                    int ox = x + sx * scale + px;
                    if (ox >= c->width) continue;
                    uint8_t *o = row + ox * 3;
                    o[0] = rgb[0]; o[1] = rgb[1]; o[2] = rgb[2];
                }
            }
        }
    }
}

/* Decode the vector map of one strip within rows [y0,y1). Low nibble of the
 * chunk id: bit0 => inter (per-MB "coded" flag, 0 = skip/keep previous frame),
 * bit1 => V4-only (no per-MB type flag, every coded MB is V4). The "coded" and
 * "type" flags share one MSB-first 32-bit reservoir that is refilled from the
 * same byte stream the vector index bytes are read from - so a single cursor
 * feeds both. */
static void decode_vectors(cvid_ctx *c, cvid_strip_cb *cb, int chunk_id,
                           int x0, int y0, int x1, int y1,
                           const uint8_t *data, uint32_t size)
{
    const uint8_t *end = data + size;
    bitrdr br;
    int x, y;
    int inter  = (chunk_id & 0x0100) != 0;
    int v4only = (chunk_id & 0x0200) != 0;

    br_init(&br, data, end);

    for (y = y0; y < y1; y += 4) {
        for (x = x0; x < x1; x += 4) {
            int is_v4;
            if (inter && !br_bit(&br))
                continue;                    /* skipped MB: keep prev frame */

            if (y < c->dy0) c->dy0 = y;      /* this MB row changes         */
            if (y + 4 > c->dy1) c->dy1 = y + 4;

            is_v4 = v4only ? 1 : br_bit(&br);

            if (is_v4) {
                int q;                        /* four 2x2 sub-blocks         */
                for (q = 0; q < 4; q++) {
                    if (br.p >= end) return;
                    put_vector(c, &cb->v4[*br.p++],
                               x + (q & 1) * 2, y + (q >> 1) * 2, 1, q);
                }
            } else {
                if (br.p >= end) return;
                put_vector(c, &cb->v1[*br.p++], x, y, 2, 0);
            }
        }
    }
}

static mr_status cvid_open(mr_decoder *dec)
{
    cvid_ctx *c = (cvid_ctx *)calloc(1, sizeof(cvid_ctx));
    if (!c) return MR_ENOMEM;
    c->width  = dec->width;
    c->height = dec->height;
    c->stride = dec->width * 3;
    c->fb = (uint8_t *)calloc((size_t)c->stride * c->height, 1);
    if (!c->fb) { free(c); return MR_ENOMEM; }
    dec->priv = c;
    dec->frame.width  = c->width;
    dec->frame.height = c->height;
    dec->frame.fmt    = MR_PIX_RGB24;
    dec->frame.stride = c->stride;
    dec->frame.data   = c->fb;
    dec->frame.dirty_y0 = 0;
    dec->frame.dirty_y1 = c->height;
    return MR_OK;
}

int mr_cinepak_set_indexed_output(mr_decoder *dec, int depth)
{
    cvid_ctx *c;
    uint8_t *fb;
    size_t bytes;
    if (!dec || dec->codec != &mr_codec_cinepak || !dec->priv ||
        (depth != 4 && depth != 5 && depth != 8))
        return 0;
    c = (cvid_ctx *)dec->priv;
    if (c->decoded_any || c->num_strip_cb) return 0;
    bytes = (size_t)c->width * (size_t)c->height;
    fb = (uint8_t *)calloc(bytes, 1);
    if (!fb) return 0;
    free(c->fb);
    c->fb = fb;
    c->stride = c->width;
    c->indexed_depth = depth;
    dec->frame.fmt = MR_PIX_INDEX8;
    dec->frame.stride = c->stride;
    dec->frame.data = c->fb;
    dec->frame.u_data = dec->frame.v_data = NULL;
    dec->frame.u_stride = dec->frame.v_stride = 0;
    return 1;
}

static cvid_strip_cb *ensure_strip_cb(cvid_ctx *c, int n)
{
    if (n >= c->num_strip_cb) {
        int want = n + 1;
        cvid_strip_cb *ns = (cvid_strip_cb *)realloc(c->strips,
                                (size_t)want * sizeof(cvid_strip_cb));
        if (!ns) return NULL;
        memset(ns + c->num_strip_cb, 0,
               (size_t)(want - c->num_strip_cb) * sizeof(cvid_strip_cb));
        c->strips = ns;
        c->num_strip_cb = want;
    }
    return &c->strips[n];
}

static mr_status cvid_decode(mr_decoder *dec, const uint8_t *data, uint32_t len)
{
    cvid_ctx *c = (cvid_ctx *)dec->priv;
    if (len < 10) return MR_EFORMAT;

    c->decoded_any = 1;

    c->dy0 = c->height; c->dy1 = 0;        /* empty until an MB is coded    */

    uint16_t num_strips = mr_rb16(data + 8);
    const uint8_t *p   = data + 10;
    const uint8_t *end = data + len;
    int strip_i;
    int y_top = 0;

    for (strip_i = 0; strip_i < num_strips && p + 12 <= end; strip_i++) {
        uint16_t sid  = mr_rb16(p);
        uint32_t ssz  = mr_rb16(p + 2);
        int r_y0 = mr_rb16(p + 4);
        int r_y1 = mr_rb16(p + 8);
        int strip_h = r_y1 - r_y0;         /* stored coords are per-strip   */
        const uint8_t *sp  = p + 12;
        const uint8_t *send = p + ssz;
        if (send > end) send = end;
        if (strip_h <= 0) strip_h = c->height - y_top;

        int y0 = y_top;
        int y1 = y_top + strip_h;
        if (y1 > c->height) y1 = c->height;

        cvid_strip_cb *cb = ensure_strip_cb(c, strip_i);
        if (!cb) return MR_ENOMEM;
        /* An intra strip (0x1000) starts from fresh codebooks; an inter strip
         * (0x1100) inherits the previous frame's codebooks for this strip. */
        if (sid == 0x1000)
            memset(cb, 0, sizeof(*cb));

        while (sp + 4 <= send) {
            uint16_t cid = mr_rb16(sp);
            uint32_t csz = mr_rb16(sp + 2);
            const uint8_t *cdata = sp + 4;
            uint32_t cbody = (csz >= 4) ? csz - 4 : 0;
            if (cdata + cbody > send) cbody = (uint32_t)(send - cdata);

            if ((cid & 0xf000) == 0x2000) {
                /* Codebook. Bit 0x0200 selects V1 vs V4; load_codebook reads
                 * bit 0x0400 (gray) and 0x0100 (selective) itself. */
                int is_v1 = (cid & 0x0200) != 0;
                cvid_cb *tbl = is_v1 ? cb->v1 : cb->v4;
                load_codebook(c, tbl, cid, is_v1, y0 & 3, cdata, cbody);
            } else if ((cid & 0xf000) == 0x3000) {
                /* Vectors: 0x0100 inter, 0x0200 V4-only. */
                decode_vectors(c, cb, cid, 0, y0, c->width, y1,
                               cdata, cbody);
            }   /* else: unknown chunk, skip */

            csz += (csz & 1);
            if (csz < 4) break;
            sp += csz;
        }

        y_top = y1;
        ssz += (ssz & 1);
        if (ssz < 12) break;
        p += ssz;
    }

    if (c->dy1 > c->height) c->dy1 = c->height;
    dec->frame.data     = c->fb;
    dec->frame.dirty_y0 = c->dy0;
    dec->frame.dirty_y1 = c->dy1;
    return MR_OK;
}

static void cvid_close(mr_decoder *dec)
{
    cvid_ctx *c = (cvid_ctx *)dec->priv;
    if (!c) return;
    free(c->fb);
    free(c->strips);
    free(c);
    dec->priv = NULL;
}

const mr_codec mr_codec_cinepak = {
    "cinepak",
    { MR_FOURCC('c','v','i','d'), MR_FOURCC('C','V','I','D'), 0, 0 },
    cvid_open,
    cvid_decode,
    cvid_close,
    NULL,                    /* no reordering */
};
