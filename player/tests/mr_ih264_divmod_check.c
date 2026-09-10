/* Exact check for mr_ih264_divmod_u32/mr_ih264_divmod_s32
 * (vendor/libavc_port/ih264_m68k_divmod.h) against the native '/'/'%'
 * operators - both edge cases and the actual "macroblock x/y from a
 * linear address" shape used at every real call site (mb_x = addr %
 * width; mb_y = addr / width). On host/non-68060 targets this is the
 * trivial fallback; on a real -mcpu=68060 cross build (see
 * tests/run_m68k_check.sh) it exercises the inline-asm DIVU.L/DIVS.L path
 * that exists specifically to avoid the forbidden extended-dividend
 * DIVSL.L/DIVUL.L GCC would otherwise synthesise for this pattern.
 *
 * The signed-variant section below (mr_ih264_divmod_s32) exists because
 * the unsigned-only version of this test suite passed while a real
 * regression shipped: ih264d_parse_slice.c's `ps_dec->u2_mbx = MOD(
 * u2_first_mb_in_slice - 1, ...)` promotes to a signed int and is -1
 * whenever a slice starts at macroblock 0 (the first slice of essentially
 * every picture) - a case this file's earlier "addr"/"width" random
 * ranges never generated, since they were deliberately restricted to
 * non-negative "realistic" values. Passing that -1 to the UWORD32-typed
 * unsigned helper reinterpreted it as 0xFFFFFFFF, producing a garbage
 * quotient/remainder that corrupted decode on essentially every frame
 * (confirmed on real WinUAE 68040 and 68060, both streaming and local
 * files - i.e. not 68060-specific, since the bug was in the call site's
 * signedness, not the asm path). Bit-exactness alone was never the gap -
 * covering the actual negative-dividend shape was. */
#include <stdint.h>
#include <stdio.h>
#include "../vendor/libavc_port/ih264_m68k_divmod.h"

static uint32_t rng = 88172645;
static uint32_t next(void) { rng = rng * 1664525u + 1013904223u; return rng; }

int main(void)
{
    int fails = 0;

    static const uint32_t dividend_edges[] = {
        0, 1, 2, 65534, 65535, 65536, 0x7fffffffu, 0xfffffffeu, 0xffffffffu
    };
    static const uint32_t divisor_edges[] = {1, 2, 3, 11, 384, 65535};

    for (unsigned i = 0; i < sizeof dividend_edges / sizeof dividend_edges[0]; ++i)
        for (unsigned j = 0; j < sizeof divisor_edges / sizeof divisor_edges[0]; ++j) {
            UWORD32 dividend = dividend_edges[i], divisor = divisor_edges[j];
            UWORD32 expected_rem = dividend % divisor;
            UWORD32 expected_q = dividend / divisor;
            UWORD32 actual_rem, actual_q;
            actual_q = mr_ih264_divmod_u32(dividend, divisor, &actual_rem);
            if (actual_q != expected_q || actual_rem != expected_rem) {
                printf("FAIL(edge) dividend=%u divisor=%u "
                       "expected q=%u r=%u actual q=%u r=%u\n",
                       dividend, divisor, expected_q, expected_rem,
                       actual_q, actual_rem);
                fails++;
            }
        }

    for (int t = 0; t < 500000; ++t) {
        UWORD32 dividend = next();
        UWORD32 divisor = (next() % 4096u) + 1u; /* realistic pic-width-in-mbs range */
        UWORD32 expected_rem = dividend % divisor;
        UWORD32 expected_q = dividend / divisor;
        UWORD32 actual_rem, actual_q;
        actual_q = mr_ih264_divmod_u32(dividend, divisor, &actual_rem);
        if (actual_q != expected_q || actual_rem != expected_rem) {
            printf("FAIL(rand) dividend=%u divisor=%u "
                   "expected q=%u r=%u actual q=%u r=%u\n",
                   dividend, divisor, expected_q, expected_rem,
                   actual_q, actual_rem);
            fails++;
            if (fails > 20) break;
        }
    }

    /* The real call shape: mb_x = addr % width; mb_y = addr / width, both
     * from one dividend - realistic picture widths (macroblocks), not the
     * full uint32 range. */
    for (int t = 0; t < 200000; ++t) {
        UWORD32 width = (next() % 512u) + 1u;
        UWORD32 addr = next() % 200000u;
        UWORD32 mb_y, mb_x;
        mb_x = addr % width;
        mb_y = addr / width;
        UWORD32 actual_mb_x;
        UWORD32 actual_mb_y = mr_ih264_divmod_u32(addr, width, &actual_mb_x);
        if (actual_mb_x != mb_x || actual_mb_y != mb_y) {
            printf("FAIL(mbxy) addr=%u width=%u expected x=%u y=%u actual x=%u y=%u\n",
                   addr, width, mb_x, mb_y, actual_mb_x, actual_mb_y);
            fails++;
            if (fails > 20) break;
        }
    }

    /* Signed variant: edges, including the exact regression shape
     * (dividend = -1, i.e. u2_first_mb_in_slice == 0). */
    static const int32_t s_dividend_edges[] = {
        0, 1, -1, 2, -2, 65534, -65534, 65535, -65535,
        0x7fffffff, -0x7fffffff, INT32_MIN
    };
    static const int32_t s_divisor_edges[] = {1, 2, 3, 11, 384, 65535};

    for (unsigned i = 0; i < sizeof s_dividend_edges / sizeof s_dividend_edges[0]; ++i)
        for (unsigned j = 0; j < sizeof s_divisor_edges / sizeof s_divisor_edges[0]; ++j) {
            WORD32 dividend = s_dividend_edges[i], divisor = s_divisor_edges[j];
            WORD32 expected_rem = dividend % divisor;
            WORD32 expected_q = dividend / divisor;
            WORD32 actual_rem, actual_q;
            actual_q = mr_ih264_divmod_s32(dividend, divisor, &actual_rem);
            if (actual_q != expected_q || actual_rem != expected_rem) {
                printf("FAIL(s_edge) dividend=%d divisor=%d "
                       "expected q=%d r=%d actual q=%d r=%d\n",
                       dividend, divisor, expected_q, expected_rem,
                       actual_q, actual_rem);
                fails++;
            }
        }

    for (int t = 0; t < 500000; ++t) {
        WORD32 dividend = (WORD32)next();
        WORD32 divisor = (WORD32)((next() % 4096u) + 1u);
        WORD32 expected_rem = dividend % divisor;
        WORD32 expected_q = dividend / divisor;
        WORD32 actual_rem, actual_q;
        actual_q = mr_ih264_divmod_s32(dividend, divisor, &actual_rem);
        if (actual_q != expected_q || actual_rem != expected_rem) {
            printf("FAIL(s_rand) dividend=%d divisor=%d "
                   "expected q=%d r=%d actual q=%d r=%d\n",
                   dividend, divisor, expected_q, expected_rem,
                   actual_q, actual_rem);
            fails++;
            if (fails > 20) break;
        }
    }

    /* The exact regression shape: ps_dec->u2_mbx/u2_mby computed from
     * `(WORD32)u2_first_mb_in_slice - 1`, over the realistic
     * first_mb_in_slice/width ranges - first_mb_in_slice == 0 (dividend
     * == -1) is the common, previously-broken case. */
    for (int t = 0; t < 200000; ++t) {
        WORD32 width = (WORD32)((next() % 512u) + 1u);
        WORD32 first_mb_in_slice = (WORD32)(next() % 65536u);
        WORD32 dividend = first_mb_in_slice - 1;
        WORD32 expected_mbx = dividend % width;
        WORD32 expected_mby = dividend / width;
        WORD32 actual_mbx;
        WORD32 actual_mby = mr_ih264_divmod_s32(dividend, width, &actual_mbx);
        if (actual_mbx != expected_mbx || actual_mby != expected_mby) {
            printf("FAIL(s_mbxy) first_mb_in_slice=%d width=%d "
                   "expected x=%d y=%d actual x=%d y=%d\n",
                   first_mb_in_slice, width, expected_mbx, expected_mby,
                   actual_mbx, actual_mby);
            fails++;
            if (fails > 20) break;
        }
    }

    if (fails) {
        printf("FAILED: %d mismatches\n", fails);
        return 1;
    }
    puts("mr_ih264_divmod_u32/s32: edges + 1000000 random + 400000 mb-x/y "
         "(incl. first_mb_in_slice==0) checks passed");
    return 0;
}
