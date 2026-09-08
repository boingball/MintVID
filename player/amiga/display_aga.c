/*
 * MintVID - AGA (planar) display backend.
 *
 * Opens a custom screen and blits each frame through a portable pixel encoder
 * (16/32/256-colour dither / HAM8 / HAM6) plus a chunky->planar step. The default
 * CPU-matched Kalms conversion is the default. --wpa selects WritePixelArray8,
 * --c2p selects the portable mr_c2p8, and --riva-c2p selects an opt-in
 * 32-pixel, direct-to-plane variant for hardware measurement. Kalms includes
 * fused 2x2 output and a 040/060 HAM6 path where the geometry permits it; an
 * incompatible bitmap or geometry falls back safely to WritePixelArray8.
 */
#include "amiga_display.h"
#include "display_backend.h"
#include "mr_akiko.h"
#include "../core/mr_dither.h"
#include "../core/mr_ham.h"
#include "../core/mr_scale.h"
#include "../core/mr_c2p.h"
#ifdef MR_KALMS_040
#include "../vendor/kalms-c2p/normal/c2p1x1_8_c5_040.h"
#include "../vendor/kalms-c2p/bitmap/c2p1x1_6_c5_bm_040.h"
#else
#include "../vendor/kalms-c2p/normal/c2p1x1_8_c5_030.h"
#endif
#include "../vendor/kalms-c2p/bitmap/c2p2x2_8_c5_bm.h"

#include <exec/types.h>
#include <intuition/intuition.h>
#include <intuition/screens.h>
#include <graphics/gfx.h>
#include <graphics/gfxbase.h>
#include <graphics/rastport.h>
#include <graphics/view.h>
#include <graphics/displayinfo.h>

#include <proto/exec.h>
#include <proto/intuition.h>
#include <proto/graphics.h>

#include <stdlib.h>
#include <string.h>
#include <time.h>

#define ESC_RAWKEY 0x45

enum {
    KALMS_NONE = 0,
    KALMS_1X1_8,
    KALMS_2X2_8,
    KALMS_1X1_6
};

/* Encode vs blit time, for mrplay --time. */
static clock_t s_enc = 0, s_blit = 0, s_frame_enc = 0, s_frame_blit = 0;
static int s_kalms_active = 0;
void display_aga_timing(unsigned long *enc_ms, unsigned long *blit_ms)
{
    if (enc_ms)  *enc_ms  = (unsigned long)(s_enc  * 1000 / CLOCKS_PER_SEC);
    if (blit_ms) *blit_ms = (unsigned long)(s_blit * 1000 / CLOCKS_PER_SEC);
}
void display_aga_frame_timing(unsigned long *enc_ms, unsigned long *blit_ms)
{
    if (enc_ms)  *enc_ms  = (unsigned long)(s_frame_enc  * 1000 / CLOCKS_PER_SEC);
    if (blit_ms) *blit_ms = (unsigned long)(s_frame_blit * 1000 / CLOCKS_PER_SEC);
}

/* Effective (post-negotiation) mode of the currently-open AGA screen, for
 * mrplay --time's "AGA path:" diagnostic. Deliberately captures what
 * aga_open() actually settled on (e.g. HAM8 downgraded to HAM6 on non-AGA
 * chipset, ECS/OCS forcing depth 5, a requested resolution that did or did
 * not need a resize) rather than the raw --ham/--2x/--c2p command-line
 * request, since aga_supports_indexed()/aga_supports_yuv_indexed() gate on
 * exactly these effective fields and a mismatch between what was asked for
 * and what was granted is the whole point of the diagnostic. */
static int s_diag_depth = -1, s_diag_ham = 0, s_diag_scale = 1, s_diag_resize = 0;
static const char *s_diag_c2p = "standard";
void display_aga_describe(int *depth, int *ham, int *scale, int *resize,
                          const char **c2p)
{
    if (depth)  *depth  = s_diag_depth;
    if (ham)    *ham    = s_diag_ham;
    if (scale)  *scale  = s_diag_scale;
    if (resize) *resize = s_diag_resize;
    if (c2p)    *c2p    = s_diag_c2p;
}

int display_aga_kalms_timing(unsigned long *conversion_ms)
{
    if (conversion_ms)
        *conversion_ms = (unsigned long)(s_blit * 1000 / CLOCKS_PER_SEC);
    return s_kalms_active;
}

typedef struct {
    struct Screen  *scr;
    struct Window  *win;
    struct RastPort temprp;      /* only used on the WritePixelArray8 path  */
    struct BitMap  *tempbm;
    unsigned char  *enc;         /* 1x encode buffer (scale==2 only)        */
    unsigned char  *enc_alloc;   /* allocation base for aligned enc         */
    unsigned char  *chunky;      /* pw*dh pixels to blit                     */
    unsigned char  *chunky_alloc;/* allocation base for aligned chunky      */
    unsigned char  *scaled;      /* resized RGB (resize only)                */
    int             w, h;
    int             dw, dh;      /* displayed size                          */
    int             pw;          /* chunky row stride (>= dw)               */
    int             depth;
    int             x0, y0, x0byte;
    int             ham, scale, resize, use_c2p, use_riva_c2p, use_akiko;
    int             kalms_kind;
    long            kalms_plane_spacing;
    int             quit;
} aga_state;

/* Kalms recommends a 16-byte chunky-buffer alignment on the 040/060 and
 * bitmap kernels. Keep the original allocation pointer so ordinary free()
 * remains valid on every supported Amiga C library. */
static unsigned char *alloc_aligned16(size_t bytes, unsigned char **allocation)
{
    unsigned char *raw = (unsigned char *)calloc(bytes + 15, 1);
    if (!raw) { *allocation = NULL; return NULL; }
    *allocation = raw;
    return (unsigned char *)(((ULONG)(raw + 15)) & ~15UL);
}

/*
 * CD32 Akiko hardware chunky->planar. The Akiko chip exposes a C2P port at
 * 0xB80038: feed 8 chunky longwords (32 pixels), then read back 8 planar
 * longwords (one 32-bit slice per bitplane), and store each into its plane.
 * This offloads the transpose from the 020, which is the win on a stock CD32.
 *
 * pw must be a multiple of 32 and x0byte a multiple of 4 (aga_open enforces
 * both when Akiko is active), so every plane store lands longword-aligned.
 *
 * NOTE: the plane/bit ordering of the Akiko handshake here is reconstructed
 * from documentation - it needs verifying on real CD32 hardware and may need
 * the read order (or a bit reversal) tweaked if the picture comes out garbled.
 */
#define AKIKO_C2P_REG 0xB80038
static void akiko_c2p(const uint8_t *chunky, int pw, int h, int chunky_stride,
                      int nplanes, uint8_t *const planes[], int bpr,
                      int x0byte, int y0)
{
    volatile ULONG *ak = (volatile ULONG *)AKIKO_C2P_REG;
    int nbatch = pw >> 5;                          /* 32 pixels per batch     */
    int y;
    for (y = 0; y < h; y++) {
        const ULONG *src = (const ULONG *)(chunky + (size_t)y * chunky_stride);
        int dstrow = (y0 + y) * bpr + x0byte;
        int b;
        for (b = 0; b < nbatch; b++) {
            ULONG planeword[8];
            int i, p;
            for (i = 0; i < 8; i++) *ak = *src++;        /* feed 32 chunky px */
            for (i = 0; i < 8; i++) planeword[i] = *ak;  /* 8 planar slices   */
            for (p = 0; p < nplanes; p++)
                *(ULONG *)(planes[p] + dstrow + b * 4) = planeword[p];
        }
    }
}

static int chipset_has_aga(void)
{
    return GfxBase && (GfxBase->ChipRevBits0 & GFXF_AA_LISA) != 0;
}

static void load_palette(struct Screen *scr, int ham, int depth)
{
    ULONG tab[1 + 256 * 3 + 1];
    int   n, i;
    uint8_t pal[256 * 3];
    if (ham) { n = (ham >= 8) ? 64 : 16; mr_ham_palette(pal, ham); }
    else {
        n = depth == 4 ? 16 : depth == 5 ? 32 : 256;
        mr_dither_palette_indexed(pal, depth);
    }
    tab[0] = ((ULONG)n << 16) | 0;
    for (i = 0; i < n; i++) {
        ULONG r = pal[i*3+0], g = pal[i*3+1], b = pal[i*3+2];
        tab[1 + i*3 + 0] = (r << 24) | (r << 16) | (r << 8) | r;
        tab[1 + i*3 + 1] = (g << 24) | (g << 16) | (g << 8) | g;
        tab[1 + i*3 + 2] = (b << 24) | (b << 16) | (b << 8) | b;
    }
    tab[1 + n*3] = 0;
    LoadRGB32(&scr->ViewPort, tab);
}

static void *aga_open(int w, int h, const char *title)
{
    aga_state *s;
    int   scale = (g_aga_scale == 2) ? 2 : 1;
    int   ham   = g_aga_ham;
    int   akiko = g_aga_akiko && mr_akiko_available();
    int   riva_c2p_mode = (g_aga_c2p == 2) && !akiko;
    int   kalms_c2p_mode = (g_aga_c2p == 3) && !akiko;
    int   c2p   = (g_aga_c2p == 1) && !akiko;
    int   dw, dh, depth;
    int   sw, sh, physical_w, physical_h, hires, lace, resize;
    ULONG modeid;

    s_enc = s_blit = s_frame_enc = s_frame_blit = 0;
    s_kalms_active = 0;

    if (g_aga_akiko && !akiko)
        printf("planar: Akiko not detected; using CPU C2P\n");

    /* HAM8 and eight indexed planes need AGA. HAM6 is the original Amiga HAM
     * mode and works on OCS/ECS too. Ordinary OCS/ECS output uses a genuine
     * five-plane/32-colour encoder instead of trying to open an impossible
     * eight-plane screen and silently losing the upper index bits. */
    if (ham == 8 && !chipset_has_aga()) {
        printf("planar: HAM8 requires AGA; using HAM6\n");
        ham = 6;
    }
    depth = ham == 6 ? 6 :
            g_aga_ecs_fast ? 4 :
            g_aga_ecs32 ? 5 :
            (chipset_has_aga() ? 8 : 5);

    /*
     * AGA raster pixels are not square: HIRES doubles horizontal resolution
     * and interlace doubles vertical resolution without changing the physical
     * display size.  Fit in a 320x256 "physical pixel" canvas first, then
     * multiply the corresponding raster axis.  Thus 854x480 becomes 640x180
     * in non-laced HIRES, or 640x360 with --lace, instead of the squeezed
     * 427x240 image produced by treating HIRES pixels as square.
     */
    hires = w * scale > 320;
    /* ECS/OCS Denise+Agnus cannot fetch more than 4 bitplanes' worth of data
     * per scanline at HIRES pixel rates - a genuine display DMA bandwidth
     * limit, not something AGA needed since it redesigned the fetch path.
     * This backend's non-AGA paths normally use depth 5 (the 4x4x2 cube, the
     * ECS/OCS 32-colour cube) or depth 6 (HAM6), both invalid HIRES bitplane
     * counts on real ECS/OCS hardware - OpenScreenTags simply fails for that
     * combination. Confirmed on real ECS hardware: a live stream narrow
     * enough to stay LORES opened fine; two wider ones that crossed the 320
     * threshold into HIRES both failed to open a display at all. Keep ECS/
     * OCS in LORES (the picture is downscaled a bit more instead of not
     * appearing) unless depth is already within the real 4-plane HIRES
     * limit - --ecs-fast's 4-plane encoder qualifies on any chipset. */
    if (!chipset_has_aga() && depth > 4) hires = 0;
    lace = g_aga_lace && h * scale > 256;
    physical_w = w * scale;
    physical_h = h * scale;
    mr_scale_fit_rect(physical_w, physical_h, 320, 256,
                      &physical_w, &physical_h);
    dw = physical_w * (hires ? 2 : 1);
    dh = physical_h * (lace ? 2 : 1);

    /* Preserve the cheap encoded-chunky 2x path when the geometry is exactly
     * 2x.  All aspect compensation goes through the general RGB resizer. */
    if (dw == w * 2 && dh == h * 2) {
        scale = 2;
        resize = 0;
    } else {
        scale = 1;
        resize = (dw != w || dh != h);
    }

    sw = hires ? 640 : 320;
    sh = lace ? ((dh + 15) & ~15) : 256;
    if (sh < 256) sh = 256;
    modeid = hires ? HIRES_KEY : LORES_KEY;
    if (lace) modeid |= LACE;
    if (ham) modeid |= HAM;
    (void)title;

    s = (aga_state *)calloc(1, sizeof *s);
    if (!s) return NULL;
    s->w = w; s->h = h; s->dw = dw; s->dh = dh;
    s->ham = ham; s->scale = scale; s->resize = resize;
    s->depth = depth; s->use_c2p = c2p; s->use_riva_c2p = riva_c2p_mode;
    s->use_akiko = akiko;
    s->kalms_kind = KALMS_NONE;

    if (kalms_c2p_mode && depth == 8 && scale == 2 && !resize &&
        (w & 15) == 0)
        s->kalms_kind = KALMS_2X2_8;
    else if (kalms_c2p_mode && depth == 8)
        s->kalms_kind = KALMS_1X1_8;
#ifdef MR_KALMS_040
    else if (kalms_c2p_mode && depth == 6)
        s->kalms_kind = KALMS_1X1_6;
#endif
    /* Akiko converts 32 pixels per batch, so it needs a 32-pixel-aligned x and
     * a 32-multiple row stride; the built-in C2P only needs 8-pixel alignment.
     * graphics.library WritePixelArray8 requires each source row rounded up to
     * 16 pixels even though xstop names the unpadded visible width.  Packing an
     * odd width tightly makes every following row start early (854 / 2 = 427
     * exposed this as five-pixel diagonal wraps). */
    if (s->kalms_kind == KALMS_1X1_8)
                   { s->pw = sw; s->x0 = ((sw - dw) / 2) & ~31; }
    else if (s->kalms_kind == KALMS_1X1_6)
                   { s->pw = (dw + 31) & ~31; s->x0 = ((sw - dw) / 2) & ~31; }
    else if (s->kalms_kind == KALMS_2X2_8)
                   { s->pw = dw; s->x0 = ((sw - dw) / 2) & ~7; }
    else if (akiko || riva_c2p_mode)
                   { s->pw = (dw + 31) & ~31; s->x0 = ((sw - dw) / 2) & ~31; }
    else if (c2p)  { s->pw = (dw + 7)  & ~7;  s->x0 = ((sw - dw) / 2) & ~7;  }
    else           { s->pw = (dw + 15) & ~15; s->x0 = (sw - dw) / 2;         }
    if (s->x0 < 0) s->x0 = 0;
    s->y0 = (sh - dh) / 2;
    s->x0byte = s->x0 >> 3;

    s->scr = OpenScreenTags(NULL,
        SA_Width, (ULONG)sw, SA_Height, (ULONG)sh, SA_Depth, (ULONG)depth,
        SA_DisplayID, modeid, SA_Type, CUSTOMSCREEN,
        SA_Quiet, TRUE, SA_ShowTitle, FALSE, TAG_END);
    if (!s->scr) { free(s); return NULL; }
    load_palette(s->scr, ham, depth);

    s->win = OpenWindowTags(NULL,
        WA_CustomScreen, (ULONG)s->scr,
        WA_Left, 0, WA_Top, 0, WA_Width, (ULONG)sw, WA_Height, (ULONG)sh,
        WA_Flags, WFLG_BORDERLESS | WFLG_BACKDROP | WFLG_ACTIVATE |
                  WFLG_RMBTRAP | WFLG_NOCAREREFRESH,
        WA_IDCMP, IDCMP_RAWKEY, TAG_END);
    if (!s->win) { CloseScreen(s->scr); free(s); return NULL; }

    if (s->kalms_kind != KALMS_NONE) {
        struct BitMap *bm = s->scr->RastPort.BitMap;
        long spacing = 0;
        int p, planes = s->kalms_kind == KALMS_1X1_6 ? 6 : 8;
        int compatible = bm && bm->Depth >= planes &&
                         s->x0 + s->dw <= bm->BytesPerRow * 8;
        for (p = 0; compatible && p < planes; p++)
            compatible = bm->Planes[p] != NULL;

        if (compatible && s->kalms_kind == KALMS_1X1_8) {
            compatible = bm->BytesPerRow == s->pw / 8;
            if (compatible) {
                spacing = (long)((ULONG)bm->Planes[1] - (ULONG)bm->Planes[0]);
                compatible = spacing > 0;
#ifndef MR_KALMS_040
                /* The 030 kernel patches a 16-bit plane displacement. */
                compatible = compatible && spacing <= 16384;
#endif
            }
            for (p = 1; compatible && p < 8; p++)
                compatible = (UBYTE *)bm->Planes[p] ==
                             (UBYTE *)bm->Planes[0] + (size_t)p * spacing;
        }
        if (compatible && s->kalms_kind == KALMS_1X1_6)
            compatible = (s->pw & 31) == 0 && (s->x0 & 7) == 0;
        if (compatible && s->kalms_kind == KALMS_2X2_8)
            compatible = (w & 15) == 0 && (s->x0 & 7) == 0;

        if (compatible) {
            s->kalms_plane_spacing = spacing;
            /* Set the 1x1 plane displacement once. The cheap non-SMC init is
             * repeated for each dirty row range immediately before conversion. */
            if (s->kalms_kind == KALMS_1X1_8) {
#ifdef MR_KALMS_040
                c2p1x1_8_c5_040_init(s->pw, s->dh, s->y0, spacing);
#else
            c2p1x1_8_c5_030_smcinit(s->pw, s->dh, s->y0, spacing);
#endif
            }
            s_kalms_active = 1;
        } else {
            s->kalms_kind = KALMS_NONE;
            /* An incompatible planar allocation must never reach Kalms: use
             * the established graphics.library path instead. WPA derives its
             * source-row modulo from the visible width, so restore the normal
             * WPA padding rather than retaining Kalms' screen-wide stride.
             * Keeping the latter made HAM6 (which necessarily has only six
             * planes) read each following chunky row from the wrong offset. */
            s->pw = (dw + 15) & ~15;
            s->x0 = (sw - dw) / 2;
            if (s->x0 < 0) s->x0 = 0;
            s->x0byte = s->x0 >> 3;
        }
    }

    /* padded + cleared so C2P's pad columns are black */
    if (s->kalms_kind != KALMS_2X2_8) {
        s->chunky = alloc_aligned16((size_t)s->pw * dh, &s->chunky_alloc);
        if (!s->chunky) goto fail;
    }
    if (scale == 2) {
        s->enc = alloc_aligned16((size_t)w * h, &s->enc_alloc);
        if (!s->enc) goto fail;
    }
    if (resize) {
        s->scaled = (unsigned char *)malloc((size_t)dw * dh * 3);
        if (!s->scaled) goto fail;
    }
    if (!c2p && !riva_c2p_mode && s->kalms_kind == KALMS_NONE && !akiko) {
        /* WPA path, including any unsupported/incompatible Kalms request. */
        s->tempbm = AllocBitMap((ULONG)s->pw, 1, (ULONG)depth, 0,
                                s->scr->RastPort.BitMap);
        if (!s->tempbm) goto fail;
        InitRastPort(&s->temprp);
        s->temprp.BitMap = s->tempbm;
    }
    s_diag_depth = s->depth; s_diag_ham = s->ham; s_diag_scale = s->scale;
    s_diag_resize = s->resize;
    s_diag_c2p = s->kalms_kind == KALMS_2X2_8 ? "kalms-2x2" :
                s->kalms_kind == KALMS_1X1_6 ? "kalms-ham6" :
                s->kalms_kind == KALMS_1X1_8 ?
#ifdef MR_KALMS_040
                                               "kalms-040" :
#else
                                               "kalms-030" :
#endif
                s->use_akiko      ? "akiko" :
                s->use_riva_c2p   ? "riva"  :
                s->use_c2p        ? "c2p"   : "wpa";
    return s;

fail:
    s_kalms_active = 0;
    if (s->enc_alloc) free(s->enc_alloc);
    if (s->scaled) free(s->scaled);
    if (s->chunky_alloc) free(s->chunky_alloc);
    if (s->tempbm) FreeBitMap(s->tempbm);
    CloseWindow(s->win);
    CloseScreen(s->scr);
    free(s);
    return NULL;
}

/*
 * Chunky-to-planar + blit. src[ddy0..ddy0+ddh) must hold this frame's
 * dithered/HAM-encoded pixels at src_stride bytes/row (Kalms ignores src -
 * see below). Two callers: aga_show() passes s->chunky/s->pw after encoding
 * into it itself; aga_show_indexed() passes either s->chunky/s->pw (after
 * copying in already-dithered rows) or, for the common aligned case, the
 * caller's own already-dithered buffer directly - see that function.
 */
static void aga_blit(aga_state *s, const uint8_t *src, int src_stride,
                     int ddy0, int ddh)
{
    int pw = s->pw, dw = s->dw;
    clock_t a = 0;
    if (g_display_want_time) a = clock();
    {
    const uint8_t *crow = src + (size_t)ddy0 * src_stride;
    if (s->use_akiko) {
        struct BitMap *bm = s->scr->RastPort.BitMap;
        akiko_c2p(crow, pw, ddh, src_stride, s->depth,
                  (uint8_t *const *)bm->Planes, bm->BytesPerRow,
                  s->x0byte, s->y0 + ddy0);
    } else if (s->kalms_kind == KALMS_1X1_8) {
        struct BitMap *bm = s->scr->RastPort.BitMap;
        /* Both normal kernels consume a tightly packed rectangle. Since the
         * Kalms 1x1 layout uses a screen-width chunky stride, a dirty row band
         * is itself tightly packed and can be converted without untouched rows. */
#ifdef MR_KALMS_040
        c2p1x1_8_c5_040_init(pw, ddh, s->y0 + ddy0,
                             s->kalms_plane_spacing);
        c2p1x1_8_c5_040((void *)crow, bm->Planes[0]);
#else
        c2p1x1_8_c5_030_init(pw, ddh, s->y0 + ddy0);
        c2p1x1_8_c5_030((void *)crow, bm->Planes[0]);
#endif
#ifdef MR_KALMS_040
    } else if (s->kalms_kind == KALMS_1X1_6) {
        struct BitMap *bm = s->scr->RastPort.BitMap;
        c2p1x1_6_c5_bm_040(pw, ddh, s->x0, s->y0 + ddy0,
                           (void *)crow, bm);
#endif
    } else if (s->use_riva_c2p) {
        struct BitMap *bm = s->scr->RastPort.BitMap;
        mr_c2p8_riva32(crow, pw, ddh, src_stride, s->depth,
                       (uint8_t *const *)bm->Planes, bm->BytesPerRow,
                       s->x0byte, s->y0 + ddy0);
    } else if (s->use_c2p) {
        struct BitMap *bm = s->scr->RastPort.BitMap;
        mr_c2p8(crow, pw, ddh, src_stride, s->depth,
                (uint8_t *const *)bm->Planes, bm->BytesPerRow,
                s->x0byte, s->y0 + ddy0);
    } else {
        WritePixelArray8(&s->scr->RastPort,
                         (UWORD)s->x0, (UWORD)(s->y0 + ddy0),
                         (UWORD)(s->x0 + dw - 1), (UWORD)(s->y0 + ddy0 + ddh - 1),
                         (UBYTE *)crow, &s->temprp);
    }
    }
    /* Kalms is timed and accumulated by the same counters as every other AGA
     * blit backend. Keep diagnostics out of this frame-rendering hot path. */
    if (g_display_want_time) { s_frame_blit = clock() - a; s_blit += s_frame_blit; }
}

/* Fused encoded-pixel 2x2 enlargement + C2P. The source contains only the
 * dirty rows, so the output offset advances by twice the source row number. */
static void aga_blit_kalms2x2(aga_state *s, const uint8_t *src,
                              int src_rows, int src_y)
{
    clock_t a = 0;
    struct BitMap *bm = s->scr->RastPort.BitMap;
    if (g_display_want_time) a = clock();
    c2p2x2_8_c5_bm(s->w, src_rows, s->x0, s->y0 + src_y * 2,
                   (void *)src, bm);
    if (g_display_want_time) {
        s_frame_blit = clock() - a;
        s_blit += s_frame_blit;
    }
}

static void aga_show(void *handle, const unsigned char *rgb, int w, int h,
                     int stride, int dy0, int dy1,
                     mr_display_service_fn service, void *service_opaque)
{
    aga_state *s = (aga_state *)handle;
    int pw = s->pw, dw = s->dw, sc = s->scale;
    int ddy0, ddh;
    if (!s || !s->scr) return;
    s_frame_enc = s_frame_blit = 0;

    /* clock() around encode/blit only ever feeds display_aga_timing()/
     * display_aga_frame_timing(), read solely by mrplay.c's --time report -
     * see display_cgx.c/display_p96.c, which already gate their own timing
     * on g_display_want_time. Skip both clock() calls on a normal run so
     * every frame's encode+blit is not paying for two clock() reads nobody
     * asked for. */
    { clock_t a = 0;
    if (g_display_want_time) a = clock();
    if (s->resize) {
        /* Resize the whole frame, then encode it. Dirty rows do not map
         * cleanly through arbitrary scaling, and the fitted image is small. */
        mr_scale_resize_rgb24(rgb, w, h, stride, s->scaled,
                              dw, s->dh, dw * 3);
        uint8_t *encoded = s->chunky +
                           (s->kalms_kind == KALMS_1X1_8 ? s->x0 : 0);
        if (s->ham) mr_ham_encode(s->scaled, dw, s->dh, dw * 3, encoded, pw, s->ham);
        else mr_dither_rgb_indexed(s->scaled, dw, s->dh, dw * 3,
                                   encoded, pw, 0, s->depth);
        ddy0 = 0; ddh = s->dh;
    } else {
        /* Encode only the changed source rows [dy0,dy1); the screen keeps the
         * rest. (HAM rows are independent; dither is told its y_base.) */
        const uint8_t *src;
        int rows;
        if (dy0 < 0) dy0 = 0;
        if (dy1 > h)  dy1 = h;
        if (dy1 <= dy0) {
            if (g_display_want_time) { s_frame_enc = clock() - a; s_enc += s_frame_enc; }
            return;
        }
        src = rgb + (size_t)dy0 * stride;
        rows = dy1 - dy0;
        if (sc == 2) {
            if (s->ham) mr_ham_encode(src, w, rows, stride, s->enc, w, s->ham);
            else mr_dither_rgb_indexed(src, w, rows, stride, s->enc, w,
                                       dy0, s->depth);
            if (s->kalms_kind != KALMS_2X2_8)
                mr_scale2x_u8(s->enc, w, rows, w,
                              s->chunky + (size_t)(dy0*2) * pw +
                              (s->kalms_kind == KALMS_1X1_8 ? s->x0 : 0), pw);
            ddy0 = dy0 * 2; ddh = rows * 2;
        } else {
            uint8_t *dst = s->chunky + (size_t)dy0 * pw +
                           (s->kalms_kind == KALMS_1X1_8 ? s->x0 : 0);
            if (s->ham) mr_ham_encode(src, w, rows, stride, dst, pw, s->ham);
            else mr_dither_rgb_indexed(src, w, rows, stride, dst, pw,
                                       dy0, s->depth);
            ddy0 = dy0; ddh = rows;
        }
    }
    if (g_display_want_time) { s_frame_enc = clock() - a; s_enc += s_frame_enc; } }

    if (s->kalms_kind == KALMS_2X2_8)
        aga_blit_kalms2x2(s, s->enc, ddh / 2, ddy0 / 2);
    else
        aga_blit(s, s->chunky, pw, ddy0, ddh);
}

/*
 * Non-zero for a plain 4-, 5- or 8-plane indexed configuration: no HAM, no
 * pixel doubling, no resize. That is exactly when aga_show() would otherwise
 * call mr_dither_rgb_indexed() itself with a 1:1 row mapping - here
 * the caller (mrplay.c's decode-ahead queue) has already done that dither
 * once, earlier, into the queue slot, and hands us the indexed rows directly
 * instead of paying for a second dither pass over the same pixels.
 */
static int aga_supports_indexed(void *handle, int *indexed_depth)
{
    aga_state *s = (aga_state *)handle;
    if (!s || s->ham || s->scale != 1 || s->resize ||
        (s->depth != 4 && s->depth != 5 && s->depth != 8)) return 0;
    if (indexed_depth) *indexed_depth = s->depth;
    return 1;
}

/*
 * Non-zero whenever this AGA geometry can go straight from H.264's decoded
 * YUV420P planes to chunky pixels instead of the ordinary
 * mr_yuv420_to_rgb24() -> mr_scale_resize_rgb24() -> encode pipeline.
 *
 * Two encodings qualify, reported through *ham:
 *
 *   - *ham == 0: any plain 4-, 5- or 8-plane indexed configuration, via
 *     core/mr_yuv_dither.h's ordered dither;
 *   - *ham == 6 or 8: a HAM6/HAM8 screen, via core/mr_yuv_ham.h, and only
 *     for the exact vertical-downscale shape. Hold-and-modify resets at every
 *     row start, so it fuses with YUV->RGB just as the dither does, and the
 *     resulting HAM pixel bytes are one chunky byte per pixel that reach the
 *     screen through the same aga_show_indexed() copy, C2P and blit as
 *     palette indices. The restriction to one shape is a measured one, not a
 *     structural limit: the three-stage path aga_show() uses converts YUV to
 *     RGB in hand-written assembly, and only where the fused encoder converts
 *     *fewer samples* - the surviving rows of a vertical downscale - does it
 *     beat that reliably. See core/mr_yuv_ham.h.
 *
 * Pixel doubling is not offered for HAM (see the s->scale test below): the
 * scale==2 shapes go through aga_show()'s own encode-then-mr_scale2x_u8()
 * ordering, and the Kalms 2x2 kernel enlarges while converting, neither of
 * which this has any way to reproduce for HAM control bytes.
 *
 * Three geometry shapes exist (HAM takes only the second):
 *
 *   - identity, no resize at all (s->dw==src_w, s->dh==src_h) - reported as
 *     vscale == 1 (mr_yuv420_dither_indexed() accepts it as a valid no-op,
 *     already covered by tests/mr_yuv_dither_check.c and its m68k asm).
 *     Skipping straight from decode to indexed is strictly cheaper than
 *     decoding to RGB24 first and dithering that (aga_supports_indexed()'s
 *     queue_copy_indexed() path) even when nothing needs to shrink or grow,
 *     so a caller should prefer this over aga_supports_indexed() whenever
 *     both apply - the two are no longer mutually exclusive at 1:1, unlike
 *     the two resize shapes below (which do require s->resize, hence still
 *     exclude aga_supports_indexed()'s !resize);
 *   - the exact *vertical-only* integer downscale (width unchanged, height
 *     an exact multiple of s->dh) that aga_open()'s resize computation
 *     produces for typical HIRES non-laced playback (e.g. a 640x360 source
 *     fits 640x180) - reported as vscale >= 2, for the LUT-generic
 *     hand-tuned m68k asm fast path;
 *   - every other resize shape (any horizontal change, any non-integer
 *     ratio, upscale as well as downscale - e.g. the BBC HLS mobile
 *     variant's 192x108 fitted up to a 320x180 AGA screen) - reported as
 *     vscale == 0, for mr_yuv420_dither_indexed_resize()'s general 2D
 *     nearest-neighbour path (portable C only, no asm yet).
 */
static int aga_supports_yuv_indexed(void *handle, int src_w, int src_h,
                                    int *dst_w, int *dst_h, int *vscale,
                                    int *indexed_depth, int *ham)
{
    aga_state *s = (aga_state *)handle;
    if (!s) return 0;
    if (!s->ham && s->depth != 4 && s->depth != 5 && s->depth != 8) return 0;
    if (src_w <= 0 || src_h <= 0) return 0;
    if (indexed_depth) *indexed_depth = s->depth;
    if (ham) *ham = s->ham;
    /* Kalms' fused 2x2 converter consumes the source-sized indexed image and
     * performs the enlargement while producing the bitplanes. Let H.264 use
     * the existing direct YUV420P -> indexed path here too: this removes the
     * RGB24 queue/intermediate and the later RGB -> indexed pass, in addition
     * to the scale pass that the Kalms kernel already removed. HAM never
     * selects this kernel (it needs depth 8 and scale 2, and the HAM shapes
     * below require scale 1). */
    if (!s->ham && s->kalms_kind == KALMS_2X2_8 &&
        src_w == s->w && src_h == s->h) {
        *dst_w = src_w; *dst_h = src_h; *vscale = 1;
        return 1;
    }
    if (s->scale != 1) return 0;
    if (!s->resize) {
        if (s->ham) return 0;
        *dst_w = s->w; *dst_h = s->h; *vscale = 1;
        return 1;
    }
    if (s->dw <= 0 || s->dh <= 0) return 0;
    *dst_w = s->dw; *dst_h = s->dh;
    if (s->dw == src_w && s->dh > 0 && src_h % s->dh == 0) {
        int vs = src_h / s->dh;
        if (vs > 1) { *vscale = vs; return 1; }
    }
    if (s->ham) return 0;
    *vscale = 0;
    return 1;
}

static void aga_show_indexed(void *handle, const unsigned char *idx, int w,
                             int h, int idx_stride, int dy0, int dy1,
                             mr_display_service_fn service, void *service_opaque)
{
    aga_state *s = (aga_state *)handle;
    int pw, ddy0, ddh, y;
    (void)service; (void)service_opaque;
    if (!s || !s->scr) return;
    pw = s->pw;
    if (dy0 < 0) dy0 = 0;
    if (dy1 > h)  dy1 = h;
    if (dy1 <= dy0) return;
    ddy0 = dy0; ddh = dy1 - dy0;

    /* The fused converter wants the undoubled indexed source. Unlike the 1x1
     * paths there is deliberately no screen-sized chunky buffer to copy into. */
    if (s->kalms_kind == KALMS_2X2_8) {
        if (w != s->w || h != s->h || idx_stride != w) return;
        s_frame_enc = 0;
        aga_blit_kalms2x2(s, idx + (size_t)dy0 * idx_stride, ddh, ddy0);
        return;
    }

    /* For the common aligned case (idx already laid out at exactly pw
     * bytes/row - true whenever dw needs no C2P/WPA padding, e.g. the
     * ordinary 640-wide HIRES geometry) every blit backend can consume the
     * queue buffer directly, skipping this frame's copy into s->chunky
     * entirely. The Kalms 1x1 routine can now consume the same direct dirty
     * row band because its per-call init is updated to that band. A padded or
     * off-pw-stride geometry (idx_stride != pw) keeps the copy - the
     * pad columns beyond the visible width must read as s->chunky's cleared
     * black, not whatever the queue buffer happens to hold past its own
     * (narrower) row. */
    if (idx_stride == pw) {
        s_frame_enc = 0;
        aga_blit(s, idx, idx_stride, ddy0, ddh);
        return;
    }

    { clock_t a = 0;
    if (g_display_want_time) a = clock();
    /* Already-dithered rows: a plain per-row copy into the padded chunky
     * layout, no LUT arithmetic - mr_dither_rgb_indexed() already ran once, in
     * queue_copy_indexed(), when this frame was queued. */
    for (y = dy0; y < dy1; y++) {
        const uint8_t *sr = idx + (size_t)y * idx_stride;
        uint8_t *dr = s->chunky + (size_t)y * pw +
                     (s->kalms_kind == KALMS_1X1_8 ? s->x0 : 0);
        memcpy(dr, sr, (size_t)w);
    }
    if (g_display_want_time) { s_frame_enc = clock() - a; s_enc += s_frame_enc; } }

    aga_blit(s, s->chunky, pw, ddy0, ddh);
}

static int aga_poll(void *handle)
{
    aga_state *s = (aga_state *)handle;
    struct IntuiMessage *msg;
    int ev = MR_EV_NONE;
    if (!s || !s->win) return MR_EV_QUIT;
    while ((msg = (struct IntuiMessage *)GetMsg(s->win->UserPort))) {
        ULONG cls = msg->Class; UWORD code = msg->Code;
        ReplyMsg((struct Message *)msg);
        if (cls == IDCMP_RAWKEY && !(code & 0x80)) {       /* key down only  */
            switch (code) {
            case 0x45: s->quit = 1; break;             /* ESC              */
            case 0x40: ev = MR_EV_PAUSE; break;        /* space            */
            case 0x4E: ev = MR_EV_SEEK_FWD; break;     /* cursor right     */
            case 0x4F: ev = MR_EV_SEEK_BACK; break;    /* cursor left      */
            case 0x4C: ev = MR_EV_VOLUME_UP; break;    /* cursor up        */
            case 0x4D: ev = MR_EV_VOLUME_DOWN; break;  /* cursor down      */
            }
        }
    }
    return s->quit ? MR_EV_QUIT : ev;
}

static void aga_close(void *handle)
{
    aga_state *s = (aga_state *)handle;
    struct IntuiMessage *msg;
    int attempt;

    if (!s) return;

    /*
     * Custom planar screens are shared Intuition/graphics objects.  On real
     * hardware CloseWindow() returning does not guarantee that the last IDCMP
     * message or blit/screen reference has disappeared on the same instruction.
     * Losing the Screen pointer after a failed CloseScreen() leaves Workbench
     * in a very unhappy state, so make shutdown deliberately conservative.
     */
    if (s->win) {
        ModifyIDCMP(s->win, 0);
        if (s->win->UserPort) {
            while ((msg = (struct IntuiMessage *)GetMsg(s->win->UserPort)))
                ReplyMsg((struct Message *)msg);
        }
    }

    WaitBlit();

    if (s->win) {
        CloseWindow(s->win);
        s->win = NULL;
        /* Give Intuition two frames to retire window/layer references. */
        WaitTOF();
        WaitTOF();
    }

    if (s->scr) {
        for (attempt = 0; attempt < 50; attempt++) {
            if (CloseScreen(s->scr)) {
                s->scr = NULL;
                break;
            }
            WaitTOF();
        }
        if (s->scr) {
            ScreenToBack(s->scr);
            printf("planar: WARNING custom screen still busy after "
                   "50 VBlanks (shutdown)\n");
        } else if (attempt > 0) {
            printf("planar: custom screen closed after %d VBlank(s) "
                   "(shutdown)\n", attempt);
        }
    }

    /* Keep all frame/blit storage alive until graphics has quiesced above. */
    if (s->enc_alloc) free(s->enc_alloc);
    if (s->scaled) free(s->scaled);
    if (s->chunky_alloc) free(s->chunky_alloc);
    if (s->tempbm) FreeBitMap(s->tempbm);
    s_kalms_active = 0;
    free(s);
}

static ULONG aga_wait_mask(void *handle)
{
    aga_state *s = (aga_state *)handle;
    if (!s || !s->win || !s->win->UserPort) return 0;
    return 1UL << s->win->UserPort->mp_SigBit;
}

const display_backend backend_aga = {
    .name = "AGA",
    .open = aga_open,
    .show = aga_show,
    .poll = aga_poll,
    .close = aga_close,
    .wait_mask = aga_wait_mask,
    .supports_indexed = aga_supports_indexed,
    .show_indexed = aga_show_indexed,
    .supports_yuv_indexed = aga_supports_yuv_indexed
    /* timing/status/toggle_fullscreen left NULL - AGA has none of these. */
};
