#include "mr_berryvp.h"
#include <exec/types.h>
#include <exec/libraries.h>
#include <proto/exec.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef MR_DEBUG_LOGS
#define MR_DEBUG_LOGS 0
#endif

#if MR_DEBUG_LOGS
#define DBG_PRINTF(...) printf(__VA_ARGS__)
#else
#define DBG_PRINTF(...) ((void)0)
#endif

/* berryvp.library Stage 4F: Stage-3D 3.2 ABI with HUNK-safe audio vectors. */
#define BVP_MIN_VERSION 3
#define BVP_CODEC_H264 1UL
#define BVP_CODEC_AAC  2UL
#define BVP_CODEC_AC3  3UL
#define BVP_CODEC_EAC3 4UL
#define BVP_OK 0L
#define BVP_CAP_PTS64       0x00000002UL
#define BVP_CAP_H264_HW     0x00000100UL
#define BVP_CAP_DIRECT_HVS  0x00000400UL
#define BVP_CAP_FRAME_FLAGS 0x00000800UL
#define BVP_CAP_INPUT_PTS   0x00001000UL
#define BVP_REQUIRED_CAPS (BVP_CAP_PTS64 | BVP_CAP_H264_HW | \
                           BVP_CAP_DIRECT_HVS | BVP_CAP_FRAME_FLAGS | \
                           BVP_CAP_INPUT_PTS)
#define BVP_PKT_CONFIG      0x00000001UL
#define BVP_PKT_FRAME_START 0x00000002UL
#define BVP_PKT_FRAME_END   0x00000004UL
#define BVP_PKT_KEY         0x00000008UL
#define BVP_MAX_PACKET      0x0003fffcUL
#define BVP_PTS_UNKNOWN_HI  0x80000000UL

extern ULONG bvp_call_getversion(struct Library *base);
extern ULONG bvp_call_getcaps(struct Library *base);
extern LONG  bvp_call_openstream(struct Library *base, ULONG flags);
extern LONG  bvp_call_closestream(struct Library *base);
extern LONG  bvp_call_configvideo(struct Library *base, ULONG codec,
                                  ULONG width, ULONG height, ULONG timescale);
extern LONG  bvp_call_configaudio(struct Library *base, ULONG codec,
                                  ULONG rate, ULONG channels, ULONG timescale);
extern LONG  bvp_call_sendvideo(struct Library *base, const void *data,
                                ULONG len, ULONG pts_hi, ULONG pts_lo,
                                ULONG flags);
extern LONG  bvp_call_sendaudio(struct Library *base, const void *data,
                                ULONG len, ULONG pts_hi, ULONG pts_lo,
                                ULONG flags);
extern struct Library *bvp_exec_openlibrary(const char *name, ULONG version);
extern void bvp_exec_closelibrary(struct Library *lib);


/* AC309F: diagnostic only.  Trace the actual BerryVP ABI boundary around the
 * reproducible MOV/MP4 stall.  No scheduling or packet contents are changed. */
/* OpenStream flags: 0 = cinema (HVS), 10/25/50/75 = P96 window size. */
ULONG mr_berryvp_stream_flags = 0;
int mr_berryvp_force_isp = 0;   /* ISP82: ISP = cinema via ISP graph (test) */
unsigned long mr_berryvp_isp_out = 0;  /* N84: 0 / 0xFFFFFFFF / (W<<16)|H (test) */
void *mr_berryvp_osd_req = 0;           /* O1: OSD test request block or 0 */
extern LONG bvp_call_control2(struct Library *base, ULONG cmd, ULONG arg);

static FILE *ac309f_trace;

static FILE *ac309f_trace_file(void)
{
#if MR_DEBUG_LOGS
    if (mr_berryvp_stream_flags) return NULL;   /* no file trace in window mode */
    if (!ac309f_trace) ac309f_trace = fopen("AC309F_BRIDGE.TXT", "w");
    return ac309f_trace;
#else
    return NULL;
#endif
}

static int ac309f_trace_pts(int has_pts, uint64_t pts)
{
#if MR_DEBUG_LOGS
    return has_pts && pts >= 45000000ULL;
#else
    (void)has_pts; (void)pts;
    return 0;
#endif
}

static void ac309f_nal_summary(const uint8_t *s, uint32_t n, uint64_t pts,
                               uint32_t raw_len)
{
    uint32_t i = 0, c1 = 0, c5 = 0, c6 = 0, c7 = 0, c8 = 0, c9 = 0, co = 0;
    FILE *f = ac309f_trace_file();
    if (!f || !s) return;
    while (i + 3 < n) {
        uint32_t sc = 0, start, j;
        while (i + 3 < n) {
            if (s[i] == 0 && s[i+1] == 0 && s[i+2] == 1) { sc = 3; break; }
            if (i + 4 <= n && s[i] == 0 && s[i+1] == 0 && s[i+2] == 0 && s[i+3] == 1) { sc = 4; break; }
            i++;
        }
        if (!sc || i + sc >= n) break;
        start = i + sc;
        j = start + 1;
        while (j + 3 < n) {
            if (s[j] == 0 && s[j+1] == 0 && (s[j+2] == 1 || (j + 3 < n && s[j+2] == 0 && s[j+3] == 1))) break;
            j++;
        }
        switch (s[start] & 31u) {
            case 1: c1++; break; case 5: c5++; break; case 6: c6++; break;
            case 7: c7++; break; case 8: c8++; break; case 9: c9++; break;
            default: co++; break;
        }
        i = j;
    }
    fprintf(f, "AC309F_AU pts=$%08lx:%08lx raw=%lu annexb=%lu nal1=%lu idr5=%lu sei6=%lu sps7=%lu pps8=%lu aud9=%lu other=%lu\n",
            (unsigned long)(pts >> 32), (unsigned long)pts,
            (unsigned long)raw_len, (unsigned long)n,
            (unsigned long)c1, (unsigned long)c5, (unsigned long)c6,
            (unsigned long)c7, (unsigned long)c8, (unsigned long)c9,
            (unsigned long)co);
    fflush(f);
}

static const char *bvp_error_name(LONG r)
{
    switch (r) {
        case  0: return "OK";
        case -1: return "BADSTATE";
        case -2: return "BADPARAM";
        case -3: return "UNSUPPORTED";
        case -4: return "BUSY";
        case -5: return "TIMEOUT";
        case -6: return "BACKEND";
        case -7: return "CONFIG";
        case -8: return "TOOLARGE";
        case -10: return "NOBACKEND";
        case -11: return "CONFIG_SPS";
        case -12: return "CONFIG_PPS";
        case -13: return "CONFIG_EXTRA";
        case -14: return "CONFIG_GEOM";
        case -15: return "CONFIG_COMPAT";
        default: return "UNKNOWN";
    }
}

static mr_status send_config(mr_berryvp *b, const uint8_t *p, uint32_t n)
{
    LONG r;
    if (!p || !n || n > sizeof b->config_buf) return MR_EFORMAT;
    r = bvp_call_sendvideo(b->lib, p, n, BVP_PTS_UNKNOWN_HI, 0,
                           BVP_PKT_CONFIG);
    if (r != BVP_OK) {
        printf("Error: BerryVP video configuration packet failed: %ld (%s).\n", (long)r, bvp_error_name(r));
        return MR_ERR;
    }
    b->config_sent = 1;
    DBG_PRINTF("BerryVP CONFIG: OK (%lu bytes SPS/PPS)\n", (unsigned long)n);
    return MR_OK;
}

static mr_status send_annexb(mr_berryvp *b, const uint8_t *p, uint32_t n,
                             int has_pts, uint64_t pts, int key,
                             int frame_boundaries)
{
    uint32_t off = 0, chunk = 0;
    uint32_t limit = b->video_chunk_limit;
    int first = 1;
    int trace = ac309f_trace_pts(has_pts, pts) || n > 200000u;
    if (!limit || limit > BVP_MAX_PACKET) limit = BVP_MAX_PACKET;
    while (off < n) {
        uint32_t part = n - off;
        ULONG flags = 0, hi, lo;
        LONG r;
        FILE *tf = trace ? ac309f_trace_file() : NULL;
        if (part > limit) part = limit;
        if (frame_boundaries && first) flags |= BVP_PKT_FRAME_START;
        if (frame_boundaries && off + part == n) flags |= BVP_PKT_FRAME_END;
        if (frame_boundaries && first && key) flags |= BVP_PKT_KEY;
        if (has_pts && first) {
            hi = (ULONG)(pts >> 32);
            lo = (ULONG)(pts & 0xffffffffu);
        } else {
            hi = BVP_PTS_UNKNOWN_HI;
            lo = 0;
        }
        chunk++;
        if (tf) {
            fprintf(tf, "AC309F_SV_ENTER au=%lu chunk=%lu off=%lu part=%lu total=%lu flags=$%08lx pts=$%08lx:%08lx key=%d\n",
                    (unsigned long)(b->packets + 1), (unsigned long)chunk,
                    (unsigned long)off, (unsigned long)part, (unsigned long)n,
                    (unsigned long)flags, (unsigned long)hi, (unsigned long)lo, key);
            fflush(tf);
        }
        r = bvp_call_sendvideo(b->lib, p + off, part, hi, lo, flags);
        if (tf) {
            fprintf(tf, "AC309F_SV_EXIT  au=%lu chunk=%lu r=%ld\n",
                    (unsigned long)(b->packets + 1), (unsigned long)chunk, (long)r);
            fflush(tf);
        }
        if (r != BVP_OK) {
            printf("Error: BerryVP video transfer failed: %ld (%s), len=%lu, flags=$%lx.\n",
                   (long)r, bvp_error_name(r), (unsigned long)part,
                   (unsigned long)flags);
            return MR_ERR;
        }
        off += part;
        first = 0;
    }
    b->packets++;
    b->bytes += n;
    return MR_OK;
}

static const uint32_t aac_rates[13] = {
    96000, 88200, 64000, 48000, 44100, 32000, 24000,
    22050, 16000, 12000, 11025, 8000, 7350
};

static int aac_rate_index(uint32_t rate)
{
    unsigned i;
    for (i = 0; i < 13; ++i)
        if (aac_rates[i] == rate) return (int)i;
    return -1;
}

static int parse_aac_config(mr_berryvp *b, const mr_audio_info *ai)
{
    unsigned obj, sfi, ch;
    if (!b || !ai || ai->format_tag != MR_AUDIO_FORMAT_AAC) return 0;
    if (ai->codec_tag == MR_FOURCC('L','A','T','M')) return 0;
    if (ai->config_len >= 2) {
        obj = (unsigned)(ai->config[0] >> 3);
        sfi = (unsigned)(((ai->config[0] & 7u) << 1) | (ai->config[1] >> 7));
        ch = (unsigned)((ai->config[1] >> 3) & 0x0fu);
        /* P96W31: stereo, 5.0 or 5.1; berry_faad03 mixes 5.x to stereo. */
        if (obj != 2u || sfi >= 13u || (ch != 2u && ch != 5u && ch != 6u)) return 0;
        if (ai->sample_rate && aac_rates[sfi] != ai->sample_rate) return 0;
        b->aac_object_type = (uint8_t)obj;
        b->aac_sf_index = (uint8_t)sfi;
        b->aac_channels = (uint8_t)ch;
        return 1;
    }
    if (ai->channels != 2 && ai->channels != 5 && ai->channels != 6) return 0;
    {
        int idx = aac_rate_index(ai->sample_rate);
        if (idx < 0) return 0;
        b->aac_object_type = 2;
        b->aac_sf_index = (uint8_t)idx;
        b->aac_channels = (uint8_t)ai->channels;
    }
    return 1;
}

static int packet_is_adts(const uint8_t *p, uint32_t n)
{
    return p && n >= 7 && p[0] == 0xff && (p[1] & 0xf6) == 0xf0;
}

static mr_status make_adts(mr_berryvp *b, const uint8_t *src, uint32_t n,
                           const uint8_t **out, uint32_t *out_n)
{
    uint32_t total;
    uint8_t profile, sfi, ch;
    uint8_t *p;
    if (!b || !src || !n || !out || !out_n) return MR_EFORMAT;
    if (packet_is_adts(src, n)) { *out = src; *out_n = n; return MR_OK; }
    if (!b->audio_ready || b->aac_object_type != 2 ||
        (b->aac_channels != 2 && b->aac_channels != 5 && b->aac_channels != 6))
        return MR_EUNSUPPORTED;
    if (n > 0x1fffu - 7u) return MR_EFORMAT;
    total = n + 7u;
    if (b->audio_cap < total) {
        uint32_t cap = total < 4096u ? 4096u : total;
        uint8_t *nb = (uint8_t *)realloc(b->audio_buf, cap);
        if (!nb) return MR_ENOMEM;
        b->audio_buf = nb; b->audio_cap = cap;
    }
    p = b->audio_buf;
    profile = (uint8_t)(b->aac_object_type - 1u);
    sfi = b->aac_sf_index; ch = b->aac_channels;
    p[0] = 0xff;
    p[1] = 0xf1;
    p[2] = (uint8_t)((profile << 6) | (sfi << 2) | (ch >> 2));
    p[3] = (uint8_t)(((ch & 3u) << 6) | ((total >> 11) & 3u));
    p[4] = (uint8_t)(total >> 3);
    p[5] = (uint8_t)(((total & 7u) << 5) | 0x1fu);
    p[6] = 0xfc;
    memcpy(p + 7, src, n);
    *out = p; *out_n = total;
    return MR_OK;
}

mr_status mr_berryvp_open(mr_berryvp *b, const mr_video_info *vi)
{
    ULONG ver;
    LONG r;
    const uint8_t *cfg;
    uint32_t cfg_len;
    if (!b || !vi || !vi->valid || vi->fourcc != MR_FOURCC('a','v','c','1'))
        return MR_EUNSUPPORTED;
    memset(b, 0, sizeof *b);
    mr_avc_annexb_init(&b->avc);
    DBG_PRINTF("[B01] before OldOpenLibrary(-408) berryvp.library\n");
    b->lib = bvp_exec_openlibrary("berryvp.library", BVP_MIN_VERSION);
    DBG_PRINTF("[B02] after OpenLibrary ptr=%p\n", (void *)b->lib);
    if (!b->lib) {
        printf("Error: cannot open LIBS:berryvp.library V%u or newer.\n", BVP_MIN_VERSION);
        return MR_EUNSUPPORTED;
    }
    DBG_PRINTF("[B03] before GetVersion/GetCaps\n");
    ver = bvp_call_getversion(b->lib);
    b->caps = bvp_call_getcaps(b->lib);
    (void)ver;
    DBG_PRINTF("[B04] after GetVersion/GetCaps\n");
    DBG_PRINTF("BerryVP library %lu.%lu caps=$%08lx\n",
           (unsigned long)(ver >> 16), (unsigned long)(ver & 0xffffu), b->caps);
    if ((b->caps & BVP_REQUIRED_CAPS) != BVP_REQUIRED_CAPS) {
        printf("Error: berryvp.library is missing required H.264/HVS/PTS capabilities.\n");
        mr_berryvp_close(b);
        return MR_EUNSUPPORTED;
    }
    /* ISP82: always tell the library (it stays loaded between films). */
    (void)bvp_call_control2(b->lib, 8, mr_berryvp_force_isp ? 1UL : 0UL);
    /* N84: ISP output size test (always set; 0 = normal N83) */
    (void)bvp_call_control2(b->lib, 9, mr_berryvp_isp_out);
    /* O1: OSD test layer (always set; 0 = none) */
    (void)bvp_call_control2(b->lib, 10, (unsigned long)mr_berryvp_osd_req);
    DBG_PRINTF("[B05] before OpenStream\n");
    r = bvp_call_openstream(b->lib, mr_berryvp_stream_flags);
    DBG_PRINTF("[B06] after OpenStream r=%ld\n", (long)r);
    if (r != BVP_OK) {
        printf("Error: BerryVP OpenStream failed: %ld (%s).\n", (long)r, bvp_error_name(r));
        mr_berryvp_close(b);
        return MR_ERR;
    }
    DBG_PRINTF("[B07] before ConfigVideo %lux%lu\n", (unsigned long)vi->width, (unsigned long)vi->height);
    r = bvp_call_configvideo(b->lib, BVP_CODEC_H264,
                             (ULONG)vi->width, (ULONG)vi->height, 1000000UL);
    DBG_PRINTF("[B08] after ConfigVideo r=%ld\n", (long)r);
    if (r != BVP_OK) {
        printf("Error: BerryVP ConfigVideo failed: %ld (%s).\n", (long)r, bvp_error_name(r));
        mr_berryvp_close(b);
        return MR_ERR;
    }

    /* MOV/MP4 and MKV expose avcC directly. TS normally has no avcC and the
     * bridge extracts SPS/PPS from its first Annex-B video packet instead. */
    if (vi->config && vi->config_len) {
        DBG_PRINTF("[B09] before avcC->AnnexB config len=%lu\n", (unsigned long)vi->config_len);
        if (mr_avc_config_to_annexb(&b->avc, vi->config, vi->config_len,
                                    &cfg, &cfg_len) != MR_OK) {
            printf("Error: invalid H.264 avcC configuration.\n");
            mr_berryvp_close(b);
            return MR_EFORMAT;
        }
        DBG_PRINTF("[B10] avcC->AnnexB OK cfg_len=%lu\n", (unsigned long)cfg_len);
        if (cfg_len > sizeof b->config_buf) {
            printf("Error: H.264 SPS/PPS configuration is too large (%lu bytes).\n",
                   (unsigned long)cfg_len);
            mr_berryvp_close(b);
            return MR_ENOMEM;
        }
        memcpy(b->config_buf, cfg, cfg_len);
        b->config_len = cfg_len;
        DBG_PRINTF("[B11] before BerryVP CONFIG SendVideo\n");
        if (send_config(b, b->config_buf, b->config_len) != MR_OK) {
            mr_berryvp_close(b);
            return MR_ERR;
        }
        DBG_PRINTF("[B12] after BerryVP CONFIG SendVideo\n");
    }
    DBG_PRINTF("[B13] mr_berryvp_open complete\n");
    return MR_OK;
}

mr_status mr_berryvp_config_audio(mr_berryvp *b, const mr_audio_info *ai)
{
    LONG r;
    ULONG codec, channels;
    if (!b || !b->lib || !ai || !ai->valid)
        return MR_EUNSUPPORTED;

    if (ai->format_tag == MR_AUDIO_FORMAT_AAC) {
        if (!parse_aac_config(b, ai)) {
            printf("Error: AAC playback supports AAC-LC stereo or 5.1 at a standard ADTS sample rate only.\n");
            return MR_EUNSUPPORTED;
        }
        codec = BVP_CODEC_AAC;
        channels = 2UL;
        DBG_PRINTF("[A01] ConfigAudio AAC-LC %lu Hz input-ch=%u -> stereo HDMI\n",
               (unsigned long)ai->sample_rate, (unsigned)b->aac_channels);
    } else if (ai->format_tag == MR_AUDIO_FORMAT_AC3) {
        if (ai->sample_rate < 8000 || ai->sample_rate > 192000 ||
            ai->channels < 1 || ai->channels > 6) {
            printf("Error: unsupported AC3 sample rate or channel layout.\n");
            return MR_EUNSUPPORTED;
        }
        codec = BVP_CODEC_AC3;
        channels = (ULONG)ai->channels;
        DBG_PRINTF("[AC307A] ConfigAudio AC3 %lu Hz input-ch=%lu -> stereo HDMI\n",
               (unsigned long)ai->sample_rate, (unsigned long)channels);
    } else if (ai->format_tag == MR_AUDIO_FORMAT_EAC3) {
        /* AC308N3: the current scalar E-AC3 path is realtime-capable for
         * stereo input only.  Multichannel decode works functionally but is
         * too slow for realtime playback on CPU1, so reject it before the
         * decoder/module is configured. */
        if (ai->sample_rate < 8000 || ai->sample_rate > 192000) {
            printf("Error: unsupported E-AC3 sample rate.\n");
            return MR_EUNSUPPORTED;
        }
        if (ai->channels != 2) {
            printf("Error: E-AC3 multichannel audio is not supported for real-time playback. Stereo E-AC3 only.\n");
            return MR_EUNSUPPORTED;
        }
        codec = BVP_CODEC_EAC3;
        channels = 2UL;
        DBG_PRINTF("[AC308N3] ConfigAudio E-AC3 %lu Hz stereo -> stereo HDMI\n",
               (unsigned long)ai->sample_rate);
    } else {
        return MR_EUNSUPPORTED;
    }

    r = bvp_call_configaudio(b->lib, codec, (ULONG)ai->sample_rate,
                             channels, 1000000UL);
    DBG_PRINTF("[A02] ConfigAudio returned %ld (%s)\n", (long)r, bvp_error_name(r));
    if (r != BVP_OK) return MR_ERR;
    b->audio_format_tag = ai->format_tag;
    b->audio_ready = 1;
    return MR_OK;
}

mr_status mr_berryvp_send_audio_packet(mr_berryvp *b, const mr_packet *pkt)
{
    const uint8_t *audio;
    uint32_t audio_len;
    ULONG hi, lo;
    LONG r;
    mr_status st;
    if (!b || !b->lib || !b->audio_ready || !pkt || pkt->is_video ||
        !pkt->data || !pkt->len) return MR_EFORMAT;

    if (b->audio_format_tag == MR_AUDIO_FORMAT_AAC) {
        st = make_adts(b, pkt->data, pkt->len, &audio, &audio_len);
        if (st != MR_OK) return st;
    } else if (b->audio_format_tag == MR_AUDIO_FORMAT_AC3 ||
               b->audio_format_tag == MR_AUDIO_FORMAT_EAC3) {
        /* Matroska A_AC3/A_EAC3 block payload is already raw compressed audio. */
        audio = pkt->data;
        audio_len = pkt->len;
    } else {
        return MR_EUNSUPPORTED;
    }

    if (pkt->has_pts) {
        hi = (ULONG)(pkt->pts_us >> 32);
        lo = (ULONG)(pkt->pts_us & 0xffffffffu);
    } else {
        hi = BVP_PTS_UNKNOWN_HI; lo = 0;
    }
    if (ac309f_trace_pts(pkt->has_pts, pkt->pts_us)) {
        FILE *tf = ac309f_trace_file();
        if (tf) {
            fprintf(tf, "AC309F_SA_ENTER audio=%lu len=%lu pts=$%08lx:%08lx\n",
                    (unsigned long)(b->audio_packets + 1), (unsigned long)audio_len,
                    (unsigned long)hi, (unsigned long)lo);
            fflush(tf);
        }
    }
    r = bvp_call_sendaudio(b->lib, audio, (ULONG)audio_len, hi, lo, 0);
    if (ac309f_trace_pts(pkt->has_pts, pkt->pts_us)) {
        FILE *tf = ac309f_trace_file();
        if (tf) {
            fprintf(tf, "AC309F_SA_EXIT  audio=%lu r=%ld\n",
                    (unsigned long)(b->audio_packets + 1), (long)r);
            fflush(tf);
        }
    }
    if (r != BVP_OK) {
        printf("Error: BerryVP audio transfer failed: %ld (%s), len=%lu.\n",
               (long)r, bvp_error_name(r), (unsigned long)audio_len);
        return MR_ERR;
    }
    b->audio_packets++;
    b->audio_bytes += audio_len;
    return MR_OK;
}

mr_status mr_berryvp_send_packet(mr_berryvp *b, const mr_packet *pkt)
{
    const uint8_t *out;
    uint32_t out_len;
    int key;
    if (!b || !b->lib || !pkt || !pkt->is_video || !pkt->data || !pkt->len)
        return MR_EFORMAT;

    if (pkt->is_annexb) {
        out = pkt->data;
        out_len = pkt->len;
    } else {
        if (mr_avc_sample_to_annexb(&b->avc, pkt->data, pkt->len,
                                    &out, &out_len) != MR_OK)
            return MR_EFORMAT;
    }

    if (!b->config_sent) {
        uint32_t n = 0;
        mr_status st = mr_avc_annexb_extract_parameter_sets(out, out_len,
                                                            b->config_buf,
                                                            sizeof b->config_buf,
                                                            &n);
        if (st == MR_OK) {
            b->config_len = n;
            if (send_config(b, b->config_buf, n) != MR_OK) return MR_ERR;
        } else {
            /* Do not feed slices before VideoCore has SPS/PPS. TS streams
             * normally provide them at the beginning of the first IDR PES. */
            return MR_EAGAIN;
        }
    }

    key = mr_avc_annexb_is_keyframe(out, out_len);
    if (ac309f_trace_pts(pkt->has_pts, pkt->pts_us) || out_len > 200000u)
        ac309f_nal_summary(out, out_len, pkt->pts_us, pkt->len);
    /* MOV/MP4 and MKV packets are access units, so their boundaries map to
     * MMAL FRAME_START/FRAME_END. TS hands us a complete PES payload, which
     * can contain more than one access unit; do not invent AU boundaries. */
    /* N75: the current VPr8 scheduler path owns presentation.  Pass the
     * MintVID demuxer's real microsecond PTS on the first chunk of every AU;
     * continuation chunks remain MMAL_TIME_UNKNOWN exactly as send_annexb()
     * already enforces. */
    return send_annexb(b, out, out_len, pkt->has_pts, pkt->pts_us, key,
                       !pkt->is_annexb);
}

mr_status mr_berryvp_close(mr_berryvp *b)
{
    LONG r = BVP_OK;
    if (!b) return MR_EFORMAT;
    if (b->lib) {
        r = bvp_call_closestream(b->lib);
        if (r != BVP_OK)
            printf("Error: BerryVP CloseStream failed: %ld (%s).\n", (long)r, bvp_error_name(r));
        else
            DBG_PRINTF("BerryVP CloseStream: OK (EOS + HDMI cleanup)\n");
        bvp_exec_closelibrary(b->lib);
    }
    mr_avc_annexb_close(&b->avc);
    if (ac309f_trace) { fclose(ac309f_trace); ac309f_trace = NULL; }
    free(b->audio_buf);
    b->audio_buf = NULL;
    b->audio_cap = 0;
    b->audio_ready = 0;
    b->lib = NULL;
    return (r == BVP_OK) ? MR_OK : MR_ERR;
}
