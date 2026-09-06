/*
 * MintVID - internal display backend vtable.
 *
 * Each backend (RTG/cybergraphics, AGA) implements these four calls over its
 * own opaque handle; display.c picks one and routes the public API to it.
 */
#ifndef DISPLAY_BACKEND_H
#define DISPLAY_BACKEND_H

#include <exec/types.h>

typedef struct {
    const char *name;
    void *(*open)(int w, int h, const char *title);
    /* dy0..dy1 are the changed source rows to (re)draw; the rest of the display
     * is left untouched (it persists from the previous frame). */
    void  (*show)(void *handle, const unsigned char *rgb, int w, int h,
                  int stride, int dy0, int dy1,
                  mr_display_service_fn service, void *service_opaque);
    /* Optional packed-BGR24 entry point.  P96's native RGBFB_B8G8R8 backend
     * implements this so callers that already hold BGR can skip its per-pixel
     * RGB channel shuffle.  NULL for backends whose public input is RGB24. */
    void  (*show_bgr)(void *handle, const unsigned char *bgr, int w, int h,
                      int stride, int dy0, int dy1,
                      mr_display_service_fn service, void *service_opaque);
    int   (*timing)(void *handle, mr_display_timing *timing);
    int   (*poll)(void *handle);
    void  (*close)(void *handle);
    /* Optional: show a short status string (NULL/empty restores the normal
     * title). Backends may leave this NULL. */
    void  (*status)(void *handle, const char *text);
    /* Optional: signal bits a caller may Wait() on to wake for backend events. */
    ULONG (*wait_mask)(void *handle);
    int   (*toggle_fullscreen)(void *handle);
    /* Optional: accept one-byte palette indices directly. Returns the active
     * native depth through indexed_depth (4/5/8); false for HAM/scaled modes. */
    int   (*supports_indexed)(void *handle, int *indexed_depth);
    /* Optional: blit pre-dithered palette indices - idx_stride bytes per row,
     * one byte per pixel, already in this backend's active palette space.
     * Only implemented (non-NULL) where supports_indexed exists. */
    void  (*show_indexed)(void *handle, const unsigned char *idx, int w,
                          int h, int idx_stride, int dy0, int dy1,
                          mr_display_service_fn service, void *service_opaque);
    /* Optional: report whether this handle can accept direct YUV420P ->
     * chunky conversion, filling the destination geometry, fast-path vscale
     * (1 or an exact vertical divisor; 0 selects general resize), the active
     * plane depth, and the pixel encoding. A caller should prefer this over
     * the RGB supports_indexed() route when both apply.
     *
     * *ham is 0 when the bytes are palette indices in the active 4/5/8-plane
     * cube (core/mr_yuv_dither.h), or 6/8 when they are HAM6/HAM8 pixel bytes
     * (core/mr_yuv_ham.h). Both are one chunky byte per pixel bound for the
     * same C2P and blit, so they are produced into the same buffer and
     * consumed the same way; only the converter the caller runs differs.
     * *indexed_depth is the screen's plane count either way (HAM6 -> 6,
     * HAM8 -> 8).
     *
     * Produced buffers are consumed the same way as supports_indexed()'s -
     * via show_indexed(), called with *dst_w and *dst_h instead of the source
     * dimensions. A backend may subsequently scale those bytes while
     * presenting them (the AGA Kalms 2x2 path does this). */
    int   (*supports_yuv_indexed)(void *handle, int src_w, int src_h,
                                  int *dst_w, int *dst_h, int *vscale,
                                  int *indexed_depth, int *ham);
} display_backend;

extern const display_backend backend_cgx;
extern const display_backend backend_p96;
extern const display_backend backend_aga;

/* AGA backend configuration, set via the public display_set_* calls. */
extern int g_aga_ham;    /* 0 = indexed planar, 6 = HAM6, 8 = AGA HAM8      */
extern int g_aga_scale;  /* 1 or 2 (pixel doubling)                        */
extern int g_aga_c2p;    /* 0 = WPA8, 1 = portable, 2 = RiVA, 3 = Kalms     */
extern int g_aga_lace;   /* 1 = allow interlaced screens (taller fit)       */
extern int g_aga_akiko;  /* 1 = use CD32 Akiko hardware C2P                  */
extern int g_aga_ecs_fast; /* 1 = force the 4-plane/16-colour fast encoder   */
extern int g_aga_ecs32;    /* 1 = force the 5-plane/32-colour ECS/OCS cube    */
extern int g_display_fullscreen;

/* Library bases opened once by display.c and shared by the backends. */
struct IntuitionBase;
struct GfxBase;
struct Library;
extern struct IntuitionBase *IntuitionBase;
extern struct GfxBase       *GfxBase;
extern struct Library       *CyberGfxBase;
/* Picasso96API.library base, opened only when the P96 backend is selected
 * (display_set_force_p96()); NULL otherwise, in which case backend_p96's
 * open() must fail so display_open() falls back to CGX/AGA. */
extern struct Library       *P96Base;

/* Set non-zero to enable timing/diagnostic printf output.  Wired to --time. */
extern int g_display_want_time;

#endif /* DISPLAY_BACKEND_H */
