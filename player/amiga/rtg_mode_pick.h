/*
 * MintVID - shared CyberGraphX "native-or-downscale" private-screen mode
 * picker, used by both display_p96.c and display_cgx.c.
 */
#ifndef MR_RTG_MODE_PICK_H
#define MR_RTG_MODE_PICK_H

#include <exec/types.h>

/*
 * Pick an available CyberGraphX mode at the given depth which can contain
 * a source_w x source_h picture at 1:1, preferring a scanout with the same
 * aspect ratio and then the least spare area - falling back to the closest
 * smaller mode (which the caller's own scale path must then downscale into)
 * only when nothing actually fits. A private screen sized to the video is
 * far cheaper to display into than one forced to match an unrelated public
 * screen's resolution: a real Voodoo3/P96 capture (RAM:MintVID.log, a
 * 540x360 clip against a 1024x768 Workbench) measured a plain WritePixelArray
 * fullscreen path paying ~289 ms/frame of software upscale alone once forced
 * to fill 1024x768, on top of the blit itself - this policy exists to avoid
 * that entirely whenever the source already fits (or nearly fits) a smaller
 * real mode.
 *
 * The first request is the exact source size. The common-mode probes make
 * the choice deterministic on boards/drivers whose BestCModeIDTags() rounds
 * an unusual video size (for example 854x480) to a 4:3 mode: a containing
 * 1280x720 mode is preferred when available.
 *
 * Only ever asks for `depth` exactly - the caller is responsible for trying
 * several depths in its own preference order and verifying the resulting
 * bitmap's real pixel format (a mode existing at a given depth does not by
 * itself guarantee an RGBFB/PIXFMT layout the caller's direct-write path
 * knows how to handle).
 *
 * Returns INVALID_ID (graphics/displayinfo.h) if nothing at this depth is
 * usable; *depth_out is only written on success.
 */
ULONG mr_rtg_best_mode_for_source(int source_w, int source_h, ULONG depth,
                                  ULONG *depth_out);

#endif /* MR_RTG_MODE_PICK_H */
