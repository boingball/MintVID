/*
 * MintVID - cybergraphics RTG window backend.
 *
 * Opens a titled window on the default public screen (truecolour RTG on
 * SAGA/P96/CGX) and blits RGB24 frames with WritePixelArray, letting
 * cybergraphics do the RGB->screen-depth conversion. Library bases are opened
 * by display.c; this backend just needs CyberGfxBase to be present.
 *
 * On our own private fullscreen screen (cgx_open_private_screen() - never on
 * a shared Workbench screen, windowed or otherwise) this also tries a direct
 * cybergraphics.library LockBitMapTagList()/UnLockBitMap() write, the same
 * technique display_p96.c uses via Picasso96API.library's p96LockBitMap()
 * but through the base cybergraphics.library API instead - no Picasso96API
 * dependency, so it also covers genuine CyberGraphX-only boards that have
 * cybergraphics.library but never had Picasso96API.library at all (display_
 * p96.c's backend_p96 refuses to even open on those - see its file header).
 * Supports the three PIXFMT_* layouts cybergraphics.library's own autodoc
 * lists as "recommended" (PIXFMT_RGB16, PIXFMT_RGB24, PIXFMT_ARGB32 -
 * PIXFMT_LUT8 aside, irrelevant here since this backend is truecolour-only);
 * anything else falls back to WritePixelArray exactly as before, so
 * selecting an unsupported format never loses playback, only the chance at
 * the faster path. See detect_cgx_format()/write_cgx_pixel_strip() below and
 * display_p96.c's file header for the shared background on why this is
 * fullscreen (well, private-screen) only: a direct lock has none of
 * WritePixelArray's automatic Layer-based window clipping. Restricting it to
 * cgx_open_private_screen()'s own screen (never the "fullscreen requested
 * but a private screen could not be opened, fall back to a fullscreen window
 * on the shared Workbench screen" path a few lines below) sidesteps needing
 * display_p96.c's win->LeftEdge/TopEdge absolute-position tracking entirely:
 * a private screen's borderless, undraggable window is opened once at (0,0)
 * and never moves for the screen's whole lifetime (cgx_toggle_fullscreen()
 * tears the whole thing down and rebuilds rather than repositioning it).
 */
#include "amiga_display.h"
#include "display_backend.h"
#include "mr_aspect.h"
#include "../core/mr_scale.h"

#include <stddef.h>
#include <exec/types.h>
#include <exec/memory.h>
#include <intuition/intuition.h>
#include <intuition/screens.h>
#include <graphics/displayinfo.h>
#include <cybergraphx/cybergraphics.h>

#include <proto/exec.h>
#include <proto/intuition.h>
#include <proto/graphics.h>
#include <proto/cybergraphics.h>
#include <string.h>
#include <time.h>
#include <stdio.h>

/* Number of destination rows scaled (and blitted) at a time when the stream
 * is not native size. Keeping this small means a downscaled 720p/1080p
 * stream never needs a full second RGB24 destination frame resident in Fast
 * RAM - only this many rows of it at once. */
#define MR_CGX_STRIP_ROWS 32

/* Native bitmap pixel layouts the private-screen direct-lock path can write
 * directly - see the file header comment. Only ever considered for our own
 * private screen (cgx_open_private_screen()); everything else (windowed, or
 * the fullscreen-on-shared-Workbench-screen fallback) stays on
 * WritePixelArray, which needs no format detection since cybergraphics does
 * the RGB->native conversion for us there. */
typedef enum {
    CGX_FMT_NONE = 0,
    CGX_FMT_RGB16,   /* PIXFMT_RGB16,  2 bytes/pixel */
    CGX_FMT_RGB24,   /* PIXFMT_RGB24,  3 bytes/pixel */
    CGX_FMT_ARGB32   /* PIXFMT_ARGB32, 4 bytes/pixel, alpha unused */
} cgx_fmt;

static int cgx_fmt_bytes(cgx_fmt fmt)
{
    switch (fmt) {
    case CGX_FMT_RGB16:  return 2;
    case CGX_FMT_RGB24:  return 3;
    case CGX_FMT_ARGB32: return 4;
    default:              return 0;
    }
}

typedef struct {
    struct Window *win;
    struct Screen *screen;   /* owned private RTG screen, NULL on Workbench */
    struct Screen *retired_screen; /* close retry after a transient refusal */
    int            bl, bt;   /* blit origin inside the window borders       */
    int            iw, ih;   /* current resizeable inner dimensions         */
    int            source_w, source_h;
    int            dx, dy, dw, dh; /* aspect-fitted video rectangle          */
    int            pending_w, pending_h;
    clock_t        resize_at;
    unsigned char *scaled;   /* persistent RGB24 scale strip (MR_CGX_STRIP_ROWS
                               * rows tall, s->dw wide); NULL until the first
                               * non-native frame actually needs it          */
    size_t         scaled_size;
    int            scaled_w, scaled_stride;
    /* geometry cache --------------------------------------------------------
     * These are initialised to impossible sentinels so that the first call to
     * cgx_show() always enters cgx_rebuild_geometry(), fully priming the RTG
     * state before the first frame is blitted (not lazily mid-presentation).
     * geometry_valid is cleared whenever the cached values need recomputation.
     */
    int            cached_dst_w;    /* -1 sentinel  = rebuild required       */
    int            cached_dst_h;    /* -1 sentinel  = rebuild required       */
    struct BitMap *cached_bitmap;   /* NULL sentinel = rebuild required      */
    int            geometry_valid;  /* 0 = rebuild required                  */
    cgx_fmt        fmt;             /* private-screen direct-lock format,
                                     * NONE = use WritePixelArray instead    */
    mr_display_timing timing;
    int            quit;
    int            fullscreen;
    int            have_window_geometry;
    int            window_left, window_top, window_width, window_height;
    int            window_iw, window_ih;
    int            force_full_redraw;
    /* Owned copy of the current title, not an alias of the caller's string:
     * struct Window.Title is just a pointer Intuition never copies, so it
     * must stay valid for as long as the window does (including across a
     * later fullscreen reopen, see cgx_toggle_fullscreen()) - and some
     * callers (the live playhead) reuse one buffer with new content every
     * second, which a pointer identity compare cannot tell apart from "no
     * change". title tracks what was last actually applied, for the cheap
     * idempotent-update check in cgx_status(). */
    char           title[80];
} cgx_state;

#define ESC_RAWKEY 0x45

/* Forward declarations (implementations follow cgx_open in source order). */
static void cgx_rebuild_geometry(cgx_state *s, const char *reason);
static int  ensure_scaled(cgx_state *s, int w);
static int  cgx_toggle_fullscreen(void *h);
static cgx_fmt detect_cgx_format(struct BitMap *bm);
static void clear_cgx_pixel_rect(struct BitMap *bm, cgx_fmt fmt, int x, int y,
                                 int w, int h);
static int write_cgx_pixel_strip(struct BitMap *bm, int dst_x, int dst_y,
                                 const unsigned char *src, int src_stride,
                                 int w, int rows, cgx_fmt fmt);

/* CloseScreen() may temporarily refuse while Intuition still sees a window or
 * screen lock.  Real PiStorm/Workbench testing has shown that a few VBlanks are
 * not always enough after a fullscreen/private-window teardown.  Keep the
 * screen pointer until CloseScreen() actually succeeds and make a stubborn
 * shutdown visible in the log instead of silently forgetting the screen. */
static int cgx_close_private_screen(struct Screen **screen, const char *reason)
{
    int attempt;
    if (!screen || !*screen) return 1;

    WaitBlit();
    for (attempt = 0; attempt < 50; attempt++) {
        if (CloseScreen(*screen)) {
            *screen = NULL;
            if (attempt > 0)
                printf("rtg-fullscreen: private screen closed after %d "
                       "VBlank(s) (%s)\n", attempt,
                       reason ? reason : "unknown");
            return 1;
        }
        WaitTOF();
    }
    ScreenToBack(*screen);
    printf("rtg-fullscreen: WARNING private screen still busy after "
           "50 VBlanks (%s)\n", reason ? reason : "unknown");
    return 0;
}

/* Open a private RTG screen matching the current public-screen geometry.
 * Fullscreen means the whole display, not the closest mode to the video's
 * native dimensions; cgx_rebuild_geometry() aspect-fits the video afterwards.
 * A 16-bit mode is preferred to reduce RTG memory traffic; if unavailable,
 * retain truecolour compatibility by trying 32 and 24-bit modes. */
static struct Screen *cgx_open_private_screen(cgx_state *s, const char *title)
{
    static const ULONG depths[] = { 16, 32, 24 };
    struct Screen *public_screen;
    struct Screen *scr = NULL;
    ULONG best_modeid = (ULONG)INVALID_ID;
    ULONG best_depth = 0;
    ULONG best_score = ~0UL;
    int target_w = s->source_w;
    int target_h = s->source_h;
    unsigned i;

    /* Preserve the user's chosen Workbench size (for example 1024x768) while
     * still allowing a lower-depth private screen for faster video uploads. */
    public_screen = LockPubScreen(NULL);
    if (public_screen) {
        if (public_screen->Width > 0 && public_screen->Height > 0) {
            target_w = public_screen->Width;
            target_h = public_screen->Height;
        }
        UnlockPubScreen(NULL, public_screen);
    }

    for (i = 0; i < sizeof depths / sizeof depths[0]; i++) {
        ULONG modeid = BestCModeIDTags(
            CYBRBIDTG_NominalWidth, (ULONG)target_w,
            CYBRBIDTG_NominalHeight, (ULONG)target_h,
            CYBRBIDTG_Depth, depths[i],
            TAG_END);
        ULONG mode_w, mode_h, mode_depth, score;
        if (modeid == (ULONG)INVALID_ID || !IsCyberModeID(modeid))
            continue;
        mode_w = GetCyberIDAttr(CYBRIDATTR_WIDTH, modeid);
        mode_h = GetCyberIDAttr(CYBRIDATTR_HEIGHT, modeid);
        mode_depth = GetCyberIDAttr(CYBRIDATTR_DEPTH, modeid);
        if (!mode_w || !mode_h || mode_w == ~0UL || mode_h == ~0UL ||
            mode_depth <= 8 || mode_depth == ~0UL)
            continue;
        score = (ULONG)((int)mode_w >= target_w ? (int)mode_w - target_w
                                                     : target_w - (int)mode_w) +
                (ULONG)((int)mode_h >= target_h ? (int)mode_h - target_h
                                                     : target_h - (int)mode_h);
        /* Match the public-screen dimensions first. For otherwise equal modes
         * prefer the lower depth and lower RTG memory traffic. */
        if (score < best_score ||
            (score == best_score && mode_depth < best_depth)) {
            best_modeid = modeid;
            best_depth = mode_depth;
            best_score = score;
        }
    }
    if (best_modeid != (ULONG)INVALID_ID)
        scr = OpenScreenTags(NULL,
            SA_DisplayID, best_modeid,
            SA_Depth, best_depth,
            SA_Type, CUSTOMSCREEN,
            SA_Title, (ULONG)(title ? title : "MintVID"),
            SA_Quiet, TRUE,
            SA_ShowTitle, FALSE,
            SA_Draggable, FALSE,
            TAG_END);
    /* Some older CGX versions may still open a mode with the wrong depth.
     * Never let that turn the private path into an unusable LUT8 screen. */
    if (scr && GetCyberMapAttr(scr->RastPort.BitMap,
                               CYBRMATTR_DEPTH) <= 8) {
        CloseScreen(scr);
        scr = NULL;
    }
    if (scr && g_display_want_time)
        printf("rtg-fullscreen private=%dx%d depth=%lu target=%dx%d "
               "source=%dx%d\n",
               scr->Width, scr->Height,
               (unsigned long)GetCyberMapAttr(scr->RastPort.BitMap,
                                               CYBRMATTR_DEPTH),
               target_w, target_h, s->source_w, s->source_h);
    return scr;
}

/* Three native layouts are supported for the private-screen direct-lock
 * path - see the file header comment. bm may be any live BitMap, but this
 * is only ever actually called (from cgx_rebuild_geometry()) when s->screen
 * is set, i.e. our own private screen. Cross-checks CYBRMATTR_BPPIX against
 * the format as an extra sanity check, not just trust in the PIXFMT enum
 * value alone (mirrors display_p96.c's detect_bitmap_format()). */
static cgx_fmt detect_cgx_format(struct BitMap *bm)
{
    ULONG pixfmt, bpp;
    if (!bm || !CyberGfxBase) return CGX_FMT_NONE;
    pixfmt = GetCyberMapAttr(bm, CYBRMATTR_PIXFMT);
    bpp = GetCyberMapAttr(bm, CYBRMATTR_BPPIX);
    switch (pixfmt) {
    case PIXFMT_RGB16:  return bpp == 2 ? CGX_FMT_RGB16  : CGX_FMT_NONE;
    case PIXFMT_RGB24:  return bpp == 3 ? CGX_FMT_RGB24  : CGX_FMT_NONE;
    case PIXFMT_ARGB32: return bpp == 4 ? CGX_FMT_ARGB32 : CGX_FMT_NONE;
    default:              return CGX_FMT_NONE;
    }
}

/* Black out a rect directly (same lock/unlock discipline as
 * write_cgx_pixel_strip). Used by cgx_rebuild_geometry() to clear the
 * letterbox/pillarbox area on our own private screen instead of
 * FillPixelArray, same reasoning as display_p96.c's clear_pixel_rect(): a
 * zero byte is black in all three supported layouts (PIXFMT_ARGB32's alpha
 * byte is unused, so a zeroed alpha there is harmless too). */
static void clear_cgx_pixel_rect(struct BitMap *bm, cgx_fmt fmt, int x, int y,
                                 int w, int h)
{
    APTR lock;
    ULONG base = 0, bpr = 0;
    int row, bpp = cgx_fmt_bytes(fmt);
    unsigned char *drow_base;

    if (w <= 0 || h <= 0 || !bpp) return;
    lock = LockBitMapTags(bm,
        LBMI_BASEADDRESS, (ULONG)&base,
        LBMI_BYTESPERROW, (ULONG)&bpr,
        TAG_END);
    if (!lock) return;
    drow_base = (unsigned char *)base + (size_t)y * (size_t)bpr +
               (size_t)x * (size_t)bpp;
    for (row = 0; row < h; row++)
        memset(drow_base + (size_t)row * (size_t)bpr, 0, (size_t)w * (size_t)bpp);
    UnLockBitMap(lock);
}

/* Lock our own private screen's bitmap directly (cybergraphics.library's
 * LockBitMapTagList()/UnLockBitMap() - see the file header comment) and
 * write `rows` of already-RGB24 source (cgx_show() never carries a BGR24
 * source the way display_p96.c's H.264 direct-YUV path can, so there is no
 * src_is_bgr parameter here) into native screen memory, in whichever of the
 * three supported layouts `fmt` names.
 *
 * Per cybergraphics.library/LockBitMapTagList's own autodoc: "DON'T use any
 * library calls while the bitmap is locked!" - so, same as display_p96.c,
 * the lock is taken and released per strip, with service() (in cgx_show())
 * called only between strips, never while locked.
 *
 * The 16/32-bit stores below are plain UWORD/ULONG writes at pixel-aligned
 * (even/4-byte) offsets - on this big-endian target that alone produces the
 * byte order PIXFMT_RGB16/PIXFMT_ARGB32 name (same reasoning as display_
 * p96.c's write_pixel_strip() for RGBFB_R5G6B5/RGBFB_A8R8G8B8 - cyber-
 * graphics.library's own WritePixelArray autodoc documents RECTFMT_RGB as
 * red-byte-first and RECTFMT_ARGB as alpha-byte-first, the same letter-order-
 * is-byte-order convention PIXFMT_* follows), with no separate byte-swap
 * step needed. */
static int write_cgx_pixel_strip(struct BitMap *bm, int dst_x, int dst_y,
                                 const unsigned char *src, int src_stride,
                                 int w, int rows, cgx_fmt fmt)
{
    APTR lock;
    ULONG base = 0, bpr = 0;
    int y, bpp = cgx_fmt_bytes(fmt);
    unsigned char *drow_base;

    if (!bpp) return 0;
    lock = LockBitMapTags(bm,
        LBMI_BASEADDRESS, (ULONG)&base,
        LBMI_BYTESPERROW, (ULONG)&bpr,
        TAG_END);
    if (!lock) return 0;

    drow_base = (unsigned char *)base + (size_t)dst_y * (size_t)bpr +
               (size_t)dst_x * (size_t)bpp;
    for (y = 0; y < rows; y++) {
        const unsigned char *srow = src + (size_t)y * (size_t)src_stride;
        unsigned char *drow = drow_base + (size_t)y * (size_t)bpr;
        if (fmt == CGX_FMT_RGB24) {
            memcpy(drow, srow, (size_t)w * 3u);
            continue;
        }
        {
            int x;
            for (x = 0; x < w; x++) {
                unsigned char r = srow[x * 3 + 0];
                unsigned char g = srow[x * 3 + 1];
                unsigned char b = srow[x * 3 + 2];
                switch (fmt) {
                case CGX_FMT_RGB16:
                    *(UWORD *)(drow + x * 2) = (UWORD)
                        (((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
                    break;
                case CGX_FMT_ARGB32:
                    *(ULONG *)(drow + x * 4) =
                        ((ULONG)r << 16) | ((ULONG)g << 8) | (ULONG)b;
                    break;
                default:
                    break;
                }
            }
        }
    }

    UnLockBitMap(lock);
    return 1;
}

static struct Window *cgx_open_window(cgx_state *s, const char *title,
                                      int inner_w, int inner_h,
                                      struct Screen **private_screen)
{
    struct Screen *scr;
    struct Window *win = NULL;
    if (private_screen) *private_screen = NULL;
    if (s->fullscreen) {
        scr = cgx_open_private_screen(s, title);
        if (scr) {
            win = OpenWindowTags(NULL,
                WA_CustomScreen, (ULONG)scr,
                WA_Title, (ULONG)(title ? title : "MintVID"),
                WA_Left, 0, WA_Top, 0,
                WA_Width, (ULONG)scr->Width, WA_Height, (ULONG)scr->Height,
                WA_Flags, WFLG_BORDERLESS | WFLG_BACKDROP | WFLG_ACTIVATE |
                          WFLG_RMBTRAP | WFLG_NOCAREREFRESH,
                WA_IDCMP, IDCMP_CLOSEWINDOW | IDCMP_RAWKEY,
                TAG_END);
            if (win) {
                if (private_screen) *private_screen = scr;
                return win;
            }
            CloseScreen(scr);
        }
        if (g_display_want_time)
            printf("rtg-fullscreen: private screen unavailable, "
                   "using Workbench screen\n");
    }

    scr = LockPubScreen(NULL);
    if (!scr) return NULL;
    if (s->fullscreen) {
        win = OpenWindowTags(NULL,
            WA_PubScreen, (ULONG)scr,
            WA_Title, (ULONG)(title ? title : "MintVID"),
            WA_Left, 0, WA_Top, 0,
            WA_Width, (ULONG)scr->Width, WA_Height, (ULONG)scr->Height,
            WA_Borderless, TRUE, WA_Activate, TRUE,
            WA_Flags, WFLG_NOCAREREFRESH | WFLG_RMBTRAP,
            WA_IDCMP, IDCMP_CLOSEWINDOW | IDCMP_RAWKEY | IDCMP_NEWSIZE,
            TAG_END);
    } else {
        win = OpenWindowTags(NULL,
            WA_PubScreen, (ULONG)scr,
            WA_Title, (ULONG)(title ? title : "MintVID"),
            WA_InnerWidth, (ULONG)inner_w,
            WA_InnerHeight, (ULONG)inner_h,
            WA_Flags, WFLG_DRAGBAR | WFLG_DEPTHGADGET | WFLG_CLOSEGADGET |
                      WFLG_SIZEGADGET | WFLG_ACTIVATE | WFLG_NOCAREREFRESH,
            WA_MinWidth, 160, WA_MinHeight, 100,
            WA_MaxWidth, (ULONG)-1, WA_MaxHeight, (ULONG)-1,
            WA_IDCMP, IDCMP_CLOSEWINDOW | IDCMP_RAWKEY | IDCMP_NEWSIZE,
            TAG_END);
    }
    UnlockPubScreen(NULL, scr);
    return win;
}

/* Having cybergraphics.library is not enough - the actual public screen we'd
 * render into must be an RTG/truecolour mode. On an AGA (planar) Workbench,
 * WritePixelArray can't draw, so report "not RTG" and let the dispatcher fall
 * back to the AGA backend. */
static int default_screen_is_rtg(void)
{
    struct Screen *scr = LockPubScreen(NULL);
    int rtg = 0;
    if (scr) {
        ULONG modeid = GetVPModeID(&scr->ViewPort);
        if (modeid != (ULONG)INVALID_ID && IsCyberModeID(modeid))
            rtg = 1;
        UnlockPubScreen(NULL, scr);
    }
    return rtg;
}

/* Fit a w*h picture inside a bounding box, preserving aspect ratio. Only ever
 * shrinks: content that already fits is left at native size. Used so a stream
 * larger than the Workbench screen (e.g. a 1920x1080 broadcast on a 720p RTG
 * desktop) opens a window that fits and is downscaled, instead of a window
 * bigger than the display. */
static void fit_within(int w, int h, int max_w, int max_h, int *out_w,
                       int *out_h)
{
    *out_w = w;
    *out_h = h;
    if (w <= 0 || h <= 0 || max_w <= 0 || max_h <= 0)
        return;
    if (w <= max_w && h <= max_h)
        return;
    /* Compare w/h against max_w/max_h without floats: width-bound when
     * w*max_h > h*max_w. Round to nearest to avoid a stray black edge. */
    if ((long)w * max_h > (long)h * max_w) {
        *out_w = max_w;
        *out_h = (int)(((long)h * max_w + w / 2) / w);
    } else {
        *out_h = max_h;
        *out_w = (int)(((long)w * max_h + h / 2) / h);
    }
    if (*out_w < 1) *out_w = 1;
    if (*out_h < 1) *out_h = 1;
}

static void *cgx_open(int w, int h, const char *title)
{
    cgx_state *s;
    int win_w = w, win_h = h;
    struct Screen *scr;
    if (!CyberGfxBase || !default_screen_is_rtg())
        return NULL;                              /* not RTG -> try AGA      */

    s = (cgx_state *)AllocVec(sizeof *s, MEMF_CLEAR);
    if (!s) return NULL;

    /* Cap the initial window to what the public screen can actually show, so a
     * picture larger than the desktop is downscaled into a visible window
     * rather than opened past the screen edges. The user can still size up. */
    scr = LockPubScreen(NULL);
    if (scr) {
        int avail_w = scr->Width - scr->WBorLeft - scr->WBorRight;
        int avail_h = scr->Height - scr->BarHeight - 1 - scr->WBorTop -
                      scr->WBorBottom;
        UnlockPubScreen(NULL, scr);
        if (avail_w < 160) avail_w = 160;
        if (avail_h < 100) avail_h = 100;
        fit_within(w, h, avail_w, avail_h, &win_w, &win_h);
    }

    s->fullscreen = g_display_fullscreen;
    s->source_w = w;
    s->source_h = h;
    snprintf(s->title, sizeof s->title, "%s", (title && *title) ? title : "MintVID");
    s->win = cgx_open_window(s, title, win_w, win_h, &s->screen);
    if (!s->win) { FreeVec(s); return NULL; }

    s->bl = s->win->BorderLeft;
    s->bt = s->win->BorderTop;
    s->iw = s->win->Width - s->win->BorderLeft - s->win->BorderRight;
    s->ih = s->win->Height - s->win->BorderTop - s->win->BorderBottom;
    s->pending_w = s->iw; s->pending_h = s->ih;
    s->force_full_redraw = 1;
    if (!s->fullscreen) {
        s->have_window_geometry = 1;
        s->window_left = s->win->LeftEdge;
        s->window_top = s->win->TopEdge;
        s->window_width = s->win->Width;
        s->window_height = s->win->Height;
        s->window_iw = s->iw;
        s->window_ih = s->ih;
    }

    /* Initialise geometry cache with impossible sentinels so that the first
     * cgx_show() call always triggers cgx_rebuild_geometry(), matching the
     * RTG state that a resize round-trip would produce.                     */
    s->cached_dst_w  = -1;
    s->cached_dst_h  = -1;
    s->cached_bitmap = NULL;
    s->geometry_valid = 0;

    /* Prime the cache before the first frame is presented. Note this does
     * NOT allocate s->scaled: a scale buffer is only ever needed once
     * cgx_show() determines a given frame is non-native, and is allocated
     * lazily there (see ensure_scaled()).                                   */
    cgx_rebuild_geometry(s, "init");

    return s;
}

static unsigned long elapsed_us(clock_t begin)
{
    return (unsigned long)((clock() - begin) * 1000000UL / CLOCKS_PER_SEC);
}

/*
 * Rebuild all RTG geometry and cache state atomically.
 *
 * This is the single shared path for initial window creation, resize events
 * and screen/bitmap changes.  Callers set geometry_valid = 0 (or the sentinel
 * values) to force a rebuild; the result is stored back into the cache so
 * subsequent frames skip the work entirely.
 *
 * This deliberately does NOT touch s->scaled. Native-size rendering (the
 * common case for 720p/1080p streams, whose window opens at the stream's own
 * resolution) never reads s->scaled - cgx_show() sends the incoming RGB
 * straight to WritePixelArray for native frames. Allocating a full-frame
 * scale buffer here unconditionally would waste Fast RAM (~2.7MB at 720p,
 * ~6.2MB at 1080p) for streams that will never use it, and if that
 * allocation failed it was previously ignored, leaving cgx_show() rendering
 * onto an otherwise untouched (grey) RTG window with no diagnostic. The
 * scale buffer is now allocated lazily, and much smaller, in cgx_show() -
 * see ensure_scaled() / MR_CGX_STRIP_ROWS.
 */
static void cgx_rebuild_geometry(cgx_state *s, const char *reason)
{
    struct BitMap *bm;
    int bm_changed, scale_map_changed, old_iw, old_ih;
    mr_aspect_rect fit;
    ULONG bm_depth = 0;

    /* Re-read inner dimensions from the live window struct; these reflect the
     * post-open, post-resize values as reported by Intuition.               */
    old_iw = s->iw;
    old_ih = s->ih;
    s->bl = s->win->BorderLeft;
    s->bt = s->win->BorderTop;
    s->iw = s->win->Width  - s->win->BorderLeft - s->win->BorderRight;
    s->ih = s->win->Height - s->win->BorderTop  - s->win->BorderBottom;
    if (s->iw < 1) s->iw = 1;
    if (s->ih < 1) s->ih = 1;

    fit = mr_aspect_fit(s->source_w, s->source_h, s->iw, s->ih);

    /* Detect whether the aspect-fitted output dimensions (and thus the scale
     * factors) changed. The enclosing window may be any shape; video never is. */
    scale_map_changed = (fit.w != s->cached_dst_w ||
                         fit.h != s->cached_dst_h ||
                         fit.x != s->dx || fit.y != s->dy ||
                         s->iw != old_iw || s->ih != old_ih);

    /* Query destination bitmap for depth / pitch.  Available only on real RTG
     * hardware where CyberGfxBase is present; safe to skip if not.          */
    bm = s->win->RPort->BitMap;
    bm_changed = (bm != s->cached_bitmap);
    if (bm && CyberGfxBase)
        bm_depth = GetCyberMapAttr(bm, CYBRMATTR_DEPTH);
    /* Direct-lock format is only ever detected (and so only ever used) for
     * our own private screen - see the file header comment. Re-detected
     * whenever the live bitmap changes, same trigger display_p96.c's
     * detect_bitmap_format() call uses. */
    if (bm_changed)
        s->fmt = s->screen ? detect_cgx_format(bm) : CGX_FMT_NONE;

    /* Cache the new geometry. */
    s->dx = fit.x; s->dy = fit.y;
    s->dw = fit.w; s->dh = fit.h;
    s->cached_dst_w  = s->dw;
    s->cached_dst_h  = s->dh;
    s->cached_bitmap = bm;
    s->geometry_valid = 1;
    if (scale_map_changed || bm_changed) {
        /* Clear the whole client area once so resized/toggled windows acquire
         * clean black letterbox or pillarbox bars. Later frames touch only the
         * fitted video rectangle. FillPixelArray is clipped by the window
         * layer; the direct-lock clear isn't, but s->bl/s->bt are always 0 on
         * our own borderless private screen and s->iw/s->ih are the whole
         * screen, so there is no window layer to clip against there anyway. */
        if (s->fmt != CGX_FMT_NONE)
            clear_cgx_pixel_rect(bm, s->fmt, s->bl, s->bt, s->iw, s->ih);
        else
            FillPixelArray(s->win->RPort, (UWORD)s->bl, (UWORD)s->bt,
                           (UWORD)s->iw, (UWORD)s->ih, 0x00000000UL);
        s->force_full_redraw = 1;
    }

    if (g_display_want_time)
        printf("rtg-geometry reason=%s iw=%d ih=%d video=%d,%d %dx%d "
               "bm-changed=%d scale-map-rebuild=%d "
               "depth=%lu fmt=%d\n",
               reason ? reason : "?",
               s->iw, s->ih, s->dx, s->dy, s->dw, s->dh,
               bm_changed, scale_map_changed,
               (unsigned long)bm_depth, (int)s->fmt);
}

static void report_slow(const char *operation, unsigned long usec,
                        mr_display_service_fn service, void *opaque)
{
    if (usec <= 10000) return;
    if (service) service(opaque);
    printf("cgx-slow operation=%s duration=%lu us\n", operation, usec);
    if (service) service(opaque);
}

/* Allocate (or reuse) the scale strip buffer: MR_CGX_STRIP_ROWS destination
 * rows, s->dw wide, RGB24. Only called from cgx_show() once it has actually
 * determined a frame is non-native, so native-size streams (640x360 today,
 * and typically 720p/1080p too) never allocate this at all. */
static int ensure_scaled(cgx_state *s, int w)
{
    size_t stride, bytes;
    unsigned char *p;
    if (w <= 0 || w > 65535) return 0;
    if ((size_t)w > (size_t)-1 / 3) return 0;
    stride = (size_t)w * 3;
    if ((size_t)MR_CGX_STRIP_ROWS > (size_t)-1 / stride) return 0;
    bytes = stride * (size_t)MR_CGX_STRIP_ROWS;
    if (s->scaled && s->scaled_w == w) return 1;
    p = (unsigned char *)AllocVec(bytes, MEMF_ANY);
    if (!p) return 0; /* retain the old buffer and the existing failure mode */
    if (s->scaled) FreeVec(s->scaled);
    s->scaled = p; s->scaled_size = bytes;
    s->scaled_w = w; s->scaled_stride = (int)stride;
    return 1;
}

/* Integer nearest-neighbour RGB24 scaler, one strip at a time: shares the
 * exact DDA (no per-pixel multiply/divide, just add-and-occasionally-carry)
 * that core/mr_scale.c's full-image resize uses, via the _strip() entry
 * point that fast-forwards its row state to dst_y0 instead of starting at
 * row 0. The previous version here re-derived a 16.16 fixed-point x/y step
 * and paid a size_t multiply per output pixel to turn it back into a byte
 * offset (`(sx >> 16) * 3`) - real cost at 1024x576, all of it on top of the
 * WritePixelArray blit already following each strip. */
static void scale_rgb24_strip(cgx_state *s, const unsigned char *src, int sw,
                              int sh, int src_stride, int dst_y0, int rows)
{
    mr_scale_resize_rgb24_strip(src, sw, sh, src_stride, s->scaled, s->dw,
                                s->dh, s->scaled_stride, dst_y0, rows);
}

static void cgx_show(void *h, const unsigned char *rgb, int w, int hh,
                     int stride, int dy0, int dy1,
                     mr_display_service_fn service, void *service_opaque)
{
    cgx_state *s = (cgx_state *)h;
    clock_t total = 0, mark = 0;
    int native, timing;
    if (!s || !s->win) return;
    timing = g_display_want_time;
    if (timing) {
        memset(&s->timing, 0, sizeof s->timing);
        total = mark = clock();
    }
    /* Rebuild RTG geometry when:
     *   - geometry has never been built (geometry_valid == 0, handles first frame)
     *   - the window's bitmap changed (screen mode flip)
     *   - a resize has been pending long enough (Intuition quiet for 100 ms)   */
    {
        struct BitMap *cur_bm = s->win->RPort->BitMap;
        int need_rebuild = !s->geometry_valid || cur_bm != s->cached_bitmap;
        if (!need_rebuild &&
            (s->pending_w != s->iw || s->pending_h != s->ih) &&
            clock() - s->resize_at >= CLOCKS_PER_SEC / 10)
            need_rebuild = 1;
        if (need_rebuild)
            cgx_rebuild_geometry(s, !s->geometry_valid ? "invalid" :
                                  cur_bm != s->cached_bitmap ? "bitmap-change"
                                                              : "resize");
    }
    if (timing) {
        s->timing.resize_us = elapsed_us(mark);
        report_slow("resize-handling", s->timing.resize_us,
                    service, service_opaque);
        mark = clock();
    }
    if (s->force_full_redraw) {
        dy0 = 0;
        dy1 = hh;
        s->force_full_redraw = 0;
    }
    if (dy0 < 0) dy0 = 0;
    if (dy1 > hh) dy1 = hh;
    if (dy1 <= dy0) return;                       /* nothing changed         */
    if (timing) {
        s->timing.clip_us = elapsed_us(mark);
        report_slow("clipping-setup", s->timing.clip_us,
                    service, service_opaque);
        mark = clock();
    }
    if (timing) {
        s->timing.src_w = w; s->timing.src_h = hh;
        s->timing.dst_w = s->dw; s->timing.dst_h = s->dh;
        s->timing.src_format = "RGB24";
        s->timing.dst_format = s->fmt == CGX_FMT_RGB16 ? "RGB16" :
                               s->fmt == CGX_FMT_ARGB32 ? "ARGB32" : "RGB24";
    }
    native = s->dw == w && s->dh == hh;
    if (timing) {
        s->timing.geometry_us = elapsed_us(mark);
        report_slow("geometry-format-setup", s->timing.geometry_us,
                    service, service_opaque);
    }

    if (!native) {
        /* Downscaled path: only here do we ever need a scale buffer, and only
         * MR_CGX_STRIP_ROWS destination rows of it at a time - source RGB is
         * scaled a strip at a time and blitted immediately, so at most one
         * small strip (not a whole second destination frame) is resident. */
        int y;
        if (timing) mark = clock();
        if (service) service(service_opaque);
        if (!ensure_scaled(s, s->dw)) {
            if (service) service(service_opaque);
            printf("cgx-error: scale strip allocation failed (%dx%d, "
                   "%lu bytes) - dropping frame\n",
                   s->dw, MR_CGX_STRIP_ROWS,
                   (unsigned long)((size_t)s->dw * 3 *
                                   (size_t)MR_CGX_STRIP_ROWS));
            return;
        }
        if (service) service(service_opaque);
        if (timing) {
            s->timing.allocation_us = elapsed_us(mark);
            report_slow("destination-buffer-allocation",
                        s->timing.allocation_us, service, service_opaque);
        }

        if (timing) s->timing.prepare_us = elapsed_us(total);
        /* Time every service() call in this path too, accumulated into one
         * field - see mr_display_timing::service_us's own comment (and the
         * native path below, which hits the same gap in a simpler shape).
         * Without this, none of these calls' cost showed up in scale_us or
         * blit_us, yet all of it was still included in total_us. */
        if (timing) {
            clock_t svc_mark = clock();
            if (service) service(service_opaque);
            s->timing.service_us = elapsed_us(svc_mark);
        } else if (service) service(service_opaque);

        /* Time the CPU-side resample and the WritePixelArray blit separately
         * (previously one clock() span covered both strip loop iterations,
         * so scale_us and blit_us were always identical - unable to tell
         * whether the CPU resample or the RTG driver blit call was the
         * actual cost). service() runs between the two, outside of both. */
        if (timing) {
            s->timing.scale_us = 0;
            s->timing.blit_us = 0;
        }
        for (y = 0; y < s->dh; y += MR_CGX_STRIP_ROWS) {
            int rows = s->dh - y < MR_CGX_STRIP_ROWS ? s->dh - y
                                                      : MR_CGX_STRIP_ROWS;
            clock_t step;

            if (timing) step = clock();
            scale_rgb24_strip(s, rgb, w, hh, stride, y, rows);
            if (timing) s->timing.scale_us += elapsed_us(step);

            if (timing) step = clock();
            if (s->fmt != CGX_FMT_NONE)
                write_cgx_pixel_strip(s->win->RPort->BitMap,
                                      s->bl + s->dx, s->bt + s->dy + y,
                                      s->scaled, s->scaled_stride, s->dw,
                                      rows, s->fmt);
            else
                WritePixelArray((APTR)s->scaled, 0, 0, (UWORD)s->scaled_stride,
                                s->win->RPort, (UWORD)(s->bl + s->dx),
                                (UWORD)(s->bt + s->dy + y),
                                (UWORD)s->dw, (UWORD)rows, RECTFMT_RGB);
            if (timing) s->timing.blit_us += elapsed_us(step);

            if (timing) {
                clock_t svc_step = clock();
                if (service) service(service_opaque);
                s->timing.service_us += elapsed_us(svc_step);
            } else if (service) service(service_opaque);
        }
        if (timing) {
            report_slow("software-scale", s->timing.scale_us,
                        service, service_opaque);
            report_slow("cgx-blit", s->timing.blit_us,
                        service, service_opaque);
        }
        if (timing) {
            s->timing.pixels = (unsigned long)s->dw * (unsigned long)s->dh;
            s->timing.bytes = (unsigned long)s->scaled_size *
                              (unsigned long)((s->dh + MR_CGX_STRIP_ROWS - 1) /
                                              MR_CGX_STRIP_ROWS);
            s->timing.copies =
                (unsigned long)((s->dh + MR_CGX_STRIP_ROWS - 1) /
                                MR_CGX_STRIP_ROWS);
        }
        if (timing) {
            clock_t svc_mark = clock();
            if (service) service(service_opaque);
            s->timing.service_us += elapsed_us(svc_mark);
            /* NULL/NULL, not service/service_opaque: this report's own
             * subject *is* the service callback, so bracketing it with more
             * service() calls (report_slow's normal "keep audio fed during
             * a slow print" behaviour) would re-invoke the very thing being
             * measured - two extra full service_audio_for_display() calls
             * every frame once service_us is already over the 10ms
             * threshold, which it reliably is once things are behind. That
             * self-amplification, not real servicing cost, was inflating
             * these numbers. */
            report_slow("service-callback", s->timing.service_us, NULL, NULL);
        } else if (service) service(service_opaque);
        if (timing) s->timing.total_us = elapsed_us(total);
        return;
    }

    if (timing) s->timing.prepare_us = elapsed_us(total);
    /* Time the service callback itself, separately from the blit calls below
     * - see mr_display_timing::service_us's own comment. Without this, its
     * cost (Paula refill, and a due-frame presentation when the main loop
     * has released the queue) fell into neither prepare_us nor blit_us, yet
     * was still included in total_us - so a slow service call (observed:
     * Paula needing an unusually large catch-up refill right after an
     * audio-rescue episode) made total_us balloon with no matching increase
     * in any of the reported sub-phases, the exact symptom that motivated
     * this field. */
    if (timing) {
        clock_t svc_mark = clock();
        if (service) service(service_opaque);
        s->timing.service_us = elapsed_us(svc_mark);
    } else if (service) service(service_opaque);
    if (timing) s->timing.blit_us = 0;
    /* Keep the dirty-row fast path, but split tall native pictures into small
     * writes. Several real P96/CGX drivers visibly retain old horizontal
     * bands when handed one multi-megabyte 1080p RGB24 WritePixelArray. The
     * same pixels and source stride are used here; only the transfer height is
     * bounded, which also gives Paula/input a service point during a large
     * frame upload. */
    {
        int y;
        for (y = dy0; y < dy1; y += MR_CGX_STRIP_ROWS) {
            int rows = dy1 - y < MR_CGX_STRIP_ROWS ? dy1 - y
                                                    : MR_CGX_STRIP_ROWS;
            clock_t step;
            if (timing) step = clock();
            if (s->fmt != CGX_FMT_NONE)
                write_cgx_pixel_strip(s->win->RPort->BitMap,
                                      s->bl + s->dx, s->bt + s->dy + y,
                                      rgb + (size_t)y * stride, stride, w,
                                      rows, s->fmt);
            else
                WritePixelArray((APTR)(rgb + (size_t)y * stride), 0, 0,
                                (UWORD)stride, s->win->RPort,
                                (UWORD)(s->bl + s->dx),
                                (UWORD)(s->bt + s->dy + y), (UWORD)w,
                                (UWORD)rows, RECTFMT_RGB);
            if (timing) { s->timing.blit_us += elapsed_us(step); s->timing.copies++; }
            if (timing) {
                clock_t svc_step = clock();
                if (service) service(service_opaque);
                s->timing.service_us += elapsed_us(svc_step);
            } else if (service) service(service_opaque);
        }
    }
    if (timing)
        /* NULL/NULL - see the matching comment on the native-path call
         * above: re-servicing to report on servicing cost self-amplifies. */
        report_slow("service-callback", s->timing.service_us, NULL, NULL);
    if (timing) {
        s->timing.pixels = (unsigned long)w * (unsigned long)(dy1 - dy0);
        s->timing.bytes = (unsigned long)stride * (unsigned long)(dy1 - dy0);
        s->timing.total_us = elapsed_us(total);
    }
}

static int cgx_timing(void *h, mr_display_timing *timing)
{
    cgx_state *s = (cgx_state *)h;
    if (!s || !timing) return 0;
    *timing = s->timing; return 1;
}

static int cgx_poll(void *h)
{
    cgx_state *s = (cgx_state *)h;
    struct IntuiMessage *msg;
    int ev = MR_EV_NONE;
    if (!s || !s->win) return MR_EV_QUIT;
    while ((msg = (struct IntuiMessage *)GetMsg(s->win->UserPort))) {
        ULONG cls = msg->Class; UWORD code = msg->Code;
        ReplyMsg((struct Message *)msg);
        if (cls == IDCMP_CLOSEWINDOW) s->quit = 1;
        else if (cls == IDCMP_NEWSIZE) {
            s->pending_w = s->win->Width - s->win->BorderLeft - s->win->BorderRight;
            s->pending_h = s->win->Height - s->win->BorderTop - s->win->BorderBottom;
            s->resize_at = clock();
        }
        else if (cls == IDCMP_RAWKEY && !(code & 0x80)) {  /* key down only  */
            switch (code) {
            case 0x45: s->quit = 1; break;             /* ESC              */
            case 0x40: ev = MR_EV_PAUSE; break;        /* space            */
            case 0x23: cgx_toggle_fullscreen(s); break;/* F                */
            case 0x4E: ev = MR_EV_SEEK_FWD; break;     /* cursor right     */
            case 0x4F: ev = MR_EV_SEEK_BACK; break;    /* cursor left      */
            case 0x4C: ev = MR_EV_VOLUME_UP; break;    /* cursor up        */
            case 0x4D: ev = MR_EV_VOLUME_DOWN; break;  /* cursor down      */
            }
        }
    }
    return s->quit ? MR_EV_QUIT : ev;
}

static int cgx_toggle_fullscreen(void *h)
{
    cgx_state *s = (cgx_state *)h;
    struct Window *old, *replacement;
    struct Screen *old_screen, *replacement_screen = NULL;
    struct IntuiMessage *msg;
    int next_fullscreen;
    if (!s || !s->win) return 0;
    cgx_close_private_screen(&s->retired_screen, "toggle retry");
    if (!s->fullscreen && s->retired_screen) {
        /* Do not accumulate custom screens if Intuition still has a lock on
         * the previous one.  The current Workbench window remains usable. */
        printf("rtg-fullscreen: previous private screen still busy\n");
        return 0;
    }
    old = s->win;
    old_screen = s->screen;
    if (!s->fullscreen) {
        s->have_window_geometry = 1;
        s->window_left = old->LeftEdge;
        s->window_top = old->TopEdge;
        s->window_width = old->Width;
        s->window_height = old->Height;
        s->window_iw = old->Width - old->BorderLeft - old->BorderRight;
        s->window_ih = old->Height - old->BorderTop - old->BorderBottom;
    }
    next_fullscreen = !s->fullscreen;
    s->fullscreen = next_fullscreen;
    replacement = cgx_open_window(s, s->title,
        s->have_window_geometry ? s->window_iw : s->iw,
        s->have_window_geometry ? s->window_ih : s->ih,
        &replacement_screen);
    if (!replacement) {
        s->fullscreen = !next_fullscreen;
        return 0;
    }
    while ((msg = (struct IntuiMessage *)GetMsg(old->UserPort)) != NULL)
        ReplyMsg((struct Message *)msg);
    CloseWindow(old);
    s->win = replacement;
    s->screen = replacement_screen;
    if (old_screen &&
        !cgx_close_private_screen(&old_screen, "fullscreen toggle"))
        s->retired_screen = old_screen;
    if (!s->fullscreen && s->have_window_geometry)
        ChangeWindowBox(s->win, s->window_left, s->window_top,
                        s->window_width, s->window_height);
    s->bl = s->win->BorderLeft;
    s->bt = s->win->BorderTop;
    s->pending_w = s->iw = s->win->Width - s->win->BorderLeft -
                            s->win->BorderRight;
    s->pending_h = s->ih = s->win->Height - s->win->BorderTop -
                            s->win->BorderBottom;
    s->cached_dst_w = s->cached_dst_h = -1;
    s->cached_bitmap = NULL;
    s->geometry_valid = 0;
    s->force_full_redraw = 1;
    return 1;
}

static void cgx_status(void *h, const char *text)
{
    cgx_state *s = (cgx_state *)h;
    const char *want = (text && *text) ? text : "MintVID";
    if (!s || !s->win) return;
    /* Compare content, not the caller's pointer: some callers (the live
     * playhead) reuse one buffer with new text every second, which a
     * pointer identity check cannot tell apart from "no change" - this
     * still cheaply skips the common case of the same status being set
     * repeatedly (e.g. every reconnect attempt). The second arg to
     * SetWindowTitles leaves the screen title unchanged. */
    if (strcmp(s->title, want) == 0) return;
    snprintf(s->title, sizeof s->title, "%s", want);
    SetWindowTitles(s->win, (CONST_STRPTR)s->title, (CONST_STRPTR)~0UL);
}

static void cgx_close(void *h)
{
    cgx_state *s = (cgx_state *)h;
    struct IntuiMessage *msg;

    if (!s) return;

    if (s->win) {
        /*
         * Stop new IDCMP traffic, reply anything already queued, then wait for
         * graphics activity before destroying the window/private screen.
         */
        ModifyIDCMP(s->win, 0);
        if (s->win->UserPort) {
            while ((msg = (struct IntuiMessage *)GetMsg(s->win->UserPort)))
                ReplyMsg((struct Message *)msg);
        }
        WaitBlit();
        CloseWindow(s->win);
        s->win = NULL;
        WaitTOF();
        WaitTOF();
    }

    cgx_close_private_screen(&s->screen, "shutdown");
    cgx_close_private_screen(&s->retired_screen, "shutdown retry");

    /* WritePixelArray source storage stays valid until teardown is quiescent. */
    if (s->scaled) FreeVec(s->scaled);
    FreeVec(s);
}

static ULONG cgx_wait_mask(void *h)
{
    cgx_state *s = (cgx_state *)h;
    if (!s || !s->win || !s->win->UserPort) return 0;
    return 1UL << s->win->UserPort->mp_SigBit;
}

const display_backend backend_cgx = {
    .name = "RTG (CGX)",
    .open = cgx_open,
    .show = cgx_show,
    .timing = cgx_timing,
    .poll = cgx_poll,
    .close = cgx_close,
    .status = cgx_status,
    .wait_mask = cgx_wait_mask,
    .toggle_fullscreen = cgx_toggle_fullscreen
    /* supports_indexed/show_indexed/supports_yuv_indexed left NULL - RTG
     * backends don't implement the AGA-only indexed fast paths. */
};
