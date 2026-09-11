/* MintVID - MPEG-1/2 program-stream demuxer. */
#ifndef MR_PS_H
#define MR_PS_H

#include "mr_demux.h"

typedef struct {
    const uint8_t *buf;
    size_t         len;
    /* Independent, monotonically-advancing scan positions used to locate the
     * next video/audio PES respectively - see mr_ps_next_packet()'s own
     * comment for why these must not share one cursor: fair delivery needs
     * to discover a pending audio PES while a video one is still being
     * drained (and vice versa), which a single shared scan position cannot
     * do without one stream's search consuming ground the other still
     * needs to cover. */
    size_t         video_scan;
    size_t         audio_scan;
    size_t         video_cursor;
    size_t         video_end;
    size_t         audio_cursor;
    size_t         audio_end;
    /* PTS of the PES packet currently being handed out in chunks; consumed by
     * the first chunk, since only one picture starts in a PES and only the
     * first audio chunk begins where the timestamp points. */
    int            pending_has_pts;
    uint64_t       pending_pts_us;
    int            pending_audio_has_pts;
    uint64_t       pending_audio_pts_us;
    /* Which stream the last-returned chunk came from, so mr_ps_next_packet()
     * alternates types whenever both have data pending instead of letting
     * one drain completely before the other gets a turn. */
    int            last_chunk_was_video;
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