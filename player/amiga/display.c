/*
 * MintVID - display front end.
 *
 * Opens the shared library bases, then tries the RTG (cybergraphics) backend
 * and falls back to AGA - so the same mrplay runs on a PiStorm/RTG box and on a
 * plain AGA machine with no RTG. display_set_force_aga() skips the RTG attempt
 * (useful for testing AGA on an RTG machine).
 */
#include "amiga_display.h"
#include "display_backend.h"
#include "../core/mr_yuv.h"

#include <proto/exec.h>
#include <proto/dos.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Single definitions of the shared library bases (the proto inlines and the
 * backends reference these globals). */
struct IntuitionBase *IntuitionBase = NULL;
struct GfxBase       *GfxBase       = NULL;
struct Library       *CyberGfxBase  = NULL;
struct Library       *P96Base       = NULL;

static int g_force_aga = 0;
static int g_force_p96 = 0;
int g_aga_window = 0;  /* 0 off, 1 Window, 2 Window (Half); shared with
                        * display_aga_window.c */
int g_aga_ham   = 0;   /* shared with the AGA backend */
int g_aga_scale = 1;
int g_aga_c2p   = 3;   /* CPU-matched Kalms by default; its runtime checks
                        * fall back safely to WritePixelArray8. --wpa selects
                        * that graphics.library path explicitly. */
int g_aga_lace  = 0;
int g_aga_akiko = 0;
int g_aga_ecs_fast = 0;
int g_aga_ecs32 = 0;
int g_aga_ehb = 0;
int g_aga_copper_vdouble = 0;
int g_display_want_time = 0;  /* enable RTG geometry diagnostics (--time)   */
int g_display_fullscreen = 0;

void display_set_force_aga(int on) { g_force_aga = on; }
void display_set_force_p96(int on) { g_force_p96 = on; }
void display_set_aga_window(int mode)
{
    g_aga_window = mode == 2 ? 2 : mode ? 1 : 0;
}
void display_set_ham(int bits) { g_aga_ham = bits; if (bits) g_force_aga = 1; }
void display_set_scale(int n)  { g_aga_scale = (n == 2) ? 2 : 1; }
void display_set_c2p(int on)   { g_aga_c2p = on ? 1 : 0; }
void display_set_riva_c2p(int on) { g_aga_c2p = on ? 2 : 0; }
void display_set_kalms_c2p(int on) { g_aga_c2p = on ? 3 : 0; }
void display_set_direct_c2p(int on) { g_aga_c2p = on ? 4 : 0; }
void display_set_lace(int on)  { g_aga_lace = on ? 1 : 0; }
void display_set_akiko(int on) { g_aga_akiko = on ? 1 : 0; }
void display_set_ecs_fast(int on) {
    g_aga_ecs_fast = on ? 1 : 0;
    if (on) g_force_aga = 1;
}
void display_set_ecs32(int on) {
    g_aga_ecs32 = on ? 1 : 0;
    if (on) g_force_aga = 1;
}
void display_set_ehb(int on) {
    g_aga_ehb = on ? 1 : 0;
    if (on) g_force_aga = 1;
}
void display_set_copper_vdouble(int on) {
    g_aga_copper_vdouble = on ? 1 : 0;
}
void display_set_timing_mode(int on) { g_display_want_time = on ? 1 : 0; }
void display_set_fullscreen(int on) { g_display_fullscreen = on ? 1 : 0; }

struct amiga_display {
    const display_backend *be;
    void                  *h;
    mr_display_service_fn service;
    void                  *service_opaque;
    /* The size/title display_open() was called with, kept so
     * switch_to_cgx_fallback() can reopen a different backend with the
     * same geometry after the original one is closed. */
    int                    open_w, open_h;
    char                   open_title[128];
    /* Scratch RGB24 buffer for display_show_yuv422()'s software fallback -
     * only allocated the first time the active backend lacks show_yuv422
     * (see switch_to_cgx_fallback()); NULL otherwise. */
    unsigned char         *yuv422_fallback_buf;
    size_t                 yuv422_fallback_cap;
    /* Set by switch_to_cgx_fallback() when it moves off backend_p96pip
     * specifically because that backend's own PIP fullscreen path failed
     * (not because P96 was never available at all) - the windowed PIP was
     * proven working right before that failure. display_poll_event()
     * watches for this backend leaving fullscreen again and, if so, retries
     * the PIP windowed rather than staying on CGX for the rest of the
     * session - see maybe_retry_pip_windowed(). */
    int                    pip_fallback_active;
};

static void close_libs(void)
{
    if (P96Base)       { CloseLibrary(P96Base);                         P96Base       = NULL; }
    if (CyberGfxBase)  { CloseLibrary(CyberGfxBase);                    CyberGfxBase  = NULL; }
    if (GfxBase)       { CloseLibrary((struct Library *)GfxBase);       GfxBase       = NULL; }
    if (IntuitionBase) { CloseLibrary((struct Library *)IntuitionBase); IntuitionBase = NULL; }
}

amiga_display *display_open(int w, int h, const char *title)
{
    const display_backend *order[5];
    int n = 0, i;

    IntuitionBase = (struct IntuitionBase *)
                    OpenLibrary((CONST_STRPTR)"intuition.library", 39);
    GfxBase       = (struct GfxBase *)
                    OpenLibrary((CONST_STRPTR)"graphics.library", 39);
    if (!IntuitionBase || !GfxBase) { close_libs(); return NULL; }

    /* RTG is optional; its absence is exactly when we want the AGA fallback. */
    CyberGfxBase = OpenLibrary((CONST_STRPTR)"cybergraphics.library", 40);
    /* Also optional, and only opened at all when P96 mode was explicitly
     * requested - no reason to touch it on a CGX-only or AGA-only box. */
    if (g_force_p96 && !g_force_aga)
        P96Base = OpenLibrary((CONST_STRPTR)"Picasso96API.library", 0);

    /* P96 mode (--p96) tries the PIP overlay backend first - real hardware
     * acceleration where the board/driver supports it - then falls back to
     * the older direct screen-bitmap-lock backend (backend_p96) if the PIP
     * can't open, then CGX, then AGA. This is a plain internal fallback, not
     * a separate user-facing choice: both backends serve the one "P96" mode
     * (display_set_force_p96()), so selecting P96 never loses playback,
     * only the chance at real acceleration, same discipline as the rest of
     * this chain. display_backend_name() still reports which of the two
     * actually opened ("RTG (P96 Overlay)" vs "RTG (P96)"), so a --time log
     * can tell them apart even though the user only ever picks one option.
     * backend_p96pip doesn't need CyberGfxBase at all (see its own file
     * header); backend_p96 does. */
    /* Window mode shares the Workbench screen instead of opening one; if
     * that window can't open, the normal chain below still gets a picture
     * up. */
    if (g_aga_window)
        order[n++] = &backend_aga_window;
    if (!g_force_aga && g_force_p96 && P96Base)
        order[n++] = &backend_p96pip;
    if (!g_force_aga && g_force_p96 && CyberGfxBase && P96Base)
        order[n++] = &backend_p96;
    if (!g_force_aga && CyberGfxBase) order[n++] = &backend_cgx;
    order[n++] = &backend_aga;

    for (i = 0; i < n; i++) {
        void *hh = order[i]->open(w, h, title);
        if (hh) {
            amiga_display *d = (amiga_display *)malloc(sizeof *d);
            if (!d) { order[i]->close(hh); close_libs(); return NULL; }
            d->be = order[i];
            d->h  = hh;
            d->service = NULL;
            d->service_opaque = NULL;
            d->open_w = w;
            d->open_h = h;
            snprintf(d->open_title, sizeof d->open_title, "%s",
                     (title && *title) ? title : "MintVID");
            d->yuv422_fallback_buf = NULL;
            d->yuv422_fallback_cap = 0;
            return d;
        }
    }
    close_libs();
    return NULL;
}

/*
 * A backend's toggle_fullscreen()/poll() can report display_backend.h's
 * sentinel return value 2 ("this backend's own fullscreen path failed on
 * real hardware, but it rolled back to a working windowed state") -
 * currently only backend_p96pip, whose file header documents a real
 * Voodoo3/P96 2.1 setup where neither PIP type can be opened fullscreen at
 * all. Rather than leave the caller stuck retrying the same failing PIP
 * fullscreen path forever, close that backend and open backend_cgx
 * (ordinary WritePixelArray, no hardware overlay) at the same size/title,
 * then take *that* fullscreen - CGX has none of the PIP's hardware-overlay
 * ambitions, so its own toggle_fullscreen() has no equivalent failure mode
 * to inherit.
 *
 * d->be/d->h are updated in place, so every other display_* call already
 * routes to the new backend automatically with no further caller-side
 * change - including display_show_yuv422(), whose own software fallback
 * (see below) is what lets an H.264/MPEG-2 session that chose the YUV422
 * queue path at startup keep working through a backend that has no
 * show_yuv422 of its own.
 *
 * Returns 1 if the switch (and its fullscreen toggle) succeeded, 0 if CGX
 * itself could not be opened either - in which case d->be/d->h are left
 * exactly as they were (the old, now-windowed PIP session), since a caller
 * that can't get fullscreen at all is still better off with a working
 * windowed session than none.
 */
static int switch_to_cgx_fallback(amiga_display *d)
{
    void *hh;
    const display_backend *old_be;
    void *old_h;

    if (!d || !CyberGfxBase || d->be == &backend_cgx)
        return 0;

    printf("display: P96 PIP overlay could not open fullscreen on this "
           "hardware; falling back to RTG (CGX) fullscreen\n");
    Flush(Output());

    hh = backend_cgx.open(d->open_w, d->open_h, d->open_title);
    if (!hh) {
        printf("display: CGX fallback open failed; keeping the existing "
               "windowed session\n");
        Flush(Output());
        return 0;
    }

    old_be = d->be;
    old_h = d->h;
    d->be = &backend_cgx;
    d->h = hh;
    old_be->close(old_h);
    /* The windowed PIP this replaces was proven working right up until its
     * own fullscreen attempt failed - remember that so display_poll_event()
     * can retry it once this CGX session goes back to windowed, instead of
     * settling for CGX (no hardware overlay) for the rest of the session.
     * Only meaningful when the backend being replaced was actually the PIP
     * overlay - a caller that opened this way from something else (there is
     * currently no other caller) would have nothing proven-working to
     * retry. */
    d->pip_fallback_active = (old_be == &backend_p96pip);

    if (backend_cgx.toggle_fullscreen)
        backend_cgx.toggle_fullscreen(d->h);
    return 1;
}

/*
 * Companion to switch_to_cgx_fallback(): once that fallback's CGX session
 * goes back to windowed, retry backend_p96pip windowed rather than staying
 * on CGX (with no hardware overlay) for the rest of the session - the PIP
 * was working windowed right before its fullscreen attempt failed, so
 * there is a real, working configuration worth returning to.
 *
 * g_display_fullscreen is p96pip_open()'s own "open fullscreen" switch,
 * driven by the --fullscreen CLI flag at session start - unrelated to the
 * live F-key fullscreen state this function reacts to, but read by
 * p96pip_open() regardless, so it is forced to 0 for the duration of this
 * call (and restored after) to guarantee a windowed reopen regardless of
 * how the session was originally launched.
 *
 * Called at most once per fullscreen-failure episode: d->pip_fallback_
 * active is cleared unconditionally after this runs, whether or not the
 * PIP reopen actually succeeds, so a driver that also fails to reopen PIP
 * windowed a second time is not retried every subsequent poll.
 */
static void maybe_retry_pip_windowed(amiga_display *d)
{
    void *hh;
    int saved_fullscreen;

    d->pip_fallback_active = 0;
    if (!P96Base) return;

    saved_fullscreen = g_display_fullscreen;
    g_display_fullscreen = 0;
    hh = backend_p96pip.open(d->open_w, d->open_h, d->open_title);
    g_display_fullscreen = saved_fullscreen;
    if (!hh) return;

    printf("display: back to windowed - retrying the P96 PIP overlay\n");
    Flush(Output());
    backend_cgx.close(d->h);
    d->be = &backend_p96pip;
    d->h = hh;
}

void display_show_rgb(amiga_display *d, const unsigned char *rgb,
                      int w, int h, int stride, int dy0, int dy1)
{
    if (d) d->be->show(d->h, rgb, w, h, stride, dy0, dy1,
                       d->service, d->service_opaque);
}

int display_supports_bgr24(amiga_display *d)
{
    return d && d->be->show_bgr;
}

void display_show_bgr24(amiga_display *d, const unsigned char *bgr,
                        int w, int h, int stride, int dy0, int dy1)
{
    if (d && d->be->show_bgr)
        d->be->show_bgr(d->h, bgr, w, h, stride, dy0, dy1,
                        d->service, d->service_opaque);
}

int display_supports_yuv422(amiga_display *d)
{
    return d && d->be->show_yuv422 && d->be->supports_yuv422 &&
           d->be->supports_yuv422(d->h);
}

void display_show_yuv422(amiga_display *d, const unsigned char *yuv,
                         int w, int h, int stride, int dy0, int dy1)
{
    size_t need;
    unsigned char *p;

    if (!d || !yuv || w <= 0 || h <= 0) return;

    if (d->be->show_yuv422) {
        d->be->show_yuv422(d->h, yuv, w, h, stride, dy0, dy1,
                           d->service, d->service_opaque);
        return;
    }

    /* switch_to_cgx_fallback() moved the active backend away from the P96
     * PIP overlay mid-session; the new backend (CGX/AGA) has no
     * show_yuv422 of its own, so unpack to RGB24 here and forward to its
     * ordinary show() - see amiga_display.h's own doc on this function and
     * core/mr_yuv.c's mr_y4u2v2_to_rgb24(). The whole frame is converted
     * (not just [dy0,dy1)) for simplicity: this path only exists as a
     * last-resort correctness fallback once real hardware overlay is
     * already unavailable, not a performance-sensitive one. */
    need = (size_t)w * 3u * (size_t)h;
    if (d->yuv422_fallback_cap < need) {
        p = (unsigned char *)realloc(d->yuv422_fallback_buf, need);
        if (!p) return;
        d->yuv422_fallback_buf = p;
        d->yuv422_fallback_cap = need;
    }
    if (!mr_y4u2v2_to_rgb24(d->yuv422_fallback_buf, w * 3, yuv, stride, w, h))
        return;
    display_show_rgb(d, d->yuv422_fallback_buf, w, h, w * 3, dy0, dy1);
}

void display_set_service(amiga_display *d, mr_display_service_fn fn,
                         void *opaque)
{
    if (d) { d->service = fn; d->service_opaque = opaque; }
}

int display_supports_indexed(amiga_display *d, int *indexed_depth)
{
    return d && d->be->supports_indexed &&
           d->be->supports_indexed(d->h, indexed_depth);
}

void display_show_indexed(amiga_display *d, const unsigned char *idx,
                          int w, int h, int idx_stride, int dy0, int dy1)
{
    if (d && d->be->show_indexed)
        d->be->show_indexed(d->h, idx, w, h, idx_stride, dy0, dy1,
                            d->service, d->service_opaque);
}

int display_supports_yuv_indexed(amiga_display *d, int src_w, int src_h,
                                 int *dst_w, int *dst_h, int *vscale,
                                 int *indexed_depth, int *ham)
{
    if (ham) *ham = 0;
    return d && d->be->supports_yuv_indexed &&
           d->be->supports_yuv_indexed(d->h, src_w, src_h, dst_w, dst_h,
                                       vscale, indexed_depth, ham);
}

int display_rtg_frame_timing(amiga_display *d, mr_display_timing *timing)
{
    return d && d->be->timing ? d->be->timing(d->h, timing) : 0;
}

int display_toggle_fullscreen(amiga_display *d)
{
    int r;
    if (!d || !d->be->toggle_fullscreen) return 0;
    r = d->be->toggle_fullscreen(d->h);
    if (r == 2)
        return switch_to_cgx_fallback(d);
    return r;
}

int display_poll_event(amiga_display *d)
{
    int ev;
    if (!d) return MR_EV_QUIT;
    ev = d->be->poll(d->h);
    if (ev == MR_EV_RENDERER_SWITCH) {
        /* Whether or not the CGX fallback itself succeeds, the previous
         * backend is left in a known-working (windowed) state by the
         * p96pip_toggle_fullscreen() call that produced this event - so
         * either way there is a working session to keep playing, not a
         * reason to quit. */
        switch_to_cgx_fallback(d);
        return MR_EV_NONE;
    }
    /* Every poll is cheap insurance against staying on CGX (no hardware
     * overlay) longer than necessary: once the fallback CGX session
     * (entered only because PIP fullscreen failed) goes back to windowed -
     * via its own F-key handling inside cgx_poll(), which display.c has no
     * other way to observe - retry the PIP once. See maybe_retry_pip_
     * windowed()'s own comment for why this is safe to attempt repeatedly
     * until the flag clears itself. */
    if (d->pip_fallback_active && d->be == &backend_cgx &&
        backend_cgx.is_fullscreen && !backend_cgx.is_fullscreen(d->h))
        maybe_retry_pip_windowed(d);
    return ev;
}

unsigned long display_wait_mask(amiga_display *d)
{
    return (d && d->be->wait_mask) ? d->be->wait_mask(d->h) : 0;
}

void display_set_status(amiga_display *d, const char *text)
{
    if (d && d->be->status) d->be->status(d->h, text);
}

void display_close(amiga_display *d)
{
    if (!d) return;
    d->be->close(d->h);
    free(d->yuv422_fallback_buf);
    free(d);
    close_libs();
}

const char *display_backend_name(amiga_display *d)
{
    return d ? d->be->name : "none";
}
