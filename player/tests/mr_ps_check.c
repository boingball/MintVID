#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "../core/mr_ps.h"

int main(void)
{
    static const uint8_t stream[] = {
        /* MPEG-2 pack header (the demuxer only needs its start code). */
        0x00,0x00,0x01,0xba, 0x44,0x00,0x04,0x00,
        /* Video PES: MPEG-2 PES header, then a 720x480, 29.97 sequence. */
        0x00,0x00,0x01,0xe0, 0x00,0x19,
        0x80,0x00,0x00,
        0x00,0x00,0x01,0xb3, 0x2d,0x01,0xe0,0x34,
        0x12,0x34,0x56,0x78,
        /* MPEG-2 sequence extension (extension_start_code_identifier 1). */
        0x00,0x00,0x01,0xb5, 0x14,0x8a,0x00,0x01,0x00,0x00,
        /* MPEG-1 Layer II audio PES, 44.1 kHz stereo. */
        0x00,0x00,0x01,0xc0, 0x00,0x07,
        0x80,0x00,0x00, 0xff,0xfd,0x80,0x00
    };
    mr_ps ps;
    mr_packet packet;
    static const uint8_t multi_picture_stream[] = {
        0x00,0x00,0x01,0xba, 0x44,0x00,0x04,0x00,
        /* One video PES containing a sequence header and three pictures. */
        0x00,0x00,0x01,0xe0, 0x00,0x1d,
        0x80,0x00,0x00,
        0x00,0x00,0x01,0xb3, 0x04,0x20,0x32,0x13,
        0x00,0x00,0x01,0x00, 0x11,0x22,
        0x00,0x00,0x01,0x00, 0x33,0x44,
        0x00,0x00,0x01,0x00, 0x55,0x66
    };
    /* Same pack/video prefix as `stream`, but with a deliberately large MP2
     * PES payload.  The scheduler-facing demux contract must bound this to
     * 512-byte packets so one pl_mpeg feed cannot decode a whole PES worth of
     * audio before mrplay gets another chance to present video/service Paula. */
    uint8_t split_audio_stream[39 + 6 + 8 + 1200];
    size_t apos;
    /* The same shape again, but every PES now carries a PTS: the MPEG-2 form
     * (flags byte, header length, then five marker-interleaved bytes) on the
     * video, the MPEG-1 form (stuffing, then a bare 0x2x PTS) on the audio.
     * 90000 ticks is 1 s; 135000 is 1.5 s. */
    static const uint8_t pts_stream[] = {
        0x00,0x00,0x01,0xba, 0x44,0x00,0x04,0x00,
        /* Video PES, PTS = 90000 ticks (1.000000 s), three pictures. */
        0x00,0x00,0x01,0xe0, 0x00,0x22,
        0x80,0x80,0x05, 0x21,0x00,0x05,0xbf,0x21,
        0x00,0x00,0x01,0xb3, 0x04,0x20,0x32,0x13,
        0x00,0x00,0x01,0x00, 0x11,0x22,
        0x00,0x00,0x01,0x00, 0x33,0x44,
        0x00,0x00,0x01,0x00, 0x55,0x66,
        /* MPEG-1-style audio PES: 0xff stuffing, then a lone PTS of 135000
         * ticks (1.500000 s), then four payload bytes. */
        0x00,0x00,0x01,0xc0, 0x00,0x0a,
        0xff, 0x21,0x00,0x09,0x1e,0xb1, 0xff,0xfd,0x80,0x00
    };

    if (mr_ps_open(&ps, stream, sizeof stream) != MR_OK) {
        fprintf(stderr, "could not open synthetic MPEG-PS\n");
        return 1;
    }
    if (!ps.video.valid ||
        ps.video.fourcc != MR_FOURCC('m','p','g','2') ||
        ps.video.width != 720 || ps.video.height != 480 ||
        ps.video.rate != 30000 || ps.video.scale != 1001) {
        fprintf(stderr, "bad MPEG video metadata\n");
        return 1;
    }
    if (!ps.audio.valid || ps.audio.format_tag != MR_AUDIO_FORMAT_MP2 ||
        ps.audio.sample_rate != 44100 || ps.audio.channels != 2) {
        fprintf(stderr, "bad MP2 audio metadata\n");
        return 1;
    }
    if (mr_ps_next_packet(&ps, &packet) != MR_OK || !packet.is_video ||
        packet.len != 22 || packet.data[3] != 0xb3) {
        fprintf(stderr, "bad video PES payload\n");
        return 1;
    }
    if (mr_ps_next_packet(&ps, &packet) != MR_OK || packet.is_video ||
        packet.len != 4 || packet.data[0] != 0xff) {
        fprintf(stderr, "bad audio PES payload\n");
        return 1;
    }
    if (mr_ps_next_packet(&ps, &packet) != MR_EAGAIN) {
        fprintf(stderr, "expected end of program stream\n");
        return 1;
    }
    mr_ps_rewind(&ps);
    if (mr_ps_next_packet(&ps, &packet) != MR_OK) {
        fprintf(stderr, "rewind did not restore first packet\n");
        return 1;
    }
    mr_ps_close(&ps);

    if (mr_ps_open(&ps, multi_picture_stream,
                   sizeof multi_picture_stream) != MR_OK) {
        fprintf(stderr, "could not open multi-picture MPEG-PS\n");
        return 1;
    }
    if (ps.video.fourcc != MR_FOURCC('m','p','g','1')) {
        fprintf(stderr, "MPEG-1 stream was not identified as MPEG-1\n");
        return 1;
    }
    if (mr_ps_next_packet(&ps, &packet) != MR_OK || !packet.is_video ||
        packet.len != 14 || packet.data[3] != 0xb3) {
        fprintf(stderr, "bad first split video payload\n");
        return 1;
    }
    if (mr_ps_next_packet(&ps, &packet) != MR_OK || !packet.is_video ||
        packet.len != 6 || packet.data[3] != 0x00) {
        fprintf(stderr, "bad second split video payload\n");
        return 1;
    }
    if (mr_ps_next_packet(&ps, &packet) != MR_OK || !packet.is_video ||
        packet.len != 6 || packet.data[3] != 0x00) {
        fprintf(stderr, "bad final split video payload\n");
        return 1;
    }
    if (mr_ps_next_packet(&ps, &packet) != MR_EAGAIN) {
        fprintf(stderr, "expected end of split program stream\n");
        return 1;
    }
    mr_ps_close(&ps);

    /* Build a valid PS around a 1200-byte audio PES without spelling 1200
     * filler bytes into this source file. `stream`'s first 39 bytes are the
     * complete pack + video PES. */
    memcpy(split_audio_stream, stream, 39);
    apos = 39;
    split_audio_stream[apos++] = 0x00;
    split_audio_stream[apos++] = 0x00;
    split_audio_stream[apos++] = 0x01;
    split_audio_stream[apos++] = 0xc0;
    split_audio_stream[apos++] = 0x04; /* PES length = 8-byte header + 1200 */
    split_audio_stream[apos++] = 0xb8;
    split_audio_stream[apos++] = 0x80;
    split_audio_stream[apos++] = 0x80; /* PTS present */
    split_audio_stream[apos++] = 0x05;
    /* 90000 ticks = 1.000000 s. */
    split_audio_stream[apos++] = 0x21;
    split_audio_stream[apos++] = 0x00;
    split_audio_stream[apos++] = 0x05;
    split_audio_stream[apos++] = 0xbf;
    split_audio_stream[apos++] = 0x21;
    memset(split_audio_stream + apos, 0x55, 1200);
    split_audio_stream[apos + 0] = 0xff;
    split_audio_stream[apos + 1] = 0xfd;
    split_audio_stream[apos + 2] = 0x80;
    split_audio_stream[apos + 3] = 0x00;

    if (mr_ps_open(&ps, split_audio_stream, sizeof split_audio_stream) != MR_OK) {
        fprintf(stderr, "could not open split-audio MPEG-PS\n");
        return 1;
    }
    if (!ps.audio.valid || ps.audio.format_tag != MR_AUDIO_FORMAT_MP2) {
        fprintf(stderr, "large audio PES was not recognised as MP2\n");
        return 1;
    }
    if (mr_ps_next_packet(&ps, &packet) != MR_OK || !packet.is_video) {
        fprintf(stderr, "split-audio stream lost its video packet\n");
        return 1;
    }
    /* The PES timestamp points at where the payload starts, so the first
     * bounded chunk carries it and the rest of the same PES must not. */
    if (mr_ps_next_packet(&ps, &packet) != MR_OK || packet.is_video ||
        packet.len != 512 || packet.data[0] != 0xff ||
        !packet.has_pts || packet.pts_us != 1000000u) {
        fprintf(stderr, "first bounded audio chunk is wrong (has_pts=%d "
                        "pts_us=%lu)\n",
                packet.has_pts, (unsigned long)packet.pts_us);
        return 1;
    }
    if (mr_ps_next_packet(&ps, &packet) != MR_OK || packet.is_video ||
        packet.len != 512 || packet.has_pts) {
        fprintf(stderr, "second bounded audio chunk is wrong\n");
        return 1;
    }
    if (mr_ps_next_packet(&ps, &packet) != MR_OK || packet.is_video ||
        packet.len != 176 || packet.has_pts) {
        fprintf(stderr, "final bounded audio chunk is wrong\n");
        return 1;
    }
    if (mr_ps_next_packet(&ps, &packet) != MR_EAGAIN) {
        fprintf(stderr, "bounded audio stream did not end cleanly\n");
        return 1;
    }
    mr_ps_close(&ps);

    /* PES timestamps. The player's A/V sync and the libmpeg2 adapter's
     * reorder tagging both key off mr_packet::has_pts, and an MPEG-PS clip
     * whose timestamps never arrive falls back to a synthetic frame counter
     * without ever failing - so pin the values, and pin which chunk carries
     * them.
     *
     * This stream's video PES (3 pictures) is immediately followed by one
     * audio PES, and mr_ps_next_packet() now delivers them fairly instead of
     * draining the whole video PES first (see mr_ps_next_packet()'s own
     * comment): once both a video and an audio PES are open, it alternates
     * between them rather than letting one run ahead of the other, only
     * falling back to "whichever one has data" once a stream runs dry. So
     * the audio chunk (this stream's only one) is delivered second, between
     * the first and second video pictures, not last. */
    if (mr_ps_open(&ps, pts_stream, sizeof pts_stream) != MR_OK) {
        fprintf(stderr, "could not open timestamped MPEG-PS\n");
        return 1;
    }
    /* A PES packet's PTS belongs to the first picture that starts in it, so
     * only the first of its picture-sized chunks may claim it. */
    if (mr_ps_next_packet(&ps, &packet) != MR_OK || !packet.is_video ||
        !packet.has_pts || packet.pts_us != 1000000u) {
        fprintf(stderr, "MPEG-2 PES video PTS not decoded (has_pts=%d "
                        "pts_us=%lu, expected 1 1000000)\n",
                packet.has_pts, (unsigned long)packet.pts_us);
        return 1;
    }
    /* MPEG-1 PES puts the PTS after the stuffing bytes instead, with no
     * flags/length pair in front of it. Both streams have data pending at
     * this point (3 video pictures still to come, this one audio chunk), so
     * fair delivery serves it now rather than after the video PES drains. */
    if (mr_ps_next_packet(&ps, &packet) != MR_OK || packet.is_video ||
        !packet.has_pts || packet.pts_us != 1500000u) {
        fprintf(stderr, "MPEG-1 PES audio PTS not decoded or delivered out "
                        "of fair order (is_video=%d has_pts=%d pts_us=%lu, "
                        "expected video=0 has_pts=1 pts_us=1500000)\n",
                packet.is_video, packet.has_pts, (unsigned long)packet.pts_us);
        return 1;
    }
    if (packet.len != 4 || packet.data[0] != 0xff) {
        fprintf(stderr, "MPEG-1 PES PTS was not skipped before the payload\n");
        return 1;
    }
    /* The audio stream is now exhausted, so the remaining two pictures come
     * back one after another with no more interleaving to do. */
    if (mr_ps_next_packet(&ps, &packet) != MR_OK || !packet.is_video ||
        packet.has_pts) {
        fprintf(stderr, "second picture of a PES must not reuse its PTS\n");
        return 1;
    }
    if (mr_ps_next_packet(&ps, &packet) != MR_OK || !packet.is_video ||
        packet.has_pts) {
        fprintf(stderr, "third picture of a PES must not reuse its PTS\n");
        return 1;
    }
    if (mr_ps_next_packet(&ps, &packet) != MR_EAGAIN) {
        fprintf(stderr, "expected end of timestamped program stream\n");
        return 1;
    }
    mr_ps_close(&ps);

    /* Regression for the fair-delivery fix itself: a long run of pictures
     * packed into one video PES (more than any real audio chunk cadence
     * would tolerate) must not fully drain before a pending audio PES gets
     * served. A real MPEG-PS capture hit a run of 19 consecutive video
     * packets with no audio in between, starving the player's software
     * audio FIFO and triggering repeated audio-rescue episodes that then
     * dropped video to catch back up - see mr_ps_next_packet()'s comment.
     * The fixture is built at runtime (PES lengths computed from the actual
     * bytes written) rather than hand-counted, since a 20-picture PES is too
     * easy to miscount by hand. */
    {
#define LONG_RUN_PICTURES 20
        uint8_t buf[8 + 6 + 3 + 8 + 6 * LONG_RUN_PICTURES + 6 + 1 + 5 + 4];
        size_t pos = 0, video_pes_start, video_payload_start, video_len;
        size_t audio_pes_start, audio_payload_start, audio_len;
        size_t i;
        int seen_audio_at = -1;
        mr_status st;

        /* Pack header - only its start code matters to this demuxer. */
        buf[pos++] = 0x00; buf[pos++] = 0x00; buf[pos++] = 0x01; buf[pos++] = 0xba;
        buf[pos++] = 0x44; buf[pos++] = 0x00; buf[pos++] = 0x04; buf[pos++] = 0x00;

        /* Video PES: start code + 16-bit length (patched in below once the
         * payload size is known), no PTS, then a sequence header (the same
         * small picture as multi_picture_stream/pts_stream above) followed
         * by LONG_RUN_PICTURES picture start codes. */
        video_pes_start = pos;
        pos += 6;
        video_payload_start = pos;
        buf[pos++] = 0x80; buf[pos++] = 0x00; buf[pos++] = 0x00;
        buf[pos++] = 0x00; buf[pos++] = 0x00; buf[pos++] = 0x01; buf[pos++] = 0xb3;
        buf[pos++] = 0x04; buf[pos++] = 0x20; buf[pos++] = 0x32; buf[pos++] = 0x13;
        for (i = 0; i < LONG_RUN_PICTURES; i++) {
            buf[pos++] = 0x00; buf[pos++] = 0x00; buf[pos++] = 0x01; buf[pos++] = 0x00;
            buf[pos++] = (uint8_t)(0x10 + i); buf[pos++] = (uint8_t)(0x20 + i);
        }
        video_len = pos - video_payload_start;
        buf[video_pes_start + 0] = 0x00; buf[video_pes_start + 1] = 0x00;
        buf[video_pes_start + 2] = 0x01; buf[video_pes_start + 3] = 0xe0;
        buf[video_pes_start + 4] = (uint8_t)(video_len >> 8);
        buf[video_pes_start + 5] = (uint8_t)(video_len & 0xff);

        /* Audio PES right after the whole video run: MPEG-1-style stuffing
         * plus a lone PTS (2.000000 s = 180000 ticks), then a minimal MP2
         * sync payload - same shape as pts_stream's audio PES above. */
        audio_pes_start = pos;
        pos += 6;
        audio_payload_start = pos;
        buf[pos++] = 0xff;
        buf[pos++] = 0x21; buf[pos++] = 0x00; buf[pos++] = 0x0b;
        buf[pos++] = 0x7e; buf[pos++] = 0x41;
        buf[pos++] = 0xff; buf[pos++] = 0xfd; buf[pos++] = 0x80; buf[pos++] = 0x00;
        audio_len = pos - audio_payload_start;
        buf[audio_pes_start + 0] = 0x00; buf[audio_pes_start + 1] = 0x00;
        buf[audio_pes_start + 2] = 0x01; buf[audio_pes_start + 3] = 0xc0;
        buf[audio_pes_start + 4] = (uint8_t)(audio_len >> 8);
        buf[audio_pes_start + 5] = (uint8_t)(audio_len & 0xff);

        if (pos != sizeof buf) {
            fprintf(stderr, "long-run fixture size mismatch (built %lu, "
                            "buffer %lu)\n",
                    (unsigned long)pos, (unsigned long)sizeof buf);
            return 1;
        }
        if (mr_ps_open(&ps, buf, sizeof buf) != MR_OK) {
            fprintf(stderr, "could not open long-run MPEG-PS\n");
            return 1;
        }
        for (i = 0; ; i++) {
            st = mr_ps_next_packet(&ps, &packet);
            if (st == MR_EAGAIN) break;
            if (st != MR_OK) {
                fprintf(stderr, "long-run stream: unexpected demux status "
                                "%d at packet %lu\n", (int)st,
                        (unsigned long)i);
                return 1;
            }
            if (!packet.is_video && seen_audio_at < 0) {
                seen_audio_at = (int)i;
                if (!packet.has_pts || packet.pts_us != 2000000u) {
                    fprintf(stderr, "long-run stream: audio PTS wrong "
                                    "(has_pts=%d pts_us=%lu, expected 1 "
                                    "2000000)\n",
                            packet.has_pts, (unsigned long)packet.pts_us);
                    return 1;
                }
            }
        }
        if (seen_audio_at < 0) {
            fprintf(stderr, "long-run stream: audio packet never appeared\n");
            return 1;
        }
        /* The property the fix promises: audio must not wait for the whole
         * video run to drain. Deliberately not pinned to the exact index
         * fair alternation happens to produce today (packet 1) - only that
         * it is not starved until the run's end, so a future tweak to the
         * alternation schedule that still interleaves reasonably cannot
         * false-fail this. */
        if (seen_audio_at >= LONG_RUN_PICTURES) {
            fprintf(stderr, "long-run stream: audio packet %d arrived only "
                            "after the whole %d-picture video run drained - "
                            "starvation fix regressed\n",
                    seen_audio_at, LONG_RUN_PICTURES);
            return 1;
        }
        mr_ps_close(&ps);
#undef LONG_RUN_PICTURES
    }
    return 0;
}