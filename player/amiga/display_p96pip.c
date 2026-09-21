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
 * the PIP surface. The P96 Output Format menu selects YVYU or YUYV to
 * accommodate differing driver interpretations, with both the packed-YUV
 * and RGB-only producers using the same saved choice.
 *
 * A second real-hardware finding: on at least one Voodoo3/P96 2.1 setup,
 * neither PIP type can be opened fullscreen on the public screen at all -
 * p96PIP_OpenTags() returns PIPERR_NOTAVAILABLE for PIPT_VideoWindow and
 * PIPERR_CROPPED for PIPT_MemoryWindow, even though the same source/dest
 * geometry opens fine windowed. Both startup fullscreen and
 * p96pip_toggle_fullscreen() retry progressively smaller/even-aligned
 * destination rectangles on geometry failures before giving up, and the
 * toggle reports 2 (not just 0) when entering fullscreen still fails after
 * those retries - display.c's switch_to_cgx_fallback()
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
#include "mr_p96_format.h"
#include "p96_rgb565.h"
#include "../core/mr_yuv.h"

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
    ULONG source_format;         /* actual negotiated overlay pixel format */
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
    int            fullscreen_dest_policy; /* progressive overlay-size retry */
    int            geometry_rejected; /* open failed for geometry/alignment */
    int            geometry_valid;
    int            force_full_redraw;
    mr_display_timing timing;
    int            quit;
    int            have_window_geometry;
    int            window_left, window_top, window_width, window_height;
    char           title[80];
} p96pip_state;

enum {
    P96PIP_DEST_ASPECT = 0,
    P96PIP_DEST_EVEN,
    P96PIP_DEST_VGA,
    P96PIP_DEST_NATIVE
};

static struct Window *open_pip(p96pip_state *s, ULONG type,
                               ULONG source_format, LONG *err);
static void close_pip(p96pip_state *s);
static int close_video_screen(struct Screen **screen, const char *reason);
static struct Screen *open_video_screen(p96pip_state *s, int target_w,
                                        int target_h);
static void sync_content_geometry(p96pip_state *s);
static void paint_letterbox(p96pip_state *s);
static void rebuild_geometry(p96pip_state *s, const char *reason);
static int reopen_fullscreen_pip(p96pip_state *s, const char *reason,
                                 int exhaust_sizes);
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
    if (!s->fullscreen) {
        /* Match P96PipDemo in ordinary windowed mode: the PIP fills the
         * entire inner window and its default relative rectangle follows
         * live Intuition resizing. */
        s->dx = 0;
        s->dy = 0;
        s->dw = s->win_w;
        s->dh = s->win_h;
        return;
    }
    {
        int box_w = s->win_w;
        int box_h = s->win_h;
        mr_aspect_rect fit;

        /* Keep the fullscreen window, but progressively reduce only the
         * overlay destination when an older board rejects desktop-sized
         * scaling with PIPERR_CROPPED. */
        if (s->fullscreen_dest_policy == P96PIP_DEST_VGA) {
            if (box_w > 640) box_w = 640;
            if (box_h > 480) box_h = 480;
        } else if (s->fullscreen_dest_policy == P96PIP_DEST_NATIVE) {
            if (box_w > s->source_w) box_w = s->source_w;
            if (box_h > s->source_h) box_h = s->source_h;
        }

        fit = mr_aspect_fit(s->source_w, s->source_h, box_w, box_h);
        s->dw = fit.w;
        s->dh = fit.h;
        if (s->fullscreen_dest_policy != P96PIP_DEST_ASPECT) {
            /* Packed-YUV overlay/scaler registers commonly require even
             * destination dimensions and coordinates. Round down so the
             * adjusted rectangle cannot leave the valid screen area. */
            if (s->dw > 1 && (s->dw & 1)) --s->dw;
            if (s->dh > 1 && (s->dh & 1)) --s->dh;
        }
        s->dx = (s->win_w - s->dw) / 2;
        s->dy = (s->win_h - s->dh) / 2;
        if (s->fullscreen_dest_policy != P96PIP_DEST_ASPECT) {
            if (s->dx & 1) --s->dx;
            if (s->dy & 1) --s->dy;
        }
    }
}

static const char *fullscreen_dest_policy_name(int policy)
{
    switch (policy) {
    case P96PIP_DEST_ASPECT: return "aspect-fit";
    case P96PIP_DEST_EVEN:   return "even-aligned";
    case P96PIP_DEST_VGA:    return "640-class";
    case P96PIP_DEST_NATIVE: return "native-size";
    default:                 return "unknown";
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
 * Prefer a mode explicitly marked VideoCompatible, trying the public
 * screen's depth first and then the common RTG depths. Real Voodoo3/P96 2.x
 * hardware has now shown that this flag is not authoritative: its public
 * mode reports false while successfully hosting a MemoryWindow overlay.
 * Therefore make a second pass over ordinary P96 modes if the strict pass
 * cannot open a private screen, and let the actual PIP open be the final
 * capability test.
 */
static struct Screen *open_video_screen(p96pip_state *s, int target_w,
                                        int target_h)
{
    static const ULONG fallback_depths[] = { 16, 24, 32 };
    ULONG depths[4], preferred_depth = 16;
    struct Screen *pub, *scr;
    ULONG mode_id, video_compatible, is_p96;
    LONG err;
    int count = 0, i, j, relaxed;

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

    for (relaxed = 0; relaxed < 2; relaxed++) {
        for (i = 0; i < count; i++) {
            mode_id = p96BestModeIDTags(
                P96BIDTAG_NominalWidth, (ULONG)target_w,
                P96BIDTAG_NominalHeight, (ULONG)target_h,
                P96BIDTAG_Depth, depths[i],
                relaxed ? TAG_IGNORE : P96BIDTAG_VideoCompatible, TRUE,
                TAG_END);
            if (mode_id == (ULONG)INVALID_ID) {
                if (g_display_want_time) {
                    printf("p96pip-screen: no %smode near %dx%d depth=%lu\n",
                           relaxed ? "P96 " : "video-compatible ",
                           target_w, target_h,
                           (unsigned long)depths[i]);
                    Flush(Output());
                }
                continue;
            }

            video_compatible =
                p96GetModeIDAttr(mode_id, P96IDA_VIDEOCOMPATIBLE);
            is_p96 = p96GetModeIDAttr(mode_id, P96IDA_ISP96);
            if (!is_p96 || (!relaxed && !video_compatible)) {
                if (g_display_want_time) {
                    printf("p96pip-screen: rejected mode=0x%08lx depth=%lu "
                           "isp96=%lu video-compatible=%lu\n",
                           (unsigned long)mode_id,
                           (unsigned long)depths[i],
                           (unsigned long)is_p96,
                           (unsigned long)video_compatible);
                    Flush(Output());
                }
                continue;
            }

            if (g_display_want_time) {
                printf("p96pip-screen: trying %smode=0x%08lx depth=%lu "
                       "video-compatible=%lu\n",
                       relaxed ? "relaxed P96 " : "video-compatible ",
                       (unsigned long)mode_id,
                       (unsigned long)depths[i],
                       (unsigned long)video_compatible);
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
                    printf("p96pip-screen: open failed mode=0x%08lx "
                           "depth=%lu error=%ld\n",
                           (unsigned long)mode_id,
                           (unsigned long)depths[i], (long)err);
                    Flush(Output());
                }
                continue;
            }

            s->screen_mode_id = mode_id;
            log_screen_target(relaxed ? "private-p96-relaxed" :
                                         "private-video-compatible",
                              scr, mode_id);
            return scr;
        }
    }

    s->screen_mode_id = (ULONG)INVALID_ID;
    if (g_display_want_time) {
        printf("p96pip-screen: no usable private P96 mode for "
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
 * Fullscreen uses absolute destination dimensions. Windowed mode instead
 * uses PIPRel_Width|PIPRel_Height and encodes the aspect-fit rectangle's
 * right/bottom gaps as negative margins. This is the P96 mechanism that
 * lets its window hook adjust the hardware rectangle while Intuition is
 * resizing the containing window. A normal windowed PIP uses the demo's
 * default rectangle (no extra P96PIP geometry tags), fills its inner window,
 * and stays open during resizing; fullscreen uses explicit aspect fit.
 */
static struct Window *open_pip(p96pip_state *s, ULONG type,
                               ULONG source_format, LONG *err)
{
    struct Screen *scr;
    struct Window *win;
    ULONG flags, idcmp, screen_tag, relativity;
    LONG pip_width, pip_height;
    int simple_window;
    ULONG max_w = (ULONG)-1, max_h = (ULONG)-1;
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
        relativity = 0;
        pip_width = s->dw;
        pip_height = s->dh;
    } else {
        int right_margin = s->win_w - s->dx - s->dw;
        int bottom_margin = s->win_h - s->dy - s->dh;

        flags = WFLG_DRAGBAR | WFLG_DEPTHGADGET | WFLG_CLOSEGADGET |
                WFLG_SIZEGADGET | WFLG_ACTIVATE | WFLG_NOCAREREFRESH;
        idcmp = IDCMP_CLOSEWINDOW | IDCMP_RAWKEY | IDCMP_NEWSIZE;
        left = s->have_window_geometry ? s->window_left : 0;
        top  = s->have_window_geometry ? s->window_top  : 0;

        /* When an explicit rectangle is needed, express right/bottom as
         * relative margins. Native windowed playback uses PIP.c's default
         * relative full-window rectangle and never reopens for resizing. */
        relativity = PIPRel_Width | PIPRel_Height;
        pip_width = -(LONG)right_margin;
        pip_height = -(LONG)bottom_margin;

        /* The P96 MemoryWindow source has fixed dimensions, but the
         * destination may scale in hardware. Do not cap the window
         * to the source dimensions: P96PipDemo explicitly enlarges
         * a MemoryWindow on Picasso IV/CVision3D. */
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
    simple_window = !s->fullscreen && s->dx == 0 && s->dy == 0 &&
                    s->dw == s->win_w && s->dh == s->win_h;
    *err = 0;
    win = (struct Window *)p96PIP_OpenTags(
        screen_tag, (ULONG)scr,
        WA_Title, (ULONG)s->title,
        WA_Left, (ULONG)left, WA_Top, (ULONG)top,
        WA_InnerWidth, (ULONG)s->win_w,
        WA_InnerHeight, (ULONG)s->win_h,
        WA_Flags, flags,
        /* Intuition does not infer useful resize limits merely from
         * WFLG_SIZEGADGET. Match the ordinary P96/CGX backends: without
         * explicit min/max tags the gadget can drag while the window stays
         * constrained to its opening dimensions, so no real IDCMP_NEWSIZE
         * geometry ever reaches the reopen/debounce path below. */
        WA_MinWidth, (ULONG)160, WA_MinHeight, (ULONG)100,
        WA_MaxWidth, max_w, WA_MaxHeight, max_h,
        WA_IDCMP, idcmp,
        P96PIP_SourceFormat, source_format,
        P96PIP_SourceWidth, (ULONG)s->source_w,
        P96PIP_SourceHeight, (ULONG)s->source_h,
        P96PIP_Type, type,
        /* The PIP rectangle is relative to the window's interior, not its
         * outer RastPort coordinates. Do not add BorderLeft/BorderTop here:
         * doing so shifts a full-size PIP outside the interior and can make
         * an otherwise valid open look cropped to the driver. */
        simple_window ? TAG_IGNORE : P96PIP_Relativity, relativity,
        simple_window ? TAG_IGNORE : P96PIP_Left, (ULONG)s->dx,
        simple_window ? TAG_IGNORE : P96PIP_Top, (ULONG)s->dy,
        simple_window ? TAG_IGNORE : P96PIP_Width, (ULONG)pip_width,
        simple_window ? TAG_IGNORE : P96PIP_Height, (ULONG)pip_height,
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

/* Paint only fullscreen letterbox bars, never the live windowed PIP.
 * Filling the whole window used to leave blank/grey areas when the PIP
 * source rectangle remained at its old size after a resize. */
static void paint_letterbox(p96pip_state *s)
{
    int l, t, r, b;
    if (!s->win || !s->win->RPort || !s->fullscreen) return;
    l = s->bl; t = s->bt;
    r = l + s->win_w - 1;
    b = t + s->win_h - 1;
    SetAPen(s->win->RPort, 0);
    if (s->dy > 0)
        RectFill(s->win->RPort, (WORD)l, (WORD)t,
                 (WORD)r, (WORD)(t + s->dy - 1));
    if (s->dy + s->dh < s->win_h)
        RectFill(s->win->RPort, (WORD)l, (WORD)(t + s->dy + s->dh),
                 (WORD)r, (WORD)b);
    if (s->dx > 0 && s->dh > 0)
        RectFill(s->win->RPort, (WORD)l, (WORD)(t + s->dy),
                 (WORD)(l + s->dx - 1), (WORD)(t + s->dy + s->dh - 1));
    if (s->dx + s->dw < s->win_w && s->dh > 0)
        RectFill(s->win->RPort, (WORD)(l + s->dx + s->dw),
                 (WORD)(t + s->dy), (WORD)r,
                 (WORD)(t + s->dy + s->dh - 1));
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
/*
 * A real-hardware report: dragging the windowed PIP's size gadget produces
 * a video rectangle that goes small-and-centred or off-centre relative to
 * the actual window bounds, and stays wrong until the next fullscreen
 * round-trip (which happens to fix it). The mechanism: P96PIP_Left/Top/
 * Width/Height are init-only (see open_pip()'s own comment) - they are
 * computed by calculate_geometry() from s->win_w/win_h *before* the window
 * is actually (re)created, and baked into the one p96PIP_OpenTags() call
 * that opens it. WA_InnerWidth/InnerHeight is only a *request*; the real
 * resulting window can come back a different size (border/decoration
 * differences between window states, driver quirks, etc.) - and if it
 * does, the destination rectangle already sent to the PIP hardware no
 * longer matches the window that actually exists, silently, until some
 * *later* reopen_pip() call (the next resize, or a fullscreen toggle)
 * happens to start from the corrected size and self-heals by coincidence.
 *
 * Fixed by checking for that drift directly: after the window opens and
 * sync_content_geometry() reads back its real content size, if it differs
 * from what calculate_geometry() used a moment ago, reopen once more with
 * the corrected size so the rectangle actually baked into the hardware
 * matches the real window - rather than leaving the mismatch to be found
 * by chance on some later, unrelated reopen. Bounded to one retry: if the
 * driver still doesn't converge, this stops trying rather than risking an
 * unbounded reopen loop.
 */
static int reopen_pip(p96pip_state *s, const char *reason)
{
    LONG err = 0;
    int attempt;

    s->geometry_rejected = 0;

    for (attempt = 0; attempt < 2; attempt++) {
        int requested_w = s->win_w;
        int requested_h = s->win_h;

        close_pip(s);
        calculate_geometry(s);

        /* VideoWindow refers to live video-input hardware, not a decoded
         * video frame in RAM. Probe the same MemoryWindow formats as the
         * official P96PipDemo, after retaining RiVA's fast YUV-first mode.
         * A format is saved only after its open succeeds. */
        {
            static const ULONG formats[] = {
                RGBFB_Y4U2V2, RGBFB_R5G6B5PC, RGBFB_R5G6B5
            };
            int format_index;
            s->win = NULL;
            s->hw_overlay = 0;
            for (format_index = 0; format_index < 3; ++format_index) {
                ULONG fmt = formats[format_index];
                s->win = open_pip(s, PIPT_MemoryWindow, fmt, &err);
                if (g_display_want_time) {
                    printf("p96pip: MemoryWindow fmt=%lu %s: %s (err=%ld: %s)\n",
                           (unsigned long)fmt,
                           fmt == RGBFB_Y4U2V2 ? "YVYU/YUYV" :
                           fmt == RGBFB_R5G6B5PC ? "RGB565PC" : "RGB565BE",
                           s->win ? "opened" : "unavailable",
                           (long)err, pip_err_name(err));
                    Flush(Output());
                }
                if (s->win) {
                    s->source_format = fmt;
                    s->hw_overlay = 1; /* MemoryWindow can use HW overlay. */
                    break;
                }
                if (err == PIPERR_CROPPED ||
                    err == PIPERR_BADDIMENSIONS ||
                    err == PIPERR_BADALIGNMENT)
                    s->geometry_rejected = 1;
            }
        }
        if (!s->win) {
            if (g_display_want_time) {
                printf("p96pip: all MemoryWindow formats failed (err=%ld: %s)\n",
                       (long)err, pip_err_name(err));
                Flush(Output());
            }
            return 0;
        }

        if (g_display_want_time) {
            printf("p96pip: calling p96PIP_GetTags for source bitmap\n");
            Flush(Output());
        }
        /* p96PIP_GetTagList() returns a count, not a success boolean. Check
         * the retrieved pointer itself so either convention remains
         * harmless. */
        p96PIP_GetTags(s->win, P96PIP_SourceBitMap, (ULONG)&s->source_bitmap,
                       TAG_END);
        if (!s->source_bitmap) {
            if (g_display_want_time) {
                printf("p96pip: could not retrieve source bitmap - "
                       "closing\n");
                Flush(Output());
            }
            close_pip(s);
            return 0;
        }

        sync_content_geometry(s);

        /* Tolerate a small difference instead of demanding exact equality.
         * Now that windowed mode opens with PIPRel_Width|PIPRel_Height
         * (see open_pip()'s own comment), the driver's own window hook is
         * what keeps the overlay rectangle following the window - a few
         * pixels of border/rounding slack here is normal, not a real drift,
         * and forcing an exact match risked a spurious extra reopen right
         * after every single resize (this retry loop and the relative-
         * margin mechanism were added and tested separately; a real-
         * hardware report after both landed together showed the window
         * visibly reopening and reverting shortly after every resize -
         * consistent with this retry firing on an ordinary few-pixel
         * mismatch and feeding a slightly-off size back into a second,
         * untested-in-combination reopen). Only retry for a mismatch large
         * enough that it cannot plausibly be rounding - the original
         * failure mode this loop exists for (an absolute-geometry open
         * landing on a completely different real window size). */
        {
            enum { DRIFT_TOLERANCE_PX = 8 };
            int dw = s->win_w - requested_w;
            int dh = s->win_h - requested_h;
            if (dw < 0) dw = -dw;
            if (dh < 0) dh = -dh;
            if (dw <= DRIFT_TOLERANCE_PX && dh <= DRIFT_TOLERANCE_PX)
                break;
        }

        if (g_display_want_time) {
            printf("p96pip: real window %dx%d differs from the %dx%d used "
                   "for the destination rectangle just opened; %s\n",
                   s->win_w, s->win_h, requested_w, requested_h,
                   attempt == 0 ? "reopening once more with the corrected "
                                  "size"
                                : "giving up after one retry");
            Flush(Output());
        }
    }

    rebuild_geometry(s, reason);
    return 1;
}

/* A Voodoo3/P96 2.x report proves that a native 540x360 MemoryWindow works
 * on a 1024x768 public screen while a 1024x683 destination is rejected as
 * cropped. Use the same progression for --fullscreen startup and live F-key
 * toggles: preserve the ideal fill first, then try alignment, a conventional
 * 640-class scaler target, and finally the source's known-good native size. */
static int reopen_fullscreen_pip(p96pip_state *s, const char *reason,
                                 int exhaust_sizes)
{
    static const int policies[] = {
        P96PIP_DEST_ASPECT,
        P96PIP_DEST_EVEN,
        P96PIP_DEST_VGA,
        P96PIP_DEST_NATIVE
    };
    int i;

    for (i = 0; i < 4; ++i) {
        s->fullscreen_dest_policy = policies[i];
        calculate_geometry(s);
        if (g_display_want_time) {
            printf("p96pip-fullscreen: trying %s destination %d,%d %dx%d "
                   "inside %dx%d window\n",
                   fullscreen_dest_policy_name(s->fullscreen_dest_policy),
                   s->dx, s->dy, s->dw, s->dh, s->win_w, s->win_h);
            Flush(Output());
        }
        if (reopen_pip(s, reason))
            return 1;

        /* A private screen failure should normally fall through quickly to
         * the public screen. On that public-screen attempt, however, exhaust
         * every size even when an emulator/driver reports OUTOFPENS or
         * NOTAVAILABLE rather than a geometry-specific error: the proven
         * native-size public MemoryWindow may still work. */
        if (i == 0 && !s->geometry_rejected && !exhaust_sizes)
            break;
    }

    s->fullscreen_dest_policy = P96PIP_DEST_ASPECT;
    return 0;
}

static void *p96pip_open(int w, int h, const char *title)
{
    p96pip_state *s;
    struct Screen *scr;
    int screen_w = 0, screen_h = 0, opened;

    if (!P96Base || w <= 0 || h <= 0 || (w & 1)) return NULL;

    /* One setting for both packed YUV producers and the software fallback;
     * read once at session open, not once per rendered frame. */
    mr_yuv_set_p96_format(mr_p96_format_load());
    if (g_display_want_time) {
        printf("p96pip: output format %s (%s)\n",
               mr_yuv_get_p96_format() ? "YUYV" : "YVYU",
               mr_yuv_get_p96_format() ? "Voodoo" : "WinUAE");
        Flush(Output());
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

    opened = s->fullscreen ? reopen_fullscreen_pip(s, "init",
                                                   s->screen ? 0 : 1) :
                             reopen_pip(s, "init");
    if (!opened) {
        /* A driver may advertise a VideoCompatible mode yet refuse PIP on
         * it. Preserve the old public-screen attempt before falling through
         * to the other display backends. */
        if (s->screen) {
            close_video_screen(&s->screen, "initial PIP fallback");
            s->screen_mode_id = (ULONG)INVALID_ID;
            s->win_w = screen_w;
            s->win_h = screen_h;
            opened = reopen_fullscreen_pip(s, "init-public-fallback", 1);
        }
        if (!opened) {
            close_video_screen(&s->screen, "initial open failure");
            FreeVec(s);
            return NULL;
        }
    }

    if (g_display_want_time) {
        printf("p96pip: opened MemoryWindow format=%lu, window=%dx%d source=%dx%d\n",
               (unsigned long)s->source_format, s->win_w, s->win_h, w, h);
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
static int write_yuv422_rows(struct BitMap *bm, ULONG format, int y0,
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
        if (format == RGBFB_Y4U2V2)
            memcpy(drow, srow, (size_t)w * 2u);
        else
            mr_p96_rgb565_yuv422_row(srow, drow, w,
                mr_yuv_get_p96_format(), format == RGBFB_R5G6B5PC);
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
 * dst_pair[1]/[3] are V/U for YVYU and U/V for YUYV. This RGB-only
 * producer follows the same saved preference as mr_yuv420_to_y4u2v2().
 *
 * The RGB->YCbCr coefficients below are the ordinary studio-range matrix,
 * matching mr_yuv.c's own mr_yuv420_to_rgb24(). A full-range variant was
 * tried here (and in mr_yuv420_to_y4u2v2()) but caused severe white/black
 * clipping on real video in the P96 overlay path. */
static int write_rgb_rows(struct BitMap *bm, ULONG format, int y0,
                          const unsigned char *src, int src_stride,
                          int w, int rows, int src_is_bgr)
{
    struct RenderInfo ri;
    LONG lock;
    int y, bpr;
    int yuyv = mr_yuv_get_p96_format();
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
        if (format != RGBFB_Y4U2V2) {
            mr_p96_rgb565_rgb24_row(src_pixel, dst_pair, w, src_is_bgr,
                                      format == RGBFB_R5G6B5PC);
            continue;
        }
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
            int v = clamp_byte(
                ((112 * r - 94 * g - 18 * b + 128) >> 8) + 128);
            int u = clamp_byte(
                ((-38 * r - 74 * g + 112 * b + 128) >> 8) + 128);
            dst_pair[0] = clamp_byte(
                ((66 * r0 + 129 * g0 + 25 * b0 + 128) >> 8) + 16);
            dst_pair[1] = (unsigned char)(yuyv ? u : v);
            dst_pair[2] = clamp_byte(
                ((66 * r1 + 129 * g1 + 25 * b1 + 128) >> 8) + 16);
            dst_pair[3] = (unsigned char)(yuyv ? v : u);
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

    if (!write_rgb_rows(s->source_bitmap, s->source_format, dy0,
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
        s->timing.dst_format = s->source_format == RGBFB_Y4U2V2
            ? "Y4U2V2 (P96 PIP source)"
            : s->source_format == RGBFB_R5G6B5PC
              ? "RGB565PC (P96 PIP source)" : "RGB565BE (P96 PIP source)";
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

    if (!write_yuv422_rows(s->source_bitmap, s->source_format, dy0,
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
        s->timing.dst_format = s->source_format == RGBFB_Y4U2V2
            ? "Y4U2V2 (P96 PIP source)"
            : s->source_format == RGBFB_R5G6B5PC
              ? "RGB565PC (P96 PIP source)" : "RGB565BE (P96 PIP source)";
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
            int old_w = s->win_w, old_h = s->win_h;

            /* P96PipDemo leaves its MemoryWindow open while Intuition
             * resizes it: P96's window hook scales the source automatically.
             * Reopening here loses that behaviour and can snap the window
             * back to its original source size. Read back the real *inner*
             * dimensions for timing and fullscreen restoration only. */
            sync_content_geometry(s);
            s->have_window_geometry = 1;
            s->window_left = s->win->LeftEdge;
            s->window_top = s->win->TopEdge;
            s->window_width = s->win_w;
            s->window_height = s->win_h;
            calculate_geometry(s);
            s->geometry_valid = 1;
            if (g_display_want_time &&
                (old_w != s->win_w || old_h != s->win_h)) {
                printf("p96pip-resize: live MemoryWindow %dx%d -> %dx%d "
                       "(P96 scaling; no close/reopen)\n",
                       old_w, old_h, s->win_w, s->win_h);
                Flush(Output());
            }
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
    return s->quit ? MR_EV_QUIT : ev;
}

static int p96pip_toggle_fullscreen(void *h)
{
    p96pip_state *s = (p96pip_state *)h;
    struct Screen *old_screen;
    ULONG old_mode_id;
    int old_fullscreen, old_win_w, old_win_h;
    int old_dest_policy;
    int public_w = 640, public_h = 480;
    int entering;

    if (!s || !s->win) return 0;
    old_fullscreen = s->fullscreen;
    entering = !old_fullscreen;
    old_win_w = s->win_w;
    old_win_h = s->win_h;
    old_screen = s->screen;
    old_mode_id = s->screen_mode_id;
    old_dest_policy = s->fullscreen_dest_policy;

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
        s->fullscreen_dest_policy = P96PIP_DEST_ASPECT;
        s->screen = open_video_screen(s, public_w, public_h);
        if (s->screen) {
            s->win_w = s->screen->Width;
            s->win_h = s->screen->Height;
        } else {
            s->win_w = public_w;
            s->win_h = public_h;
        }
        if (reopen_fullscreen_pip(s, "fullscreen-toggle",
                                  s->screen ? 0 : 1))
            return 1;

        /* WinUAE can open an ordinary private P96 screen whose mode reports
         * VideoCompatible=0, but then rejects every MemoryWindow on it with
         * PIPERR_OUTOFPENS while the same overlay works on the public screen.
         * Do not let a successfully opened-but-PIP-incompatible private mode
         * hide the public-screen path. Close it and exhaust the full size
         * ladder there before recommending CGX. */
        if (s->screen) {
            close_video_screen(&s->screen, "private PIP fallback");
            s->screen_mode_id = (ULONG)INVALID_ID;
            s->win_w = public_w;
            s->win_h = public_h;
            if (g_display_want_time) {
                printf("p96pip-fullscreen: private-screen PIP unavailable; "
                       "retrying on public screen\n");
                Flush(Output());
            }
            if (reopen_fullscreen_pip(s, "fullscreen-toggle-public", 1))
                return 1;
        }

        close_video_screen(&s->screen, "fullscreen open rollback");
        s->screen_mode_id = (ULONG)INVALID_ID;
    } else {
        /* Keep the private screen alive until the public-screen PIP opens,
         * so a failed toggle can restore the previous mode. reopen_pip()
         * closes the old PIP before it requests the replacement. */
        s->fullscreen = 0;
        s->fullscreen_dest_policy = P96PIP_DEST_ASPECT;
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
    s->fullscreen_dest_policy = old_dest_policy;
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
