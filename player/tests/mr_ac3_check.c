/*
 * AC-3 conformance: decode a container's AC-3 track through the real adapter
 * and compare the PCM against ffmpeg's decode of the same track, sample by
 * sample. ffmpeg is the oracle here exactly as it is for video in mr_decode.
 *
 * This test exists because the audio suite had no oracle at all: mr_audio_check
 * only ever asserted that a plausible number of non-silent samples came out,
 * and full-scale hash passes that as easily as music does. Two defects hid
 * behind it, both found the day this file was written:
 *
 *  - The host build compiled MintAMP's vendored Rockbox FFT (liba52's IMDCT
 *    calls ff_fft_calc_c) with -DAMIGA_M68K, which that code reads as
 *    "big-endian" and uses to pick the field order of the union MULT32 takes
 *    the high half of a 64-bit product through. On this little-endian host it
 *    read the low half instead, so every AC-3 IMDCT returned noise. See
 *    WMA_FFT_FLAGS in the Makefile. The Amiga build was never affected: there
 *    the define is true.
 *  - feed_ac3() shifted liba52's samples down by 12 where the level it asks
 *    for needs 13, so AC-3 played 6 dB hot and clipped on loud material. That
 *    one *was* affecting the Amiga.
 *
 * This checks the stereo decode. --audio-mono's fold is covered separately, by
 * mr_audio_check holding the mono run against the stereo one.
 *
 * The reference is raw signed 16-bit little-endian PCM at the track's own rate
 * and channel count, written by tests/gen_audio_assets.sh. The decoder
 * decimates for Paula (see compute_stride()), so the reference is decimated by
 * the same ratio here - dropping the same samples the decoder drops, not
 * resampling - and only then compared.
 *
 * The media argument is normally a container, demuxed as the player would. Two
 * things make that awkward on the m68k conformance run: it drags in the whole
 * demuxer and, through it, the H.264 tier. Built with MR_AC3_CHECK_NO_DEMUX it
 * instead takes a raw AC-3 elementary stream (0x0B77 frames) and feeds it to
 * the adapter directly, which needs nothing but the adapter itself - see
 * tests/run_m68k_check.sh, where this runs on a real big-endian target because
 * that is precisely the axis the FFT defect above turned on.
 */
#include "../core/mr_demux.h"
#include "../audio/mr_audio_decode.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_FRAMES 400000u
#define MAX_CHANNELS 2u

struct capture {
    int16_t      *pcm;          /* interleaved, `channels` per frame */
    unsigned long cap;          /* capacity in sample frames */
    unsigned long frames;
    unsigned      channels;
    int           channels_changed;
};

static void collect(void *user, const int16_t *pcm,
                    unsigned frames, unsigned channels)
{
    struct capture *c = (struct capture *)user;
    unsigned i, ch;

    /* More channels than the buffer holds, or a layout that changed
     * mid-decode: either would silently misalign every frame after it against
     * the reference, so refuse rather than compare nonsense. */
    if (!channels || channels > MAX_CHANNELS ||
        (c->channels && channels != c->channels)) {
        c->channels_changed = 1;
        return;
    }
    c->channels = channels;

    for (i = 0; i < frames && c->frames < c->cap; i++) {
        for (ch = 0; ch < channels; ch++)
            c->pcm[c->frames * channels + ch] = pcm[(size_t)i * channels + ch];
        c->frames++;
    }
}

static int16_t *load_reference(const char *path, unsigned long *count)
{
    FILE *f = fopen(path, "rb");
    long bytes;
    int16_t *data;
    unsigned char *raw;
    unsigned long i, n;

    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    bytes = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (bytes < 2) { fclose(f); return NULL; }
    raw = (unsigned char *)malloc((size_t)bytes);
    data = (int16_t *)malloc((size_t)bytes);
    if (!raw || !data) { free(raw); free(data); fclose(f); return NULL; }
    if (fread(raw, 1, (size_t)bytes, f) != (size_t)bytes) {
        free(raw); free(data); fclose(f); return NULL;
    }
    fclose(f);
    /* Explicit little-endian assembly, so the reference reads identically on a
     * big-endian host (this runs under qemu-m68k too). Assembled unsigned and
     * folded into range afterwards: shifting a negative value left is not
     * something to rely on. */
    n = (unsigned long)bytes / 2;
    for (i = 0; i < n; i++) {
        unsigned value = (unsigned)raw[i * 2] | ((unsigned)raw[i * 2 + 1] << 8);
        data[i] = (int16_t)(value < 0x8000u ? (int)value
                                            : (int)value - 0x10000);
    }
    free(raw);
    *count = n;
    return data;
}

/* Whole-file slurp, for the raw elementary-stream path. */
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
    if (!data || fread(data, 1, (size_t)bytes, f) != (size_t)bytes) {
        free(data); fclose(f); return NULL;
    }
    fclose(f);
    *len = (unsigned long)bytes;
    return data;
}

int main(int argc, char **argv)
{
#ifndef MR_AC3_CHECK_NO_DEMUX
    mr_demux *dx;
    const mr_audio_info *ai;
    mr_packet pkt;
#else
    mr_audio_info info;
    unsigned char *raw;
    unsigned long raw_len = 0;
#endif
    mr_audio_decoder *dec;
    struct capture cap;
    int16_t *ref;
    unsigned long ref_count = 0, ref_frames, i, compared;
    unsigned ref_rate, ref_channels, stride, decoded_rate;
    long worst = 0;
    double sum = 0.0;
    long worst_allowed, mean_allowed_x100;
    int rc = 0;

    if (argc != 5) {
        fprintf(stderr, "usage: mr_ac3_check <media> <ref.raw> <ref rate> "
                        "<ref channels>\n");
        return 2;
    }
    ref_rate = (unsigned)strtoul(argv[3], NULL, 10);
    ref_channels = (unsigned)strtoul(argv[4], NULL, 10);
    if (!ref_rate || !ref_channels) return 2;

    ref = load_reference(argv[2], &ref_count);
    if (!ref) { fprintf(stderr, "cannot read reference %s\n", argv[2]); return 2; }
    ref_frames = ref_count / ref_channels;

#ifndef MR_AC3_CHECK_NO_DEMUX
    dx = mr_demux_open_file(argv[1]);
    if (!dx) { fprintf(stderr, "cannot open %s\n", argv[1]); free(ref); return 2; }
    ai = mr_demux_audio(dx);
    if (ai->format_tag != MR_AUDIO_FORMAT_AC3) {
        fprintf(stderr, "not an AC-3 track: tag=0x%04x\n",
                (unsigned)ai->format_tag);
        mr_demux_close(dx); free(ref); return 1;
    }
    dec = mr_audio_decoder_open(ai, 0, 0);
#else
    raw = slurp(argv[1], &raw_len);
    if (!raw) { fprintf(stderr, "cannot read %s\n", argv[1]); free(ref); return 2; }
    if (raw_len < 2 || raw[0] != 0x0b || raw[1] != 0x77) {
        fprintf(stderr, "%s is not a raw AC-3 elementary stream\n", argv[1]);
        free(raw); free(ref); return 1;
    }
    /* feed_ac3() re-reads rate and layout from every frame's syncinfo; this
     * only has to say "AC-3, and here is the rate to size the Paula
     * decimation from". */
    memset(&info, 0, sizeof info);
    info.valid = 1;
    info.format_tag = MR_AUDIO_FORMAT_AC3;
    info.sample_rate = ref_rate;
    info.channels = (unsigned short)ref_channels;
    dec = mr_audio_decoder_open(&info, 0, 0);
#endif
    if (!dec) {
        fprintf(stderr, "AC-3 decoder rejected the stream\n");
#ifndef MR_AC3_CHECK_NO_DEMUX
        mr_demux_close(dx);
#else
        free(raw);
#endif
        free(ref); return 1;
    }

    memset(&cap, 0, sizeof cap);
    cap.cap = MAX_FRAMES;
    cap.pcm = (int16_t *)malloc(cap.cap * MAX_CHANNELS * sizeof(int16_t));
    if (!cap.pcm) {
        mr_audio_decoder_close(dec);
#ifndef MR_AC3_CHECK_NO_DEMUX
        mr_demux_close(dx);
#else
        free(raw);
#endif
        free(ref);
        return 2;
    }

#ifndef MR_AC3_CHECK_NO_DEMUX
    while (mr_demux_next_packet(dx, &pkt) == MR_OK) {
        if (!pkt.is_video && pkt.len &&
            mr_audio_decoder_feed(dec, pkt.data, pkt.len, collect, &cap) < 0) {
            fprintf(stderr, "fatal AC-3 decode error\n");
            rc = 1;
            break;
        }
    }
#else
    /* Deliberately in small pieces, so the adapter's frame-join buffer is
     * exercised rather than handed one tidy frame at a time. */
    {
        unsigned long pos;
        for (pos = 0; pos < raw_len; pos += 512) {
            uint32_t n = (uint32_t)(raw_len - pos < 512 ? raw_len - pos : 512);
            if (mr_audio_decoder_feed(dec, raw + pos, n, collect, &cap) < 0) {
                fprintf(stderr, "fatal AC-3 decode error\n");
                rc = 1;
                break;
            }
        }
    }
#endif
    decoded_rate = mr_audio_decoder_rate(dec);
    mr_audio_decoder_close(dec);
#ifndef MR_AC3_CHECK_NO_DEMUX
    mr_demux_close(dx);
#else
    free(raw);
#endif

    if (!rc) {
        if (!decoded_rate || ref_rate % decoded_rate) {
            fprintf(stderr, "reference rate %u is not a whole multiple of the "
                            "decoded rate %u\n", ref_rate, decoded_rate);
            rc = 1;
        } else if (cap.channels_changed) {
            fprintf(stderr, "the decoder changed channel layout mid-stream\n");
            rc = 1;
        } else if (cap.channels != ref_channels && ref_channels != 1) {
            /* The one layout difference this understands is liba52 handing
             * back two channels for a single-channel stream (acmod 1 comes out
             * as A52_DOLBY): there every decoded channel is checked against
             * the one reference channel, which also pins down that the second
             * channel is a real copy rather than silence. Anything else means
             * the fixture and the reference disagree. */
            fprintf(stderr, "decoded %u channels against a %u channel "
                            "reference\n", cap.channels, ref_channels);
            rc = 1;
        }
    }
    if (!rc) {
        stride = ref_rate / decoded_rate;
        /* The decoder keeps every stride'th sample frame from frame 0, so the
         * reference is thinned the same way rather than resampled. Every
         * decoded channel is compared, not just the first: a fold or an upmix
         * that got the second channel wrong is exactly the kind of defect this
         * file exists to catch. */
        compared = 0;
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
                compared++;
            }
        }
        /* liba52's fixed-point decode is not bit-identical to ffmpeg's float
         * one, but it is very close: this measures worst 2 / mean 0.50 on the
         * mono fixtures and worst 5 / 0.52 on the stereo one. The bar is set
         * far tighter than any of the failures above (which ran to tens of
         * thousands) and still leaves room for ordinary fixed-point drift. */
        worst_allowed = 64;
        mean_allowed_x100 = 200;
        printf("%s: %lu frames x %u ch at %u Hz vs %s (stride %u): "
               "worst |diff| %ld, mean %.2f\n", argv[1], cap.frames,
               cap.channels, decoded_rate, argv[2], stride, worst,
               compared ? sum / (double)compared : 0.0);
        if (!compared || cap.frames * 2 < ref_frames / stride) {
            fprintf(stderr, "decoded far less audio than the reference "
                            "(%lu vs %lu frames)\n",
                    cap.frames, ref_frames / stride);
            rc = 1;
        } else if (worst > worst_allowed ||
                   (compared &&
                    sum * 100.0 / (double)compared > (double)mean_allowed_x100)) {
            fprintf(stderr, "AC-3 PCM does not match the ffmpeg reference "
                            "(worst %ld > %ld, or mean above %.2f)\n",
                    worst, worst_allowed, mean_allowed_x100 / 100.0);
            rc = 1;
        }
    }

    free(cap.pcm);
    free(ref);
    return rc;
}
