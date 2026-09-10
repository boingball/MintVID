/* Bit-exact check for the 68060 trap-free 32x32->64 signed multiply that
 * backs plm_audio_mul_q15 and plm_audio_synth_window when MR_CPU_68060 is
 * defined (see pl_mpeg.h and core/mr_cpu.h). This only builds on m68k - the
 * primitive is m68k inline asm - so it is m68k-cross/qemu-only, like the
 * other hand-asm MP2 checks, and is compiled with -mcpu=68060 so the guard
 * actually fires. */
#include <stdint.h>
#include <stdio.h>
#include "../core/mr_cpu.h"

#if !defined(MR_M68K_ASM) || !defined(MR_CPU_68060)
#error "build with -DMR_M68K_ASM=1 -mcpu=68060"
#endif

#define PL_MPEG_IMPLEMENTATION
#include "../core/pl_mpeg.h"

static uint32_t rng = 987654321u;
static uint32_t next(void) { rng = rng * 1664525u + 1013904223u; return rng; }

static int check(int32_t a, int32_t b)
{
    int64_t expected = (int64_t)a * (int64_t)b;
    int64_t actual = plm_audio_smul64_060(a, b);
    if (expected != actual) {
        printf("FAIL a=%d b=%d expected=%lld actual=%lld\n",
               a, b, (long long)expected, (long long)actual);
        return 0;
    }
    return 1;
}

int main(void)
{
    static const int32_t edges[] = {
        0, 1, -1, 2, -2, 3, -3,
        0x7fff, -0x7fff, 0x8000, -0x8000, 0xffff, -0xffff,
        0x10000, -0x10000, 0x10001, -0x10001,
        16404, 16563, 16890, 111661, 167154, 333906,
        INT32_MAX, INT32_MIN, INT32_MIN + 1, INT32_MAX - 1
    };
    unsigned n = sizeof edges / sizeof edges[0];

    for (unsigned i = 0; i < n; ++i)
        for (unsigned j = 0; j < n; ++j)
            if (!check(edges[i], edges[j])) return 1;

    for (int trial = 0; trial < 200000; ++trial) {
        int32_t a = (int32_t)next();
        int32_t b = (int32_t)next();
        if (!check(a, b)) return 2;
        /* Bias toward the small-positive-coefficient shape mul_q15 actually
         * uses in production (value can be anything, coefficient is one of
         * the fixed IDCT36 constants, all comfortably under 2^19). */
        if (!check(a, (int32_t)(next() % 400000))) return 3;
    }

    puts("MP2 68060 mul64: exhaustive edge pairs and 400000 randomized "
         "comparisons passed");
    return 0;
}
