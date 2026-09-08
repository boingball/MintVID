/*
 * MintVID - direct YUV420P -> HAM6/HAM8 encoding.
 *
 * The AGA HAM path runs the decoder's output through a full-resolution RGB24
 * intermediate: aga_show() does mr_yuv420_to_rgb24() ->
 * mr_scale_resize_rgb24() -> mr_ham_encode(). Hold-and-modify is sequential
 * *along* a scanline but every row starts from a fresh held colour, so the
 * greedy per-pixel decision can just as well consume RGB the instant it is
 * computed from Y/Cb/Cr and never store it - the same algebraic fusion
 * core/mr_yuv_dither.h performs for the indexed modes, with mr_ham_encode()
 * in place of the ordered dither.
 *
 * Skipping the intermediate is not by itself the win it looks like, which is
 * why this covers one geometry rather than all of them. The three-stage
 * pipeline's YUV->RGB stage is hand-written m68k assembly and this is
 * portable C, and measured under qemu-m68k that assembly is worth roughly as
 * much as the buffer traffic it costs: at 1:1 the two are a wash, swinging
 * either way by more with a change of inlining than the fusion itself is
 * worth. What does not depend on out-coding the assembly is converting fewer
 * samples. On the vertical downscale an AGA fit normally needs - 640x360
 * decoded into a 640x180 non-laced screen - the three-stage path converts all
 * 360 source rows to RGB and then throws half of them away, while this
 * converts only the 180 that survive. That is a ~45% saving on the whole
 * conversion, consistent across every code-layout variant measured, so it is
 * the shape aga_supports_yuv_indexed() routes here.
 *
 * Output is one HAM pixel byte per pixel - control<<(bits-2) | data - which
 * the display side consumes exactly like palette indices: both are chunky
 * bytes headed for the same C2P and blit (see aga_show_indexed()).
 *
 * Like core/mr_yuv_dither.c, this deliberately rebuilds its own copies of
 * the YCbCr->RGB and HAM quantiser tables from the same published formulas
 * rather than sharing those files' private ones, so
 * tests/mr_yuv_ham_check.c can compare this against the real three-stage
 * composition as an independent check that neither copy has drifted.
 */
#ifndef MR_YUV_HAM_H
#define MR_YUV_HAM_H

#include "mr_types.h"

/*
 * Convert width x height YUV420P directly to HAM pixel bytes, selecting one
 * source row out of every `vscale` by the same nearest-neighbour rule
 * mr_scale_resize_rgb24() uses for an exact-fit downscale (source row =
 * vscale*out_row + vscale/2). Pass vscale=1 to keep every row.
 *
 * `bits` is 8 for HAM8 (AGA, 6-bit modify) or 6 for HAM6.
 *
 * Precondition, as for mr_yuv420_dither8(): this matches the three-stage
 * pipeline only when the destination width equals `width` (no horizontal
 * resize) and `vscale` evenly divides `height`. out holds height/vscale rows
 * of `width` bytes at out_stride bytes per row.
 */
void mr_yuv420_ham_encode(const uint8_t *y_plane, int y_stride,
                          const uint8_t *u_plane, int u_stride,
                          const uint8_t *v_plane, int v_stride,
                          int width, int height, int vscale, int bits,
                          uint8_t *out, int out_stride);

/*
 * There is deliberately no general resize form, and no vscale==1 identity
 * user, even though both are straightforward to write: see the header comment
 * above and aga_supports_yuv_indexed(). Those shapes convert exactly as many
 * samples as the three-stage pipeline does, and that pipeline's YUV->RGB
 * stage is hand-written m68k assembly (core/mr_yuv_m68k.S) while this is C -
 * measured under qemu-m68k they are a wash at best. Only the vertical
 * downscale has a saving that does not depend on out-coding that assembly.
 */

#endif /* MR_YUV_HAM_H */
