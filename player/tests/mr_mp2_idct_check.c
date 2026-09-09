/* Exact Q15 rounding and whole-transform regression for MP2 synthesis. */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <limits.h>
#define PL_MPEG_IMPLEMENTATION
#include "../core/pl_mpeg.h"

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
    static const int32_t coefficients[] = {
        16404,16563,16890,17401,18124,19102,20398,22112,24397,27504,
        31869,38320,48633,67429,111661,333906,16463,17121,18578,21195,
        25826,34756,56441,167154,16705,19705,29490,83982,17734,42813,23170
    };
    static const int32_t extremes[] = {INT32_MIN,INT32_MIN+1,-1000000,
        -65537,-65536,-65535,65535,65536,65537,1000000,INT32_MAX-1,INT32_MAX};
    int input[32][3], saved[32][3];
    int32_t expected[1026], actual[1026];
    for (unsigned c = 0; c < sizeof coefficients / sizeof coefficients[0]; ++c) {
        /* Includes every low-15-bit residue, either side of zero, and ties. */
        for (int32_t value = -32770; value <= 32770; ++value)
            if (plm_audio_mul_q15(value, coefficients[c]) !=
                reference_mul_q15(value, coefficients[c])) return 1;
        for (unsigned i = 0; i < sizeof extremes / sizeof extremes[0]; ++i)
            if (plm_audio_mul_q15(extremes[i], coefficients[c]) !=
                reference_mul_q15(extremes[i], coefficients[c])) return 2;
    }
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
                plm_audio_idct36(input, ss, actual+1, dp);
                if (memcmp(expected, actual, sizeof actual) ||
                    memcmp(saved, input, sizeof input)) {
                    printf("FAIL trial=%d ss=%d dp=%d\n",trial,ss,dp);
                    return 3;
                }
            }
    }
    puts("MP2 IDCT: rounding checks and 4800 exact transform comparisons passed");
    return 0;
}
