/*
 * MintVID - native ITU-T H.263 / H.263+ decoder.
 *
 * The picture layer understands both the H.263 version 1 header and the
 * version 2 extended PTYPE (PLUSPTYPE), including custom picture sizes, the
 * custom picture clock frequency, the per-picture rounding type, unrestricted
 * motion vectors (Annex D) and slice-structured mode (Annex K).  Annex tools
 * that would change reconstruction - SAC, advanced prediction, advanced intra
 * coding, the deblocking filter, PB/B pictures, modified quantisation and the
 * alternative inter VLC - are still refused before any macroblock is decoded,
 * so an unsupported stream stops rather than producing corrupt output.
 */
#include "mr_h263.h"
#include "mr_yuv.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- bit reader (MSB first) ------------------------------------------ */
typedef struct {
    const uint8_t *buf;
    int len;
    int pos;
} bitreader;

static void br_init(bitreader *b, const uint8_t *data, int len)
{
    b->buf = data;
    b->len = len;
    b->pos = 0;
}

static unsigned br_bit(bitreader *b)
{
    int byte = b->pos >> 3;
    int shift = 7 - (b->pos & 7);
    b->pos++;
    if (byte >= b->len)
        return 0;
    return (b->buf[byte] >> shift) & 1u;
}

static unsigned br_bits(bitreader *b, int n)
{
    unsigned v = 0;
    while (n-- > 0)
        v = (v << 1) | br_bit(b);
    return v;
}

static unsigned br_peek(bitreader *b, int n)
{
    int save = b->pos;
    unsigned v = br_bits(b, n);
    b->pos = save;
    return v;
}

static void br_skip(bitreader *b, int n)
{
    b->pos += n;
}

static int br_overrun(const bitreader *b)
{
    return b->pos > b->len * 8;
}

static int br_left(const bitreader *b)
{
    return b->len * 8 - b->pos;
}

/* ---- VLC table types ------------------------------------------------- */
typedef struct {
    uint16_t code;
    uint8_t len, last, run;
    int16_t level;
} tcoef_t;
typedef struct { uint16_t code; uint8_t len, val; } vlc3_t;
typedef struct { uint16_t code; uint8_t len, mbtype, cbpc; } mcbpc_t;
typedef struct { uint16_t code; uint8_t len, intra, inter; } cbpy_t;
typedef struct { uint16_t code; uint8_t len; int16_t data; } mvd_t;

#include "mr_mpeg4_tables.inc"

/* grid[v][u] gives the serial coefficient position for that matrix cell. */
static const uint8_t scan_zigzag[8][8] = {
    { 0, 1, 5, 6,14,15,27,28}, { 2, 4, 7,13,16,26,29,42},
    { 3, 8,12,17,25,30,41,43}, { 9,11,18,24,31,40,44,53},
    {10,19,23,32,39,45,52,54}, {20,22,33,38,46,51,55,60},
    {21,34,37,47,50,56,59,61}, {35,36,48,49,57,58,62,63}
};

/* One 16x16 motion vector per macroblock.  Intra and skipped macroblocks
 * contribute a zero vector, exactly as they do in the reference decoder. */
typedef struct {
    int16_t x, y;
} mvblk;

/* Picture-layer state that survives a PLUSPTYPE with UFEP == 0: such a
 * picture repeats only MPPTYPE and inherits the annex flags of the last
 * picture that carried a full OPPTYPE. */
typedef struct {
    int w, h, mb_w, mb_h, cw, ch;
    int ystride, cstride;
    int gob_index;                   /* macroblock rows per GOB             */
    int have_ref;                    /* a reference picture was decoded     */
    int custom_pcf;                  /* custom picture clock frequency      */
    int umv;                         /* Annex D, H.263+ signalling          */
    int long_vectors;                /* Annex D, H.263 version 1 signalling */
    int slice_structured;            /* Annex K                             */
    uint8_t *cur[3], *ref[3];
    uint8_t *rgb;
    mvblk *mv;
} h263_ctx;

/* Decoded picture header.  Everything here is per-picture. */
typedef struct {
    int pict_type;                   /* 0 intra, 1 inter                    */
    int quant;
    int no_rounding;                 /* RTYPE: 1 selects rounding type 1    */
    int mb_start;                    /* first macroblock of the picture     */
} h263_picture;

#define MV_ERROR 0xffff

static int h263_debug(void)
{
    static int enabled = -1;
    if (enabled < 0)
        enabled = getenv("MRDBG") != NULL;
    return enabled;
}

/* ---- unsupported-feature reporting ----------------------------------- */
enum {
    FEAT_SAC, FEAT_AP, FEAT_PB, FEAT_AIC, FEAT_DF, FEAT_RPS, FEAT_ISD,
    FEAT_AIV, FEAT_MQ, FEAT_BPIC, FEAT_CPM, FEAT_RESAMPLE, FEAT_RECT_SLICE,
    FEAT_ASO, FEAT_4MV, FEAT_COUNT
};

static const char *const feature_names[FEAT_COUNT] = {
    "SAC", "advanced prediction", "PB frames", "advanced intra coding",
    "deblocking filter", "reference picture selection",
    "independent segment decoding", "alternative inter VLC",
    "modified quantisation", "B pictures", "continuous presence multipoint",
    "reference picture resampling", "rectangular slices",
    "arbitrary slice ordering", "four motion vectors"
};

static int unsupported(int feature)
{
    static unsigned reported;
    if (!(reported & (1u << feature))) {
        reported |= 1u << feature;
        fprintf(stderr, "H.263: unsupported feature %s\n",
                feature_names[feature]);
    }
    return -2;
}

/* ---- generic VLC helpers --------------------------------------------- */
static int match_cbpy(bitreader *b)
{
    unsigned w = br_peek(b, 6);
    int i;
    for (i = 0; i < 16; i++) {
        if ((w >> (6 - cbpy_tab[i].len)) == cbpy_tab[i].code) {
            br_skip(b, cbpy_tab[i].len);
            return cbpy_tab[i].intra;
        }
    }
    return -1;
}

static int match_mcbpc(bitreader *b, int intra, int *type, int *cbpc)
{
    const mcbpc_t *tab = intra ? mcbpc_i : mcbpc_p;
    int count = intra ? 9 : 21, i;
    unsigned w = br_peek(b, 9);
    for (i = 0; i < count; i++) {
        if ((w >> (9 - tab[i].len)) == tab[i].code) {
            br_skip(b, tab[i].len);
            *type = tab[i].mbtype;
            *cbpc = tab[i].cbpc;
            return 0;
        }
    }
    return -1;
}

static int match_mvd(bitreader *b)
{
    unsigned w = br_peek(b, 13);
    int i;
    for (i = 0; i < 65; i++) {
        if ((w >> (13 - mvd_tab[i].len)) == mvd_tab[i].code) {
            br_skip(b, mvd_tab[i].len);
            return mvd_tab[i].data;
        }
    }
    return 999;
}

/* The run/level VLC is decoded 12 bits at a time.  A flat 4096-entry prefix
 * table maps that window straight to the matching table index, replacing the
 * per-coefficient linear scan over 102 entries (the single most-called routine
 * in the decoder).  It is byte-for-byte equivalent: each entry covers exactly
 * the prefix range the linear "(w >> (12-len)) == code" test accepted, and the
 * ranges are filled in reverse so the lowest index still wins on any overlap. */
static int16_t tcoef_lut[4096];
static int tcoef_lut_ready;

static void build_tcoef_lut(void)
{
    int i, j;
    for (j = 0; j < 4096; j++)
        tcoef_lut[j] = -1;
    for (i = 101; i >= 0; i--) {
        unsigned base = (unsigned)tcoef_inter[i].code << (12 - tcoef_inter[i].len);
        unsigned span = 1u << (12 - tcoef_inter[i].len), k;
        for (k = 0; k < span; k++)
            tcoef_lut[base + k] = (int16_t)i;
    }
}

/* H.263 codes both intra AC and inter coefficients with the one TCOEF table;
 * the separate intra table belongs to MPEG-4 and to Annex I, which is
 * refused in the picture header. */
static const tcoef_t *match_tcoef(bitreader *b)
{
    unsigned w = br_peek(b, 12);
    int i;
    if (!tcoef_lut_ready) {
        build_tcoef_lut();
        tcoef_lut_ready = 1;
    }
    i = tcoef_lut[w];
    return i < 0 ? NULL : &tcoef_inter[i];
}

static int decode_plain_rl(bitreader *b, int *last, int *run, int *level)
{
    const tcoef_t *e = match_tcoef(b);
    if (!e)
        return -1;
    br_skip(b, e->len);
    *last = e->last;
    *run = e->run;
    *level = e->level;
    if (br_bit(b))
        *level = -*level;
    return br_overrun(b) ? -1 : 0;
}

/* H.263 has a single escape: the 7-bit ESCAPE code is followed by a plain
 * LAST/RUN/LEVEL triple.  (MPEG-4 and the Microsoft variants instead spend
 * further bits selecting one of three escape forms - decoding those here cost
 * two extra bits per escape and desynchronised the bitstream.)  LEVEL -128 is
 * the Annex T 11-bit level extension. */
static int decode_rl_event(bitreader *b, int *last, int *run, int *level)
{
    int v;

    if (br_peek(b, 7) != 0x03)
        return decode_plain_rl(b, last, run, level);

    br_skip(b, 7);                      /* ESCAPE: 0000011 */
    *last = (int)br_bit(b);
    *run = (int)br_bits(b, 6);
    v = (int)br_bits(b, 8);
    if (v & 0x80)
        v -= 256;
    if (v == -128) {
        unsigned lo = br_bits(b, 5);
        int hi = (int)br_bits(b, 6);
        if (hi & 0x20)
            hi -= 64;
        v = (int)lo | (hi << 5);
    }
    *level = v;
    return br_overrun(b) ? -1 : 0;
}

/* ---- transform and pixel helpers ------------------------------------- */
/* Separable integer IDCT.  The original transform used a double-precision
 * cosine sum, which is fine on the dev host but pins the m68k target on its
 * (optional, slow) FPU for the single hottest routine in the decoder.  This
 * replacement uses a scaled-integer cosine matrix -- no floating point in the
 * per-block path at all -- and sparse fast paths that skip the many all-zero
 * rows typical of coded blocks.  The rounding matches the float reference to
 * within +/-1 on the conformance fixture, which itself sits well inside the
 * ffmpeg tolerance. */
#define IDCT_P 13                        /* cosine-matrix fractional bits    */
#define IDCT_SHIFT (2 * IDCT_P + 2)      /* two passes + the 1/4 IDCT norm    */
/* Keeping this table in the binary avoids floating-point cos() setup and
 * makes the complete decoder path integer-only. */
static const int32_t idct_tab[8][8] = {
    {5793, 5793, 5793, 5793, 5793, 5793, 5793, 5793},
    {8035, 6811, 4551, 1598,-1598,-4551,-6811,-8035},
    {7568, 3135,-3135,-7568,-7568,-3135, 3135, 7568},
    {6811,-1598,-8035,-4551, 4551, 8035, 1598,-6811},
    {5793,-5793,-5793, 5793, 5793,-5793,-5793, 5793},
    {4551,-8035, 1598, 6811,-6811,-1598, 8035,-4551},
    {3135,-7568, 7568,-3135,-3135, 7568,-7568, 3135},
    {1598,-4551, 6811,-8035, 8035,-6811, 4551,-1598}
};

/* Round acc / 2^IDCT_SHIFT half-away-from-zero, matching the old float path. */
static int idct_round(int64_t acc)
{
    int64_t half = (int64_t)1 << (IDCT_SHIFT - 1);
    return acc >= 0 ? (int)((acc + half) >> IDCT_SHIFT)
                    : -(int)((-acc + half) >> IDCT_SHIFT);
}

static void idct_8x8(const int in[8][8], int out[8][8])
{
    int32_t tmp[8][8];
    int rowmask = 0;                     /* bit v set => tmp row v nonzero    */
    int u, x, v, y;

    /* Pass 1: horizontal transform of each nonzero input row. */
    for (v = 0; v < 8; v++) {
        const int *ir = in[v];
        if (!(ir[0] | ir[1] | ir[2] | ir[3] |
              ir[4] | ir[5] | ir[6] | ir[7]))
            continue;                    /* tmp row stays implicitly zero     */
        rowmask |= 1 << v;
        for (x = 0; x < 8; x++) {
            int32_t a = 0;
            for (u = 0; u < 8; u++)
                a += ir[u] * idct_tab[u][x];
            tmp[v][x] = a;
        }
    }

    if (rowmask == 0) {                  /* wholly zero block                 */
        for (y = 0; y < 8; y++)
            for (x = 0; x < 8; x++)
                out[y][x] = 0;
        return;
    }
    if (rowmask == 1) {                  /* only row 0 (DC / horizontal) set  */
        for (x = 0; x < 8; x++)
            for (y = 0; y < 8; y++)
                out[y][x] = idct_round((int64_t)tmp[0][x] * idct_tab[0][y]);
        return;
    }

    /* Pass 2: vertical transform, accumulating over nonzero rows only. */
    for (x = 0; x < 8; x++) {
        for (y = 0; y < 8; y++) {
            int64_t a = 0;
            for (v = 0; v < 8; v++)
                if (rowmask & (1 << v))
                    a += (int64_t)tmp[v][x] * idct_tab[v][y];
            out[y][x] = idct_round(a);
        }
    }
}

static int clip8(int v)
{
    if (v < 0) return 0;
    if (v > 255) return 255;
    return v;
}

static int median3(int a, int b, int c)
{
    int hi = a > b ? a : b;
    int lo = a < b ? a : b;
    if (c > hi) return hi;
    if (c < lo) return lo;
    return c;
}

static int floor_div(int n, int d)
{
    int q = n / d;
    int r = n % d;
    if (r < 0)
        q--;
    return q;
}

static int fetch_px(const uint8_t *p, int w, int h, int stride, int x, int y)
{
    if (x < 0) x = 0; else if (x >= w) x = w - 1;
    if (y < 0) y = 0; else if (y >= h) y = h - 1;
    return p[(size_t)y * stride + x];
}

/* rnd is 1 for rounding type 0 (the only type H.263 version 1 has) and 0 for
 * H.263+ rounding type 1, which drops the +1/2 bias from half-pixel
 * interpolation.  Encoders alternate the two between P pictures, so getting
 * this wrong drifts a little further on every inter frame. */
static void mc_block(const uint8_t *ref, int w, int h, int stride,
                     int px, int py, int mvx, int mvy, int rnd, int out[8][8])
{
    int ix = floor_div(mvx, 2), iy = floor_div(mvy, 2);
    int hx = mvx - ix * 2, hy = mvy - iy * 2;
    int bx = px + ix, by = py + iy;
    int y, x;

    /* Fast path: the whole 8x8 fetch window (plus the +1 half-pel neighbour)
     * lands inside the frame, so no per-pixel edge clamp is needed. */
    if (bx >= 0 && by >= 0 &&
        bx + 8 + (hx ? 1 : 0) <= w && by + 8 + (hy ? 1 : 0) <= h) {
        const uint8_t *base = ref + (size_t)by * stride + bx;
        for (y = 0; y < 8; y++) {
            const uint8_t *r = base + (size_t)y * stride;
            if (!hx && !hy)
                for (x = 0; x < 8; x++) out[y][x] = r[x];
            else if (hx && !hy)
                for (x = 0; x < 8; x++) out[y][x] = (r[x] + r[x + 1] + rnd) >> 1;
            else if (!hx && hy)
                for (x = 0; x < 8; x++)
                    out[y][x] = (r[x] + r[x + stride] + rnd) >> 1;
            else
                for (x = 0; x < 8; x++)
                    out[y][x] = (r[x] + r[x + 1] +
                                 r[x + stride] + r[x + stride + 1] + 1 + rnd) >> 2;
        }
        return;
    }

    for (y = 0; y < 8; y++) {
        for (x = 0; x < 8; x++) {
            int sx = px + x + ix, sy = py + y + iy;
            int a = fetch_px(ref, w, h, stride, sx, sy);
            int b = fetch_px(ref, w, h, stride, sx + 1, sy);
            int c = fetch_px(ref, w, h, stride, sx, sy + 1);
            int d = fetch_px(ref, w, h, stride, sx + 1, sy + 1);
            if (!hx && !hy) out[y][x] = a;
            else if (hx && !hy) out[y][x] = (a + b + rnd) >> 1;
            else if (!hx && hy) out[y][x] = (a + c + rnd) >> 1;
            else out[y][x] = (a + b + c + d + 1 + rnd) >> 2;
        }
    }
}

/* H.263 chroma MV reduction for one 16x16 luma vector. */
static int chroma_mv(int mv)
{
    static const uint8_t roundtab[4] = { 0, 1, 1, 1 };
    int q = floor_div(mv, 4);
    int r = mv - q * 4;
    return q * 2 + roundtab[r];
}

/* ---- block decode/reconstruction ------------------------------------ */
static int dequant_ac(int level, int q)
{
    int qadd = (q - 1) | 1;
    if (level > 0) return level * (q << 1) + qadd;
    if (level < 0) return level * (q << 1) - qadd;
    return 0;
}

static int decode_intra_block(h263_ctx *c, bitreader *b, int block,
                              int mbx, int mby, int coded, int q)
{
    int serial[64], coeff[8][8], spatial[8][8];
    int dc, pos = 1, u, v;
    uint8_t *plane;
    int stride, px, py;

    /* INTRADC is a fixed-length code whose reconstruction level is eight
     * times its value, except for 11111111, which stands for 1024. */
    dc = (int)br_bits(b, 8);
    if (br_overrun(b)) return -1;
    if (dc == 255) dc = 128;
    memset(serial, 0, sizeof serial);
    serial[0] = dc * 8;
    if (coded) {
        for (;;) {
            int last, run, level, target;
            if (decode_rl_event(b, &last, &run, &level)) return -1;
            target = pos + run;
            if (target >= 64) return -1;
            serial[target] = dequant_ac(level, q);
            pos = target + 1;
            if (last) break;
        }
    }
    for (v = 0; v < 8; v++)
        for (u = 0; u < 8; u++)
            coeff[v][u] = serial[scan_zigzag[v][u]];
    idct_8x8(coeff, spatial);
    if (block < 4) {
        plane=c->cur[0]; stride=c->ystride;
        px=mbx*16+(block&1)*8; py=mby*16+(block>>1)*8;
    } else {
        plane=c->cur[block-3]; stride=c->cstride;
        px=mbx*8; py=mby*8;
    }
    for (v=0; v<8; v++) {
        uint8_t *dst=plane+(size_t)(py+v)*stride+px;
        for (u=0; u<8; u++) dst[u]=(uint8_t)clip8(spatial[v][u]);
    }
    return 0;
}

static int decode_inter_residual(bitreader *b, int q, int out[8][8])
{
    int serial[64], coeff[8][8], pos = 0, u, v, event = 0;
    memset(serial, 0, sizeof serial);
    for (;;) {
        int last, run, level, target;
        int event_pos = b->pos;
        if (decode_rl_event(b, &last, &run, &level)) {
            if (h263_debug())
                fprintf(stderr, "[h263] bad RL event %d at bit %d peek=%06lx\n",
                        event, event_pos,
                        (unsigned long)br_peek(b, 24));
            return -1;
        }
        target = pos + run;
        if (target >= 64) {
            /* A final coefficient one slot past 63 terminates the block in
             * some encoders.  FFmpeg's non-strict path drops that coefficient
             * and accepts the block. */
            if (last)
                break;
            if (h263_debug())
                fprintf(stderr, "[h263] RL overflow event %d pos=%d run=%d "
                        "last=%d level=%d startbit=%d\n",
                        event, pos, run, last, level, event_pos);
            return -1;
        }
        serial[target] = dequant_ac(level, q);
        pos = target + 1;
        event++;
        if (last)
            break;
    }
    for (v = 0; v < 8; v++)
        for (u = 0; u < 8; u++)
            coeff[v][u] = serial[scan_zigzag[v][u]];
    idct_8x8(coeff, out);
    return 0;
}

/* Candidate predictors are the vectors of the macroblocks to the left, above
 * and above-right; anything outside the picture, and anything above the first
 * line of the current slice, counts as zero.  On that first line the median
 * collapses to the left-hand candidate alone. */
static void motion_predict(const h263_ctx *c, int mbx, int mby,
                           int first_line, int resync_x, int *px, int *py)
{
    const mvblk *l = mbx > 0 ? &c->mv[mby * c->mb_w + mbx - 1] : NULL;

    if (first_line) {
        if (mbx == resync_x) {
            *px = *py = 0;
        } else {
            *px = l ? l->x : 0;
            *py = l ? l->y : 0;
        }
        return;
    }
    {
        const mvblk *t = &c->mv[(mby - 1) * c->mb_w + mbx];
        const mvblk *tr = mbx + 1 < c->mb_w
                        ? &c->mv[(mby - 1) * c->mb_w + mbx + 1] : NULL;
        *px = median3(l ? l->x : 0, t->x, tr ? tr->x : 0);
        *py = median3(l ? l->y : 0, t->y, tr ? tr->y : 0);
    }
}

/* Annex D reversible VLC, used when unrestricted motion vectors are on. */
static int decode_umotion(bitreader *b, int pred)
{
    int code, sign;

    if (br_bit(b))
        return pred;                     /* difference is zero */
    code = 2 + (int)br_bit(b);
    while (br_bit(b)) {
        code = (code << 1) + (int)br_bit(b);
        if (code >= 32768 || br_overrun(b))
            return MV_ERROR;
    }
    sign = code & 1;
    code >>= 1;
    return sign ? pred - code : pred + code;
}

static int decode_motion(bitreader *b, int pred, const h263_ctx *c)
{
    int diff, val;

    if (c->umv)
        return decode_umotion(b, pred);
    diff = match_mvd(b);
    if (diff == 999)
        return MV_ERROR;
    val = pred + diff;
    if (!c->long_vectors) {
        val = ((val + 32) & 63) - 32;    /* modulo the +-32 half-pel range */
    } else {
        if (pred < -31 && val < -63) val += 64;
        if (pred > 32 && val > 63) val -= 64;
    }
    return val;
}

static int reconstruct_inter_mb(h263_ctx *c, bitreader *b, int mbx, int mby,
                                int cbp, int q, int mvx, int mvy, int rnd)
{
    int block;
    for (block = 0; block < 6; block++) {
        int chroma = block >= 4;
        int plane_no = chroma ? block - 3 : 0;
        uint8_t *plane = c->cur[plane_no];
        const uint8_t *ref = c->ref[plane_no];
        int stride = chroma ? c->cstride : c->ystride;
        int rw = chroma ? c->cw / 2 : c->cw;
        int rh = chroma ? c->ch / 2 : c->ch;
        int px = chroma ? mbx * 8 : mbx * 16 + (block & 1) * 8;
        int py = chroma ? mby * 8 : mby * 16 + (block >> 1) * 8;
        int bx = chroma ? chroma_mv(mvx) : mvx;
        int by = chroma ? chroma_mv(mvy) : mvy;
        int pred[8][8], residual[8][8];
        int y, x, coded = (cbp >> (5 - block)) & 1;

        mc_block(ref, rw, rh, stride, px, py, bx, by, rnd, pred);
        if (coded && decode_inter_residual(b, q, residual)) {
            if (h263_debug())
                fprintf(stderr, "[h263] residual block %d failed at bit %d/%d\n",
                        block, b->pos, b->len * 8);
            return -1;
        }
        for (y = 0; y < 8; y++) {
            uint8_t *dst = plane + (size_t)(py + y) * stride + px;
            for (x = 0; x < 8; x++)
                dst[x] = (uint8_t)clip8(pred[y][x] +
                                        (coded ? residual[y][x] : 0));
        }
    }
    return 0;
}

/* ---- picture and slice headers --------------------------------------- */
static const uint16_t std_format[8][2] = {
    {    0,    0 }, {  128,   96 }, {  176,  144 }, {  352,  288 },
    {  704,  576 }, { 1408, 1152 }, {    0,    0 }, {    0,    0 }
};

/* Slice-structured mode addresses a macroblock directly; the field width
 * follows from the number of macroblocks in the picture. */
static int decode_mba(const h263_ctx *c, bitreader *b)
{
    static const uint16_t mba_max[6] = { 47, 98, 395, 1583, 6335, 9215 };
    static const uint8_t mba_length[7] = { 6, 7, 9, 11, 13, 14, 14 };
    int mb_num = c->mb_w * c->mb_h, i;

    for (i = 0; i < 6; i++)
        if (mb_num - 1 <= (int)mba_max[i])
            break;
    return (int)br_bits(b, mba_length[i]);
}

static int parse_picture_header(h263_ctx *c, bitreader *b, h263_picture *p)
{
    int format, width = 0, height = 0, ufep = 0;

    p->pict_type = 0;
    p->quant = 0;
    p->no_rounding = 0;
    p->mb_start = 0;

    if (br_bits(b, 22) != 0x20) return -1;         /* picture start code */
    br_skip(b, 8);                                 /* temporal reference */
    if (br_bit(b) != 1) return -1;                 /* PTYPE marker       */
    if (br_bit(b) != 0) return -1;                 /* H.263 id           */
    br_skip(b, 3);            /* split screen, document camera, freeze   */
    format = (int)br_bits(b, 3);
    if (br_overrun(b)) return -1;

    if (format != 7) {
        /* H.263 version 1: the whole picture type sits in PTYPE. */
        if (format < 1 || format > 5) return -1;
        width = std_format[format][0];
        height = std_format[format][1];
        c->custom_pcf = c->umv = c->slice_structured = 0;
        p->pict_type = (int)br_bit(b);
        c->long_vectors = (int)br_bit(b);
        if (br_bit(b)) return unsupported(FEAT_SAC);
        if (br_bit(b)) return unsupported(FEAT_AP);
        if (br_bit(b)) return unsupported(FEAT_PB);
        p->quant = (int)br_bits(b, 5);
        if (br_bit(b)) return unsupported(FEAT_CPM);
    } else {
        /* H.263 version 2: PLUSPTYPE.  UFEP 0 repeats only MPPTYPE and
         * inherits the annex flags and the picture size already in force. */
        c->long_vectors = 0;
        ufep = (int)br_bits(b, 3);
        if (ufep == 1) {
            format = (int)br_bits(b, 3);
            c->custom_pcf = (int)br_bit(b);
            c->umv = (int)br_bit(b);
            if (br_bit(b)) return unsupported(FEAT_SAC);
            if (br_bit(b)) return unsupported(FEAT_AP);
            if (br_bit(b)) return unsupported(FEAT_AIC);
            if (br_bit(b)) return unsupported(FEAT_DF);
            c->slice_structured = (int)br_bit(b);
            if (br_bit(b)) return unsupported(FEAT_RPS);
            if (br_bit(b)) return unsupported(FEAT_ISD);
            if (br_bit(b)) return unsupported(FEAT_AIV);
            if (br_bit(b)) return unsupported(FEAT_MQ);
            if (!br_bit(b)) return -1;             /* OPPTYPE marker */
            br_skip(b, 3);                         /* reserved       */
        } else if (ufep != 0) {
            return -1;
        }

        switch ((int)br_bits(b, 3)) {              /* MPPTYPE picture type */
        case 0: p->pict_type = 0; break;
        case 1: p->pict_type = 1; break;
        case 2: return unsupported(FEAT_PB);       /* improved PB */
        case 3: return unsupported(FEAT_BPIC);
        default: return -1;                        /* EI/EP scalability */
        }
        if (br_bits(b, 2)) return unsupported(FEAT_RESAMPLE);  /* RPR, RRU */
        p->no_rounding = (int)br_bit(b);
        br_skip(b, 2);                             /* reserved        */
        if (!br_bit(b)) return -1;                 /* MPPTYPE marker  */
        if (br_bit(b)) return unsupported(FEAT_CPM);

        if (ufep) {
            if (format == 6) {                     /* CPFMT */
                int par = (int)br_bits(b, 4);
                width = ((int)br_bits(b, 9) + 1) * 4;
                if (!br_bit(b)) return -1;         /* CPFMT marker */
                height = (int)br_bits(b, 9) * 4;
                if (par == 15) br_skip(b, 16);     /* EPAR */
            } else if (format >= 1 && format <= 5) {
                width = std_format[format][0];
                height = std_format[format][1];
            } else {
                return -1;
            }
            if (c->custom_pcf) br_skip(b, 8);      /* CPCFC */
        }
        if (c->custom_pcf) br_skip(b, 2);          /* ETR */
        if (ufep) {
            if (c->umv && br_bit(b) == 0) br_skip(b, 1);   /* UUI */
            if (c->slice_structured) {             /* SSS */
                if (br_bit(b)) return unsupported(FEAT_RECT_SLICE);
                if (br_bit(b)) return unsupported(FEAT_ASO);
            }
        }
        p->quant = (int)br_bits(b, 5);
    }

    /* The frame buffers are sized when the decoder is opened, so a picture
     * that disagrees has to be refused rather than overrun them. */
    if (width && (width != c->w || height != c->h)) {
        static int reported;
        if (!reported) {
            reported = 1;
            fprintf(stderr, "H.263: picture is %dx%d, decoder was opened for "
                    "%dx%d\n", width, height, c->w, c->h);
        }
        return -2;
    }
    if (p->quant < 1 || p->quant > 31 || br_overrun(b)) return -1;

    while (br_bit(b)) {                            /* PEI / PSPARE */
        br_skip(b, 8);
        if (br_overrun(b)) return -1;
    }
    if (c->slice_structured) {
        if (!br_bit(b)) return -1;                 /* SEPB1 */
        p->mb_start = decode_mba(c, b);
        if (!br_bit(b)) return -1;                 /* SEPB2 */
    }
    return br_overrun(b) ? -1 : 0;
}

/* GOB header, or the slice header that replaces it in slice-structured mode.
 * Both are introduced by 16 zero bits, which may be preceded by up to seven
 * zero stuffing bits, and terminated by the first one bit. */
static int decode_gob_header(h263_ctx *c, bitreader *b, int *quant, int *mb_pos)
{
    int q, pos, left;

    if (br_peek(b, 16) != 0) return -1;
    br_skip(b, 16);
    left = br_left(b);
    if (left > 32) left = 32;                      /* GSTUF is under a byte */
    for (; left > 13; left--)
        if (br_bit(b))
            break;
    if (left <= 13) return -1;

    if (c->slice_structured) {
        if (!br_bit(b)) return -1;                 /* SEPB1  */
        pos = decode_mba(c, b);
        if (c->mb_w * c->mb_h > 1583 && !br_bit(b)) return -1;   /* SEPB2 */
        q = (int)br_bits(b, 5);                    /* SQUANT */
        if (!br_bit(b)) return -1;                 /* SEPB3  */
        br_skip(b, 2);                             /* GFID   */
    } else {
        int gn = (int)br_bits(b, 5);               /* GN     */
        br_skip(b, 2);                             /* GFID   */
        q = (int)br_bits(b, 5);                    /* GQUANT */
        if (gn < 1) return -1;
        pos = gn * c->gob_index * c->mb_w;
    }
    if (q < 1 || q > 31 || br_overrun(b)) return -1;
    *quant = q;
    *mb_pos = pos;
    return 0;
}

/* ---- picture decode -------------------------------------------------- */
static int decode_macroblock(h263_ctx *c, bitreader *b, const h263_picture *p,
                             int mbx, int mby, int first_line, int resync_x,
                             int *quant)
{
    mvblk *mv = &c->mv[mby * c->mb_w + mbx];
    int type, cbpc, cbpy, cbp, intra, block, q = *quant;
    int mvx = 0, mvy = 0;

    if (p->pict_type && br_bit(b)) {               /* COD: skipped */
        if (reconstruct_inter_mb(c, b, mbx, mby, 0, q, 0, 0, !p->no_rounding))
            return -1;
        mv->x = mv->y = 0;
        return 0;
    }
    do {
        if (match_mcbpc(b, !p->pict_type, &type, &cbpc))
            return -1;
    } while (type == 5);                           /* macroblock stuffing */

    intra = type >= 3;
    if (type == 2) return unsupported(FEAT_4MV);
    cbpy = match_cbpy(b);
    if (cbpy < 0) return -1;
    if (!intra) cbpy ^= 15;
    cbp = (cbpy << 2) | cbpc;
    if (type == 1 || type == 4) {                  /* DQUANT */
        static const int dq[4] = { -1, -2, 1, 2 };
        q += dq[br_bits(b, 2)];
        if (q < 1) q = 1;
        if (q > 31) q = 31;
        *quant = q;
    }

    if (intra) {
        for (block = 0; block < 6; block++)
            if (decode_intra_block(c, b, block, mbx, mby,
                                   (cbp >> (5 - block)) & 1, q))
                return -1;
    } else {
        int px, py;
        motion_predict(c, mbx, mby, first_line, resync_x, &px, &py);
        mvx = decode_motion(b, px, c);
        mvy = decode_motion(b, py, c);
        if (mvx >= MV_ERROR || mvy >= MV_ERROR) return -1;
        if (c->umv && mvx - px == 1 && mvy - py == 1)
            br_skip(b, 1);                         /* PSC emulation stuffing */
        if (reconstruct_inter_mb(c, b, mbx, mby, cbp, q, mvx, mvy,
                                 !p->no_rounding))
            return -1;
    }
    mv->x = (int16_t)mvx;
    mv->y = (int16_t)mvy;
    return 0;
}

static int decode_picture(h263_ctx *c, bitreader *b)
{
    h263_picture p;
    int mb_total = c->mb_w * c->mb_h;
    int mb_index, resync_x, resync_y, first_line, quant, rc;

    rc = parse_picture_header(c, b, &p);
    if (rc)
        return rc;
    if (p.pict_type && !c->have_ref)
        return -3;
    if (p.mb_start < 0 || p.mb_start >= mb_total)
        return -1;

    memset(c->mv, 0, (size_t)mb_total * sizeof(*c->mv));
    quant = p.quant;
    mb_index = p.mb_start;
    resync_x = mb_index % c->mb_w;
    resync_y = mb_index / c->mb_w;
    first_line = 1;

    while (mb_index < mb_total) {
        int mbx = mb_index % c->mb_w, mby = mb_index / c->mb_w;

        /* Macroblock data can never present 16 zero bits, so they can only be
         * the GOB/slice header that starts the next segment. */
        if (mb_index != p.mb_start && br_left(b) >= 16 && br_peek(b, 16) == 0) {
            int pos;
            if (decode_gob_header(c, b, &quant, &pos))
                return -1;
            if (pos != mb_index)                   /* segments are contiguous */
                return -1;
            resync_x = mbx;
            resync_y = mby;
            first_line = 1;
        }
        if (mbx == resync_x && mby == resync_y + 1)
            first_line = 0;

        rc = decode_macroblock(c, b, &p, mbx, mby, first_line, resync_x, &quant);
        if (rc)
            return rc;
        if (br_overrun(b))
            return -1;
        mb_index++;
    }
    return 0;
}

static void yuv_to_rgb(h263_ctx *c)
{
    mr_yuv420_to_rgb24(c->rgb, c->w * 3,
                       c->cur[0], c->ystride,
                       c->cur[1], c->cstride,
                       c->cur[2], c->cstride,
                       c->w, c->h, NULL, NULL);
}

/* ---- codec lifecycle ------------------------------------------------- */
static mr_status h263_open(mr_decoder *dec)
{
    h263_ctx *c = (h263_ctx *)calloc(1, sizeof(*c));
    int i;
    if (!c)
        return MR_ENOMEM;

    c->w = dec->width; c->h = dec->height;
    c->mb_w = (c->w + 15) >> 4; c->mb_h = (c->h + 15) >> 4;
    c->cw = c->mb_w * 16; c->ch = c->mb_h * 16;
    c->ystride = c->cw; c->cstride = c->cw >> 1;
    c->gob_index = c->h <= 400 ? 1 : (c->h <= 800 ? 2 : 4);
    for (i = 0; i < 3; i++) {
        size_t size = i == 0 ? (size_t)c->ystride * c->ch
                             : (size_t)c->cstride * (c->ch >> 1);
        c->cur[i] = (uint8_t *)malloc(size);
        c->ref[i] = (uint8_t *)malloc(size);
        if (!c->cur[i] || !c->ref[i])
            goto oom;
        memset(c->cur[i], i ? 128 : 16, size);
        memset(c->ref[i], i ? 128 : 16, size);
    }
    c->rgb = (uint8_t *)malloc((size_t)c->w * c->h * 3);
    c->mv = (mvblk *)calloc((size_t)c->mb_w * c->mb_h, sizeof(*c->mv));
    if (!c->rgb || !c->mv)
        goto oom;

    dec->priv = c;
    dec->frame.width = c->w; dec->frame.height = c->h;
    dec->frame.fmt = MR_PIX_RGB24; dec->frame.stride = c->w * 3;
    dec->frame.data = c->rgb;
    dec->frame.dirty_y0 = 0; dec->frame.dirty_y1 = c->h;
    return MR_OK;

oom:
    for (i = 0; i < 3; i++) {
        free(c->cur[i]);
        free(c->ref[i]);
    }
    free(c->rgb); free(c->mv);
    free(c);
    return MR_ENOMEM;
}

static mr_status h263_decode(mr_decoder *dec, const uint8_t *data, uint32_t len)
{
    h263_ctx *c = (h263_ctx *)dec->priv;
    bitreader br;
    int i;
    if (!c || !data || len == 0 || len > 0x7fffffffUL)
        return MR_EFORMAT;
    br_init(&br, data, (int)len);
    { int rc = decode_picture(c, &br);
      if (rc == -2) return MR_EUNSUPPORTED;
      if (rc) return MR_EFORMAT; }
    yuv_to_rgb(c);
    for (i = 0; i < 3; i++) {
        uint8_t *tmp = c->ref[i];
        c->ref[i] = c->cur[i];
        c->cur[i] = tmp;
    }
    c->have_ref = 1;
    dec->frame.dirty_y0 = 0;
    dec->frame.dirty_y1 = c->h;
    return MR_OK;
}

static void h263_close(mr_decoder *dec)
{
    h263_ctx *c = (h263_ctx *)dec->priv;
    int i;
    if (!c)
        return;
    for (i = 0; i < 3; i++) {
        free(c->cur[i]);
        free(c->ref[i]);
    }
    free(c->rgb); free(c->mv);
    free(c);
    dec->priv = NULL;
}

const mr_codec mr_codec_h263 = {
    "H.263",
    { MR_FOURCC('H','2','6','3'), MR_FOURCC('h','2','6','3'),
      MR_FOURCC('I','2','6','3'), MR_FOURCC('i','2','6','3'),
      MR_FOURCC('U','2','6','3'), MR_FOURCC('u','2','6','3'),
      MR_FOURCC('T','2','6','3'), MR_FOURCC('X','2','6','3'),
      /* QuickTime/3GP sample description for the same bitstream. */
      MR_FOURCC('s','2','6','3'), MR_FOURCC('S','2','6','3') },
    h263_open,
    h263_decode,
    h263_close,
    NULL
};
