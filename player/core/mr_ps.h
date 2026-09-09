/* MintVID - MPEG-1/2 program-stream demuxer. */
#ifndef MR_PS_H
#define MR_PS_H

#include "mr_demux.h"

typedef struct {
    const uint8_t *buf;
    size_t         len;
    size_t         cursor;
    size_t         video_cursor;
    size_t         video_end;
    size_t         audio_cursor;
    size_t         audio_end;
    uint8_t        video_stream;
    uint8_t        audio_stream;
    mr_video_info  video;
    mr_audio_info  audio;
} mr_ps;

mr_status mr_ps_open(mr_ps *p, const uint8_t *buf, size_t len);
mr_status mr_ps_next_packet(mr_ps *p, mr_packet *pkt);
void      mr_ps_rewind(mr_ps *p);
void      mr_ps_close(mr_ps *p);

#endif