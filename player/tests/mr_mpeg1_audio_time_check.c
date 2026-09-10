/* Proves MR_PL_MPEG_SKIP_AUDIO_TIME (pl_mpeg.h's plm_audio_decode() audio
 * time bookkeeping skip - see CLAUDE.md's "MPEG-1/2 (libmpeg2) notes") does
 * not change anything mr_mpeg1.c actually hands back to the player: this
 * source is built twice (Makefile: mr_mpeg1_audio_time_check_base without
 * the flag, mr_mpeg1_audio_time_check_skip with it), and both binaries
 * decode the same fixture through the exact call pattern play_mpeg1() uses
 * (mr_mpeg1_next()/mr_mpeg1_audio() in alternation, mirroring
 * tests/mr_mp2_check.c) and print a checksum of every video frame's pts_us
 * and pixels plus every decoded PCM byte. `make check` diffs the two
 * outputs and fails if they differ by even one byte.
 *
 * This is not a test of whether the bookkeeping is safe to skip in the
 * abstract (that's argued in the comment beside the #if in pl_mpeg.h) - it
 * is a direct check that skipping it changes nothing observable through the
 * one interface this player actually uses. */
#include "../core/mr_mpeg1.h"

#include <stdio.h>
#include <stdlib.h>

static uint32_t fnv1a(uint32_t hash, const void *data, size_t n)
{
    const unsigned char *p = data;
    for (size_t i = 0; i < n; ++i) {
        hash ^= p[i];
        hash *= 16777619u;
    }
    return hash;
}

int main(int argc, char **argv)
{
    if (argc != 2) {
        fprintf(stderr, "usage: %s <mpeg1-ps-with-mp2.mpg>\n", argv[0]);
        return 2;
    }

    FILE *f = fopen(argv[1], "rb");
    if (!f) { perror("fopen"); return 2; }
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *buf = malloc((size_t)len);
    if (!buf || fread(buf, 1, (size_t)len, f) != (size_t)len) {
        fprintf(stderr, "read failed\n");
        return 2;
    }
    fclose(f);

    mr_mpeg1 *m = mr_mpeg1_open(buf, (size_t)len, 0, 0, 0);
    if (!m) { fprintf(stderr, "mr_mpeg1_open rejected the stream\n"); return 2; }

    int channels = mr_mpeg1_channels(m);
    unsigned char abuf[1152 * 4];
    mr_frame fr;
    int64_t pts_us;
    uint32_t hash = 2166136261u;
    unsigned frames = 0, audio_calls = 0;
    long total_pcm_bytes = 0;

    while (mr_mpeg1_next(m, &fr, &pts_us)) {
        hash = fnv1a(hash, &pts_us, sizeof pts_us);
        hash = fnv1a(hash, fr.data, (size_t)fr.stride * (size_t)fr.height);
        frames++;
        int n;
        while ((n = mr_mpeg1_audio(m, abuf)) > 0) {
            size_t bytes = (size_t)n * (size_t)channels * 2u;
            hash = fnv1a(hash, abuf, bytes);
            total_pcm_bytes += (long)bytes;
            audio_calls++;
        }
    }

    mr_mpeg1_close(m);
    free(buf);

    printf("frames=%u audio_calls=%u total_pcm_bytes=%ld checksum=%08x\n",
           frames, audio_calls, total_pcm_bytes, hash);
    return 0;
}
