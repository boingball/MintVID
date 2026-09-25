/*
 * MintVID - direct YUV420P -> HAM6/HAM8 encoding (see the header for the
 * geometry contract and why the tables below are rebuilt rather than shared).
 *
 * A straight algebraic fusion of three independently validated passes:
 * core/mr_yuv.c's mr_yuv420_to_rgb24(), core/mr_scale.c's nearest-neighbour
 * row/column selection, and core/mr_ham.c's greedy hold-and-modify encoder.
 * The RGB triple exists only in registers between the first and the last.
 */
#include "mr_yuv_ham.h"

#if defined(__GNUC__)
#define MR_FORCE_INLINE static inline __attribute__((always_inline))
#else
#define MR_FORCE_INLINE static inline
#endif

/* ---- YCbCr -> RGB tables (mirrors core/mr_yuv.c's build_tables()) ---- */
static int g_luma_x298[256];
static int g_e_x409[256];
static int g_d_xm100[256];
static int g_e_xm208[256];
static int g_d_x516[256];
static int g_yuv_tables_ready = 0;

static void build_yuv_tables(void)
{
    int i;
    for (i = 0; i < 256; i++) {
        int d = i - 128, e = i - 128;
        g_luma_x298[i] = 298 * i;
        g_e_x409[i] = 409 * e;
        g_d_xm100[i] = -100 * d;
        g_e_xm208[i] = -208 * e;
        g_d_x516[i] = 516 * d;
    }
    g_yuv_tables_ready = 1;
}

static uint8_t clip8(int value)
{
    if (value < 0) return 0;
    if (value > 255) return 255;
    return (uint8_t)value;
}

static int iabs(int v) { return v < 0 ? -v : v; }

/* ---- HAM quantisers (mirrors core/mr_ham.c's build_ham_lut()) ----
 *
 * grey_dist[q][v] = |v - q*17| is 4 KB of table for a value that could just
 * be computed as |R - grey_v[sum]|, since grey_v already holds that q*17.
 * It is kept anyway, exactly as mr_ham.c keeps it: HAM6 evaluates it three
 * times per pixel, and on m68k three indexed byte loads beat three
 * subtract-and-branch absolute values. Dropping it to save the 4 KB was tried
 * and measured ~20% slower on the HAM6 encode under qemu-m68k. */
static uint8_t q4[256], s4[256], serr8[256];
static uint8_t grey_q[766], grey_v[766], grey_dist[16][256];
static int     g_ham_tables_ready = 0;

static void build_ham_tables(void)
{
    int v, q, sum;
    for (v = 0; v < 256; v++) {
        q4[v]    = (uint8_t)((v * 3 + 127) / 255);
        s4[v]    = (uint8_t)(q4[v] * 85);
        serr8[v] = (uint8_t)iabs(v - q4[v] * 85);
    }
    for (q = 0; q < 16; q++)
        for (v = 0; v < 256; v++)
            grey_dist[q][v] = (uint8_t)iabs(v - q * 17);
    for (sum = 0; sum <= 765; sum++) {
        q = ((sum / 3) * 15 + 127) / 255;
        grey_q[sum] = (uint8_t)q;
        grey_v[sum] = (uint8_t)(q * 17);
    }
    g_ham_tables_ready = 1;
}

/*
 * The scanline state threaded through the encoders: r/g/b are the previous
 * pixel's reconstructed colour - the "hold" half of hold-and-modify - and px
 * is the byte the call just produced.
 *
 * Passed and returned by value rather than updated through int pointers: the
 * caller stores px into a uint8_t row, and a uint8_t write may alias anything,
 * so with pointer arguments nothing stops the compiler reloading all three
 * held channels after every pixel. Measured under qemu-m68k the two forms came
 * out even, so this is chosen for not depending on the compiler's judgement
 * rather than for a demonstrated speedup.
 */
typedef struct { int r, g, b; uint8_t px; } ham_hold;

/* One HAM8 pixel: base palette is a 4x4x4 RGB cube, modify is 6-bit.
 * t is the ordered-dither threshold added before a modify write's truncation
 * (0 when not dithering - see mr_ham_encode_ex() in core/mr_ham.h). */
MR_FORCE_INLINE ham_hold ham8_pixel(int R, int G, int B, int t, ham_hold h)
{
    int pr = h.r, pg = h.g, pb = h.b;
    int v;
    int dpr = iabs(R - pr), dpg = iabs(G - pg), dpb = iabs(B - pb);
    int er = (R & 3) + dpg + dpb;
    int eg = dpr + (G & 3) + dpb;
    int eb = dpr + dpg + (B & 3);
    int e_set = serr8[R] + serr8[G] + serr8[B];

    if (e_set <= er && e_set <= eg && e_set <= eb) {
        h.r = s4[R]; h.g = s4[G]; h.b = s4[B];
        h.px = (uint8_t)((q4[R] << 4) | (q4[G] << 2) | q4[B]);
    } else if (er <= eg && er <= eb) {
        v = R + t; if (v > 255) v = 255;
        h.r = v & ~3;
        h.px = (uint8_t)(0x80 | (v >> 2));
    } else if (eg <= eb) {
        v = G + t; if (v > 255) v = 255;
        h.g = v & ~3;
        h.px = (uint8_t)(0xc0 | (v >> 2));
    } else {
        v = B + t; if (v > 255) v = 255;
        h.b = v & ~3;
        h.px = (uint8_t)(0x40 | (v >> 2));
    }
    return h;
}

/* One HAM6 pixel; the base palette is a 16-entry grey ramp. */
MR_FORCE_INLINE ham_hold ham6_pixel(int R, int G, int B, int t, ham_hold h)
{
    int pr = h.r, pg = h.g, pb = h.b;
    int v;
    int dpr = iabs(R - pr), dpg = iabs(G - pg), dpb = iabs(B - pb);
    int held = dpr + dpg + dpb;
    int best = (R & 15) - dpr, channel = 0;
    int delta = (G & 15) - dpg;
    int sum = R + G + B, qi = grey_q[sum];
    int e_set = grey_dist[qi][R] + grey_dist[qi][G] + grey_dist[qi][B];

    if (delta < best) { best = delta; channel = 1; }
    delta = (B & 15) - dpb;
    if (delta < best) { best = delta; channel = 2; }

    if (e_set <= held + best) {
        h.r = h.g = h.b = grey_v[sum];
        h.px = (uint8_t)qi;
    } else if (channel == 0) {
        v = R + t; if (v > 255) v = 255;
        h.r = v & ~15;
        h.px = (uint8_t)(0x20 | (v >> 4));
    } else if (channel == 1) {
        v = G + t; if (v > 255) v = 255;
        h.g = v & ~15;
        h.px = (uint8_t)(0x30 | (v >> 4));
    } else {
        v = B + t; if (v > 255) v = 255;
        h.b = v & ~15;
        h.px = (uint8_t)(0x10 | (v >> 4));
    }
    return h;
}

/* One YCbCr sample to RGB, mirroring core/mr_yuv.c. The three chroma-derived
 * addends are loop-invariant across a 4:2:0 luma pair, so the caller hoists
 * them and this stays a per-luma-sample step. */
MR_FORCE_INLINE void yuv_sample_rgb(int luma, int red_add, int green_add,
                                    int blue_add, int *r, int *g, int *b)
{
    int scaled_y;
    if (luma < 0) luma = 0;
    scaled_y = g_luma_x298[luma];
    *r = clip8((scaled_y + red_add) >> 8);
    *g = clip8((scaled_y + green_add) >> 8);
    *b = clip8((scaled_y + blue_add) >> 8);
}

static void ham_prepare(int *bits)
{
    if (!g_yuv_tables_ready) build_yuv_tables();
    if (!g_ham_tables_ready) build_ham_tables();
    *bits = (*bits >= 8) ? 8 : 6;
}

/*
 * One output row, generated once per HAM depth so the depth test sits outside
 * the pixel loop - the same shape mr_ham_encode() uses.
 *
 * Each pixel is converted and immediately encoded rather than converting a
 * whole 4:2:0 chroma pair and then encoding both, keeping one RGB triple live
 * instead of two. The chroma-derived addends, which are what the pair
 * genuinely shares, are still hoisted out.
 *
 * A caution for anyone tuning this further: under qemu-m68k the 1:1 timings
 * for this loop swing by up to 2x purely with how the encoders get inlined,
 * in directions that contradict each other between HAM6 and HAM8. Those are
 * code-layout artefacts of the emulator's translation, not properties of the
 * algorithm, and they will not transfer to a 68040 or to Emu68 - do not tune
 * against them without measuring on real hardware. The geometry this module
 * is actually wired to (aga_supports_yuv_indexed(), vertical downscale only)
 * was chosen because its ~45% saving comes from converting fewer rows and so
 * held steady across every one of those arrangements.
 */
/* 4x4 ordered-dither thresholds, identical to core/mr_ham.c's. */
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

/* DITHER is a constant 0 or 1 per generated function; trow is the pattern
 * row, already scaled to one modify step (ham_bayer4_8 for HAM8,
 * ham_bayer4 for HAM6). With DITHER 0 every threshold folds to 0. */
#define MR_HAM_ROW_FN(NAME, PIXEL, DITHER)                                   \
static void NAME(const uint8_t *src_y, const uint8_t *src_u,                 \
                 const uint8_t *src_v, int width, uint8_t *dr,               \
                 const uint8_t *trow)                                        \
{                                                                            \
    ham_hold h;                 /* hold-and-modify resets every scanline */  \
    int x, r, g, b;                                                          \
    (void)trow;                                                              \
    h.r = h.g = h.b = 0; h.px = 0;                                           \
    for (x = 0; x + 1 < width; x += 2) {                                     \
        unsigned uu = src_u[x >> 1], vv = src_v[x >> 1];                     \
        int red_add = g_e_x409[vv] + 128;                                    \
        int green_add = g_d_xm100[uu] + g_e_xm208[vv] + 128;                 \
        int blue_add = g_d_x516[uu] + 128;                                   \
        yuv_sample_rgb((int)src_y[x] - 16, red_add, green_add, blue_add,     \
                       &r, &g, &b);                                          \
        h = PIXEL(r, g, b, (DITHER) ? trow[x & 3] : 0, h);       \
        dr[x] = h.px;                                                        \
        yuv_sample_rgb((int)src_y[x + 1] - 16, red_add, green_add, blue_add, \
                       &r, &g, &b);                                          \
        h = PIXEL(r, g, b, (DITHER) ? trow[(x + 1) & 3] : 0, h); \
        dr[x + 1] = h.px;                                                    \
    }                                                                        \
    if (x < width) {                                                         \
        unsigned uu = src_u[x >> 1], vv = src_v[x >> 1];                     \
        yuv_sample_rgb((int)src_y[x] - 16, g_e_x409[vv] + 128,               \
                       g_d_xm100[uu] + g_e_xm208[vv] + 128,                  \
                       g_d_x516[uu] + 128, &r, &g, &b);                      \
        h = PIXEL(r, g, b, (DITHER) ? trow[x & 3] : 0, h);       \
        dr[x] = h.px;                                                        \
    }                                                                        \
}
MR_HAM_ROW_FN(ham_row8, ham8_pixel, 0)
MR_HAM_ROW_FN(ham_row6, ham6_pixel, 0)
MR_HAM_ROW_FN(ham_row8_dither, ham8_pixel, 1)
MR_HAM_ROW_FN(ham_row6_dither, ham6_pixel, 1)
#undef MR_HAM_ROW_FN

void mr_yuv420_ham_encode_ex(const uint8_t *y_plane, int y_stride,
                             const uint8_t *u_plane, int u_stride,
                             const uint8_t *v_plane, int v_stride,
                             int width, int height, int vscale, int bits,
                             int dither, uint8_t *out, int out_stride)
{
    int dst_h, oy;
    if (!y_plane || !u_plane || !v_plane || !out ||
        width <= 0 || height <= 0 || vscale <= 0)
        return;
    ham_prepare(&bits);
    dst_h = height / vscale;

    for (oy = 0; oy < dst_h; oy++) {
        int src_row = vscale * oy + vscale / 2;
        int chroma_row = src_row >> 1;
        const uint8_t *ry = y_plane + (size_t)src_row * y_stride;
        const uint8_t *ru = u_plane + (size_t)chroma_row * u_stride;
        const uint8_t *rv = v_plane + (size_t)chroma_row * v_stride;
        const uint8_t *trow = bits == 8 ? ham_bayer4_8[oy & 3]
                                        : ham_bayer4[oy & 3];
        uint8_t *dr = out + (size_t)oy * out_stride;
        if (dither) {
            if (bits == 8) ham_row8_dither(ry, ru, rv, width, dr, trow);
            else           ham_row6_dither(ry, ru, rv, width, dr, trow);
        } else {
            if (bits == 8) ham_row8(ry, ru, rv, width, dr, trow);
            else           ham_row6(ry, ru, rv, width, dr, trow);
        }
    }
}

void mr_yuv420_ham_encode(const uint8_t *y_plane, int y_stride,
                          const uint8_t *u_plane, int u_stride,
                          const uint8_t *v_plane, int v_stride,
                          int width, int height, int vscale, int bits,
                          uint8_t *out, int out_stride)
{
    mr_yuv420_ham_encode_ex(y_plane, y_stride, u_plane, u_stride, v_plane,
                            v_stride, width, height, vscale, bits, 0, out,
                            out_stride);
}
