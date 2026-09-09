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

#include <proto/exec.h>
#include <stdlib.h>

/* Single definitions of the shared library bases (the proto inlines and the
 * backends reference these globals). */
struct IntuitionBase *IntuitionBase = NULL;
struct GfxBase       *GfxBase       = NULL;
struct Library       *CyberGfxBase  = NULL;
struct Library       *P96Base       = NULL;

static int g_force_aga = 0;
static int g_force_p96 = 0;
int g_aga_ham   = 0;   /* shared with the AGA backend */
int g_aga_scale = 1;
int g_aga_c2p   = 3;   /* CPU-matched Kalms by default; its runtime checks
                        * fall back safely to WritePixelArray8. --wpa selects
                        * that graphics.library path explicitly. */
int g_aga_lace  = 0;
int g_aga_akiko = 0;
int g_aga_ecs_fast = 0;
int g_aga_ecs32 = 0;
int g_display_want_time = 0;  /* enable RTG geometry diagnostics (--time)   */
int g_display_fullscreen = 0;

void display_set_force_aga(int on) { g_force_aga = on; }
void display_set_force_p96(int on) { g_force_p96 = on; }
void display_set_ham(int bits) { g_aga_ham = bits; if (bits) g_force_aga = 1; }
void display_set_scale(int n)  { g_aga_scale = (n == 2) ? 2 : 1; }
void display_set_c2p(int on)   { g_aga_c2p = on ? 1 : 0; }
void display_set_riva_c2p(int on) { g_aga_c2p = on ? 2 : 0; }
void display_set_kalms_c2p(int on) { g_aga_c2p = on ? 3 : 0; }
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
void display_set_timing_mode(int on) { g_display_want_time = on ? 1 : 0; }
void display_set_fullscreen(int on) { g_display_fullscreen = on ? 1 : 0; }

struct amiga_display {
    const display_backend *be;
    void                  *h;
    mr_display_service_fn service;
    void                  *service_opaque;
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
    const display_backend *order[3];
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

    /* P96 is tried first (only when selected and available); backend_p96's
     * own open() fails cleanly for unsupported screen formats or geometry,
     * in which case this still falls through to CGX, then AGA - so choosing
     * P96 never loses working playback, only the chance at the faster path. */
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
            return d;
        }
    }
    close_libs();
    return NULL;
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
    return d && d->be->toggle_fullscreen
         ? d->be->toggle_fullscreen(d->h) : 0;
}

int display_poll_event(amiga_display *d)
{
    return d ? d->be->poll(d->h) : MR_EV_QUIT;
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
    free(d);
    close_libs();
}

const char *display_backend_name(amiga_display *d)
{
    return d ? d->be->name : "none";
}
