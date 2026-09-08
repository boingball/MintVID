/*
 * Host smoke test for the MintAMP packet adapter.  It demuxes a real A/V
 * container, feeds every compressed audio packet through Helix and checks
 * that a plausible amount of non-silent PCM is produced. Runs each fixture
 * three times - once at the normal decoded rate, once with --audio-rate=low's
 * low_rate flag, once with --audio-mono's mono flag.
 *
 * The low-rate run's decoded_rate is checked against the normal run's, since
 * mr_audio_decoder_open()'s low_rate always doubles whatever stride normal
 * mode picked (see compute_stride() in audio/mr_audio_decode.c), so the two
 * must differ by exactly 2x.
 *
 * The mono run must produce the same number of sample *frames* at the same
 * rate - mono changes how many channels each frame carries, never the
 * timeline - and every buffer handed to the sink must be single-channel. That
 * is the part worth pinning down: MP3 and MP2 reach it by having their decoder
 * synthesise one channel (and MP3 reports the source channel count in
 * MP3GetLastFrameInfo(), not the output count, which is exactly how a mono
 * frame count would silently come out halved).
 */
#include "../core/mr_demux.h"
#include "../audio/mr_audio_decode.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct pcm_stats {
    unsigned long frames;
    unsigned long nonzero;
    unsigned      max_channels;
    /* The first CAPTURE_FRAMES sample frames, two shorts each (both channels,
     * or the single channel twice), so a mono run can be held against the
     * stereo run's left channel and against its (L+R)/2 average. */
    int16_t      *capture;
    unsigned long capture_cap;
    unsigned long captured;
};

static void count_pcm(void *user, const int16_t *pcm,
                      unsigned frames, unsigned channels)
{
    struct pcm_stats *s = (struct pcm_stats *)user;
    unsigned i, total = frames * channels;
    s->frames += frames;
    if (channels > s->max_channels) s->max_channels = channels;
    for (i = 0; i < total; i++)
        if (pcm[i] > 8 || pcm[i] < -8) s->nonzero++;
    if (s->capture) {
        for (i = 0; i < frames && s->captured < s->capture_cap; i++) {
            const int16_t *frame = pcm + (size_t)i * channels;
            s->capture[s->captured * 2] = frame[0];
            s->capture[s->captured * 2 + 1] = channels > 1 ? frame[1] : frame[0];
            s->captured++;
        }
    }
}

/* Decodes the whole file once under the given low_rate setting. Returns 0 on
 * success (and *decoded_rate_out is the decoder's effective output rate), or
 * a nonzero exit code on failure. */
static int run_once(const char *path, const char *kind, int low_rate, int mono,
                    unsigned *decoded_rate_out, unsigned long *frames_out,
                    struct pcm_stats *capture_out)
{
    mr_demux *dx;
    const mr_audio_info *ai;
    mr_audio_decoder *dec;
    mr_packet pkt;
    struct pcm_stats stats;
    unsigned long packets = 0;
    unsigned decoded_rate;

    memset(&stats, 0, sizeof stats);
    if (capture_out) {
        stats.capture = capture_out->capture;
        stats.capture_cap = capture_out->capture_cap;
    }
    dx = mr_demux_open_file(path);
    if (!dx) return 2;
    ai = mr_demux_audio(dx);
    if ((!strcmp(kind, "mp3") && ai->format_tag != MR_AUDIO_FORMAT_MP3) ||
        (!strcmp(kind, "mp2") && ai->format_tag != MR_AUDIO_FORMAT_MP2) ||
        ((!strcmp(kind, "aac") || !strcmp(kind, "latm")) &&
         ai->format_tag != MR_AUDIO_FORMAT_AAC) ||
        (!strcmp(kind, "latm") &&
         ai->codec_tag != MR_FOURCC('L','A','T','M')) ||
        (!strcmp(kind, "ac3") &&
         ai->format_tag != MR_AUDIO_FORMAT_AC3)) {
        fprintf(stderr, "wrong demuxed audio setup: tag=0x%04x config=%u\n",
                (unsigned)ai->format_tag, (unsigned)ai->config_len);
        mr_demux_close(dx); return 1;
    }
    dec = mr_audio_decoder_open(ai, low_rate, mono);
    if (!dec) {
        fprintf(stderr, "unsupported audio setup: tag=0x%04x config=%u\n",
                (unsigned)ai->format_tag, (unsigned)ai->config_len);
        mr_demux_close(dx); return 1;
    }
    while (mr_demux_next_packet(dx, &pkt) == MR_OK) {
        if (!pkt.is_video) {
            uint32_t pos = 0;
            /* AVI is allowed to split an MP3 frame across chunks. Exercise the
             * join buffer deterministically instead of relying on ffmpeg's
             * particular packet sizes. MP4 AAC must retain whole access units. */
            do {
                uint32_t n = pkt.len - pos;
                if (ai->format_tag == MR_AUDIO_FORMAT_MP3 && n > 37) n = 37;
                if (mr_audio_decoder_feed(dec, pkt.data + pos, n,
                                          count_pcm, &stats) < 0) {
                    fprintf(stderr, "fatal audio decode error\n");
                    mr_audio_decoder_close(dec);
                    mr_demux_close(dx); return 1;
                }
                pos += n;
            } while (pos < pkt.len);
            if (!pkt.len) {
                mr_audio_decoder_feed(dec, pkt.data, 0, count_pcm, &stats);
            }
            packets++;
        }
    }
    decoded_rate = mr_audio_decoder_rate(dec);
    printf("%s%s: %lu packets, %lu PCM frames at %u Hz, %lu nonzero samples, "
           "%u ch\n",
           mr_audio_decoder_name(dec), low_rate ? " (low)" : mono ? " (mono)" : "",
           packets, stats.frames, decoded_rate, stats.nonzero,
           stats.max_channels);
    if (mono && stats.max_channels != 1) {
        fprintf(stderr, "mono decode still produced %u channels\n",
                stats.max_channels);
        mr_audio_decoder_close(dec); mr_demux_close(dx); return 1;
    }
    if (mono && mr_audio_decoder_channels(dec) != 1) {
        fprintf(stderr, "mono decoder reports %u channels\n",
                mr_audio_decoder_channels(dec));
        mr_audio_decoder_close(dec); mr_demux_close(dx); return 1;
    }
    mr_audio_decoder_close(dec);
    mr_demux_close(dx);
    if (decoded_rate_out) *decoded_rate_out = decoded_rate;
    if (frames_out) *frames_out = stats.frames;
    if (capture_out) capture_out->captured = stats.captured;
    /* MP4 exposes one AAC access unit per packet; TS coalesces several ADTS
     * frames into each PES packet, so packet count is not a quality signal. */
    if (packets < 1 || !decoded_rate ||
        stats.frames < (unsigned long)decoded_rate * 17 / 10 ||
        stats.frames > (unsigned long)decoded_rate * 23 / 10 ||
        stats.nonzero < stats.frames / 2)
        return 1;
    return 0;
}

#define CAPTURE_FRAMES 65536u

int main(int argc, char **argv)
{
    unsigned rate_normal = 0, rate_low = 0, rate_mono = 0;
    unsigned long frames_normal = 0, frames_mono = 0;
    struct pcm_stats left, mono;
    unsigned long i, compare;
    long worst_left = 0, worst_mix = 0, tolerance;
    int rc;

    if (argc != 3) {
        fprintf(stderr,
                "usage: mr_audio_check <media> <mp3|mp2|aac|latm|ac3>\n");
        return 2;
    }
    memset(&left, 0, sizeof left);
    memset(&mono, 0, sizeof mono);
    left.capture = (int16_t *)malloc(CAPTURE_FRAMES * 2 * sizeof(int16_t));
    mono.capture = (int16_t *)malloc(CAPTURE_FRAMES * 2 * sizeof(int16_t));
    if (!left.capture || !mono.capture) {
        fprintf(stderr, "out of memory\n");
        free(left.capture); free(mono.capture);
        return 2;
    }
    left.capture_cap = mono.capture_cap = CAPTURE_FRAMES;

    rc = run_once(argv[1], argv[2], 0, 0, &rate_normal, &frames_normal, &left);
    if (!rc) rc = run_once(argv[1], argv[2], 1, 0, &rate_low, NULL, NULL);
    if (!rc) rc = run_once(argv[1], argv[2], 0, 1, &rate_mono, &frames_mono,
                           &mono);
    if (rc) { free(left.capture); free(mono.capture); return rc; }

    /* The low-rate run halves whatever stride normal mode picked, so its
     * output rate is the normal one divided by two - truncated, since
     * mr_audio_decoder_open() divides integers. An odd rate like 11025 comes
     * back as 5512, not 5512.5, so the two are compared with that truncation
     * allowed for rather than requiring an exact doubling. */
    if (rate_low != rate_normal / 2) {
        fprintf(stderr, "low-rate mismatch: normal=%u Hz low=%u Hz "
                        "(expected %u Hz)\n", rate_normal, rate_low,
                rate_normal / 2);
        free(left.capture); free(mono.capture);
        return 1;
    }
    /* Mono changes how many channels a sample frame carries, never the
     * timeline: same rate, same frame count. */
    if (rate_mono != rate_normal || frames_mono != frames_normal) {
        fprintf(stderr, "mono mismatch: normal=%u Hz/%lu frames "
                        "mono=%u Hz/%lu frames (expected identical)\n",
                rate_normal, frames_normal, rate_mono, frames_mono);
        free(left.capture); free(mono.capture);
        return 1;
    }

    /* And what comes out is one of the two folds a codec can hand us, not the
     * wrong channel, a half-rate stream or silence: MP2 and AAC keep the first
     * channel (pl_mpeg synthesises channel 0; Helix AAC has no mono mode, so
     * emit_pcm() drops the second channel), while MP3 and AC-3 hand back the
     * (L+R)/2 average their own decoders fold for free - MintAMP's
     * CollapseStereoToMono()/mid-channel reuse and liba52's A52_MONO. Either
     * is right; something else is not. The tolerance is there because a fold
     * done inside a fixed-point decoder is not bit-identical to averaging its
     * stereo output afterwards. */
    compare = left.captured < mono.captured ? left.captured : mono.captured;
    for (i = 0; i < compare; i++) {
        long l = left.capture[i * 2], r = left.capture[i * 2 + 1];
        long got = mono.capture[i * 2];
        long d_left = got - l, d_mix = got - (l + r) / 2;
        if (d_left < 0) d_left = -d_left;
        if (d_mix < 0) d_mix = -d_mix;
        if (d_left > worst_left) worst_left = d_left;
        if (d_mix > worst_mix) worst_mix = d_mix;
    }
    printf("mono fold: %lu samples, worst |diff| vs left %ld, vs (L+R)/2 %ld\n",
           compare, worst_left, worst_mix);
    free(left.capture);
    free(mono.capture);
    if (!compare) {
        fprintf(stderr, "no PCM captured for the mono comparison\n");
        return 1;
    }
    /* ~1.5% of full scale: comfortably tighter than the wrong channel or a
     * 3 dB level slip, comfortably looser than fixed-point fold noise. AC-3
     * was exempt from this while its decode did not resemble ffmpeg's at all;
     * with that fixed (see tests/mr_ac3_check.c) it is held to the same bar as
     * everything else. */
    tolerance = 512;
    if (worst_left > tolerance && worst_mix > tolerance) {
        fprintf(stderr, "mono output matches neither the left channel nor the "
                        "(L+R)/2 average (worst %ld / %ld, tolerance %ld)\n",
                worst_left, worst_mix, tolerance);
        return 1;
    }
    return 0;
}
