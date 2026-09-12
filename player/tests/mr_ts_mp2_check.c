#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "../core/mr_ts.h"

/* The synthetic check exercises the in-memory TS path. Satisfy the file-backed
 * hook referenced by mr_ts.c without pulling the complete HTTP/HLS stack into
 * this tiny unit test. */
int mr_source_read_at(mr_source *source, size_t off, void *dst, size_t len)
{
    (void)source; (void)off; (void)dst; (void)len;
    return 0;
}

static uint8_t *packet(uint8_t *p, unsigned pid, int start)
{
    memset(p, 0xff, 188);
    p[0] = 0x47;
    p[1] = (uint8_t)((start ? 0x40 : 0) | ((pid >> 8) & 0x1f));
    p[2] = (uint8_t)pid;
    p[3] = 0x10;
    return p + 4;
}

int main(void)
{
    uint8_t stream[188 * 4];
    uint8_t *p;
    mr_ts ts;
    mr_packet pkt;

    p = packet(stream, 0x0000, 1);
    /* PAT: program 1 -> PMT PID 0x100. */
    { static const uint8_t pat[] = {
        0x00, 0x00,0xb0,0x0d, 0x00,0x01,0xc1,0x00,0x00,
        0x00,0x01,0xe1,0x00, 0,0,0,0
    }; memcpy(p, pat, sizeof pat); }

    p = packet(stream + 188, 0x0100, 1);
    /* PMT: MPEG-2 video PID 0x101 and stream type 0x04 audio PID 0x102. */
    { static const uint8_t pmt[] = {
        0x00, 0x02,0xb0,0x17, 0x00,0x01,0xc1,0x00,0x00,
        0xe1,0x01, 0xf0,0x00,
        0x02,0xe1,0x01,0xf0,0x00,
        0x04,0xe1,0x02,0xf0,0x00, 0,0,0,0
    }; memcpy(p, pmt, sizeof pmt); }

    p = packet(stream + 188 * 2, 0x0101, 1);
    { static const uint8_t video[] = {
        0x00,0x00,0x01,0xe0, 0x00,0x0f, 0x80,0x00,0x00,
        0x00,0x00,0x01,0xb3, 0x2d,0x01,0xe0,0x34,
        0x12,0x34,0x56,0x78
    }; memcpy(p, video, sizeof video); }

    p = packet(stream + 188 * 3, 0x0102, 1);
    { static const uint8_t audio[] = {
        0x00,0x00,0x01,0xc0, 0x00,0x07, 0x80,0x00,0x00,
        0xff,0xfd,0x80,0x00
    }; memcpy(p, audio, sizeof audio); }

    if (mr_ts_open(&ts, stream, sizeof stream) != MR_OK) {
        fprintf(stderr, "could not open synthetic MPEG-TS\n");
        return 1;
    }
    if (!ts.audio.valid || ts.audio_type != 0x04 ||
        ts.audio.format_tag != MR_AUDIO_FORMAT_MP2 ||
        ts.audio.sample_rate != 44100 || ts.audio.channels != 2) {
        fprintf(stderr, "bad MPEG Layer II metadata\n");
        return 1;
    }
    /* Audio arrives first now, video second - not because audio is somehow
     * prioritized, but because mr_ts_next_packet() no longer trusts a
     * video PES's own declared PES_packet_length to know it is complete
     * (see the fix in mr_ts.c): video only ever finalizes on the next PES
     * start code for that PID, or end-of-stream drain, matching the real
     * MPEG-2 Systems spec convention for video regardless of what this
     * particular encoder happened to declare. This fixture's one video PES
     * has no second video PES after it, so it is only ever recognized as
     * complete by the end-of-stream drain (after the audio PES, the very
     * next and only remaining PES in the stream, has already emitted
     * through the ordinary in-loop path). Before the fix this fixture's
     * declared video length happened to exactly match its real payload, so
     * it emitted eagerly and arrived first - correct by coincidence, not by
     * something the fix should have to preserve. */
    if (mr_ts_next_packet(&ts, &pkt) != MR_OK || pkt.is_video ||
        pkt.len != 4 || pkt.data[0] != 0xff || pkt.data[1] != 0xfd) {
        fprintf(stderr, "missing MPEG Layer II PES\n");
        return 1;
    }
    if (mr_ts_next_packet(&ts, &pkt) != MR_OK || !pkt.is_video) {
        fprintf(stderr, "missing video PES\n");
        return 1;
    }
    mr_ts_close(&ts);
    return 0;
}
