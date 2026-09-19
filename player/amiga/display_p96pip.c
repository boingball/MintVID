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
 * boards that support one) or PIPT_MemoryWindow (the original/default PIP
 * type used by RiVA on P96 2.x). This backend requests PIPT_VideoWindow first and
 * tries the older PIPT_MemoryWindow form used by RiVA if that specific open
 * fails; drivers which expose no PIP support can reject both, in which case
 * display_backend.h's normal fallback chain selects direct P96 or CGX.
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
 * The source format is RGBFB_Y4U2V2 (packed Y,chroma,Y,chroma pairs),
 * matching RiVA's known-working P96 2.x PIP path. Older Voodoo drivers
 * reject the earlier RGBFB_B8G8R8 request with PIPERR_NOTAVAILABLE before a
 * window is opened. H.264 and MPEG-2 therefore stay YUV from decoder to
 * overlay and only repack 4:2:0 to 4:2:2; RGB-only codecs retain a
 * correctness fallback which converts their rows while copying them into
 * the PIP surface. Real hardware also showed the chroma *order* the format
 * name implies (Y0,U0,Y1,V0) to be backwards from what the driver actually
 * consumes - see core/mr_yuv.c's mr_yuv420_to_y4u2v2() and this file's own
 * write_rgb_rows() for the swapped V-then-U convention both this backend's
 * producers now use.
 *
 * A second real-hardware finding: on at least one Voodoo3/P96 2.1 setup,
 * neither PIP type can be opened fullscreen on the public screen at all -
 * p96PIP_OpenTags() returns PIPERR_NOTAVAILABLE for PIPT_VideoWindow and
 * PIPERR_CROPPED for PIPT_MemoryWindow, even though the same source/dest
 * geometry opens fine windowed. p96pip_toggle_fullscreen() retries once
 * with a full-window (non-letterboxed) destination rectangle on CROPPED
 * before giving up, and reports 2 (not just 0) when entering fullscreen
 * still fails after that retry - display.c's switch_to_cgx_fallback()
 * treats that as "give up on PIP fullscreen for this session, fall back to
 * ordinary CGX fullscreen" rather than leaving repeated F presses restore
 * the same working-but-windowed PIP forever with no way to actually reach
 * fullscreen. See display.c for how the fallback keeps this backend's own
 * YUV422 producers working unmodified through a CGX/AGA backend that has
 * no show_yuv422 of its own.
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
#include <graphics/displayinfo.h>
#include <graphics/rastport.h>
#include <libraries/Picasso96.h>
#include <inline/Picasso96API.h>

#include <proto/exec.h>
#include <proto/intuition.h>
#include <proto/graphics.h>
#include <proto/dos.h>
#include <string.h>
#include <time.h>
#include <stdio.h>

typedef struct {
    struct Window *win;             /* the PIP's own window (p96PIP_OpenTags) */
    struct BitMap *source_bitmap;   /* P96PIP_SourceBitMap - what we write to */
    struct Screen *screen;          /* private video-compatible fullscreen mode */
    ULONG          screen_mode_id;
    int            source_w, source_h;
    int            bl, bt;          /* border offset (0,0 when borderless)    */
    int            win_w, win_h;    /* current window CONTENT (inner) size -
                                     * before the first open_pip() call this
                                     * instead holds the requested size (see
                                     * sync_content_geometry()) */
    int            dx, dy, dw, dh;  /* aspect-fitted PIP rect within the window */
    int            fullscreen;
    int            hw_overlay;      /* 1 = PIPT_VideoWindow, 0 = MemoryWindow */
    int            force_full_dest; /* 1 = calculate_geometry() skips aspect
                                     * fit and fills the whole window - the
                                     * one-shot PIPERR_CROPPED retry, see
                                     * p96pip_toggle_fullscreen() */
    LONG           last_hw_err, last_mem_err; /* most recent open_pip() error
                                               * per PIP type, for the CROPPED
                                               * retry and diagnostics */
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
static int close_video_screen(struct Screen **screen, const char *reason);
static struct Screen *open_video_screen(p96pip_state *s, int target_w,
                                        int target_h);
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
    if (s->force_full_dest) {
        /* One-shot PIPERR_CROPPED retry (see p96pip_toggle_fullscreen()):
         * fill the whole window instead of the normal aspect-fitted
         * rectangle, in case the driver's crop check is tripping on the
         * letterboxed inset rather than on anything about the window or
         * screen bounds themselves. */
        s->dx = 0;
        s->dy = 0;
        s->dw = s->win_w;
        s->dh = s->win_h;
        return;
    }
    {
        mr_aspect_rect fit =
            mr_aspect_fit(s->source_w, s->source_h, s->win_w, s->win_h);
        s->dx = fit.x;
        s->dy = fit.y;
        s->dw = fit.w;
        s->dh = fit.h;
    }
}

static const char *pip_err_name(LONG err)
{
    switch (err) {
    case PIPERR_NOMEMORY:     return "PIPERR_NOMEMORY";
    case PIPERR_ATTACHFAIL:   return "PIPERR_ATTACHFAIL";
    case PIPERR_NOTAVAILABLE: return "PIPERR_NOTAVAILABLE";
    case PIPERR_OUTOFPENS:    return "PIPERR_OUTOFPENS";
    case PIPERR_BADDIMENSIONS:return "PIPERR_BADDIMENSIONS";
    case PIPERR_NOWINDOW:     return "PIPERR_NOWINDOW";
    case PIPERR_BADALIGNMENT: return "PIPERR_BADALIGNMENT";
    case PIPERR_CROPPED:      return "PIPERR_CROPPED";
    default:                  return "unknown";
    }
}

static void log_screen_target(const char *label, struct Screen *scr,
                              ULONG mode_id)
{
    struct BitMap *bm;
    ULONG depth, format, isp96, onboard, video_compatible;

    if (!g_display_want_time || !scr) return;
    bm = scr->RastPort.BitMap;
    depth = p96GetBitMapAttr(bm, P96BMA_DEPTH);
    format = p96GetBitMapAttr(bm, P96BMA_RGBFORMAT);
    isp96 = p96GetBitMapAttr(bm, P96BMA_ISP96);
    onboard = p96GetBitMapAttr(bm, P96BMA_ISONBOARD);
    video_compatible =
        mode_id != (ULONG)INVALID_ID
            ? p96GetModeIDAttr(mode_id, P96IDA_VIDEOCOMPATIBLE)
            : 0;

    printf("p96pip-screen target=%s mode=0x%08lx size=%dx%d depth=%lu "
           "format=%lu isp96=%lu onboard=%lu video-compatible=%lu\n",
           label ? label : "?", (unsigned long)mode_id,
           scr->Width, scr->Height, (unsigned long)depth,
           (unsigned long)format, (unsigned long)isp96,
           (unsigned long)onboard, (unsigned long)video_compatible);
    /* This whole backend is new and, per the file header, not even known to
     * be exercised by WinUAE's own P96/UAEGFX emulation - flush every
     * diagnostic line the moment it's printed (not just at the well-behaved
     * end of a call) so a real-hardware/WinUAE crash mid-open leaves a log
     * that shows exactly which stage was last reached. */
    Flush(Output());
}

static int close_video_screen(struct Screen **screen, const char *reason)
{
    int attempt;
    if (!screen || !*screen) return 1;

    WaitBlit();
    for (attempt = 0; attempt < 50; attempt++) {
        if (p96CloseScreen(*screen)) {
            *screen = NULL;
            if (g_display_want_time && attempt > 0) {
                printf("p96pip-screen: private screen closed after %d "
                       "VBlank(s) (%s)\n", attempt,
                       reason ? reason : "unknown");
                Flush(Output());
            }
            return 1;
        }
        WaitTOF();
    }
    printf("p96pip-screen: WARNING private screen still busy after "
           "50 VBlanks (%s)\n", reason ? reason : "unknown");
    Flush(Output());
    return 0;
}

/*
 * A PIP attaches to a screen; putting a borderless window on the current
 * public screen does not make that screen capable of hosting a hardware
 * video window. Ask Picasso96 for a mode explicitly marked VideoCompatible,
 * trying the public screen's depth first and then the common RTG depths.
 */
static struct Screen *open_video_screen(p96pip_state *s, int target_w,
                                        int target_h)
{
    static const ULONG fallback_depths[] = { 16, 24, 32 };
    ULONG depths[4], preferred_depth = 16;
    struct Screen *pub, *scr;
    ULONG mode_id, video_compatible;
    LONG err;
    int count = 0, i, j;

    pub = LockPubScreen(NULL);
    if (pub) {
        ULONG d = p96GetBitMapAttr(pub->RastPort.BitMap, P96BMA_DEPTH);
        if (d == 15 || d == 16 || d == 24 || d == 32)
            preferred_depth = d;
        UnlockPubScreen(NULL, pub);
    }

    depths[count++] = preferred_depth;
    for (i = 0; i < 3; i++) {
        for (j = 0; j < count; j++)
            if (depths[j] == fallback_depths[i])
                break;
        if (j == count)
            depths[count++] = fallback_depths[i];
    }

    for (i = 0; i < count; i++) {
        mode_id = p96BestModeIDTags(
            P96BIDTAG_NominalWidth, (ULONG)target_w,
            P96BIDTAG_NominalHeight, (ULONG)target_h,
            P96BIDTAG_Depth, depths[i],
            P96BIDTAG_VideoCompatible, TRUE,
            TAG_END);
        if (mode_id == (ULONG)INVALID_ID) {
            if (g_display_want_time) {
                printf("p96pip-screen: no video-compatible mode near "
                       "%dx%d depth=%lu\n", target_w, target_h,
                       (unsigned long)depths[i]);
                Flush(Output());
            }
            continue;
        }

        video_compatible =
            p96GetModeIDAttr(mode_id, P96IDA_VIDEOCOMPATIBLE);
        if (!video_compatible) {
            if (g_display_want_time) {
                printf("p96pip-screen: rejected mode=0x%08lx depth=%lu; "
                       "driver did not mark it video-compatible\n",
                       (unsigned long)mode_id, (unsigned long)depths[i]);
                Flush(Output());
            }
            continue;
        }

        if (g_display_want_time) {
            printf("p96pip-screen: trying p96OpenScreenTags mode=0x%08lx "
                   "depth=%lu\n", (unsigned long)mode_id,
                   (unsigned long)depths[i]);
            Flush(Output());
        }
        err = 0;
        scr = p96OpenScreenTags(
            P96SA_DisplayID, mode_id,
            P96SA_Type, CUSTOMSCREEN,
            P96SA_Title, (ULONG)s->title,
            P96SA_Quiet, TRUE,
            P96SA_ShowTitle, FALSE,
            P96SA_ErrorCode, (ULONG)&err,
            TAG_END);
        if (!scr) {
            if (g_display_want_time) {
                printf("p96pip-screen: open failed mode=0x%08lx depth=%lu "
                       "error=%ld\n", (unsigned long)mode_id,
                       (unsigned long)depths[i], (long)err);
                Flush(Output());
            }
            continue;
        }

        s->screen_mode_id = mode_id;
        log_screen_target("private-video-compatible", scr, mode_id);
        return scr;
    }

    s->screen_mode_id = (ULONG)INVALID_ID;
    if (g_display_want_time) {
        printf("p96pip-screen: no private video-compatible P96 mode for "
               "%dx%d; trying the public screen\n", target_w, target_h);
        Flush(Output());
    }
    return NULL;
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
    ULONG flags, idcmp, screen_tag;
    int left = 0, top = 0;
    int pub_locked = 0;

    scr = s->screen;
    screen_tag = WA_CustomScreen;
    if (!scr) {
        scr = LockPubScreen(NULL);
        if (!scr) return NULL;
        pub_locked = 1;
        screen_tag = WA_PubScreen;
    }

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
    if (g_display_want_time) {
        printf("p96pip: calling p96PIP_OpenTags type=%lu win=%dx%d "
               "source=%dx%d dest=%d,%d %dx%d\n", (unsigned long)type,
               s->win_w, s->win_h, s->source_w, s->source_h,
               s->dx, s->dy, s->dw, s->dh);
        Flush(Output());
    }
    *err = 0;
    win = (struct Window *)p96PIP_OpenTags(
        screen_tag, (ULONG)scr,
        WA_Title, (ULONG)s->title,
        WA_Left, (ULONG)left, WA_Top, (ULONG)top,
        WA_InnerWidth, (ULONG)s->win_w,
        WA_InnerHeight, (ULONG)s->win_h,
        WA_Flags, flags,
        WA_IDCMP, idcmp,
        P96PIP_SourceFormat, (ULONG)RGBFB_Y4U2V2,
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

    if (g_display_want_time) {
        printf("p96pip: p96PIP_OpenTags returned win=%p err=%ld (%s)\n",
               (void *)win, (long)*err, pip_err_name(*err));
        Flush(Output());
    }
    if (pub_locked)
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
    if (g_display_want_time) {
        printf("p96pip-geometry reason=%s win=%dx%d video=%d,%d %dx%d "
               "hw-overlay=%d\n",
               reason ? reason : "?", s->win_w, s->win_h,
               s->dx, s->dy, s->dw, s->dh, s->hw_overlay);
        Flush(Output());
    }
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
    s->last_hw_err = err;
    if (!s->win) {
        if (g_display_want_time) {
            printf("p96pip: video-window PIP unavailable (error %ld: %s), "
                   "trying RiVA-compatible memory-window PIP\n",
                   (long)err, pip_err_name(err));
            Flush(Output());
        }
        s->win = open_pip(s, PIPT_MemoryWindow, &err);
        s->last_mem_err = err;
    } else {
        s->last_mem_err = 0;
    }
    if (!s->win) {
        if (g_display_want_time) {
            printf("p96pip: PIP open failed (error %ld: %s)\n",
                   (long)err, pip_err_name(err));
            Flush(Output());
        }
        return 0;
    }

    if (g_display_want_time) {
        printf("p96pip: calling p96PIP_GetTags for source bitmap\n");
        Flush(Output());
    }
    /* p96PIP_GetTagList() returns a count, not a success boolean. Check the
     * retrieved pointer itself so either convention remains harmless. */
    p96PIP_GetTags(s->win, P96PIP_SourceBitMap, (ULONG)&s->source_bitmap,
                   TAG_END);
    if (!s->source_bitmap) {
        if (g_display_want_time) {
            printf("p96pip: could not retrieve source bitmap - closing\n");
            Flush(Output());
        }
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

    if (!P96Base || w <= 0 || h <= 0 || (w & 1)) return NULL;

    if (g_display_want_time) {
        printf("p96pip: p96pip_open entered, source=%dx%d\n", w, h);
        Flush(Output());
    }

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
        log_screen_target("public", scr, GetVPModeID(&scr->ViewPort));
        UnlockPubScreen(NULL, scr);
    }
    if (screen_w < 160) screen_w = 640;
    if (screen_h < 100) screen_h = 480;

    s->screen_mode_id = (ULONG)INVALID_ID;
    if (s->fullscreen) {
        s->screen = open_video_screen(s, screen_w, screen_h);
        if (s->screen) {
            s->win_w = s->screen->Width;
            s->win_h = s->screen->Height;
        } else {
            s->win_w = screen_w;
            s->win_h = screen_h;
        }
    } else {
        int avail_w = screen_w, avail_h = screen_h;
        fit_within(w, h, avail_w, avail_h, &s->win_w, &s->win_h);
    }

    if (!reopen_pip(s, "init")) {
        /* A driver may advertise a VideoCompatible mode yet refuse PIP on
         * it. Preserve the old public-screen attempt before falling through
         * to the other display backends. */
        if (s->screen) {
            close_video_screen(&s->screen, "initial PIP fallback");
            s->screen_mode_id = (ULONG)INVALID_ID;
            s->win_w = screen_w;
            s->win_h = screen_h;
        }
        if (!reopen_pip(s, "init-public-fallback")) {
            close_video_screen(&s->screen, "initial open failure");
            FreeVec(s);
            return NULL;
        }
    }

    if (g_display_want_time) {
        printf("p96pip: opened %s, window=%dx%d source=%dx%d\n",
               s->hw_overlay ? "hardware (PIPT_VideoWindow)" :
                               "PIPT_MemoryWindow overlay",
               s->win_w, s->win_h, w, h);
        Flush(Output());
    }

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

    if (w <= 0 || h <= 0 || (w & 1)) return 0;

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
 * by P96PIP_Left/Top/Width/Height (set by open_pip()), not by us. */
static int write_yuv422_rows(struct BitMap *bm, int y0,
                             const unsigned char *src, int src_stride,
                             int w, int rows)
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
        memcpy(drow, srow, (size_t)w * 2u);
    }
    p96UnlockBitMap(bm, lock);
    return 1;
}

static unsigned char clamp_byte(int v)
{
    if (v < 0) return 0;
    if (v > 255) return 255;
    return (unsigned char)v;
}

/* Correctness fallback for RGB-only decoders. The performance path never
 * calls this: H.264/MPEG-2 queue RGBFB_Y4U2V2 directly.
 *
 * dst_pair[1]/[3] are V then U, not the U-then-V the RGBFB_Y4U2V2 name
 * implies - real hardware confirmed the driver's actual chroma order is
 * swapped from the nominal one (see this file's own header and
 * core/mr_yuv.c's mr_yuv420_to_y4u2v2(), the other producer of this same
 * buffer layout - both must agree).
 *
 * The RGB->YCbCr coefficients below are the *full*-range (PC/JPEG) matrix,
 * not the studio-range one mr_yuv.c's own mr_yuv420_to_rgb24() uses (no
 * +16 luma floor, chroma coefficients scaled for the wider 0-255 span) -
 * matching mr_yuv420_to_y4u2v2()'s own full-range rescale, for the same
 * real-hardware reason: a washed-out/pastel overlay picture next to a
 * correct WritePixel one on identical content. See core/mr_yuv.c's comment
 * on mr_yuv420_to_y4u2v2() for the full finding. */
static int write_rgb_rows(struct BitMap *bm, int y0,
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
        const unsigned char *src_pixel =
            src + (size_t)y * (size_t)src_stride;
        unsigned char *dst_pair = base + (size_t)y * (size_t)bpr;
        int x;
        for (x = 0; x < w; x += 2) {
            int r0 = src_pixel[src_is_bgr ? 2 : 0];
            int g0 = src_pixel[1];
            int b0 = src_pixel[src_is_bgr ? 0 : 2];
            int r1 = src_pixel[3 + (src_is_bgr ? 2 : 0)];
            int g1 = src_pixel[4];
            int b1 = src_pixel[3 + (src_is_bgr ? 0 : 2)];
            int r = (r0 + r1 + 1) >> 1;
            int g = (g0 + g1 + 1) >> 1;
            int b = (b0 + b1 + 1) >> 1;

            dst_pair[0] = clamp_byte(
                (77 * r0 + 150 * g0 + 29 * b0 + 128) >> 8);
            dst_pair[1] = clamp_byte(
                ((128 * r - 107 * g - 21 * b + 128) >> 8) + 128);
            dst_pair[2] = clamp_byte(
                (77 * r1 + 150 * g1 + 29 * b1 + 128) >> 8);
            dst_pair[3] = clamp_byte(
                ((-43 * r - 85 * g + 128 * b + 128) >> 8) + 128);
            src_pixel += 6;
            dst_pair += 4;
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
    if (!s || !s->win || !rgb || (w & 1)) return;

    if (w > 0 && hh > 0 && (w != s->source_w || hh != s->source_h)) {
        if (g_display_want_time) {
            printf("p96pip-source-size metadata=%dx%d frame=%dx%d; "
                   "reopening PIP\n", s->source_w, s->source_h, w, hh);
            Flush(Output());
        }
        if (!reopen_for_size(s, w, hh)) return;
    }

    timing = g_display_want_time;
    if (timing) { memset(&s->timing, 0, sizeof s->timing); total = clock(); }

    if (s->force_full_redraw) { dy0 = 0; dy1 = hh; s->force_full_redraw = 0; }
    if (dy0 < 0) dy0 = 0;
    if (dy1 > hh) dy1 = hh;
    if (dy1 <= dy0) return;

    if (!write_rgb_rows(s->source_bitmap, dy0,
                        rgb + (size_t)dy0 * (size_t)stride,
                        stride, w, dy1 - dy0, src_is_bgr)) {
        printf("p96pip-error: p96LockBitMap failed - dropped strip\n");
        Flush(Output());
    }
    if (service) service(service_opaque);

    if (timing) {
        s->timing.src_w = w; s->timing.src_h = hh;
        s->timing.dst_w = s->dw; s->timing.dst_h = s->dh;
        s->timing.src_format = src_is_bgr ? "BGR24" : "RGB24";
        s->timing.dst_format = "Y4U2V2 (P96 PIP source)";
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

static void p96pip_show_yuv422(void *h, const unsigned char *yuv, int w,
                               int hh, int stride, int dy0, int dy1,
                               mr_display_service_fn service,
                               void *service_opaque)
{
    p96pip_state *s = (p96pip_state *)h;
    clock_t total = 0;
    int timing;
    if (!s || !s->win || !yuv || (w & 1)) return;

    if (w > 0 && hh > 0 && (w != s->source_w || hh != s->source_h)) {
        if (!reopen_for_size(s, w, hh)) return;
    }
    timing = g_display_want_time;
    if (timing) { memset(&s->timing, 0, sizeof s->timing); total = clock(); }
    if (s->force_full_redraw) { dy0 = 0; dy1 = hh; s->force_full_redraw = 0; }
    if (dy0 < 0) dy0 = 0;
    if (dy1 > hh) dy1 = hh;
    if (dy1 <= dy0) return;

    if (!write_yuv422_rows(s->source_bitmap, dy0,
                           yuv + (size_t)dy0 * (size_t)stride,
                           stride, w, dy1 - dy0)) {
        printf("p96pip-error: p96LockBitMap failed - dropped YUV strip\n");
        Flush(Output());
    }
    if (service) service(service_opaque);
    if (timing) {
        s->timing.src_w = w; s->timing.src_h = hh;
        s->timing.dst_w = s->dw; s->timing.dst_h = s->dh;
        s->timing.src_format = "Y4U2V2";
        s->timing.dst_format = "Y4U2V2 (P96 PIP source)";
        s->timing.pixels = (unsigned long)w * (unsigned long)(dy1 - dy0);
        s->timing.bytes = (unsigned long)stride * (unsigned long)(dy1 - dy0);
        s->timing.blit_us = s->timing.total_us = elapsed_us(total);
    }
}

static int p96pip_supports_yuv422(void *h)
{
    p96pip_state *s = (p96pip_state *)h;
    return s && s->win && !(s->source_w & 1);
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
            case 0x23:
                /* p96pip_toggle_fullscreen() returning 2 means it gave up
                 * on PIP fullscreen and is asking display.c to fall back
                 * to CGX - surface that up through poll()'s own event
                 * return rather than swallowing it here, since this
                 * function has no access to the amiga_display wrapper
                 * (display.c's switch_to_cgx_fallback() does the actual
                 * backend swap). */
                if (p96pip_toggle_fullscreen(s) == 2)
                    ev = MR_EV_RENDERER_SWITCH;
                break;
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
    struct Screen *old_screen;
    ULONG old_mode_id;
    int old_fullscreen, old_win_w, old_win_h;
    int public_w = 640, public_h = 480;
    int entering;

    if (!s || !s->win) return 0;
    old_fullscreen = s->fullscreen;
    entering = !old_fullscreen;
    old_win_w = s->win_w;
    old_win_h = s->win_h;
    old_screen = s->screen;
    old_mode_id = s->screen_mode_id;

    if (entering) {
        struct Screen *pub;
        s->have_window_geometry = 1;
        s->window_left = s->win->LeftEdge;
        s->window_top = s->win->TopEdge;
        /* Content size, same units as open_pip()'s WA_InnerWidth/Height. */
        s->window_width = s->win_w;
        s->window_height = s->win_h;

        pub = LockPubScreen(NULL);
        if (pub) {
            public_w = pub->Width;
            public_h = pub->Height;
            UnlockPubScreen(NULL, pub);
        }
        if (g_display_want_time) {
            printf("p96pip-fullscreen: public screen reports %dx%d; "
                   "current window %dx%d\n", public_w, public_h,
                   s->win_w, s->win_h);
            Flush(Output());
        }

        s->fullscreen = 1;
        s->force_full_dest = 0;
        s->screen = open_video_screen(s, public_w, public_h);
        if (s->screen) {
            s->win_w = s->screen->Width;
            s->win_h = s->screen->Height;
        } else {
            s->win_w = public_w;
            s->win_h = public_h;
        }
        if (reopen_pip(s, "fullscreen-toggle"))
            return 1;

        /* Real Voodoo3/P96 2.1 hardware reports PIPERR_CROPPED for the
         * normal aspect-fitted (letterboxed) destination rectangle on the
         * public screen, even though the rectangle is entirely inside the
         * requested window bounds logged just above. Try once more with a
         * full-window destination before giving up on this screen size. */
        if ((s->last_hw_err == PIPERR_CROPPED ||
             s->last_mem_err == PIPERR_CROPPED) && !s->force_full_dest) {
            if (g_display_want_time) {
                printf("p96pip-fullscreen: PIPERR_CROPPED for aspect-fitted "
                       "dest %d,%d %dx%d inside window %dx%d; retrying with "
                       "a full-window destination rectangle\n",
                       s->dx, s->dy, s->dw, s->dh, s->win_w, s->win_h);
                Flush(Output());
            }
            s->force_full_dest = 1;
            if (reopen_pip(s, "fullscreen-toggle-full-dest"))
                return 1;
            s->force_full_dest = 0;
        }

        close_video_screen(&s->screen, "fullscreen open rollback");
        s->screen_mode_id = (ULONG)INVALID_ID;
    } else {
        /* Keep the private screen alive until the public-screen PIP opens,
         * so a failed toggle can restore the previous mode. reopen_pip()
         * closes the old PIP before it requests the replacement. */
        s->fullscreen = 0;
        s->force_full_dest = 0;
        s->screen = NULL;
        s->screen_mode_id = (ULONG)INVALID_ID;
        if (s->have_window_geometry) {
            s->win_w = s->window_width;
            s->win_h = s->window_height;
        } else {
            /* Started fullscreen and never had a windowed size to save -
             * s->win_w/win_h still hold the fullscreen screen dimensions
             * at this point. Falling through with those would open a
             * borderless-looking window the size of the whole display
             * instead of one sized to the video, so fit it against the
             * public screen exactly like p96pip_open()'s windowed path. */
            struct Screen *pub = LockPubScreen(NULL);
            int avail_w = 640, avail_h = 480;
            if (pub) {
                avail_w = pub->Width;
                avail_h = pub->Height;
                UnlockPubScreen(NULL, pub);
            }
            fit_within(s->source_w, s->source_h, avail_w, avail_h,
                      &s->win_w, &s->win_h);
        }
        if (reopen_pip(s, "fullscreen-toggle")) {
            close_video_screen(&old_screen, "leave fullscreen");
            return 1;
        }
    }

    /* Restore the previous mode if the requested PIP cannot be opened. */
    s->fullscreen = old_fullscreen;
    s->screen = old_screen;
    s->screen_mode_id = old_mode_id;
    s->win_w = old_win_w;
    s->win_h = old_win_h;
    s->force_full_dest = 0;
    if (!reopen_pip(s, "fullscreen-rollback")) {
        s->quit = 1;
        return 0;
    }

    /* Entering fullscreen failed even after the CROPPED geometry retry
     * above, but the previous windowed PIP is back and working. Repeating
     * the same F keypress would only hit the identical failure again with
     * no path to a working PIP fullscreen from this backend - tell the
     * caller (display.c's switch_to_cgx_fallback(), via display_poll_
     * event()/display_toggle_fullscreen()) to try ordinary CGX fullscreen
     * instead of leaving the user stuck toggling F forever. Leaving
     * fullscreen never returns this: going windowed always has a working
     * fallback (fit_within() against the public screen), so there is
     * nothing for a caller to recover from in that direction. */
    if (g_display_want_time && entering) {
        printf("p96pip-fullscreen: giving up on PIP fullscreen this "
               "session; recommending CGX fallback\n");
        Flush(Output());
    }
    return entering ? 2 : 0;
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
    close_video_screen(&s->screen, "shutdown");
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
    .show_yuv422 = p96pip_show_yuv422,
    .timing = p96pip_timing,
    .poll = p96pip_poll,
    .close = p96pip_close,
    .status = p96pip_status,
    .wait_mask = p96pip_wait_mask,
    .toggle_fullscreen = p96pip_toggle_fullscreen,
    .supports_yuv422 = p96pip_supports_yuv422
    /* supports_indexed/show_indexed/supports_yuv_indexed left NULL - RTG
     * backends don't implement the AGA-only indexed fast paths. */
};
