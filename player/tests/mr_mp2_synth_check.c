/* Exact regression against the original window walk, independent of PCM
 * clipping. Exercise every circular-buffer phase and both sign extremes, at
 * decim=1 (the original fixed-32-lane shape) and decim=2/4 (see
 * plm_audio_set_decim() in pl_mpeg.h - the ASM kernel takes decim directly
 * and, at decim=1, computes exactly what it always did; see the .S file's
 * own comment for why stepping the lane index is safe). */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
/* The m68k test calls the synthesis asm directly. Do not turn on pl_mpeg's
 * global MR_M68K_ASM dispatch here: that would make this focused test depend
 * on every other MPEG video/audio asm helper as well. */
#if defined(MR_M68K_ASM)
#define MR_TEST_M68K_SYNTH 1
#undef MR_M68K_ASM
#endif
#define PL_MPEG_IMPLEMENTATION
#include "../core/pl_mpeg.h"
#if defined(MR_TEST_M68K_SYNTH)
extern void plm_audio_synth_window_m68k(const int32_t *, const int32_t *, int, int64_t *, int);
#endif

static void reference(const int32_t *d, const int32_t *v, int pos, int64_t *u)
{
    int di = 512 - (pos >> 1), vi = (pos % 128) >> 1;
    memset(u, 0, 32 * sizeof(*u));
    while (vi < 1024) {
        for (int i = 0; i < 32; ++i)
            u[i] += (int64_t)d[di++] * v[vi++];
        vi += 96; di += 32;
    }
    di -= 480;
    vi = 1120 - vi;
    while (vi < 1024) {
        for (int i = 0; i < 32; ++i)
            u[i] += (int64_t)d[di++] * v[vi++];
        vi += 96; di += 32;
    }
}

static uint32_t rng = 1234567;
static uint32_t next(void) { rng = rng * 1664525u + 1013904223u; return rng; }

int main(void)
{
    int32_t d[1024], v[1024];
    int64_t expected[32], actual[34];
    static const int decims[] = { 1, 2, 4 };
    for (int trial = 0; trial < 64; ++trial) {
        for (int i = 0; i < 1024; ++i) {
            d[i] = (int32_t)(next() & 65535) - 32768;
            v[i] = (int32_t)(next() & 0x7fffffff);
            if (next() & 0x100) v[i] = -v[i];
            if (trial < 4) {
                d[i] = (trial & 1) ? -32768 : 32767;
                v[i] = (trial & 2) ? INT32_MIN : INT32_MAX;
            }
        }
        for (int pos = 0; pos < 1024; pos += 64) {
            reference(d, v, pos, expected);
            for (int di = 0; di < 3; ++di) {
                int decim = decims[di];
                int lanes = 32 / decim;
                actual[0] = actual[33] = INT64_C(0x123456789abcdef);
                memset(actual + 1, 0xa5, 32 * sizeof(int64_t));
#if defined(MR_TEST_M68K_SYNTH)
                plm_audio_synth_window_m68k(d, v, pos, actual + 1, decim);
#else
                plm_audio_synth_window_decim(d, v, pos, actual + 1, decim);
#endif
                for (int j = 0; j < lanes; j++) {
                    if (actual[1 + j] != expected[j * decim]) {
                        printf("FAIL trial=%d phase=%d decim=%d lane=%d\n",
                               trial, pos, decim, j);
                        return 1;
                    }
                }
                if (actual[0] != INT64_C(0x123456789abcdef) ||
                    actual[33] != INT64_C(0x123456789abcdef)) {
                    printf("FAIL trial=%d phase=%d decim=%d: guard clobbered\n",
                           trial, pos, decim);
                    return 1;
                }
            }
        }
    }
    puts("MP2 synthesis: 1024 exact comparisons passed (all 16 phases, decim=1/2/4)");
    return 0;
}
