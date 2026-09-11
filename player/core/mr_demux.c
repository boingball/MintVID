/*
 * MintVID - container auto-detect front end.
 *
 * Sniffs the buffer signature and delegates to the matching backend through
 * the neutral mr_demux interface so the player is container-blind.
 */
#include "mr_demux.h"
#include "mr_avi.h"
#include "mr_mov.h"
#include "mr_ts.h"
#include "mr_ps.h"
#include "mr_source.h"
#include "mr_raw_mjpeg.h"
#include "mr_raw_mpeg4.h"
#include "mr_mkv.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* Record why a transport-stream open failed. When the PMT carried a video codec
 * we recognise but cannot decode (e.g. HEVC), name it so the player can tell the
 * user *what* it couldn't play rather than a generic container error. */
static void set_ts_open_error(const mr_ts *ts)
{
    const char *name = ts->unsupported_video_type
                           ? mr_ts_video_type_name(ts->unsupported_video_type)
                           : NULL;
    if (name) {
        char msg[96];
        snprintf(msg, sizeof msg,
                 "%s video is not supported (MPEG-TS stream type 0x%02x)", name,
                 (unsigned)ts->unsupported_video_type);
        mr_source_set_error(msg);
    } else {
        mr_source_set_error("unsupported or malformed MPEG-TS stream");
    }
}

struct mr_demux {
    mr_container kind;
    union {
        mr_avi avi;
        mr_mov mov;
        mr_ts ts;
        mr_ps ps;
        mr_raw_mjpeg raw_mjpeg;
        mr_raw_mpeg4 raw_mpeg4;
        mr_mkv mkv;
    } u;
    mr_source *owned_source;
};

size_t mr_demux_source_buffer_capacity(const mr_demux *d)
{
    return d ? mr_source_buffer_capacity(d->owned_source) : 0;
}

/* AVI  = 'RIFF' .... 'AVI '   ;  MOV = an early 'ftyp'/'moov'/'mdat' atom;
 * TS has 0x47 sync bytes every 188 bytes (or at +4 in 192-byte M2TS packets);
 * PS starts with an MPEG pack header (00 00 01 BA);
 * raw MJPEG begins directly with a JPEG SOI marker. */
static mr_container sniff(const uint8_t *b, size_t len)
{
    if (len >= 4 && b[0] == 0x1a && b[1] == 0x45 &&
        b[2] == 0xdf && b[3] == 0xa3)
        return MR_CONTAINER_MKV;
    if (len >= 12 &&
        mr_rl32(b) == MR_FOURCC('R','I','F','F') &&
        mr_rl32(b + 8) == MR_FOURCC('A','V','I',' '))
        return MR_CONTAINER_AVI;
    if (len >= 8) {
        /* atom types are stored big-endian, so pack the constants big-endian */
        #define BE4(a,b_,c,d) (((uint32_t)(a)<<24)|((uint32_t)(b_)<<16)| \
                               ((uint32_t)(c)<<8)|(uint32_t)(d))
        uint32_t t = mr_rb32(b + 4);
        if (t == BE4('f','t','y','p') ||
            t == BE4('m','o','o','v') ||
            t == BE4('m','d','a','t') ||
            t == BE4('w','i','d','e') ||
            t == BE4('f','r','e','e') ||
            t == BE4('s','k','i','p'))
            return MR_CONTAINER_MOV;
        #undef BE4
    }
    if (len >= 377 && b[0] == 0x47 && b[188] == 0x47 && b[376] == 0x47)
        return MR_CONTAINER_TS;
    if (len >= 389 && b[4] == 0x47 && b[196] == 0x47 && b[388] == 0x47)
        return MR_CONTAINER_TS;
    if (len >= 4 && b[0] == 0 && b[1] == 0 && b[2] == 1 && b[3] == 0xba)
        return MR_CONTAINER_PS;
    if (len >= 2 && b[0] == 0xff && b[1] == 0xd8)
        return MR_CONTAINER_RAW_MJPEG;
    if (len >= 4 && b[0] == 0 && b[1] == 0 && b[2] == 1 &&
        (b[3] == 0xb0 || b[3] == 0xb5 || b[3] <= 0x2f))
        return MR_CONTAINER_RAW_MPEG4;
    return MR_CONTAINER_NONE;
}

mr_demux *mr_demux_open(const uint8_t *buf, size_t len)
{
    mr_container kind = sniff(buf, len);
    if (kind == MR_CONTAINER_NONE) return NULL;

    mr_demux *d = (mr_demux *)calloc(1, sizeof *d);
    if (!d) return NULL;
    d->kind = kind;

    mr_status st;
    if (kind == MR_CONTAINER_AVI)
        st = mr_avi_open(&d->u.avi, buf, len);
    else if (kind == MR_CONTAINER_MOV)
        st = mr_mov_open(&d->u.mov, buf, len);
    else if (kind == MR_CONTAINER_TS)
        st = mr_ts_open(&d->u.ts, buf, len);
    else if (kind == MR_CONTAINER_PS)
        st = mr_ps_open(&d->u.ps, buf, len);
    else if (kind == MR_CONTAINER_RAW_MJPEG)
        st = mr_raw_mjpeg_open(&d->u.raw_mjpeg, buf, len);
    else if (kind == MR_CONTAINER_MKV)
        st = mr_mkv_open(&d->u.mkv, buf, len);
    else
        st = mr_raw_mpeg4_open(&d->u.raw_mpeg4, buf, len);
    if (st != MR_OK) {
        if (kind == MR_CONTAINER_TS) set_ts_open_error(&d->u.ts);
        if (kind == MR_CONTAINER_AVI) mr_avi_close(&d->u.avi);
        else if (kind == MR_CONTAINER_MOV) mr_mov_close(&d->u.mov);
        else if (kind == MR_CONTAINER_TS) mr_ts_close(&d->u.ts);
        else if (kind == MR_CONTAINER_PS) mr_ps_close(&d->u.ps);
        else if (kind == MR_CONTAINER_MKV) mr_mkv_close(&d->u.mkv);
        free(d);
        return NULL;
    }
    return d;
}

mr_demux *mr_demux_open_file_ex(const char *path,
                                const struct mr_http_options *options)
{
    uint8_t head[512];
    size_t got;
    size_t end;
    mr_container kind;
    mr_demux *d;
    mr_status st;
    mr_source *source;

    if (!path) return NULL;
    source = mr_source_open_ex(path, options);
    if (!source) return NULL;
    end = mr_source_length(source);
    got = end < sizeof head ? end : sizeof head;
    if (!mr_source_read_at(source, 0, head, got)) {
        mr_source_set_error("cannot read media response body");
        mr_source_close(source);
        return NULL;
    }
    kind = sniff(head, got);
    if (kind != MR_CONTAINER_AVI && kind != MR_CONTAINER_MOV &&
        kind != MR_CONTAINER_TS && kind != MR_CONTAINER_MKV) {
        mr_source_set_error(
            "source is not a supported AVI, MOV/MP4, MKV or MPEG-TS");
        mr_source_close(source);
        return NULL;
    }
    /* AVI/MOV rely on seeking a sample index, which needs a known length. Only
     * MPEG-TS plays forward from a length-less stream. */
    if (mr_source_is_streaming(source) && kind != MR_CONTAINER_TS) {
        mr_source_set_error(
            "streamed AVI/MOV/MKV needs a seekable server (Content-Length)");
        mr_source_close(source);
        return NULL;
    }

    d = (mr_demux *)calloc(1, sizeof *d);
    if (!d) {
        mr_source_set_error("not enough memory for demuxer");
        mr_source_close(source);
        return NULL;
    }
    d->kind = kind;
    d->owned_source = source;

    if (kind == MR_CONTAINER_AVI)
        st = mr_avi_open_source(&d->u.avi, source, end);
    else if (kind == MR_CONTAINER_MOV)
        st = mr_mov_open_source(&d->u.mov, source, end);
    else if (kind == MR_CONTAINER_MKV)
        st = mr_mkv_open_source(&d->u.mkv, source, end);
    else
        st = mr_ts_open_source(&d->u.ts, source, end);
    if (st != MR_OK) {
        if (kind == MR_CONTAINER_TS)
            set_ts_open_error(&d->u.ts);
        else
            mr_source_set_error("unsupported or malformed streamed container");
        if (kind == MR_CONTAINER_AVI) mr_avi_close(&d->u.avi);
        else if (kind == MR_CONTAINER_MOV) mr_mov_close(&d->u.mov);
        else if (kind == MR_CONTAINER_MKV) mr_mkv_close(&d->u.mkv);
        else mr_ts_close(&d->u.ts);
        mr_source_close(source);
        free(d);
        return NULL;
    }
    return d;
}

mr_demux *mr_demux_open_file(const char *path)
{
    return mr_demux_open_file_ex(path, NULL);
}

int mr_demux_is_file_backed_container(const char *path)
{
    uint8_t head[512];
    size_t got;
    mr_container kind;
    mr_source *source;
    if (!path) return 0;
    if (mr_source_is_url(path)) return 1;
    source = mr_source_open(path);
    if (!source) return 0;
    got = mr_source_length(source) < sizeof head
        ? mr_source_length(source) : sizeof head;
    if (!mr_source_read_at(source, 0, head, got)) got = 0;
    mr_source_close(source);
    kind = sniff(head, got);
    return kind == MR_CONTAINER_AVI || kind == MR_CONTAINER_MOV ||
           kind == MR_CONTAINER_TS || kind == MR_CONTAINER_MKV;
}

const char *mr_demux_last_open_error(void)
{
    return mr_source_last_error();
}

mr_status mr_demux_next_packet(mr_demux *d, mr_packet *pkt)
{
    /* begin/the elapsed-time calculation below only ever feed d->u.ts.timing,
     * which nothing populates or reads unless mr_demux_set_timing_enabled()
     * turned it on - skip the clock() call (a ReadEClock() read on Amiga)
     * for every packet otherwise, instead of paying for a timestamp this
     * function immediately discards. */
    int ts_timing = d->kind == MR_CONTAINER_TS && d->u.ts.timing_enabled;
    clock_t begin = ts_timing ? clock() : 0;
    mr_status status;
    mr_demux_timing before;
    if (ts_timing) before = d->u.ts.timing;
    if (pkt) { pkt->has_pts = 0; pkt->pts_us = 0; pkt->is_annexb = 0; }
    if (d->kind == MR_CONTAINER_AVI) status = mr_avi_next_packet(&d->u.avi, pkt);
    else if (d->kind == MR_CONTAINER_MOV) status = mr_mov_next_packet(&d->u.mov, pkt);
    else if (d->kind == MR_CONTAINER_TS) status = mr_ts_next_packet(&d->u.ts, pkt);
    else if (d->kind == MR_CONTAINER_PS) status = mr_ps_next_packet(&d->u.ps, pkt);
    else if (d->kind == MR_CONTAINER_MKV) status = mr_mkv_next_packet(&d->u.mkv, pkt);
    else if (d->kind == MR_CONTAINER_RAW_MJPEG)
        status = mr_raw_mjpeg_next_packet(&d->u.raw_mjpeg, pkt);
    else status = mr_raw_mpeg4_next_packet(&d->u.raw_mpeg4, pkt);
    if (ts_timing) {
        mr_demux_timing *t = &d->u.ts.timing;
        unsigned long us = (unsigned long)((clock() - begin) * 1000000UL /
                                           CLOCKS_PER_SEC);
        unsigned long delta;
#define UPDATE_MAX(field) do { delta = t->field##_us - before.field##_us; \
    if (delta > t->field##_max_us) t->field##_max_us = delta; } while (0)
        t->calls++;
        t->call_us += us;
        if (us > t->call_max_us) t->call_max_us = us;
        UPDATE_MAX(source); UPDATE_MAX(sync); UPDATE_MAX(assembly);
        UPDATE_MAX(copy); UPDATE_MAX(audio); UPDATE_MAX(video);
        delta = t->packets_scanned - before.packets_scanned;
        if (delta > t->scanned_max) t->scanned_max = delta;
#undef UPDATE_MAX
    }
    return status;
}

void mr_demux_set_service(mr_demux *d, mr_demux_service_fn fn, void *opaque)
{
    if (d && d->kind == MR_CONTAINER_TS) {
        d->u.ts.service = fn; d->u.ts.service_opaque = opaque;
    }
}

void mr_demux_set_timing_enabled(mr_demux *d, int enabled)
{
    if (d && d->kind == MR_CONTAINER_TS) d->u.ts.timing_enabled = enabled != 0;
}

void mr_demux_timing_get(mr_demux *d, mr_demux_timing *timing, int reset)
{
    if (!timing) return;
    memset(timing, 0, sizeof *timing);
    if (d && d->kind == MR_CONTAINER_TS) {
        *timing = d->u.ts.timing;
        if (reset) memset(&d->u.ts.timing, 0, sizeof d->u.ts.timing);
    }
}

void mr_demux_rewind(mr_demux *d)
{
    if (d->kind == MR_CONTAINER_AVI) mr_avi_rewind(&d->u.avi);
    else if (d->kind == MR_CONTAINER_MOV) mr_mov_rewind(&d->u.mov);
    else if (d->kind == MR_CONTAINER_TS) mr_ts_rewind(&d->u.ts);
    else if (d->kind == MR_CONTAINER_PS) mr_ps_rewind(&d->u.ps);
    else if (d->kind == MR_CONTAINER_MKV) mr_mkv_rewind(&d->u.mkv);
    else if (d->kind == MR_CONTAINER_RAW_MJPEG)
        mr_raw_mjpeg_rewind(&d->u.raw_mjpeg);
    else mr_raw_mpeg4_rewind(&d->u.raw_mpeg4);
}

int mr_demux_can_seek(const mr_demux *d)
{
    /* Excludes network sources on principle even though mr_mov_seek() itself
     * only touches metadata already resident in RAM: a "seekable" HTTP MOV
     * would still need mr_demux_seek() to trigger a fresh byte-range read for
     * the target packet, which mr_mov_next_packet() already does via
     * file_read_at() - so this is future-proofing the contract, not masking
     * a real limitation today. */
    return d && d->kind == MR_CONTAINER_MOV && !mr_source_is_streaming(d->owned_source);
}

mr_status mr_demux_seek(mr_demux *d, uint64_t target_us, uint64_t *out_us)
{
    if (!mr_demux_can_seek(d)) return MR_EUNSUPPORTED;
    {
        uint64_t out_ms = 0;
        mr_status st = mr_mov_seek(&d->u.mov, target_us / 1000, &out_ms);
        if (st == MR_OK && out_us) *out_us = out_ms * 1000;
        return st;
    }
}

void mr_demux_close(mr_demux *d)
{
    if (!d) return;
    if (d->kind == MR_CONTAINER_AVI) mr_avi_close(&d->u.avi);
    else if (d->kind == MR_CONTAINER_MOV) mr_mov_close(&d->u.mov);
    else if (d->kind == MR_CONTAINER_TS) mr_ts_close(&d->u.ts);
    else if (d->kind == MR_CONTAINER_PS) mr_ps_close(&d->u.ps);
    else if (d->kind == MR_CONTAINER_MKV) mr_mkv_close(&d->u.mkv);
    if (d->owned_source) mr_source_close(d->owned_source);
    free(d);
}

const mr_video_info *mr_demux_video(const mr_demux *d)
{
    if (d->kind == MR_CONTAINER_AVI) return &d->u.avi.video;
    if (d->kind == MR_CONTAINER_MOV) return &d->u.mov.video;
    if (d->kind == MR_CONTAINER_TS) return &d->u.ts.video;
    if (d->kind == MR_CONTAINER_PS) return &d->u.ps.video;
    if (d->kind == MR_CONTAINER_MKV) return &d->u.mkv.video;
    if (d->kind == MR_CONTAINER_RAW_MJPEG) return &d->u.raw_mjpeg.video;
    return &d->u.raw_mpeg4.video;
}

const mr_audio_info *mr_demux_audio(const mr_demux *d)
{
    if (d->kind == MR_CONTAINER_AVI) return &d->u.avi.audio;
    if (d->kind == MR_CONTAINER_MOV) return &d->u.mov.audio;
    if (d->kind == MR_CONTAINER_TS) return &d->u.ts.audio;
    if (d->kind == MR_CONTAINER_PS) return &d->u.ps.audio;
    if (d->kind == MR_CONTAINER_MKV) return &d->u.mkv.audio;
    if (d->kind == MR_CONTAINER_RAW_MJPEG) return &d->u.raw_mjpeg.audio;
    return &d->u.raw_mpeg4.audio;
}

const char *mr_demux_container_name(const mr_demux *d)
{
    return d->kind == MR_CONTAINER_AVI ? "AVI"
         : d->kind == MR_CONTAINER_MOV ? "MOV"
         : d->kind == MR_CONTAINER_TS ? "MPEG-TS"
         : d->kind == MR_CONTAINER_PS ? "MPEG-PS"
         : d->kind == MR_CONTAINER_MKV ? "Matroska/MKV"
         : d->kind == MR_CONTAINER_RAW_MJPEG ? "raw MJPEG"
         : d->kind == MR_CONTAINER_RAW_MPEG4 ? "raw M4V" : "?";
}

static void fourcc_text(uint32_t fourcc, char tag[5])
{
    int i;
    tag[0] = (char)(fourcc & 255);
    tag[1] = (char)((fourcc >> 8) & 255);
    tag[2] = (char)((fourcc >> 16) & 255);
    tag[3] = (char)((fourcc >> 24) & 255);
    tag[4] = 0;
    for (i = 0; i < 4; i++)
        if (tag[i] < 32 || tag[i] > 126) tag[i] = '?';
}

static const char *video_fourcc_name(uint32_t fourcc)
{
    char tag[5];
    int i;
    fourcc_text(fourcc, tag);
    for (i = 0; i < 4; i++)
        if (tag[i] >= 'a' && tag[i] <= 'z') tag[i] -= 'a' - 'A';
    if (!strcmp(tag, "H264") || !strcmp(tag, "AVC1") ||
        !strcmp(tag, "X264") || !strcmp(tag, "DAVC")) return "H.264/AVC";
    if (!strcmp(tag, "HEVC") || !strcmp(tag, "HVC1") ||
        !strcmp(tag, "HEV1") || !strcmp(tag, "H265") ||
        !strcmp(tag, "DHEV")) return "H.265/HEVC";
    if (!strcmp(tag, "VVC1") || !strcmp(tag, "VVI1")) return "H.266/VVC";
    if (!strcmp(tag, "MPG1")) return "MPEG-1 video";
    if (!strcmp(tag, "MPG2")) return "MPEG-2 video";
    if (!strcmp(tag, "CVID")) return "Cinepak";
    if (!strcmp(tag, "MJPG") || !strcmp(tag, "JPEG")) return "Motion JPEG";
    if (!strcmp(tag, "MP4V") || !strcmp(tag, "DIVX") ||
        !strcmp(tag, "XVID") || !strcmp(tag, "DX50") ||
        !strcmp(tag, "FMP4")) return "MPEG-4 Part 2";
    if (!strcmp(tag, "MP42") || !strcmp(tag, "DIV2"))
        return "Microsoft MPEG-4 v2";
    if (!strcmp(tag, "H263") || !strcmp(tag, "S263")) return "H.263";
    if (!strcmp(tag, "MSVC") || !strcmp(tag, "CRAM"))
        return "Microsoft Video 1";
    if (!strcmp(tag, "MRLE") || !strcmp(tag, "RLE8")) return "Microsoft RLE";
    if (!strcmp(tag, "2VUY") || !strcmp(tag, "UYVY")) return "raw UYVY422";
    if (!strcmp(tag, "AV01")) return "AV1";
    if (!strcmp(tag, "VP80")) return "VP8";
    if (!strcmp(tag, "VP90")) return "VP9";
    if (!strncmp(tag, "VP6", 3)) return "VP6";
    if (!strcmp(tag, "WMV1") || !strcmp(tag, "WMV2")) return "Windows Media Video";
    if (!strcmp(tag, "WMV3")) return "Windows Media Video 9";
    if (!strcmp(tag, "VC-1") || !strcmp(tag, "WVC1")) return "VC-1";
    if (!strcmp(tag, "THEO")) return "Theora";
    if (!strcmp(tag, "FLV1")) return "Sorenson Spark";
    if (!strcmp(tag, "SVQ1") || !strcmp(tag, "SVQ3")) return "Sorenson Video";
    return NULL;
}

static const char *audio_fourcc_name(uint32_t fourcc)
{
    char tag[5];
    int i;
    fourcc_text(fourcc, tag);
    for (i = 0; i < 4; i++)
        if (tag[i] >= 'a' && tag[i] <= 'z') tag[i] -= 'a' - 'A';
    if (!strcmp(tag, "MP4A")) return "AAC/MPEG-4 audio";
    if (!strcmp(tag, "AC-3")) return "AC-3";
    if (!strcmp(tag, "EC-3")) return "E-AC-3";
    if (!strcmp(tag, "OPUS")) return "Opus";
    if (!strcmp(tag, "FLAC")) return "FLAC";
    if (!strcmp(tag, "ALAC")) return "Apple Lossless";
    if (!strcmp(tag, "VORB")) return "Vorbis";
    if (!strcmp(tag, "WMA1") || !strcmp(tag, "WMA2")) return "Windows Media Audio";
    return NULL;
}

void mr_demux_describe_video_codec(const mr_demux *d, char *out, size_t cap)
{
    const mr_video_info *vi;
    const char *name;
    char tag[5];
    if (!out || !cap) return;
    out[0] = 0;
    if (!d) { snprintf(out, cap, "unknown"); return; }
    vi = mr_demux_video(d);
    if (!vi || !vi->valid) {
        if (d->kind == MR_CONTAINER_TS && d->u.ts.unsupported_video_type) {
            name = mr_ts_video_type_name(d->u.ts.unsupported_video_type);
            snprintf(out, cap, "%s (MPEG-TS type 0x%02x)",
                     name ? name : "unknown video",
                     (unsigned)d->u.ts.unsupported_video_type);
        } else {
            snprintf(out, cap, "none detected");
        }
        return;
    }
    fourcc_text(vi->fourcc, tag);
    name = video_fourcc_name(vi->fourcc);
    if (name) snprintf(out, cap, "%s (fourcc '%s')", name, tag);
    else snprintf(out, cap, "video fourcc '%s'", tag);
}

void mr_demux_describe_audio_codec(const mr_demux *d, char *out, size_t cap)
{
    const mr_audio_info *ai;
    const char *name = NULL;
    char tag[5];
    if (!out || !cap) return;
    out[0] = 0;
    if (!d) { snprintf(out, cap, "unknown"); return; }
    ai = mr_demux_audio(d);
    if (ai && ai->valid) {
        if (ai->format_tag == MR_AUDIO_FORMAT_PCM) name = "PCM";
        else if (ai->format_tag == MR_AUDIO_FORMAT_MP2) name = "MPEG Layer II";
        else if (ai->format_tag == MR_AUDIO_FORMAT_MP3) name = "MP3";
        else if (ai->format_tag == MR_AUDIO_FORMAT_AAC)
            name = ai->config_len &&
                   ((ai->config[0] >> 3) == 5 || (ai->config[0] >> 3) == 29)
                 ? "HE-AAC" : "AAC-LC";
        else if (ai->format_tag == MR_AUDIO_FORMAT_AC3) name = "AC-3";
        if (name) { snprintf(out, cap, "%s", name); return; }
        if (ai->codec_tag > 0xffffU) {
            fourcc_text(ai->codec_tag, tag);
            name = audio_fourcc_name(ai->codec_tag);
            if (name) snprintf(out, cap, "%s (fourcc '%s')", name, tag);
            else snprintf(out, cap, "audio fourcc '%s'", tag);
        } else {
            snprintf(out, cap, "WAVE audio format 0x%04x",
                     (unsigned)ai->format_tag);
        }
        return;
    }
    if (d->kind == MR_CONTAINER_TS && d->u.ts.audio_type) {
        name = mr_ts_audio_type_name(d->u.ts.audio_type);
        snprintf(out, cap, "%s (MPEG-TS type 0x%02x)",
                 name ? name : "unknown audio", (unsigned)d->u.ts.audio_type);
    } else {
        snprintf(out, cap, "none detected");
    }
}
