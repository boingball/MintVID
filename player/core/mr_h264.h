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
     * residual coefficient parsing, MV prediction, and per-MB neighbour-
     * availability/context setup - see that header for why these four are
     * disjoint from each other. mbparse_us/intramb_us are different: each
     * is the whole wall-clock cost of one macroblock-type syntax dispatch
     * (pf_parse_inter_mb for inter MBs, ih264d_parse_imb_cabac for intra
     * MBs - see ih264d_mbinfo_wrap_port.c/ih264d_intramb_wrap_port.c), so
     * both necessarily overlap bin/coeff/mvpred, since that's what they
     * call into - treat them as direct measurements of (most of) the
     * remainder below, not a fifth/sixth additive bucket. A real A1200
     * retest found mbparse_us alone only explains ~12% of the remainder
     * (most macroblocks on real content are skip or intra, never reaching
     * pf_parse_inter_mb at all - see CLAUDE.md's "mbparse_us retest"),
     * which is what motivated adding intramb_us as its intra-side sibling.
     * A real A1200 retest with intramb_us wired in found mbparse_us+
     * intramb_us together only explained ~13 of the ~49.5% pre-existing
     * remainder, leaving ~36% still unattributed - see CLAUDE.md's
     * "intramb_us"/terminate_us-mbtype_us sections. terminate_us/mbtype_us
     * are two more per-MB costs found the same way: terminate_us (ih264d_
     * decode_terminate - the CABAC end_of_slice_flag bin, called once per
     * *macroblock*, skip or not) is genuinely disjoint from every other
     * bucket; mbtype_us (ih264d_parse_mb_type_cabac - the mb_type syntax
     * dispatch for a non-skip MB) overlaps bin_us the same way mbparse_us/
     * intramb_us do. core_us minus mc/deblock/recon/intra/bin/coeff/
     * mvpred/mbinfo/mbparse/intramb/terminate/mbtype is what remains
     * unattributed by subtraction - possibly dominated by per-MB-loop
     * skip-macroblock bookkeeping, which has no function-pointer boundary
     * of its own to hook and so is not measured here at all yet. */
    unsigned long bin_us, bin_count, coeff_us, coeff_count;
    unsigned long mvpred_us, mvpred_count;
    unsigned long mbinfo_us, mbinfo_count;
    unsigned long mbparse_us, mbparse_count;
    unsigned long intramb_us, intramb_count;
    unsigned long terminate_us, terminate_count;
    unsigned long mbtype_us, mbtype_count;
} mr_h264_timing;
typedef enum mr_h264_speed_mode {
    MR_H264_SPEED_QUALITY = 0,
    MR_H264_SPEED_BALANCED,
    MR_H264_SPEED_FAST,
    MR_H264_SPEED_TURBO,
    MR_H264_SPEED_TURBO_PLUS,
    /* Turbo's decode policy; the player additionally drops late P/B access
     * units with mr_h264_set_drop_nonsync(). */
    MR_H264_SPEED_SMOOSH
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
/* Smoosh ("datamosh") frame dropping, for the next mr_decoder_decode() call
 * only - pass it before every decode, like mr_h264_set_input_pts(). When
 * set, an access unit with no keyframe (IDR, I or SI slice) and no SPS/PPS
 * is not given to libavc at all and the call returns MR_SKIPPED. Later
 * P pictures then predict from a stale reference, so the picture smears
 * until the next keyframe restores it exactly. That is the whole point: on
 * a stream with no B-frames (YouTube's 360p itag 18 is Baseline) every
 * P picture is a reference, so this is the only way to shed decode work
 * without freezing the picture until the next keyframe. Anything the
 * classifier cannot parse is treated as a keyframe and decoded. */
void mr_h264_set_drop_nonsync(mr_decoder *dec, int drop);
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
 * pictures. Turbo keeps Fast's degrade policy, requests degradation on every
 * decoded picture (including keyframes - see mr_h264_set_speed_mode()'s own
 * comment for why a mixed degrade policy is unsafe), and asks libavc to skip
 * B pictures. Turbo+ asks it to skip both P and B pictures too, so every
 * displayed picture is a keyframe; that all-picture degradation, which in
 * this libavc revision also disables I-frame deblocking, keeps Turbo+'s one
 * remaining expensive call (the keyframe decode) short enough that a slow
 * CPU doesn't drain the audio hardware buffer between displayed frames.
 * Returns non-zero when the decoder accepted both the degrade and frame-skip
 * controls. */
int mr_h264_set_speed_mode(mr_decoder *dec, mr_h264_speed_mode mode);
/* Dynamically escalate to libavc's IVD_SKIP_PB frame-skip mode (skip_pb
 * non-zero) - a near-zero-cost picture skip that reads only the slice
 * header and returns, doing no CABAC/motion-compensation/deblock/
 * reconstruction at all, until the next IDR - or de-escalate back
 * (skip_pb zero) to whatever base skip mode mr_h264_set_speed_mode()'s
 * current performance setting selected (IVD_SKIP_NONE for Quality/
 * Balanced/Fast, IVD_SKIP_B for Turbo, IVD_SKIP_PB for Turbo+ -
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
