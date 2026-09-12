#ifndef MR_PLAY_OPTIONS_H
#define MR_PLAY_OPTIONS_H

#include <stddef.h>

/* AmigaDOS requester pattern for the local containers MintVID can probe.
 * Playback still sniffs the container; this is only to keep audio-only files
 * out of the video picker. */
#define MR_VIDEO_FILE_PATTERN \
    "#?.(avi|divx|mov|mp4|m4v|mkv|mpg|mpeg|mpe|vob|ts|m2ts|mts|m3u8)"

typedef enum {
    MR_DISPLAY_AGA = 0,
    MR_DISPLAY_HAM6,
    MR_DISPLAY_HAM8,
    MR_DISPLAY_CGX,
    MR_DISPLAY_P96,
    /* Native planar at reduced depth: same encoder family as MR_DISPLAY_AGA
     * (indexed dither, not HAM), but forced to 5 planes/32 colours or 4
     * planes/16 colours instead of auto-selecting by chipset. Available on
     * any chipset, including AGA, as an explicit speed/quality tradeoff. */
    MR_DISPLAY_AGA_ECS32,
    MR_DISPLAY_AGA_ECS16
} mr_display_mode;

typedef enum {
    MR_C2P_STANDARD = 0,
    MR_C2P_AKIKO,
    MR_C2P_KALMS,
    MR_C2P_RIVA,
    MR_C2P_WPA,
    /* Single-kernel dither+C2P for the plain 1:1 8-plane AGA case, 040/060
     * only - see amiga/display_aga.c's aga_supports_yuv_indexed() and
     * core/mr_yuv_dither_planar_direct_m68k.S. */
    MR_C2P_DIRECT
} mr_c2p_mode;

typedef enum {
    MR_H264_PERF_AUTO = 0,
    MR_H264_PERF_QUALITY,
    MR_H264_PERF_BALANCED,
    MR_H264_PERF_FAST,
    MR_H264_PERF_TURBO,
    MR_H264_PERF_TURBO_PLUS,
    MR_H264_PERF_TURBO_GT
} mr_h264_performance;

/* Paula output rate policy. NORMAL keeps the existing >28kHz halving
 * (48kHz->24kHz, 44.1kHz->22.05kHz - see mr_audio_decoder_open()'s stride).
 * LOW halves whatever NORMAL would already produce again (48kHz->12kHz,
 * 44.1kHz->11.025kHz, and a source already at/below 28kHz like 22.05kHz
 * drops to 11.025kHz) - a relative reduction, not a forced absolute rate,
 * so every source still lands on a clean integer stride of its own rate.
 * Noticeably telephone-like, especially for music, but a real CPU saving on
 * a 68k pushed hard by H.264 decode - fewer samples to downmix, convert to
 * 8-bit and queue to Paula. */
typedef enum {
    MR_AUDIO_RATE_NORMAL = 0,
    MR_AUDIO_RATE_LOW
} mr_audio_rate_mode;

typedef enum {
    MR_FAST_BUFFER_AUTO = 0,
    MR_FAST_BUFFER_OFF,
    MR_FAST_BUFFER_4MB,
    MR_FAST_BUFFER_8MB,
    MR_FAST_BUFFER_16MB
} mr_fast_buffer_mode;

typedef struct mr_play_options {
    mr_display_mode display;
    mr_c2p_mode c2p;
    int laced;
    int scale_2x;
    /* pass --copper-vdouble: AGA-only, opt-in, confirmed on real AGA
     * hardware with --c2p (portable) - see amiga/display_aga.c's file
     * header comment and build_copper_vdouble(); --riva-c2p/--cd32 and
     * ECS/OCS chipsets not yet exercised. No effect unless scale_2x is also
     * set and the chosen c2p/display combination qualifies (display_aga.c
     * decides that at runtime; an ineligible combination just plays
     * normally). */
    int copper_vdouble;
    int hls_low;
    unsigned hls_max_width;
    unsigned hls_max_height;
    unsigned hls_max_fps;
    int live_resync;   /* pass --live-resync: catch up / reconnect live streams */
    mr_h264_performance h264_performance;
    mr_audio_rate_mode audio_rate;
    mr_fast_buffer_mode fast_buffer;
    int no_audio;      /* pass --no-audio: skip the audio decoder/Paula entirely */
    /* pass --audio-mono: decode one channel instead of two. Paula output has
     * always been mono; this moves the fold from "decode both, average them"
     * to "ask the codec for one channel", which skips about half the
     * per-channel synthesis work in MP3, MP2 and AC-3 (Helix AAC has no mono
     * mode, so there it only saves the downmix). */
    int mono_audio;
    /* GUI label: "Video: All Frames" (1, the default) / "Skip Frames" (0).
     * Non-zero passes --throughput, zero passes --no-throughput - always
     * one or the other (see append_playback_flags()), so a GUI-launched
     * session's explicit choice overrides mrplay.c's own per-source
     * default (network/HLS on, local file off) in both directions. "All
     * Frames" means never skip a decoded video frame purely for PTS
     * lateness (skip_stale_output's pts_late clause and micro-rescue's own
     * entry are both disabled - see mrplay.c's throughput_mode and
     * CLAUDE.md's "Live HLS playback stall notes" for the real-hardware
     * regression this fixed: a decode-bound stream that fell behind the
     * live clock went from occasional lateness to no video at all once
     * this same lateness check became correct). "Skip Frames" restores
     * that lateness check, trading a frozen picture during a stall for
     * staying closer to real-time sync once decode catches back up. */
    int throughput;
} mr_play_options;

void mr_play_options_default(mr_play_options *options);
int mr_play_options_parse(mr_play_options *options, int argc, char **argv,
                          char *error, size_t error_size);
int mr_build_player_arguments(char *output, size_t output_size,
                              const mr_play_options *options, const char *url,
                              const char *user_agent, const char *referer);
int mr_build_iptv_arguments(char *output, size_t output_size,
                            const mr_play_options *options);
void mr_play_options_summary(const mr_play_options *options, char *output,
                             size_t output_size);
int mr_path_is_audio_only(const char *path);

#endif
