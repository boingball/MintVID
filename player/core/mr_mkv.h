/* MintVID - bounded Matroska demuxer for existing codec backends. */
#ifndef MR_MKV_H
#define MR_MKV_H

#include "mr_demux.h"
#include "mr_source.h"

typedef struct {
    const uint8_t *buf;
    size_t len;
    mr_source *source;
    int file_backed;

    uint64_t segment_start, segment_end;
    uint64_t first_cluster, cursor, cluster_end, cluster_timecode;
    uint64_t timecode_scale;
    uint64_t default_duration_ns;
    uint64_t audio_default_duration_ns;
    uint64_t video_track, audio_track;

    uint8_t *scratch;
    size_t scratch_cap;
    uint8_t *packet;
    size_t packet_cap;
    uint32_t lace_offset[256];
    uint32_t lace_size[256];
    unsigned lace_count, lace_index;
    uint64_t lace_pts_us, lace_duration_ns;
    int lace_video;
    uint8_t *video_config;
    size_t video_config_len;

    /* Seeking. cues_pos is the absolute offset of the Cues element (from the
     * SeekHead, or seen before the first cluster), 0 if unknown. The cue
     * table is loaded on the first seek: cues_state 0 = not tried, 1 =
     * loaded, -1 = absent or unusable (seek then scans cluster timecodes). */
    uint64_t cues_pos;
    int cues_state;
    unsigned cue_count;
    uint64_t *cue_time;      /* CueTime, in timecode-scale ticks          */
    uint64_t *cue_cluster;   /* absolute offset of the cluster it names   */

    mr_video_info video;
    mr_audio_info audio;
} mr_mkv;

mr_status mr_mkv_open(mr_mkv *m, const uint8_t *buf, size_t len);
mr_status mr_mkv_open_source(mr_mkv *m, mr_source *source, size_t len);
mr_status mr_mkv_next_packet(mr_mkv *m, mr_packet *pkt);
/* Reposition so the next packet is the video keyframe at or before
 * target_us (the first keyframe of the file if target_us is earlier).
 * *out_us receives that keyframe's presentation time. Uses the Cues index
 * when the file has one and otherwise scans cluster timecodes, so files
 * written without an index (e.g. to a pipe) still seek. Packets before the
 * keyframe inside its cluster are skipped, audio included. */
mr_status mr_mkv_seek(mr_mkv *m, uint64_t target_us, uint64_t *out_us);
void mr_mkv_rewind(mr_mkv *m);
void mr_mkv_close(mr_mkv *m);

#endif
