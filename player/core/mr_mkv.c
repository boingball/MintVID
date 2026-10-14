#include "mr_mkv.h"

#include <stdlib.h>
#include <string.h>

#define MKV_META_MAX   (2U * 1024U * 1024U)
#define MKV_PACKET_MAX (16U * 1024U * 1024U)
#define MKV_UNKNOWN UINT64_MAX

typedef struct { uint64_t id, size, payload, end; int unknown; } ebml_elem;

static int read_at(mr_mkv *m, uint64_t off, void *dst, size_t n)
{
    if (off > m->len || n > m->len - (size_t)off) return 0;
    if (m->file_backed)
        return mr_source_read_at(m->source, (size_t)off, dst, n);
    memcpy(dst, m->buf + (size_t)off, n);
    return 1;
}

static int vint(const uint8_t *p, size_t n, uint64_t *value,
                unsigned *used, int keep_marker, int *unknown)
{
    unsigned bytes = 1, i;
    uint8_t mask = 0x80;
    uint64_t v;
    while (bytes <= 8 && !(p[0] & mask)) { mask >>= 1; bytes++; }
    if (bytes > 8 || bytes > n) return 0;
    v = keep_marker ? p[0] : (p[0] & (mask - 1));
    for (i = 1; i < bytes; i++) v = (v << 8) | p[i];
    if (unknown) {
        uint64_t max = bytes == 8 ? UINT64_C(0x00ffffffffffffff)
                                  : ((UINT64_C(1) << (bytes * 7)) - 1);
        *unknown = !keep_marker && v == max;
    }
    *value = v; *used = bytes;
    return 1;
}

static int elem_at(mr_mkv *m, uint64_t off, uint64_t limit, ebml_elem *e)
{
    uint8_t h[16];
    uint64_t id, size;
    unsigned ni, ns;
    int unknown = 0;
    size_t avail = limit > off ? (size_t)(limit - off) : 0;
    if (avail > sizeof h) avail = sizeof h;
    if (!avail || !read_at(m, off, h, avail) ||
        !vint(h, avail, &id, &ni, 1, NULL) ||
        !vint(h + ni, avail - ni, &size, &ns, 0, &unknown)) return 0;
    e->id = id; e->size = unknown ? MKV_UNKNOWN : size;
    e->payload = off + ni + ns; e->unknown = unknown;
    if (e->payload > limit || (!unknown && size > limit - e->payload)) return 0;
    e->end = unknown ? limit : e->payload + size;
    return 1;
}

static uint64_t uint_be(const uint8_t *p, size_t n)
{
    uint64_t v = 0;
    while (n--) v = (v << 8) | *p++;
    return v;
}

static int reserve(uint8_t **p, size_t *cap, size_t need, size_t max)
{
    uint8_t *q;
    size_t n = *cap ? *cap : 4096;
    if (need > max) return 0;
    while (n < need) { if (n > max / 2) n = max; else n *= 2; }
    if (need <= *cap) return 1;
    q = (uint8_t *)realloc(*p, n);
    if (!q) return 0;
    *p = q; *cap = n;
    return 1;
}

static int load(mr_mkv *m, uint64_t off, size_t n)
{
    return reserve(&m->scratch, &m->scratch_cap, n, MKV_META_MAX) &&
           read_at(m, off, m->scratch, n);
}

static int mem_elem(const uint8_t *p, size_t len, size_t off, ebml_elem *e)
{
    uint64_t id, size;
    unsigned ni, ns;
    int unknown = 0;
    if (off >= len || !vint(p + off, len - off, &id, &ni, 1, NULL) ||
        !vint(p + off + ni, len - off - ni, &size, &ns, 0, &unknown) ||
        unknown || size > len - off - ni - ns) return 0;
    e->id = id; e->size = size; e->payload = off + ni + ns;
    e->end = e->payload + size; e->unknown = 0;
    return 1;
}

/* Matroska stores SamplingFrequency as a big-endian IEEE-754 float.  Decode
 * the positive integer rate directly: pulling soft-float into the Amiga build
 * just for this metadata field costs space and requires __adddf3. */
static uint32_t sample_rate_be(const uint8_t *p, size_t n)
{
    uint64_t bits, mantissa, value, fraction_mask;
    unsigned mantissa_bits, exponent_bits, exponent, exponent_max;
    int bias, shift;
    if (n == 4) {
        mantissa_bits = 23; exponent_bits = 8; bias = 127;
    } else if (n == 8) {
        mantissa_bits = 52; exponent_bits = 11; bias = 1023;
    } else return 0;
    bits = uint_be(p, n);
    if (bits & (UINT64_C(1) << (mantissa_bits + exponent_bits))) return 0;
    exponent_max = (1U << exponent_bits) - 1U;
    exponent = (unsigned)((bits >> mantissa_bits) & exponent_max);
    if (!exponent || exponent == exponent_max) return 0;
    fraction_mask = (UINT64_C(1) << mantissa_bits) - 1;
    mantissa = (bits & fraction_mask) | (UINT64_C(1) << mantissa_bits);
    shift = (int)exponent - bias - (int)mantissa_bits;
    if (shift >= 0) {
        if (shift >= 32 || mantissa > (UINT32_MAX >> shift)) return 0;
        value = mantissa << shift;
    } else {
        unsigned right = (unsigned)-shift;
        if (right >= 64) return 0;
        if (right) mantissa += UINT64_C(1) << (right - 1); /* nearest Hz */
        value = mantissa >> right;
    }
    return value <= UINT32_MAX ? (uint32_t)value : 0;
}

static void parse_video(mr_video_info *v, const uint8_t *p, size_t len)
{
    size_t pos = 0;
    ebml_elem e;
    while (mem_elem(p, len, pos, &e)) {
        if (e.id == 0xb0 && e.size <= 8)
            v->width = (int)uint_be(p + e.payload, (size_t)e.size);
        else if (e.id == 0xba && e.size <= 8)
            v->height = (int)uint_be(p + e.payload, (size_t)e.size);
        pos = (size_t)e.end;
    }
}

static void parse_audio(mr_audio_info *a, const uint8_t *p, size_t len)
{
    size_t pos = 0;
    ebml_elem e;
    while (mem_elem(p, len, pos, &e)) {
        if (e.id == 0xb5 && (e.size == 4 || e.size == 8))
            a->sample_rate = sample_rate_be(p + e.payload, (size_t)e.size);
        else if (e.id == 0x9f && e.size <= 8)
            a->channels = (uint16_t)uint_be(p + e.payload, (size_t)e.size);
        else if (e.id == 0x6264 && e.size <= 8)
            a->bits_per_sample = (uint16_t)uint_be(p + e.payload,
                                                   (size_t)e.size);
        pos = (size_t)e.end;
    }
}

static int codec_is(const char *id, size_t id_len, const char *wanted)
{
    return strlen(wanted) == id_len && !memcmp(id, wanted, id_len);
}

static void parse_track(mr_mkv *m, const uint8_t *p, size_t len)
{
    size_t pos = 0, codec_len = 0, private_len = 0;
    uint64_t number = 0, type = 0, duration = 0;
    const char *codec = NULL;
    const uint8_t *priv = NULL;
    mr_video_info vi;
    mr_audio_info ai;
    ebml_elem e;
    memset(&vi, 0, sizeof vi); memset(&ai, 0, sizeof ai);
    ai.channels = 2; ai.bits_per_sample = 16;
    while (mem_elem(p, len, pos, &e)) {
        const uint8_t *q = p + e.payload;
        if (e.id == 0xd7 && e.size <= 8) number = uint_be(q, (size_t)e.size);
        else if (e.id == 0x83 && e.size <= 8) type = uint_be(q, (size_t)e.size);
        else if (e.id == 0x86) { codec = (const char *)q; codec_len = (size_t)e.size; }
        else if (e.id == 0x63a2) { priv = q; private_len = (size_t)e.size; }
        else if (e.id == 0x23e383 && e.size <= 8)
            duration = uint_be(q, (size_t)e.size);
        else if (e.id == 0xe0) parse_video(&vi, q, (size_t)e.size);
        else if (e.id == 0xe1) parse_audio(&ai, q, (size_t)e.size);
        pos = (size_t)e.end;
    }
    if (!number || !codec) return;
    if (type == 1 && !m->video_track) {
        if (codec_is(codec, codec_len, "V_MPEG4/ISO/AVC"))
            vi.fourcc = MR_FOURCC('a','v','c','1');
        else if (codec_is(codec, codec_len, "V_MPEG4/ISO/ASP") ||
                 codec_is(codec, codec_len, "V_MPEG4/ISO/SP"))
            vi.fourcc = MR_FOURCC('m','p','4','v');
        else if (codec_is(codec, codec_len, "V_MPEG2"))
            vi.fourcc = MR_FOURCC('m','p','g','2');
        else if (codec_is(codec, codec_len, "V_MJPEG"))
            vi.fourcc = MR_FOURCC('M','J','P','G');
        else return;
        if (priv && private_len) {
            m->video_config = (uint8_t *)malloc(private_len);
            if (!m->video_config) return;
            memcpy(m->video_config, priv, private_len);
            m->video_config_len = private_len;
            vi.config = m->video_config;
            vi.config_len = (uint32_t)private_len;
        }
        vi.rate = duration ? 1000000000U : 25;
        vi.scale = duration && duration <= 0xffffffffU ? (uint32_t)duration : 1;
        vi.valid = vi.width > 0 && vi.height > 0;
        if (vi.valid) { m->video = vi; m->video_track = number;
                        m->default_duration_ns = duration; }
    } else if (type == 2 && !m->audio_track) {
        if (codec_is(codec, codec_len, "A_AAC")) {
            ai.format_tag = MR_AUDIO_FORMAT_AAC;
            ai.codec_tag = MR_FOURCC('m','p','4','a');
            if (priv && private_len <= MR_AUDIO_CONFIG_MAX) {
                memcpy(ai.config, priv, private_len);
                ai.config_len = (uint8_t)private_len;
            }
        } else if (codec_is(codec, codec_len, "A_MPEG/L3"))
            ai.format_tag = MR_AUDIO_FORMAT_MP3;
        else if (codec_is(codec, codec_len, "A_MPEG/L2"))
            ai.format_tag = MR_AUDIO_FORMAT_MP2;
        else if (codec_is(codec, codec_len, "A_AC3")) {
            ai.format_tag = MR_AUDIO_FORMAT_AC3;
            ai.codec_tag = MR_FOURCC('a','c','-','3');
        } else if (codec_is(codec, codec_len, "A_PCM/INT/LIT")) {
            ai.format_tag = MR_AUDIO_FORMAT_PCM; ai.pcm_signed = 1;
        } else if (codec_is(codec, codec_len, "A_PCM/INT/BIG")) {
            ai.format_tag = MR_AUDIO_FORMAT_PCM; ai.pcm_signed = 1;
            ai.pcm_big_endian = 1;
        } else return;
        if (!ai.sample_rate) ai.sample_rate = 48000;
        if (!ai.channels) ai.channels = 2;
        if (ai.format_tag != MR_AUDIO_FORMAT_PCM) ai.bits_per_sample = 16;
        if (!ai.bits_per_sample) ai.bits_per_sample = 16;
        ai.block_align = (uint16_t)(ai.channels * ((ai.bits_per_sample + 7) / 8));
        ai.valid = 1; m->audio = ai; m->audio_track = number;
        m->audio_default_duration_ns = duration;
    }
}

static void parse_info(mr_mkv *m, const uint8_t *p, size_t len)
{
    size_t pos = 0; ebml_elem e;
    while (mem_elem(p, len, pos, &e)) {
        if (e.id == 0x2ad7b1 && e.size <= 8)
            m->timecode_scale = uint_be(p + e.payload, (size_t)e.size);
        pos = (size_t)e.end;
    }
}

static void parse_tracks(mr_mkv *m, const uint8_t *p, size_t len)
{
    size_t pos = 0; ebml_elem e;
    while (mem_elem(p, len, pos, &e)) {
        if (e.id == 0xae) parse_track(m, p + e.payload, (size_t)e.size);
        pos = (size_t)e.end;
    }
}

#define MKV_ID_CLUSTER   0x1f43b675
#define MKV_ID_CUES      0x1c53bb6b
#define MKV_ID_SEEKHEAD  0x114d9b74

/* SeekHead: Seek { SeekID (the element ID's raw bytes), SeekPosition
 * (relative to the segment payload) }. Only the Cues entry is used. */
static void parse_seekhead(mr_mkv *m, const uint8_t *p, size_t len)
{
    size_t pos = 0; ebml_elem e;
    while (mem_elem(p, len, pos, &e)) {
        if (e.id == 0x4dbb) {
            size_t q = (size_t)e.payload; ebml_elem c;
            uint64_t id = 0, at = 0;
            int have_at = 0;
            while (q < e.end && mem_elem(p, (size_t)e.end, q, &c)) {
                if (c.id == 0x53ab && c.size <= 8)
                    id = uint_be(p + c.payload, (size_t)c.size);
                else if (c.id == 0x53ac && c.size <= 8) {
                    at = uint_be(p + c.payload, (size_t)c.size);
                    have_at = 1;
                }
                q = (size_t)c.end;
            }
            if (id == MKV_ID_CUES && have_at && !m->cues_pos &&
                at < m->segment_end - m->segment_start)
                m->cues_pos = m->segment_start + at;
        }
        pos = (size_t)e.end;
    }
}

static mr_status open_common(mr_mkv *m)
{
    ebml_elem e, segment;
    uint64_t pos;
    if (!elem_at(m, 0, m->len, &e) || e.id != 0x1a45dfa3) return MR_EFORMAT;
    pos = e.end;
    if (!elem_at(m, pos, m->len, &segment) || segment.id != 0x18538067)
        return MR_EFORMAT;
    m->segment_start = segment.payload; m->segment_end = segment.end;
    m->timecode_scale = 1000000;
    for (pos = segment.payload; pos < segment.end;) {
        if (!elem_at(m, pos, segment.end, &e)) return MR_EFORMAT;
        if (e.id == MKV_ID_CLUSTER) { m->first_cluster = pos; break; }
        if (e.id == MKV_ID_CUES && !m->cues_pos) m->cues_pos = pos;
        if ((e.id == 0x1549a966 || e.id == 0x1654ae6b ||
             e.id == MKV_ID_SEEKHEAD) &&
            e.size != MKV_UNKNOWN && e.size <= MKV_META_MAX) {
            if (!load(m, e.payload, (size_t)e.size)) return MR_ENOMEM;
            if (e.id == 0x1549a966) parse_info(m, m->scratch, (size_t)e.size);
            else if (e.id == 0x1654ae6b)
                parse_tracks(m, m->scratch, (size_t)e.size);
            else parse_seekhead(m, m->scratch, (size_t)e.size);
        }
        if (e.unknown || e.end <= pos) return MR_EFORMAT;
        pos = e.end;
    }
    if (!m->first_cluster || !m->video.valid) return MR_EUNSUPPORTED;
    mr_mkv_rewind(m);
    return MR_OK;
}

mr_status mr_mkv_open(mr_mkv *m, const uint8_t *buf, size_t len)
{
    memset(m, 0, sizeof *m); m->buf = buf; m->len = len;
    return open_common(m);
}

mr_status mr_mkv_open_source(mr_mkv *m, mr_source *source, size_t len)
{
    memset(m, 0, sizeof *m); m->source = source; m->len = len;
    m->file_backed = 1; return open_common(m);
}

static mr_status emit_lace(mr_mkv *m, mr_packet *pkt)
{
    unsigned i = m->lace_index++;
    pkt->is_video = m->lace_video;
    pkt->data = m->packet + m->lace_offset[i];
    pkt->len = m->lace_size[i];
    pkt->has_pts = 1;
    pkt->pts_us = m->lace_pts_us +
                  (m->lace_duration_ns * i) / 1000U;
    return pkt->len ? MR_OK : MR_EAGAIN;
}

static mr_status emit_block(mr_mkv *m, const ebml_elem *e, mr_packet *pkt)
{
    uint64_t track;
    unsigned nt;
    int16_t rel;
    uint8_t flags;
    size_t pos, remain, i, total;
    unsigned lacing, count;
    int64_t ticks;
    if (e->size < 4 || e->size > MKV_PACKET_MAX ||
        !reserve(&m->packet, &m->packet_cap, (size_t)e->size,
                 MKV_PACKET_MAX) ||
        !read_at(m, e->payload, m->packet, (size_t)e->size) ||
        !vint(m->packet, (size_t)e->size, &track, &nt, 0, NULL) ||
        nt + 3 > e->size) return MR_EFORMAT;
    rel = (int16_t)(((unsigned)m->packet[nt] << 8) | m->packet[nt + 1]);
    flags = m->packet[nt + 2];
    if (track != m->video_track && track != m->audio_track) return MR_EAGAIN;
    pos = nt + 3;
    lacing = (flags >> 1) & 3;
    count = 1;
    if (lacing) {
        if (pos >= e->size) return MR_EFORMAT;
        count = (unsigned)m->packet[pos++] + 1;
        if (count < 2) return MR_EFORMAT;
    }
    remain = (size_t)e->size - pos;
    memset(m->lace_size, 0, sizeof m->lace_size);
    if (lacing == 0) {
        m->lace_size[0] = (uint32_t)remain;
    } else if (lacing == 1) {                    /* Xiph lacing */
        total = 0;
        for (i = 0; i + 1 < count; i++) {
            size_t n = 0;
            unsigned b;
            do {
                if (pos >= e->size) return MR_EFORMAT;
                b = m->packet[pos++]; n += b;
            } while (b == 255);
            if (n > UINT32_MAX || total > (size_t)e->size - pos ||
                n > (size_t)e->size - pos - total)
                return MR_EFORMAT;
            m->lace_size[i] = (uint32_t)n; total += n;
        }
        remain = (size_t)e->size - pos;
        if (total > remain) return MR_EFORMAT;
        m->lace_size[count - 1] = (uint32_t)(remain - total);
    } else if (lacing == 2) {                   /* fixed-size lacing */
        if (remain % count) return MR_EFORMAT;
        for (i = 0; i < count; i++)
            m->lace_size[i] = (uint32_t)(remain / count);
    } else {                                    /* EBML lacing */
        uint64_t u;
        unsigned used;
        int64_t previous;
        total = 0;
        if (!vint(m->packet + pos, (size_t)e->size - pos,
                  &u, &used, 0, NULL) || u > UINT32_MAX)
            return MR_EFORMAT;
        pos += used; previous = (int64_t)u;
        m->lace_size[0] = (uint32_t)u; total = (size_t)u;
        for (i = 1; i + 1 < count; i++) {
            uint64_t bias;
            int64_t delta, current;
            if (!vint(m->packet + pos, (size_t)e->size - pos,
                      &u, &used, 0, NULL)) return MR_EFORMAT;
            pos += used;
            bias = (UINT64_C(1) << (used * 7 - 1)) - 1;
            delta = (int64_t)u - (int64_t)bias;
            current = previous + delta;
            if (current < 0 || current > UINT32_MAX ||
                (uint64_t)total + (uint64_t)current > SIZE_MAX)
                return MR_EFORMAT;
            m->lace_size[i] = (uint32_t)current;
            total += (size_t)current; previous = current;
        }
        remain = (size_t)e->size - pos;
        if (total > remain) return MR_EFORMAT;
        m->lace_size[count - 1] = (uint32_t)(remain - total);
    }
    total = pos;
    for (i = 0; i < count; i++) {
        if (m->lace_size[i] > (size_t)e->size - total) return MR_EFORMAT;
        m->lace_offset[i] = (uint32_t)total;
        total += m->lace_size[i];
    }
    if (total != e->size) return MR_EFORMAT;
    ticks = (int64_t)m->cluster_timecode + rel;
    if (ticks < 0) ticks = 0;
    m->lace_pts_us = (uint64_t)ticks * m->timecode_scale / 1000U;
    m->lace_duration_ns = track == m->video_track
                        ? m->default_duration_ns
                        : m->audio_default_duration_ns;
    m->lace_video = track == m->video_track;
    m->lace_count = count; m->lace_index = 0;
    return emit_lace(m, pkt);
}

mr_status mr_mkv_next_packet(mr_mkv *m, mr_packet *pkt)
{
    ebml_elem e;
    if (m->lace_index < m->lace_count) return emit_lace(m, pkt);
    while (m->cursor < m->segment_end) {
        if (!m->cluster_end || m->cursor >= m->cluster_end) {
            if (!elem_at(m, m->cursor, m->segment_end, &e)) return MR_EFORMAT;
            if (e.id != 0x1f43b675) { if (e.unknown) return MR_EFORMAT;
                                      m->cursor = e.end; continue; }
            m->cursor = e.payload; m->cluster_end = e.end;
            m->cluster_timecode = 0; continue;
        }
        {
        uint64_t element_start = m->cursor;
        if (!elem_at(m, m->cursor, m->cluster_end, &e)) return MR_EFORMAT;
        if (e.id == 0x1f43b675) {
            m->cursor = element_start; m->cluster_end = 0; continue;
        }
        m->cursor = e.end;
        if (e.id == 0xe7 && e.size <= 8) {
            uint8_t b[8];
            if (!read_at(m, e.payload, b, (size_t)e.size)) return MR_EFORMAT;
            m->cluster_timecode = uint_be(b, (size_t)e.size);
        } else if (e.id == 0xa3) {
            mr_status st = emit_block(m, &e, pkt);
            if (st != MR_EAGAIN) return st;
        } else if (e.id == 0xa0 && !e.unknown) {
            uint64_t child = e.payload;
            while (child < e.end) {
                ebml_elem block;
                if (!elem_at(m, child, e.end, &block)) return MR_EFORMAT;
                if (block.id == 0xa1) {
                    mr_status st = emit_block(m, &block, pkt);
                    if (st != MR_EAGAIN) return st;
                }
                if (block.unknown || block.end <= child) return MR_EFORMAT;
                child = block.end;
            }
        }
        }
    }
    return MR_EAGAIN;
}

void mr_mkv_rewind(mr_mkv *m)
{
    m->cursor = m->first_cluster; m->cluster_end = 0; m->cluster_timecode = 0;
    m->lace_count = m->lace_index = 0;
}

/* ---- Seeking ----------------------------------------------------------- */

static uint64_t ticks_to_us(const mr_mkv *m, int64_t ticks)
{
    if (ticks < 0) ticks = 0;
    return (uint64_t)ticks * m->timecode_scale / 1000U;
}

/* Top-level IDs that end a cluster of unknown size. */
static int is_top_level_id(uint64_t id)
{
    return id == MKV_ID_CLUSTER || id == MKV_ID_CUES ||
           id == MKV_ID_SEEKHEAD || id == 0x1549a966 || id == 0x1654ae6b ||
           id == 0x1254c367 || id == 0x1043a770 || id == 0x1941a469;
}

/* Track number, relative timecode and keyframe flag of a SimpleBlock (0xa3)
 * or BlockGroup (0xa0). A BlockGroup is a keyframe when it carries no
 * ReferenceBlock. Returns 0 if the element is neither or is malformed. */
static int block_info(mr_mkv *m, const ebml_elem *e, uint64_t *track,
                      int16_t *rel, int *key)
{
    uint8_t h[12];
    unsigned nt;
    size_t n;
    ebml_elem block, c;
    uint64_t child;
    int have_block = 0, referenced = 0;

    if (e->id == 0xa3) { block = *e; have_block = 1; }
    else if (e->id == 0xa0 && !e->unknown) {
        for (child = e->payload; child < e->end; child = c.end) {
            if (!elem_at(m, child, e->end, &c) || c.unknown) return 0;
            if (c.id == 0xa1) { block = c; have_block = 1; }
            else if (c.id == 0xfb) referenced = 1;
        }
    }
    if (!have_block || block.size < 4) return 0;
    n = block.size < sizeof h ? (size_t)block.size : sizeof h;
    if (!read_at(m, block.payload, h, n) ||
        !vint(h, n, track, &nt, 0, NULL) || nt + 3 > n) return 0;
    *rel = (int16_t)(((unsigned)h[nt] << 8) | h[nt + 1]);
    *key = e->id == 0xa3 ? (h[nt + 2] & 0x80) != 0 : !referenced;
    return 1;
}

/* Bounds and timecode of the cluster starting at pos. An unknown-size
 * cluster ends at the next top-level element. */
static int cluster_at(mr_mkv *m, uint64_t pos, uint64_t *payload,
                      uint64_t *end, uint64_t *tc)
{
    ebml_elem e, c;
    uint64_t child;
    int have_tc = 0;
    if (!elem_at(m, pos, m->segment_end, &e) || e.id != MKV_ID_CLUSTER)
        return 0;
    *payload = e.payload; *end = e.end; *tc = 0;
    for (child = e.payload; child < e.end; child = c.end) {
        if (!elem_at(m, child, e.end, &c)) break;
        if (e.unknown && is_top_level_id(c.id)) { *end = child; break; }
        if (c.id == 0xe7 && c.size <= 8 && !have_tc) {
            uint8_t b[8];
            if (!read_at(m, c.payload, b, (size_t)c.size)) return 0;
            *tc = uint_be(b, (size_t)c.size);
            have_tc = 1;
            if (!e.unknown) break;      /* known size: the end is known */
        }
        if (c.unknown || c.end <= child) return 0;
    }
    return have_tc;
}

typedef struct {
    uint64_t cluster, cluster_end, tc;
    uint64_t block, pts_us;
    int found;
} mkv_key;

/* Walk one cluster's blocks. *last gets the last video keyframe at or
 * before target_us, *first the cluster's first video keyframe. */
static int scan_cluster(mr_mkv *m, uint64_t pos, uint64_t target_us,
                        mkv_key *last, mkv_key *first)
{
    uint64_t payload, end, tc, child;
    ebml_elem c;
    last->found = first->found = 0;
    if (!cluster_at(m, pos, &payload, &end, &tc)) return 0;
    for (child = payload; child < end; child = c.end) {
        uint64_t track, pts;
        int16_t rel;
        int key;
        if (!elem_at(m, child, end, &c) || c.unknown || c.end <= child)
            break;
        if ((c.id != 0xa3 && c.id != 0xa0) ||
            !block_info(m, &c, &track, &rel, &key) ||
            track != m->video_track || !key) continue;
        pts = ticks_to_us(m, (int64_t)tc + rel);
        if (!first->found) {
            first->cluster = pos; first->cluster_end = end; first->tc = tc;
            first->block = child; first->pts_us = pts; first->found = 1;
        }
        if (pts <= target_us) {
            last->cluster = pos; last->cluster_end = end; last->tc = tc;
            last->block = child; last->pts_us = pts; last->found = 1;
        }
    }
    return 1;
}

static void load_cues(mr_mkv *m)
{
    ebml_elem e, cp, c, t;
    size_t pos, q, r, len;
    unsigned count = 0, n = 0;
    uint64_t prev = 0;

    m->cues_state = -1;
    if (!m->cues_pos ||
        !elem_at(m, m->cues_pos, m->segment_end, &e) ||
        e.id != MKV_ID_CUES || e.unknown || e.size > MKV_META_MAX ||
        !load(m, e.payload, (size_t)e.size)) return;
    len = (size_t)e.size;
    for (pos = 0; mem_elem(m->scratch, len, pos, &cp); pos = (size_t)cp.end)
        if (cp.id == 0xbb) count++;
    if (!count) return;
    m->cue_time = (uint64_t *)malloc(count * sizeof *m->cue_time);
    m->cue_cluster = (uint64_t *)malloc(count * sizeof *m->cue_cluster);
    if (!m->cue_time || !m->cue_cluster) goto fail;

    for (pos = 0; mem_elem(m->scratch, len, pos, &cp); pos = (size_t)cp.end) {
        uint64_t time = 0, cluster = 0;
        int have_time = 0, have_cluster = 0;
        if (cp.id != 0xbb) continue;
        for (q = (size_t)cp.payload;
             q < cp.end && mem_elem(m->scratch, (size_t)cp.end, q, &c);
             q = (size_t)c.end) {
            if (c.id == 0xb3 && c.size <= 8) {
                time = uint_be(m->scratch + c.payload, (size_t)c.size);
                have_time = 1;
            } else if (c.id == 0xb7 && !have_cluster) {
                uint64_t track = 0, at = 0;
                int have_at = 0;
                for (r = (size_t)c.payload;
                     r < c.end && mem_elem(m->scratch, (size_t)c.end, r, &t);
                     r = (size_t)t.end) {
                    if (t.id == 0xf7 && t.size <= 8)
                        track = uint_be(m->scratch + t.payload, (size_t)t.size);
                    else if (t.id == 0xf1 && t.size <= 8) {
                        at = uint_be(m->scratch + t.payload, (size_t)t.size);
                        have_at = 1;
                    }
                }
                if (have_at && (track == m->video_track || !track)) {
                    cluster = at; have_cluster = 1;
                }
            }
        }
        if (!have_time || !have_cluster) continue;
        if (cluster >= m->segment_end - m->segment_start ||
            (n && time < prev)) goto fail;
        m->cue_time[n] = time;
        m->cue_cluster[n] = m->segment_start + cluster;
        prev = time; n++;
    }
    if (!n) goto fail;
    m->cue_count = n;
    m->cues_state = 1;
    return;
fail:
    free(m->cue_time); free(m->cue_cluster);
    m->cue_time = m->cue_cluster = NULL;
    m->cue_count = 0;
}

static mr_status seek_to(mr_mkv *m, const mkv_key *k, uint64_t *out_us)
{
    m->cursor = k->block;
    m->cluster_end = k->cluster_end;
    m->cluster_timecode = k->tc;
    m->lace_count = m->lace_index = 0;
    if (out_us) *out_us = k->pts_us;
    return MR_OK;
}

mr_status mr_mkv_seek(mr_mkv *m, uint64_t target_us, uint64_t *out_us)
{
    mkv_key last, first;
    uint64_t pos, payload, end, tc;
    uint64_t *offs = NULL;
    size_t noffs = 0, cap = 0, i;
    mr_status st = MR_EUNSUPPORTED;

    if (!m || !m->first_cluster || !m->video_track) return MR_EUNSUPPORTED;
    if (!m->cues_state) load_cues(m);

    /* Cues name the cluster holding a keyframe at the cue time. */
    if (m->cues_state > 0) {
        unsigned lo = 0, hi = m->cue_count;
        while (lo < hi) {                 /* first cue later than target */
            unsigned mid = lo + (hi - lo) / 2;
            if (ticks_to_us(m, (int64_t)m->cue_time[mid]) <= target_us)
                lo = mid + 1;
            else hi = mid;
        }
        i = lo ? lo - 1 : 0;
        if (scan_cluster(m, m->cue_cluster[i], target_us, &last, &first)) {
            if (last.found) return seek_to(m, &last, out_us);
            if (!lo && first.found) return seek_to(m, &first, out_us);
        }
    }

    /* No usable index: collect the clusters that start at or before the
     * target, then search them backwards for a keyframe. */
    for (pos = m->first_cluster; pos < m->segment_end; pos = end) {
        ebml_elem e;
        if (!elem_at(m, pos, m->segment_end, &e)) break;
        if (e.id != MKV_ID_CLUSTER) {
            if (e.unknown || e.end <= pos) break;
            end = e.end; continue;
        }
        if (!cluster_at(m, pos, &payload, &end, &tc) || end <= pos) break;
        if (ticks_to_us(m, (int64_t)tc) > target_us) break;
        if (noffs == cap) {
            size_t ncap = cap ? cap * 2 : 64;
            uint64_t *n = (uint64_t *)realloc(offs, ncap * sizeof *offs);
            if (!n) { free(offs); return MR_ENOMEM; }
            offs = n; cap = ncap;
        }
        offs[noffs++] = pos;
    }
    for (i = noffs; i > 0; i--) {
        if (scan_cluster(m, offs[i - 1], target_us, &last, &first) &&
            last.found) {
            st = seek_to(m, &last, out_us);
            break;
        }
    }
    free(offs);
    if (st == MR_OK) return st;

    /* Target precedes every keyframe: start at the first one. */
    for (pos = m->first_cluster; pos < m->segment_end; pos = end) {
        ebml_elem e;
        if (!elem_at(m, pos, m->segment_end, &e)) break;
        if (e.id != MKV_ID_CLUSTER) {
            if (e.unknown || e.end <= pos) break;
            end = e.end; continue;
        }
        if (!cluster_at(m, pos, &payload, &end, &tc) || end <= pos) break;
        if (scan_cluster(m, pos, 0, &last, &first) && first.found)
            return seek_to(m, &first, out_us);
    }
    return MR_EUNSUPPORTED;
}

void mr_mkv_close(mr_mkv *m)
{
    if (!m) return;
    free(m->scratch); free(m->packet); free(m->video_config);
    free(m->cue_time); free(m->cue_cluster);
    m->scratch = m->packet = m->video_config = NULL;
    m->cue_time = m->cue_cluster = NULL;
    m->cue_count = 0; m->cues_state = 0;
}
