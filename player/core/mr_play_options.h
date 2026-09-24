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
    /* Picasso96 (Picasso96API.library). display_open() (amiga/display.c)
     * tries two backends under this one option, in order: a "PIP"
     * (Picture-In-Picture) overlay window - p96PIP_OpenTags() with
     * P96PIP_Type=PIPT_VideoWindow (falling back to PIPT_MemoryWindow if the
     * board/driver refuses a real hardware video window), writing BGR24 into
     * the PIP's own dedicated source bitmap - first, and the older direct
     * screen-bitmap lock (p96LockBitMap) if the PIP backend can't open -
     * see amiga/display_p96pip.c's/display_p96.c's file headers. Whether the
     * destination board's overlay hardware actually engages for an RGB
     * (rather than YUV) source, or Picasso96 silently falls back to a
     * software-composited window, is unconfirmed without a real board to
     * test against (the Picasso96.h RGBFTYPE comment marks only the YUV
     * formats as "for use with a hardware window only", which the PIP
     * backend deliberately does not use yet - see its file header for why).
     */
    MR_DISPLAY_P96,
    /* Native planar at reduced depth: same encoder family as MR_DISPLAY_AGA
     * (indexed dither, not HAM), but forced to 5 planes/32 colours or 4
     * planes/16 colours instead of auto-selecting by chipset. Available on
     * any chipset, including AGA, as an explicit speed/quality tradeoff. */
    MR_DISPLAY_AGA_ECS32,
    MR_DISPLAY_AGA_ECS16,
    /* Extra Half-Brite: genuine ECS/OCS chipset feature (no AGA needed), 6
     * planes for 64 apparent colours - 32 real palette registers plus 32
     * free half-brightness duplicates the hardware derives itself. See
     * core/mr_dither.h's mr_dither_rgb_ehb()/mr_dither_palette_ehb() and
     * amiga/display_aga.c's EHB section. */
    MR_DISPLAY_AGA_EHB,
    /* Fullscreen P96 startup; append to preserve existing saved enum values. */
    MR_DISPLAY_P96_FULLSCREEN,
    /* RTG WritePixel (CGX) at half resolution: H.264 is converted straight
     * to a (w/2)x(h/2) RGB picture - a quarter of the colour conversion and
     * blit work, for RTG boards without a working P96 overlay (PiStorm).
     * Passes --rtg-half; see mrplay.c's rtg_half_active. */
    MR_DISPLAY_RTG_HALF,
    /* Video in a window on the Workbench screen (AGA, ECS or OCS), using
     * shared screen pens (ObtainBestPen) for the dither palette - see
     * amiga/display_aga_window.c. Native chipset, but no C2P, lace or 2x
     * options apply. --aga-window. */
    MR_DISPLAY_AGA_WINDOW,
    /* The same window at half the video's width and height: H.264/MPEG-2
     * dither straight to that size. --aga-window-half. */
    MR_DISPLAY_AGA_WINDOW_HALF,
    /* GUI Display "No Video": play only the soundtrack. Same as no_video
     * below (mrplay gets --no-video); as a display choice it sits where
     * people look for "how do I show this", and the frame/VQ controls grey
     * out under it. */
    MR_DISPLAY_NONE
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

/* "Skip after" range for --skip-trigger= (see mrplay.c's micro-rescue):
 * how far a Skip Frames session may fall behind before it sheds frames. */
#define MR_SKIP_TRIGGER_MIN_MS      200u
#define MR_SKIP_TRIGGER_MAX_MS     2000u
#define MR_SKIP_TRIGGER_DEFAULT_MS  700u

typedef enum {
    MR_H264_PERF_AUTO = 0,
    MR_H264_PERF_QUALITY,
    MR_H264_PERF_BALANCED,
    MR_H264_PERF_FAST,
    MR_H264_PERF_TURBO,
    MR_H264_PERF_TURBO_PLUS,
    /* Turbo plus datamosh-style dropping of late P/B pictures - see
     * core/mr_h264.h's mr_h264_set_drop_nonsync(). Appended so saved
     * settings keep their existing values. */
    MR_H264_PERF_SMOOSH
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
    MR_FAST_BUFFER_16MB,
    MR_FAST_BUFFER_32MB,
    MR_FAST_BUFFER_64MB
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
    /* pass --audio-mono: decode one channel instead of two and duplicate it
     * to both Paula speakers. Saves per-channel synthesis in MP3, MP2 and
     * AC-3; Helix AAC has no decoder-side mono synthesis mode. */
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
    /* GUI "Skip after" (--skip-trigger=): how many ms a "Skip Frames"
     * session may fall behind before it starts shedding frames, clamped to
     * MR_SKIP_TRIGGER_MIN_MS..MR_SKIP_TRIGGER_MAX_MS. No effect with
     * "All Frames" (throughput) or Smoosh, which never escalate. */
    unsigned skip_trigger_ms;
    /* --no-video: demux the file but decode and show no video at all, only
     * the audio - for machines too slow for the picture that just want to
     * listen (e.g. a YouTube talk). The GUIs set it through the Display
     * chooser's "No Video" row (MR_DISPLAY_NONE); see
     * mr_play_options_no_video(). */
    int no_video;
} mr_play_options;

/* The display modes that play through an RTG window rather than a native
 * Amiga screen (no C2P, lace or 2x options apply). */
int mr_display_is_rtg(mr_display_mode display);

/* The display modes that open their own native screen, where the C2P, lace,
 * 2x and Copper 2x options apply: everything except RTG and the Workbench
 * window modes. */
int mr_display_has_screen_options(mr_display_mode display);

/* True when the session plays audio only: no_video set directly (--no-video)
 * or the "No Video" display (MR_DISPLAY_NONE). */
int mr_play_options_no_video(const mr_play_options *options);

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
