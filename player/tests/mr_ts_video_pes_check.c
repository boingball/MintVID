/*
 * Regression check for the video-PES PES_packet_length fix in mr_ts.c.
 *
 * The MPEG-2 Systems spec's documented convention is that video PES packets
 * declare PES_packet_length = 0 ("unbounded", read until the next PES start
 * code) specifically because compressed frames routinely exceed the 16-bit
 * field's 65535-byte limit. Some real-world encoders declare a real,
 * non-zero length for video anyway - and when the true access unit exceeds
 * that declared length, trusting it truncates the frame right at the
 * declared length and silently drops every following continuation TS
 * packet until the next real PES start code, which then decode_annexb()
 * sees as a NAL cut off mid-payload: h264-decode-error on every single
 * frame this happens to, indefinitely, on a live stream that never stops
 * sending more data to fail the same way (observed on two independent
 * IPTV sources, both mrplay hanging with a black display and unresponsive
 * to ESC - the flood of failing decode attempts, not a literal infinite
 * loop). mr_ts_next_packet() now always treats a *video* PES as unbounded,
 * matching the already-correct handling of a genuinely-zero declared
 * length, and only trusts the declared length for audio (ADTS/AAC frames
 * are always comfortably under the 16-bit limit, so this doesn't apply).
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "../core/mr_ts.h"

/* Satisfy the file-backed hook referenced by mr_ts.c without pulling in the
 * complete HTTP/HLS stack - this check exercises the in-memory TS path only,
 * same as mr_ts_mp2_check.c. */
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
    size_t i;

    p = packet(stream, 0x0000, 1);
    /* PAT: program 1 -> PMT PID 0x100 (same fixture as mr_ts_mp2_check.c). */
    { static const uint8_t pat[] = {
        0x00, 0x00,0xb0,0x0d, 0x00,0x01,0xc1,0x00,0x00,
        0x00,0x01,0xe1,0x00, 0,0,0,0
    }; memcpy(p, pat, sizeof pat); }

    p = packet(stream + 188, 0x0100, 1);
    /* PMT: MPEG-2 video (stream_type 0x02) on PID 0x101, no audio track -
     * this check is only about video PES reassembly, and the bug/fix is in
     * mr_ts_next_packet() itself, unconditional on codec (unlike
     * emit_pes()'s is_annexb flag, which is H.264-specific). MPEG-2 needs
     * only a valid sequence header for mr_ts_open()'s probe to succeed,
     * avoiding the much larger SPS/PPS an H.264 avcC probe would need. */
    { static const uint8_t pmt[] = {
        0x00, 0x02,0xb0,0x12, 0x00,0x01,0xc1,0x00,0x00,
        0xe1,0x01, 0xf0,0x00,
        0x02,0xe1,0x01,0xf0,0x00, 0,0,0,0
    }; memcpy(p, pmt, sizeof pmt); }

    /* Video PES, PUSI packet: declares PES_packet_length = 23, i.e. only 20
     * bytes of ES data (23 - 3 header-after-length bytes, no PTS, no extra
     * PES header data) - deliberately far short of the 175 ES bytes this
     * one TS packet alone actually carries after the 9-byte PES header. The
     * first 12 of those (a minimal valid MPEG-2 sequence header, same
     * fixture as mr_ts_mp2_check.c) let mr_ts_open()'s probe succeed; the
     * declared-length bug/fix only affects mr_ts_next_packet() afterward. */
    p = packet(stream + 188 * 2, 0x0101, 1);
    {
        static const uint8_t pes_hdr[] = {
            0x00,0x00,0x01, 0xe0, 0x00,0x17, 0x80,0x00,0x00
        };
        static const uint8_t seq_hdr[] = {
            0x00,0x00,0x01,0xb3, 0x2d,0x01,0xe0,0x34, 0x12,0x34,0x56,0x78
        };
        memcpy(p, pes_hdr, sizeof pes_hdr);
        memcpy(p + sizeof pes_hdr, seq_hdr, sizeof seq_hdr);
        memset(p + sizeof pes_hdr + sizeof seq_hdr, 0x11,
               184 - sizeof pes_hdr - sizeof seq_hdr);
    }

    /* Continuation TS packet for the same access unit: no PES header, no
     * PUSI - a real too-large frame's payload keeps arriving here. Under
     * the declared-length-truncation bug this never reaches the caller at
     * all (dropped once the PUSI packet above already satisfied and
     * emitted the "expected" 20 bytes). */
    p = packet(stream + 188 * 3, 0x0101, 0);
    memset(p, 0x22, 184);

    if (mr_ts_open(&ts, stream, sizeof stream) != MR_OK) {
        fprintf(stderr, "could not open synthetic MPEG-TS\n");
        return 1;
    }
    if (mr_ts_next_packet(&ts, &pkt) != MR_OK || !pkt.is_video) {
        fprintf(stderr, "missing video PES\n");
        return 1;
    }
    if (pkt.len != 175 + 184) {
        fprintf(stderr,
                "video PES truncated to the declared PES_packet_length: "
                "got %lu bytes, want %lu (the too-small declared length "
                "was trusted instead of being ignored for video)\n",
                (unsigned long)pkt.len, (unsigned long)(175 + 184));
        return 1;
    }
    {
        static const uint8_t seq_hdr[] = {
            0x00,0x00,0x01,0xb3, 0x2d,0x01,0xe0,0x34, 0x12,0x34,0x56,0x78
        };
        if (memcmp(pkt.data, seq_hdr, sizeof seq_hdr) != 0) {
            fprintf(stderr, "sequence header at the start of the video PES "
                            "corrupted\n");
            return 1;
        }
    }
    for (i = 12; i < 175; i++)
        if (pkt.data[i] != 0x11) {
            fprintf(stderr, "byte %lu of the first TS packet's ES data "
                            "corrupted\n", (unsigned long)i);
            return 1;
        }
    for (i = 175; i < pkt.len; i++)
        if (pkt.data[i] != 0x22) {
            fprintf(stderr, "byte %lu of the continuation TS packet's ES "
                            "data missing or corrupted\n", (unsigned long)i);
            return 1;
        }
    mr_ts_close(&ts);
    puts("MPEG-TS video PES reassembly check passed");
    return 0;
}
