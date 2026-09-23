#include "mr_play_options.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int mr_path_is_audio_only(const char *path)
{
    static const char *const audio_extensions[] = {
        "mp3", "mp2", "aac", "m4a", "flac", "ogg", "oga", "opus",
        "wav", "wma", "8svx", "svx", "aif", "aiff", "ac3", "mka"
    };
    const char *extension = NULL, *end, *p;
    size_t extension_length, i, j;

    if (!path || !*path) return 0;
    end = path + strlen(path);
    for (p = path; p < end && *p != '?' && *p != '#'; p++) {
        if (*p == '/' || *p == ':' || *p == '\\')
            extension = NULL;
        else if (*p == '.')
            extension = p + 1;
    }
    end = p;
    if (!extension || extension >= end) return 0;
    extension_length = (size_t)(end - extension);
    for (i = 0; i < sizeof(audio_extensions) / sizeof(audio_extensions[0]); i++) {
        if (strlen(audio_extensions[i]) != extension_length) continue;
        for (j = 0; j < extension_length; j++)
            if (tolower((unsigned char)extension[j]) !=
                tolower((unsigned char)audio_extensions[i][j]))
                break;
        if (j == extension_length) return 1;
    }
    return 0;
}

void mr_play_options_default(mr_play_options *o)
{
    if (!o) return;
    memset(o, 0, sizeof(*o));
    o->display = MR_DISPLAY_AGA;
    /* Release builds always include the CPU-matched Kalms converters. Their
     * runtime geometry/layout checks fail closed onto graphics.library, so
     * use the faster path by default without sacrificing compatibility. */
    o->c2p = MR_C2P_KALMS;
    /* Default to the smallest HLS rendition: it is the one most likely to play
     * on any machine, and picking a bigger one automatically can break a channel
     * that worked (a 720p variant may be a codec we can't decode, or just too
     * heavy). Higher quality is opt-in via --hls-max-height / clearing --hls-low.
     * The picker still selects the *best* variant within the ceiling once low is
     * off, so a caller that raises the ceiling gets the best stream that fits. */
    o->hls_low = 1;
    o->hls_max_width = 640;
    /* On by default for GUI-launched playback (IPTV streams are always live);
     * a direct "mrplay <url>" invocation keeps its own conservative default of
     * off. Disable with --no-live-resync. */
    o->live_resync = 1;
    /* Turbo keeps the P-frame reference chain while applying the strongest
     * practical libavc degradation policy and skipping B pictures - see
     * mr_h264_set_speed_mode(). */
    o->h264_performance = MR_H264_PERF_TURBO;
    o->audio_rate = MR_AUDIO_RATE_NORMAL;
    o->fast_buffer = MR_FAST_BUFFER_AUTO;
    /* On by default for every GUI-launched session (local file or network
     * alike) - see the real-hardware regression this fixed, in CLAUDE.md's
     * "Live HLS playback stall notes". A direct "mrplay <url>" invocation
     * with no options struct at all keeps mrplay.c's own conservative
     * per-source default (on for a network/HLS source, off for a local
     * file) instead, since neither --throughput nor --no-throughput is
     * passed unless something built the command line through this file. */
    o->throughput = 1;
    o->skip_trigger_ms = MR_SKIP_TRIGGER_DEFAULT_MS;
}

int mr_display_is_rtg(mr_display_mode display)
{
    return display == MR_DISPLAY_CGX || display == MR_DISPLAY_P96 ||
           display == MR_DISPLAY_P96_FULLSCREEN ||
           display == MR_DISPLAY_RTG_HALF;
}

static unsigned clamp_skip_trigger(unsigned ms)
{
    if (ms < MR_SKIP_TRIGGER_MIN_MS) return MR_SKIP_TRIGGER_MIN_MS;
    if (ms > MR_SKIP_TRIGGER_MAX_MS) return MR_SKIP_TRIGGER_MAX_MS;
    return ms;
}

static int append_text(char *out, size_t cap, const char *text)
{
    size_t used = strlen(out), length = strlen(text);
    if (used >= cap || length >= cap - used) return 0;
    memcpy(out + used, text, length + 1);
    return 1;
}

static int append_quoted(char *out, size_t cap, const char *value)
{
    size_t used = strlen(out), i;
    if (used && !append_text(out, cap, " ")) return 0;
    if (!append_text(out, cap, "\"")) return 0;
    for (i = 0; value && value[i]; i++) {
        char one[3];
        if (value[i] == '\r' || value[i] == '\n') return 0;
        one[0] = 0;
        if (value[i] == '"' || value[i] == '*') {
            one[0] = '*'; one[1] = value[i]; one[2] = 0;
        } else {
            one[0] = value[i]; one[1] = 0;
        }
        if (!append_text(out, cap, one)) return 0;
    }
    return append_text(out, cap, "\"");
}

static int append_option(char *out, size_t cap, const char *option)
{
    return (!out[0] || append_text(out, cap, " ")) &&
           append_text(out, cap, option);
}

static const char *display_name(mr_display_mode display)
{
    switch (display) {
    case MR_DISPLAY_HAM6: return "ham6";
    case MR_DISPLAY_HAM8: return "ham8";
    case MR_DISPLAY_CGX: return "cgx";
    case MR_DISPLAY_P96: return "p96";
    case MR_DISPLAY_P96_FULLSCREEN: return "p96-fullscreen";
    case MR_DISPLAY_RTG_HALF: return "rtg-half";
    case MR_DISPLAY_AGA_ECS32: return "ecs32";
    case MR_DISPLAY_AGA_ECS16: return "ecs16";
    case MR_DISPLAY_AGA_EHB: return "ehb";
    default: return "aga";
    }
}

static const char *c2p_name(mr_c2p_mode c2p)
{
    switch (c2p) {
    case MR_C2P_AKIKO: return "akiko";
    case MR_C2P_KALMS: return "kalms";
    case MR_C2P_RIVA: return "riva";
    case MR_C2P_WPA: return "wpa";
    case MR_C2P_DIRECT: return "direct";
    default: return "standard";
    }
}

/* The GUIs' "VQ:" chooser (still backed by h264_performance/MR_H264_PERF_* -
 * H.264's own speed dial, see core/mr_h264.h) doubles as a generic "does
 * this session want fast decode over full quality" preference, reused for
 * any other codec that has its own speed/quality lever - DV's
 * mr_dv_set_speed_mode() (core/mr_dv.h) today, see CLAUDE.md's "DV decode
 * speed" notes. Quality (the one mode a user picks specifically for best
 * output) maps to that codec's own quality mode; every other choice - Auto
 * included, since Auto's own H.264 resolution already defaults to Turbo,
 * see mrplay.c's effective_h264_speed() - maps to fast. Safe to apply
 * unconditionally regardless of what codec a given file turns out to be:
 * mr_dv_set_speed_mode()/apply_h264_speed() in amiga/mrplay.c both no-op
 * for a mismatched codec, the same way --h264-speed= already does. */
static int mr_video_quality_prefers_fast(mr_h264_performance mode)
{
    return mode != MR_H264_PERF_QUALITY;
}

static int append_playback_flags(char *out, size_t cap,
                                 const mr_play_options *o, int explicit)
{
    char number[32];
    const char *fast_buffer;
    if (explicit) {
        if (!append_option(out, cap, "--display") ||
            !append_option(out, cap, display_name(o->display))) return 0;
        if (!mr_display_is_rtg(o->display)) {
            if (!append_option(out, cap, "--c2p") ||
                !append_option(out, cap, c2p_name(o->c2p)) ||
                !append_option(out, cap, o->laced ? "--laced" : "--no-laced") ||
                !append_option(out, cap, o->scale_2x ? "--scale-2x" :
                                                       "--no-scale-2x") ||
                !append_option(out, cap, o->copper_vdouble ?
                               "--copper-vdouble" : "--no-copper-vdouble"))
                return 0;
        }
    } else {
        if (o->display == MR_DISPLAY_AGA && !append_option(out, cap, "--aga")) return 0;
        if (o->display == MR_DISPLAY_HAM6 &&
            (!append_option(out, cap, "--aga") || !append_option(out, cap, "--ham6"))) return 0;
        if (o->display == MR_DISPLAY_HAM8 &&
            (!append_option(out, cap, "--aga") || !append_option(out, cap, "--ham"))) return 0;
        if (o->display == MR_DISPLAY_AGA_ECS32 &&
            (!append_option(out, cap, "--aga") || !append_option(out, cap, "--ecs32"))) return 0;
        if (o->display == MR_DISPLAY_AGA_ECS16 &&
            (!append_option(out, cap, "--aga") || !append_option(out, cap, "--ecs-fast"))) return 0;
        if (o->display == MR_DISPLAY_AGA_EHB &&
            (!append_option(out, cap, "--aga") || !append_option(out, cap, "--ehb"))) return 0;
        if ((o->display == MR_DISPLAY_P96 ||
             o->display == MR_DISPLAY_P96_FULLSCREEN) &&
            !append_option(out, cap, "--p96")) return 0;
        if (o->display == MR_DISPLAY_P96_FULLSCREEN &&
            !append_option(out, cap, "--fullscreen")) return 0;
        if (o->display == MR_DISPLAY_RTG_HALF &&
            !append_option(out, cap, "--rtg-half")) return 0;
        if (!mr_display_is_rtg(o->display)) {
            const char *flag = o->c2p == MR_C2P_AKIKO ? "--cd32" :
                               o->c2p == MR_C2P_KALMS ? "--kalms-c2p" :
                               o->c2p == MR_C2P_RIVA ? "--riva-c2p" :
                               o->c2p == MR_C2P_DIRECT ? "--direct-c2p" :
                               o->c2p == MR_C2P_STANDARD ? "--wpa" : "--c2p";
            if (!append_option(out, cap, flag)) return 0;
            if (o->laced && !append_option(out, cap, "--lace")) return 0;
            if (o->scale_2x && !append_option(out, cap, "--2x")) return 0;
            if (o->scale_2x && o->copper_vdouble &&
                !append_option(out, cap, "--copper-vdouble")) return 0;
        }
    }
    if (o->hls_low && !append_option(out, cap, "--hls-low")) return 0;
    if (o->hls_max_width) {
        snprintf(number, sizeof(number), "--hls-max-width=%u", o->hls_max_width);
        if (!append_option(out, cap, number)) return 0;
    }
    if (o->hls_max_height) {
        snprintf(number, sizeof(number), "--hls-max-height=%u", o->hls_max_height);
        if (!append_option(out, cap, number)) return 0;
    }
    if (o->hls_max_fps) {
        snprintf(number, sizeof(number), "--hls-max-fps=%u", o->hls_max_fps);
        if (!append_option(out, cap, number)) return 0;
    }
    if (o->live_resync && !append_option(out, cap, "--live-resync")) return 0;
    fast_buffer = o->fast_buffer == MR_FAST_BUFFER_OFF ? "off" :
                  o->fast_buffer == MR_FAST_BUFFER_4MB ? "4" :
                  o->fast_buffer == MR_FAST_BUFFER_8MB ? "8" :
                  o->fast_buffer == MR_FAST_BUFFER_16MB ? "16" :
                  o->fast_buffer == MR_FAST_BUFFER_32MB ? "32" :
                  o->fast_buffer == MR_FAST_BUFFER_64MB ? "64" : "auto";
    snprintf(number, sizeof(number), "--fast-buffer=%s", fast_buffer);
    if (!append_option(out, cap, number)) return 0;
    if (o->h264_performance != MR_H264_PERF_AUTO) {
        const char *mode = o->h264_performance == MR_H264_PERF_QUALITY
                         ? "--h264-speed=quality" :
                           o->h264_performance == MR_H264_PERF_BALANCED
                         ? "--h264-speed=balanced" :
                           o->h264_performance == MR_H264_PERF_TURBO
                         ? "--h264-speed=turbo" :
                           o->h264_performance == MR_H264_PERF_TURBO_PLUS
                         ? "--h264-speed=turbo+" :
                           o->h264_performance == MR_H264_PERF_SMOOSH
                         ? "--h264-speed=smoosh" : "--h264-speed=fast";
        if (!append_option(out, cap, mode)) return 0;
    }
    /* Always explicit, like --throughput below: a GUI-launched session's
     * VQ choice should override mrplay.c's own MR_DV_SPEED_QUALITY default
     * in both directions, which a conditionally-omitted flag can't do -
     * see mr_video_quality_prefers_fast()'s own header. */
    if (!append_option(out, cap, mr_video_quality_prefers_fast(o->h264_performance)
                                 ? "--dv-speed=fast" : "--dv-speed=quality"))
        return 0;
    /* Same VQ value, same "always explicit" reasoning, for MPEG-1/2's own
     * B-frame skip (mr_mpeg2_set_speed_mode(), core/mr_mpeg2.h) - see
     * CLAUDE.md's "MPEG-1/2 B-frame skip" notes. */
    if (!append_option(out, cap, mr_video_quality_prefers_fast(o->h264_performance)
                                 ? "--mpeg2-speed=fast" : "--mpeg2-speed=quality"))
        return 0;
    if (o->no_audio) {
        if (!append_option(out, cap, "--no-audio")) return 0;
    } else {
        if (o->audio_rate == MR_AUDIO_RATE_LOW &&
            !append_option(out, cap, "--audio-rate=low")) return 0;
        if (o->mono_audio && !append_option(out, cap, "--audio-mono")) return 0;
    }
    /* Always explicit, like --fast-buffer= above: a GUI-launched session's
     * choice (the "Video: All Frames / Skip Frames" control) should always
     * override mrplay.c's own per-source default, in both directions - not
     * just when forcing throughput mode on. */
    if (!append_option(out, cap,
                       o->throughput ? "--throughput" : "--no-throughput"))
        return 0;
    /* Always explicit too, so the GUI's "Skip after" choice reaches mrplay
     * (and survives an IPTV/YouTube browser's re-parse) unchanged. */
    snprintf(number, sizeof(number), "--skip-trigger=%u",
             clamp_skip_trigger(o->skip_trigger_ms));
    if (!append_option(out, cap, number)) return 0;
    if (o->no_video && !append_option(out, cap, "--no-video")) return 0;
    return 1;
}

int mr_build_player_arguments(char *out, size_t cap,
                              const mr_play_options *o, const char *url,
                              const char *ua, const char *referer)
{
    mr_play_options defaults;
    if (!out || !cap || !url || !*url) return 0;
    if (!o) { mr_play_options_default(&defaults); o = &defaults; }
    out[0] = 0;
    if (!append_playback_flags(out, cap, o, 0)) return 0;
    /* P96 mode used to force --fullscreen here, back when "RTG (P96)" meant
     * only the older direct screen-bitmap-lock backend, which refuses to
     * open at all unless already fullscreen (unclipped writes would corrupt
     * sibling windows - see amiga/display_p96.c's file header). Now that
     * display_open() (amiga/display.c) tries the PIP overlay backend first
     * for P96 - see amiga/display_p96pip.c's file header - that hazard is
     * gone: the overlay backend opens perfectly well windowed, with no
     * corruption risk, so P96 no longer needs to start fullscreen at all.
     * The real desired flow: P96 opens as a normal window (still using
     * hardware overlay if the board grants it), and pressing F is what
     * takes it to fullscreen - amiga/display_p96pip.c's own
     * p96pip_toggle_fullscreen() tries real hardware acceleration
     * (PIPT_VideoWindow) again on every toggle, falling back to software
     * compositing (PIPT_MemoryWindow) only if the board refuses, exactly
     * mirroring the windowed-open behaviour. Forcing --fullscreen here
     * would skip straight past that windowed-then-F flow and (for a board
     * where the PIP backend can't open at all) risk falling through to the
     * older direct-lock backend instead of the CGX/WritePixelArray
     * fallback a windowed session would otherwise get. */
    if (ua && *ua &&
        (!append_option(out, cap, "--user-agent") ||
         !append_quoted(out, cap, ua))) return 0;
    if (referer && *referer &&
        (!append_option(out, cap, "--referer") ||
         !append_quoted(out, cap, referer))) return 0;
    if (!append_quoted(out, cap, url) || !append_text(out, cap, "\n")) return 0;
    return 1;
}

int mr_build_iptv_arguments(char *out, size_t cap, const mr_play_options *o)
{
    mr_play_options defaults;
    if (!out || !cap) return 0;
    if (!o) { mr_play_options_default(&defaults); o = &defaults; }
    out[0] = 0;
    return append_playback_flags(out, cap, o, 1) &&
           append_text(out, cap, "\n");
}

static int parse_uint(const char *text, unsigned *value)
{
    char *end;
    unsigned long parsed;
    if (!text || !*text) return 0;
    parsed = strtoul(text, &end, 10);
    if (*end || parsed > 65535) return 0;
    *value = (unsigned)parsed;
    return 1;
}

int mr_play_options_parse(mr_play_options *o, int argc, char **argv,
                          char *error, size_t error_size)
{
    int i;
    if (!o) return 0;
    for (i = 1; i < argc; i++) {
        const char *arg = argv[i], *value;
        if (!strcmp(arg, "--display")) {
            if (i + 1 >= argc) goto bad;
            value = argv[++i];
            if (!strcmp(value, "aga")) o->display = MR_DISPLAY_AGA;
            else if (!strcmp(value, "ham6")) o->display = MR_DISPLAY_HAM6;
            else if (!strcmp(value, "ham8")) o->display = MR_DISPLAY_HAM8;
            else if (!strcmp(value, "cgx") || !strcmp(value, "rtg")) o->display = MR_DISPLAY_CGX;
            else if (!strcmp(value, "p96")) o->display = MR_DISPLAY_P96;
            else if (!strcmp(value, "p96-fullscreen")) o->display = MR_DISPLAY_P96_FULLSCREEN;
            else if (!strcmp(value, "rtg-half")) o->display = MR_DISPLAY_RTG_HALF;
            else if (!strcmp(value, "ecs32")) o->display = MR_DISPLAY_AGA_ECS32;
            else if (!strcmp(value, "ecs16")) o->display = MR_DISPLAY_AGA_ECS16;
            else if (!strcmp(value, "ehb")) o->display = MR_DISPLAY_AGA_EHB;
            else goto bad;
        } else if (!strcmp(arg, "--c2p")) {
            if (i + 1 >= argc) goto bad;
            value = argv[++i];
            if (!strcmp(value, "standard")) o->c2p = MR_C2P_STANDARD;
            else if (!strcmp(value, "akiko")) o->c2p = MR_C2P_AKIKO;
            else if (!strcmp(value, "kalms")) o->c2p = MR_C2P_KALMS;
            else if (!strcmp(value, "riva")) o->c2p = MR_C2P_RIVA;
            else if (!strcmp(value, "wpa")) o->c2p = MR_C2P_WPA;
            else if (!strcmp(value, "direct")) o->c2p = MR_C2P_DIRECT;
            else goto bad;
        } else if (!strcmp(arg, "--laced")) o->laced = 1;
        else if (!strcmp(arg, "--no-laced")) o->laced = 0;
        else if (!strcmp(arg, "--scale-2x")) o->scale_2x = 1;
        else if (!strcmp(arg, "--no-scale-2x")) o->scale_2x = 0;
        else if (!strcmp(arg, "--copper-vdouble")) o->copper_vdouble = 1;
        else if (!strcmp(arg, "--no-copper-vdouble")) o->copper_vdouble = 0;
        else if (!strcmp(arg, "--hls-low")) o->hls_low = 1;
        else if (!strcmp(arg, "--live-resync")) o->live_resync = 1;
        else if (!strcmp(arg, "--no-live-resync")) o->live_resync = 0;
        else if (!strncmp(arg, "--h264-speed=", 13)) {
            value = arg + 13;
            if (!strcmp(value, "auto")) o->h264_performance = MR_H264_PERF_AUTO;
            else if (!strcmp(value, "quality")) o->h264_performance = MR_H264_PERF_QUALITY;
            else if (!strcmp(value, "balanced")) o->h264_performance = MR_H264_PERF_BALANCED;
            else if (!strcmp(value, "fast")) o->h264_performance = MR_H264_PERF_FAST;
            else if (!strcmp(value, "turbo")) o->h264_performance = MR_H264_PERF_TURBO;
            else if (!strcmp(value, "turbo+") || !strcmp(value, "turbo-plus"))
                o->h264_performance = MR_H264_PERF_TURBO_PLUS;
            else if (!strcmp(value, "smoosh"))
                o->h264_performance = MR_H264_PERF_SMOOSH;
            /* TurboGT is a retired name, kept accepted here for scripts/
             * saved settings from before it collapsed onto Turbo's own
             * policy - see CLAUDE.md's H.264 TurboGT retirement notes. */
            else if (!strcmp(value, "turbogt") || !strcmp(value, "turbo-gt"))
                o->h264_performance = MR_H264_PERF_TURBO;
            else goto bad;
        }
        /* --dv-speed= is always emitted by append_playback_flags() (see
         * mr_video_quality_prefers_fast()'s own header) but has no
         * separate field of its own - it's fully derived from
         * h264_performance/VQ, so it's accepted here and simply
         * discarded: whichever VQ choice produced it is already captured
         * by the --h264-speed= flag parsed above in the same argv, except
         * on the one path where h264_performance is Auto (--h264-speed=
         * itself omitted, since Auto already resolves to Turbo at runtime
         * anyway - see mrplay.c's effective_h264_speed()), where the
         * struct's own default is functionally equivalent. Recognizing
         * but ignoring it here (rather than leaving it unrecognized) is
         * what actually matters: an unrecognized flag falls through to
         * "invalid playback option" below and refuses to parse an
         * otherwise-valid inherited argv at all - exactly the iptvgui/
         * ytgui launch failure this fixes. */
        else if (!strncmp(arg, "--dv-speed=", 11)) {
            value = arg + 11;
            if (strcmp(value, "fast") && strcmp(value, "quality")) goto bad;
        }
        /* --mpeg2-speed= is always emitted by append_playback_flags() too
         * (same mr_video_quality_prefers_fast() derivation as --dv-speed=
         * above - see CLAUDE.md's "MPEG-1/2 B-frame skip" notes), and has
         * no separate field here for the identical reason: recognized and
         * discarded, not acted on, so an inherited iptvgui/ytgui launch
         * argv carrying it still parses instead of hitting "invalid
         * playback option" below. */
        else if (!strncmp(arg, "--mpeg2-speed=", 14)) {
            value = arg + 14;
            if (strcmp(value, "fast") && strcmp(value, "quality")) goto bad;
        }
        else if (!strncmp(arg, "--audio-rate=", 13)) {
            value = arg + 13;
            if (!strcmp(value, "normal")) o->audio_rate = MR_AUDIO_RATE_NORMAL;
            else if (!strcmp(value, "low")) o->audio_rate = MR_AUDIO_RATE_LOW;
            else goto bad;
        }
        else if (!strcmp(arg, "--no-audio")) o->no_audio = 1;
        else if (!strcmp(arg, "--no-video")) o->no_video = 1;
        else if (!strcmp(arg, "--audio-mono")) o->mono_audio = 1;
        else if (!strcmp(arg, "--audio-stereo")) o->mono_audio = 0;
        else if (!strcmp(arg, "--throughput")) o->throughput = 1;
        else if (!strcmp(arg, "--no-throughput")) o->throughput = 0;
        else if (!strncmp(arg, "--skip-trigger=", 15)) {
            unsigned ms;
            if (!parse_uint(arg + 15, &ms) || ms < MR_SKIP_TRIGGER_MIN_MS ||
                ms > MR_SKIP_TRIGGER_MAX_MS) goto bad;
            o->skip_trigger_ms = ms;
        }
        else if (!strncmp(arg, "--fast-buffer=", 14)) {
            value = arg + 14;
            if (!strcmp(value, "auto")) o->fast_buffer = MR_FAST_BUFFER_AUTO;
            else if (!strcmp(value, "off")) o->fast_buffer = MR_FAST_BUFFER_OFF;
            else if (!strcmp(value, "4")) o->fast_buffer = MR_FAST_BUFFER_4MB;
            else if (!strcmp(value, "8")) o->fast_buffer = MR_FAST_BUFFER_8MB;
            else if (!strcmp(value, "16")) o->fast_buffer = MR_FAST_BUFFER_16MB;
            else if (!strcmp(value, "32")) o->fast_buffer = MR_FAST_BUFFER_32MB;
            else if (!strcmp(value, "64")) o->fast_buffer = MR_FAST_BUFFER_64MB;
            else goto bad;
        }
        else if (!strncmp(arg, "--hls-max-width=", 16)) {
            if (!parse_uint(arg + 16, &o->hls_max_width)) goto bad;
        } else if (!strncmp(arg, "--hls-max-height=", 17)) {
            if (!parse_uint(arg + 17, &o->hls_max_height)) goto bad;
        } else if (!strncmp(arg, "--hls-max-fps=", 14)) {
            if (!parse_uint(arg + 14, &o->hls_max_fps)) goto bad;
        } else goto bad;
    }
    return 1;
bad:
    if (error && error_size) snprintf(error, error_size, "invalid playback option near %s", argv[i]);
    return 0;
}

/* Short description of the current HLS rendition policy for the status line. */
static void hls_policy_text(const mr_play_options *o, char *out, size_t cap)
{
    if (o->hls_low)
        snprintf(out, cap, "HLS low");
    else if (o->hls_max_height)
        snprintf(out, cap, "HLS <=%up", o->hls_max_height);
    else
        snprintf(out, cap, "HLS best");
}

/* Short description of the audio output policy for the status line. */
static const char *audio_policy_text(const mr_play_options *o)
{
    if (o->no_audio) return "off";
    if (o->audio_rate == MR_AUDIO_RATE_LOW)
        return o->mono_audio ? "Low mono" : "Low";
    return o->mono_audio ? "Mono" : "Normal";
}

static const char *fast_buffer_text(const mr_play_options *o)
{
    switch (o->fast_buffer) {
    case MR_FAST_BUFFER_OFF: return "off";
    case MR_FAST_BUFFER_4MB: return "4 MB";
    case MR_FAST_BUFFER_8MB: return "8 MB";
    case MR_FAST_BUFFER_16MB: return "16 MB";
    case MR_FAST_BUFFER_32MB: return "32 MB";
    case MR_FAST_BUFFER_64MB: return "64 MB";
    default: return "Auto";
    }
}

void mr_play_options_summary(const mr_play_options *o, char *out, size_t cap)
{
    char hls[24], video[32];
    const char *h264, *audio;
    if (!out || !cap || !o) return;
    hls_policy_text(o, hls, sizeof hls);
    h264 = o->h264_performance == MR_H264_PERF_QUALITY ? "Quality" :
           o->h264_performance == MR_H264_PERF_BALANCED ? "Balanced" :
           o->h264_performance == MR_H264_PERF_FAST ? "Fast" :
           o->h264_performance == MR_H264_PERF_TURBO ? "Turbo" :
           o->h264_performance == MR_H264_PERF_TURBO_PLUS ? "Turbo+" :
           o->h264_performance == MR_H264_PERF_SMOOSH ? "Smoosh" : "Auto";
    audio = audio_policy_text(o);
    if (o->no_video)
        snprintf(video, sizeof video, "Off (audio only)");
    else if (o->throughput)
        snprintf(video, sizeof video, "All Frames");
    else {
        unsigned ms = clamp_skip_trigger(o->skip_trigger_ms);
        snprintf(video, sizeof video, "Skip Frames after %u.%us",
                 ms / 1000u, (ms % 1000u) / 100u);
    }
    if (mr_display_is_rtg(o->display))
        snprintf(out, cap, "Playback: RTG (%s) / %s / H264 %s / Audio %s / Fast buffer %s%s / Video %s",
                 o->display == MR_DISPLAY_P96 ? "P96" :
                 o->display == MR_DISPLAY_P96_FULLSCREEN ? "P96 Fullscreen" :
                 o->display == MR_DISPLAY_RTG_HALF ? "Half" :
                 "WritePixel",
                 hls, h264, audio, fast_buffer_text(o),
                 o->live_resync ? " / Live-resync" : "", video);
    else
        snprintf(out, cap,
                 "Playback: %s / %s / Lace %s / 2x %s%s / %s / H264 %s / Audio %s / Fast buffer %s%s / Video %s",
                 o->display == MR_DISPLAY_HAM6 ? "HAM6" :
                 o->display == MR_DISPLAY_HAM8 ? "HAM8" :
                 o->display == MR_DISPLAY_AGA_ECS32 ? "ECS (32)" :
                 o->display == MR_DISPLAY_AGA_ECS16 ? "ECS (16)" :
                 o->display == MR_DISPLAY_AGA_EHB ? "ECS (EHB)" : "Native planar",
                 c2p_name(o->c2p), o->laced ? "on" : "off",
                 o->scale_2x ? "on" : "off",
                 o->scale_2x && o->copper_vdouble ? " (copper)" : "",
                 hls, h264, audio,
                 fast_buffer_text(o),
                 o->live_resync ? " / Live-resync" : "", video);
}
