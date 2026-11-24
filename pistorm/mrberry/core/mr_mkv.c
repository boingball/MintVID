#include "mr_mkv.h"
#include "mr_mkv_tracks.h"

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

/* ---- MKA1: audio track list and choice ------------------------------------
 * Every audio track of the file is listed (also ones mrberry cannot play,
 * e.g. DTS or Opus), in file order.  mr_mkv_audio_want = 1-based index into
 * this list (0 = automatic).  Automatic = first playable track marked
 * "default" (FlagDefault, 1 when missing), else the first playable track.
 * One mrberry process opens one film at a time, so plain globals are fine. */
mr_mkv_atrack mr_mkv_atracks[MR_MKV_ATRACK_MAX];
unsigned mr_mkv_atrack_count;
unsigned mr_mkv_audio_want;     /* set before opening: 0 = automatic */
unsigned mr_mkv_audio_chosen;   /* after opening: 1-based, 0 = none */
int mr_mkv_audio_want_bad;      /* the wanted track is missing / not playable */

static void copy_text(char *dst, size_t cap, const uint8_t *src, size_t n)
{
    size_t i, o = 0;
    for (i = 0; i < n && o + 1 < cap && src[i]; i++) {
        uint8_t c = src[i];
        if (c == '|' || c == '\n' || c == '\r') c = ' ';
        if (c >= 0x80) {                 /* UTF-8: keep Latin-1 letters */
            if ((c == 0xc3 || c == 0xc2) && i + 1 < n) { c = (uint8_t)(src[++i] + (c == 0xc3 ? 0x40 : 0)); }
            else { while (i + 1 < n && (src[i + 1] & 0xc0) == 0x80) i++; c = '?'; }
        }
        dst[o++] = (char)c;
    }
    dst[o] = 0;
}

int (*mr_mkv_audio_usable)(const mr_audio_info *ai);   /* MKA1c */

/* v96.84: subtitle tracks (see mr_mkv_tracks.h) */
mr_mkv_strack mr_mkv_stracks[MR_MKV_STRACK_MAX];
unsigned mr_mkv_strack_count;
uint64_t mr_mkv_sub_track;
void (*mr_mkv_sub_cb)(uint64_t pts_us, uint64_t dur_us,
                      const uint8_t *data, size_t len, int kind);
static uint64_t mkv_block_dur_ticks;    /* BlockDuration of the BlockGroup */

static int track_usable(const mr_mkv_atrack *t)
{
    return t->supported && (!mr_mkv_audio_usable || mr_mkv_audio_usable(&t->ai));
}

static void mkv_pick_audio(mr_mkv *m)
{
    unsigned i, pick = 0;
    mr_mkv_audio_chosen = 0;
    mr_mkv_audio_want_bad = 0;
    if (mr_mkv_audio_want) {
        if (mr_mkv_audio_want <= mr_mkv_atrack_count &&
            track_usable(&mr_mkv_atracks[mr_mkv_audio_want - 1]))
            pick = mr_mkv_audio_want;
        else
            mr_mkv_audio_want_bad = 1;
    }
    for (i = 0; !pick && i < mr_mkv_atrack_count; i++)
        if (track_usable(&mr_mkv_atracks[i]) && mr_mkv_atracks[i].is_default) pick = i + 1;
    for (i = 0; !pick && i < mr_mkv_atrack_count; i++)
        if (track_usable(&mr_mkv_atracks[i])) pick = i + 1;
    /* MKA1c: no track usable with this output -> as before, first supported
     * one (the film then plays without HDMI sound, see MKA1b) */
    for (i = 0; !pick && i < mr_mkv_atrack_count; i++)
        if (mr_mkv_atracks[i].supported) pick = i + 1;
    if (!pick) return;
    m->audio = mr_mkv_atracks[pick - 1].ai;
    m->audio_track = mr_mkv_atracks[pick - 1].number;
    m->audio_default_duration_ns = mr_mkv_atracks[pick - 1].duration_ns;
    mr_mkv_audio_chosen = pick;
}

static void parse_track(mr_mkv *m, const uint8_t *p, size_t len)
{
    size_t pos = 0, codec_len = 0, private_len = 0, lang_len = 0, name_len = 0;
    uint64_t number = 0, type = 0, duration = 0, flag_default = 1, flag_forced = 0;
    const char *codec = NULL;
    const uint8_t *priv = NULL, *lang = NULL, *tname = NULL;
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
        else if (e.id == 0x22b59c && !lang) { lang = q; lang_len = (size_t)e.size; }
        else if (e.id == 0x22b59d) { lang = q; lang_len = (size_t)e.size; } /* BCP47 wins */
        else if (e.id == 0x536e) { tname = q; name_len = (size_t)e.size; }
        else if (e.id == 0x88 && e.size <= 8) flag_default = uint_be(q, (size_t)e.size);
        else if (e.id == 0x55aa && e.size <= 8) flag_forced = uint_be(q, (size_t)e.size);
        pos = (size_t)e.end;
    }
    if (!number || !codec) return;
    if (type == 0x11 && mr_mkv_strack_count < MR_MKV_STRACK_MAX) {
        /* v96.84: subtitle track */
        mr_mkv_strack *t = &mr_mkv_stracks[mr_mkv_strack_count++];
        memset(t, 0, sizeof *t);
        t->number = number;
        t->is_default = flag_default != 0;
        t->is_forced = flag_forced != 0;
        copy_text(t->codec, sizeof t->codec, (const uint8_t *)codec, codec_len);
        if (lang) copy_text(t->lang, sizeof t->lang, lang, lang_len);
        else strcpy(t->lang, "eng");
        if (tname) copy_text(t->name, sizeof t->name, tname, name_len);
        if (codec_is(codec, codec_len, "S_TEXT/UTF8")) t->kind = MR_MKV_SUB_SRT;
        else if (codec_is(codec, codec_len, "S_TEXT/ASS") ||
                 codec_is(codec, codec_len, "S_TEXT/SSA") ||
                 codec_is(codec, codec_len, "S_ASS") ||
                 codec_is(codec, codec_len, "S_SSA")) t->kind = MR_MKV_SUB_ASS;
        return;
    }
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
    } else if (type == 2 && mr_mkv_atrack_count < MR_MKV_ATRACK_MAX) {
        /* MKA1: list every audio track; the choice is made afterwards */
        mr_mkv_atrack *t = &mr_mkv_atracks[mr_mkv_atrack_count++];
        memset(t, 0, sizeof *t);
        t->number = number;
        t->duration_ns = duration;
        t->is_default = flag_default != 0;
        copy_text(t->codec, sizeof t->codec, (const uint8_t *)codec, codec_len);
        if (lang) copy_text(t->lang, sizeof t->lang, lang, lang_len);
        else strcpy(t->lang, "eng");     /* Matroska default */
        if (tname) copy_text(t->name, sizeof t->name, tname, name_len);
        t->ai.channels = ai.channels;
        t->ai.sample_rate = ai.sample_rate;
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
        } else if (codec_is(codec, codec_len, "A_EAC3")) {
            ai.format_tag = MR_AUDIO_FORMAT_EAC3;
            ai.codec_tag = MR_FOURCC('e','c','-','3');
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
        ai.valid = 1;
        t->ai = ai;
        t->supported = 1;
    }
}

/* P96W53: film length.  Segment Duration (0x4489) is a float in timecode
 * ticks; decoded to an integer like sample_rate_be (no soft-float). */
uint64_t mr_mkv_duration_us;
static uint64_t mkv_duration_ticks;

static uint64_t float_be_u64(const uint8_t *p, size_t n)
{
    uint64_t bits, mantissa;
    unsigned mantissa_bits, exponent_bits, exponent, exponent_max;
    int bias, shift;
    if (n == 4) { mantissa_bits = 23; exponent_bits = 8; bias = 127; }
    else if (n == 8) { mantissa_bits = 52; exponent_bits = 11; bias = 1023; }
    else return 0;
    bits = uint_be(p, n);
    if (bits & (UINT64_C(1) << (mantissa_bits + exponent_bits))) return 0;
    exponent_max = (1U << exponent_bits) - 1U;
    exponent = (unsigned)((bits >> mantissa_bits) & exponent_max);
    if (!exponent || exponent == exponent_max) return 0;
    mantissa = (bits & ((UINT64_C(1) << mantissa_bits) - 1)) |
               (UINT64_C(1) << mantissa_bits);
    shift = (int)exponent - bias - (int)mantissa_bits;
    if (shift >= 0) return shift >= 11 ? 0 : mantissa << shift;
    if ((unsigned)-shift >= 64) return 0;
    return mantissa >> (unsigned)-shift;
}

static void parse_info(mr_mkv *m, const uint8_t *p, size_t len)
{
    size_t pos = 0; ebml_elem e;
    while (mem_elem(p, len, pos, &e)) {
        if (e.id == 0x2ad7b1 && e.size <= 8)
            m->timecode_scale = uint_be(p + e.payload, (size_t)e.size);
        else if (e.id == 0x4489)
            mkv_duration_ticks = float_be_u64(p + e.payload, (size_t)e.size);
        pos = (size_t)e.end;
    }
}

/* v96.85: chapters (first EditionEntry; hidden/disabled atoms skipped) */
uint64_t mr_mkv_chapters_us[MR_MKV_CHAPTER_MAX];
unsigned mr_mkv_chapter_count;
static void parse_chapters(const uint8_t *p, size_t len)
{
    size_t pos = 0;
    ebml_elem ed;
    while (mem_elem(p, len, pos, &ed)) {
        if (ed.id == 0x45b9) {                    /* EditionEntry */
            const uint8_t *q = p + ed.payload;
            size_t qlen = (size_t)ed.size, qp = 0;
            ebml_elem a;
            while (mem_elem(q, qlen, qp, &a)) {
                if (a.id == 0xb6 && mr_mkv_chapter_count < MR_MKV_CHAPTER_MAX) {
                    const uint8_t *r = q + a.payload;
                    size_t rlen = (size_t)a.size, rp = 0;
                    ebml_elem c;
                    uint64_t start = 0;
                    int have = 0, hidden = 0, enabled = 1;
                    while (mem_elem(r, rlen, rp, &c)) {
                        const uint8_t *v = r + c.payload;
                        if (c.id == 0x91 && c.size <= 8) { start = uint_be(v, (size_t)c.size); have = 1; }
                        else if (c.id == 0x98 && c.size <= 8) hidden = uint_be(v, (size_t)c.size) != 0;
                        else if (c.id == 0x4598 && c.size <= 8) enabled = uint_be(v, (size_t)c.size) != 0;
                        rp = (size_t)c.end;
                    }
                    if (have && !hidden && enabled) {
                        unsigned i = mr_mkv_chapter_count++;
                        uint64_t us = start / 1000U;          /* ns -> us */
                        while (i > 0 && mr_mkv_chapters_us[i - 1] > us) {
                            mr_mkv_chapters_us[i] = mr_mkv_chapters_us[i - 1]; i--;
                        }
                        mr_mkv_chapters_us[i] = us;
                    }
                }
                qp = (size_t)a.end;
            }
            return;                                /* first edition only */
        }
        pos = (size_t)ed.end;
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
    mkv_duration_ticks = 0; mr_mkv_duration_us = 0;
    mr_mkv_atrack_count = 0; mr_mkv_audio_chosen = 0;   /* MKA1 */
    mr_mkv_strack_count = 0;                             /* v96.84 */
    mr_mkv_chapter_count = 0;                            /* v96.85 */
    for (pos = segment.payload; pos < segment.end;) {
        if (!elem_at(m, pos, segment.end, &e)) return MR_EFORMAT;
        if (e.id == 0x1f43b675) { m->first_cluster = pos; break; }
        if ((e.id == 0x1549a966 || e.id == 0x1654ae6b || e.id == 0x1043a770) &&
            e.size != MKV_UNKNOWN && e.size <= MKV_META_MAX) {
            if (!load(m, e.payload, (size_t)e.size)) return MR_ENOMEM;
            if (e.id == 0x1549a966) parse_info(m, m->scratch, (size_t)e.size);
            else if (e.id == 0x1043a770) parse_chapters(m->scratch, (size_t)e.size);
            else parse_tracks(m, m->scratch, (size_t)e.size);
        }
        if (e.unknown || e.end <= pos) return MR_EFORMAT;
        pos = e.end;
    }
    if (!m->first_cluster || !m->video.valid) return MR_EUNSUPPORTED;
    mkv_pick_audio(m);                                  /* MKA1 */
    mr_mkv_duration_us = mkv_duration_ticks * m->timecode_scale / 1000U;
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
    if (mr_mkv_sub_track && track == mr_mkv_sub_track && mr_mkv_sub_cb &&
        !((flags >> 1) & 3)) {
        /* v96.84: a subtitle block of the chosen track (never laced) */
        unsigned si;
        int kind = 0;
        int64_t st = (int64_t)m->cluster_timecode + rel;
        if (st < 0) st = 0;
        for (si = 0; si < mr_mkv_strack_count; si++)
            if (mr_mkv_stracks[si].number == track) kind = mr_mkv_stracks[si].kind;
        if (kind)
            mr_mkv_sub_cb((uint64_t)st * m->timecode_scale / 1000U,
                          mkv_block_dur_ticks * m->timecode_scale / 1000U,
                          m->packet + nt + 3, (size_t)e->size - nt - 3, kind);
        return MR_EAGAIN;
    }
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
            mr_status st;
            mkv_block_dur_ticks = 0;
            st = emit_block(m, &e, pkt);
            if (st != MR_EAGAIN) return st;
        } else if (e.id == 0xa0 && !e.unknown) {
            uint64_t child = e.payload;
            /* v96.84: BlockDuration (0x9b) of this group (subtitles) */
            mkv_block_dur_ticks = 0;
            if (mr_mkv_sub_track) {
                uint64_t c2 = e.payload;
                while (c2 < e.end) {
                    ebml_elem d;
                    if (!elem_at(m, c2, e.end, &d) || d.unknown || d.end <= c2) break;
                    if (d.id == 0x9b && d.size <= 8) {
                        uint8_t b[8];
                        if (read_at(m, d.payload, b, (size_t)d.size))
                            mkv_block_dur_ticks = uint_be(b, (size_t)d.size);
                    }
                    c2 = d.end;
                }
            }
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

/* ---- P96W50: T: start time (seek) ---------------------------------------
 * Like VPr8's R5 compressed T: seek, but on the host demux: no frame is
 * decoded during the search, only element headers and NAL headers are read.
 *   1. walk the cluster headers (offset + timestamp; cluster payloads are
 *      skipped by their size),
 *   2. in the last cluster that starts at or before the target, and if
 *      needed in the clusters before it, look for the last video block that
 *      is a real H.264 IDR (nal_unit_type 5) with PTS <= target,
 *   3. none within ~2 minutes before the target: take the first IDR after it.
 * The cursor is left on that block, so the next packet is the IDR itself. */
#define MKV_SEEK_BACK_US  (120ULL * 1000000ULL)

static int block_head(mr_mkv *m, const ebml_elem *e, uint64_t *track,
                      int16_t *rel, uint8_t *flags, unsigned *hdr)
{
    uint8_t b[12];
    unsigned nt;
    size_t n = e->size < sizeof b ? (size_t)e->size : sizeof b;
    if (n < 4 || !read_at(m, e->payload, b, n) ||
        !vint(b, n, track, &nt, 0, NULL) || nt + 3 > n) return 0;
    *rel = (int16_t)(((unsigned)b[nt] << 8) | b[nt + 1]);
    *flags = b[nt + 2];
    *hdr = nt + 3;
    return 1;
}

/* AVC length-prefixed access unit at [off,end): 1 = contains an IDR slice. */
static int block_is_idr(mr_mkv *m, uint64_t off, uint64_t end, unsigned ls)
{
    uint8_t b[5];
    unsigned i, guard = 0;
    while (off + ls < end && guard++ < 64) {
        uint32_t len = 0;
        unsigned t;
        if (!read_at(m, off, b, ls + 1)) return 0;
        for (i = 0; i < ls; i++) len = (len << 8) | b[i];
        if (!len || len > end - off - ls) return 0;
        t = b[ls] & 31u;
        if (t == 5) return 1;
        if (t >= 1 && t <= 4) return 0;     /* first slice is not IDR */
        off += ls + len;
    }
    return 0;
}

typedef struct {
    int found;
    uint64_t elem, cluster, cluster_end, cluster_tc, pts_us;
} mkv_seek_hit;

/* Scan one cluster for IDR video blocks.  mode 0: keep the one with the
 * largest PTS <= target; mode 1: stop at the
 * first one.  tc_us and end return the cluster start time and end. */
static int scan_cluster(mr_mkv *m, uint64_t coff, uint64_t target, int mode,
                        unsigned ls, mkv_seek_hit *hit,
                        uint64_t *tc_us, uint64_t *end)
{
    ebml_elem c, e;
    uint64_t pos, tc = 0;
    if (!elem_at(m, coff, m->segment_end, &c) || c.id != 0x1f43b675 ||
        c.unknown) return 0;
    if (end) *end = c.end;
    if (tc_us) *tc_us = 0;
    for (pos = c.payload; pos < c.end; pos = e.end) {
        ebml_elem blk;
        uint64_t track, pts;
        int16_t rel;
        uint8_t flags;
        unsigned hdr;
        int64_t ticks;
        if (!elem_at(m, pos, c.end, &e) || e.unknown || e.end <= pos) return 0;
        if (e.id == 0xe7 && e.size <= 8) {
            uint8_t b[8];
            if (!read_at(m, e.payload, b, (size_t)e.size)) return 0;
            tc = uint_be(b, (size_t)e.size);
            if (tc_us) *tc_us = tc * m->timecode_scale / 1000U;
            if (mode == 0 && tc * m->timecode_scale / 1000U > target) return 1;
            continue;
        }
        if (e.id == 0xa3) {
            blk = e;
        } else if (e.id == 0xa0) {
            uint64_t child = e.payload;
            int got = 0;
            while (child < e.end) {
                if (!elem_at(m, child, e.end, &blk) || blk.unknown ||
                    blk.end <= child) return 0;
                if (blk.id == 0xa1) { got = 1; break; }
                child = blk.end;
            }
            if (!got) continue;
        } else continue;
        if (!block_head(m, &blk, &track, &rel, &flags, &hdr)) return 0;
        if (track != m->video_track) continue;
        ticks = (int64_t)tc + rel;
        if (ticks < 0) ticks = 0;
        pts = (uint64_t)ticks * m->timecode_scale / 1000U;
        /* blocks are in decode order: once one video block is past the
         * target, every later IDR is too (it follows all earlier frames) */
        if (mode == 0 && pts > target) return 1;
        if (e.id == 0xa3 && !(flags & 0x80)) continue;   /* not a keyframe */
        if ((flags >> 1) & 3) continue;                    /* laced video */
        if (mode == 0 && hit->found && pts <= hit->pts_us) continue;
        if (!block_is_idr(m, blk.payload + hdr, blk.end, ls)) continue;
        hit->found = 1; hit->elem = pos; hit->cluster = coff;
        hit->cluster_end = c.end; hit->cluster_tc = tc; hit->pts_us = pts;
        if (mode == 1) return 1;
    }
    return 1;
}

typedef struct { uint64_t *off, *us; size_t n, cap; } mkv_index;

/* 1 when the last mr_mkv_seek() could use the file's Cues index. */
int mr_mkv_seek_used_cues;

static int index_add(mkv_index *x, uint64_t off, uint64_t us)
{
    if (x->n == x->cap) {
        size_t nc = x->cap ? x->cap * 2 : 256;
        uint64_t *a = (uint64_t *)realloc(x->off, nc * sizeof *a), *b;
        if (!a) return 0;
        x->off = a;
        b = (uint64_t *)realloc(x->us, nc * sizeof *b);
        if (!b) return 0;
        x->us = b; x->cap = nc;
    }
    x->off[x->n] = off; x->us[x->n] = us; x->n++;
    return 1;
}

/* Cues (the file's own key frame index): their offset from a SeekHead, or a
 * Cues element in front of the first cluster.  0 = none found. */
static uint64_t find_cues(mr_mkv *m)
{
    uint64_t pos;
    ebml_elem e;
    for (pos = m->segment_start; pos < m->segment_end; pos = e.end) {
        if (!elem_at(m, pos, m->segment_end, &e) || e.unknown || e.end <= pos) return 0;
        if (e.id == 0x1c53bb6b) return pos;
        if (e.id == 0x1f43b675) return 0;
        if (e.id == 0x114d9b74 && e.size <= 65536 && load(m, e.payload, (size_t)e.size)) {
            size_t sp = 0, size = (size_t)e.size;
            ebml_elem s;
            while (mem_elem(m->scratch, size, sp, &s)) {
                if (s.id == 0x4dbb) {
                    size_t cp = (size_t)s.payload;
                    uint64_t id = 0, at = 0;
                    ebml_elem f;
                    while (cp < s.end && mem_elem(m->scratch, (size_t)s.end, cp, &f)) {
                        if (f.id == 0x53ab && f.size <= 8)
                            id = uint_be(m->scratch + f.payload, (size_t)f.size);
                        else if (f.id == 0x53ac && f.size <= 8)
                            at = uint_be(m->scratch + f.payload, (size_t)f.size);
                        cp = (size_t)f.end;
                    }
                    if (id == 0x1c53bb6b) {
                        ebml_elem cues;
                        uint64_t off = m->segment_start + at;
                        if (elem_at(m, off, m->segment_end, &cues) &&
                            cues.id == 0x1c53bb6b) return off;
                    }
                }
                sp = (size_t)s.end;
            }
        }
    }
    return 0;
}

/* Video cue points (time, cluster offset) in file order. */
static int load_cues(mr_mkv *m, uint64_t off, mkv_index *x)
{
    ebml_elem cues, cp, f, t;
    size_t pos, size;
    if (!elem_at(m, off, m->segment_end, &cues) || cues.unknown ||
        cues.size > MKV_META_MAX || !load(m, cues.payload, (size_t)cues.size))
        return 0;
    size = (size_t)cues.size;
    for (pos = 0; mem_elem(m->scratch, size, pos, &cp); pos = (size_t)cp.end) {
        size_t q;
        uint64_t time = 0, cluster = 0, track = 0;
        int have_time = 0, have_pos = 0;
        if (cp.id != 0xbb) continue;
        for (q = (size_t)cp.payload; q < cp.end &&
             mem_elem(m->scratch, (size_t)cp.end, q, &f); q = (size_t)f.end) {
            if (f.id == 0xb3 && f.size <= 8) {
                time = uint_be(m->scratch + f.payload, (size_t)f.size);
                have_time = 1;
            } else if (f.id == 0xb7) {
                size_t r;
                uint64_t tr = 0, cl = 0;
                int hc = 0;
                for (r = (size_t)f.payload; r < f.end &&
                     mem_elem(m->scratch, (size_t)f.end, r, &t); r = (size_t)t.end) {
                    if (t.id == 0xf7 && t.size <= 8)
                        tr = uint_be(m->scratch + t.payload, (size_t)t.size);
                    else if (t.id == 0xf1 && t.size <= 8) {
                        cl = uint_be(m->scratch + t.payload, (size_t)t.size);
                        hc = 1;
                    }
                }
                if (hc && (tr == m->video_track || !have_pos)) {
                    track = tr; cluster = cl; have_pos = 1;
                }
            }
        }
        if (!have_time || !have_pos || track != m->video_track) continue;
        if (x->n && m->segment_start + cluster == x->off[x->n - 1]) continue;
        if (!index_add(x, m->segment_start + cluster, time * m->timecode_scale / 1000U))
            return 0;
    }
    return x->n != 0;
}

/* No Cues: walk the cluster headers up to the first one after the target
 * (one small read per cluster). */
static int walk_clusters(mr_mkv *m, uint64_t target_us, mkv_index *x)
{
    uint64_t pos;
    ebml_elem e;
    for (pos = m->first_cluster; pos < m->segment_end; pos = e.end) {
        uint64_t tc = 0, child, us;
        if (!elem_at(m, pos, m->segment_end, &e)) break;
        if (e.id != 0x1f43b675) { if (e.unknown || e.end <= pos) break; continue; }
        if (e.unknown) break;                      /* cannot skip it */
        for (child = e.payload; child < e.end;) {
            ebml_elem t;
            if (!elem_at(m, child, e.end, &t) || t.unknown || t.end <= child) break;
            if (t.id == 0xe7 && t.size <= 8) {
                uint8_t b[8];
                if (read_at(m, t.payload, b, (size_t)t.size))
                    tc = uint_be(b, (size_t)t.size);
                break;
            }
            if (t.id == 0xa3 || t.id == 0xa0) break;
            child = t.end;
        }
        us = tc * m->timecode_scale / 1000U;
        if (x->n && us > target_us) break;
        if (!index_add(x, pos, us)) return 0;
    }
    return x->n != 0;
}

mr_status mr_mkv_seek(mr_mkv *m, uint64_t target_us, uint64_t *out_us)
{
    mkv_index x;
    uint64_t pos, cues;
    size_t k, j;
    unsigned ls = 4;
    mkv_seek_hit hit;
    ebml_elem e;
    mr_status st = MR_OK;

    if (!m->video_track) return MR_EUNSUPPORTED;
    if (m->video.config && m->video.config_len >= 5 && m->video.config[0] == 1)
        ls = (m->video.config[4] & 3u) + 1u;
    if (ls == 3) ls = 4;
    memset(&hit, 0, sizeof hit);
    memset(&x, 0, sizeof x);
    mr_mkv_seek_used_cues = 0;

    /* 1: start points - the Cues index, else the cluster headers */
    cues = find_cues(m);
    if (cues && load_cues(m, cues, &x)) mr_mkv_seek_used_cues = 1;
    else {
        x.n = 0;
        if (!walk_clusters(m, target_us, &x)) { st = MR_EFORMAT; goto out; }
    }
    for (k = 0; k + 1 < x.n && x.us[k + 1] <= target_us; k++) {}

    /* 2: last IDR at or before the target: from start point k forward over
     * the clusters that begin before the target, then back over earlier
     * start points (at most ~2 minutes) */
    for (pos = x.off[k]; pos < m->segment_end;) {
        uint64_t tc = 0, end = 0;
        if (!elem_at(m, pos, m->segment_end, &e) || e.unknown || e.end <= pos) break;
        if (e.id != 0x1f43b675) { pos = e.end; continue; }
        if (!scan_cluster(m, pos, target_us, 0, ls, &hit, &tc, &end)) break;
        if (tc > target_us) break;
        pos = end;
    }
    for (j = k; !hit.found && j-- > 0;) {
        if (target_us > x.us[j] && target_us - x.us[j] > MKV_SEEK_BACK_US) break;
        if (!scan_cluster(m, x.off[j], target_us, 0, ls, &hit, NULL, NULL)) break;
    }

    /* 3: otherwise the first IDR after it */
    for (pos = x.off[k]; !hit.found && pos < m->segment_end; pos = e.end) {
        if (!elem_at(m, pos, m->segment_end, &e) || e.unknown || e.end <= pos) break;
        if (e.id != 0x1f43b675) continue;
        if (!scan_cluster(m, pos, target_us, 1, ls, &hit, NULL, NULL)) break;
    }
    if (!hit.found) { st = MR_EUNSUPPORTED; goto out; }

    m->cursor = hit.elem;
    m->cluster_end = hit.cluster_end;
    m->cluster_timecode = hit.cluster_tc;
    m->lace_count = m->lace_index = 0;
    if (out_us) *out_us = hit.pts_us;
out:
    free(x.off); free(x.us);
    return st;
}

void mr_mkv_rewind(mr_mkv *m)
{
    m->cursor = m->first_cluster; m->cluster_end = 0; m->cluster_timecode = 0;
    m->lace_count = m->lace_index = 0;
}

void mr_mkv_close(mr_mkv *m)
{
    if (!m) return;
    free(m->scratch); free(m->packet); free(m->video_config);
    m->scratch = m->packet = m->video_config = NULL;
}
