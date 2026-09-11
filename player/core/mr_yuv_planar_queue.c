/*
 * MintVID - AGA YUV->planar queue: process-wide state for the direct-planar
 * C2P path (mr_c2p_mode MR_C2P_DIRECT).
 *
 * The actual YUV->plane-major conversion lives entirely in
 * mr_yuv_dither_planar_direct_m68k.S's mr_yuv420_dither8_planar_direct_m68k()
 * - a single self-contained kernel doing dither and C2P together, one
 * 32-pixel block at a time, with no C round trip per tile. This file is only
 * the runtime-active/geometry bookkeeping that the assembly dispatcher
 * (mr_yuv_dither_planar_m68k.S) and the AGA backend's capability check both
 * read.
 *
 * An earlier iteration of this experiment took a different shape: dither
 * four scanlines at a time into a small scratch tile via the established
 * base kernel, then convert each tile with a separate multi-row C2P call
 * (core/mr_c2p_riva_native_m68k.S's mr_c2p8_riva_native_m68k). The
 * self-contained single-kernel approach above superseded it - the dispatcher
 * was retargeted directly at the new kernel - but the superseded C function
 * was left in this file, never removed, and so was never actually reachable
 * again from anywhere. Removed here; mr_c2p_riva_native_m68k.S itself is
 * untouched (it is documented, MIT-licensed, adapted content per CLAUDE.md)
 * but is presently unused by any live code path - worth the project owner's
 * own look before deciding whether to wire it up elsewhere or retire it.
 */
#include "mr_yuv_planar_queue.h"

#define MR_PLANAR_MAX_WIDTH 640

/* Bare symbol names are intentional.  The hand-written .S wrapper references
 * these directly; m68k-amigaos-gcc otherwise decorates C symbols. */
#if defined(__GNUC__)
volatile int mr_yuv_planar_queue_active
    __asm__("mr_yuv_planar_queue_active") = 0;
#else
volatile int mr_yuv_planar_queue_active = 0;
#endif

static int g_visible_width;
static int g_height;
static int g_padded_width;

int mr_yuv_planar_queue_configure(int visible_width, int height,
                                  int padded_width)
{
    if (visible_width <= 0 || height <= 0 ||
        padded_width < visible_width || padded_width > MR_PLANAR_MAX_WIDTH ||
        (padded_width & 31) != 0) {
        mr_yuv_planar_queue_active = 0;
        g_visible_width = g_height = g_padded_width = 0;
        return 0;
    }
    g_visible_width = visible_width;
    g_height = height;
    g_padded_width = padded_width;
    mr_yuv_planar_queue_active = 1;
    return 1;
}

void mr_yuv_planar_queue_disable(void)
{
    mr_yuv_planar_queue_active = 0;
    g_visible_width = g_height = g_padded_width = 0;
}

int mr_yuv_planar_queue_is_active(void)
{
    return mr_yuv_planar_queue_active != 0;
}

int mr_yuv_planar_queue_visible_width(void) { return g_visible_width; }
int mr_yuv_planar_queue_height(void)        { return g_height; }
int mr_yuv_planar_queue_padded_width(void)  { return g_padded_width; }
