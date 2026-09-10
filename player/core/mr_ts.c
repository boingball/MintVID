/*
 * MintVID - MPEG-TS/M2TS demuxer.
 */
#include "mr_ts.h"
#include "mr_latm.h"
#include "mr_muldiv64.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>

#define TS_SYNC             0x47
#define TS_PID_NONE         0x1fff
#define TS_PROBE_LIMIT      (8UL * 1024 * 1024)
#define TS_PROBE_VIDEO_MAX  (1024UL * 1024)
#define TS_PROBE_AUDIO_MAX  (64UL * 1024)
/* How many PTS to sample before deriving the frame period, and the sample
 * ceiling. One delta is not enough: with B-frames the decode-order timestamps
 * are reordered, so the period only emerges from the minimum gap across a run
 * of frames long enough to include display-adjacent ones (see probe_stream). */
#define TS_PTS_STEP_SAMPLES 16
#define TS_PTS_SAMPLE_MAX   32
#define TS_PES_MAX          (16UL * 1024 * 1024)
#define TS_SERVICE_PACKETS  16

static unsigned long ticks_us(clock_t begin)
{
    return (unsigned long)((clock() - begin) * 1000000UL / CLOCKS_PER_SEC);
}

typedef struct {
    const uint8_t *p;
    size_t         bits;
    size_t         pos;
    int            bad;
} ts_bits;

static int reserve(uint8_t **buf, size_t *cap, size_t need, size_t limit)
{
    uint8_t *p;
    size_t n;
    if (need <= *cap) return 1;
    n = *cap ? *cap : 4096;
    while (n < need) {
        if (n >= limit) return 0;
        n *= 2;
        if (n > limit) n = limit;
    }
    p = (uint8_t *)realloc(*buf, n);
    if (!p) return 0;
    *buf = p;
    *cap = n;
    return 1;
}

static int ts_read_at(mr_ts *t, size_t off, void *dst, size_t len)
{
    if (!t->file_backed) {
        if (off > t->len || len > t->len - off) return 0;
        memcpy(dst, t->buf + off, len);
        return 1;
    } else {
        return mr_source_read_at(t->source, off, dst, len);
    }
}

static int detect_layout(const uint8_t *b, size_t len,
                         int *packet_size, int *sync_off)
{
    if (len >= 377 && b[0] == TS_SYNC &&
        b[188] == TS_SYNC && b[376] == TS_SYNC) {
        *packet_size = 188;
        *sync_off = 0;
        return 1;
    }
    if (len >= 389 && b[4] == TS_SYNC &&
        b[196] == TS_SYNC && b[388] == TS_SYNC) {
        *packet_size = 192;
        *sync_off = 4;
        return 1;
    }
    return 0;
}

/* Return payload and length for one 188-byte packet, or NULL if absent/bad. */
static const uint8_t *payload(const uint8_t *p, size_t *len, int *pusi,
                              uint16_t *pid)
{
    int afc, off = 4;
    if (p[0] != TS_SYNC || (p[1] & 0x80)) return NULL;
    *pusi = (p[1] & 0x40) != 0;
    *pid = (uint16_t)(((p[1] & 0x1f) << 8) | p[2]);
    afc = (p[3] >> 4) & 3;
    if (afc == 0 || afc == 2) return NULL;
    if (afc == 3) {
        off += 1 + p[4];
        if (off > 188) return NULL;
    }
    *len = (size_t)(188 - off);
    return p + off;
}

static void parse_pat(mr_ts *t, const uint8_t *p, size_t len, int pusi)
{
    size_t section_len, end, pos, skip;
    if (!pusi || !len) return;
    skip = 1 + (size_t)p[0];
    if (skip > len) return;
    p += skip;
    len -= skip;
    if (len < 12 || p[0] != 0x00) return;
    section_len = (size_t)(((p[1] & 0x0f) << 8) | p[2]);
    if (section_len + 3 > len || section_len < 9) return;
    end = 3 + section_len - 4;                   /* exclude CRC              */
    for (pos = 8; pos + 4 <= end; pos += 4) {
        uint16_t program = mr_rb16(p + pos);
        uint16_t pid = (uint16_t)(((p[pos + 2] & 0x1f) << 8) | p[pos + 3]);
        if (program) {
            t->pmt_pid = pid;
            return;
        }
    }
}

/* Names for the common PMT video stream_types we can identify but do not have a
 * decoder for. Returns NULL for audio/data/private types and for the video
 * types we *do* decode (MPEG-1/2, H.264) - callers only use this to explain a
 * failure. Values per ITU-T H.222.0 stream_type assignments and de-facto
 * registrations. */
const char *mr_ts_video_type_name(unsigned stream_type)
{
    switch (stream_type) {
    case 0x10: return "MPEG-4 Part 2";
    case 0x24:
    case 0x25: return "H.265/HEVC";
    case 0x33: return "H.266/VVC";
    case 0x42: return "AVS";
    case 0xd1: return "Dirac";
    case 0xea: return "VC-1";
    default:   return NULL;
    }
}

/* Audio stream_type names are retained even when MintVID cannot decode the
 * track yet. This makes IPTV failures useful codec-porting data instead of a
 * generic silent-audio result. Type 0x06 is intentionally described as private
 * PES: its registration descriptor, which we do not parse yet, identifies the
 * actual codec (often AC-3, E-AC-3 or another private format). */
const char *mr_ts_audio_type_name(unsigned stream_type)
{
    switch (stream_type) {
    case 0x03: return "MPEG-1 audio";
    case 0x04: return "MPEG-2 audio";
    case 0x0f: return "AAC (ADTS)";
    case 0x11: return "AAC (LATM)";
    case 0x06: return "private PES audio (descriptor required)";
    case 0x81: return "AC-3";
    case 0x82:
    case 0x8a: return "DTS";
    case 0x87: return "E-AC-3";
    default:   return NULL;
    }
}

static int descriptors_identify_ac3(const uint8_t *p, size_t len)
{
    size_t pos = 0;
    while (pos + 2 <= len) {
        unsigned tag = p[pos], n = p[pos + 1];
        if (pos + 2 + n > len) break;
        if (tag == 0x6a ||
            (tag == 0x05 && n >= 4 &&
             p[pos + 2] == 'A' && p[pos + 3] == 'C' &&
             p[pos + 4] == '-' && p[pos + 5] == '3'))
            return 1;
        pos += 2 + n;
    }
    return 0;
}

static void parse_pmt(mr_ts *t, const uint8_t *p, size_t len, int pusi)
{
    size_t section_len, end, pos, program_info_len, skip;
    if (!pusi || !len) return;
    skip = 1 + (size_t)p[0];
    if (skip > len) return;
    p += skip;
    len -= skip;
    if (len < 16 || p[0] != 0x02) return;
    section_len = (size_t)(((p[1] & 0x0f) << 8) | p[2]);
    if (section_len + 3 > len || section_len < 13) return;
    end = 3 + section_len - 4;
    program_info_len = (size_t)(((p[10] & 0x0f) << 8) | p[11]);
    pos = 12 + program_info_len;
    while (pos + 5 <= end) {
        uint8_t type = p[pos];
        uint16_t pid =
            (uint16_t)(((p[pos + 1] & 0x1f) << 8) | p[pos + 2]);
        size_t es_info_len =
            (size_t)(((p[pos + 3] & 0x0f) << 8) | p[pos + 4]);
        if (pos + 5 + es_info_len > end) break;
        if ((type == 0x01 || type == 0x02 || type == 0x1b) &&
            t->video_pid == TS_PID_NONE) {
            t->video_pid = pid;                  /* MPEG-1/2 or AVC/H.264    */
            t->video_type = type;
        } else if ((type == 0x03 || type == 0x04 || type == 0x06 ||
                    type == 0x0f || type == 0x11 || type == 0x81) &&
                   t->audio_pid == TS_PID_NONE) {
            t->audio_pid = pid;
            /* Blu-ray M2TS commonly labels ADTS AAC as private PES 0x06.
             * AC-3 on the same stream type is distinguished by its AC-3 or
             * registration descriptor; both are then confirmed by syncword
             * probing before the track becomes valid. */
            t->audio_type = type == 0x06 && descriptors_identify_ac3(
                                p + pos + 5, es_info_len) ? 0x81 : type;
        } else if (!t->audio_type && mr_ts_audio_type_name(type)) {
            /* Remember an unsupported audio track for diagnostics. Do not
             * select its PID: the packet path must not feed it to an AAC
             * decoder merely because the video track itself is supported. */
            t->audio_type = type;
        } else if (t->video_pid == TS_PID_NONE &&
                   !t->unsupported_video_type &&
                   mr_ts_video_type_name(type)) {
            /* A video track we have no decoder for (HEVC, MPEG-4, VC-1...).
             * Remember it so a failed open can name what it couldn't play
             * instead of a generic "unsupported container". */
            t->unsupported_video_type = type;
        }
        pos += 5 + es_info_len;
    }
}

/* Strip the PES header from the first TS payload of a PES packet. */
static const uint8_t *pes_payload(const uint8_t *p, size_t *len,
                                  uint64_t *pts, int *has_pts,
                                  size_t *expected)
{
    size_t hdr, packet_len;
    *has_pts = 0;
    *expected = 0;
    if (*len < 9 || p[0] != 0 || p[1] != 0 || p[2] != 1) return NULL;
    packet_len = mr_rb16(p + 4);
    hdr = 9 + p[8];
    if (hdr > *len) return NULL;
    if (packet_len) {
        size_t pes_header_after_length = 3 + (size_t)p[8];
        if (packet_len < pes_header_after_length) return NULL;
        *expected = packet_len - pes_header_after_length;
    }
    if ((p[7] & 0x80) && p[8] >= 5) {
        const uint8_t *q = p + 9;
        *pts = ((uint64_t)(q[0] & 0x0e) << 29) |
               ((uint64_t)q[1] << 22) |
               ((uint64_t)(q[2] & 0xfe) << 14) |
               ((uint64_t)q[3] << 7) |
               ((uint64_t)(q[4] & 0xfe) >> 1);
        *has_pts = 1;
    }
    *len -= hdr;
    return p + hdr;
}

static unsigned bits_get(ts_bits *b, unsigned n)
{
    unsigned v = 0, i;
    if (n > 32 || b->pos + n > b->bits) {
        b->bad = 1;
        return 0;
    }
    for (i = 0; i < n; i++) {
        v = (v << 1) | ((b->p[b->pos >> 3] >> (7 - (b->pos & 7))) & 1);
        b->pos++;
    }
    return v;
}

static unsigned bits_ue(ts_bits *b)
{
    unsigned zeros = 0;
    while (!b->bad && b->pos < b->bits && bits_get(b, 1) == 0) {
        if (++zeros > 30) {
            b->bad = 1;
            return 0;
        }
    }
    return zeros ? ((1u << zeros) - 1u + bits_get(b, zeros)) : 0;
}

static int bits_se(ts_bits *b)
{
    unsigned v = bits_ue(b);
    return (v & 1) ? (int)((v + 1) >> 1) : -(int)(v >> 1);
}

static void skip_scaling_list(ts_bits *b, int count)
{
    int last = 8, next = 8, j;
    for (j = 0; j < count; j++) {
        if (next) next = (last + bits_se(b) + 256) & 255;
        last = next ? next : last;
    }
}

static uint8_t *nal_rbsp(const uint8_t *nal, size_t len, size_t *out_len)
{
    uint8_t *r;
    size_t i, n = 0;
    int zeros = 0;
    if (len < 2) return NULL;
    r = (uint8_t *)malloc(len - 1);
    if (!r) return NULL;
    for (i = 1; i < len; i++) {                 /* skip NAL header          */
        uint8_t v = nal[i];
        if (zeros >= 2 && v == 3) {
            zeros = 0;
            continue;
        }
        r[n++] = v;
        zeros = v == 0 ? zeros + 1 : 0;
    }
    *out_len = n;
    return r;
}

static int parse_sps_geometry(const uint8_t *nal, size_t len,
                              int *width, int *height)
{
    uint8_t *rbsp;
    size_t rbsp_len;
    ts_bits b;
    unsigned profile, chroma = 1, frame_only;
    unsigned width_mbs, height_map, crop = 0;
    unsigned crop_l = 0, crop_r = 0, crop_t = 0, crop_b = 0;

    rbsp = nal_rbsp(nal, len, &rbsp_len);
    if (!rbsp) return 0;
    b.p = rbsp; b.bits = rbsp_len * 8; b.pos = 0; b.bad = 0;
    profile = bits_get(&b, 8);
    bits_get(&b, 8);                             /* constraints              */
    bits_get(&b, 8);                             /* level                    */
    bits_ue(&b);                                 /* sps id                   */
    if (profile == 100 || profile == 110 || profile == 122 ||
        profile == 244 || profile == 44 || profile == 83 ||
        profile == 86 || profile == 118 || profile == 128 ||
        profile == 138 || profile == 139 || profile == 134) {
        unsigned i, scaling;
        chroma = bits_ue(&b);
        if (chroma == 3) bits_get(&b, 1);
        bits_ue(&b);
        bits_ue(&b);
        bits_get(&b, 1);
        scaling = bits_get(&b, 1);
        if (scaling) {
            unsigned count = chroma == 3 ? 12 : 8;
            for (i = 0; i < count; i++)
                if (bits_get(&b, 1)) skip_scaling_list(&b, i < 6 ? 16 : 64);
        }
    }
    bits_ue(&b);                                 /* log2_max_frame_num       */
    {
        unsigned poc = bits_ue(&b);
        if (poc == 0) bits_ue(&b);
        else if (poc == 1) {
            unsigned i, n;
            bits_get(&b, 1);
            bits_se(&b);
            bits_se(&b);
            n = bits_ue(&b);
            for (i = 0; i < n; i++) bits_se(&b);
        }
    }
    bits_ue(&b);                                 /* max refs                 */
    bits_get(&b, 1);
    width_mbs = bits_ue(&b) + 1;
    height_map = bits_ue(&b) + 1;
    frame_only = bits_get(&b, 1);
    if (!frame_only) bits_get(&b, 1);
    bits_get(&b, 1);
    crop = bits_get(&b, 1);
    if (crop) {
        crop_l = bits_ue(&b); crop_r = bits_ue(&b);
        crop_t = bits_ue(&b); crop_b = bits_ue(&b);
    }
    if (!b.bad && width_mbs && height_map) {
        unsigned sub_w = (chroma == 1 || chroma == 2) ? 2 : 1;
        unsigned sub_h = chroma == 1 ? 2 : 1;
        unsigned unit_x = chroma ? sub_w : 1;
        unsigned unit_y = chroma ? sub_h * (2 - frame_only)
                                 : (2 - frame_only);
        unsigned w = width_mbs * 16;
        unsigned h = (2 - frame_only) * height_map * 16;
        unsigned cx = (crop_l + crop_r) * unit_x;
        unsigned cy = (crop_t + crop_b) * unit_y;
        if (cx < w && cy < h) {
            *width = (int)(w - cx);
            *height = (int)(h - cy);
            free(rbsp);
            return 1;
        }
    }
    free(rbsp);
    return 0;
}

/* Find the next Annex-B start code. Returns len when none remains. */
static size_t start_code(const uint8_t *p, size_t len, size_t from,
                         size_t *prefix)
{
    size_t i;
    for (i = from; i + 3 <= len; i++) {
        if (p[i] == 0 && p[i + 1] == 0) {
            if (p[i + 2] == 1) {
                *prefix = 3;
                return i;
            }
            if (i + 4 <= len && p[i + 2] == 0 && p[i + 3] == 1) {
                *prefix = 4;
                return i;
            }
        }
    }
    return len;
}

static int make_avcc_config(mr_ts *t, const uint8_t *p, size_t len)
{
    const uint8_t *sps = NULL, *pps = NULL;
    size_t sps_len = 0, pps_len = 0, pos = 0, prefix;
    while ((pos = start_code(p, len, pos, &prefix)) < len) {
        size_t begin = pos + prefix, next_prefix, end;
        size_t next = start_code(p, len, begin, &next_prefix);
        end = next;
        while (end > begin && p[end - 1] == 0) end--;
        if (end > begin) {
            unsigned type = p[begin] & 0x1f;
            if (type == 7 && !sps) { sps = p + begin; sps_len = end - begin; }
            if (type == 8 && !pps) { pps = p + begin; pps_len = end - begin; }
        }
        if (sps && pps) break;
        pos = next;
    }
    if (!sps || !pps || sps_len > 65535 || pps_len > 65535 ||
        sps_len < 4 || !parse_sps_geometry(sps, sps_len,
                                           &t->video.width,
                                           &t->video.height))
        return 0;
    t->config = (uint8_t *)malloc(11 + sps_len + pps_len);
    if (!t->config) return 0;
    t->config[0] = 1;
    t->config[1] = sps[1];
    t->config[2] = sps[2];
    t->config[3] = sps[3];
    t->config[4] = 0xff;                         /* four-byte NAL lengths    */
    t->config[5] = 0xe1;
    t->config[6] = (uint8_t)(sps_len >> 8);
    t->config[7] = (uint8_t)sps_len;
    memcpy(t->config + 8, sps, sps_len);
    t->config[8 + sps_len] = 1;
    t->config[9 + sps_len] = (uint8_t)(pps_len >> 8);
    t->config[10 + sps_len] = (uint8_t)pps_len;
    memcpy(t->config + 11 + sps_len, pps, pps_len);
    t->video.config = t->config;
    t->video.config_len = (uint32_t)(11 + sps_len + pps_len);
    return 1;
}

static int parse_mpeg_video_sequence(mr_ts *t, const uint8_t *p, size_t len)
{
    static const uint32_t rates[9] = {
        0, 24000, 24, 25, 30000, 30, 50, 60000, 60
    };
    static const uint32_t scales[9] = {
        0, 1001, 1, 1, 1001, 1, 1, 1001, 1
    };
    size_t i;
    for (i = 0; i + 8 <= len; i++) {
        unsigned code, width, height;
        if (p[i] != 0 || p[i + 1] != 0 || p[i + 2] != 1 ||
            p[i + 3] != 0xb3)
            continue;
        width = ((unsigned)p[i + 4] << 4) | (p[i + 5] >> 4);
        height = ((unsigned)(p[i + 5] & 0x0f) << 8) | p[i + 6];
        code = p[i + 7] & 0x0f;
        if (!width || !height || code == 0 || code > 8) return 0;
        t->video.width = (int)width;
        t->video.height = (int)height;
        t->video.rate = rates[code];
        t->video.scale = scales[code];
        return 1;
    }
    return 0;
}

static void parse_adts_info(mr_ts *t, const uint8_t *p, size_t len)
{
    static const uint32_t rates[13] = {
        96000, 88200, 64000, 48000, 44100, 32000, 24000,
        22050, 16000, 12000, 11025, 8000, 7350
    };
    size_t i;
    if (t->audio.valid) return;
    for (i = 0; i + 7 <= len; i++) {
        if (p[i] == 0xff && (p[i + 1] & 0xf6) == 0xf0) {
            unsigned sri = (p[i + 2] >> 2) & 0x0f;
            unsigned ch = ((p[i + 2] & 1) << 2) | (p[i + 3] >> 6);
            if (sri < 13 && ch >= 1 && ch <= 2) {
                t->audio.format_tag = MR_AUDIO_FORMAT_AAC;
                t->audio.sample_rate = rates[sri];
                t->audio.channels = (uint16_t)ch;
                t->audio.bits_per_sample = 16;
                t->audio.valid = 1;
                return;
            }
        }
    }
}

static void parse_latm_info(mr_ts *t, const uint8_t *p, size_t len)
{
    size_t i;
    mr_latm_config cfg;
    memset(&cfg, 0, sizeof cfg);
    if (t->audio.valid) return;
    for (i = 0; i + 3 <= len; i++) {
        size_t mux_len, payload_bit, payload_len;
        if (p[i] != 0x56 || (p[i + 1] & 0xe0) != 0xe0) continue;
        mux_len = ((size_t)(p[i + 1] & 0x1f) << 8) | p[i + 2];
        if (i + 3 + mux_len > len) return;
        if (!mr_latm_payload(p + i + 3, mux_len, &cfg,
                             &payload_bit, &payload_len))
            continue;
        t->audio.format_tag = MR_AUDIO_FORMAT_AAC;
        t->audio.codec_tag = MR_FOURCC('L','A','T','M');
        t->audio.sample_rate = cfg.sample_rate;
        t->audio.channels = (uint16_t)cfg.channels;
        t->audio.bits_per_sample = 16;
        t->audio.config_len = cfg.asc_len;
        memcpy(t->audio.config, cfg.asc, cfg.asc_len);
        t->audio.valid = 1;
        return;
    }
}

/* PMT stream types 0x03/0x04 identify an MPEG audio elementary stream, but
 * the actual layer and clock live in each frame header. MintVID's existing
 * pl_mpeg audio path decodes Layer II, so accept that layer only and leave
 * Layer I/III visible as an unsupported PMT track rather than feeding it to
 * the wrong decoder. MPEG-2.5 is not represented by stream type 0x04 and is
 * deliberately excluded here. */
static void parse_mp2_info(mr_ts *t, const uint8_t *p, size_t len)
{
    static const uint32_t rates[3] = { 44100, 48000, 32000 };
    size_t i;
    if (t->audio.valid) return;
    for (i = 0; i + 4 <= len; i++) {
        unsigned version, layer, sri, mode;
        if (p[i] != 0xff || (p[i + 1] & 0xe0) != 0xe0) continue;
        version = (p[i + 1] >> 3) & 3;
        layer = (p[i + 1] >> 1) & 3;
        sri = (p[i + 2] >> 2) & 3;
        mode = p[i + 3] >> 6;
        if ((version != 2 && version != 3) || layer != 2 || sri == 3)
            continue;
        t->audio.format_tag = MR_AUDIO_FORMAT_MP2;
        t->audio.sample_rate = rates[sri] / (version == 2 ? 2 : 1);
        t->audio.channels = (uint16_t)(mode == 3 ? 1 : 2);
        t->audio.bits_per_sample = 16;
        t->audio.valid = 1;
        return;
    }
}

static void parse_ac3_info(mr_ts *t, const uint8_t *p, size_t len)
{
    static const uint32_t rates[3] = { 48000, 44100, 32000 };
    size_t i;
    if (t->audio.valid) return;
    for (i = 0; i + 7 <= len; i++) {
        unsigned fscod, bsid, half;
        if (p[i] != 0x0b || p[i + 1] != 0x77) continue;
        fscod = p[i + 4] >> 6;
        bsid = p[i + 5] >> 3;
        if (fscod == 3 || bsid > 10) continue;
        half = bsid > 8 ? bsid - 8 : 0;
        t->audio.format_tag = MR_AUDIO_FORMAT_AC3;
        t->audio.codec_tag = MR_FOURCC('a','c','-','3');
        t->audio.sample_rate = rates[fscod] >> half;
        t->audio.channels = 2;          /* fixed decoder downmix target */
        t->audio.bits_per_sample = 16;
        t->audio.valid = 1;
        return;
    }
}

static mr_status probe_stream(mr_ts *t)
{
    uint8_t packet[192];
    uint8_t *video_probe = NULL;
    uint8_t *audio_probe = NULL;
    size_t video_len = 0, video_cap = 0;
    size_t audio_len = 0, audio_cap = 0;
    size_t pos, limit = t->len < TS_PROBE_LIMIT ? t->len : TS_PROBE_LIMIT;
    uint32_t pts_step = 0;
    uint64_t pts_samples[TS_PTS_SAMPLE_MAX];
    int nsamp = 0, i, j;

    for (pos = 0; pos + (size_t)t->packet_size <= limit;
         pos += (size_t)t->packet_size) {
        const uint8_t *p, *es;
        size_t n, es_len;
        int pusi, has_pts = 0;
        uint16_t pid;
        uint64_t pts = 0;
        size_t expected;
        if (!ts_read_at(t, pos, packet, (size_t)t->packet_size)) break;
        p = payload(packet + t->sync_off, &n, &pusi, &pid);
        if (!p) continue;
        if (pid == 0) parse_pat(t, p, n, pusi);
        else if (pid == t->pmt_pid) parse_pmt(t, p, n, pusi);
        else if (pid == t->video_pid) {
            es = p; es_len = n;
            if (pusi) {
                es = pes_payload(p, &es_len, &pts, &has_pts, &expected);
                if (!es) continue;
                /* Collect PTS samples; the frame period is derived from them
                 * below. Sampling here (not a running delta) is deliberate: PES
                 * arrive in decode order, so with B-frames the timestamps are
                 * reordered - and hierarchical (pyramid) B-frames mean no two
                 * decode-order-consecutive frames are one period apart either.
                 * The single delta this once used caught an I->P gap and reported
                 * fps/(1+Bframes), e.g. 6.25 for a 25 fps, 3-B stream. */
                if (has_pts && nsamp < TS_PTS_SAMPLE_MAX)
                    pts_samples[nsamp++] = pts;
            }
            if (video_len < TS_PROBE_VIDEO_MAX) {
                size_t add = es_len;
                if (add > TS_PROBE_VIDEO_MAX - video_len)
                    add = TS_PROBE_VIDEO_MAX - video_len;
                if (!reserve(&video_probe, &video_cap, video_len + add,
                             TS_PROBE_VIDEO_MAX)) {
                    free(video_probe); free(audio_probe);
                    return MR_ENOMEM;
                }
                memcpy(video_probe + video_len, es, add);
                video_len += add;
            }
        } else if (pid == t->audio_pid) {
            es = p; es_len = n;
            if (pusi) {
                es = pes_payload(p, &es_len, &pts, &has_pts, &expected);
                if (!es) continue;
            }
            if (audio_len < TS_PROBE_AUDIO_MAX) {
                size_t add = es_len;
                if (add > TS_PROBE_AUDIO_MAX - audio_len)
                    add = TS_PROBE_AUDIO_MAX - audio_len;
                if (!reserve(&audio_probe, &audio_cap, audio_len + add,
                             TS_PROBE_AUDIO_MAX)) {
                    free(video_probe); free(audio_probe);
                    return MR_ENOMEM;
                }
                memcpy(audio_probe + audio_len, es, add);
                audio_len += add;
            }
            if (t->audio_type == 0x03 || t->audio_type == 0x04)
                parse_mp2_info(t, audio_probe, audio_len);
            else if (t->audio_type == 0x81)
                parse_ac3_info(t, audio_probe, audio_len);
            else if (t->audio_type == 0x11)
                parse_latm_info(t, audio_probe, audio_len);
            else
                parse_adts_info(t, audio_probe, audio_len);
        }
        if (t->video_pid != TS_PID_NONE && video_len) {
            int video_ready =
                t->video_type == 0x1b
                    ? (t->config != NULL ||
                       make_avcc_config(t, video_probe, video_len))
                    : parse_mpeg_video_sequence(t, video_probe, video_len);
            int audio_ready =
                t->audio_pid == TS_PID_NONE || t->audio.valid;
            if (video_ready && audio_ready && nsamp >= TS_PTS_STEP_SAMPLES)
                break;
        }
    }
    if (t->video_type == 0x1b && !t->config && video_len)
        make_avcc_config(t, video_probe, video_len);
    else if ((t->video_type == 0x01 || t->video_type == 0x02) && video_len)
        parse_mpeg_video_sequence(t, video_probe, video_len);
    free(video_probe);
    free(audio_probe);

    /* Frame period = the smallest gap between any two sampled PTS. Presentation
     * timestamps are spaced one period apart in display order; decode-order
     * reordering (incl. pyramid B-frames) only permutes them, so the minimum
     * pairwise distance across a contiguous run of frames is exactly one period.
     * O(n^2) over a tiny bounded sample, once, at open. */
    for (i = 0; i < nsamp; i++)
        for (j = i + 1; j < nsamp; j++) {
            uint64_t a = pts_samples[i], b = pts_samples[j];
            uint64_t d = a > b ? a - b : b - a;
            if (d && d <= 0xffffffffUL && (!pts_step || (uint32_t)d < pts_step))
                pts_step = (uint32_t)d;
        }

    if (t->video_pid == TS_PID_NONE ||
        (t->video_type != 0x01 && t->video_type != 0x02 &&
         t->video_type != 0x1b) ||
        (t->video_type == 0x1b && !t->config) ||
        t->video.width <= 0 || t->video.height <= 0)
        return MR_EUNSUPPORTED;
    if (t->video_type == 0x1b)
        t->video.fourcc = MR_FOURCC('a','v','c','1');
    else
        t->video.fourcc = t->video_type == 0x02
                        ? MR_FOURCC('m','p','g','2')
                        : MR_FOURCC('m','p','g','1');
    if (t->video_type == 0x1b && pts_step >= 300) {
        t->video.rate = 90000;
        t->video.scale = pts_step;
    } else if (!t->video.rate || !t->video.scale) {
        t->video.rate = 25;
        t->video.scale = 1;
    }
    t->video.valid = 1;
    return MR_OK;
}

static void ts_init(mr_ts *t)
{
    memset(t, 0, sizeof *t);
    t->pmt_pid = TS_PID_NONE;
    t->video_pid = TS_PID_NONE;
    t->audio_pid = TS_PID_NONE;
}

static mr_status ts_open_common(mr_ts *t)
{
    uint8_t head[512];
    mr_status st;
    size_t n = t->len < sizeof head ? t->len : sizeof head;
    if (n < 389 || !ts_read_at(t, 0, head, n) ||
        !detect_layout(head, n, &t->packet_size, &t->sync_off))
        return MR_EFORMAT;
    st = probe_stream(t);
    if (st != MR_OK) return st;
    mr_ts_rewind(t);
    return MR_OK;
}

mr_status mr_ts_open(mr_ts *t, const uint8_t *buf, size_t len)
{
    ts_init(t);
    t->buf = buf;
    t->len = len;
    return ts_open_common(t);
}

mr_status mr_ts_open_source(mr_ts *t, mr_source *source, size_t len)
{
    ts_init(t);
    t->source = source;
    t->len = len;
    t->file_backed = 1;
    t->streaming = (len == MR_SOURCE_LEN_UNKNOWN);
    return ts_open_common(t);
}

static int pes_append(mr_ts_pes *p, const uint8_t *data, size_t len)
{
    if (!len) return 1;
    if (p->len > TS_PES_MAX - len ||
        !reserve(&p->data, &p->cap, p->len + len, TS_PES_MAX))
        return 0;
    memcpy(p->data + p->len, data, len);
    p->len += len;
    return 1;
}

static mr_status emit_pes(mr_ts *t, mr_ts_pes *p, int video, mr_packet *pkt)
{
    mr_status st;
    clock_t begin = t->timing_enabled ? clock() : 0;
    pkt->has_pts = p->has_pts;
    pkt->pts_us = p->has_pts
        ? mr_u64_div_u24(mr_u64_mul_u32(p->pts, 1000000u), 90000u) : 0;
    /* MPEG-TS carries H.264 in genuine Annex-B (byte-stream) format already -
     * this used to be rewritten into AVCC (4-byte length prefixes) here via
     * annexb_to_avcc(), purely so mr_h264_decode() could feed it through the
     * SAME uniform AVCC-in interface MOV/MP4's native avc1 sample format
     * needs. mr_h264_decode() immediately converted it straight back to
     * Annex-B (avcc_sample_to_annexb()) before decoding, since that is the
     * only format libavc's decode_annexb() actually accepts - two full
     * scan+copy passes over every NAL that cancelled out format-wise and
     * decoded the exact same bytes TS handed over in the first place.
     * is_annexb tells mr_h264_decode() to skip its own conversion and
     * decode straight from this PES payload - see mr_h264_set_input_annexb()
     * and its call site in mrplay.c. */
    if (video && t->video_type == 0x1b) {
        pkt->is_video = 1;
        pkt->is_annexb = 1;
        pkt->data = p->data;
        pkt->len = (uint32_t)p->len;
        st = p->len ? MR_OK : MR_EAGAIN;
        if (t->timing_enabled) t->timing.copy_us += ticks_us(begin);
    } else {
        pkt->is_video = video;
        pkt->data = p->data;
        pkt->len = (uint32_t)p->len;
        st = p->len ? MR_OK : MR_EAGAIN;
    }
    p->len = 0;
    p->expected = 0;
    p->active = 0;
    p->has_pts = 0;
    if (t->timing_enabled) {
        if (video) t->timing.video_us += ticks_us(begin);
        else t->timing.audio_us += ticks_us(begin);
    }
    return st;
}

mr_status mr_ts_next_packet(mr_ts *t, mr_packet *pkt)
{
    uint8_t packet[192];
    unsigned service_countdown = TS_SERVICE_PACKETS;
    while (t->cursor + (size_t)t->packet_size <= t->len) {
        const uint8_t *p, *es;
        size_t n, es_len;
        int pusi, has_pts;
        uint16_t pid;
        uint64_t pts;
        size_t expected;
        mr_ts_pes *a;
        int video;
        clock_t mark = 0;
        clock_t handling_mark = 0;

        if (t->timing_enabled) mark = clock();
        if (!ts_read_at(t, t->cursor, packet, (size_t)t->packet_size)) {
            if (t->timing_enabled) t->timing.source_us += ticks_us(mark);
            /* A streaming source has no known end: a short/failed read is the
             * end of the stream, so fall through to flush any pending PES. */
            if (t->streaming) break;
            return MR_EFORMAT;
        }
        if (t->timing_enabled) t->timing.source_us += ticks_us(mark);
        t->timing.packets_scanned++;
        if (--service_countdown == 0) {
            if (t->service) {
                t->service(t->service_opaque);
                t->timing.service_calls++;
            }
            service_countdown = TS_SERVICE_PACKETS;
        }
        if (t->timing_enabled) mark = clock();
        p = payload(packet + t->sync_off, &n, &pusi, &pid);
        if (t->timing_enabled) t->timing.sync_us += ticks_us(mark);
        if (!p || (pid != t->video_pid && pid != t->audio_pid)) {
            t->cursor += (size_t)t->packet_size;
            continue;
        }
        video = pid == t->video_pid;
        a = video ? &t->video_pes : &t->audio_pes;
        if (t->timing_enabled) handling_mark = clock();

        /* Return the completed old PES first. Leave this PUSI packet at the
         * cursor so the next call starts the new PES without losing bytes. */
        if (pusi && a->active && a->len)
            return emit_pes(t, a, video, pkt);

        t->cursor += (size_t)t->packet_size;
        es = p;
        es_len = n;
        if (t->timing_enabled) mark = clock();
        if (pusi) {
            es = pes_payload(p, &es_len, &pts, &has_pts, &expected);
            if (!es) {
                a->active = 0;
                a->len = 0;
                continue;
            }
            a->active = 1;
            a->expected = expected;
            a->has_pts = has_pts;
            a->pts = pts;
        } else if (!a->active) {
            continue;
        }
        if (a->expected) {
            size_t remain = a->expected > a->len ? a->expected - a->len : 0;
            if (es_len > remain) es_len = remain;
        }
        if (t->timing_enabled) t->timing.assembly_us += ticks_us(mark);
        if (t->timing_enabled) mark = clock();
        if (!pes_append(a, es, es_len)) {
            if (t->timing_enabled) t->timing.copy_us += ticks_us(mark);
            return MR_ENOMEM;
        }
        if (t->timing_enabled) {
            t->timing.copy_us += ticks_us(mark);
            if (video) t->timing.video_us += ticks_us(handling_mark);
            else t->timing.audio_us += ticks_us(handling_mark);
        }
        if (a->expected && a->len >= a->expected)
            return emit_pes(t, a, video, pkt);
    }

    if (!t->video_pes.drained && t->video_pes.len) {
        t->video_pes.drained = 1;
        return emit_pes(t, &t->video_pes, 1, pkt);
    }
    if (!t->audio_pes.drained && t->audio_pes.len) {
        t->audio_pes.drained = 1;
        return emit_pes(t, &t->audio_pes, 0, pkt);
    }
    return MR_EAGAIN;
}

void mr_ts_rewind(mr_ts *t)
{
    t->cursor = 0;
    t->video_pes.len = t->audio_pes.len = 0;
    t->video_pes.expected = t->audio_pes.expected = 0;
    t->video_pes.active = t->audio_pes.active = 0;
    t->video_pes.has_pts = t->audio_pes.has_pts = 0;
    t->video_pes.drained = t->audio_pes.drained = 0;
}

void mr_ts_close(mr_ts *t)
{
    if (!t) return;
    free(t->video_pes.data);
    free(t->audio_pes.data);
    free(t->config);
    t->video_pes.data = t->audio_pes.data = NULL;
    t->config = NULL;
}
