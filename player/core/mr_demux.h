/*
 * MintVID - container-agnostic demux interface.
 *
 * The player shouldn't care whether frames come from AVI or QuickTime MOV, so
 * all parsers fill these neutral info/packet structs and are reached through
 * one auto-detecting front end (mr_demux_open sniffs the signature). Adding a
 * container = adding a backend here, exactly like adding a codec behind
 * mr_codec.h.
 */
#ifndef MR_DEMUX_H
#define MR_DEMUX_H

#include "mr_types.h"

typedef struct {
    uint32_t fourcc;      /* video codec (e.g. 'cvid')                      */
    int      width;
    int      height;
    uint32_t rate;        /* fps = rate / scale                             */
    uint32_t scale;
    /* Borrowed container decoder setup.  For avc1 this is the avcC payload,
     * including SPS/PPS and the AVCC NAL length size. */
    const uint8_t *config;
    uint32_t config_len;
    int      valid;
} mr_video_info;

#define MR_AUDIO_CONFIG_MAX 16
#define MR_AUDIO_FORMAT_PCM 0x0001
#define MR_AUDIO_FORMAT_MP3 0x0055
#define MR_AUDIO_FORMAT_MP2 0x0050
#define MR_AUDIO_FORMAT_AAC 0x00ff
#define MR_AUDIO_FORMAT_AC3 0x2000

typedef struct {
    uint16_t format_tag;  /* WAVE tag (AVI) or mapped from MOV codec        */
    uint32_t codec_tag;   /* original WAVE tag or MOV audio sample-entry 4CC */
    uint16_t channels;
    uint32_t sample_rate;
    uint16_t bits_per_sample;
    uint16_t block_align; /* bytes in one complete interleaved sample frame */
    uint8_t  pcm_signed;  /* source PCM uses two's-complement samples        */
    uint8_t  pcm_big_endian; /* multi-byte PCM stores its MSB first         */
    /* Container codec setup bytes.  MP4 AAC stores its AudioSpecificConfig
     * here; packet decoders may ignore this for self-describing formats such
     * as PCM, MP3 and ADTS AAC. */
    uint8_t  config[MR_AUDIO_CONFIG_MAX];
    uint8_t  config_len;
    int      valid;
} mr_audio_info;

typedef struct {
    int            is_video;
    /* Presentation timestamp in the container timebase converted to usec.
     * Demuxers which do not expose timestamps leave has_pts clear. */
    int            has_pts;
    uint64_t       pts_us;
    /* Borrowed until the next mr_demux_next_packet call.  Memory-backed
     * demuxers point into their input; file-backed demuxers reuse one buffer. */
    const uint8_t *data;
    uint32_t       len;
    /* H.264 only: 1 when data is already Annex-B (start-code-prefixed) NAL
     * data, as MPEG-TS carries it natively - lets the H.264 decoder skip its
     * own AVCC->Annex-B conversion and decode straight from this buffer (see
     * mr_h264_set_input_annexb()). 0 (the default mr_demux_next_packet()
     * resets every packet to) for MOV/MP4's native AVCC avc1 samples and for
     * every non-H.264 packet, which never reads this field. */
    int            is_annexb;
} mr_packet;

typedef enum {
    MR_CONTAINER_NONE = 0,
    MR_CONTAINER_AVI,
    MR_CONTAINER_MOV,
    MR_CONTAINER_TS,
    MR_CONTAINER_PS,
    MR_CONTAINER_RAW_MJPEG,
    MR_CONTAINER_RAW_MPEG4,
    MR_CONTAINER_MKV
} mr_container;

typedef struct mr_demux mr_demux;
typedef void (*mr_demux_service_fn)(void *opaque);
typedef struct mr_demux_timing {
    unsigned long calls, call_us, call_max_us;
    unsigned long source_us, sync_us, assembly_us, copy_us;
    unsigned long audio_us, video_us, packets_scanned, service_calls;
    unsigned long source_max_us, sync_max_us, assembly_max_us, copy_max_us;
    unsigned long audio_max_us, video_max_us, scanned_max;
} mr_demux_timing;
struct mr_http_options;

/* Auto-detect container and open over an in-memory buffer (borrowed, must
 * outlive the demux). Returns NULL if unrecognised/malformed. */
mr_demux    *mr_demux_open(const uint8_t *buf, size_t len);
/* Path-backed AVI/MOV/TS opener. path may be a local filename or an http(s)
 * URL. Container metadata is retained in memory, while compressed packets are
 * read into reusable buffers on demand. HTTP seeking uses byte ranges. Raw
 * streams and MPEG program streams remain on the memory path. */
mr_demux    *mr_demux_open_file(const char *path);
mr_demux    *mr_demux_open_file_ex(const char *path,
                                    const struct mr_http_options *options);
/* True when the file signature is one of the file-backed containers. Useful
 * after an open failure so callers do not try to slurp a huge but unsupported
 * or malformed AVI/MOV/TS into memory as a raw stream. */
int          mr_demux_is_file_backed_container(const char *path);
const char  *mr_demux_last_open_error(void);
mr_status    mr_demux_next_packet(mr_demux *d, mr_packet *pkt);
void         mr_demux_set_service(mr_demux *d, mr_demux_service_fn fn,
                                  void *opaque);
void         mr_demux_timing_get(mr_demux *d, mr_demux_timing *timing,
                                 int reset);
/* Off by default. mr_demux_timing_get()'s TS-specific fields (source/sync/
 * assembly/copy/audio/video us, per-call maxima) only report real numbers
 * once this is turned on - otherwise mr_ts_next_packet() skips its clock()
 * reads (several per 188/192-byte packet - source read, sync parse, PES
 * assembly, copy) entirely, since nothing would read the result. No effect
 * on any container other than TS. */
void         mr_demux_set_timing_enabled(mr_demux *d, int enabled);
void         mr_demux_rewind(mr_demux *d);
/* True when mr_demux_seek() can reposition this demux instance - a local,
 * keyframe-indexed container (MOV/MP4 today). Streamed/live sources and
 * containers without a keyframe index (AVI, MKV, TS, PS, raw) return 0. */
int          mr_demux_can_seek(const mr_demux *d);
/* Reposition to the nearest video keyframe at or before target_us (media
 * timeline, microseconds). *out_us receives the keyframe actually reached,
 * which is not necessarily target_us. Subsequent mr_demux_next_packet calls
 * resume from there. The caller must reset any decoder reference state
 * (mr_decoder_reset, mr_audio_decoder_reset) before decoding what follows -
 * seeking does not by itself make old reference frames valid. Returns
 * MR_EUNSUPPORTED when mr_demux_can_seek() would say 0. */
mr_status    mr_demux_seek(mr_demux *d, uint64_t target_us, uint64_t *out_us);
void         mr_demux_close(mr_demux *d);
size_t       mr_demux_source_buffer_capacity(const mr_demux *d);
/* True once the source has fully preloaded the file into a Fast RAM block -
 * see mr_source_is_memory_cached(). */
int          mr_demux_source_is_cached(const mr_demux *d);

const mr_video_info *mr_demux_video(const mr_demux *d);
const mr_audio_info *mr_demux_audio(const mr_demux *d);
const char          *mr_demux_container_name(const mr_demux *d);
/* Human-readable codec identities for diagnostics and GUI error reporting.
 * These describe tracks even when no decoder exists. */
void mr_demux_describe_video_codec(const mr_demux *d, char *out, size_t cap);
void mr_demux_describe_audio_codec(const mr_demux *d, char *out, size_t cap);

#endif /* MR_DEMUX_H */
