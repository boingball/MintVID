/*
 * MintVID - H.264/AVC decoder plugin backed by Ittiam libavc.
 *
 * The MOV demuxer supplies the avcC decoder configuration and one AVCC
 * (length-prefixed) access unit per packet.  The adapter converts both to
 * Annex B and lets libavc handle High Profile tools and display reordering.
 */
#ifndef MR_H264_H
#define MR_H264_H

#include "mr_codec.h"

extern const mr_codec mr_codec_h264;
typedef void (*mr_h264_service_fn)(void *opaque);
/* Returns non-zero to abort the current decode (called between NAL sub-calls). */
typedef int  (*mr_h264_quit_fn)(void *opaque);
typedef struct mr_h264_timing {
    unsigned long input_us, core_us, output_us;
    /* Sub-stages of core_us, broken out via wrapped libavc function pointers
     * (vendor/libavc_port/ih264d_stage_profile.c): motion compensation,
     * deblocking, IDCT/reconstruction, and intra prediction. */
    unsigned long mc_us, deblock_us, recon_us, intra_us;
    /* Further sub-stages under MR_H264_CABAC_PROFILE
     * (vendor/libavc_port/ih264d_cabac_profile.h): CABAC bin decode,
     * residual coefficient parsing, and MV prediction - see that header for
     * why these three, and not a fourth "macroblock parsing" bucket, are as
     * far as this breakdown goes. core_us minus all seven of mc/deblock/
     * recon/intra/bin/coeff/mvpred is what remains unattributed - mostly
     * macroblock-header syntax dispatch and per-MB bookkeeping. */
    unsigned long bin_us, bin_count, coeff_us, coeff_count;
    unsigned long mvpred_us, mvpred_count;
} mr_h264_timing;
typedef enum mr_h264_speed_mode {
    MR_H264_SPEED_QUALITY = 0,
    MR_H264_SPEED_BALANCED,
    MR_H264_SPEED_FAST,
    MR_H264_SPEED_TURBO,
    MR_H264_SPEED_TURBO_PLUS,
    MR_H264_SPEED_TURBO_GT
} mr_h264_speed_mode;
void mr_h264_set_service(mr_decoder *dec, mr_h264_service_fn fn, void *opaque);
void mr_h264_set_quit(mr_decoder *dec, mr_h264_quit_fn fn, void *opaque);
void mr_h264_set_diag(mr_decoder *dec, const char *path, int width, int height);
void mr_h264_frame_timing(mr_decoder *dec, mr_h264_timing *timing);
/* Off by default. mr_h264_frame_timing() (and the per-stage mc/deblock/
 * recon/intra breakdown under MR_H264_STAGE_PROFILE) only report real
 * numbers once this is turned on - otherwise h264_decode() skips its
 * clock() calls and stage-profile bookkeeping entirely, since nothing
 * would read the result anyway. */
void mr_h264_set_timing_enabled(mr_decoder *dec, int enabled);
void mr_h264_set_skip_output(mr_decoder *dec, int skip);
/* Off by default. When enabled, decoded frames come back as
 * MR_PIX_YUV420P (dec->frame.data/stride = Y, u_data/u_stride = Cb,
 * v_data/v_stride = Cr) instead of MR_PIX_RGB24 - no RGB24 buffer is
 * allocated or written. For a caller (e.g. an AGA direct-to-indexed
 * dither path, or an RTG player converting directly into its retained queue
 * slot) that wants the raw decoded planes instead of a decoder-owned RGB24
 * intermediate. See core/mr_yuv_dither.h for a fused YUV420P -> 8-bit
 * indexed conversion matching mr_yuv420_to_rgb24() + a nearest-neighbour
 * resize + mr_dither_rgb8() bit-exactly, without either intermediate
 * buffer. */
void mr_h264_set_yuv_output(mr_decoder *dec, int enabled);
/* Associate the next compressed access unit with its container PTS.  Libavc
 * may emit an older access unit after display reordering; output_pts() returns
 * the PTS belonging to that emitted frame rather than the current input. */
void mr_h264_set_input_pts(mr_decoder *dec, int has_pts, uint64_t pts_us);
int mr_h264_output_pts(mr_decoder *dec, uint64_t *pts_us);
/* Set when the next mr_decoder_decode() call's data is already Annex-B
 * (start-code-prefixed) NAL data, as MPEG-TS carries H.264 natively - skips
 * the AVCC->Annex-B conversion mr_h264_decode() otherwise always does, and
 * decodes straight from the caller's buffer instead. Not "sticky": pass the
 * current packet's own mr_packet.is_annexb before every decode call, same
 * as mr_h264_set_input_pts(). */
void mr_h264_set_input_annexb(mr_decoder *dec, int is_annexb);
/* Select libavc's quality/performance trade-off. Balanced only degrades
 * non-reference pictures; Fast applies cheaper filtering to all non-key
 * pictures. Turbo keeps Fast's degrade policy and asks libavc to skip B
 * pictures. Turbo+ asks it to skip both P and B pictures, so every displayed
 * picture is a keyframe, and - like TurboGT - requests degradation on every
 * decoded picture, including keyframes, which in this libavc revision
 * additionally disables I-frame deblocking; this keeps Turbo+'s one
 * remaining expensive call (the keyframe decode) short enough that a slow
 * CPU doesn't drain the audio hardware buffer between displayed frames.
 * TurboGT keeps Turbo's B-only skip policy so the P-frame reference chain
 * survives, but requests that same all-picture degradation; faster inter
 * prediction remains restricted to non-reference pictures.
 * Returns non-zero when the decoder accepted both the degrade and frame-skip
 * controls. */
int mr_h264_set_speed_mode(mr_decoder *dec, mr_h264_speed_mode mode);
/* Dynamically escalate to libavc's IVD_SKIP_PB frame-skip mode (skip_pb
 * non-zero) - a near-zero-cost picture skip that reads only the slice
 * header and returns, doing no CABAC/motion-compensation/deblock/
 * reconstruction at all, until the next IDR - or de-escalate back
 * (skip_pb zero) to whatever base skip mode mr_h264_set_speed_mode()'s
 * current performance setting selected (IVD_SKIP_NONE for Quality/
 * Balanced/Fast, IVD_SKIP_B for Turbo/TurboGT, IVD_SKIP_PB for Turbo+ -
 * so de-escalating out of a Turbo+ session is a no-op, not a quality
 * regression). Unlike mr_h264_set_speed_mode(), this touches only the
 * frame-skip control, not degrade/MC-quality settings - intended to be
 * called repeatedly through a session (only on actual state transitions,
 * not every packet - the caller owns that edge detection) as a "Skip
 * Frames" playback mode's own lateness signal comes and goes, not once at
 * open. See mrplay.c's throughput_mode/skip_stale_output and
 * CLAUDE.md's "Live HLS playback stall notes" for why: when Skip Frames
 * mode's existing mr_h264_set_skip_output() skip is engaged only because
 * the decoder itself cannot keep up in real time (not just a full queue
 * or a momentary late packet), skipping the RGB conversion alone barely
 * helps - the expensive part (CABAC/MC/deblock/recon) still runs for
 * every frame regardless. Escalating to real IVD_SKIP_PB frees that CPU
 * back to the scheduler (keeping audio fed) at the cost of freezing the
 * picture until the next keyframe, rather than the reverse. Returns
 * non-zero on success. No-op (returns 0) for a non-H.264 decoder. */
int mr_h264_set_dynamic_skip(mr_decoder *dec, int skip_pb);

#endif /* MR_H264_H */
