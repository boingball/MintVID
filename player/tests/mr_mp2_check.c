/*
 * MP2 conformance for the MPEG-1 program-stream source: decode a .mpg through
 * mr_mpeg1 - the same path play_mpeg1() uses - and compare its PCM against
 * ffmpeg's decode of the same track, sample by sample.
 *
 * This exists because .mpg audio had no test at all. `make check` ran the
 * MPEG-1 fixture for video only, and the one MP2 fixture in `make check-audio`
 * is 44.1 kHz, which is MPEG-1 Layer II. Nothing ever fed the decoder an
 * MPEG-2 Layer II stream ("low sampling frequency": 16/22.05/24 kHz), and
 * pl_mpeg refused those headers outright - so every such file reported a
 * sample rate of 0, play_mpeg1() skipped opening Paula, and the clip played
 * silently. 22.05 kHz is the obvious rate to encode for a Paula target, which
 * is exactly why it was the case nobody had covered.
 *
 * The reference is raw signed 16-bit little-endian PCM at the track's own rate
 * and channel count, written by tests/gen_audio_assets.sh. mr_mpeg1_audio()
 * decimates for Paula the same way the adapter does, so the reference is
 * thinned by that ratio here - dropping the same sample frames the source
 * drops, not resampling - and only then compared.
 *
 * The bar is relative, not absolute: pl_mpeg's fixed-point Layer II synthesis
 * runs about 2% quieter than ffmpeg's float decode, on MPEG-1 and MPEG-2
 * streams alike, and that is the decoder's own accuracy rather than anything
 * this change introduced. What the thresholds below do catch is silence, a
 * dropped or duplicated frame (the waveform slips and the error goes to the
 * signal's own amplitude), and a wrong bit-allocation table (which decodes to
 * noise).
 */
#include "../core/mr_mpeg1.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_FRAMES 2000000u              /* sample frames captured           */
#define MAX_CHANNELS 2u

/* Every decoded sample frame of one run, interleaved. */
struct capture {
    int16_t      *pcm;
    unsigned long frames;
    unsigned      channels;
    unsigned      rate;
};

static unsigned char *slurp(const char *path, size_t *len)
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
    if (!data || fread(data, 1, (size_t)bytes, f) != (size_t)bytes) {
        free(data); fclose(f); return NULL;
    }
    fclose(f);
    *len = (size_t)bytes;
    return data;
}

/* Explicit little-endian assembly, so the reference reads identically on a
 * big-endian host - this can run under qemu-m68k. */
static int16_t *load_reference(const char *path, unsigned long *count)
{
    size_t bytes = 0;
    unsigned char *raw = slurp(path, &bytes);
    int16_t *data;
    unsigned long i, n;

    if (!raw) return NULL;
    if (bytes < 2) { free(raw); return NULL; }
    n = (unsigned long)(bytes / 2);
    data = (int16_t *)malloc(n * sizeof *data);
    if (!data) { free(raw); return NULL; }
    for (i = 0; i < n; i++) {
        unsigned value = (unsigned)raw[i * 2] | ((unsigned)raw[i * 2 + 1] << 8);
        data[i] = (int16_t)(value < 0x8000u ? (int)value
                                            : (int)value - 0x10000);
    }
    free(raw);
    *count = n;
    return data;
}

/* Decode the whole stream once and keep the PCM. Video frames are pulled too,
 * because that is what drives the demuxer forward - exactly as the player
 * does it. */
static int decode_run(const unsigned char *buf, size_t len, int low_rate,
                      int mono, struct capture *cap)
{
    mr_mpeg1 *m = mr_mpeg1_open(buf, len, low_rate, 0, mono);
    unsigned char *abuf;
    mr_frame fr;
    int n;

    memset(cap, 0, sizeof *cap);
    if (!m) {
        fprintf(stderr, "mr_mpeg1_open rejected the stream\n");
        return 0;
    }
    cap->rate = mr_mpeg1_samplerate(m);
    cap->channels = (unsigned)mr_mpeg1_channels(m);
    if (!cap->rate) {
        fprintf(stderr, "the source reports no audio track "
                        "(mr_mpeg1_samplerate() == 0)\n");
        mr_mpeg1_close(m);
        return 0;
    }
    if (!cap->channels || cap->channels > MAX_CHANNELS) {
        fprintf(stderr, "unusable channel count %u\n", cap->channels);
        mr_mpeg1_close(m);
        return 0;
    }
    cap->pcm = (int16_t *)malloc((size_t)MAX_FRAMES * MAX_CHANNELS *
                                 sizeof(int16_t));
    abuf = (unsigned char *)malloc(1152 * 4);
    if (!cap->pcm || !abuf) {
        free(cap->pcm); free(abuf); mr_mpeg1_close(m);
        return 0;
    }
    while (mr_mpeg1_next(m, &fr, NULL)) {
        while ((n = mr_mpeg1_audio(m, abuf)) > 0) {
            int i;
            unsigned ch;
            for (i = 0; i < n && cap->frames < MAX_FRAMES; i++) {
                for (ch = 0; ch < cap->channels; ch++) {
                    const unsigned char *p =
                        abuf + ((size_t)i * cap->channels + ch) * 2;
                    unsigned value = (unsigned)p[0] | ((unsigned)p[1] << 8);
                    cap->pcm[cap->frames * cap->channels + ch] =
                        (int16_t)(value < 0x8000u ? (int)value
                                                  : (int)value - 0x10000);
                }
                cap->frames++;
            }
        }
    }
    free(abuf);
    mr_mpeg1_close(m);
    return 1;
}

int main(int argc, char **argv)
{
    unsigned char *media;
    size_t media_len = 0;
    int16_t *ref;
    unsigned long ref_count = 0, ref_frames, i, compared = 0;
    unsigned ref_rate, ref_channels, stride;
    struct capture cap, mono_cap, low_cap;
    long worst = 0, ref_peak = 0;
    double sum = 0.0, ref_sum = 0.0, got_sum = 0.0;
    double mean, ref_mean, gain;
    int rc = 0;

    if (argc != 5) {
        fprintf(stderr, "usage: mr_mp2_check <file.mpg> <ref.raw> "
                        "<ref rate> <ref channels>\n");
        return 2;
    }
    ref_rate = (unsigned)strtoul(argv[3], NULL, 10);
    ref_channels = (unsigned)strtoul(argv[4], NULL, 10);
    if (!ref_rate || !ref_channels || ref_channels > MAX_CHANNELS) return 2;

    media = slurp(argv[1], &media_len);
    if (!media) { fprintf(stderr, "cannot read %s\n", argv[1]); return 2; }
    ref = load_reference(argv[2], &ref_count);
    if (!ref) {
        fprintf(stderr, "cannot read reference %s\n", argv[2]);
        free(media); return 2;
    }
    ref_frames = ref_count / ref_channels;

    if (!decode_run(media, media_len, 0, 0, &cap)) {
        free(media); free(ref); return 1;
    }
    if (!ref_rate || cap.rate == 0 || ref_rate % cap.rate) {
        fprintf(stderr, "reference rate %u is not a whole multiple of the "
                        "decoded rate %u\n", ref_rate, cap.rate);
        rc = 1;
    }

    if (!rc) {
        stride = ref_rate / cap.rate;
        for (i = 0; i * stride < ref_frames && i < cap.frames; i++) {
            unsigned ch;
            for (ch = 0; ch < cap.channels; ch++) {
                long got = cap.pcm[i * cap.channels + ch];
                long want = ref[i * stride * ref_channels +
                                (ref_channels == 1 ? 0 : ch)];
                long diff = got - want;
                if (diff < 0) diff = -diff;
                if (diff > worst) worst = diff;
                sum += (double)diff;
                ref_sum += (double)(want < 0 ? -want : want);
                got_sum += (double)(got < 0 ? -got : got);
                if ((want < 0 ? -want : want) > ref_peak)
                    ref_peak = want < 0 ? -want : want;
                compared++;
            }
        }
        mean = compared ? sum / (double)compared : 0.0;
        ref_mean = compared ? ref_sum / (double)compared : 0.0;
        gain = ref_sum > 0.0 ? got_sum / ref_sum : 0.0;
        printf("%s: %lu frames x %u ch at %u Hz vs %s (stride %u): "
               "worst |diff| %ld (ref peak %ld), mean %.2f (ref mean %.2f), "
               "gain %.4f\n", argv[1], cap.frames, cap.channels, cap.rate,
               argv[2], stride, worst, ref_peak, mean, ref_mean, gain);

        /* Measured on the committed fixture: mean 4% of the reference's own
         * mean amplitude, worst 4% of its peak, gain 0.98. The bars sit at
         * roughly twice that, which is still nowhere near what silence, a
         * slipped frame or a wrong allocation table produce. */
        if (!compared || cap.frames * 10 < (ref_frames / stride) * 9) {
            fprintf(stderr, "decoded far less audio than the reference "
                            "(%lu vs %lu frames)\n",
                    cap.frames, ref_frames / stride);
            rc = 1;
        } else if (ref_mean <= 0.0 || ref_peak <= 0) {
            fprintf(stderr, "the reference is silent\n");
            rc = 1;
        } else if (mean > ref_mean * 0.08) {
            fprintf(stderr, "MP2 PCM does not match the ffmpeg reference "
                            "(mean %.2f is over 8%% of %.2f)\n", mean, ref_mean);
            rc = 1;
        } else if ((double)worst > (double)ref_peak * 0.15) {
            fprintf(stderr, "MP2 PCM does not match the ffmpeg reference "
                            "(worst %ld is over 15%% of peak %ld)\n",
                    worst, ref_peak);
            rc = 1;
        } else if (gain < 0.90 || gain > 1.10) {
            fprintf(stderr, "MP2 output level is %.4f of the reference\n", gain);
            rc = 1;
        }
    }

    /* --audio-mono folds in the decoder (pl_mpeg synthesises one channel);
     * that must change the channel count and nothing else about the
     * timeline. --audio-rate=low doubles the Paula decimation instead. */
    if (!rc && decode_run(media, media_len, 0, 1, &mono_cap)) {
        if (mono_cap.channels != 1 || mono_cap.rate != cap.rate ||
            mono_cap.frames != cap.frames) {
            fprintf(stderr, "mono run: %lu frames x %u ch at %u Hz, "
                            "expected %lu x 1 at %u\n", mono_cap.frames,
                    mono_cap.channels, mono_cap.rate, cap.frames, cap.rate);
            rc = 1;
        }
        free(mono_cap.pcm);
    } else if (!rc) {
        rc = 1;
    }
    if (!rc && decode_run(media, media_len, 1, 0, &low_cap)) {
        if (low_cap.rate * 2 != cap.rate) {
            fprintf(stderr, "low-rate run: %u Hz against %u Hz\n",
                    low_cap.rate, cap.rate);
            rc = 1;
        }
        free(low_cap.pcm);
    } else if (!rc) {
        rc = 1;
    }

    free(cap.pcm);
    free(media);
    free(ref);
    return rc;
}
