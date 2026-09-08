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
    /* TurboGT keeps the P-frame reference chain while applying the strongest
     * practical libavc degradation policy and skipping B pictures. It now
     * resolves to the same policy as Turbo - see mr_h264_set_speed_mode(). */
    o->h264_performance = MR_H264_PERF_TURBO_GT;
    o->audio_rate = MR_AUDIO_RATE_NORMAL;
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
    case MR_DISPLAY_AGA_ECS32: return "ecs32";
    case MR_DISPLAY_AGA_ECS16: return "ecs16";
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
    default: return "standard";
    }
}

static int append_playback_flags(char *out, size_t cap,
                                 const mr_play_options *o, int explicit)
{
    char number[32];
    if (explicit) {
        if (!append_option(out, cap, "--display") ||
            !append_option(out, cap, display_name(o->display))) return 0;
        if (o->display != MR_DISPLAY_CGX && o->display != MR_DISPLAY_P96) {
            if (!append_option(out, cap, "--c2p") ||
                !append_option(out, cap, c2p_name(o->c2p)) ||
                !append_option(out, cap, o->laced ? "--laced" : "--no-laced") ||
                !append_option(out, cap, o->scale_2x ? "--scale-2x" :
                                                       "--no-scale-2x")) return 0;
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
        if (o->display == MR_DISPLAY_P96 && !append_option(out, cap, "--p96")) return 0;
        if (o->display != MR_DISPLAY_CGX && o->display != MR_DISPLAY_P96) {
            const char *flag = o->c2p == MR_C2P_AKIKO ? "--cd32" :
                               o->c2p == MR_C2P_KALMS ? "--kalms-c2p" :
                               o->c2p == MR_C2P_RIVA ? "--riva-c2p" :
                               o->c2p == MR_C2P_STANDARD ? "--wpa" : "--c2p";
            if (!append_option(out, cap, flag)) return 0;
            if (o->laced && !append_option(out, cap, "--lace")) return 0;
            if (o->scale_2x && !append_option(out, cap, "--2x")) return 0;
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
    if (o->h264_performance != MR_H264_PERF_AUTO) {
        const char *mode = o->h264_performance == MR_H264_PERF_QUALITY
                         ? "--h264-speed=quality" :
                           o->h264_performance == MR_H264_PERF_BALANCED
                         ? "--h264-speed=balanced" :
                           o->h264_performance == MR_H264_PERF_TURBO
                         ? "--h264-speed=turbo" :
                           o->h264_performance == MR_H264_PERF_TURBO_PLUS
                         ? "--h264-speed=turbo+" :
                           o->h264_performance == MR_H264_PERF_TURBO_GT
                         ? "--h264-speed=turbogt" : "--h264-speed=fast";
        if (!append_option(out, cap, mode)) return 0;
    }
    if (o->no_audio) {
        if (!append_option(out, cap, "--no-audio")) return 0;
    } else if (o->audio_rate == MR_AUDIO_RATE_LOW) {
        if (!append_option(out, cap, "--audio-rate=low")) return 0;
    }
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
    /* P96's direct bitmap-lock backend is fullscreen-only.  GUI callers
     * select P96 as a display mode but do not have a separate startup
     * fullscreen option, so enter fullscreen before display_open().  This
     * lets display_p96 open its private screen instead of rejecting a
     * windowed P96 launch and falling back to CGX/WritePixelArray. */
    if (o->display == MR_DISPLAY_P96 &&
        !append_option(out, cap, "--fullscreen")) return 0;
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
            else if (!strcmp(value, "ecs32")) o->display = MR_DISPLAY_AGA_ECS32;
            else if (!strcmp(value, "ecs16")) o->display = MR_DISPLAY_AGA_ECS16;
            else goto bad;
        } else if (!strcmp(arg, "--c2p")) {
            if (i + 1 >= argc) goto bad;
            value = argv[++i];
            if (!strcmp(value, "standard")) o->c2p = MR_C2P_STANDARD;
            else if (!strcmp(value, "akiko")) o->c2p = MR_C2P_AKIKO;
            else if (!strcmp(value, "kalms")) o->c2p = MR_C2P_KALMS;
            else if (!strcmp(value, "riva")) o->c2p = MR_C2P_RIVA;
            else if (!strcmp(value, "wpa")) o->c2p = MR_C2P_WPA;
            else goto bad;
        } else if (!strcmp(arg, "--laced")) o->laced = 1;
        else if (!strcmp(arg, "--no-laced")) o->laced = 0;
        else if (!strcmp(arg, "--scale-2x")) o->scale_2x = 1;
        else if (!strcmp(arg, "--no-scale-2x")) o->scale_2x = 0;
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
            else if (!strcmp(value, "turbogt") || !strcmp(value, "turbo-gt"))
                o->h264_performance = MR_H264_PERF_TURBO_GT;
            else goto bad;
        }
        else if (!strncmp(arg, "--audio-rate=", 13)) {
            value = arg + 13;
            if (!strcmp(value, "normal")) o->audio_rate = MR_AUDIO_RATE_NORMAL;
            else if (!strcmp(value, "low")) o->audio_rate = MR_AUDIO_RATE_LOW;
            else goto bad;
        }
        else if (!strcmp(arg, "--no-audio")) o->no_audio = 1;
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

/* Short description of the audio output rate policy for the status line. */
static const char *audio_policy_text(const mr_play_options *o)
{
    if (o->no_audio) return "off";
    return o->audio_rate == MR_AUDIO_RATE_LOW ? "Low" : "Normal";
}

void mr_play_options_summary(const mr_play_options *o, char *out, size_t cap)
{
    char hls[24];
    const char *h264, *audio;
    if (!out || !cap || !o) return;
    hls_policy_text(o, hls, sizeof hls);
    h264 = o->h264_performance == MR_H264_PERF_QUALITY ? "Quality" :
           o->h264_performance == MR_H264_PERF_BALANCED ? "Balanced" :
           o->h264_performance == MR_H264_PERF_FAST ? "Fast" :
           o->h264_performance == MR_H264_PERF_TURBO ? "Turbo" :
           o->h264_performance == MR_H264_PERF_TURBO_PLUS ? "Turbo+" :
           o->h264_performance == MR_H264_PERF_TURBO_GT ? "TurboGT" : "Auto";
    audio = audio_policy_text(o);
    if (o->display == MR_DISPLAY_CGX || o->display == MR_DISPLAY_P96)
        snprintf(out, cap, "Playback: RTG (%s) / %s / H264 %s / Audio %s%s",
                 o->display == MR_DISPLAY_P96 ? "P96" : "WritePixel",
                 hls, h264, audio, o->live_resync ? " / Live-resync" : "");
    else
        snprintf(out, cap,
                 "Playback: %s / %s / Lace %s / 2x %s / %s / H264 %s / Audio %s%s",
                 o->display == MR_DISPLAY_HAM6 ? "HAM6" :
                 o->display == MR_DISPLAY_HAM8 ? "HAM8" :
                 o->display == MR_DISPLAY_AGA_ECS32 ? "ECS (32)" :
                 o->display == MR_DISPLAY_AGA_ECS16 ? "ECS (16)" : "Native planar",
                 c2p_name(o->c2p), o->laced ? "on" : "off",
                 o->scale_2x ? "on" : "off", hls, h264, audio,
                 o->live_resync ? " / Live-resync" : "");
}
