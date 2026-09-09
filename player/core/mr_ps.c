/*
 * MintVID - MPEG-1/2 program-stream demuxer.
 *
 * Program streams already contain MPEG video elementary-stream bytes inside
 * PES packets. The demuxer exposes picture-aware chunks of those bytes to the
 * existing libmpeg2 decoder.
 */
#include "mr_ps.h"

#include <string.h>

/* Keep one local-audio scheduler iteration small.  A low-bitrate PS PES can
 * carry several complete MP2 frames; handing the whole payload to feed_mp2()
 * makes pl_mpeg decode all of them synchronously before mrplay can present a
 * due video frame or re-evaluate the audio clock.  pl_mpeg's input buffer is
 * explicitly streaming, so arbitrary byte boundaries are safe: a partial MP2
 * frame simply completes on a later call. */
#define MR_PS_AUDIO_CHUNK_MAX 512U

static size_t find_start(const uint8_t *b, size_t len, size_t from)
{
    size_t i;
    for (i = from; i + 4 <= len; i++)
        if (b[i] == 0 && b[i + 1] == 0 && b[i + 2] == 1)
            return i;
    return len;
}

static size_t find_picture_start(const uint8_t *b, size_t end, size_t from)
{
    size_t i;
    for (i = from; i + 4 <= end; i++)
        if (b[i] == 0 && b[i + 1] == 0 && b[i + 2] == 1 && b[i + 3] == 0)
            return i;
    return end;
}

/* MPEG-1 and MPEG-2 share the sequence header. MPEG-2 identifies itself with
 * a sequence extension (extension id 1) before the first picture. Keep the
 * distinction in the stream metadata even though libmpeg2 decodes both. */
static uint32_t video_fourcc(const uint8_t *b, size_t len, size_t sequence)
{
    size_t pos = sequence + 8;
    while ((pos = find_start(b, len, pos)) < len) {
        unsigned code = b[pos + 3];
        if (code == 0x00 || code == 0xb3 || code == 0xb7)
            break;
        if (code == 0xb5 && pos + 5 <= len && (b[pos + 4] >> 4) == 1)
            return MR_FOURCC('m','p','g','2');
        pos += 4;
    }
    return MR_FOURCC('m','p','g','1');
}

/* libmpeg2's adapter returns one display frame per decode call.  A small,
 * low-bitrate MPEG-PS PES packet can contain several complete pictures, so
 * handing over the whole PES makes the adapter overflow its one-frame return
 * contract.  Return at most one picture start per demux packet; libmpeg2 keeps
 * partial pictures across calls, including pictures split across PES packets. */
static mr_status next_video_chunk(mr_ps *p, mr_packet *pkt)
{
    size_t first, next, end;
    if (p->video_cursor >= p->video_end) return MR_EAGAIN;
    first = find_picture_start(p->buf, p->video_end, p->video_cursor);
    next = first < p->video_end
         ? find_picture_start(p->buf, p->video_end, first + 4)
         : p->video_end;
    end = next > p->video_cursor ? next : p->video_end;
    pkt->is_video = 1;
    pkt->data = p->buf + p->video_cursor;
    pkt->len = (uint32_t)(end - p->video_cursor);
    p->video_cursor = end;
    return pkt->len ? MR_OK : MR_EAGAIN;
}

/* MP2 uses a streaming input buffer in mr_audio_decode.c.  Unlike video there
 * is no need to discover codec frame boundaries here: bounded byte chunks are
 * enough to prevent one large PES from monopolising the single Amiga task,
 * while a frame split across two chunks is transparently reassembled by
 * plm_buffer_write()/plm_audio_decode(). */
static mr_status next_audio_chunk(mr_ps *p, mr_packet *pkt)
{
    size_t left, take;
    if (p->audio_cursor >= p->audio_end) return MR_EAGAIN;
    left = p->audio_end - p->audio_cursor;
    take = left > MR_PS_AUDIO_CHUNK_MAX ? MR_PS_AUDIO_CHUNK_MAX : left;
    pkt->is_video = 0;
    pkt->data = p->buf + p->audio_cursor;
    pkt->len = (uint32_t)take;
    p->audio_cursor += take;
    return take ? MR_OK : MR_EAGAIN;
}

static int parse_sequence(mr_ps *p)
{
    static const uint32_t rates[9] = {
        0, 24000, 24, 25, 30000, 30, 50, 60000, 60
    };
    static const uint32_t scales[9] = {
        0, 1001, 1, 1, 1001, 1, 1, 1001, 1
    };
    size_t i;
    for (i = 0; i + 8 <= p->len; i++) {
        unsigned width, height, rate_code;
        if (p->buf[i] || p->buf[i + 1] || p->buf[i + 2] != 1 ||
            p->buf[i + 3] != 0xb3)
            continue;
        width = ((unsigned)p->buf[i + 4] << 4) | (p->buf[i + 5] >> 4);
        height = ((unsigned)(p->buf[i + 5] & 15) << 8) | p->buf[i + 6];
        rate_code = p->buf[i + 7] & 15;
        if (!width || !height || !rate_code || rate_code > 8) return 0;
        p->video.fourcc = video_fourcc(p->buf, p->len, i);
        p->video.width = (int)width;
        p->video.height = (int)height;
        p->video.rate = rates[rate_code];
        p->video.scale = scales[rate_code];
        p->video.valid = 1;
        return 1;
    }
    return 0;
}

/* Locate a PES payload. MPEG-2 PES has the 10 marker in byte 6 and an
 * explicit header-data length. MPEG-1 PES uses stuffing and optional STD/PTS
 * fields instead. */
static int pes_payload(const uint8_t *b, size_t end, size_t start,
                       size_t *payload, size_t *packet_end)
{
    size_t n, pos;
    if (start + 6 > end) return 0;
    n = mr_rb16(b + start + 4);
    *packet_end = n ? start + 6 + n : end;
    if (!n) {
        size_t next = start + 6;
        while ((next = find_start(b, end, next)) < end) {
            /* Codes below B9 belong to the video elementary stream. */
            if (b[next + 3] >= 0xb9) {
                *packet_end = next;
                break;
            }
            next += 4;
        }
    }
    if (*packet_end > end) return 0;
    pos = start + 6;
    if (pos >= *packet_end) return 0;
    if ((b[pos] & 0xc0) == 0x80) {
        if (pos + 3 > *packet_end || pos + 3 + b[pos + 2] > *packet_end)
            return 0;
        pos += 3 + b[pos + 2];
    } else {
        while (pos < *packet_end && b[pos] == 0xff) pos++;
        if (pos + 2 <= *packet_end && (b[pos] & 0xc0) == 0x40) pos += 2;
        if (pos >= *packet_end) return 0;
        if ((b[pos] & 0xf0) == 0x20) pos += 5;
        else if ((b[pos] & 0xf0) == 0x30) pos += 10;
        else if (b[pos] == 0x0f) pos++;
        else return 0;
        if (pos > *packet_end) return 0;
    }
    *payload = pos;
    return 1;
}

static void parse_mp2_audio(mr_ps *p, const uint8_t *b, size_t len)
{
    static const uint32_t rates[3] = { 44100, 48000, 32000 };
    size_t i;
    for (i = 0; i + 4 <= len; i++) {
        unsigned version, layer, sri, mode;
        if (b[i] != 0xff || (b[i + 1] & 0xe0) != 0xe0) continue;
        version = (b[i + 1] >> 3) & 3;
        layer = (b[i + 1] >> 1) & 3;
        sri = (b[i + 2] >> 2) & 3;
        mode = b[i + 3] >> 6;
        if ((version != 2 && version != 3) || layer != 2 || sri == 3)
            continue;
        p->audio.format_tag = MR_AUDIO_FORMAT_MP2;
        p->audio.sample_rate = rates[sri] / (version == 2 ? 2 : 1);
        p->audio.channels = mode == 3 ? 1 : 2;
        p->audio.bits_per_sample = 16;
        p->audio.valid = 1;
        return;
    }
}

mr_status mr_ps_open(mr_ps *p, const uint8_t *buf, size_t len)
{
    size_t i;
    if (!p || !buf || len < 16 || buf[0] || buf[1] || buf[2] != 1 ||
        buf[3] != 0xba)
        return MR_EFORMAT;
    memset(p, 0, sizeof *p);
    p->buf = buf;
    p->len = len;
    p->video_stream = 0xe0;
    p->audio_stream = 0xc0;
    if (!parse_sequence(p)) return MR_EUNSUPPORTED;
    /* Prefer the first video stream actually present. */
    for (i = find_start(buf, len, 4); i < len;
         i = find_start(buf, len, i + 4)) {
        if (buf[i + 3] >= 0xe0 && buf[i + 3] <= 0xef) {
            p->video_stream = buf[i + 3];
            break;
        }
    }
    for (i = find_start(buf, len, 4); i < len;
         i = find_start(buf, len, i + 4)) {
        unsigned code = buf[i + 3];
        size_t payload, end;
        if (code < 0xc0 || code > 0xdf ||
            !pes_payload(buf, len, i, &payload, &end))
            continue;
        p->audio_stream = (uint8_t)code;
        parse_mp2_audio(p, buf + payload, end - payload);
        break;
    }
    return MR_OK;
}

mr_status mr_ps_next_packet(mr_ps *p, mr_packet *pkt)
{
    size_t start = p->cursor;
    if (p->video_cursor < p->video_end)
        return next_video_chunk(p, pkt);
    if (p->audio_cursor < p->audio_end)
        return next_audio_chunk(p, pkt);
    while ((start = find_start(p->buf, p->len, start)) < p->len) {
        unsigned code = p->buf[start + 3];
        size_t payload, end;
        start += 4;
        if (code != p->video_stream &&
            (!p->audio.valid || code != p->audio_stream)) continue;
        start -= 4;
        if (!pes_payload(p->buf, p->len, start, &payload, &end)) {
            p->cursor = start + 4;
            start = p->cursor;
            continue;
        }
        p->cursor = end;
        if (code == p->video_stream) {
            p->video_cursor = payload;
            p->video_end = end;
            return next_video_chunk(p, pkt);
        }
        p->audio_cursor = payload;
        p->audio_end = end;
        return next_audio_chunk(p, pkt);
    }
    p->cursor = p->len;
    return MR_EAGAIN;
}

void mr_ps_rewind(mr_ps *p)
{
    p->cursor = 0;
    p->video_cursor = 0;
    p->video_end = 0;
    p->audio_cursor = 0;
    p->audio_end = 0;
}
void mr_ps_close(mr_ps *p) { (void)p; }
