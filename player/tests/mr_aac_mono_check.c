/*
 * AAC mono: with --audio-mono the adapter asks Helix for channel 0 only
 * (AACSetOutputMono(), see aac_apply_decim() in audio/mr_audio_decode.c), so
 * Helix skips TNS and the inverse transform for the right channel. The left
 * channel must not change by a single bit: every mono decode here is
 * compared sample for sample with the left channel of a normal stereo decode
 * of the same stream, at full rate and through both of Helix's decimated
 * transforms (Paula's 1/2 and --audio-rate=low's 1/4).
 *
 * test_aac_pns.aac (tests/gen_audio_assets.sh) carries noise in both
 * channels and is encoded with PNS on. Helix's noise generator is one
 * sequence shared by every channel, so skipping PNS on the right channel
 * would change the left channel's noise in later frames; this file makes
 * that show. test_aac_decim.aac adds short blocks and window switching.
 *
 * Takes raw ADTS, so it needs no demuxer and also runs on m68k
 * (tests/run_m68k_check.sh).
 */
#include "../audio/mr_audio_decode.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_FRAMES 400000ul

struct capture {
    int16_t      *pcm;          /* channel 0 only */
    unsigned long frames;
    unsigned      channels;     /* as delivered; 0 until the first call */
    int           bad;
};

static void collect(void *user, const int16_t *pcm, unsigned frames,
                    unsigned channels)
{
    struct capture *c = (struct capture *)user;
    unsigned i;
    if (!channels || (c->channels && c->channels != channels)) {
        c->bad = 1;
        return;
    }
    c->channels = channels;
    for (i = 0; i < frames && c->frames < MAX_FRAMES; i++)
        c->pcm[c->frames++] = pcm[(size_t)i * channels];
}

static unsigned char *slurp(const char *path, unsigned long *len)
{
    FILE *f = fopen(path, "rb");
    long bytes;
    unsigned char *data;
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    bytes = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (bytes <= 0) { fclose(f); return NULL; }
    data = (unsigned char *)malloc((size_t)bytes);
    if (data && fread(data, 1, (size_t)bytes, f) != (size_t)bytes) {
        free(data);
        data = NULL;
    }
    fclose(f);
    *len = (unsigned long)bytes;
    return data;
}

static int decode(const unsigned char *aac, unsigned long len, unsigned rate,
                  int low_rate, int mono, unsigned max_rate,
                  struct capture *cap, unsigned *out_rate)
{
    mr_audio_info info;
    mr_audio_decoder *dec;
    unsigned long pos = 0;

    memset(&info, 0, sizeof info);
    info.valid = 1;
    info.format_tag = MR_AUDIO_FORMAT_AAC;
    info.sample_rate = rate;
    info.channels = 2;
    dec = mr_audio_decoder_open(&info, low_rate, mono, max_rate);
    if (!dec) { fprintf(stderr, "AAC decoder rejected the stream\n"); return 0; }
    memset(cap, 0, sizeof *cap);
    cap->pcm = (int16_t *)malloc(sizeof(int16_t) * MAX_FRAMES);
    /* uneven chunks, as a demuxer would feed them */
    while (pos < len) {
        unsigned long chunk = len - pos < 777 ? len - pos : 777;
        if (mr_audio_decoder_feed(dec, aac + pos, (uint32_t)chunk,
                                  collect, cap) < 0) {
            fprintf(stderr, "feed failed at %lu\n", pos);
            mr_audio_decoder_close(dec);
            return 0;
        }
        pos += chunk;
    }
    *out_rate = mr_audio_decoder_rate(dec);
    mr_audio_decoder_close(dec);
    return !cap->bad;
}

static int check(const char *name, const unsigned char *aac, unsigned long len,
                 unsigned rate, int low_rate, unsigned max_rate)
{
    struct capture st, mo;
    unsigned st_rate = 0, mo_rate = 0;
    unsigned long i, diff = 0, first = 0;
    int ok = decode(aac, len, rate, low_rate, 0, max_rate, &st, &st_rate) &&
             decode(aac, len, rate, low_rate, 1, max_rate, &mo, &mo_rate);

    if (ok && st.channels != 2) {
        fprintf(stderr, "%s: stereo decode gave %u channels\n", name,
                st.channels);
        ok = 0;
    }
    if (ok && mo.channels != 1) {
        fprintf(stderr, "%s: mono decode gave %u channels\n", name,
                mo.channels);
        ok = 0;
    }
    if (ok && (st.frames == 0 || st.frames != mo.frames ||
               st_rate != mo_rate)) {
        fprintf(stderr, "%s: %lu stereo vs %lu mono frames, %u vs %u Hz\n",
                name, st.frames, mo.frames, st_rate, mo_rate);
        ok = 0;
    }
    if (ok) {
        for (i = 0; i < st.frames; i++)
            if (st.pcm[i] != mo.pcm[i] && !diff++) first = i;
        printf("  %s, %u Hz: %lu frames, %lu differ from the stereo left "
               "channel\n", name, st_rate, st.frames, diff);
        if (diff) {
            fprintf(stderr, "%s: first difference at frame %lu (%d vs %d)\n",
                    name, first, st.pcm[first], mo.pcm[first]);
            ok = 0;
        }
    }
    free(st.pcm);
    free(mo.pcm);
    return ok;
}

int main(int argc, char **argv)
{
    int ok = 1, a;

    if (argc < 2 || argc % 2 == 0) {
        fprintf(stderr,
                "usage: mr_aac_mono_check <stream.aac> <rate> [...]\n");
        return 2;
    }
    for (a = 1; a + 1 < argc; a += 2) {
        unsigned long len = 0;
        unsigned char *aac = slurp(argv[a], &len);
        unsigned rate = (unsigned)strtoul(argv[a + 1], NULL, 10);
        if (!aac) { fprintf(stderr, "cannot read %s\n", argv[a]); return 2; }
        /* full rate (AHI), Paula's 1/2 transform, --audio-rate=low's 1/4 */
        ok &= check(argv[a], aac, len, rate, 0, 0);
        ok &= check(argv[a], aac, len, rate, 0, MR_AUDIO_PAULA_RATE_MAX);
        ok &= check(argv[a], aac, len, rate, 1, MR_AUDIO_PAULA_RATE_MAX);
        free(aac);
    }
    if (!ok) { fprintf(stderr, "mr_aac_mono_check: FAILED\n"); return 1; }
    printf("mr_aac_mono_check: OK\n");
    return 0;
}
