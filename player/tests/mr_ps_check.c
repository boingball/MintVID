#include <stdint.h>
#include <stdio.h>

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
    return 0;
}
