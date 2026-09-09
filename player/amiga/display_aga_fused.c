/*
 * MintVID - experimental AGA wrapper for the fused planar queue build.
 *
 * Keep the production backend source untouched while hardware decides whether
 * this layout is worth integrating.  Include it into this translation unit
 * under private names, then replace only the two YUV/indexed capability hooks
 * and close() with thin wrappers.
 */
#include "../core/mr_yuv_planar_queue.h"

#define backend_aga               backend_aga_base
#define aga_supports_yuv_indexed  aga_supports_yuv_indexed_base
#define aga_show_indexed          aga_show_indexed_base
#define aga_close                 aga_close_base
#include "display_aga.c"
#undef aga_close
#undef aga_show_indexed
#undef aga_supports_yuv_indexed
#undef backend_aga

/* The experimental queue representation is enabled only for the geometry we
 * actually measured: plain 8-plane AGA, 1:1 source/display mapping.  The body
 * is compiled on every CPU so the normal 68030 CI build still syntax-checks it;
 * MR_KALMS_040 merely controls whether it can be selected at runtime (040/060
 * profiles define that flag in Makefile.amiga). */
static int aga_supports_yuv_indexed_fused(void *handle, int src_w, int src_h,
                                          int *dst_w, int *dst_h, int *vscale,
                                          int *indexed_depth, int *ham)
{
    aga_state *s = (aga_state *)handle;
    int dw = 0, dh = 0, vs = 0, depth = 0, hmode = 0;
    int ok, allow = 0;

    mr_yuv_planar_queue_disable();
    ok = aga_supports_yuv_indexed_base(handle, src_w, src_h,
                                       &dw, &dh, &vs, &depth, &hmode);
    if (!ok) return 0;

#if defined(MR_KALMS_040)
    allow = 1;
#endif

    if (allow && s && s->scr && !hmode && depth == 8 &&
        s->depth == 8 && !s->ham && s->scale == 1 && !s->resize &&
        dw == src_w && dh == src_h && vs == 1 &&
        src_w > 0 && src_w <= 640 && src_h > 0) {
        int pw = (src_w + 31) & ~31;
        struct BitMap *bm = s->scr->RastPort.BitMap;
        int compatible = bm && bm->Depth >= 8 && pw <= bm->BytesPerRow * 8;
        int p;
        for (p = 0; compatible && p < 8; p++)
            compatible = bm->Planes[p] != NULL;

        if (compatible && mr_yuv_planar_queue_configure(src_w, src_h, pw)) {
            /* queue_copy_yuv_indexed() allocates dst_w*dst_h bytes.  Eight
             * bitplanes at pw/8 bytes/row occupy exactly pw*height bytes, so
             * reporting the padded width gives the existing queue precisely
             * the storage this representation needs without changing mrplay. */
            dw = pw;
            dh = src_h;
            vs = 1;
            depth = 8;
            hmode = 0;
            s_diag_c2p = "yuv-planar";
            /* The opened backend may still have a Kalms fallback prepared.
             * This session will bypass it while the planar queue is active. */
            s_kalms_active = 0;
        }
    }

    if (dst_w) *dst_w = dw;
    if (dst_h) *dst_h = dh;
    if (vscale) *vscale = vs;
    if (indexed_depth) *indexed_depth = depth;
    if (ham) *ham = hmode;
    return 1;
}

static void aga_show_indexed_fused(void *handle, const unsigned char *idx,
                                   int w, int h, int idx_stride,
                                   int dy0, int dy1,
                                   mr_display_service_fn service,
                                   void *service_opaque)
{
    aga_state *s = (aga_state *)handle;

    if (!mr_yuv_planar_queue_is_active()) {
        aga_show_indexed_base(handle, idx, w, h, idx_stride, dy0, dy1,
                              service, service_opaque);
        return;
    }

    /* In this mode idx is already the final eight-plane image, plane-major:
     * plane p starts at p*(pw/8)*h.  Presentation therefore has no chunky
     * conversion at all; it is just eight narrow sequential Fast->Chip row
     * copies.  The encoder centred the visible image inside the 32-pixel pad,
     * so centre that padded rectangle on the physical screen as a unit. */
    {
        struct BitMap *bm;
        int pw = mr_yuv_planar_queue_padded_width();
        int ph = mr_yuv_planar_queue_height();
        int src_bpr, screen_bpr, screen_w, x0, x0byte;
        size_t plane_size;
        int p, y;
        clock_t a = 0;
        (void)service;
        (void)service_opaque;

        if (!s || !s->scr || !idx || w != pw || h != ph ||
            idx_stride != pw || pw <= 0 || (pw & 31) != 0) {
            printf("planar-fused: invalid queued planar frame geometry\n");
            return;
        }

        bm = s->scr->RastPort.BitMap;
        if (!bm || bm->Depth < 8) {
            printf("planar-fused: AGA bitmap no longer has eight planes\n");
            return;
        }
        for (p = 0; p < 8; p++) {
            if (!bm->Planes[p]) {
                printf("planar-fused: AGA plane %d disappeared\n", p);
                return;
            }
        }

        if (dy0 < 0) dy0 = 0;
        if (dy1 > h) dy1 = h;
        if (dy1 <= dy0) return;

        src_bpr = pw >> 3;
        screen_bpr = bm->BytesPerRow;
        screen_w = screen_bpr << 3;
        x0 = (screen_w - pw) >> 1;
        x0 &= ~7;
        if (x0 < 0 || x0 + pw > screen_w) {
            printf("planar-fused: padded row does not fit AGA bitmap\n");
            return;
        }
        x0byte = x0 >> 3;
        plane_size = (size_t)src_bpr * (size_t)h;

        s_frame_enc = 0;
        s_frame_blit = 0;
        if (g_display_want_time) a = clock();
        for (p = 0; p < 8; p++) {
            const uint8_t *sp = idx + (size_t)p * plane_size +
                                (size_t)dy0 * src_bpr;
            uint8_t *dp = (uint8_t *)bm->Planes[p] +
                          (size_t)(s->y0 + dy0) * screen_bpr + x0byte;
            for (y = dy0; y < dy1; y++) {
                memcpy(dp, sp, (size_t)src_bpr);
                sp += src_bpr;
                dp += screen_bpr;
            }
        }
        if (g_display_want_time) {
            s_frame_blit = clock() - a;
            s_blit += s_frame_blit;
        }
    }
}

static void aga_close_fused(void *handle)
{
    mr_yuv_planar_queue_disable();
    aga_close_base(handle);
}

const display_backend backend_aga = {
    .name = "AGA",
    .open = aga_open,
    .show = aga_show,
    .poll = aga_poll,
    .close = aga_close_fused,
    .wait_mask = aga_wait_mask,
    .supports_indexed = aga_supports_indexed,
    .show_indexed = aga_show_indexed_fused,
    .supports_yuv_indexed = aga_supports_yuv_indexed_fused
    /* timing/status/toggle_fullscreen left NULL, matching display_aga.c. */
};
