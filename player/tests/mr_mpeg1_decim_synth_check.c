/* Direct, synthetic regression for plm_audio_synth_window_decim() (pl_mpeg.h -
 * see CLAUDE.md's "MPEG-1/2 (libmpeg2) notes" for the Fast MP2 mode this
 * backs) against plm_audio_synth_window() itself, independent of any real
 * MP2 bitstream or of mr_demux/mr_mpeg1.c: for every v_pos phase and a mix of
 * random and sign-extreme D/V history (mirroring mr_mp2_synth_check.c), the
 * decim=2 output must equal every even-indexed lane of the full decim=1
 * output, and decim=4 every 4th-indexed lane - proving the lane-independence
 * claim in pl_mpeg.h's comment beside plm_audio_synth_window_decim(), not
 * just that a full real-file decode happens to agree (see
 * tests/mr_mpeg1_decim_check.c for that end-to-end proof). Built at both
 * -m68030 (MR_M68K_ASM off, matching mr_mp2_synth_check.c's isolation - the
 * decim path never calls the .S kernels regardless) and -mcpu=68060 with
 * MR_M68K_ASM on, to also exercise plm_audio_smul64_060 inside this specific
 * function on real 68060 codegen. */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define PL_MPEG_IMPLEMENTATION
#include "../core/pl_mpeg.h"

static uint32_t rng = 246813579;
static uint32_t next(void) { rng = rng * 1664525u + 1013904223u; return rng; }

static int check_decim(const int32_t *d, const int32_t *v, int pos,
                       const int64_t *full, int decim, int trial)
{
    int64_t compact[32];
    int i;
    plm_audio_synth_window_decim(d, v, pos, compact, decim);
    for (i = 0; i < 32 / decim; i++) {
        if (compact[i] != full[i * decim]) {
            printf("FAIL trial=%d phase=%d decim=%d lane=%d: full=%lld compact=%lld\n",
                   trial, pos, decim, i,
                   (long long)full[i * decim], (long long)compact[i]);
            return 1;
        }
    }
    return 0;
}

int main(void)
{
    int32_t d[1024], v[1024];
    int64_t full[32];
    int trial, pos;

    for (trial = 0; trial < 64; ++trial) {
        int i;
        for (i = 0; i < 1024; ++i) {
            d[i] = (int32_t)(next() & 65535) - 32768;
            v[i] = (int32_t)(next() & 0x7fffffff);
            if (next() & 0x100) v[i] = -v[i];
            if (trial < 4) {
                d[i] = (trial & 1) ? -32768 : 32767;
                v[i] = (trial & 2) ? INT32_MIN : INT32_MAX;
            }
        }
        for (pos = 0; pos < 1024; pos += 64) {
            plm_audio_synth_window(d, v, pos, full);
            if (check_decim(d, v, pos, full, 2, trial)) return 1;
            if (check_decim(d, v, pos, full, 4, trial)) return 1;
        }
    }
    puts("MP2 decimated synthesis: 64 trials x 16 phases x {decim=2,4} bit-exact");
    return 0;
}
