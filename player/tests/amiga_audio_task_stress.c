#include "../amiga/mr_audio.h"

#include <exec/types.h>
#include <proto/dos.h>
#include <stdio.h>

struct Device *TimerBase = NULL;

int main(void)
{
    static const unsigned waits_ms[] = { 20, 50, 100, 150, 250 };
    static short pcm[44100];
    static short stereo[44100 * 2];
    unsigned run, i;
    for (i = 0; i < sizeof pcm / sizeof pcm[0]; i++) {
        pcm[i] = (short)(((i / 50) & 1) ? 5000 : -5000);
        stereo[i * 2] = pcm[i];
        stereo[i * 2 + 1] = (short)-pcm[i];
    }
    for (run = 0; run < 100; run++) {
        int channels = run & 1 ? 2 : 1;
        mr_audio *audio = audio_open(44100, channels, 16);
        mr_audio_diagnostics diag;
        if (!audio) { printf("open failed at run %u\n", run); return 10; }
        audio_write_s16(audio, channels == 2 ? stereo : pcm, 44100, channels);
        audio_set_running(audio, 1);
        Delay((LONG)((waits_ms[run % 5] + 19) / 20));
        audio_diagnostics(audio, &diag);
        if (diag.fifo_samples && !diag.active_requests) {
            printf("starved run=%u wait=%u fifo=%lu\n", run,
                   waits_ms[run % 5], diag.fifo_samples);
            audio_close(audio);
            return 20;
        }
        /* Closing here deliberately exercises abort during/after every wait. */
        audio_close(audio);
    }
    printf("Paula task stress passed 100 mono/stereo open/block/close cycles\n");
    return 0;
}
