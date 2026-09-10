/* Exact regression for the new 68060 hand kernel plm_audio_idct36_m68k_060
 * (core/plm_audio_idct36_m68k_060.S) against the same reference_idct36
 * oracle mr_mp2_idct_check.c/mr_mp2_idct_060_check.c use - see that oracle
 * (tests/mr_mp2_idct_reference.h) and CLAUDE.md's "MPEG-1/2 (libmpeg2)
 * notes" for the trap background. Like mr_mp2_idct_check.c, this does NOT
 * enable pl_mpeg's global MR_M68K_ASM dispatch - that would also pull in
 * every unrelated video/audio asm helper - it links only the one kernel
 * under test. */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

extern void plm_audio_idct36_m68k_060(int s[32][3], int ss, int32_t *d, int dp);

static int32_t reference_mul_q15(int32_t value, int32_t coefficient)
{
    int64_t product = (int64_t)value * coefficient;
    int64_t rounded = product < 0 ? -((-product + 16384) >> 15)
                                  : ((product + 16384) >> 15);
    return (int32_t)rounded;
}
#include "mr_mp2_idct_reference.h"

static uint32_t state = 1234567;
static uint32_t next(void) { state = state * 1664525u + 1013904223u; return state; }

int main(void)
{
    int input[32][3], saved[32][3];
    int32_t expected[1026], actual[1026];
    for (int trial = 0; trial < 100; ++trial) {
        for (int i = 0; i < 32; ++i)
            for (int j = 0; j < 3; ++j)
                input[i][j] = trial < 32 ? (i == trial ? 32767 : 0)
                    : trial < 64 ? (i == trial-32 ? -32768 : 0)
                    : (int)(next() & 2047) - 1024;
        memcpy(saved, input, sizeof input);
        for (int ss = 0; ss < 3; ++ss)
            for (int dp = 0; dp < 1024; dp += 64) {
                memset(expected, 0xa5, sizeof expected);
                memset(actual, 0xa5, sizeof actual);
                reference_idct36(input, ss, expected+1, dp);
                plm_audio_idct36_m68k_060(input, ss, actual+1, dp);
                if (memcmp(expected, actual, sizeof actual) ||
                    memcmp(saved, input, sizeof input)) {
                    printf("FAIL trial=%d ss=%d dp=%d\n",trial,ss,dp);
                    return 3;
                }
            }
    }
    puts("MP2 68060 asm IDCT: 4800 exact transform comparisons passed");
    return 0;
}
