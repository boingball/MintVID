#ifndef MR_BERRYVP_H
#define MR_BERRYVP_H

#include "../core/mr_demux.h"
#include "../core/mr_avc_annexb.h"

struct Library;

typedef struct {
    struct Library *lib;
    mr_avc_annexb avc;
    uint8_t config_buf[4096];
    uint32_t config_len;
    int config_sent;
    unsigned long caps;
    /* AC309I: optional host-side compressed-video fragment limit.  Zero keeps
     * the normal BerryVP ABI maximum; MOV/MP4 uses the VPv149-proven 128 KiB. */
    uint32_t video_chunk_limit;
    unsigned long packets;
    unsigned long bytes;
    uint8_t *audio_buf;
    uint32_t audio_cap;
    uint8_t aac_object_type;
    uint8_t aac_sf_index;
    uint8_t aac_channels;
    uint32_t audio_format_tag;
    int audio_ready;
    unsigned long audio_packets;
    unsigned long audio_bytes;
} mr_berryvp;

extern unsigned long mr_berryvp_stream_flags;

mr_status mr_berryvp_open(mr_berryvp *b, const mr_video_info *vi);
mr_status mr_berryvp_config_audio(mr_berryvp *b, const mr_audio_info *ai);
mr_status mr_berryvp_send_packet(mr_berryvp *b, const mr_packet *pkt);
mr_status mr_berryvp_send_audio_packet(mr_berryvp *b, const mr_packet *pkt);
mr_status mr_berryvp_close(mr_berryvp *b);

#endif
