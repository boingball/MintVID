/*
 * MintVID - HAM8/HAM6 encoder + decoder.
 */
#include "mr_ham.h"

static int iabs(int v) { return v < 0 ? -v : v; }

/* Divide-free channel quantisers, built once (divides are slow on 68030).
 * q4[v]   -> nearest of the 4 HAM8 cube levels, index 0..3
 * s4[v]   -> that level's value (q4[v]*85)
 * grey_q/sum and grey_v/sum replace HAM6's per-pixel divide and quantise;
 * grey_dist caches its three channel-to-grey distance calculations.
 * Values are identical to the previous (v*N+127)/255 arithmetic. */
static uint8_t q4[256], s4[256], serr8[256];
static uint8_t grey_q[766], grey_v[766], grey_dist[16][256];
static int     ham_lut_ready = 0;

#if defined(MR_M68K_ASM) && !defined(MR_HAM_NO_ASM)
/* core/mr_ham_m68k.S - the same rows in hand-written m68k, every value in a
 * register (see its header). Bound with __asm__ to the bare .globl names,
 * so m68k-amigaos-gcc's underscore prefix never applies. Arguments: source
 * row, destination row, width, &g_m68k_abs[255], the depth's aux table,
 * and the row's four dither thresholds packed low byte first. */
typedef void (*mr_ham_row_fn)(const uint8_t *src, uint8_t *dst, int32_t w,
                              const int32_t *abs_centre, const int32_t *aux,
                              uint32_t tw);
void mr_ham8_row_m68k(const uint8_t *, uint8_t *, int32_t, const int32_t *,
                      const int32_t *, uint32_t) __asm__("mr_ham8_row_m68k");
void mr_ham8d_row_m68k(const uint8_t *, uint8_t *, int32_t, const int32_t *,
                       const int32_t *, uint32_t) __asm__("mr_ham8d_row_m68k");
void mr_ham6_row_m68k(const uint8_t *, uint8_t *, int32_t, const int32_t *,
                      const int32_t *, uint32_t) __asm__("mr_ham6_row_m68k");
void mr_ham6d_row_m68k(const uint8_t *, uint8_t *, int32_t, const int32_t *,
                       const int32_t *, uint32_t) __asm__("mr_ham6d_row_m68k");

/* |i| for i = -255..255; the kernel indexes from entry 255. */
static int32_t g_m68k_abs[511];
/* HAM8, byte offsets as in mr_ham_m68k.S: longs serr8, q4, -4 * s4 (a
 * held-colour pointer offset) and -4 * (v & ~3) (the held offset after a
 * modify), then the three channels' modify output bytes, 0x80/0xc0/0x40 |
 * v >> 2. 1216 longs = 4864 bytes. */
static int32_t g_m68k_aux8[1216];
/* HAM6: longs grey value[766] and grey index[766] by R+G+B, -4 * (v & ~15),
 * then the modify output bytes 0x20/0x30/0x10 | v >> 4. 1980 longs. */
static int32_t g_m68k_aux6[1980];

static void build_m68k_tables(void)
{
    static const uint8_t code8[3] = { 0x80, 0xc0, 0x40 };
    static const uint8_t code6[3] = { 0x20, 0x30, 0x10 };
    uint8_t *bytes8 = (uint8_t *)&g_m68k_aux8[1024];
    uint8_t *bytes6 = (uint8_t *)&g_m68k_aux6[1788];
    int i, c;
    for (i = 0; i < 511; i++)
        g_m68k_abs[i] = iabs(i - 255);
    for (i = 0; i < 256; i++) {
        g_m68k_aux8[i]       = serr8[i];
        g_m68k_aux8[256 + i] = q4[i];
        g_m68k_aux8[512 + i] = -4 * (int32_t)s4[i];
        g_m68k_aux8[768 + i] = -4 * (int32_t)(i & ~3);
        g_m68k_aux6[1532 + i] = -4 * (int32_t)(i & ~15);
        for (c = 0; c < 3; c++) {
            bytes8[c * 256 + i] = (uint8_t)(code8[c] | (i >> 2));
            bytes6[c * 256 + i] = (uint8_t)(code6[c] | (i >> 4));
        }
    }
    for (i = 0; i < 766; i++) {
        g_m68k_aux6[i]       = grey_v[i];
        g_m68k_aux6[766 + i] = grey_q[i];
    }
}
#endif

static void build_ham_lut(void)
{
    int v, q, sum;
    for (v = 0; v < 256; v++) {
        q4[v]    = (uint8_t)((v * 3 + 127) / 255);
        s4[v]    = (uint8_t)(q4[v] * 85);
        serr8[v] = (uint8_t)iabs(v - q4[v] * 85);   /* HAM8 per-channel set err */
    }
    for (q = 0; q < 16; q++)
        for (v = 0; v < 256; v++)
            grey_dist[q][v] = (uint8_t)iabs(v - q * 17);
    for (sum = 0; sum <= 765; sum++) {
        q = ((sum / 3) * 15 + 127) / 255;
        grey_q[sum] = (uint8_t)q;
        grey_v[sum] = (uint8_t)(q * 17);
    }
#if defined(MR_M68K_ASM) && !defined(MR_HAM_NO_ASM)
    build_m68k_tables();
#endif
    ham_lut_ready = 1;
}

void mr_ham_palette(uint8_t *pal, int bits)
{
    int i;
    if (bits >= 8) {
        /* 4x4x4 RGB cube (levels 0,85,170,255) */
        for (i = 0; i < 64; i++) {
            pal[i * 3 + 0] = (uint8_t)(((i >> 4) & 3) * 85);
            pal[i * 3 + 1] = (uint8_t)(((i >> 2) & 3) * 85);
            pal[i * 3 + 2] = (uint8_t)((i & 3) * 85);
        }
    } else {
        /* 16-entry grey ramp */
        for (i = 0; i < 16; i++)
            pal[i * 3 + 0] = pal[i * 3 + 1] = pal[i * 3 + 2] = (uint8_t)(i * 17);
    }
}

/* 4x4 ordered-dither thresholds for the modify writes (see mr_ham.h).
 * HAM6 modifies in steps of 16 and uses them as they are (0..15); HAM8
 * modifies in steps of 4 and uses them >> 2 (0..3), pre-shifted below. */
static const uint8_t ham_bayer4[4][4] = {
    {  0,  8,  2, 10 }, { 12,  4, 14,  6 },
    {  3, 11,  1,  9 }, { 15,  7, 13,  5 }
};
/* The same scaled to one HAM8 modify step (values >> 2), so the pixel loop
 * reads a ready threshold instead of shifting one. */
static const uint8_t ham_bayer4_8[4][4] = {
    { 0, 2, 0, 2 }, { 3, 1, 3, 1 },
    { 0, 2, 0, 2 }, { 3, 1, 3, 1 }
};

/* One encoder row. `T` is the dither threshold expression for pixel x: the
 * literal 0 for the plain encoder (the compiler folds every "+ 0" and
 * "> 255" test away), or a table read for the dithered one. The dither only
 * moves the value written when a pixel modifies one channel: that write is
 * a truncation (R >> 2, R >> 4), and adding a threshold that cycles through
 * 0..step-1 over a 4x4 block first makes it average to the true value
 * instead of always rounding down. The channel decision and the base-colour
 * ("set") choice stay undithered, so edges are handled exactly as before. */
#define HAM8_ROW(T)                                                          \
    for (x = 0; x < w; x++, sr += 3) {                                       \
        int R = sr[0], G = sr[1], B = sr[2];                                 \
        int dpr = iabs(R - pr), dpg = iabs(G - pg), dpb = iabs(B - pb);      \
        int er = (R & 3) + dpg + dpb;                                        \
        int eg = dpr + (G & 3) + dpb;                                        \
        int eb = dpr + dpg + (B & 3);                                        \
        int e_set = serr8[R] + serr8[G] + serr8[B];                          \
        int v;                                                               \
                                                                             \
        if (e_set <= er && e_set <= eg && e_set <= eb) {                     \
            *dr++ = (uint8_t)((q4[R] << 4) | (q4[G] << 2) | q4[B]);          \
            pr = s4[R]; pg = s4[G]; pb = s4[B];                              \
        } else if (er <= eg && er <= eb) {                                   \
            v = R + (T); if (v > 255) v = 255;                               \
            *dr++ = (uint8_t)(0x80 | (v >> 2)); pr = v & ~3;                 \
        } else if (eg <= eb) {                                               \
            v = G + (T); if (v > 255) v = 255;                               \
            *dr++ = (uint8_t)(0xc0 | (v >> 2)); pg = v & ~3;                 \
        } else {                                                             \
            v = B + (T); if (v > 255) v = 255;                               \
            *dr++ = (uint8_t)(0x40 | (v >> 2)); pb = v & ~3;                 \
        }                                                                    \
    }

#define HAM6_ROW(T)                                                          \
    for (x = 0; x < w; x++, sr += 3) {                                       \
        int R = sr[0], G = sr[1], B = sr[2];                                 \
        int dpr = iabs(R - pr), dpg = iabs(G - pg), dpb = iabs(B - pb);      \
        int held = dpr + dpg + dpb;                                          \
        int best = (R & 15) - dpr, channel = 0;                              \
        int delta = (G & 15) - dpg;                                          \
        int sum = R + G + B, qi = grey_q[sum];                               \
        int e_set = grey_dist[qi][R] + grey_dist[qi][G] + grey_dist[qi][B];  \
        int v;                                                               \
        if (delta < best) { best = delta; channel = 1; }                     \
        delta = (B & 15) - dpb;                                              \
        if (delta < best) { best = delta; channel = 2; }                     \
                                                                             \
        if (e_set <= held + best) {                                          \
            *dr++ = (uint8_t)qi;                                             \
            pr = pg = pb = grey_v[sum];                                      \
        } else if (channel == 0) {                                           \
            v = R + (T); if (v > 255) v = 255;                               \
            *dr++ = (uint8_t)(0x20 | (v >> 4)); pr = v & ~15;                \
        } else if (channel == 1) {                                           \
            v = G + (T); if (v > 255) v = 255;                               \
            *dr++ = (uint8_t)(0x30 | (v >> 4)); pg = v & ~15;                \
        } else {                                                             \
            v = B + (T); if (v > 255) v = 255;                               \
            *dr++ = (uint8_t)(0x10 | (v >> 4)); pb = v & ~15;                \
        }                                                                    \
    }

void mr_ham_encode_ex(const uint8_t *rgb, int w, int h, int rgb_stride,
                      uint8_t *out, int out_stride, int bits, int y_base,
                      int dither)
{
    int x, y;

    if (!ham_lut_ready) build_ham_lut();
#if defined(MR_M68K_ASM) && !defined(MR_HAM_NO_ASM)
    {
        mr_ham_row_fn fn;
        const int32_t *aux;
        if (bits >= 8) {
            fn = dither ? mr_ham8d_row_m68k : mr_ham8_row_m68k;
            aux = g_m68k_aux8;
        } else {
            fn = dither ? mr_ham6d_row_m68k : mr_ham6_row_m68k;
            aux = g_m68k_aux6;
        }
        for (y = 0; y < h; y++) {
            const uint8_t *trow = bits >= 8 ? ham_bayer4_8[(y_base + y) & 3]
                                            : ham_bayer4[(y_base + y) & 3];
            uint32_t tw = (uint32_t)trow[0] | ((uint32_t)trow[1] << 8) |
                          ((uint32_t)trow[2] << 16) |
                          ((uint32_t)trow[3] << 24);
            fn(rgb + (size_t)y * rgb_stride, out + (size_t)y * out_stride, w,
               &g_m68k_abs[255], aux, tw);
        }
        (void)x;
        return;
    }
#endif
    for (y = 0; y < h; y++) {
        const uint8_t *sr = rgb + (size_t)y * rgb_stride;
        uint8_t       *dr = out + (size_t)y * out_stride;
        const uint8_t *trow = bits >= 8 ? ham_bayer4_8[(y_base + y) & 3]
                                        : ham_bayer4[(y_base + y) & 3];
        int pr = 0, pg = 0, pb = 0;           /* held colour (line start = 0)*/
        if (bits >= 8) {
            if (dither) HAM8_ROW(trow[x & 3])
            else        HAM8_ROW(0)
        } else {
            if (dither) HAM6_ROW(trow[x & 3])
            else        HAM6_ROW(0)
        }
    }
}

#undef HAM8_ROW
#undef HAM6_ROW

void mr_ham_encode(const uint8_t *rgb, int w, int h, int rgb_stride,
                   uint8_t *out, int out_stride, int bits)
{
    mr_ham_encode_ex(rgb, w, h, rgb_stride, out, out_stride, bits, 0, 0);
}

void mr_ham_decode(const uint8_t *ham, int w, int h, int in_stride,
                   const uint8_t *pal, uint8_t *rgb, int rgb_stride, int bits)
{
    int data_bits = bits - 2;
    int mshift    = 8 - data_bits;
    int data_mask = (1 << data_bits) - 1;
    int x, y;

    for (y = 0; y < h; y++) {
        const uint8_t *sr = ham + (size_t)y * in_stride;
        uint8_t       *dr = rgb + (size_t)y * rgb_stride;
        int r = 0, g = 0, b = 0;
        for (x = 0; x < w; x++) {
            int px   = sr[x];
            int ctrl = px >> data_bits;
            int data = px & data_mask;
            switch (ctrl) {
            case 0: r = pal[data * 3 + 0]; g = pal[data * 3 + 1];
                    b = pal[data * 3 + 2]; break;
            case 2: r = data << mshift; break;      /* modify red   */
            case 3: g = data << mshift; break;      /* modify green */
            default: b = data << mshift; break;     /* 1 = modify blue */
            }
            dr[x * 3 + 0] = (uint8_t)r;
            dr[x * 3 + 1] = (uint8_t)g;
            dr[x * 3 + 2] = (uint8_t)b;
        }
    }
}
