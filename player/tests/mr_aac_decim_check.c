/*
 * AAC output decimation: the adapter's AAC path asks Helix to run its inverse
 * transform at 1/2 or 1/4 size (MintAMP's AACSetOutputDecimation(), see
 * aac_apply_decim() in audio/mr_audio_decode.c) instead of decoding at full
 * rate and dropping samples. That changes the PCM - by design, it band-limits
 * where dropping samples aliased - so it is checked against ffmpeg's own
 * full-rate decode of the same stream, resampled here to what an ideal
 * decimator would produce.
 *
 * The reduced-size transform yields the band-limited signal at input sample
 * d*m + (d-1)/2 (imdct.c explains why), so the reference is ffmpeg's PCM
 * through a Kaiser-windowed sinc low-pass at the new Nyquist, evaluated at
 * exactly those positions. Two figures are required:
 *
 *  - overall SNR, which drags in the band edge: the reduced transform is a
 *    brick wall per frame and the reference filter has a transition band, so
 *    this is a coarse bound. Dropping samples scores ~4 dB (24 kHz) and below
 *    0 dB (12 kHz) on this fixture; any scale, alignment or window error
 *    lands in the same place.
 *  - in-band SNR, both signals low-passed to 80% of the new Nyquist: this is
 *    what the decimation is supposed to preserve, and it is measured tightly.
 *
 * The fixture (tests/gen_audio_assets.sh) is 48 kHz stereo with a chirp
 * across each new Nyquist and sharp noise bursts, so every window sequence -
 * long, start, eight-short, stop - and both window shapes occur. It is encoded
 * without PNS, whose noise is random by design and would make ffmpeg a
 * useless sample-level oracle. Helix's full-rate decode of it matches ffmpeg's
 * to within 2 LSB.
 *
 * Takes a raw ADTS stream, so it needs no demuxer and also runs on m68k
 * (tests/run_m68k_check.sh).
 */
#include "../audio/mr_audio_decode.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI                        /* not in strict C99's <math.h> */
#define M_PI 3.14159265358979323846
#endif

#define MAX_FRAMES 200000ul

struct capture {
    int16_t      *pcm;
    unsigned long frames;
    unsigned      channels;
    int           bad;
};

static void collect(void *user, const int16_t *pcm, unsigned frames,
                    unsigned channels)
{
    struct capture *c = (struct capture *)user;
    unsigned i, ch;
    if (channels != 2 || (c->channels && c->channels != channels)) {
        c->bad = 1;
        return;
    }
    c->channels = channels;
    for (i = 0; i < frames && c->frames < MAX_FRAMES; i++) {
        for (ch = 0; ch < channels; ch++)
            c->pcm[c->frames * 2 + ch] = pcm[(size_t)i * channels + ch];
        c->frames++;
    }
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
    if (!data || fread(data, 1, (size_t)bytes, f) != (size_t)bytes) {
        free(data); fclose(f); return NULL;
    }
    fclose(f);
    *len = (unsigned long)bytes;
    return data;
}

static double bessel_i0(double x)
{
    double sum = 1.0, term = 1.0, k;
    for (k = 1.0; k < 50.0; k += 1.0) {
        term *= (x / (2.0 * k)) * (x / (2.0 * k));
        sum += term;
        if (term < 1e-12 * sum) break;
    }
    return sum;
}

/* Low-pass with cutoff pi*fc (fc = 1 is the input Nyquist) and unit DC gain,
 * at a real-valued offset t from the output position; zero outside +-half. */
static double lowpass_tap(double t, double fc, double half, double beta)
{
    double r = t / half, w, s;
    if (r <= -1.0 || r >= 1.0) return 0.0;
    w = bessel_i0(beta * sqrt(1.0 - r * r)) / bessel_i0(beta);
    s = fabs(t) < 1e-9 ? fc : sin(M_PI * fc * t) / (M_PI * t);
    return s * w;
}

/* out[m] = band-limited in at input position d*m + (d-1)/2, per channel. */
static void ideal_decimate(const double *in, unsigned long in_frames,
                           unsigned d, double *out, unsigned long out_frames)
{
    const double half = 32.0 * d, beta = 9.0;
    const double off = (d - 1) / 2.0;
    const long taps = (long)half + 2;
    double *h = (double *)malloc(sizeof(double) * (size_t)(2 * taps + 1));
    unsigned long m;
    long k;
    /* the fractional part of d*m + off - j is off's, for every m and j */
    for (k = -taps; k <= taps; k++)
        h[k + taps] = lowpass_tap((double)k + (off - floor(off)),
                                  1.0 / d, half, beta);
    for (m = 0; m < out_frames; m++) {
        long centre = (long)(d * m + (unsigned long)floor(off));
        double acc0 = 0.0, acc1 = 0.0;
        for (k = -taps; k <= taps; k++) {
            long j = centre - k;
            if (j < 0 || (unsigned long)j >= in_frames) continue;
            acc0 += in[j * 2] * h[k + taps];
            acc1 += in[j * 2 + 1] * h[k + taps];
        }
        out[m * 2] = acc0;
        out[m * 2 + 1] = acc1;
    }
    free(h);
}

/* Same-rate low-pass to 80% of Nyquist, for the in-band comparison. */
static void inband(const double *in, unsigned long frames, double *out)
{
    const long taps = 48;
    double h[2 * 48 + 1];
    unsigned long m;
    long k;
    for (k = -taps; k <= taps; k++)
        h[k + taps] = lowpass_tap((double)k, 0.8, (double)taps, 9.0);
    for (m = 0; m < frames; m++) {
        double a0 = 0.0, a1 = 0.0;
        for (k = -taps; k <= taps; k++) {
            long j = (long)m - k;
            if (j < 0 || (unsigned long)j >= frames) continue;
            a0 += in[j * 2] * h[k + taps];
            a1 += in[j * 2 + 1] * h[k + taps];
        }
        out[m * 2] = a0;
        out[m * 2 + 1] = a1;
    }
}

static double snr_db(const double *ref, const double *got,
                     unsigned long from, unsigned long to)
{
    double sig = 0.0, err = 0.0;
    unsigned long i;
    for (i = from * 2; i < to * 2; i++) {
        double e = got[i] - ref[i];
        sig += ref[i] * ref[i];
        err += e * e;
    }
    return err > 0.0 ? 10.0 * log10(sig / err) : 200.0;
}

static int run(const unsigned char *aac, unsigned long aac_len,
               const double *ref_full, unsigned long ref_frames,
               int low_rate, unsigned d,
               double min_overall, double min_inband)
{
    mr_audio_info info;
    mr_audio_decoder *dec;
    struct capture cap;
    unsigned long pos = 0, n, i, skip;
    double *got, *ideal, *got_lp, *ideal_lp, overall, in_band;
    unsigned rate;
    int ok = 1;

    memset(&info, 0, sizeof info);
    info.valid = 1;
    info.format_tag = MR_AUDIO_FORMAT_AAC;
    info.sample_rate = 48000;
    info.channels = 2;
    dec = mr_audio_decoder_open(&info, low_rate, 0);
    if (!dec) { fprintf(stderr, "AAC decoder rejected the stream\n"); return 0; }

    memset(&cap, 0, sizeof cap);
    cap.pcm = (int16_t *)malloc(sizeof(int16_t) * MAX_FRAMES * 2);
    /* feed in uneven chunks, as a demuxer would */
    while (pos < aac_len) {
        unsigned long chunk = aac_len - pos < 777 ? aac_len - pos : 777;
        if (mr_audio_decoder_feed(dec, aac + pos, (uint32_t)chunk,
                                  collect, &cap) < 0) {
            fprintf(stderr, "feed failed at %lu\n", pos);
            ok = 0;
            break;
        }
        pos += chunk;
    }
    rate = mr_audio_decoder_rate(dec);
    mr_audio_decoder_close(dec);
    if (!ok || cap.bad) { free(cap.pcm); return 0; }

    if (rate != 48000u / d) {
        fprintf(stderr, "d=%u: output rate %u, expected %u\n", d, rate,
                48000u / d);
        ok = 0;
    }
    if (cap.frames != ref_frames / d) {
        fprintf(stderr, "d=%u: %lu frames, expected %lu\n", d, cap.frames,
                ref_frames / d);
        ok = 0;
    }
    n = cap.frames < ref_frames / d ? cap.frames : ref_frames / d;
    got = (double *)malloc(sizeof(double) * n * 2);
    ideal = (double *)malloc(sizeof(double) * n * 2);
    got_lp = (double *)malloc(sizeof(double) * n * 2);
    ideal_lp = (double *)malloc(sizeof(double) * n * 2);
    for (i = 0; i < n * 2; i++) got[i] = cap.pcm[i];
    ideal_decimate(ref_full, ref_frames, d, ideal, n);
    inband(got, n, got_lp);
    inband(ideal, n, ideal_lp);
    /* the first frame is overlap-only start-up, and both filters run off the
     * ends: leave a frame's worth either side out */
    skip = 2048 / d;
    overall = snr_db(ideal, got, skip, n - skip);
    in_band = snr_db(ideal_lp, got_lp, skip, n - skip);
    printf("  %u Hz (1/%u): %lu frames, SNR %.1f dB overall (need %.0f), "
           "%.1f dB in-band (need %.0f)\n", rate, d, cap.frames,
           overall, min_overall, in_band, min_inband);
    if (overall < min_overall || in_band < min_inband) ok = 0;
    free(got); free(ideal); free(got_lp); free(ideal_lp); free(cap.pcm);
    return ok;
}

int main(int argc, char **argv)
{
    unsigned char *aac, *raw;
    unsigned long aac_len = 0, raw_len = 0, frames, i;
    double *ref;
    int ok;

    if (argc != 3) {
        fprintf(stderr, "usage: mr_aac_decim_check <stream.aac> <ref.raw>\n");
        return 2;
    }
    aac = slurp(argv[1], &aac_len);
    raw = slurp(argv[2], &raw_len);
    if (!aac || !raw) { fprintf(stderr, "cannot read inputs\n"); return 2; }
    /* ffmpeg's decode: s16le, 48 kHz stereo. Assembled explicitly so a
     * big-endian host reads it the same. */
    frames = raw_len / 4;
    ref = (double *)malloc(sizeof(double) * frames * 2);
    for (i = 0; i < frames * 2; i++) {
        unsigned v = (unsigned)raw[i * 2] | ((unsigned)raw[i * 2 + 1] << 8);
        ref[i] = v < 0x8000u ? (double)v : (double)v - 65536.0;
    }

    /* 48 kHz -> 24 kHz is the normal Paula path; --audio-rate=low halves it
     * again. Thresholds sit well clear of what dropping samples scores
     * (4 dB / -2 dB overall) and a few dB under what the reduced transform
     * measures (see the fixture's notes in gen_audio_assets.sh). */
    ok = run(aac, aac_len, ref, frames, 0, 2, 20.0, 60.0);
    ok &= run(aac, aac_len, ref, frames, 1, 4, 16.0, 48.0);
    free(ref); free(raw); free(aac);
    if (!ok) { fprintf(stderr, "mr_aac_decim_check: FAILED\n"); return 1; }
    printf("mr_aac_decim_check: OK\n");
    return 0;
}
