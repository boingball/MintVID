#include "../core/mr_yuv.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(MR_M68K_ASM)
/* mr_yuv420_to_rgb24_m68k is normally reached only through
 * mr_yuv420_to_rgb24()'s dispatch (core/mr_yuv.c). An earlier version of
 * it crashed real Pistorm hardware - d0/a0/a1 not preserved across the
 * periodic service() callback, since an ordinary C callee is entitled to
 * clobber m68k's caller-saved registers - fixed, and confirmed clean on
 * the same real hardware with audio servicing active. Declared directly
 * here (rather than relying on the public dispatch) so
 * check_yuv_service_clobber() below keeps exercising exactly that bug
 * class regardless of how the dispatch itself is wired. */
void mr_yuv420_to_rgb24_m68k(uint8_t *dst, int dst_stride,
                             const uint8_t *y_plane, int y_stride,
                             const uint8_t *u_plane, int u_stride,
                             const uint8_t *v_plane, int v_stride,
                             int width, int height,
                             mr_yuv_service_fn service, void *service_opaque,
                             const int *luma_x298, const int *e_x409,
                             const int *d_xm100, const int *e_xm208,
                             const int *d_x516)
    __asm__("mr_yuv420_to_rgb24_m68k");
#endif

static uint8_t reference_clip(int value)
{
    if (value < 0) return 0;
    if (value > 255) return 255;
    return (uint8_t)value;
}

static void reference_convert(uint8_t *dst, int dst_stride,
                              const uint8_t *yp, int ys,
                              const uint8_t *up, int us,
                              const uint8_t *vp, int vs,
                              int width, int height, int bgr)
{
    int y;
    for (y = 0; y < height; y++) {
        const uint8_t *yr = yp + y * ys;
        const uint8_t *ur = up + (y >> 1) * us;
        const uint8_t *vr = vp + (y >> 1) * vs;
        uint8_t *out = dst + y * dst_stride;
        int x;
        for (x = 0; x < width; x++) {
            int c = (int)yr[x] - 16;
            int d = (int)ur[x >> 1] - 128;
            int e = (int)vr[x >> 1] - 128;
            uint8_t r, g, b;
            if (c < 0) c = 0;
            r = reference_clip((298 * c + 409 * e + 128) >> 8);
            g = reference_clip((298 * c - 100 * d - 208 * e + 128) >> 8);
            b = reference_clip((298 * c + 516 * d + 128) >> 8);
            out[x * 3 + 0] = bgr ? b : r;
            out[x * 3 + 1] = g;
            out[x * 3 + 2] = bgr ? r : b;
        }
    }
}

static unsigned next_value(unsigned *state)
{
    *state = *state * 1664525U + 1013904223U;
    return *state;
}

static int run_case(int width, int height, unsigned seed)
{
    int ys = width + 3, cs = (width + 1) / 2 + 2, ds = width * 3 + 5;
    size_t yn = (size_t)ys * height;
    size_t cn = (size_t)cs * ((height + 1) / 2);
    size_t dn = (size_t)ds * height;
    uint8_t *yp = (uint8_t *)malloc(yn), *up = (uint8_t *)malloc(cn);
    uint8_t *vp = (uint8_t *)malloc(cn), *expected = (uint8_t *)malloc(dn);
    uint8_t *actual = (uint8_t *)malloc(dn);
    size_t i;
    int ok;
    if (!yp || !up || !vp || !expected || !actual) return 0;
    for (i = 0; i < yn; i++) yp[i] = (uint8_t)(next_value(&seed) >> 24);
    for (i = 0; i < cn; i++) up[i] = (uint8_t)(next_value(&seed) >> 24);
    for (i = 0; i < cn; i++) vp[i] = (uint8_t)(next_value(&seed) >> 24);
    memset(expected, 0xa5, dn);
    memset(actual, 0xa5, dn);
    reference_convert(expected, ds, yp, ys, up, cs, vp, cs, width, height, 0);
    mr_yuv420_to_rgb24(actual, ds, yp, ys, up, cs, vp, cs, width, height,
                       NULL, NULL);
    ok = memcmp(expected, actual, dn) == 0;
    if (!ok) fprintf(stderr, "YUV RGB mismatch at %dx%d\n", width, height);
    if (ok) {
        memset(expected, 0xa5, dn);
        memset(actual, 0xa5, dn);
        reference_convert(expected, ds, yp, ys, up, cs, vp, cs,
                          width, height, 1);
        mr_yuv420_to_bgr24(actual, ds, yp, ys, up, cs, vp, cs,
                           width, height, NULL, NULL);
        ok = memcmp(expected, actual, dn) == 0;
        if (!ok) fprintf(stderr, "YUV BGR mismatch at %dx%d\n", width, height);
    }
    free(yp); free(up); free(vp); free(expected); free(actual);
    return ok;
}

/* Chroma order is V-then-U (slots 1 and 3), not the U-then-V the RGBFB_
 * Y4U2V2 name implies - see mr_yuv.c's mr_yuv420_to_y4u2v2() for why: real
 * Voodoo3/P96 2.x hardware was confirmed to expect it swapped. Y/Cb/Cr are
 * also rescaled from studio (limited) range to near-full range (1..254,
 * not the literal 0..255 PC/JPEG span). Expected values below start with
 * mr_yuv.c's own rescale (round(
 * (sample-16)*253/219)+1 for luma, round((sample-128)*253/224)+128 for
 * chroma, both clamped to 1..254), followed by its chroma-dependent safe
 * luma ceiling where the P96 matrix would otherwise exceed 254. Test data
 * intentionally spans the studio range's black floor (16), white ceiling
 * (235/240), and deliberately out-of-gamut combinations. */
static int check_y4u2v2(void)
{
    enum { W = 6, H = 3, YS = 8, CS = 4, DS = 15 };
    static const uint8_t y[H * YS] = {
        16, 50, 100, 150, 200, 235, 0xee, 0xee,
        20, 60, 110, 160, 210, 235, 0xee, 0xee,
        24, 70, 120, 170, 220, 235, 0xee, 0xee
    };
    static const uint8_t u[2 * CS] = { 16, 128, 240, 0xee, 64, 192, 128, 0xee };
    static const uint8_t v[2 * CS] = { 240, 128, 16, 0xee, 32, 160, 96, 0xee };
    static const uint8_t expected[H][W * 2] = {
        { 1, 254, 40, 1, 98, 128, 156, 128, 31, 1, 31, 254 },
        { 6, 254, 52, 1, 110, 128, 167, 128, 31, 1, 31, 254 },
        { 10, 20, 63, 56, 121, 164, 126, 200, 228, 92, 228, 128 }
    };
    uint8_t out[H * DS];
    int row, col;

    memset(out, 0xa5, sizeof out);
    if (!mr_yuv420_to_y4u2v2(out, DS, y, YS, u, CS, v, CS,
                             W, H, NULL, NULL)) {
        fprintf(stderr, "Y4U2V2 conversion rejected valid input\n");
        return 0;
    }
    for (row = 0; row < H; row++) {
        if (memcmp(out + row * DS, expected[row], W * 2) != 0 ||
            out[row * DS + W * 2] != 0xa5) {
            fprintf(stderr, "Y4U2V2 mismatch on row %d:", row);
            for (col = 0; col < W * 2; col++)
                fprintf(stderr, " %u", (unsigned)out[row * DS + col]);
            fputc('\n', stderr);
            return 0;
        }
        /* Never a literal 0 or 255 byte - the whole point of the 1..254
         * safety margin (see the leading comment above). */
        for (col = 0; col < W * 2; col++) {
            uint8_t b = out[row * DS + col];
            if (b == 0 || b == 255) {
                fprintf(stderr, "Y4U2V2 row %d col %d hit the rail (%u), "
                                "safety margin failed\n", row, col,
                                (unsigned)b);
                return 0;
            }
        }
    }
    if (mr_yuv420_to_y4u2v2(out, DS, y, YS, u, CS, v, CS,
                            W - 1, H, NULL, NULL)) {
        fprintf(stderr, "Y4U2V2 accepted an odd width\n");
        return 0;
    }
    return 1;
}

/* Exhaustive companion to check_y4u2v2()'s own spot check: every possible
 * input byte (0..255), fed through the real public API rather than the
 * rescale formula reimplemented by hand, must land strictly inside
 * 1..254. A single missed clamp path (e.g. only the luma table guarded,
 * not chroma, or vice versa) would show up here even if check_y4u2v2()'s
 * own hand-picked spot values happened not to trigger it. */
static int check_y4u2v2_full_range_margin(void)
{
    enum { W = 256, H = 2 };
    static uint8_t y[H][W];
    static uint8_t u[H / 2][W / 2], v[H / 2][W / 2];
    static uint8_t out[H][W * 2];
    int i, row, col;

    for (i = 0; i < 256; i++) {
        y[0][i] = (uint8_t)i;
        y[1][i] = (uint8_t)i;
    }
    for (i = 0; i < 128; i++) {
        u[0][i] = (uint8_t)(i * 2);
        v[0][i] = (uint8_t)(i * 2);
    }

    if (!mr_yuv420_to_y4u2v2(&out[0][0], W * 2, &y[0][0], W, &u[0][0], W / 2,
                             &v[0][0], W / 2, W, H, NULL, NULL)) {
        fprintf(stderr, "Y4U2V2 full-sweep conversion rejected input\n");
        return 0;
    }
    for (row = 0; row < H; row++)
        for (col = 0; col < W * 2; col++) {
            uint8_t b = out[row][col];
            if (b == 0 || b == 255) {
                fprintf(stderr, "Y4U2V2 full-sweep: row %d col %d hit the "
                                "rail (%u)\n", row, col, (unsigned)b);
                return 0;
            }
        }
    return 1;
}

/* The packed bytes can all be inside 1..254 while P96's later YUV->RGB
 * matrix still exceeds 255 (bright luma plus a positive chroma term).  The
 * affected old overlay paths wrap that result to black.  Exercise every U/V
 * byte combination with the darkest and brightest possible source luma and
 * verify the un-clamped BT.601 result never crosses the upper safety rail. */
static int check_y4u2v2_matrix_ceiling(void)
{
    enum { W = 512, H = 512 };
    static uint8_t y[H][W];
    static uint8_t u[H / 2][W / 2], v[H / 2][W / 2];
    static uint8_t out[H][W * 2];
    int row, pair, adjusted = 0;

    for (row = 0; row < H; row++)
        for (pair = 0; pair < W / 2; pair++) {
            y[row][pair * 2] = 0;
            y[row][pair * 2 + 1] = 255;
        }
    for (row = 0; row < H / 2; row++)
        for (pair = 0; pair < W / 2; pair++) {
            u[row][pair] = (uint8_t)pair;
            v[row][pair] = (uint8_t)row;
        }

    if (!mr_yuv420_to_y4u2v2(&out[0][0], W * 2, &y[0][0], W,
                             &u[0][0], W / 2, &v[0][0], W / 2,
                             W, H, NULL, NULL)) {
        fprintf(stderr, "Y4U2V2 matrix-ceiling conversion failed\n");
        return 0;
    }

    for (row = 0; row < H; row++)
        for (pair = 0; pair < W / 2; pair++) {
            const uint8_t *p = &out[row][pair * 4];
            int uu = p[3], vv = p[1], pixel;
            int r_add = (359 * (vv - 128) + 128) >> 8;
            int g_add = (-88 * (uu - 128) - 183 * (vv - 128) + 128) >> 8;
            int b_add = (454 * (uu - 128) + 128) >> 8;
            if (p[2] != 254) adjusted = 1;
            for (pixel = 0; pixel < 2; pixel++) {
                int yy = p[pixel * 2];
                if (yy + r_add > 254 || yy + g_add > 254 ||
                    yy + b_add > 254) {
                    fprintf(stderr, "Y4U2V2 matrix overflow at U=%d V=%d "
                                    "Y=%d: RGB=%d,%d,%d\n", uu, vv, yy,
                                    yy + r_add, yy + g_add, yy + b_add);
                    return 0;
                }
            }
        }
    if (!adjusted) {
        fprintf(stderr, "Y4U2V2 matrix-ceiling guard was never exercised\n");
        return 0;
    }
    return 1;
}

/* mr_y4u2v2_to_rgb24() is display.c's software fallback for a backend that
 * switched away from the P96 PIP overlay mid-session (switch_to_cgx_
 * fallback()) and so has no show_yuv422 of its own. Round-trip planar
 * 4:2:0 through mr_yuv420_to_y4u2v2() and compare against
 * mr_yuv420_to_rgb24() run directly on the same source planes - not for
 * bit-exactness (the encode side's 1..254 safety margin and the decode
 * side's own separate rounding both introduce a little drift - see
 * mr_yuv.h's own comment on mr_y4u2v2_to_rgb24()), but for a small,
 * bounded per-channel difference. A gross mismatch (chroma swapped/
 * dropped, wrong matrix) would blow well past this tolerance; ordinary
 * rounding/margin drift does not. Only exercisable for even widths,
 * matching the format's own pair-based constraint. */
static int check_y4u2v2_to_rgb24(void)
{
    enum { W = 8, H = 4, TOLERANCE = 5 };
    /* Sized exactly to the W/2 stride actually passed to every call below.
     * An earlier version of this test declared these with a stray "+1" on
     * both dimensions (copied from an unrelated padding convention
     * elsewhere in this file), mismatching the real per-row stride against
     * the W/2 argument passed to the functions under test - both
     * conversions below silently read one chroma sample off, consistently
     * on both sides, which an exact memcmp couldn't catch (the same wrong
     * bytes, read the same wrong way, on both sides). Caught only once a
     * range-rescale experiment made the two sides diverge for an unrelated
     * reason and the divergence was investigated by hand. */
    uint8_t y[H][W], u[H / 2][W / 2], v[H / 2][W / 2];
    uint8_t packed[H][W * 2], direct[H][W * 3], via_packed[H][W * 3];
    unsigned seed = 0x59345556U;
    int row, col, i, max_diff = 0, max_i = 0;

    for (row = 0; row < H; row++)
        for (col = 0; col < W; col++)
            /* Keep this round-trip test inside the RGB gamut.  Deliberate
             * highlight overflow is covered exhaustively by the separate
             * matrix-ceiling check above. */
            y[row][col] = (uint8_t)(32 + (next_value(&seed) >> 24) % 168);
    for (row = 0; row < H / 2; row++)
        for (col = 0; col < W / 2; col++) {
            u[row][col] = (uint8_t)(112 + (next_value(&seed) >> 24) % 33);
            v[row][col] = (uint8_t)(112 + (next_value(&seed) >> 24) % 33);
        }

    mr_yuv420_to_rgb24(&direct[0][0], W * 3, &y[0][0], W, &u[0][0], W / 2,
                       &v[0][0], W / 2, W, H, NULL, NULL);
    if (!mr_yuv420_to_y4u2v2(&packed[0][0], W * 2, &y[0][0], W, &u[0][0],
                             W / 2, &v[0][0], W / 2, W, H, NULL, NULL)) {
        fprintf(stderr, "Y4U2V2 round-trip: pack step failed\n");
        return 0;
    }
    if (!mr_y4u2v2_to_rgb24(&via_packed[0][0], W * 3, &packed[0][0], W * 2,
                            W, H)) {
        fprintf(stderr, "Y4U2V2 round-trip: unpack step failed\n");
        return 0;
    }
    for (i = 0; i < (int)sizeof direct; i++) {
        int d = (int)((uint8_t *)direct)[i] - (int)((uint8_t *)via_packed)[i];
        if (d < 0) d = -d;
        if (d > max_diff) { max_diff = d; max_i = i; }
    }
    if (max_diff > TOLERANCE) {
        fprintf(stderr, "Y4U2V2 round-trip: byte %d differs by %d "
                        "(direct=%d via_packed=%d), exceeds tolerance %d\n",
                        max_i, max_diff, ((uint8_t *)direct)[max_i],
                        ((uint8_t *)via_packed)[max_i], TOLERANCE);
        return 0;
    }
    return 1;
}

static void count_service(void *opaque)
{
    (*(int *)opaque)++;
}

#if defined(MR_M68K_ASM)
/* Deliberately smashes exactly the registers an ordinary m68k C callee is
 * entitled to clobber (d0/d1/a0/a1 - caller-saved, never guaranteed
 * preserved), with an obviously-wrong sentinel rather than relying on
 * whatever a real service function's compiled code incidentally happens
 * to touch. This is what should have caught the real bug: every other
 * test in this project passed either NULL or a trivial one-line callback
 * as the service function, so the buggy code path (use of d0/a0/a1 after
 * the jsr, unprotected) was never actually exercised until real hardware
 * ran it with real audio servicing. */
static void clobbering_service(void *opaque)
{
    (*(int *)opaque)++;
    __asm__ volatile(
        "move.l #0xdeadbeef,%%d0\n\t"
        "move.l #0xdeadbeef,%%d1\n\t"
        "move.l #0xdeadbeef,%%a0\n\t"
        "move.l #0xdeadbeef,%%a1\n\t"
        : : : "d0", "d1", "a0", "a1");
}

static void check_yuv_service_clobber(void)
{
    enum { W = 40, H = 40 };
    int luma_x298[256], e_x409[256], d_xm100[256], e_xm208[256], d_x516[256];
    int ys = W + 3, cs = (W + 1) / 2 + 2, ds = W * 3 + 5;
    uint8_t yp[40 * 43], up[20 * 21], vp[20 * 21];
    uint8_t expected[40 * (40 * 3 + 5)], actual[40 * (40 * 3 + 5)];
    unsigned seed = 0x59555643U;
    int i, services = 0;

    for (i = 0; i < 256; i++) {
        int y = i - 16;
        int d = i - 128, e = i - 128;
        if (y < 0) y = 0;
        luma_x298[i] = 298 * y;
        e_x409[i] = 409 * e + 128;
        d_xm100[i] = -100 * d + 128;
        e_xm208[i] = -208 * e;
        d_x516[i] = 516 * d + 128;
    }
    for (i = 0; i < (int)sizeof yp; i++) yp[i] = (uint8_t)(next_value(&seed) >> 24);
    for (i = 0; i < (int)sizeof up; i++) up[i] = (uint8_t)(next_value(&seed) >> 24);
    for (i = 0; i < (int)sizeof vp; i++) vp[i] = (uint8_t)(next_value(&seed) >> 24);
    memset(expected, 0xa5, sizeof expected);
    memset(actual, 0xa5, sizeof actual);

    reference_convert(expected, ds, yp, ys, up, cs, vp, cs, W, H, 0);
    mr_yuv420_to_rgb24_m68k(actual, ds, yp, ys, up, cs, vp, cs, W, H,
                            clobbering_service, &services,
                            luma_x298, e_x409, d_xm100, e_xm208, d_x516);

    if (services != H / 16) {
        fprintf(stderr, "service count %d, expected %d\n", services, H / 16);
        exit(1);
    }
    if (memcmp(expected, actual, sizeof expected) != 0) {
        fprintf(stderr, "mr_yuv420_to_rgb24_m68k mismatch with a "
                        "register-clobbering service callback\n");
        exit(1);
    }
}
#endif

int main(void)
{
    static const int widths[] = { 1, 2, 3, 7, 16, 17, 31 };
    static const int heights[] = { 1, 2, 3, 5, 16, 17, 33 };
    unsigned i, j;
    uint8_t y[33], u[17], v[17], rgb[33 * 3];
    int services = 0;
    for (i = 0; i < sizeof widths / sizeof widths[0]; i++)
        for (j = 0; j < sizeof heights / sizeof heights[0]; j++)
            if (!run_case(widths[i], heights[j], 0x4d525956U + i * 31 + j))
                return 1;
    if (!check_y4u2v2()) return 1;
    if (!check_y4u2v2_full_range_margin()) return 1;
    if (!check_y4u2v2_matrix_ceiling()) return 1;
    if (!check_y4u2v2_to_rgb24()) return 1;
    memset(y, 16, sizeof y); memset(u, 128, sizeof u); memset(v, 128, sizeof v);
    mr_yuv420_to_rgb24(rgb, 3, y, 1, u, 1, v, 1, 1, 33,
                       count_service, &services);
    if (services != 2) {
        fprintf(stderr, "service count %d, expected 2\n", services);
        return 1;
    }
#if defined(MR_M68K_ASM)
    check_yuv_service_clobber();
#endif
    puts("YUV420 RGB/BGR and Y4U2V2 conversion: byte-exact");
    return 0;
}
