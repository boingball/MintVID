/* Exact regression for the new 68060 hand kernel
 * plm_audio_synth_window_m68k_060 (core/plm_audio_synth_window_m68k_060.S)
 * against the same window-walk reference oracle mr_mp2_synth_check.c/
 * mr_mp2_synth_060_check.c use, at decim=1/2/4 (see plm_audio_set_decim()
 * in pl_mpeg.h and CLAUDE.md's "MPEG-1/2 (libmpeg2) notes"). */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

extern void plm_audio_synth_window_m68k_060(const int32_t *, const int32_t *,
                                            int, int64_t *, int);

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
                plm_audio_synth_window_m68k_060(d, v, pos, actual + 1, decim);
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
    puts("MP2 68060 asm synthesis: 1024 exact comparisons passed (all 16 phases, decim=1/2/4)");
    return 0;
}
