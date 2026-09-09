/*
 * MintVID - pacing policy for the MPEG-1 program-stream player.
 * See mr_mpeg1_sched.h for why each rule is shaped the way it is.
 */
#include "mr_mpeg1_sched.h"

int mr_mpeg1_want_audio(unsigned long buffered_ms, int started, int pulls)
{
    int cap = started ? MPEG1_AUDIO_MAX_PULLS : MPEG1_AUDIO_PRIME_PULLS;
    if (pulls >= cap) return 0;
    return buffered_ms < MPEG1_AUDIO_CUSHION_MS;
}

int mr_mpeg1_drop_frame(unsigned long audio_elapsed_ms, unsigned long target_ms,
                        unsigned long period_ms, int drop_run)
{
    if (drop_run >= MPEG1_MAX_DROP_RUN) return 0;
    return audio_elapsed_ms > target_ms + period_ms;
}

int mr_mpeg1_skip_b_frames(unsigned long audio_elapsed_ms,
                           unsigned long next_target_ms,
                           unsigned long period_ms)
{
    return audio_elapsed_ms > next_target_ms + period_ms;
}
