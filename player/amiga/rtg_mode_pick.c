/*
 * MintVID - shared CyberGraphX "native-or-downscale" private-screen mode
 * picker. See rtg_mode_pick.h for the policy this implements and why -
 * moved here, verbatim in algorithm, from what used to be display_p96.c's
 * own static p96_mode_for_source_at_depth() so display_cgx.c's private
 * screen can use the identical policy instead of always matching the
 * public screen's resolution (see display_cgx.c's cgx_open_private_screen()
 * for the real-hardware cost that gap had).
 */
#include "rtg_mode_pick.h"

#include <graphics/displayinfo.h>
#include <cybergraphx/cybergraphics.h>

#include <proto/cybergraphics.h>

ULONG mr_rtg_best_mode_for_source(int source_w, int source_h, ULONG depth,
                                  ULONG *depth_out)
{
    static const UWORD common_modes[][2] = {
        { 320, 240 }, { 640, 400 }, { 640, 480 }, { 720, 480 },
        { 800, 600 }, { 1024, 768 }, { 1152, 864 },
        { 1280, 720 }, { 1280, 1024 }, { 1600, 900 },
        { 1600, 1200 }, { 1920, 1080 }
    };
    ULONG best_fit = (ULONG)INVALID_ID;
    ULONG best_any = (ULONG)INVALID_ID;
    ULONG best_fit_area = ~0UL;
    ULONG best_fit_delta = ~0UL;
    ULONG best_fit_aspect = ~0UL;
    ULONG best_any_delta = ~0UL;
    ULONG best_any_aspect = ~0UL;
    ULONG best_fit_depth = 0;
    ULONG best_any_depth = 0;
    unsigned i;

    for (i = 0; i <= sizeof common_modes / sizeof common_modes[0]; i++) {
        int request_w = i ? (int)common_modes[i - 1][0] : source_w;
        int request_h = i ? (int)common_modes[i - 1][1] : source_h;
        ULONG modeid, mode_w, mode_h, mode_depth, delta, aspect_error;
        ULONG aspect_w, aspect_h;

        modeid = BestCModeIDTags(
            CYBRBIDTG_NominalWidth, (ULONG)request_w,
            CYBRBIDTG_NominalHeight, (ULONG)request_h,
            CYBRBIDTG_Depth, depth,
            TAG_END);
        if (modeid == (ULONG)INVALID_ID || !IsCyberModeID(modeid))
            continue;

        mode_w = GetCyberIDAttr(CYBRIDATTR_WIDTH, modeid);
        mode_h = GetCyberIDAttr(CYBRIDATTR_HEIGHT, modeid);
        mode_depth = GetCyberIDAttr(CYBRIDATTR_DEPTH, modeid);
        if (!mode_w || !mode_h || mode_w == ~0UL || mode_h == ~0UL ||
            mode_depth != depth)
            continue;

        delta = (ULONG)((int)mode_w >= source_w ? (int)mode_w - source_w
                                                : source_w - (int)mode_w) +
                (ULONG)((int)mode_h >= source_h ? (int)mode_h - source_h
                                                : source_h - (int)mode_h);

        /*
         * Prefer a screen whose shape matches the video before considering
         * spare area. The old area-first score chose 1024x768 for 854x480
         * even when 1280x720 existed, putting a 16:9 video on a 4:3 scanout.
         * Some P96/monitor setups expand the whole private mode to the panel,
         * so the scanout shape must agree with the video's display aspect.
         */
        aspect_w = mode_w * (ULONG)source_h;
        aspect_h = mode_h * (ULONG)source_w;
        aspect_error = aspect_w >= aspect_h ? aspect_w - aspect_h
                                            : aspect_h - aspect_w;

        if ((int)mode_w >= source_w && (int)mode_h >= source_h) {
            ULONG source_area = (ULONG)source_w * (ULONG)source_h;
            ULONG mode_area = mode_w * mode_h;
            ULONG excess = mode_area >= source_area ? mode_area - source_area
                                                     : 0;
            if (best_fit == (ULONG)INVALID_ID ||
                aspect_error < best_fit_aspect ||
                (aspect_error == best_fit_aspect &&
                 (excess < best_fit_area ||
                  (excess == best_fit_area && delta < best_fit_delta)))) {
                best_fit = modeid;
                best_fit_area = excess;
                best_fit_delta = delta;
                best_fit_aspect = aspect_error;
                best_fit_depth = mode_depth;
            }
        } else if (best_any == (ULONG)INVALID_ID ||
                   aspect_error < best_any_aspect ||
                   (aspect_error == best_any_aspect &&
                    delta < best_any_delta)) {
            best_any = modeid;
            best_any_delta = delta;
            best_any_aspect = aspect_error;
            best_any_depth = mode_depth;
        }
    }

    if (best_fit != (ULONG)INVALID_ID) {
        if (depth_out) *depth_out = best_fit_depth;
        return best_fit;
    }
    if (depth_out) *depth_out = best_any_depth;
    return best_any;
}
