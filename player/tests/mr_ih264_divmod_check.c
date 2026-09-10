/* Exact check for mr_ih264_divmod_u32 (vendor/libavc_port/ih264_m68k_divmod.h)
 * against the native '/'/'%' operators - both edge cases and the actual
 * "macroblock x/y from a linear address" shape used at every real call
 * site (mb_x = addr % width; mb_y = addr / width). On host/non-68060
 * targets this is the trivial fallback; on a real -mcpu=68060 cross build
 * (see tests/run_m68k_check.sh) it exercises the inline-asm DIVU.L path
 * that exists specifically to avoid the forbidden extended-dividend
 * DIVSL.L/DIVUL.L GCC would otherwise synthesise for this pattern. */
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

    if (fails) {
        printf("FAILED: %d mismatches\n", fails);
        return 1;
    }
    puts("mr_ih264_divmod_u32: edges + 500000 random + 200000 mb-x/y checks passed");
    return 0;
}
