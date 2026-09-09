/* Exact regression against the original window walk, independent of PCM
 * clipping. Exercise every circular-buffer phase and both sign extremes. */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#define PL_MPEG_IMPLEMENTATION
#include "../core/pl_mpeg.h"
#if defined(MR_M68K_ASM)
extern void plm_audio_synth_window_m68k(const int32_t *, const int32_t *, int, int64_t *);
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
            actual[0] = actual[33] = INT64_C(0x123456789abcdef);
            memset(actual + 1, 0xa5, 32 * sizeof(int64_t));
#if defined(MR_M68K_ASM)
            plm_audio_synth_window_m68k(d, v, pos, actual + 1);
#else
            plm_audio_synth_window(d, v, pos, actual + 1);
#endif
            if (memcmp(expected, actual + 1, sizeof expected) ||
                actual[0] != INT64_C(0x123456789abcdef) ||
                actual[33] != INT64_C(0x123456789abcdef)) {
                printf("FAIL trial=%d phase=%d\n", trial, pos);
                return 1;
            }
        }
    }
    puts("MP2 synthesis: 1024 exact comparisons passed (all 16 phases)");
    return 0;
}
