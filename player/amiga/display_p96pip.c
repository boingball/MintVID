/*
 * MintVID - Picasso96 PIP (Picture-In-Picture) overlay backend.
 *
 * A customer report on real hardware (68060/66MHz + Mediator + Voodoo3, per
 * CLAUDE.md's git history) asked specifically about the "planned overlay
 * display" mode this project's own docs had mentioned - the idea being that
 * a real hardware video overlay (as Voodoo3/Permedia/BVision-class boards
 * expose) can do colourspace conversion and scaling in the graphics card
 * instead of the CPU, which matters most on exactly this kind of slow-68k +
 * fast-RTG-board combination.
 *
 * Picasso96API.library's "PIP" API (p96PIP_OpenTagList() et al, declared in
 * libraries/Picasso96.h/inline/Picasso96API.h - the real vendored NDK headers
 * checked into player/amiga/include/) is the mechanism: it opens what is,
 * from the caller's point of view, an ordinary Intuition window (the same
 * WA_* tags OpenWindowTags() takes are accepted alongside it), but backed by
 * a dedicated video surface separate from that window's own screen bitmap.
 * P96PIP_Type selects PIPT_VideoWindow (a real hardware overlay window on
 * boards that support one) or PIPT_MemoryWindow (the default, always-
 * available software-composited fallback); this backend requests
 * PIPT_VideoWindow first and falls back to PIPT_MemoryWindow only if that
 * specific open fails, so selecting overlay mode never loses playback, only
 * the chance at real hardware acceleration - same fallback discipline as
 * every other backend in display_backend.h's chain.
 *
 * Two real, structural differences from display_p96.c's own direct-lock
 * backend (backend_p96), both worth calling out explicitly:
 *
 *  1. Not fullscreen-only. backend_p96 must refuse windowed mode because it
 *     writes raw, absolute screen coordinates straight into the *shared*
 *     screen bitmap with none of WritePixelArray's automatic window
 *     clipping - a windowed video window dragged around a normal desktop
 *     corrupts whatever else is on screen (see display_p96.c's file header).
 *     A PIP owns its own dedicated surface; writes here never touch any
 *     other window's pixels, so there is no equivalent hazard, and this
 *     backend is happy to open windowed exactly like backend_cgx does.
 *
 *  2. No CPU-side scaling. backend_p96/backend_cgx both resample RGB24 into
 *     a scale strip on the CPU (mr_scale_resize_rgb24_strip()) whenever the
 *     window isn't the stream's native size. A PIP is told its source size
 *     (P96PIP_SourceWidth/Height, fixed for the PIP's lifetime) and a
 *     separate destination rectangle (P96PIP_Width/Height/Left/Top, the
 *     video's aspect-fitted placement within the window) - Picasso96 (and,
 *     for a real PIPT_VideoWindow, potentially the board itself) does the
 *     scaling. This backend always writes native-resolution pixels into the
 *     PIP's own source bitmap and never allocates or fills a scale buffer.
 *
 * What is NOT yet done here, deliberately, to keep this a reviewable first
 * step rather than a speculative rewrite of the decode pipeline: the source
 * format requested is RGBFB_B8G8R8 (reusing the exact same BGR24 pixels
 * backend_p96's show_bgr() already consumes), not one of the YUV RGBFTYPEs
 * libraries/Picasso96.h documents as "for use with a hardware window only".
 * That comment is the strongest hint in the vendored headers that a real
 * hardware overlay engine expects YUV, not RGB - so whether requesting
 * PIPT_VideoWindow with an RGB source actually engages a Voodoo3's overlay
 * hardware, or Picasso96 quietly falls back to software compositing behind
 * an identical-looking API, is unknown without a real board to test against.
 * Feeding real packed YUV into a video-window PIP (skipping the H.264/MPEG-2
 * YUV->RGB24 conversion these decoders already pay for, the same win
 * mr_mpeg2_set_yuv_output() gets for the AGA indexed path - see CLAUDE.md's
 * "The RGB24 round-trip is the expensive part" note) is the natural, larger
 * follow-up once this base mechanism is confirmed working on real hardware;
 * this first step validates the open/write/resize/close mechanics end to
 * end using the same pixel data every other RTG backend already proves
 * correct against ffmpeg.
 *
 * There is no AmigaOS toolchain, real Voodoo3/overlay-capable board, or P96
 * PIP implementation to test against on this dev host (see CLAUDE.md's
 * "Validate against ffmpeg" section) - not even WinUAE's own P96/UAEGFX
 * emulation is known to implement the PIP API at all, let alone real
 * hardware overlay. This file can only be reviewed, not compiled or run,
 * until a real test happens - the same standing limitation as every other
 * Amiga-only file in this tree.
 */
#include "amiga_display.h"
#include "display_backend.h"
#include "mr_aspect.h"

#include <stddef.h>
#include <exec/types.h>
#include <exec/memory.h>
#include <intuition/intuition.h>
#include <intuition/screens.h>
#include <graphics/gfx.h>
#include <graphics/rastport.h>
#include <libraries/Picasso96.h>
#include <inline/Picasso96API.h>

#include <proto/exec.h>
#include <proto/intuition.h>
#include <proto/graphics.h>
#include <string.h>
#include <time.h>
#include <stdio.h>

typedef struct {
    struct Window *win;             /* the PIP's own window (p96PIP_OpenTags) */
    struct BitMap *source_bitmap;   /* P96PIP_SourceBitMap - what we write to */
    int            source_w, source_h;
    int            bl, bt;          /* border offset (0,0 when borderless)    */
    int            win_w, win_h;    /* current window CONTENT (inner) size -
                                     * before the first open_pip() call this
                                     * instead holds the requested size (see
                                     * sync_content_geometry()) */
    int            dx, dy, dw, dh;  /* aspect-fitted PIP rect within the window */
    int            fullscreen;
    int            hw_overlay;      /* 1 = PIPT_VideoWindow, 0 = MemoryWindow */
    int            pending_w, pending_h;
    clock_t        resize_at;
    int            geometry_valid;
    int            force_full_redraw;
    mr_display_timing timing;
    int            quit;
    int            have_window_geometry;
    int            window_left, window_top, window_width, window_height;
    char           title[80];
} p96pip_state;

static struct Window *open_pip(p96pip_state *s, ULONG type, LONG *err);
static void close_pip(p96pip_state *s);
static void sync_content_geometry(p96pip_state *s);
static void paint_letterbox(p96pip_state *s);
static void rebuild_geometry(p96pip_state *s, const char *reason);
static int  p96pip_toggle_fullscreen(void *h);

/* Fit a w*h picture inside a bounding box, preserving aspect ratio - only
 * ever shrinks, matching display_cgx.c's/display_p96.c's identical helper
 * (each backend keeps its own small copy rather than sharing one). */
static void fit_within(int w, int h, int max_w, int max_h, int *out_w,
                       int *out_h)
{
    *out_w = w;
    *out_h = h;
    if (w <= 0 || h <= 0 || max_w <= 0 || max_h <= 0)
        return;
    if (w <= max_w && h <= max_h)
        return;
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

static void calculate_geometry(p96pip_state *s)
{
    mr_aspect_rect fit =
        mr_aspect_fit(s->source_w, s->source_h, s->win_w, s->win_h);
    s->dx = fit.x;
    s->dy = fit.y;
    s->dw = fit.w;
    s->dh = fit.h;
}

/*
 * Opens the PIP itself. WA_* tags place/size the window exactly like
 * OpenWindowTags(); the P96PIP_* tags describe the source buffer (fixed at
 * s->source_w/h for the PIP's lifetime) and its aspect-fitted destination
 * rectangle within that window (s->dx/dy/dw/dh, already computed by the
 * caller). These four geometry tags are init-only in the P96 API, so every
 * size change closes and reopens the PIP rather than trying to update them
 * with p96PIP_SetTags().
 *
 * P96PIP_Relativity is explicitly cleared to 0: the *default*
 * (PIPRel_Width|PIPRel_Height, per libraries/Picasso96.h) interprets
 * P96PIP_Width/Height as a margin *not* covered by the PIP at the window's
 * right/bottom edge, not an absolute size - easy to miss, since
 * P96PIP_Width's own doc comment ("default: inner width of window") reads
 * as if it were already an absolute value. Getting this wrong would not
 * fail to open; it would silently place the video at the wrong size, so it
 * is called out here rather than left to be rediscovered on real hardware.
 */
static struct Window *open_pip(p96pip_state *s, ULONG type, LONG *err)
{
    struct Screen *scr;
    struct Window *win;
    ULONG flags, idcmp;
    int left = 0, top = 0;

    scr = LockPubScreen(NULL);
    if (!scr) return NULL;

    if (s->fullscreen) {
        flags = WFLG_BORDERLESS | WFLG_BACKDROP | WFLG_ACTIVATE |
                WFLG_RMBTRAP | WFLG_NOCAREREFRESH;
        idcmp = IDCMP_CLOSEWINDOW | IDCMP_RAWKEY;
    } else {
        flags = WFLG_DRAGBAR | WFLG_DEPTHGADGET | WFLG_CLOSEGADGET |
                WFLG_SIZEGADGET | WFLG_ACTIVATE | WFLG_NOCAREREFRESH;
        idcmp = IDCMP_CLOSEWINDOW | IDCMP_RAWKEY | IDCMP_NEWSIZE;
        left = s->have_window_geometry ? s->window_left : 0;
        top  = s->have_window_geometry ? s->window_top  : 0;
    }

    /* P96's PIP API explicitly ignores WA_Width/WA_Height and requires
     * WA_InnerWidth/WA_InnerHeight. Use the inner-size tags for both modes;
     * fullscreen is borderless, so its inner and outer dimensions are equal.
     * This also keeps s->win_w/win_h in content-area units everywhere. */
    *err = 0;
    win = (struct Window *)p96PIP_OpenTags(
        WA_PubScreen, (ULONG)scr,
        WA_Title, (ULONG)s->title,
        WA_Left, (ULONG)left, WA_Top, (ULONG)top,
        WA_InnerWidth, (ULONG)s->win_w,
        WA_InnerHeight, (ULONG)s->win_h,
        WA_Flags, flags,
        WA_IDCMP, idcmp,
        P96PIP_SourceFormat, (ULONG)RGBFB_B8G8R8,
        P96PIP_SourceWidth, (ULONG)s->source_w,
        P96PIP_SourceHeight, (ULONG)s->source_h,
        P96PIP_Type, type,
        /* The PIP rectangle is relative to the window's interior, not its
         * outer RastPort coordinates. Do not add BorderLeft/BorderTop here:
         * doing so shifts a full-size PIP outside the interior and can make
         * an otherwise valid open look cropped to the driver. */
        P96PIP_Relativity, (ULONG)0,
        P96PIP_Left, (ULONG)s->dx, P96PIP_Top, (ULONG)s->dy,
        P96PIP_Width, (ULONG)s->dw, P96PIP_Height, (ULONG)s->dh,
        P96PIP_ErrorCode, (ULONG)err,
        TAG_END);

    UnlockPubScreen(NULL, scr);
    return win;
}

/* Read the window's real content (border-excluded) geometry back after
 * open/resize, mirroring display_p96.c's/display_cgx.c's own bl/bt/iw/ih
 * tracking. bl/bt are retained only for drawing the letterbox bars through
 * the window RastPort; P96PIP_Left/Top themselves are interior-relative and
 * must not include those border offsets. */
static void sync_content_geometry(p96pip_state *s)
{
    if (!s->win) return;
    s->bl = s->win->BorderLeft;
    s->bt = s->win->BorderTop;
    s->win_w = s->win->Width  - s->win->BorderLeft - s->win->BorderRight;
    s->win_h = s->win->Height - s->win->BorderTop  - s->win->BorderBottom;
    if (s->win_w < 1) s->win_w = 1;
    if (s->win_h < 1) s->win_h = 1;
}

static void close_pip(p96pip_state *s)
{
    struct IntuiMessage *msg;
    if (!s->win) return;
    ModifyIDCMP(s->win, 0);
    if (s->win->UserPort) {
        while ((msg = p96PIP_GetIMsg(s->win->UserPort)))
            p96PIP_ReplyIMsg(msg);
    }
    WaitBlit();
    p96PIP_Close(s->win);
    s->win = NULL;
    s->source_bitmap = NULL;
}

/* Black out the window area the PIP rectangle doesn't cover (letterbox /
 * pillarbox bars) - the PIP mechanism only ever draws its own dw x dh
 * rectangle at dx,dy; nothing else in this file touches the rest of the
 * window. A plain graphics.library RectFill on the window's own RastPort
 * works regardless of the underlying screen's depth/format (AGA, ECS, or
 * any RTG mode) since it goes through the normal blitter path, not a direct
 * bitmap lock. */
static void paint_letterbox(p96pip_state *s)
{
    if (!s->win || !s->win->RPort) return;
    SetAPen(s->win->RPort, 0);
    RectFill(s->win->RPort, (WORD)s->bl, (WORD)s->bt,
            (WORD)(s->bl + s->win_w - 1), (WORD)(s->bt + s->win_h - 1));
}

static void rebuild_geometry(p96pip_state *s, const char *reason)
{
    calculate_geometry(s);
    s->geometry_valid = 1;
    s->force_full_redraw = 1;
    paint_letterbox(s);
    if (g_display_want_time)
        printf("p96pip-geometry reason=%s win=%dx%d video=%d,%d %dx%d "
               "hw-overlay=%d\n",
               reason ? reason : "?", s->win_w, s->win_h,
               s->dx, s->dy, s->dw, s->dh, s->hw_overlay);
}

/* Close and reopen using geometry already stored in s. P96PIP_Source* and
 * P96PIP_{Left,Top,Width,Height} are init-only, and a hardware overlay may
 * allow only one live video window. Close the old PIP first so it cannot
 * make its own replacement look unavailable. */
static int reopen_pip(p96pip_state *s, const char *reason)
{
    LONG err = 0;

    close_pip(s);
    calculate_geometry(s);

    s->win = open_pip(s, PIPT_VideoWindow, &err);
    s->hw_overlay = s->win != NULL;
    if (!s->win) {
        if (g_display_want_time)
            printf("p96pip: hardware video window unavailable (error %ld), "
                   "trying software PIP\n", (long)err);
        s->win = open_pip(s, PIPT_MemoryWindow, &err);
    }
    if (!s->win) {
        if (g_display_want_time)
            printf("p96pip: PIP open failed (error %ld)\n", (long)err);
        return 0;
    }

    /* p96PIP_GetTagList() returns a count, not a success boolean. Check the
     * retrieved pointer itself so either convention remains harmless. */
    p96PIP_GetTags(s->win, P96PIP_SourceBitMap, (ULONG)&s->source_bitmap,
                   TAG_END);
    if (!s->source_bitmap) {
        if (g_display_want_time)
            printf("p96pip: could not retrieve source bitmap - closing\n");
        close_pip(s);
        return 0;
    }

    sync_content_geometry(s);
    s->pending_w = s->win_w;
    s->pending_h = s->win_h;
    rebuild_geometry(s, reason);
    return 1;
}

static void *p96pip_open(int w, int h, const char *title)
{
    p96pip_state *s;
    struct Screen *scr;
    int screen_w = 0, screen_h = 0;

    if (!P96Base) return NULL;

    s = (p96pip_state *)AllocVec(sizeof *s, MEMF_CLEAR);
    if (!s) return NULL;

    s->source_w = w;
    s->source_h = h;
    s->fullscreen = g_display_fullscreen;
    snprintf(s->title, sizeof s->title, "%s",
            (title && *title) ? title : "MintVID");

    scr = LockPubScreen(NULL);
    if (scr) {
        screen_w = scr->Width;
        screen_h = scr->Height;
        UnlockPubScreen(NULL, scr);
    }
    if (screen_w < 160) screen_w = 640;
    if (screen_h < 100) screen_h = 480;

    if (s->fullscreen) {
        s->win_w = screen_w;
        s->win_h = screen_h;
    } else {
        int avail_w = screen_w, avail_h = screen_h;
        fit_within(w, h, avail_w, avail_h, &s->win_w, &s->win_h);
    }

    if (!reopen_pip(s, "init")) {
        FreeVec(s);
        return NULL;
    }

    if (g_display_want_time)
        printf("p96pip: opened %s overlay, window=%dx%d source=%dx%d\n",
               s->hw_overlay ? "hardware (PIPT_VideoWindow)" :
                               "software (PIPT_MemoryWindow)",
               s->win_w, s->win_h, w, h);

    if (!s->fullscreen) {
        s->have_window_geometry = 1;
        s->window_left = s->win->LeftEdge;
        s->window_top = s->win->TopEdge;
        /* Content size, not outer Width/Height - open_pip()'s windowed
         * branch requests WA_InnerWidth/Height from s->win_w/win_h
         * directly, so what is saved here must be in the same units. */
        s->window_width = s->win_w;
        s->window_height = s->win_h;
    }

    return s;
}

/* Close and reopen the PIP at a new source resolution. P96PIP_SourceWidth/
 * SourceHeight are documented "(I)" (Init-only, matching every other
 * P96PIP_Source* tag) - there is no settable equivalent, so a live source
 * size change (e.g. an HLS live stream's SPS changing resolution mid-
 * segment, same case display_p96.c's p96_show_packed() handles for its own
 * screen-bitmap path) needs a full close/reopen rather than a SetTags
 * update. Keeps the current window geometry/fullscreen state; only the
 * source dimensions and the resulting aspect-fit change. If the new size
 * is refused, the previous PIP is restored instead of leaving the backend
 * with no live window. */
static int reopen_for_size(p96pip_state *s, int w, int h)
{
    int old_w = s->source_w;
    int old_h = s->source_h;

    s->source_w = w;
    s->source_h = h;
    if (reopen_pip(s, "frame-size-change"))
        return 1;

    /* Keep the backend usable if a transient/invalid source size is refused. */
    s->source_w = old_w;
    s->source_h = old_h;
    if (!reopen_pip(s, "frame-size-rollback"))
        s->quit = 1;
    return 0;
}

static unsigned long elapsed_us(clock_t begin)
{
    return (unsigned long)((clock() - begin) * 1000000UL / CLOCKS_PER_SEC);
}

/* Lock the PIP's own source bitmap (p96LockBitMap/p96UnlockBitMap - the
 * same generic Picasso96API.library calls display_p96.c uses on the shared
 * screen bitmap) and write `rows` starting at row 0 of that bitmap - there
 * is no destination offset to track the way display_p96.c's write_pixel_
 * strip() needs one: the PIP's source bitmap is always exactly source_w x
 * source_h, and its placement/scaling within the window is handled entirely
 * by P96PIP_Left/Top/Width/Height (set by open_pip()), not by us. Only
 * RGBFB_B8G8R8 is ever requested (see the file header for why), so unlike
 * display_p96.c/display_cgx.c's multi-format write_pixel_strip() this needs
 * no format switch - just an optional R/B channel swap depending on the
 * caller's own source order. */
static int write_source_rows(struct BitMap *bm, int y0,
                             const unsigned char *src, int src_stride,
                             int w, int rows, int src_is_bgr)
{
    struct RenderInfo ri;
    LONG lock;
    int y, bpr;
    unsigned char *base;

    lock = p96LockBitMap(bm, (UBYTE *)&ri, sizeof ri);
    if (!lock) return 0;
    bpr = (int)ri.BytesPerRow;
    base = (unsigned char *)ri.Memory + (size_t)y0 * (size_t)bpr;
    for (y = 0; y < rows; y++) {
        const unsigned char *srow = src + (size_t)y * (size_t)src_stride;
        unsigned char *drow = base + (size_t)y * (size_t)bpr;
        if (src_is_bgr) {
            memcpy(drow, srow, (size_t)w * 3u);
        } else {
            int x;
            for (x = 0; x < w; x++) {
                drow[x * 3 + 0] = srow[x * 3 + 2];
                drow[x * 3 + 1] = srow[x * 3 + 1];
                drow[x * 3 + 2] = srow[x * 3 + 0];
            }
        }
    }
    p96UnlockBitMap(bm, lock);
    return 1;
}

static void p96pip_show_packed(void *h, const unsigned char *rgb, int w,
                               int hh, int stride, int dy0, int dy1,
                               int src_is_bgr, mr_display_service_fn service,
                               void *service_opaque)
{
    p96pip_state *s = (p96pip_state *)h;
    clock_t total = 0;
    int timing;
    if (!s || !s->win) return;

    if (w > 0 && hh > 0 && (w != s->source_w || hh != s->source_h)) {
        if (g_display_want_time)
            printf("p96pip-source-size metadata=%dx%d frame=%dx%d; "
                   "reopening PIP\n", s->source_w, s->source_h, w, hh);
        if (!reopen_for_size(s, w, hh)) return;
    }

    timing = g_display_want_time;
    if (timing) { memset(&s->timing, 0, sizeof s->timing); total = clock(); }

    if (s->force_full_redraw) { dy0 = 0; dy1 = hh; s->force_full_redraw = 0; }
    if (dy0 < 0) dy0 = 0;
    if (dy1 > hh) dy1 = hh;
    if (dy1 <= dy0) return;

    if (!write_source_rows(s->source_bitmap, dy0, rgb + (size_t)dy0 * stride,
                           stride, w, dy1 - dy0, src_is_bgr))
        printf("p96pip-error: p96LockBitMap failed - dropped strip\n");
    if (service) service(service_opaque);

    if (timing) {
        s->timing.src_w = w; s->timing.src_h = hh;
        s->timing.dst_w = s->dw; s->timing.dst_h = s->dh;
        s->timing.src_format = src_is_bgr ? "BGR24" : "RGB24";
        s->timing.dst_format = "BGR24 (PIP source)";
        s->timing.pixels = (unsigned long)w * (unsigned long)(dy1 - dy0);
        s->timing.bytes = (unsigned long)stride * (unsigned long)(dy1 - dy0);
        s->timing.blit_us = s->timing.total_us = elapsed_us(total);
    }
}

static void p96pip_show(void *h, const unsigned char *rgb, int w, int hh,
                        int stride, int dy0, int dy1,
                        mr_display_service_fn service, void *service_opaque)
{
    p96pip_show_packed(h, rgb, w, hh, stride, dy0, dy1, 0,
                       service, service_opaque);
}

static void p96pip_show_bgr(void *h, const unsigned char *bgr, int w, int hh,
                            int stride, int dy0, int dy1,
                            mr_display_service_fn service, void *service_opaque)
{
    p96pip_show_packed(h, bgr, w, hh, stride, dy0, dy1, 1,
                       service, service_opaque);
}

static int p96pip_timing(void *h, mr_display_timing *timing)
{
    p96pip_state *s = (p96pip_state *)h;
    if (!s || !timing) return 0;
    *timing = s->timing; return 1;
}

static int p96pip_poll(void *h)
{
    p96pip_state *s = (p96pip_state *)h;
    struct IntuiMessage *msg;
    int ev = MR_EV_NONE;
    if (!s || !s->win) return MR_EV_QUIT;
    while ((msg = p96PIP_GetIMsg(s->win->UserPort))) {
        ULONG cls = msg->Class; UWORD code = msg->Code;
        p96PIP_ReplyIMsg(msg);
        if (cls == IDCMP_CLOSEWINDOW) s->quit = 1;
        else if (cls == IDCMP_NEWSIZE) {
            /* Content size (border-excluded), matching what s->win_w/win_h
             * mean everywhere else in this file - not the raw outer
             * Width/Height. */
            int cw = s->win->Width  - s->win->BorderLeft - s->win->BorderRight;
            int ch = s->win->Height - s->win->BorderTop  - s->win->BorderBottom;
            s->pending_w = cw < 1 ? 1 : cw;
            s->pending_h = ch < 1 ? 1 : ch;
            s->resize_at = clock();
        }
        else if (cls == IDCMP_RAWKEY && !(code & 0x80)) {
            switch (code) {
            case 0x45: s->quit = 1; break;
            case 0x40: ev = MR_EV_PAUSE; break;
            case 0x23: p96pip_toggle_fullscreen(s); break;
            case 0x4E: ev = MR_EV_SEEK_FWD; break;
            case 0x4F: ev = MR_EV_SEEK_BACK; break;
            case 0x4C: ev = MR_EV_VOLUME_UP; break;
            case 0x4D: ev = MR_EV_VOLUME_DOWN; break;
            }
        }
    }
    if (s->pending_w != s->win_w || s->pending_h != s->win_h) {
        if (clock() - s->resize_at >= CLOCKS_PER_SEC / 10) {
            int old_w = s->win_w, old_h = s->win_h;
            int old_saved_w = s->window_width;
            int old_saved_h = s->window_height;

            /* Preserve the user's current position before closing the old
             * PIP, then reopen because its rectangle tags are init-only. */
            s->have_window_geometry = 1;
            s->window_left = s->win->LeftEdge;
            s->window_top = s->win->TopEdge;
            s->win_w = s->pending_w;
            s->win_h = s->pending_h;
            s->window_width = s->win_w;
            s->window_height = s->win_h;
            if (!reopen_pip(s, "resize")) {
                s->win_w = old_w;
                s->win_h = old_h;
                s->window_width = old_saved_w;
                s->window_height = old_saved_h;
                if (!reopen_pip(s, "resize-rollback"))
                    s->quit = 1;
            }
        }
    }
    return s->quit ? MR_EV_QUIT : ev;
}

static int p96pip_toggle_fullscreen(void *h)
{
    p96pip_state *s = (p96pip_state *)h;
    int next_fullscreen;
    int old_fullscreen, old_win_w, old_win_h;
    if (!s || !s->win) return 0;
    if (!s->fullscreen) {
        s->have_window_geometry = 1;
        s->window_left = s->win->LeftEdge;
        s->window_top = s->win->TopEdge;
        /* Content size, same units as open_pip()'s WA_InnerWidth/Height
         * request - s->win_w/win_h already hold it, kept current by
         * sync_content_geometry() after every open/resize. */
        s->window_width = s->win_w;
        s->window_height = s->win_h;
    }
    old_fullscreen = s->fullscreen;
    old_win_w = s->win_w;
    old_win_h = s->win_h;
    next_fullscreen = !s->fullscreen;
    s->fullscreen = next_fullscreen;
    if (s->fullscreen) {
        struct Screen *scr = LockPubScreen(NULL);
        if (scr) { s->win_w = scr->Width; s->win_h = scr->Height; UnlockPubScreen(NULL, scr); }
    } else if (s->have_window_geometry) {
        s->win_w = s->window_width;
        s->win_h = s->window_height;
    }
    if (reopen_pip(s, "fullscreen-toggle"))
        return 1;

    /* Restore the previous mode if the requested PIP cannot be opened. */
    s->fullscreen = old_fullscreen;
    s->win_w = old_win_w;
    s->win_h = old_win_h;
    if (!reopen_pip(s, "fullscreen-rollback"))
        s->quit = 1;
    return 0;
}

static void p96pip_status(void *h, const char *text)
{
    p96pip_state *s = (p96pip_state *)h;
    const char *want = (text && *text) ? text : "MintVID";
    if (!s || !s->win) return;
    if (strcmp(s->title, want) == 0) return;
    snprintf(s->title, sizeof s->title, "%s", want);
    SetWindowTitles(s->win, (CONST_STRPTR)s->title, (CONST_STRPTR)~0UL);
}

static void p96pip_close(void *h)
{
    p96pip_state *s = (p96pip_state *)h;
    if (!s) return;
    close_pip(s);
    FreeVec(s);
}

static ULONG p96pip_wait_mask(void *h)
{
    p96pip_state *s = (p96pip_state *)h;
    if (!s || !s->win || !s->win->UserPort) return 0;
    return 1UL << s->win->UserPort->mp_SigBit;
}

const display_backend backend_p96pip = {
    .name = "RTG (P96 Overlay)",
    .open = p96pip_open,
    .show = p96pip_show,
    .show_bgr = p96pip_show_bgr,
    .timing = p96pip_timing,
    .poll = p96pip_poll,
    .close = p96pip_close,
    .status = p96pip_status,
    .wait_mask = p96pip_wait_mask,
    .toggle_fullscreen = p96pip_toggle_fullscreen
    /* supports_indexed/show_indexed/supports_yuv_indexed left NULL - RTG
     * backends don't implement the AGA-only indexed fast paths. */
};
