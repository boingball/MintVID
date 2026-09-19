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

static int check_y4u2v2(void)
{
    enum { W = 6, H = 3, YS = 8, CS = 4, DS = 15 };
    static const uint8_t y[H * YS] = {
        1, 2, 3, 4, 5, 6, 0xee, 0xee,
        7, 8, 9, 10, 11, 12, 0xee, 0xee,
        13, 14, 15, 16, 17, 18, 0xee, 0xee
    };
    static const uint8_t u[2 * CS] = { 21, 22, 23, 0xee, 31, 32, 33, 0xee };
    static const uint8_t v[2 * CS] = { 41, 42, 43, 0xee, 51, 52, 53, 0xee };
    static const uint8_t expected[H][W * 2] = {
        { 1, 21, 2, 41, 3, 22, 4, 42, 5, 23, 6, 43 },
        { 7, 21, 8, 41, 9, 22, 10, 42, 11, 23, 12, 43 },
        { 13, 31, 14, 51, 15, 32, 16, 52, 17, 33, 18, 53 }
    };
    uint8_t out[H * DS];
    int row;

    memset(out, 0xa5, sizeof out);
    if (!mr_yuv420_to_y4u2v2(out, DS, y, YS, u, CS, v, CS,
                             W, H, NULL, NULL)) {
        fprintf(stderr, "Y4U2V2 conversion rejected valid input\n");
        return 0;
    }
    for (row = 0; row < H; row++) {
        if (memcmp(out + row * DS, expected[row], W * 2) != 0 ||
            out[row * DS + W * 2] != 0xa5) {
            fprintf(stderr, "Y4U2V2 mismatch on row %d\n", row);
            return 0;
        }
    }
    if (mr_yuv420_to_y4u2v2(out, DS, y, YS, u, CS, v, CS,
                            W - 1, H, NULL, NULL)) {
        fprintf(stderr, "Y4U2V2 accepted an odd width\n");
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
