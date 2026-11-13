/*
 * MintVID - minimal QuickTime (MOV) demuxer implementation.
 *
 * MOV is a tree of atoms [size:4][type:4][payload]. We locate the video track's
 * sample tables and flatten them into a per-frame (offset,size) index into the
 * file, so mr_mov_next_packet just walks that index. Only what the player needs
 * is parsed - no edit lists, no fragmented MP4.
 */
#include "mr_mov.h"
#include "mr_rawvideo.h"
#include <stdlib.h>
#include <string.h>

struct mov_sample {
    uint32_t off; uint32_t size; uint32_t t_ms;
    uint64_t pts_us;      /* presentation timestamp from stts + optional ctts */
    uint8_t is_video;
    uint8_t is_keyframe; /* video only: sync sample (stss), or every sample
                          * when the track has no stss (spec default) */
    uint8_t has_pts;
};

static uint32_t rb32(const uint8_t *p){ return mr_rb32(p); }
static uint16_t rb16(const uint8_t *p){ return mr_rb16(p); }
static uint64_t rb64(const uint8_t *p){
    return ((uint64_t)mr_rb32(p) << 32) | mr_rb32(p + 4);
}
/* atom types compared as big-endian 4CC values */
#define T(a,b,c,d) (((uint32_t)(a)<<24)|((uint32_t)(b)<<16)|((uint32_t)(c)<<8)|(uint32_t)(d))

/* Find the first child atom of `type` within [p,end); returns payload pointer
 * and sets *size to the payload length. */
static const uint8_t *find_atom(const uint8_t *p, const uint8_t *end,
                                uint32_t type, uint32_t *size)
{
    while (p + 8 <= end) {
        uint64_t asz = rb32(p);
        uint32_t t   = rb32(p + 4);
        int hdr = 8;
        if (asz == 1) {                 /* 64-bit extended size */
            if (p + 16 > end) break;
            asz = rb64(p + 8);
            hdr = 16;
        } else if (asz == 0) {          /* extends to end */
            asz = (uint64_t)(end - p);
        }
        if (asz < (uint64_t)hdr) break;
        if (p + asz > end) asz = (uint64_t)(end - p);
        if (t == type) { *size = (uint32_t)(asz - hdr); return p + hdr; }
        p += asz;
    }
    return NULL;
}

static mr_status read_stbl_segments(mr_mov *m, const uint8_t *stbl,
                                    const uint8_t *end, int is_video,
                                    int coalesce_chunks, uint32_t timescale);

/* ISO/IEC 14496 descriptor lengths use up to four base-128 bytes. */
static int descriptor_header(const uint8_t *p, const uint8_t *end,
                             uint8_t *tag, const uint8_t **body,
                             const uint8_t **body_end)
{
    uint32_t n = 0;
    int i;
    if (p >= end) return 0;
    *tag = *p++;
    for (i = 0; i < 4; i++) {
        uint8_t b;
        if (p >= end) return 0;
        b = *p++;
        n = (n << 7) | (uint32_t)(b & 0x7f);
        if (!(b & 0x80)) {
            if ((uint64_t)(end - p) < n) return 0;
            *body = p;
            *body_end = p + n;
            return 1;
        }
    }
    return 0;
}

/* Find DecoderSpecificInfo (tag 0x05) inside an ESDS descriptor tree. */
static int find_decoder_config(const uint8_t *p, const uint8_t *end,
                               const uint8_t **cfg, uint32_t *cfg_len,
                               int depth)
{
    while (p < end && depth < 4) {
        uint8_t tag;
        const uint8_t *body, *body_end, *child;
        if (!descriptor_header(p, end, &tag, &body, &body_end)) return 0;
        if (tag == 0x05) {
            *cfg = body;
            *cfg_len = (uint32_t)(body_end - body);
            return 1;
        }

        child = body;
        if (tag == 0x03) {              /* ES_Descriptor */
            uint8_t flags;
            if (child + 3 > body_end) goto next;
            child += 2; flags = *child++;
            if (flags & 0x80) {                 /* dependsOn_ES_ID */
                if (body_end - child < 2) goto next;
                child += 2;
            }
            if (flags & 0x40) {                 /* URL */
                uint8_t url_len;
                if (child >= body_end) goto next;
                url_len = *child++;
                if (body_end - child < url_len) goto next;
                child += url_len;
            }
            if (flags & 0x20) {                 /* OCR_ES_ID */
                if (body_end - child < 2) goto next;
                child += 2;
            }
        } else if (tag == 0x04) {       /* DecoderConfigDescriptor */
            if (body_end - child < 13) goto next;
            child += 13;
        }
        if (child <= body_end &&
            find_decoder_config(child, body_end, cfg, cfg_len, depth + 1))
            return 1;
next:
        p = body_end;
    }
    return 0;
}

/* Order for delivery: ascending presentation time, so audio and video stay
 * finely interleaved even when the file stores them in coarse per-track chunks
 * (which otherwise starve the single-threaded player's A/V pacing). Ties put
 * audio first so its buffer leads the video frame it accompanies; the final
 * tiebreak on offset keeps each track in decode order. */
static int cmp_time(const void *a, const void *b)
{
    const struct mov_sample *sa = (const struct mov_sample *)a;
    const struct mov_sample *sb = (const struct mov_sample *)b;
    if (sa->t_ms != sb->t_ms) return (sa->t_ms > sb->t_ms) - (sa->t_ms < sb->t_ms);
    if (sa->is_video != sb->is_video) return (int)sa->is_video - (int)sb->is_video;
    return (sa->off > sb->off) - (sa->off < sb->off);
}


/* mdia/hdlr -> handler type ('vide' / 'soun'). */
static uint32_t track_handler(const uint8_t *mdia, uint32_t mdia_sz)
{
    uint32_t sz;
    const uint8_t *h = find_atom(mdia, mdia + mdia_sz, T('h','d','l','r'), &sz);
    if (!h || sz < 12) return 0;
    return rb32(h + 8);
}

/* Parse the video stbl into m->samples + m->video geometry/fourcc. */
static mr_status parse_video(mr_mov *m, const uint8_t *stbl, uint32_t stbl_sz,
                             const uint8_t *mdia, uint32_t mdia_sz)
{
    uint32_t first_sample = m->nsamples;
    const uint8_t *end = stbl + stbl_sz;
    uint32_t sz;
    const uint8_t *stsd = find_atom(stbl, end, T('s','t','s','d'), &sz);
    if (stsd && sz >= 16) {
        const uint8_t *e = stsd + 8;            /* skip ver/flags + count   */
        uint32_t entry_sz = rb32(e);
        /* codec 4CC is stored big-endian at entry+4; pack it the same way the
         * registry does (MR_FOURCC), so 'cvid' matches mr_codec_cinepak. */
        m->video.fourcc = MR_FOURCC(e[4], e[5], e[6], e[7]);
        m->video.width  = rb16(e + 32);
        m->video.height = rb16(e + 34);
        m->video.valid  = 1;
        /* VisualSampleEntry is 86 bytes including size/type.  H.264 stores
         * DecoderConfigurationRecord as a child avcC atom after it. */
        if (m->video.fourcc == MR_FOURCC('a','v','c','1') &&
            entry_sz >= 94 && entry_sz <= sz - 8) {
            uint32_t avcc_sz;
            const uint8_t *avcc = find_atom(e + 86, e + entry_sz,
                                            T('a','v','c','C'), &avcc_sz);
            if (avcc && avcc_sz >= 7) {
                m->video.config = avcc;
                m->video.config_len = avcc_sz;
            }
        } else if ((m->video.fourcc == MR_FOURCC('m','p','4','v') ||
                    m->video.fourcc == MR_FOURCC('M','P','4','V')) &&
                   entry_sz >= 86 && entry_sz <= sz - 8) {
            /* MPEG-4 Part 2 keeps its VOL only in the esds decoder config
             * (VOS/VO/VOL) - the frames carry bare VOPs. Hand the borrowed
             * DecoderSpecificInfo to the decoder so it parses the real VOL
             * instead of guessing. */
            uint32_t esds_sz;
            const uint8_t *cfg;
            uint32_t cfg_len;
            const uint8_t *esds = find_atom(e + 86, e + entry_sz,
                                            T('e','s','d','s'), &esds_sz);
            if (esds && esds_sz >= 4 &&
                find_decoder_config(esds + 4, esds + esds_sz,
                                    &cfg, &cfg_len, 0) && cfg_len) {
                m->video.config = cfg;
                m->video.config_len = cfg_len;
            }
        }
    }
    /* frame rate: mdhd timescale over the *average* stts delta. Using only the
     * first delta mistimes variable-frame-rate clips whose stts has many
     * entries (e.g. CDR-Dinner: first delta 33 -> 30.3 fps, but the true
     * average is ~41 -> 24.2 fps), making playback run fast. */
    {
        uint32_t s2;
        const uint8_t *mdhd = find_atom(mdia, mdia + mdia_sz,
                                        T('m','d','h','d'), &s2);
        if (mdhd && s2 >= 20) m->video.rate = rb32(mdhd + 12); /* timescale */
        const uint8_t *stts = find_atom(stbl, end, T('s','t','t','s'), &s2);
        if (stts && s2 >= 16) {
            uint32_t n = rb32(stts + 4);              /* entry_count           */
            uint32_t avail = (s2 - 8) / 8;
            uint64_t tot_count = 0, tot_dur = 0;
            uint32_t i;
            if (n > avail) n = avail;
            for (i = 0; i < n; i++) {
                uint32_t cnt = rb32(stts + 8 + (size_t)i * 8);
                uint32_t del = rb32(stts + 12 + (size_t)i * 8);
                tot_count += cnt;
                tot_dur   += (uint64_t)cnt * del;
            }
            if (tot_count && tot_dur)
                m->video.scale = (uint32_t)((tot_dur + tot_count / 2)
                                            / tot_count);
            else
                m->video.scale = rb32(stts + 12);     /* fall back: 1st delta  */
        }
        if (!m->video.scale) m->video.scale = m->video.rate ? m->video.rate : 1;
    }

    {
        mr_status st = read_stbl_segments(m, stbl, end, 1 /*video*/,
                                         0 /*per-sample*/,
                                         m->video.rate /*mdhd timescale*/);
        /* stsz describes the complete packet, not rowBytes. QuickTime raw
         * packets may contain a large codec-private/padding tail, so never
         * distribute sample size across the displayed rows. */
        if (st == MR_OK && mr_rawvideo_is_uyvy422(m->video.fourcc) &&
            first_sample < m->nsamples && m->video.height > 0) {
            uint32_t stride = mr_rawvideo_uyvy422_stride(m->video.width);
            m->rawvideo_config[0] = (uint8_t)stride;
            m->rawvideo_config[1] = (uint8_t)(stride >> 8);
            m->rawvideo_config[2] = (uint8_t)(stride >> 16);
            m->rawvideo_config[3] = (uint8_t)(stride >> 24);
            m->video.config = m->rawvideo_config;
            m->video.config_len = 4;
        }
        return st;
    }
}

/* Append a segment to the growing interleaved index. */
static mr_status push_seg(mr_mov *m, uint32_t off, uint32_t size, int is_video,
                          uint32_t t_ms, int is_keyframe,
                          int has_pts, uint64_t pts_us)
{
    if (!size) return MR_OK;
    if (m->nsamples >= m->cap) {
        uint32_t nc = m->cap ? m->cap * 2 : 256;
        struct mov_sample *ns = (struct mov_sample *)
            realloc(m->samples, (size_t)nc * sizeof *ns);
        if (!ns) return MR_ENOMEM;
        m->samples = ns; m->cap = nc;
    }
    m->samples[m->nsamples].off         = off;
    m->samples[m->nsamples].size        = size;
    m->samples[m->nsamples].t_ms        = t_ms;
    m->samples[m->nsamples].is_video    = (uint8_t)is_video;
    m->samples[m->nsamples].is_keyframe = (uint8_t)is_keyframe;
    m->samples[m->nsamples].has_pts     = (uint8_t)has_pts;
    m->samples[m->nsamples].pts_us      = pts_us;
    m->nsamples++;
    return MR_OK;
}

/* Sync-sample (stss) cursor: sample numbers are 1-based and ascending. A
 * track with no stss atom has every sample as a random access point (spec
 * default), so has_stss doubles as "no stss -> always a keyframe". */
struct stss_iter { const uint8_t *nums; uint32_t cnt, idx; int has_stss; };
static void stss_iter_init(struct stss_iter *it, const uint8_t *stbl,
                           const uint8_t *end)
{
    uint32_t sz;
    const uint8_t *stss = find_atom(stbl, end, T('s','t','s','s'), &sz);
    it->has_stss = stss && sz >= 8;
    it->nums = it->has_stss ? stss + 8 : NULL;
    it->cnt  = it->has_stss ? rb32(stss + 4) : 0;
    if (it->has_stss && (uint64_t)it->cnt * 4 > (uint64_t)(sz - 8))
        it->cnt = (sz - 8) / 4;
    it->idx = 0;
}
/* sample_no is the 1-based decode-order sample number being pushed now. */
static int stss_is_key(struct stss_iter *it, uint32_t sample_no)
{
    if (!it->has_stss) return 1;      /* no stss: every sample is a keyframe */
    while (it->idx < it->cnt && rb32(it->nums + (size_t)it->idx * 4) < sample_no)
        it->idx++;
    return it->idx < it->cnt && rb32(it->nums + (size_t)it->idx * 4) == sample_no;
}

/* Flatten a track's stbl into the shared index. Video emits one segment per
 * sample (frame); audio coalesces each chunk into a single PCM segment (far
 * fewer, larger packets). */
/* Decode-time iterator over stts (sample_count, sample_delta).  Keep the
 * timestamp in media-timescale ticks so MOV/MP4 can expose real packet PTS to
 * BerryVP instead of falling back to the H.264 SPS nominal frame rate. */
struct stts_iter {
    const uint8_t *e; uint32_t cnt, idx;   /* entries base, total, cursor      */
    uint32_t run_left, delta;              /* remaining in the current run     */
    uint64_t dts;                          /* cumulative, in media timescale   */
    uint32_t timescale;
};
static void stts_iter_init(struct stts_iter *it, const uint8_t *stbl,
                           const uint8_t *end, uint32_t timescale)
{
    uint32_t sz;
    const uint8_t *stts = find_atom(stbl, end, T('s','t','t','s'), &sz);
    it->e = (stts && sz >= 8) ? stts + 8 : NULL;
    it->cnt = it->e ? rb32(stts + 4) : 0;
    if (it->e && (uint64_t)it->cnt * 8 > (uint64_t)(sz - 8))
        it->cnt = (sz - 8) / 8;
    it->idx = it->run_left = it->delta = 0;
    it->dts = 0;
    it->timescale = timescale ? timescale : 1;
}
static uint64_t stts_next_ticks(struct stts_iter *it)
{
    uint64_t ticks;
    while (it->run_left == 0 && it->idx < it->cnt) {
        it->run_left = rb32(it->e + (size_t)it->idx * 8);
        it->delta    = rb32(it->e + (size_t)it->idx * 8 + 4);
        it->idx++;
    }
    ticks = it->dts;
    it->dts += it->delta;
    if (it->run_left) it->run_left--;
    return ticks;
}
static uint32_t ticks_to_ms(uint64_t ticks, uint32_t timescale)
{
    if (!timescale) timescale = 1;
    return (uint32_t)((ticks * 1000ULL) / timescale);
}
static uint64_t ticks_to_us(uint64_t ticks, uint32_t timescale)
{
    if (!timescale) timescale = 1;
    return (ticks * 1000000ULL) / timescale;
}

/* Composition-time offsets for H.264 B-frame streams.  stts is decode time;
 * ctts turns that into presentation time without reordering the decode-order
 * sample index.  Version 0 offsets are unsigned, version 1 offsets signed. */
struct ctts_iter {
    const uint8_t *e;
    uint32_t cnt, idx, run_left;
    int64_t offset;
    uint8_t version;
    int present;
};
static void ctts_iter_init(struct ctts_iter *it, const uint8_t *stbl,
                           const uint8_t *end)
{
    uint32_t sz;
    const uint8_t *ctts = find_atom(stbl, end, T('c','t','t','s'), &sz);
    memset(it, 0, sizeof *it);
    if (!ctts || sz < 8) return;
    it->version = ctts[0];
    if (it->version > 1) return;
    it->cnt = rb32(ctts + 4);
    if ((uint64_t)it->cnt * 8 > (uint64_t)(sz - 8))
        it->cnt = (sz - 8) / 8;
    if (!it->cnt) return;
    it->e = ctts + 8;
    it->present = 1;
}
static int64_t ctts_next_offset(struct ctts_iter *it)
{
    uint32_t raw;
    if (!it->present) return 0;
    while (it->run_left == 0 && it->idx < it->cnt) {
        it->run_left = rb32(it->e + (size_t)it->idx * 8);
        raw = rb32(it->e + (size_t)it->idx * 8 + 4);
        it->offset = it->version == 1 ? (int64_t)(int32_t)raw : (int64_t)raw;
        it->idx++;
    }
    if (it->run_left) it->run_left--;
    return it->offset;
}

static mr_status read_stbl_segments(mr_mov *m, const uint8_t *stbl,
                                    const uint8_t *end, int is_video,
                                    int coalesce_chunks, uint32_t timescale)
{
    uint32_t stsz_sz, stsc_sz, stco_sz;
    const uint8_t *stsz = find_atom(stbl, end, T('s','t','s','z'), &stsz_sz);
    const uint8_t *stsc = find_atom(stbl, end, T('s','t','s','c'), &stsc_sz);
    const uint8_t *stco = find_atom(stbl, end, T('s','t','c','o'), &stco_sz);
    struct stts_iter ti;
    struct ctts_iter ci;
    struct stss_iter ki;
    int64_t video_pts_base = 0;
    int video_pts_base_set = 0;
    int co64 = 0;
    if (!stco) { stco = find_atom(stbl, end, T('c','o','6','4'), &stco_sz); co64 = 1; }
    if (!stsz || !stsc || !stco || stsz_sz < 12 || stsc_sz < 8 || stco_sz < 8)
        return MR_EFORMAT;

    uint32_t uniform  = rb32(stsz + 4);
    uint32_t nsamp    = rb32(stsz + 8);
    uint32_t stsc_cnt = rb32(stsc + 4);
    uint32_t nchunks  = rb32(stco + 4);
    if (!nsamp || !nchunks) return MR_EFORMAT;

    const uint8_t *sizes = stsz + 12;
    const uint8_t *sc    = stsc + 8;
    const uint8_t *co    = stco + 8;

    stts_iter_init(&ti, stbl, end, timescale);
    ctts_iter_init(&ci, stbl, end);
    memset(&ki, 0, sizeof ki);
    if (is_video) stss_iter_init(&ki, stbl, end);

    uint32_t si = 0, e;
    for (e = 0; e < stsc_cnt && si < nsamp; e++) {
        uint32_t first = rb32(sc + e * 12);
        uint32_t spc   = rb32(sc + e * 12 + 4);
        uint32_t last  = (e + 1 < stsc_cnt) ? rb32(sc + (e + 1) * 12) - 1
                                            : nchunks;
        uint32_t chunk;
        for (chunk = first; chunk <= last && chunk <= nchunks && si < nsamp;
             chunk++) {
            uint64_t off = co64 ? rb64(co + (uint64_t)(chunk - 1) * 8)
                                : rb32(co + (uint64_t)(chunk - 1) * 4);
            uint32_t k;
            if (coalesce_chunks) {
                uint32_t start = (uint32_t)off, total = 0, seg_ms = 0;
                for (k = 0; k < spc && si < nsamp; k++) {
                    uint64_t dts_ticks = stts_next_ticks(&ti);
                    uint32_t sms = ticks_to_ms(dts_ticks, ti.timescale);
                    if (k == 0) seg_ms = sms;
                    total += uniform ? uniform : rb32(sizes + (uint64_t)si * 4);
                    si++;
                }
                if (push_seg(m, start, total, is_video, seg_ms, 0,
                             1, (uint64_t)seg_ms * 1000ULL) != MR_OK)
                    return MR_ENOMEM;
            } else {
                for (k = 0; k < spc && si < nsamp; k++) {
                    uint32_t ssz = uniform ? uniform
                                           : rb32(sizes + (uint64_t)si * 4);
                    uint64_t dts_ticks = stts_next_ticks(&ti);
                    uint32_t sms = ticks_to_ms(dts_ticks, ti.timescale);
                    int64_t pts_ticks_s = (int64_t)dts_ticks;
                    uint64_t pts_us;
                    int is_key = is_video ? stss_is_key(&ki, si + 1) : 0;
                    if (is_video) {
                        pts_ticks_s += ctts_next_offset(&ci);
                        /* Common MP4 B-frame files use a positive ctts offset
                         * plus an edit list to make displayed time start at 0.
                         * This demuxer intentionally does not implement edts;
                         * normalize the first video composition timestamp to
                         * zero while retaining all relative PTS intervals. */
                        if (!video_pts_base_set) {
                            video_pts_base = pts_ticks_s;
                            video_pts_base_set = 1;
                        }
                        pts_ticks_s -= video_pts_base;
                    }
                    if (pts_ticks_s < 0) pts_ticks_s = 0;
                    pts_us = ticks_to_us((uint64_t)pts_ticks_s, ti.timescale);
                    if (push_seg(m, (uint32_t)off, ssz, is_video, sms,
                                is_key, 1, pts_us) != MR_OK)
                        return MR_ENOMEM;
                    off += ssz;
                    si++;
                }
            }
        }
    }
    return MR_OK;
}

static void parse_audio(mr_mov *m, const uint8_t *stbl, uint32_t stbl_sz,
                        uint32_t timescale)
{
    uint32_t sz;
    const uint8_t *stsd = find_atom(stbl, stbl + stbl_sz,
                                    T('s','t','s','d'), &sz);
    if (!stsd || sz < 44) return;
    const uint8_t *e = stsd + 8;                /* audio sample entry       */
    uint32_t entry_sz = rb32(e);
    uint32_t fmt = rb32(e + 4);
    uint16_t version = rb16(e + 16);
    if (entry_sz < 36 || entry_sz > sz - 8) return;
    m->audio.channels    = rb16(e + 24);
    m->audio.bits_per_sample = rb16(e + 26);
    m->audio.sample_rate = rb32(e + 32) >> 16;  /* 16.16 fixed              */
    /* Keep the public FourCC in the same byte order as video FourCCs so
     * callers can print/examine it portably; fmt remains big-endian for atom
     * comparisons inside this parser. */
    m->audio.codec_tag = MR_FOURCC(e[4], e[5], e[6], e[7]);
    /* map common uncompressed PCM 4CCs to the WAVE PCM tag */
    if (fmt == T('s','o','w','t') || fmt == T('t','w','o','s') ||
        fmt == T('r','a','w',' ') || fmt == T('N','O','N','E') ||
        fmt == T('l','p','c','m') ||
        fmt == T('i','n','2','4') || fmt == T('i','n','3','2'))
        m->audio.format_tag = MR_AUDIO_FORMAT_PCM;
    else if (fmt == T('.','m','p','3'))
        m->audio.format_tag = MR_AUDIO_FORMAT_MP3;
    else if (fmt == T('a','c','-','3'))
        m->audio.format_tag = MR_AUDIO_FORMAT_AC3;
    else if (fmt == T('e','c','-','3'))
        m->audio.format_tag = MR_AUDIO_FORMAT_EAC3;
    else if (fmt == T('m','p','4','a')) {
        const uint8_t *entry_end;
        const uint8_t *child;
        const uint8_t *esds, *cfg;
        uint32_t esds_sz, cfg_len;
        m->audio.format_tag = MR_AUDIO_FORMAT_AAC;

        entry_end = e + entry_sz;
        child = e + 36;
        if (version == 1) child += 16;
        else if (version == 2) child += 36;
        esds = child <= entry_end
             ? find_atom(child, entry_end, T('e','s','d','s'), &esds_sz)
             : NULL;
        if (!esds && child <= entry_end) {
            uint32_t wave_sz;
            const uint8_t *wave = find_atom(child, entry_end,
                                            T('w','a','v','e'), &wave_sz);
            if (wave)
                esds = find_atom(wave, wave + wave_sz,
                                 T('e','s','d','s'), &esds_sz);
        }
        /* esds starts with version/flags, followed by MPEG-4 descriptors. */
        if (esds && esds_sz >= 4 &&
            find_decoder_config(esds + 4, esds + esds_sz,
                                &cfg, &cfg_len, 0)) {
            if (cfg_len > MR_AUDIO_CONFIG_MAX) cfg_len = MR_AUDIO_CONFIG_MAX;
            memcpy(m->audio.config, cfg, cfg_len);
            m->audio.config_len = (uint8_t)cfg_len;
        }
    }
    m->audio.valid = 1;
    if (m->audio.format_tag == MR_AUDIO_FORMAT_PCM) {
        /* QuickTime signedness is part of the sample entry, not implied by
         * the sample width.  In particular, 8-bit 'twos' is signed while
         * 8-bit 'raw ' and 'NONE' are unsigned. */
        m->audio.pcm_signed = fmt == T('t','w','o','s') ||
                              fmt == T('s','o','w','t') ||
                              fmt == T('i','n','2','4') ||
                              fmt == T('i','n','3','2');
        m->audio.pcm_big_endian = fmt == T('t','w','o','s') ||
                                  fmt == T('i','n','2','4') ||
                                  fmt == T('i','n','3','2');
        m->audio.block_align = (uint16_t)(m->audio.channels *
                                         ((m->audio.bits_per_sample + 7) / 8));
    }

    /* Compressed access units must keep their sample boundaries.  PCM remains
     * coalesced per chunk to avoid flooding the player with tiny packets. */
    read_stbl_segments(m, stbl, stbl + stbl_sz, 0 /*audio*/,
                       m->audio.format_tag == MR_AUDIO_FORMAT_PCM, timescale);
}

static void parse_trak(mr_mov *m, const uint8_t *trak, uint32_t trak_sz)
{
    uint32_t sz;
    const uint8_t *mdia = find_atom(trak, trak + trak_sz,
                                    T('m','d','i','a'), &sz);
    if (!mdia) return;
    uint32_t mdia_sz = sz, minf_sz, stbl_sz;
    uint32_t htype = track_handler(mdia, mdia_sz);
    const uint8_t *minf = find_atom(mdia, mdia + mdia_sz,
                                    T('m','i','n','f'), &minf_sz);
    if (!minf) return;
    const uint8_t *stbl = find_atom(minf, minf + minf_sz,
                                    T('s','t','b','l'), &stbl_sz);
    if (!stbl) return;

    if (htype == T('v','i','d','e') && !m->video.valid)
        parse_video(m, stbl, stbl_sz, mdia, mdia_sz);
    else if (htype == T('s','o','u','n') && !m->audio.valid) {
        uint32_t mdhd_sz, ts = 0;
        const uint8_t *mdhd = find_atom(mdia, mdia + mdia_sz,
                                        T('m','d','h','d'), &mdhd_sz);
        if (mdhd && mdhd_sz >= 20) ts = rb32(mdhd + 12);   /* media timescale */
        parse_audio(m, stbl, stbl_sz, ts);
    }
}

static mr_status parse_moov(mr_mov *m, const uint8_t *moov, uint32_t sz)
{
    /* iterate every trak in moov */
    const uint8_t *p = moov, *end = moov + sz;
    for (;;) {
        uint32_t tsz;
        const uint8_t *trak = find_atom(p, end, T('t','r','a','k'), &tsz);
        if (!trak) break;
        parse_trak(m, trak, tsz);
        p = trak + tsz;                         /* advance past this trak   */
    }

    if (!m->video.valid || !m->samples) return MR_EFORMAT;

    /* Deliver by presentation time so audio and video arrive finely
     * interleaved for A/V pacing regardless of how coarsely the file chunks
     * each track in mdat. Over HTTP this makes reads alternate between the two
     * chunk regions; the source's read-ahead buffer absorbs that so it does not
     * turn into a range re-request per packet. */
    qsort(m->samples, m->nsamples, sizeof *m->samples, cmp_time);
    return MR_OK;
}

mr_status mr_mov_open(mr_mov *m, const uint8_t *buf, size_t len)
{
    uint32_t sz;
    const uint8_t *moov;
    memset(m, 0, sizeof *m);
    m->buf = buf;
    m->len = len;

    moov = find_atom(buf, buf + len, T('m','o','o','v'), &sz);
    if (!moov) return MR_EFORMAT;
    return parse_moov(m, moov, sz);
}

static int file_read_at(mr_mov *m, size_t off, void *dst, size_t len)
{
    return mr_source_read_at(m->source, off, dst, len);
}

mr_status mr_mov_open_source(mr_mov *m, mr_source *source, size_t len)
{
    size_t pos = 0;

    memset(m, 0, sizeof *m);
    if (!source || len < 8) return MR_EFORMAT;
    m->source = source;
    m->len = len;
    m->file_backed = 1;

    /* Locate moov without reading mdat.  Fast-start MP4 has moov near the
     * front; regular files often place it at EOF, which is still one seek per
     * top-level atom rather than a read of the intervening media payload. */
    while (pos + 8 <= len) {
        uint8_t head[16];
        uint64_t atom_size;
        uint32_t type;
        size_t hdr = 8;

        if (!file_read_at(m, pos, head, 8)) return MR_EFORMAT;
        atom_size = rb32(head);
        type = rb32(head + 4);
        if (atom_size == 1) {
            if (pos + 16 > len || !file_read_at(m, pos + 8, head + 8, 8))
                return MR_EFORMAT;
            atom_size = rb64(head + 8);
            hdr = 16;
        } else if (atom_size == 0) {
            atom_size = (uint64_t)(len - pos);
        }
        if (atom_size < hdr || atom_size > (uint64_t)(len - pos))
            return MR_EFORMAT;

        if (type == T('m','o','o','v')) {
            uint64_t payload64 = atom_size - hdr;
            uint32_t payload;
            mr_status st;
            if (payload64 > 32UL * 1024 * 1024) return MR_EFORMAT;
            payload = (uint32_t)payload64;
            m->metadata = (uint8_t *)malloc(payload);
            if (!m->metadata) return MR_ENOMEM;
            if (!file_read_at(m, pos + hdr, m->metadata, payload))
                return MR_EFORMAT;
            st = parse_moov(m, m->metadata, payload);
            return st;
        }
        pos += (size_t)atom_size;
    }
    return MR_EFORMAT;
}

static uint32_t avc1_read_nal_size(const uint8_t *p, unsigned bytes)
{
    uint32_t n = 0;
    unsigned i;
    for (i = 0; i < bytes; i++) n = (n << 8) | p[i];
    return n;
}

/*
 * avc1 stores its authoritative SPS/PPS in the avcC sample-entry atom.  Some
 * hardware encoders nevertheless repeat parameter sets in the first MP4
 * sample; AMD AMF is one real-world example and can repeat an SPS with extra
 * VUI colour/timing fields even though the coded geometry is unchanged.
 *
 * MintVID has already fed avcC to libavc and allocated its shared display
 * buffers before the first sample arrives.  Feeding a second SPS at that point
 * can make libavc treat the first access unit as a sequence reconfiguration,
 * leaving playback stuck at "buffering first frame" on Amiga.  avc3 is the
 * ISO BMFF sample-entry variant intended for in-band parameter-set changes;
 * MintVID supports avc1 here, so discard type-7 SPS and type-8 PPS NALs from
 * avc1 samples and keep avcC authoritative.
 *
 * The common path does not allocate or copy anything: first scan the sample,
 * and only compact it when a stray SPS/PPS is actually present.  File-backed
 * packets already live in m->packet_buf and are compacted there in place;
 * memory-backed MOV input borrows that same scratch buffer only for affected
 * samples.  MKV and MPEG-TS/HLS never pass through this MOV-specific helper.
 */
static mr_status filter_avc1_parameter_sets(mr_mov *m,
                                            const uint8_t *src, uint32_t len,
                                            const uint8_t **out_data,
                                            uint32_t *out_len)
{
    const uint8_t *cfg;
    unsigned nls;
    uint32_t p = 0, kept = 0;
    int found = 0;
    uint8_t *dst;

    *out_data = src;
    *out_len = len;
    if (m->video.fourcc != MR_FOURCC('a','v','c','1')) return MR_OK;
    cfg = m->video.config;
    if (!cfg || m->video.config_len < 7 || cfg[0] != 1) return MR_OK;
    nls = (unsigned)(cfg[4] & 3u) + 1u;

    while (p < len) {
        uint32_t n;
        unsigned type;
        if (len - p < nls) return MR_OK; /* let the decoder report malformed AVCC */
        n = avc1_read_nal_size(src + p, nls);
        p += nls;
        if (!n || n > len - p) return MR_OK;
        type = src[p] & 0x1fu;
        if (type == 7u || type == 8u)
            found = 1;
        else
            kept += nls + n;
        p += n;
    }
    if (!found || !kept) return MR_OK;

    if (src == m->packet_buf) {
        dst = m->packet_buf;
    } else {
        if (m->packet_cap < kept) {
            uint8_t *nb = (uint8_t *)realloc(m->packet_buf, kept);
            if (!nb) return MR_ENOMEM;
            m->packet_buf = nb;
            m->packet_cap = kept;
        }
        dst = m->packet_buf;
    }

    p = 0;
    kept = 0;
    while (p < len) {
        uint32_t start = p;
        uint32_t n = avc1_read_nal_size(src + p, nls);
        unsigned type;
        p += nls;
        type = src[p] & 0x1fu;
        p += n;
        if (type != 7u && type != 8u) {
            uint32_t bytes = nls + n;
            if (dst == src)
                memmove(dst + kept, src + start, bytes);
            else
                memcpy(dst + kept, src + start, bytes);
            kept += bytes;
        }
    }
    *out_data = dst;
    *out_len = kept;
    return MR_OK;
}

mr_status mr_mov_next_packet(mr_mov *m, mr_packet *pkt)
{
    while (m->cursor < m->nsamples) {
        struct mov_sample *s = &m->samples[m->cursor++];
        mr_status filter_status;
        if ((size_t)s->off > m->len ||
            (size_t)s->size > m->len - (size_t)s->off)
            continue;
        pkt->is_video = s->is_video;
        pkt->has_pts = s->has_pts;
        pkt->pts_us = s->pts_us;
        if (m->file_backed) {
            if (m->packet_cap < s->size) {
                uint8_t *nb = (uint8_t *)realloc(m->packet_buf, s->size);
                if (!nb) return MR_ENOMEM;
                m->packet_buf = nb;
                m->packet_cap = s->size;
            }
            if (s->size &&
                !file_read_at(m, (size_t)s->off, m->packet_buf, s->size))
                return MR_EFORMAT;
            pkt->data = m->packet_buf;
        } else {
            pkt->data = m->buf + s->off;
        }
        pkt->len = s->size;
        if (s->is_video) {
            const uint8_t *filtered_data;
            uint32_t filtered_len;
            filter_status = filter_avc1_parameter_sets(
                m, pkt->data, pkt->len, &filtered_data, &filtered_len);
            if (filter_status != MR_OK) return filter_status;
            pkt->data = filtered_data;
            pkt->len = filtered_len;
        }
        return MR_OK;
    }
    return MR_EAGAIN;
}

void mr_mov_rewind(mr_mov *m) { m->cursor = 0; }

/* Seek to the nearest video keyframe at or before target_ms. m->samples[] is
 * sorted ascending by decode-time (t_ms), video-track order preserved within
 * that, so a linear scan back from the first sample at/after target_ms finds
 * it in one pass; the cursor lands on that keyframe's own index (any audio
 * strictly before it in decode time is skipped, same as any real player
 * jumping to a new position). */
mr_status mr_mov_seek(mr_mov *m, uint64_t target_ms, uint64_t *out_ms)
{
    uint32_t lo, hi, mid, hit, i;
    if (!m->nsamples) return MR_EUNSUPPORTED;

    lo = 0; hi = m->nsamples;             /* [lo,hi): first index with t_ms >= target */
    while (lo < hi) {
        mid = lo + (hi - lo) / 2;
        if ((uint64_t)m->samples[mid].t_ms < target_ms) lo = mid + 1;
        else hi = mid;
    }
    hit = m->nsamples;                    /* nsamples = "not found yet"      */
    for (i = lo; i > 0; i--) {
        uint32_t j = i - 1;
        if (m->samples[j].is_video && m->samples[j].is_keyframe) { hit = j; break; }
    }
    if (hit == m->nsamples) {
        /* target is before every keyframe (or before the first one at/after
         * it) - use the first video keyframe in the file, i.e. seek-to-start. */
        for (i = 0; i < m->nsamples; i++)
            if (m->samples[i].is_video && m->samples[i].is_keyframe) { hit = i; break; }
    }
    if (hit == m->nsamples) return MR_EUNSUPPORTED; /* no video keyframe at all */

    m->cursor = hit;
    if (out_ms) *out_ms = m->samples[hit].t_ms;
    return MR_OK;
}

uint32_t mr_mov_sample_count(const mr_mov *m) { return m->nsamples; }

void mr_mov_sample_info(const mr_mov *m, uint32_t index, uint64_t *t_ms,
                        int *is_video, int *is_keyframe)
{
    const struct mov_sample *s;
    if (index >= m->nsamples) {
        if (t_ms) *t_ms = 0;
        if (is_video) *is_video = 0;
        if (is_keyframe) *is_keyframe = 0;
        return;
    }
    s = &m->samples[index];
    if (t_ms) *t_ms = s->t_ms;
    if (is_video) *is_video = s->is_video;
    if (is_keyframe) *is_keyframe = s->is_keyframe;
}

void mr_mov_close(mr_mov *m)
{
    if (!m) return;
    free(m->samples);
    free(m->metadata);
    free(m->packet_buf);
    m->samples = NULL;
    m->metadata = NULL;
    m->packet_buf = NULL;
    m->packet_cap = 0;
}