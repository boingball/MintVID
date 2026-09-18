/*
 * MintVID - MPEG-1/MPEG-2 video decoder adapter using libmpeg2.
 */
#ifndef MR_MPEG2_H
#define MR_MPEG2_H

#include "mr_codec.h"

extern const mr_codec mr_codec_mpeg2;

typedef void (*mr_mpeg2_service_fn)(void *opaque);
/* Called periodically during decode/convert (once per completed picture
 * inside pump()'s parse loop, and threaded into mr_yuv420_to_rgb24() the same
 * way mr_h264_set_service() threads it into libavc's own RGB conversion) so
 * the caller can keep feeding Paula's software FIFO during a run of work
 * this adapter cannot otherwise yield out of. Matters more here than it
 * might look: a single demuxed packet can hand pump() more than one
 * complete picture (common for small/low-bitrate MPEG-1 GOPs), and every
 * one of them gets converted to RGB24 before pump() ever returns control to
 * the caller's own scheduler loop - with no service callback, that whole
 * run happens with Paula unfed. NULL disables (the default). */
void mr_mpeg2_set_service(mr_decoder *dec, mr_mpeg2_service_fn fn,
                          void *opaque);

/* Emit MR_PIX_YUV420P planes rather than converting each displayed picture to
 * RGB24. Worth taking whenever the caller dithers to palette indices anyway
 * (display_supports_yuv_indexed()): on real m68k the RGB24 conversion is about
 * a third of this adapter's decode time, and the RGB->indexed pass that
 * follows it costs about as much again.
 *
 * Call it immediately after open, as the H.264 path does with
 * mr_h264_set_yuv_output(); it discards anything already queued. A no-op on a
 * decoder that is not this codec, so callers need not test first. */
void mr_mpeg2_set_yuv_output(mr_decoder *dec, int enabled);

/* Preserve PES timestamps across libmpeg2 display reordering. */
void mr_mpeg2_set_input_pts(mr_decoder *dec, int has_pts, uint64_t pts_us);
int mr_mpeg2_output_pts(mr_decoder *dec, uint64_t *pts_us);

/* libmpeg2's own mpeg2_skip() (vendor/libmpeg2/include/mpeg2.h) skips a
 * picture's entire macroblock/slice decode - near-zero cost, the same shape
 * as H.264's IVD_SKIP_B/PB (see CLAUDE.md's "DV decode speed"/H.264 CABAC
 * notes). MR_MPEG2_SPEED_FAST calls it for every B picture and only B
 * pictures - never P/I, which remain full references for later pictures -
 * so this is safe by MPEG-2's own spec design: a B picture is by
 * definition never referenced by anything, so skipping its reconstruction
 * cannot corrupt any other picture's decode.
 *
 * The display-buffer handoff needs one real check, not just careful
 * plumbing: mpeg2_parse() returns STATE_SLICE for a skipped picture the
 * same as for a normally-decoded one - header.c sets mpeg2dec->state to
 * STATE_SLICE right after the header parse, meaning "ready to decode
 * slices," not "a slice was decoded" - so display_fbuf already points at
 * that picture's (in the skipped case, never-written, stale) buffer by
 * the time pump() sees STATE_SLICE either way. A first version of this
 * code trusted display_fbuf alone on that basis and produced real,
 * measured corruption (MAE 13-114 at every one of a real fixture's 32
 * B-frame positions) - caught by testing, not by re-reading the trace
 * more carefully; see CLAUDE.md's "MPEG-1/2 B-frame skip" notes for the
 * full story. The actual fix: libmpeg2 itself already flags this exact
 * situation (header.c sets PIC_FLAG_SKIP on the picture when
 * nb_decode_slices came back 0), and mr_mpeg2.c's pump() checks that flag
 * on display_picture before queuing - not merely present on
 * STATE_SLICE/STATE_END, but confirmed via PIC_FLAG_SKIP that this
 * particular picture was actually decoded. Net effect: a skipped B
 * picture simply never reaches the display queue - fewer frames, never a
 * corrupted one, verified byte-exact against a full decode by
 * tests/mr_mpeg2_bskip_check.c on host and real m68k/big-endian alike.
 * Default is MR_MPEG2_SPEED_QUALITY (decode everything, current
 * behaviour) - opt-in only. */
typedef enum {
    MR_MPEG2_SPEED_QUALITY = 0,  /* decode every picture, including B */
    MR_MPEG2_SPEED_FAST          /* skip B pictures entirely */
} mr_mpeg2_speed_mode;

void mr_mpeg2_set_speed_mode(mr_decoder *dec, mr_mpeg2_speed_mode mode);

#endif /* MR_MPEG2_H */
