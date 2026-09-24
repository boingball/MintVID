/*
 * MintVID - "Window" backend: video in a window on the Workbench.
 *
 * Every other native-chipset path (display_aga.c) opens its own screen and
 * owns its whole palette. This one shares the default public screen instead,
 * so it has to live with the pens that screen already uses. Nothing here is
 * AGA-specific: an ECS or OCS Workbench (16 or 32 colours, say) gets the
 * smaller cube its depth allows, through exactly the same code.
 *
 *   - the frame is dithered to the same fixed RGB cube the AGA screen uses
 *     (core/mr_dither.h / mr_yuv_dither.h; 6x6x6 on a 256-colour screen,
 *     4x4x2 or 2x4x2 on shallower ones);
 *   - each cube colour is mapped to a screen pen with ObtainBestPen(). With
 *     PRECISION_IMAGE that takes a free pen where one is left and otherwise
 *     shares the closest existing colour, so Workbench's own pens are never
 *     changed;
 *   - every frame goes through a 256-byte cube-index -> pen table (and a
 *     nearest-neighbour scale when the window is not the video's size) into
 *     a small strip, which WriteChunkyPixels() (graphics V40) or, on older
 *     graphics.library, WritePixelArray8() draws into the window's RastPort.
 *     Both go through layers, so overlapping windows clip correctly.
 *
 * No HAM (HAM needs its own screen) and no C2P: the chunky-to-planar step is
 * graphics.library's, which is what makes the window clipping free. Direct
 * C2P into an unobscured window would be faster and is a possible later
 * addition.
 *
 * Library bases (IntuitionBase, GfxBase) are opened by display.c before any
 * backend's open() runs.
 */
#include "amiga_display.h"
#include "display_backend.h"
#include "mr_aspect.h"
#include "../core/mr_dither.h"

#include <stddef.h>
#include <exec/types.h>
#include <exec/memory.h>
#include <intuition/intuition.h>
#include <intuition/screens.h>
#include <graphics/gfx.h>
#include <graphics/view.h>
#include <graphics/rastport.h>
#include <utility/tagitem.h>

#include <proto/exec.h>
#include <proto/intuition.h>
#include <proto/graphics.h>
#include <string.h>
#include <stdio.h>
#include <time.h>

/* Half mode (display_set_aga_window(2)): the window opens at half the
 * video's width and height. H.264/MPEG-2 dither straight to that size from
 * YUV (a quarter of the dither work and of the draw); other codecs dither
 * every other row and are scaled down while drawing. */

/* Destination rows remapped and drawn per graphics call. Keeps the strip
 * small and gives Paula a service point between strips. */
#define AW_STRIP_ROWS 32

typedef struct {
    struct Window   *win;
    struct ColorMap *cm;
    LONG             pens[256];    /* ObtainBestPen() result per cube index  */
    int              npens;        /* cube entries with an obtained pen      */
    LONG             black_pen;
    UBYTE            lut[256];     /* cube index -> pen, for the hot loop    */
    int              depth;        /* dither depth handed to callers: 4/5/8  */
    int              use_wcp;      /* graphics V40 WriteChunkyPixels()       */
    int              half;         /* window and YUV target at half size     */
    int              source_w, source_h; /* picture size shown (halved in
                                          * half mode)                        */
    int              bl, bt, iw, ih;
    int              dx, dy, dw, dh; /* aspect-fitted video rectangle        */
    int              pending_w, pending_h;
    clock_t          resize_at;
    int              geometry_valid;
    int              force_full_redraw;
    /* Source-size cube indices, only used by the RGB24 show() entry. */
    UBYTE           *idx;
    size_t           idx_size;
    /* Remapped pens, AW_STRIP_ROWS rows of strip_stride bytes. */
    UBYTE           *strip;
    size_t           strip_size;
    int              strip_stride;
    /* WritePixelArray8's one-row temporary, only without V40 graphics. */
    struct BitMap   *tempbm;
    struct RastPort  temprp;
    int              tempbm_w;
    int              quit;
    char             title[80];
    mr_display_timing timing;
} aw_state;

static unsigned long aw_elapsed_us(clock_t begin)
{
    return (unsigned long)((clock() - begin) * 1000000UL / CLOCKS_PER_SEC);
}

/* Shrink-only fit, so a video larger than the Workbench opens a window
 * that fits (same rule as display_cgx.c). */
static void aw_fit_within(int w, int h, int max_w, int max_h,
                          int *out_w, int *out_h)
{
    *out_w = w;
    *out_h = h;
    if (w <= 0 || h <= 0 || max_w <= 0 || max_h <= 0) return;
    if (w <= max_w && h <= max_h) return;
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

static LONG aw_obtain_pen(struct ColorMap *cm, int r, int g, int b)
{
    struct TagItem tags[3];
    tags[0].ti_Tag = OBP_Precision;  tags[0].ti_Data = PRECISION_IMAGE;
    tags[1].ti_Tag = OBP_FailIfBad;  tags[1].ti_Data = FALSE;
    tags[2].ti_Tag = TAG_DONE;       tags[2].ti_Data = 0;
    return ObtainBestPenA(cm, (ULONG)r * 0x01010101UL,
                          (ULONG)g * 0x01010101UL,
                          (ULONG)b * 0x01010101UL, tags);
}

static void aw_release_pens(aw_state *s)
{
    int i;
    if (!s->cm) return;
    for (i = 0; i < s->npens; i++)
        if (s->pens[i] >= 0) ReleasePen(s->cm, (ULONG)s->pens[i]);
    s->npens = 0;
    if (s->black_pen >= 0) ReleasePen(s->cm, (ULONG)s->black_pen);
    s->black_pen = -1;
}

/* Cube size per dither depth - see mr_dither_palette_indexed(). */
static int aw_cube_entries(int depth)
{
    return depth == 8 ? 6 * 6 * 6 : depth == 5 ? 4 * 4 * 2 : 2 * 4 * 2;
}

static int aw_obtain_pens(aw_state *s)
{
    uint8_t pal[256 * 3];
    int i, n = aw_cube_entries(s->depth), shared = 0;

    mr_dither_palette_indexed(pal, s->depth);
    memset(s->lut, 0, sizeof s->lut);
    for (i = 0; i < n; i++) {
        s->pens[i] = aw_obtain_pen(s->cm, pal[i * 3], pal[i * 3 + 1],
                                   pal[i * 3 + 2]);
        s->npens = i + 1;
        if (s->pens[i] < 0) return 0;
        s->lut[i] = (UBYTE)s->pens[i];
    }
    s->black_pen = aw_obtain_pen(s->cm, 0, 0, 0);
    if (s->black_pen < 0) return 0;

    if (g_display_want_time) {
        /* Count distinct pens: shared ones show how crowded the screen is. */
        int j;
        for (i = 0; i < n; i++)
            for (j = 0; j < i; j++)
                if (s->pens[j] == s->pens[i]) { shared++; break; }
        printf("aga-window: %d cube colours on %d distinct pens "
               "(dither depth %d)\n", n, n - shared, s->depth);
    }
    return 1;
}

static struct Window *aw_open_window(aw_state *s, const char *title)
{
    struct Screen *scr = LockPubScreen(NULL);
    struct Window *win;
    int win_w = s->source_w, win_h = s->source_h, scr_depth;

    if (!scr) return NULL;
    {
        int avail_w = scr->Width - scr->WBorLeft - scr->WBorRight;
        int avail_h = scr->Height - scr->BarHeight - 1 - scr->WBorTop -
                      scr->WBorBottom;
        if (avail_w < 80) avail_w = 80;
        if (avail_h < 60) avail_h = 60;
        aw_fit_within(s->source_w, s->source_h, avail_w, avail_h,
                      &win_w, &win_h);
    }
    /* Pick the dither depth from how many colours the screen can show at
     * all. More cube colours than the screen has pens would only pile onto
     * the same few shared pens. */
    scr_depth = (int)GetBitMapAttr(scr->RastPort.BitMap, BMA_DEPTH);
    s->depth = scr_depth >= 8 ? 8 : scr_depth >= 5 ? 5 : 4;

    win = OpenWindowTags(NULL,
        WA_PubScreen, (ULONG)scr,
        WA_Title, (ULONG)(title ? title : "MintVID"),
        WA_Left, 0, WA_Top, 0,
        WA_InnerWidth, (ULONG)win_w,
        WA_InnerHeight, (ULONG)win_h,
        WA_Flags, WFLG_DRAGBAR | WFLG_DEPTHGADGET | WFLG_CLOSEGADGET |
                  WFLG_SIZEGADGET | WFLG_ACTIVATE | WFLG_NOCAREREFRESH |
                  WFLG_SMART_REFRESH,
        WA_MinWidth, 80, WA_MinHeight, 60,
        WA_MaxWidth, (ULONG)-1, WA_MaxHeight, (ULONG)-1,
        WA_IDCMP, IDCMP_CLOSEWINDOW | IDCMP_RAWKEY | IDCMP_NEWSIZE,
        TAG_END);
    UnlockPubScreen(NULL, scr);
    return win;
}

static void aw_rebuild_geometry(aw_state *s)
{
    mr_aspect_rect fit;
    struct RastPort *rp = s->win->RPort;

    s->bl = s->win->BorderLeft;
    s->bt = s->win->BorderTop;
    s->iw = s->win->Width - s->win->BorderLeft - s->win->BorderRight;
    s->ih = s->win->Height - s->win->BorderTop - s->win->BorderBottom;
    if (s->iw < 1) s->iw = 1;
    if (s->ih < 1) s->ih = 1;
    s->pending_w = s->iw;
    s->pending_h = s->ih;

    fit = mr_aspect_fit(s->source_w, s->source_h, s->iw, s->ih);
    s->dx = fit.x; s->dy = fit.y; s->dw = fit.w; s->dh = fit.h;

    /* Black out the whole client area once; frames then only touch the
     * video rectangle. RectFill is clipped by the window's layer. */
    SetAPen(rp, (ULONG)s->black_pen);
    SetDrMd(rp, JAM1);
    RectFill(rp, s->bl, s->bt, s->bl + s->iw - 1, s->bt + s->ih - 1);

    s->geometry_valid = 1;
    s->force_full_redraw = 1;
    if (g_display_want_time)
        printf("aga-window geometry iw=%d ih=%d video=%d,%d %dx%d\n",
               s->iw, s->ih, s->dx, s->dy, s->dw, s->dh);
}

static int aw_ensure_strip(aw_state *s)
{
    int stride = (s->dw + 15) & ~15;   /* WritePixelArray8 row rounding */
    size_t need = (size_t)stride * AW_STRIP_ROWS;

    if (!s->strip || s->strip_size < need) {
        UBYTE *p = (UBYTE *)AllocVec(need, MEMF_ANY);
        if (!p) return 0;
        if (s->strip) FreeVec(s->strip);
        s->strip = p;
        s->strip_size = need;
    }
    s->strip_stride = stride;

    if (!s->use_wcp && (!s->tempbm || s->tempbm_w < stride)) {
        struct BitMap *scr_bm = s->win->WScreen->RastPort.BitMap;
        struct BitMap *bm = AllocBitMap((ULONG)stride, 1,
            GetBitMapAttr(scr_bm, BMA_DEPTH), 0, scr_bm);
        if (!bm) return 0;
        if (s->tempbm) { WaitBlit(); FreeBitMap(s->tempbm); }
        s->tempbm = bm;
        s->tempbm_w = stride;
        InitRastPort(&s->temprp);
        s->temprp.BitMap = s->tempbm;
    }
    return 1;
}

static void *aw_open(int w, int h, const char *title)
{
    aw_state *s;

    if (!IntuitionBase || !GfxBase || w <= 0 || h <= 0) return NULL;
    s = (aw_state *)AllocVec(sizeof *s, MEMF_CLEAR);
    if (!s) return NULL;
    s->black_pen = -1;
    s->half = g_aga_window == 2;
    s->source_w = s->half ? (w / 2 > 0 ? w / 2 : 1) : w;
    s->source_h = s->half ? (h / 2 > 0 ? h / 2 : 1) : h;
    s->use_wcp = ((struct Library *)GfxBase)->lib_Version >= 40;
    snprintf(s->title, sizeof s->title, "%s",
             (title && *title) ? title : "MintVID");

    s->win = aw_open_window(s, s->title);
    if (!s->win) { FreeVec(s); return NULL; }
    s->cm = s->win->WScreen->ViewPort.ColorMap;
    if (!s->cm || !aw_obtain_pens(s)) {
        printf("aga-window: could not obtain screen pens\n");
        aw_release_pens(s);
        CloseWindow(s->win);
        FreeVec(s);
        return NULL;
    }
    aw_rebuild_geometry(s);
    if (g_display_want_time)
        printf("aga-window: %s draw path, %s %dx%d\n",
               s->use_wcp ? "WriteChunkyPixels" : "WritePixelArray8",
               s->half ? "half size" : "full size",
               s->source_w, s->source_h);
    return s;
}

/* Remap (and nearest-neighbour scale) source cube indices into the strip,
 * then draw it. Source rows [sy0,sy1) changed; a scaled window redraws the
 * whole picture, a native-size one only those rows. */
static void aw_draw_indices(aw_state *s, const UBYTE *src, int sw, int sh,
                            int src_stride, int sy0, int sy1,
                            mr_display_service_fn service, void *opaque)
{
    struct RastPort *rp = s->win->RPort;
    clock_t total = 0, mark;
    int native, y0, y1, y;
    int timing = g_display_want_time;

    if (timing) {
        memset(&s->timing, 0, sizeof s->timing);
        total = clock();
    }
    if (!s->geometry_valid ||
        ((s->pending_w != s->iw || s->pending_h != s->ih) &&
         clock() - s->resize_at >= CLOCKS_PER_SEC / 10))
        aw_rebuild_geometry(s);
    if (!aw_ensure_strip(s)) {
        printf("aga-window: strip allocation failed - dropping frame\n");
        return;
    }

    native = s->dw == sw && s->dh == sh;
    if (s->force_full_redraw || !native) {
        y0 = 0; y1 = s->dh;
        s->force_full_redraw = 0;
    } else {
        y0 = sy0 < 0 ? 0 : sy0;
        y1 = sy1 > sh ? sh : sy1;
    }
    if (y1 <= y0) return;

    for (y = y0; y < y1; y += AW_STRIP_ROWS) {
        int rows = y1 - y < AW_STRIP_ROWS ? y1 - y : AW_STRIP_ROWS;
        int r;

        mark = timing ? clock() : 0;
        for (r = 0; r < rows; r++) {
            int dy = y + r;
            const UBYTE *srow = src + (size_t)(native ? dy :
                (int)((long)dy * sh / s->dh)) * (size_t)src_stride;
            UBYTE *drow = s->strip + (size_t)r * (size_t)s->strip_stride;
            const UBYTE *lut = s->lut;
            int x;
            if (native) {
                for (x = 0; x < sw; x++) drow[x] = lut[srow[x]];
            } else {
                int sx = 0, acc = 0, dw = s->dw;
                for (x = 0; x < dw; x++) {
                    drow[x] = lut[srow[sx]];
                    acc += sw;
                    while (acc >= dw) { acc -= dw; sx++; }
                }
            }
        }
        if (timing) { s->timing.convert_us += aw_elapsed_us(mark); mark = clock(); }

        if (s->use_wcp)
            WriteChunkyPixels(rp, (ULONG)(s->bl + s->dx),
                              (ULONG)(s->bt + s->dy + y),
                              (ULONG)(s->bl + s->dx + s->dw - 1),
                              (ULONG)(s->bt + s->dy + y + rows - 1),
                              s->strip, (LONG)s->strip_stride);
        else
            WritePixelArray8(rp, (ULONG)(s->bl + s->dx),
                             (ULONG)(s->bt + s->dy + y),
                             (ULONG)(s->bl + s->dx + s->dw - 1),
                             (ULONG)(s->bt + s->dy + y + rows - 1),
                             s->strip, &s->temprp);
        if (timing) {
            s->timing.blit_us += aw_elapsed_us(mark);
            s->timing.copies++;
            mark = clock();
        }
        if (service) service(opaque);
        if (timing) s->timing.service_us += aw_elapsed_us(mark);
    }
    if (timing) {
        s->timing.src_w = (unsigned)sw; s->timing.src_h = (unsigned)sh;
        s->timing.dst_w = (unsigned)s->dw; s->timing.dst_h = (unsigned)s->dh;
        s->timing.src_format = "INDEX";
        s->timing.dst_format = "PENS";
        s->timing.pixels = (unsigned long)s->dw * (unsigned long)(y1 - y0);
        s->timing.total_us = aw_elapsed_us(total);
    }
}

static void aw_show_indexed(void *h, const unsigned char *idx, int w, int hh,
                            int idx_stride, int dy0, int dy1,
                            mr_display_service_fn service, void *opaque)
{
    aw_state *s = (aw_state *)h;
    if (!s || !s->win || !idx || w <= 0 || hh <= 0) return;
    aw_draw_indices(s, idx, w, hh, idx_stride, dy0, dy1, service, opaque);
}

/* RGB24 entry for codecs that have no indexed output: dither the changed
 * rows into a source-size index buffer, then draw as above. */
static void aw_show(void *h, const unsigned char *rgb, int w, int hh,
                    int stride, int dy0, int dy1,
                    mr_display_service_fn service, void *opaque)
{
    aw_state *s = (aw_state *)h;
    size_t need;
    int y0, y1;

    if (!s || !s->win || !rgb || w <= 0 || hh <= 0) return;
    if (s->half && hh >= 2) {
        /* Only every other row can survive a halved picture, so dither
         * just those (the whole frame: the draw rescales it anyway). The
         * horizontal halving happens in the scaled draw. */
        int rows = hh / 2;
        need = (size_t)w * (size_t)rows;
        if (!s->idx || s->idx_size < need) {
            UBYTE *p = (UBYTE *)AllocVec(need, MEMF_ANY);
            if (!p) return;
            if (s->idx) FreeVec(s->idx);
            s->idx = p;
            s->idx_size = need;
        }
        mr_dither_rgb_indexed(rgb, w, rows, stride * 2, s->idx, w, 0,
                              s->depth);
        aw_draw_indices(s, s->idx, w, rows, w, 0, rows, service, opaque);
        return;
    }
    need = (size_t)w * (size_t)hh;
    if (!s->idx || s->idx_size < need) {
        UBYTE *p = (UBYTE *)AllocVec(need, MEMF_ANY);
        if (!p) return;
        if (s->idx) FreeVec(s->idx);
        s->idx = p;
        s->idx_size = need;
        dy0 = 0; dy1 = hh;          /* fresh buffer holds nothing yet */
    }
    y0 = dy0 < 0 ? 0 : dy0;
    y1 = dy1 > hh ? hh : dy1;
    if (y1 > y0)
        mr_dither_rgb_indexed(rgb + (size_t)y0 * (size_t)stride, w, y1 - y0,
                              stride, s->idx + (size_t)y0 * (size_t)w, w, y0,
                              s->depth);
    aw_draw_indices(s, s->idx, w, hh, w, y0, y1, service, opaque);
}

static int aw_supports_indexed(void *h, int *indexed_depth)
{
    aw_state *s = (aw_state *)h;
    if (!s) return 0;
    if (indexed_depth) *indexed_depth = s->depth;
    return 1;
}

/* H.264/MPEG-2 can dither straight from their YUV planes: at source size
 * normally (vscale 1), or straight to half size in half mode (vscale 0, the
 * general resize path). Any further window scaling happens in
 * aw_draw_indices(). */
static int aw_supports_yuv_indexed(void *h, int src_w, int src_h,
                                   int *dst_w, int *dst_h, int *vscale,
                                   int *indexed_depth, int *ham)
{
    aw_state *s = (aw_state *)h;
    if (!s || src_w <= 0 || src_h <= 0) return 0;
    if (s->half) {
        if (dst_w) *dst_w = src_w / 2 > 0 ? src_w / 2 : 1;
        if (dst_h) *dst_h = src_h / 2 > 0 ? src_h / 2 : 1;
        if (vscale) *vscale = 0;
    } else {
        if (dst_w) *dst_w = src_w;
        if (dst_h) *dst_h = src_h;
        if (vscale) *vscale = 1;
    }
    if (indexed_depth) *indexed_depth = s->depth;
    if (ham) *ham = 0;
    return 1;
}

static int aw_timing(void *h, mr_display_timing *timing)
{
    aw_state *s = (aw_state *)h;
    if (!s || !timing) return 0;
    *timing = s->timing;
    return 1;
}

static int aw_poll(void *h)
{
    aw_state *s = (aw_state *)h;
    struct IntuiMessage *msg;
    int ev = MR_EV_NONE;
    if (!s || !s->win) return MR_EV_QUIT;
    while ((msg = (struct IntuiMessage *)GetMsg(s->win->UserPort))) {
        ULONG cls = msg->Class; UWORD code = msg->Code;
        ReplyMsg((struct Message *)msg);
        if (cls == IDCMP_CLOSEWINDOW) s->quit = 1;
        else if (cls == IDCMP_NEWSIZE) {
            s->pending_w = s->win->Width - s->win->BorderLeft -
                           s->win->BorderRight;
            s->pending_h = s->win->Height - s->win->BorderTop -
                           s->win->BorderBottom;
            s->resize_at = clock();
        } else if (cls == IDCMP_RAWKEY && !(code & 0x80)) {
            switch (code) {
            case 0x45: s->quit = 1; break;             /* ESC           */
            case 0x40: ev = MR_EV_PAUSE; break;        /* space         */
            case 0x4E: ev = MR_EV_SEEK_FWD; break;     /* cursor right  */
            case 0x4F: ev = MR_EV_SEEK_BACK; break;    /* cursor left   */
            case 0x4C: ev = MR_EV_VOLUME_UP; break;    /* cursor up     */
            case 0x4D: ev = MR_EV_VOLUME_DOWN; break;  /* cursor down   */
            }
        }
    }
    return s->quit ? MR_EV_QUIT : ev;
}

static void aw_status(void *h, const char *text)
{
    aw_state *s = (aw_state *)h;
    const char *want = (text && *text) ? text : "MintVID";
    if (!s || !s->win) return;
    if (strcmp(s->title, want) == 0) return;
    snprintf(s->title, sizeof s->title, "%s", want);
    SetWindowTitles(s->win, (CONST_STRPTR)s->title, (CONST_STRPTR)~0UL);
}

static ULONG aw_wait_mask(void *h)
{
    aw_state *s = (aw_state *)h;
    if (!s || !s->win || !s->win->UserPort) return 0;
    return 1UL << s->win->UserPort->mp_SigBit;
}

static void aw_close(void *h)
{
    aw_state *s = (aw_state *)h;
    struct IntuiMessage *msg;
    if (!s) return;
    if (s->win) {
        ModifyIDCMP(s->win, 0);
        if (s->win->UserPort)
            while ((msg = (struct IntuiMessage *)GetMsg(s->win->UserPort)))
                ReplyMsg((struct Message *)msg);
        WaitBlit();
        /* Pens belong to the screen's ColorMap, which outlives the window;
         * release them while the window still pins the screen open. */
        aw_release_pens(s);
        CloseWindow(s->win);
        s->win = NULL;
    }
    if (s->tempbm) { WaitBlit(); FreeBitMap(s->tempbm); }
    if (s->strip) FreeVec(s->strip);
    if (s->idx) FreeVec(s->idx);
    FreeVec(s);
}

const display_backend backend_aga_window = {
    .name = "Window (Workbench)",
    .open = aw_open,
    .show = aw_show,
    .timing = aw_timing,
    .poll = aw_poll,
    .close = aw_close,
    .status = aw_status,
    .wait_mask = aw_wait_mask,
    .supports_indexed = aw_supports_indexed,
    .show_indexed = aw_show_indexed,
    .supports_yuv_indexed = aw_supports_yuv_indexed
    /* No toggle_fullscreen: the point of this mode is sharing the
     * Workbench. The plain AGA mode is the fullscreen equivalent. */
};
