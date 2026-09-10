/*
 * MintVID - MintAMP/Helix MP3 and AAC packet adapter.
 *
 * No codec implementation lives here.  The build supplies MintAMP's public
 * mp3dec/aacdec APIs; this file handles AVI packet joins, MP4 AAC raw-block
 * setup and Paula-friendly 2:1 output decimation above its ~28 kHz ceiling.
 */
#include "mr_audio_decode.h"
#include "mr_pcm.h"

#include "mp3dec.h"
#include "aacdec.h"
#include "config-a52.h"
#include "a52.h"
#include <stdio.h> /* FILE declaration used by pl_mpeg's public header */
#include "../core/pl_mpeg.h"
#include "../core/mr_latm.h"

#include <stdlib.h>
#include <string.h>

#define PCM_SHORTS_MAX 4096
#define PAULA_RATE_MAX 28000U
#define AC3_OUTPUT_SHIFT 13         /* see feed_ac3()'s level comment */

/* Base decimation stride (1 or 2, halving anything above Paula's ~28kHz
 * ceiling), doubled again under --audio-rate=low (mr_audio_decoder_open()'s
 * low_rate) for a further 2:1 reduction - 48kHz->12kHz, 44.1kHz->11.025kHz,
 * and a source already at/below 28kHz (e.g. 22.05kHz) drops to 11.025kHz
 * rather than being forced to a fixed absolute rate. */
static unsigned compute_stride(unsigned rate, int low_rate)
{
    unsigned stride = rate > PAULA_RATE_MAX ? 2 : 1;
    return low_rate ? stride * 2 : stride;
}

enum audio_kind {
    AUDIO_KIND_PCM,
    AUDIO_KIND_MP3,
    AUDIO_KIND_MP2,
    AUDIO_KIND_AAC_RAW,
    AUDIO_KIND_AAC_ADTS,
    AUDIO_KIND_AC3,
    AUDIO_KIND_AAC_LATM
};

struct mr_audio_decoder {
    enum audio_kind kind;
    HMP3Decoder mp3;
    HAACDecoder aac;
    plm_buffer_t *mp2_buffer;
    plm_audio_t *mp2;
    a52_state_t *ac3;
    mr_latm_config latm;
    unsigned source_rate;
    unsigned output_rate;
    unsigned channels;
    int he_aac;
    mr_audio_info pcm_info;
    unsigned stride;
    unsigned decim_phase;
    int low_rate;
    int mono;                       /* --audio-mono: emit one channel only */
    unsigned char *pending;
    size_t pending_len;
    size_t pending_cap;
    unsigned char *latm_au;
    size_t latm_au_cap;
    short pcm[PCM_SHORTS_MAX];
};

static int reserve_pending(mr_audio_decoder *d, size_t add)
{
    size_t need = d->pending_len + add;
    unsigned char *p;
    size_t cap;
    if (need <= d->pending_cap) return 1;
    cap = d->pending_cap ? d->pending_cap : 4096;
    while (cap < need) {
        if (cap > 1024U * 1024U) return 0;
        cap *= 2;
    }
    p = (unsigned char *)realloc(d->pending, cap);
    if (!p) return 0;
    d->pending = p;
    d->pending_cap = cap;
    return 1;
}

static void consume_pending(mr_audio_decoder *d, size_t n)
{
    if (n >= d->pending_len) {
        d->pending_len = 0;
        return;
    }
    memmove(d->pending, d->pending + n, d->pending_len - n);
    d->pending_len -= n;
}

/* Return a complete Layer III frame length, 0 for an invalid header. */
static unsigned mp3_frame_bytes(const unsigned char *p)
{
    static const unsigned br_mpeg1_l3[16] = {
        0, 32, 40, 48, 56, 64, 80, 96, 112,
        128, 160, 192, 224, 256, 320, 0
    };
    static const unsigned br_mpeg2_l3[16] = {
        0, 8, 16, 24, 32, 40, 48, 56, 64,
        80, 96, 112, 128, 144, 160, 0
    };
    static const unsigned sr_base[3] = { 44100, 48000, 32000 };
    unsigned version, layer, bri, sri, rate, br, pad;

    if (p[0] != 0xff || (p[1] & 0xe0) != 0xe0) return 0;
    version = (p[1] >> 3) & 3;
    layer = (p[1] >> 1) & 3;
    bri = p[2] >> 4;
    sri = (p[2] >> 2) & 3;
    pad = (p[2] >> 1) & 1;
    if (version == 1 || layer != 1 || bri == 0 || bri == 15 || sri == 3)
        return 0;                       /* reserved / not Layer III */

    rate = sr_base[sri];
    if (version == 2) rate /= 2;        /* MPEG-2 */
    else if (version == 0) rate /= 4;   /* MPEG-2.5 */
    br = (version == 3 ? br_mpeg1_l3[bri] : br_mpeg2_l3[bri]) * 1000U;
    return ((version == 3 ? 144U : 72U) * br) / rate + pad;
}

static unsigned aac_adts_frame_bytes(const unsigned char *p)
{
    if (p[0] != 0xff || (p[1] & 0xf6) != 0xf0) return 0;
    return ((unsigned)(p[3] & 3) << 11) |
           ((unsigned)p[4] << 3) | ((unsigned)p[5] >> 5);
}

/* `channels` is what d->pcm actually holds; the sink may get fewer (mono mode
 * drops everything but the first channel of a stereo buffer, for codecs whose
 * own decoder could not be asked for one channel). `stride` is normally
 * d->stride (see emit_pcm() below), except for MP2: pl_mpeg's polyphase
 * synthesis stage already decimates (see plm_audio_set_decim() in
 * core/pl_mpeg.h, set on d->mp2 at creation/reset), so feed_mp2() below
 * passes 1 here to avoid decimating that already-decimated output a second
 * time. d->decim_phase
 * is only ever advanced by the stride==1-bypassing path below, so it stays a
 * correct free-running phase across both uses - MP2's stride-1 calls never
 * touch it. */
static long emit_pcm_stride(mr_audio_decoder *d, unsigned total_shorts,
                            unsigned rate, unsigned channels, unsigned stride,
                            mr_audio_pcm_sink sink, void *user)
{
    unsigned frames, out, i, out_channels;
    if (!channels || channels > 2 || total_shorts > PCM_SHORTS_MAX)
        return -1;
    frames = total_shorts / channels;
    out_channels = (d->mono && channels > 1) ? 1 : channels;
    d->channels = out_channels;
    if (rate) d->source_rate = rate;

    if (stride == 1 && out_channels == channels) {
        if (sink && frames) sink(user, d->pcm, frames, channels);
        return (long)frames;
    }

    /* Compact in-place, preserving interleaving. d->decim_phase carries the
     * stride's position across calls, so a batch/packet boundary that lands
     * mid-stride doesn't restart the pattern at frame zero and drift the
     * effective output rate - block sizes are stride multiples for MP3/AAC/
     * AC-3, so phase is always 0 there, but raw PCM chunks (AVI/WAV) can be
     * any length. The destination index never runs ahead of the source, since
     * out <= i and out_channels <= channels. */
    out = 0;
    for (i = d->decim_phase; i < frames; i += stride) {
        unsigned ch;
        for (ch = 0; ch < out_channels; ch++)
            d->pcm[out * out_channels + ch] = d->pcm[i * channels + ch];
        out++;
    }
    d->decim_phase = i - frames;
    if (sink && out) sink(user, d->pcm, out, out_channels);
    return (long)out;
}

static long emit_pcm(mr_audio_decoder *d, unsigned total_shorts,
                     unsigned rate, unsigned channels,
                     mr_audio_pcm_sink sink, void *user)
{
    return emit_pcm_stride(d, total_shorts, rate, channels, d->stride,
                           sink, user);
}

static int parse_aac_asc(const mr_audio_info *info,
                         unsigned *object_type, unsigned *sample_rate,
                         unsigned *output_rate, unsigned *channels,
                         int *he_aac)
{
    static const unsigned rates[13] = {
        96000, 88200, 64000, 48000, 44100, 32000, 24000,
        22050, 16000, 12000, 11025, 8000, 7350
    };
    uint32_t bits;
    unsigned sf_index, extension_sf_index, extension_object_type;
    if (info->config_len < 2) return 0;
    bits = ((uint32_t)info->config[0] << 16) |
           ((uint32_t)info->config[1] << 8) |
           (info->config_len >= 3 ? info->config[2] : 0);
    *object_type = (bits >> 19) & 0x1f;
    sf_index = (bits >> 15) & 0x0f;
    *channels = (bits >> 11) & 0x0f;
    if (*object_type == 31 || sf_index == 15) {
        /* Extended object types / explicit-frequency ASC are valid MPEG-4,
         * but this fixed-point LC path intentionally rejects them cleanly. */
        return 0;
    }
    /* channelConfiguration 0 defers the channel count to an in-band program
     * config element; the container's audio entry already carries it (e.g. MOV
     * stsd), so fall back to that instead of rejecting the stream. */
    if (*channels == 0) *channels = info->channels;
    if (sf_index >= 13 || *channels < 1 || *channels > 2) return 0;
    *sample_rate = rates[sf_index];
    *output_rate = *sample_rate;
    *he_aac = 0;

    /* Explicit HE-AAC v1 signals SBR as audioObjectType 5. Its first sample
     * rate is the AAC-LC core rate; the following extension rate is the actual
     * PCM rate, followed by the underlying LC object type. For example this
     * YouTube ASC, 2b 92 08, is 22.05 kHz LC + SBR -> 44.1 kHz stereo. */
    if (*object_type == 5) {
        extension_sf_index = (bits >> 7) & 0x0f;
        extension_object_type = (bits >> 2) & 0x1f;
        if (extension_sf_index >= 13 || extension_object_type != 2)
            return 0;
        *object_type = extension_object_type;
        /* Paula cannot reproduce the 44.1/48 kHz SBR output directly. Decode
         * the embedded LC core at its native rate instead of spending 68k time
         * synthesising high frequencies only to decimate them afterwards. */
        *output_rate = *sample_rate;
        *he_aac = 1;
    }
    return 1;
}

/* MP3SetExperimentalHuffman()/MP3SetExperimentalPolyphase() route MintAMP's
 * MP3 decode through its asm-accelerated Huffman pair decode and its
 * trap-free polyphase synthesis (the 68060 build's MulShift68060 kernel,
 * or the 32-bit-accumulator fast path on 68030/040) instead of the fully
 * portable reference path. Both are compiled in by every default MintVID
 * Amiga build already (Makefile.amiga's FULL030/lowrate060 flag sets), but
 * nothing was ever calling these two setters, so every MP3 decode ran the
 * slow reference path regardless. Verified bit-exact against that reference
 * path on real m68k (qemu, 68030 and 68060) across every MP3 test fixture -
 * see CLAUDE.md - so this is a pure speedup, safe to enable unconditionally.
 * No effect on host builds, where AMIGA_FAST_POLYPHASE/AMIGA_M68K_ASM_HUFFMAN
 * are not defined and both setters are no-op stubs. gExperimentalPolyphase/
 * HuffmanEnabled are plain process-global statics (not per-decoder-instance
 * and never reset by MP3InitDecoder()), so one call after each init is
 * enough, but it costs nothing to keep this next to where the decoder is
 * actually created/reset. */
static void mp3_enable_verified_fast_paths(void)
{
    MP3SetExperimentalHuffman(1);
    MP3SetExperimentalPolyphase(1);
}

mr_audio_decoder *mr_audio_decoder_open(const mr_audio_info *info,
                                        int low_rate, int mono)
{
    mr_audio_decoder *d;
    if (!info || !info->valid) return NULL;
    if (info->format_tag != MR_AUDIO_FORMAT_PCM &&
        info->format_tag != MR_AUDIO_FORMAT_MP3 &&
        info->format_tag != MR_AUDIO_FORMAT_MP2 &&
        info->format_tag != MR_AUDIO_FORMAT_AAC &&
        info->format_tag != MR_AUDIO_FORMAT_AC3)
        return NULL;

    d = (mr_audio_decoder *)calloc(1, sizeof *d);
    if (!d) return NULL;
    d->low_rate = low_rate != 0;
    d->mono = mono != 0;
    d->source_rate = info->sample_rate;
    d->channels = d->mono ? 1 : info->channels;
    d->stride = compute_stride(info->sample_rate, d->low_rate);
    d->output_rate = info->sample_rate / d->stride;

    if (info->format_tag == MR_AUDIO_FORMAT_PCM) {
        if (!mr_pcm_supported(info)) goto fail;
        d->kind = AUDIO_KIND_PCM;
        d->pcm_info = *info;
    } else if (info->format_tag == MR_AUDIO_FORMAT_MP2) {
        d->kind = AUDIO_KIND_MP2;
        d->mp2_buffer = plm_buffer_create_with_capacity(8192);
        if (!d->mp2_buffer) goto fail;
        d->mp2 = plm_audio_create_with_buffer(d->mp2_buffer, 1);
        if (!d->mp2) goto fail;
        plm_audio_set_mono(d->mp2, d->mono);
        /* Fast MP2 decode: have pl_mpeg's polyphase synthesis stage do the
         * decimation (see plm_audio_set_decim() in core/pl_mpeg.h) instead of
         * decoding every lane and throwing most of it away in emit_pcm()'s
         * generic decimate() path afterwards - see feed_mp2(). d->stride is
         * computed by compute_stride() just above and is always 1, 2 or 4,
         * exactly the values plm_audio_set_decim() accepts (anything else
         * silently falls back to 1). */
        plm_audio_set_decim(d->mp2, (int)d->stride);
    } else if (info->format_tag == MR_AUDIO_FORMAT_MP3) {
        d->kind = AUDIO_KIND_MP3;
        d->mp3 = MP3InitDecoder();
        if (!d->mp3) goto fail;
        mp3_enable_verified_fast_paths();
        if (d->mono) {
            /* One channel out of Helix, and - on joint-stereo frames whose
             * side channel is only there to reconstruct L/R - its huffman,
             * dequant, IMDCT and synthesis skipped outright. */
            MP3SetOutputMono(d->mp3, 1);
            MP3SetMonoMSSideSkip(d->mp3, 1);
        }
    } else if (info->format_tag == MR_AUDIO_FORMAT_AC3) {
        d->kind = AUDIO_KIND_AC3;
        d->ac3 = a52_init(0);
        if (!d->ac3) goto fail;
        /* All layouts are downmixed to stereo, or to one channel in mono
         * mode - liba52 folds the channels in the frequency domain, so that
         * also spares it every IMDCT but the first. */
        d->channels = d->mono ? 1 : 2;
    } else {
        d->aac = AACInitDecoder();
        if (!d->aac) goto fail;
        if (info->config_len) {
            AACFrameInfo fi;
            unsigned object_type, rate, output_rate, channels;
            int he_aac;
            memset(&fi, 0, sizeof fi);
            if (!parse_aac_asc(info, &object_type, &rate, &output_rate,
                               &channels, &he_aac) ||
                object_type != 2)             /* AAC-LC */
                goto fail;
            fi.nChans = (int)channels;
            fi.sampRateCore = (int)rate;
            fi.profile = (int)object_type - 1;
            if (AACSetRawBlockParams(d->aac, 0, &fi) != 0) goto fail;
            d->kind = info->codec_tag == MR_FOURCC('L','A','T','M')
                    ? AUDIO_KIND_AAC_LATM : AUDIO_KIND_AAC_RAW;
            if (d->kind == AUDIO_KIND_AAC_LATM) {
                d->latm.object_type = object_type;
                d->latm.sample_rate = rate;
                d->latm.channels = channels;
                d->latm.asc_len = info->config_len;
                memcpy(d->latm.asc, info->config, info->config_len);
                d->latm.valid = 1;
            }
            d->he_aac = he_aac;
            d->source_rate = output_rate;
            d->channels = d->mono ? 1 : channels;
            d->stride = compute_stride(output_rate, d->low_rate);
            d->output_rate = output_rate / d->stride;
        } else {
            d->kind = AUDIO_KIND_AAC_ADTS;
        }
    }
    return d;

fail:
    mr_audio_decoder_close(d);
    return NULL;
}

static long feed_mp3(mr_audio_decoder *d, const uint8_t *data, uint32_t len,
                     mr_audio_pcm_sink sink, void *user)
{
    long produced = 0;
    if (!reserve_pending(d, len)) return -1;
    memcpy(d->pending + d->pending_len, data, len);
    d->pending_len += len;

    while (d->pending_len >= 4) {
        int off = MP3FindSyncWord(d->pending, (int)d->pending_len);
        unsigned frame_len;
        unsigned char *in;
        int left, err, chans;
        MP3FrameInfo fi;
        long got;
        if (off < 0) {
            if (d->pending_len > 3)
                consume_pending(d, d->pending_len - 3);
            break;
        }
        if (off) consume_pending(d, (size_t)off);
        if (d->pending_len < 4) break;
        frame_len = mp3_frame_bytes(d->pending);
        if (!frame_len) { consume_pending(d, 1); continue; }
        if (d->pending_len < frame_len) break;

        in = d->pending;
        left = (int)frame_len;
        err = MP3Decode(d->mp3, &in, &left, d->pcm, 0);
        consume_pending(d, frame_len);
        if (err == ERR_MP3_MAINDATA_UNDERFLOW) continue;
        if (err != ERR_MP3_NONE) continue;      /* resync at next frame */
        MP3GetLastFrameInfo(d->mp3, &fi);
        /* fi.nChans is the *stream's* channel count; under MP3SetOutputMono()
         * the decoder writes one channel (and scales fi.outputSamps to match),
         * so the interleave in d->pcm follows MP3GetOutputChannels(). */
        chans = MP3GetOutputChannels(d->mp3);
        if (chans <= 0) chans = fi.nChans;
        got = emit_pcm(d, (unsigned)fi.outputSamps, (unsigned)fi.samprate,
                       (unsigned)chans, sink, user);
        if (got < 0) return -1;
        produced += got;
    }
    return produced;
}

static long feed_aac_raw(mr_audio_decoder *d, const uint8_t *data, uint32_t len,
                         mr_audio_pcm_sink sink, void *user)
{
    unsigned char *in = (unsigned char *)(uintptr_t)data;
    int left = (int)len;
    AACFrameInfo fi;
    int err = AACDecode(d->aac, &in, &left, d->pcm);
    if (err != ERR_AAC_NONE) return 0;          /* bad AU: skip, keep playing */
    AACGetLastFrameInfo(d->aac, &fi);
    return emit_pcm(d, (unsigned)fi.outputSamps,
                    (unsigned)fi.sampRateOut, (unsigned)fi.nChans,
                    sink, user);
}

static long feed_mp2(mr_audio_decoder *d, const uint8_t *data, uint32_t len,
                     mr_audio_pcm_sink sink, void *user)
{
    long produced = 0;
    plm_samples_t *samples;
    if (plm_buffer_write(d->mp2_buffer,
                         (uint8_t *)(uintptr_t)data, len) != len)
        return -1;
    while ((samples = plm_audio_decode(d->mp2)) != NULL) {
        /* Mono mode has pl_mpeg synthesise and pack a single channel, and
         * plm_audio_set_decim(d->mp2, d->stride) (set at creation/reset) has
         * pl_mpeg's polyphase synthesis stage already emit only the lanes
         * that survive d->stride's decimation - samples->count is already
         * PLM_AUDIO_SAMPLES_PER_FRAME/decim. So this feeds emit_pcm_stride()
         * with an explicit stride of 1: the generic decimate-by-d->stride
         * path in emit_pcm() must NOT run again here, or it would drop a
         * further 1/stride of an already-decimated stream and both wreck the
         * output rate and double the reduction. */
        unsigned channels = (unsigned)plm_audio_get_channels(d->mp2);
        unsigned shorts = samples->count * channels;
        long got;
        memcpy(d->pcm, samples->interleaved, shorts * sizeof d->pcm[0]);
        got = emit_pcm_stride(d, shorts, d->source_rate, channels, 1,
                              sink, user);
        if (got < 0) return -1;
        produced += got;
    }
    return produced;
}

static long feed_aac_adts(mr_audio_decoder *d, const uint8_t *data, uint32_t len,
                          mr_audio_pcm_sink sink, void *user)
{
    long produced = 0;
    if (!reserve_pending(d, len)) return -1;
    memcpy(d->pending + d->pending_len, data, len);
    d->pending_len += len;

    while (d->pending_len >= 7) {
        int off = AACFindSyncWord(d->pending, (int)d->pending_len);
        unsigned frame_len;
        unsigned char *in;
        int left, err;
        AACFrameInfo fi;
        long got;
        if (off < 0) {
            if (d->pending_len > 6)
                consume_pending(d, d->pending_len - 6);
            break;
        }
        if (off) consume_pending(d, (size_t)off);
        if (d->pending_len < 7) break;
        frame_len = aac_adts_frame_bytes(d->pending);
        if (frame_len < 7) { consume_pending(d, 1); continue; }
        if (d->pending_len < frame_len) break;
        in = d->pending;
        left = (int)frame_len;
        err = AACDecode(d->aac, &in, &left, d->pcm);
        consume_pending(d, frame_len);
        if (err != ERR_AAC_NONE) continue;
        AACGetLastFrameInfo(d->aac, &fi);
        got = emit_pcm(d, (unsigned)fi.outputSamps,
                       (unsigned)fi.sampRateOut, (unsigned)fi.nChans,
                       sink, user);
        if (got < 0) return -1;
        produced += got;
    }
    return produced;
}

static long feed_ac3(mr_audio_decoder *d, const uint8_t *data, uint32_t len,
                     mr_audio_pcm_sink sink, void *user)
{
    long produced = 0;
    if (!reserve_pending(d, len)) return -1;
    memcpy(d->pending + d->pending_len, data, len);
    d->pending_len += len;

    while (d->pending_len >= 7) {
        size_t off = 0;
        int flags, rate, bitrate, frame_len, block;
        unsigned channels;
        level_t level;
        sample_t bias = 0;
        while (off + 1 < d->pending_len &&
               (d->pending[off] != 0x0b || d->pending[off + 1] != 0x77))
            off++;
        if (off) consume_pending(d, off);
        if (d->pending_len < 7) break;
        frame_len = a52_syncinfo(d->pending, &flags, &rate, &bitrate);
        if (frame_len <= 0) { consume_pending(d, 1); continue; }
        if ((size_t)frame_len > d->pending_len) break;

        /* LEVEL(x) in fixed liba52 is x << 26, so this level is 0.25 and a
         * full-scale sample arrives as 0.25 * SAMPLE(1.0) = 1<<28 - which is
         * 13 bits, not 12, above signed 16-bit full scale. AC3_OUTPUT_SHIFT
         * was 12 until tests/mr_ac3_check.c measured the output against
         * ffmpeg's: every AC-3 track played 6 dB hot, so anything but quiet
         * material spent its peaks clamped at the rails. That was equally true
         * on the Amiga - the shift has nothing to do with the host.
         *
         * The level is the same in mono mode: A52_ADJUST_LEVEL's stereo->mono
         * trim (-3 dB per channel, a52_downmix_init()) combined with
         * a52_downmix_coeff()'s own -3 dB makes the folded channel the (L+R)/2
         * average the Paula backend used to compute per sample, at the same
         * loudness as the stereo path. */
        level = 1 << 24;
        flags = (d->mono ? A52_MONO : A52_STEREO) | A52_ADJUST_LEVEL;
        if (a52_frame(d->ac3, d->pending, &flags, &level, bias)) {
            consume_pending(d, (size_t)frame_len);
            continue;
        }
        /* a52_frame() writes back the layout it accepted, which is what
         * a52_samples() will then hold. */
        channels = (flags & A52_CHANNEL_MASK) == A52_MONO ? 1 : 2;
        a52_dynrng(d->ac3, NULL, NULL);
        d->source_rate = (unsigned)rate;
        d->stride = compute_stride((unsigned)rate, d->low_rate);
        d->output_rate = (unsigned)rate / d->stride;
        for (block = 0; block < 6; block++) {
            sample_t *samples;
            unsigned i, out = 0;
            long got;
            if (a52_block(d->ac3)) break;
            samples = a52_samples(d->ac3);
            for (i = 0; i < 256; i++) {
                int32_t l = samples[i] >> AC3_OUTPUT_SHIFT;
                if (l < -32768) l = -32768; else if (l > 32767) l = 32767;
                d->pcm[out++] = (short)l;
                if (channels == 2) {
                    int32_t r = samples[256 + i] >> AC3_OUTPUT_SHIFT;
                    if (r < -32768) r = -32768; else if (r > 32767) r = 32767;
                    d->pcm[out++] = (short)r;
                }
            }
            got = emit_pcm(d, out, (unsigned)rate, channels, sink, user);
            if (got < 0) return -1;
            produced += got;
        }
        consume_pending(d, (size_t)frame_len);
    }
    return produced;
}

static int reserve_latm_au(mr_audio_decoder *d, size_t need)
{
    unsigned char *p;
    size_t cap = d->latm_au_cap ? d->latm_au_cap : 2048;
    if (need <= d->latm_au_cap) return 1;
    while (cap < need) {
        if (cap > 1024U * 1024U) return 0;
        cap *= 2;
    }
    p = (unsigned char *)realloc(d->latm_au, cap);
    if (!p) return 0;
    d->latm_au = p;
    d->latm_au_cap = cap;
    return 1;
}

static long feed_aac_latm(mr_audio_decoder *d, const uint8_t *data, uint32_t len,
                          mr_audio_pcm_sink sink, void *user)
{
    long produced = 0;
    if (!reserve_pending(d, len)) return -1;
    memcpy(d->pending + d->pending_len, data, len);
    d->pending_len += len;
    while (d->pending_len >= 3) {
        size_t off = 0, mux_len, frame_len, payload_bit, payload_len;
        long got;
        while (off + 1 < d->pending_len &&
               (d->pending[off] != 0x56 ||
                (d->pending[off + 1] & 0xe0) != 0xe0)) off++;
        if (off) consume_pending(d, off);
        if (d->pending_len < 3) break;
        mux_len = ((size_t)(d->pending[1] & 0x1f) << 8) | d->pending[2];
        frame_len = mux_len + 3;
        if (d->pending_len < frame_len) break;
        if (!mr_latm_payload(d->pending + 3, mux_len, &d->latm,
                             &payload_bit, &payload_len)) {
            consume_pending(d, frame_len);
            continue;
        }
        if (!reserve_latm_au(d, payload_len) ||
            !mr_latm_copy_payload(d->pending + 3, mux_len, payload_bit,
                                  d->latm_au, payload_len))
            return -1;
        got = feed_aac_raw(d, d->latm_au, (uint32_t)payload_len, sink, user);
        consume_pending(d, frame_len);
        if (got < 0) return -1;
        produced += got;
    }
    return produced;
}

long mr_audio_decoder_feed(mr_audio_decoder *d,
                           const uint8_t *data, uint32_t len,
                           mr_audio_pcm_sink sink, void *sink_user)
{
    if (!d || (!data && len)) return -1;
    if (!len) return 0;
    if (d->kind == AUDIO_KIND_PCM) {
        long produced = 0;
        size_t frame_bytes = d->pcm_info.block_align;
        size_t frames_left = len / frame_bytes;
        const uint8_t *p = data;
        while (frames_left) {
            size_t capacity = PCM_SHORTS_MAX / d->pcm_info.channels;
            size_t batch = frames_left < capacity ? frames_left : capacity;
            long decoded = mr_pcm_decode_s16(&d->pcm_info, p,
                                             batch * frame_bytes, d->pcm,
                                             capacity);
            long got;
            if (decoded < 0) return -1;
            got = emit_pcm(d, (unsigned)decoded * d->pcm_info.channels,
                           d->source_rate, d->pcm_info.channels,
                           sink, sink_user);
            if (got < 0) return -1;
            produced += got;
            p += batch * frame_bytes;
            frames_left -= batch;
        }
        return produced;
    }
    if (d->kind == AUDIO_KIND_MP3)
        return feed_mp3(d, data, len, sink, sink_user);
    if (d->kind == AUDIO_KIND_MP2)
        return feed_mp2(d, data, len, sink, sink_user);
    if (d->kind == AUDIO_KIND_AAC_RAW)
        return feed_aac_raw(d, data, len, sink, sink_user);
    if (d->kind == AUDIO_KIND_AAC_LATM)
        return feed_aac_latm(d, data, len, sink, sink_user);
    if (d->kind == AUDIO_KIND_AC3)
        return feed_ac3(d, data, len, sink, sink_user);
    return feed_aac_adts(d, data, len, sink, sink_user);
}

int mr_audio_decoder_reset(mr_audio_decoder *d)
{
    if (!d) return 0;
    d->pending_len = 0;
    d->decim_phase = 0;
    if (d->kind == AUDIO_KIND_PCM) return 1;
    if (d->kind == AUDIO_KIND_MP3) {
        MP3FreeDecoder(d->mp3);
        d->mp3 = MP3InitDecoder();
        if (d->mp3) mp3_enable_verified_fast_paths();
        if (d->mp3 && d->mono) {
            MP3SetOutputMono(d->mp3, 1);
            MP3SetMonoMSSideSkip(d->mp3, 1);
        }
        return d->mp3 != NULL;
    }
    if (d->kind == AUDIO_KIND_MP2) {
        plm_audio_destroy(d->mp2);
        d->mp2_buffer = plm_buffer_create_with_capacity(8192);
        d->mp2 = d->mp2_buffer
               ? plm_audio_create_with_buffer(d->mp2_buffer, 1) : NULL;
        if (d->mp2) {
            plm_audio_set_mono(d->mp2, d->mono);
            /* See the matching call in mr_audio_decoder_open(). */
            plm_audio_set_decim(d->mp2, (int)d->stride);
        }
        return d->mp2 != NULL;
    }
    if (d->kind == AUDIO_KIND_AC3) {
        a52_free(d->ac3);
        d->ac3 = a52_init(0);
        return d->ac3 != NULL;
    }
    return AACFlushCodec(d->aac) == 0;
}

unsigned mr_audio_decoder_rate(const mr_audio_decoder *d)
{
    return d ? d->output_rate : 0;
}

unsigned mr_audio_decoder_channels(const mr_audio_decoder *d)
{
    return d ? d->channels : 0;
}

const char *mr_audio_decoder_name(const mr_audio_decoder *d)
{
    if (!d) return "none";
    return d->kind == AUDIO_KIND_PCM
             ? (d->pcm_info.bits_per_sample == 8
                    ? (d->pcm_info.pcm_signed ? "PCM S8" : "PCM U8") :
                d->pcm_info.bits_per_sample == 16
                    ? (d->pcm_info.pcm_big_endian ? "PCM S16BE" : "PCM S16LE") :
                d->pcm_info.bits_per_sample == 24 ? "PCM S24LE" : "PCM S32LE")
         : d->kind == AUDIO_KIND_MP3 ? "MP3"
         : d->kind == AUDIO_KIND_MP2 ? "MP2"
         : d->kind == AUDIO_KIND_AC3 ? "AC-3"
         : d->kind == AUDIO_KIND_AAC_RAW
             ? (d->he_aac ? "HE-AAC/mp4a" : "AAC-LC/mp4a")
         : d->kind == AUDIO_KIND_AAC_LATM ? "AAC-LC/LATM"
         : "AAC-LC/ADTS";
}

void mr_audio_decoder_close(mr_audio_decoder *d)
{
    if (!d) return;
    if (d->mp3) MP3FreeDecoder(d->mp3);
    if (d->aac) AACFreeDecoder(d->aac);
    if (d->mp2) plm_audio_destroy(d->mp2);
    else if (d->mp2_buffer) plm_buffer_destroy(d->mp2_buffer);
    if (d->ac3) a52_free(d->ac3);
    free(d->pending);
    free(d->latm_au);
    free(d);
}
