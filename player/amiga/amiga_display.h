/*
 * MintVID - Amiga display backend (abstract).
 *
 * The player talks to this, not to Intuition/cybergraphics directly, so a
 * faster fullscreen RTG path or an AGA C2P path can slot in later behind the
 * same three calls without touching the player loop.
 *
 * The first backend (display_cgx.c) opens an RTG window on the default public
 * screen and blits RGB24 frames with cybergraphics.library WritePixelArray -
 * correct and simple; optimisation (direct RGB565, fullscreen, RiVA's blitters)
 * comes later.
 */
#ifndef AMIGA_DISPLAY_H
#define AMIGA_DISPLAY_H

typedef struct amiga_display amiga_display;
typedef void (*mr_display_service_fn)(void *opaque);

typedef struct mr_display_timing {
    unsigned long prepare_us, scale_us, convert_us, copy_us, blit_us;
    unsigned long clip_us, total_us, pixels, bytes;
    unsigned long geometry_us, resize_us, allocation_us, setup_us;
    /* Cost of the caller's service callback (Paula refill / due-frame
     * presentation) invoked from inside the backend's show() call, between
     * its own sub-phase measurements - see display_cgx.c's cgx_show() for
     * why this needs its own field: without it, that cost fell into none of
     * prepare_us/blit_us/etc, yet was still included in total_us, making
     * total_us appear to have an unaccounted gap whenever the service
     * callback itself ran slow (e.g. Paula catch-up refill after an
     * audio-rescue episode). */
    unsigned long service_us;
    unsigned int src_w, src_h, dst_w, dst_h, copies;
    const char *src_format, *dst_format;
} mr_display_timing;

/* Force the AGA backend (skip the RTG/cybergraphics attempt). Call before
 * display_open; default is RTG-first with automatic AGA fallback. */
void display_set_force_aga(int on);

/* Prefer P96 (Picasso96API.library) over the WritePixelArray (CGX) backend.
 * Call before display_open(). display_open() itself tries two P96 backends
 * under this one flag, in order: the PIP overlay window first (real
 * hardware acceleration where the board/driver supports it - see
 * amiga/display_p96pip.c's file header for the full design rationale and
 * its current, real-hardware-unverified status), then the older direct
 * screen-bitmap lock (p96LockBitMap) if the PIP can't open. Only takes
 * effect on a screen/format either backend actually supports; anything else
 * falls back to CGX automatically, same as CGX itself falls back to AGA.
 *
 * P96 opens windowed by default now (neither GUI nor mr_play_options.c
 * forces --fullscreen for it any more): the PIP overlay backend has none of
 * the older direct-lock backend's "unclipped writes corrupt sibling
 * windows" hazard (see that backend's own file header), so there is no
 * longer a reason to start fullscreen just to get a working P96 session.
 * Pressing F (display_toggle_fullscreen()) is what takes a P96 session to
 * fullscreen - amiga/display_p96pip.c's own toggle_fullscreen() tries real
 * hardware acceleration (PIPT_VideoWindow) again on every toggle, falling
 * back to software compositing (PIPT_MemoryWindow) only if the board
 * refuses, exactly mirroring the windowed-open behaviour, so the same
 * "hardware overlay if the board grants it, software fallback otherwise"
 * contract holds whether P96 is windowed or fullscreen. Has no effect if
 * display_set_force_aga() is also on. */
void display_set_force_p96(int on);

/* Show the video in a window on the default public screen (an AGA, ECS or
 * OCS Workbench) instead of opening a screen of its own. Frames are
 * dithered to the usual RGB cube (sized to the screen's depth) and each
 * cube colour is mapped to a shared screen pen with ObtainBestPen(), so
 * Workbench's own colours are left alone. No HAM, no C2P. mode: 0 off,
 * 1 Window, 2 Window (Half) - half the video's width and height. Tried
 * before every other backend; the normal chain is the fallback if the
 * window can't open. See display_aga_window.c. */
void display_set_aga_window(int mode);

/* Planar colour mode: 0 = indexed dither (256 colours on AGA, 32 on
 * OCS/ECS), 6 = HAM6 on any chipset, 8 = HAM8 on AGA. A non-zero HAM depth
 * forces the native planar backend. */
void display_set_ham(int bits);

/* Integer upscale for the AGA backend: 1 (default) or 2. */
void display_set_scale(int n);

/* AGA blit path: 0 = graphics WritePixelArray8 (default), 1 = built-in C2P. */
void display_set_c2p(int on);

/* Select the experimental RiVA-inspired, 32-pixel direct-to-plane C2P.  It is
 * deliberately opt-in until it has been benchmarked on each 68k generation. */
void display_set_riva_c2p(int on);

/* Select CPU-matched Kalms converters (030 or 040/060 1x1, fused 2x2 where
 * possible, plus six-plane bitmap output on 040/060). */
void display_set_kalms_c2p(int on);

/* Select the single-kernel direct-planar C2P: one hand-written 040/060
 * kernel dithers straight to the final eight-plane image, 32 pixels at a
 * time, with no separate chunky/C2P pass. Falls back to WritePixelArray8
 * whenever the geometry doesn't qualify (plain 1:1 8-plane AGA only) -
 * see amiga/display_aga.c's aga_supports_yuv_indexed(). */
void display_set_direct_c2p(int on);

/* Allow interlaced AGA screens (up to ~640x512). The AGA fitter compensates
 * for the doubled vertical resolution, preserving the video's physical aspect
 * ratio. Off by default because interlace flickers on native displays. */
void display_set_lace(int on);

/* Use the CD32 Akiko chip's hardware chunky->planar instead of the CPU C2P.
 * CD32 only; no effect (and unsafe) elsewhere, so gate it on --cd32. */
void display_set_akiko(int on);

/* Force the 4-plane/16-colour indexed encoder (half the AGA backend's normal
 * 8-plane/256-colour depth) for a faster encode + blit. Works on any chipset,
 * including AGA - unlike the ECS/OCS 32-colour fallback, this is an opt-in
 * speed/quality tradeoff, not a chipset requirement. Forces the native planar
 * backend, same as display_set_ham(). */
void display_set_ecs_fast(int on);

/* Force the 5-plane/32-colour ECS/OCS indexed encoder that non-AGA chipsets
 * already fall back to automatically - but selectable explicitly, including
 * on AGA, as a middle ground between display_set_ecs_fast() (16 colours) and
 * the normal 256-colour AGA depth. Forces the native planar backend, same as
 * display_set_ham(). */
void display_set_ecs32(int on);

/* Force Extra Half-Brite: a genuine ECS/OCS chipset feature (no AGA needed,
 * unlike every other indexed depth above 32 colours in this file), 6
 * bitplanes for 64 apparent colours - 32 real palette registers plus 32
 * free half-brightness duplicates the hardware derives from the 6th
 * bitplane's own bit, no extra palette RAM. Forces the native planar
 * backend, same as display_set_ham(). Mutually exclusive with HAM6/HAM8 in
 * practice (both want the same 6-plane depth for different reasons) -
 * display_set_ham() takes priority if both are ever set, see aga_open(). */
void display_set_ehb(int on);

/* Opt-in: skip vertically doubling every scale==2 (--2x) frame in software
 * and instead let a copper list, built once when the AGA screen opens,
 * repeat each already-doubled-width chunky/planar row a second time on the
 * real raster. Halves the rows aga_show() has to dither/HAM-encode and C2P
 * for scale==2 (the vertical half of the work mr_scale2x_u8() otherwise
 * does), at the cost of one copper list poking BPLxPT directly - see the
 * "Copper-assisted vertical doubling" comment in display_aga.c for the
 * mechanism, its Kalms/direct-planar exclusion, and its verification
 * status: confirmed on real AGA hardware with --c2p (portable) indexed
 * output - correct picture for the whole session and a clean exit (a
 * shutdown-path crash, Guru 81000005, took two attempts to actually fix -
 * see aga_close()'s comment). --riva-c2p/--cd32 and ECS/OCS chipsets are not
 * yet exercised for indexed output.
 *
 * HAM6/HAM8 (EXPERIMENTAL): the same mechanism now also applies to HAM6 and
 * HAM8 output (HAM8 requires real AGA, same as HAM8 itself does). This is
 * backed by a correctness argument about HAM's hold-and-modify state being
 * per-scanline-independent (see display_aga.c's file header comment for the
 * full reasoning), not yet by a real-hardware run - treat any HAM +
 * --copper-vdouble combination as unverified until confirmed on real AGA
 * hardware, separately from the indexed case above.
 *
 * No effect unless scale==2 is also in effect (--2x) and the geometry
 * doesn't fall onto a Kalms/direct-planar path. */
void display_set_copper_vdouble(int on);

/* Enable timing/diagnostic output for the RTG backend (mirrors --time).
 * Must be called before display_open() to capture init diagnostics. */
void display_set_timing_mode(int on);
void display_set_fullscreen(int on);

/* Accumulated AGA encode / blit time in ms (0 if the AGA backend wasn't used). */
void display_aga_timing(unsigned long *enc_ms, unsigned long *blit_ms);

/* Most recent AGA frame's conversion and blit times (rather than totals). */
void display_aga_frame_timing(unsigned long *enc_ms, unsigned long *blit_ms);

/* Return non-zero when the opened AGA screen is using Kalms, optionally
 * returning its accumulated conversion time. */
int display_aga_kalms_timing(unsigned long *conversion_ms);

/* The most recently opened AGA screen's effective (post-negotiation) mode -
 * depth (bits/pixel), ham (0/6/8), scale (1 or 2), resize (0/1) and the c2p
 * backend name ("wpa"/"c2p"/"riva"/"kalms-*"/"akiko") - for mrplay --time's
 * "AGA path:" diagnostic. depth is -1 if no AGA screen has been opened yet.
 * chipset is the detected tier - "AGA", "ECS" or "OCS" - purely informational:
 * every non-HAM8 encoding and mode-selection rule in display_aga.c already
 * keys off chipset_has_aga() alone (see its own comments), so ECS and OCS
 * take the identical code path today and this string exists only so --time
 * output and bug reports can tell a real A500/PiStorm OCS run apart from an
 * ECS one instead of both silently reading "not AGA".
 * copper reports whether --copper-vdouble is not just requested but actually
 * engaged for this screen (0/1) - see display_set_copper_vdouble() for what
 * "engaged" requires, including the HAM6/HAM8 extension and its experimental
 * status. ehb reports whether Extra Half-Brite is the encoding actually in
 * effect (0/1) - see display_set_ehb().
 * Every out-parameter is optional (pass NULL to skip it). */
void display_aga_describe(int *depth, int *ham, int *scale, int *resize,
                          const char **c2p, const char **chipset,
                          int *copper, int *ehb);

/* Open a display able to show w*h frames: tries RTG (cybergraphics) first, then
 * falls back to AGA. Returns NULL only if neither works. */
amiga_display *display_open(int w, int h, const char *title);

/* Name of the backend that actually opened ("RTG (CGX)" / "AGA"). */
const char *display_backend_name(amiga_display *d);

/* Blit one RGB24 frame (r,g,b bytes, `stride` bytes per row, top-down). Only
 * source rows [dy0,dy1) are redrawn; pass 0..h to draw the whole frame. */
void display_show_rgb(amiga_display *d, const unsigned char *rgb,
                      int w, int h, int stride, int dy0, int dy1);

/* Optional packed B,G,R path used by the P96 direct-lock backend.  Query
 * support once after display_open(); when true, display_show_bgr24() accepts
 * the same geometry/dirty-row contract as display_show_rgb() but does not
 * perform an RGB->BGR channel shuffle before writing the native bitmap. */
int display_supports_bgr24(amiga_display *d);
void display_show_bgr24(amiga_display *d, const unsigned char *bgr,
                        int w, int h, int stride, int dy0, int dy1);

/* Native RGB565 path (one native uint16_t per pixel, stride in bytes - see
 * core/mr_yuv.h's mr_yuv420_to_rgb565()). display_supports_rgb565() is true
 * when the active backend writes it to a 16-bit screen with a row copy (P96
 * direct lock on RGBFB_R5G6B5). display_show_rgb565() always works: if the
 * backend or its screen changes mid-session it unpacks to RGB24 and uses the
 * ordinary show(), the same fallback shape as display_show_yuv422(). */
int display_supports_rgb565(amiga_display *d);
void display_show_rgb565(amiga_display *d, const unsigned char *pix,
                         int w, int h, int stride, int dy0, int dy1);

/* Packed Picasso96 Y4U2V2 path (Y,chroma,Y,chroma for each horizontal pair -
 * see core/mr_yuv.c's mr_yuv420_to_y4u2v2() for the real chroma order).
 * display_supports_yuv422() reports whether the *currently active* backend
 * implements this natively (only the P96 PIP overlay backend does, and
 * only for even source widths) - but display_show_yuv422() itself always
 * works regardless: if the active backend switched away from that overlay
 * mid-session (display.c's switch_to_cgx_fallback(), see
 * display_toggle_fullscreen() below), display_show_yuv422() converts to
 * RGB24 in software and forwards to the new backend's ordinary show(), so
 * a caller that decided once at startup to feed this format never needs
 * to unwind that decision just because the display backend changed
 * underneath it. */
int display_supports_yuv422(amiga_display *d);
void display_show_yuv422(amiga_display *d, const unsigned char *yuv,
                         int w, int h, int stride, int dy0, int dy1);

/* Non-zero when `d` can accept a pre-dithered indexed frame via
 * display_show_indexed() instead of RGB24 via display_show_rgb() - true
 * only for the AGA backend's plain indexed configuration (see
 * display_backend.h's supports_indexed for the exact conditions). Callers
 * that want to skip a redundant RGB24 copy/dither pass (e.g. mrplay.c's
 * decode-ahead queue) should query this once after display_open()
 * succeeds and, if true, dither with core/mr_dither_rgb_indexed() at the
 * returned 4/5/8-plane depth and call display_show_indexed(). */
int display_supports_indexed(amiga_display *d, int *indexed_depth);

/* Blit a pre-dithered indexed frame (idx_stride bytes/row, one byte
 * per pixel, already in this display's palette index space). Only valid
 * to call when display_supports_indexed(d, ...) returned non-zero. */
void display_show_indexed(amiga_display *d, const unsigned char *idx,
                          int w, int h, int idx_stride, int dy0, int dy1);

/* Non-zero when `d` can accept a direct YUV420P -> chunky frame. Fills the
 * destination geometry, fast-path vscale (0 means general two-axis resize),
 * the active plane depth, and the pixel encoding: *ham is 0 for palette
 * indices (convert with core/mr_yuv_dither.h) or 6/8 for HAM6/HAM8 pixel
 * bytes (core/mr_yuv_ham.h). Either way the result is one chunky byte per
 * pixel; pass it to display_show_indexed(), which may perform final
 * presentation scaling. */
int display_supports_yuv_indexed(amiga_display *d, int src_w, int src_h,
                                 int *dst_w, int *dst_h, int *vscale,
                                 int *indexed_depth, int *ham);
void display_set_service(amiga_display *d, mr_display_service_fn fn,
                         void *opaque);
/* Take the active backend to/from fullscreen. Returns 1 on success, 0 on
 * failure. A backend that cannot honour fullscreen itself but rolled back
 * to a working windowed state (display_backend.h's toggle_fullscreen
 * return value 2, currently only backend_p96pip) is handled transparently
 * here: this function attempts display.c's CGX fallback and still returns
 * 1/0 for success/failure of the *overall* request, never leaking the
 * internal sentinel to callers. */
int display_rtg_frame_timing(amiga_display *d, mr_display_timing *timing);
int display_toggle_fullscreen(amiga_display *d);

/* Input events reported by display_poll_event. */
enum {
    MR_EV_NONE = 0,
    MR_EV_QUIT,          /* ESC or close gadget                            */
    MR_EV_PAUSE,         /* space - toggle pause                           */
    MR_EV_SEEK_FWD,      /* cursor right                                   */
    MR_EV_SEEK_BACK,     /* cursor left                                    */
    MR_EV_VOLUME_UP,     /* cursor up                                      */
    MR_EV_VOLUME_DOWN,   /* cursor down                                    */
    /* Internal: a backend's own poll() (e.g. backend_p96pip's F-key
     * handler) can return this to ask display_poll_event() to perform
     * display.c's CGX fallback (see display_backend.h's toggle_fullscreen
     * doc and display.c's switch_to_cgx_fallback()).  display_poll_event()
     * always handles and consumes it before returning - callers of
     * display_poll_event() never see this value. */
    MR_EV_RENDERER_SWITCH
};

/* Non-blocking: returns the most significant queued input event (QUIT wins). */
int  display_poll_event(amiga_display *d);
unsigned long display_wait_mask(amiga_display *d);

/* Show a short status string (e.g. "Buffering...", "Reconnecting..."); NULL or
 * "" restores the normal title. On the CGX backend this updates the window
 * title; backends without a status surface ignore it. */
void display_set_status(amiga_display *d, const char *text);

void display_close(amiga_display *d);

#endif /* AMIGA_DISPLAY_H */
