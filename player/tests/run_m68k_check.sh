#!/bin/sh
# Run the same conformance suite as `make check`, but built for a real m68k
# target and executed under qemu-m68k user-mode emulation, instead of the
# host's own (x86-64, little-endian, relaxed-alignment) CPU.
#
# This is NOT the AmigaOS toolchain (there isn't one on the dev host - see
# CLAUDE.md) - m68k-linux-gnu-gcc targets Linux/m68k (ELF, glibc), not
# AmigaOS (hunk, clib2/newlib). It cannot build mrplay.c or anything that
# touches dos.h/exec.h. What it DOES give us that plain host testing cannot:
# every CORE file in this list is compiled to real, executed 68030
# instructions, on a big-endian target with the same "unaligned longword
# access is legal on 68030+" assumption ih264_m68k_optim.c depends on. Host
# testing alone is little-endian and can hide an endianness bug behind
# whatever mr_rl*/mr_rb* helper was supposed to prevent it; this cannot.
#
# Usage: sh tests/run_m68k_check.sh
# Requires: m68k-linux-gnu-gcc, qemu-m68k (both installable via apt on
# Debian/Ubuntu: gcc-m68k-linux-gnu binutils-m68k-linux-gnu qemu-user).
set -e
cd "$(dirname "$0")/.."

M68K_CC=${M68K_CC:-m68k-linux-gnu-gcc}
QEMU_M68K=${QEMU_M68K:-qemu-m68k}
# libavc.mk is a Makefile fragment, not shell - hardcode the same -I/-include
# flags its consumers get from it in the main Makefile.
LIBAVC_FLAGS="-Ivendor/libavc_port -Ivendor/libavc/common -Ivendor/libavc/decoder -include vendor/libavc_port/compat.h -fno-strict-aliasing -fwrapv"
LIBMPEG2_FLAGS="-Ivendor/libmpeg2 -Ivendor/libmpeg2/include -Ivendor/libmpeg2/libmpeg2"
WARN_SILENCE="-Wno-unused-parameter -Wno-unused-variable -Wno-unused-function -Wno-unused-but-set-variable -Wno-sign-compare -Wno-implicit-fallthrough -Wno-maybe-uninitialized -Wno-type-limits"
# -DMR_H264_STAGE_PROFILE=1: the mc/deblock/recon/intra timing wrappers
# (ih264d_stage_profile.c) are opt-in on a real playback build (real
# per-call clock() overhead - see ih264d_function_selector_port.c and
# Makefile.amiga's STAGE_PROFILE variable), but this conformance run turns
# them on deliberately so that code path keeps getting exercised on real
# m68k/big-endian hardware, even though the MAE/exec checks below don't
# consume the timing output themselves.
CC="$M68K_CC -O2 -std=c99 -m68030 -static -g -DMR_HAVE_H264 -DMR_M68K_ASM=1 -DMR_PL_MPEG_SKIP_AUDIO_TIME=1 -DMR_H264_STAGE_PROFILE=1 $LIBAVC_FLAGS $LIBMPEG2_FLAGS $WARN_SILENCE"
# Same flags, -mcpu=68060 instead of -m68030 - used once below to prove the
# Fast MP2 decode dispatch inside plm_audio_decode_frame() picks the
# dedicated 68060 kernels correctly end to end (not just that the kernels
# are individually bit-exact, or that the dispatch's own instructions are
# safe - both already covered by the *_060_asm_check tests and
# check_m68060_asm.sh respectively).
CC_060="$M68K_CC -O2 -std=c99 -mcpu=68060 -static -g -DMR_HAVE_H264 -DMR_M68K_ASM=1 -DMR_PL_MPEG_SKIP_AUDIO_TIME=1 -DMR_H264_STAGE_PROFILE=1 $LIBAVC_FLAGS $LIBMPEG2_FLAGS $WARN_SILENCE"

if ! command -v "$M68K_CC" >/dev/null 2>&1; then
    echo "ERROR: $M68K_CC not found. On Debian/Ubuntu:"
    echo "  apt-get install gcc-m68k-linux-gnu binutils-m68k-linux-gnu qemu-user"
    exit 1
fi
if ! command -v "$QEMU_M68K" >/dev/null 2>&1; then
    echo "ERROR: $QEMU_M68K not found (package qemu-user)."
    exit 1
fi
test -f vendor/libavc/decoder/ih264d.h || {
    echo "ERROR: initialise the libavc submodule first:"
    echo "  git submodule update --init player/vendor/libavc"; exit 1; }

test -d tests/assets/ref_h264_high -a -d tests/assets/ref_h263 \
    || sh tests/gen_assets.sh

BUILD=/tmp/mr_m68k_check_build
mkdir -p "$BUILD"

# Any m68k translation unit that compiles pl_mpeg with MR_M68K_ASM enabled
# references this complete MP2 helper set. Keep it in one list so full decoder
# and focused audio tests cannot silently drift from the production link.
MP2_ASM_SRC="core/plm_audio_synth_window_m68k.S \
             core/plm_audio_idct36_m68k.S \
             core/plm_audio_scale_clamp_m68k.S"

# A -mcpu=68060 translation unit never references plm_audio_idct36_m68k or
# plm_audio_synth_window_m68k (both stay gated !defined(__mc68060__) - see
# pl_mpeg.h), but MR_M68K_ASM still pulls in the video-path IDCT/blockset
# helpers, the CPU-independent scale_clamp_m68k, and - since
# plm_audio_decode_frame's dispatch now reaches them unconditionally
# whenever MR_M68K_ASM && MR_CPU_68060 - the dedicated 68060 MP2 kernels.
MP2_ASM_060_SRC="core/mr_mpeg1_idct_m68k.S \
                 core/mr_mpeg1_blockset_m68k.S \
                 core/plm_audio_scale_clamp_m68k.S \
                 core/plm_audio_idct36_m68k_060.S \
                 core/plm_audio_synth_window_m68k_060.S"

CORE="core/mr_codec.c core/mr_source.c core/mr_http.c core/mr_hls.c \
      core/mr_youtube.c core/mr_demux.c core/mr_latm.c core/mr_mkv.c \
      core/mr_avi.c core/mr_mov.c core/mr_ts.c core/mr_ps.c \
      core/mr_raw_mjpeg.c core/mr_raw_mpeg4.c core/mr_cinepak.c \
      core/mr_dither.c core/mr_dither_m68k.S core/mr_ham.c core/mr_scale.c \
      core/mr_c2p.c core/mr_c2p_m68k.S \
      core/mr_mjpeg.c core/picojpeg.c core/mr_mpeg1.c core/mr_mpeg1_blockset_m68k.S \
      core/mr_mpeg1_idct_m68k.S $MP2_ASM_SRC \
      core/mr_mpeg2.c \
      vendor/libmpeg2/libmpeg2/alloc.c vendor/libmpeg2/libmpeg2/cpu_accel.c \
      vendor/libmpeg2/libmpeg2/cpu_state.c vendor/libmpeg2/libmpeg2/decode.c \
      vendor/libmpeg2/libmpeg2/header.c vendor/libmpeg2/libmpeg2/idct.c \
      vendor/libmpeg2/libmpeg2/motion_comp.c vendor/libmpeg2/libmpeg2/slice.c \
      core/mr_mpeg4.c core/mr_msmpeg4v2.c core/mr_wmv.c core/mr_wmv2.c core/mr_h263.c core/mr_h264.c \
      core/mr_msvideo1.c core/mr_rle.c core/mr_rawvideo.c core/mr_yuv.c \
      core/mr_yuv_m68k.S"

# Same as $CORE, but $MP2_ASM_060_SRC instead of $MP2_ASM_SRC - used with
# $CC_060 below for the one 68060-dispatch end-to-end test (mr_mpeg1.c
# built this way already assembles/links fine: $MP2_ASM_060_SRC is proven
# by the *_060_check builds above, which link it at -mcpu=68060 too).
CORE_060="core/mr_codec.c core/mr_source.c core/mr_http.c core/mr_hls.c \
      core/mr_youtube.c core/mr_demux.c core/mr_latm.c core/mr_mkv.c \
      core/mr_avi.c core/mr_mov.c core/mr_ts.c core/mr_ps.c \
      core/mr_raw_mjpeg.c core/mr_raw_mpeg4.c core/mr_cinepak.c \
      core/mr_dither.c core/mr_dither_m68k.S core/mr_ham.c core/mr_scale.c \
      core/mr_c2p.c core/mr_c2p_m68k.S \
      core/mr_mjpeg.c core/picojpeg.c core/mr_mpeg1.c $MP2_ASM_060_SRC \
      core/mr_mpeg2.c \
      vendor/libmpeg2/libmpeg2/alloc.c vendor/libmpeg2/libmpeg2/cpu_accel.c \
      vendor/libmpeg2/libmpeg2/cpu_state.c vendor/libmpeg2/libmpeg2/decode.c \
      vendor/libmpeg2/libmpeg2/header.c vendor/libmpeg2/libmpeg2/idct.c \
      vendor/libmpeg2/libmpeg2/motion_comp.c vendor/libmpeg2/libmpeg2/slice.c \
      core/mr_mpeg4.c core/mr_msmpeg4v2.c core/mr_wmv.c core/mr_wmv2.c core/mr_h263.c core/mr_h264.c \
      core/mr_msvideo1.c core/mr_rle.c core/mr_rawvideo.c core/mr_yuv.c \
      core/mr_yuv_m68k.S"
LIBAVC_SRC="$(printf '%s\n' vendor/libavc/common/*.c \
    | grep -v -e ithread.c -e ih264_resi_trans_quant.c -e ih264_trans_data.c) \
    $(printf '%s\n' vendor/libavc/decoder/*.c) \
    vendor/libavc_port/ih264d_function_selector_port.c \
    vendor/libavc_port/ih264_mc_degrade.c \
    vendor/libavc_port/ih264d_stage_profile.c \
    vendor/libavc_port/ih264d_update_qp_wrap.c \
    vendor/libavc_port/ih264_m68k_optim.c \
    vendor/libavc_port/ih264_m68k_interp.S \
    vendor/libavc_port/ih264_m68k_deblk.S \
    vendor/libavc_port/ih264_m68k_cabac.S \
    vendor/libavc_port/ih264d_cabac_wrap.c \
    vendor/libavc_port/ih264_m68k_chroma_mc.S \
    vendor/libavc_port/ih264_m68k_weighted_pred.S \
    vendor/libavc_port/ih264_m68k_mvpred.S \
    vendor/libavc_port/ih264d_mvpred_dispatch_port.c \
    vendor/libavc_port/ih264_m68k_cabac_coeff.S \
    vendor/libavc_port/ih264_m68k_cabac_coeff8x8.S \
    vendor/libavc_port/ih264d_parse_cabac_coeff_port.c \
    vendor/libavc_port/ih264_m68k_iquant_itrans_recon.S \
    vendor/libavc_port/ih264_m68k_intra_pred.S \
    vendor/libavc_port/ih264_m68k_bs.S \
    vendor/libavc_port/ithread_port.c \
    vendor/libavc_port/compat.c"

echo "== building mr_decode.m68k (m68k-optimised leaf functions + hand asm active) =="
# --wrap=ih264d_decode_bin: redirects every call to that vendored symbol to
# __wrap_ih264d_decode_bin (ih264d_cabac_wrap.c) without editing the
# vendored ih264d_cabac.c inside the libavc submodule - see that file.
# --wrap=ih264d_mvpred_nonmbaff/_nonmbaffB: same trick, redirecting the
# dec_struct_t::pf_mvpred assignments in ih264d_parse_slice.c to
# ih264d_mvpred_dispatch_port.c's reimplementations (which call the hand-
# asm MV predictor primitive internally) - see that file for why the
# primitive itself could not be wrapped directly.
# --wrap=ih264d_parse_residual4x4_cabac/ih264d_read_coeff4x4_cabac: same
# trick again, redirecting to ih264d_parse_cabac_coeff_port.c's
# reimplementations (which call the hand-asm CABAC residual coefficient
# primitive internally) - see that file for the two-symbol split.
# --wrap=ih264d_update_qp: same trick again, redirecting to
# ih264d_update_qp_wrap.c's reimplementation, which replaces the vendored
# function's `% 52` (an extended-dividend DIVSL.L on 68060, confirmed by
# disassembly - see that file) with a divide-free add/subtract reduction.
$CC -o "$BUILD/mr_decode.m68k" tests/mr_decode.c $CORE $LIBAVC_SRC \
    -Wl,--wrap=ih264d_decode_bin \
    -Wl,--wrap=ih264d_mvpred_nonmbaff \
    -Wl,--wrap=ih264d_mvpred_nonmbaffB \
    -Wl,--wrap=ih264d_parse_residual4x4_cabac \
    -Wl,--wrap=ih264d_read_coeff4x4_cabac \
    -Wl,--wrap=ih264d_update_qp

# AC-3 on a real big-endian target. The decoder's IMDCT runs through MintAMP's
# vendored Rockbox FFT, whose MULT32 takes the high half of a 64-bit product
# through a union whose field order follows ROCKBOX_BIG_ENDIAN - which
# decoders/wma/platform.h derives from AMIGA_M68K. Here that define is true, so
# unlike the host build (see WMA_FFT_FLAGS in the Makefile) everything is
# compiled with it. This is the one build where that claim can actually be
# checked against ffmpeg's decode, which is the whole reason the check exists.
# Built with MR_AC3_CHECK_NO_DEMUX so it takes a raw .ac3 and needs only the
# audio adapter - no container or H.264 tier to cross-build.
echo "== building mr_ac3_check.m68k =="
MINTAMP_ROOT=vendor/MintAMP
MINTAMP_FLAGS="-DAMIGA_M68K -DMR_HOST_BUILD -DARDUINO -DESP8266 \
    -I$MINTAMP_ROOT/pub -I$MINTAMP_ROOT/real -I$MINTAMP_ROOT/decoders/aac \
    -I$MINTAMP_ROOT/decoders/aac-arduino-shim -Ivendor/liba52"
MINTAMP_SRC="$MINTAMP_ROOT/mp3dec.c $MINTAMP_ROOT/mp3tabs.c \
    $MINTAMP_ROOT/real/*.c \
    $(printf '%s\n' $MINTAMP_ROOT/decoders/aac/*.c | grep -v '/sbr') \
    $MINTAMP_ROOT/decoders/wma/fft-ffmpeg.c \
    $MINTAMP_ROOT/decoders/wma/mdct_lookup.c \
    vendor/liba52/bit_allocate.c vendor/liba52/bitstream.c \
    vendor/liba52/downmix.c vendor/liba52/imdct.c vendor/liba52/parse.c"
test -f tests/assets/test_ac3.ac3 -a -f tests/assets/ref_ac3_mkv.raw \
    || sh tests/gen_audio_assets.sh
# shellcheck disable=SC2086
$CC -DMR_AC3_CHECK_NO_DEMUX $MINTAMP_FLAGS -o "$BUILD/mr_ac3_check.m68k" \
    tests/mr_ac3_check.c audio/mr_audio_decode.c audio/mr_pcm.c \
    core/mr_mpeg1.c core/mr_mpeg1_idct_m68k.S \
    core/mr_mpeg1_blockset_m68k.S $MP2_ASM_SRC core/mr_latm.c $MINTAMP_SRC -lm

# Same AC-3 decode, but -mcpu=68060 and with -DAMIGA_M68K_WMA_ASM - i.e. the
# same fft-ffmpeg.c/codeclib_misc.h MULT32() this file's default $CC build
# above exercises through the int64_t fallback, now built exactly the way
# Makefile.amiga's CPU=68060 target actually compiles it (see
# LIBA52_FFT_CPPFLAGS there). Without the define, MULT32 on a real 68060
# would fall to `jsr __muldi3` per FFT butterfly - see
# check_m68060_asm.sh's fft-ffmpeg.o scan for the instruction-level half of
# this proof. This half proves the reconstructed product is still numerically
# correct: WmaM68kMultiply's four-hardware-partial-product high-word
# recovery is an exact reimplementation of the same 32x32->64 result, so this
# is expected to pass against the identical ffmpeg reference and tolerance
# as the 68030 build above, not a relaxed one.
echo "== building mr_ac3_check_060.m68k (CPU=68060 AMIGA_M68K_WMA_ASM fix) =="
$CC_060 -DMR_AC3_CHECK_NO_DEMUX $MINTAMP_FLAGS -DAMIGA_M68K_WMA_ASM \
    -o "$BUILD/mr_ac3_check_060.m68k" \
    tests/mr_ac3_check.c audio/mr_audio_decode.c audio/mr_pcm.c \
    core/mr_mpeg1.c $MP2_ASM_060_SRC core/mr_latm.c $MINTAMP_SRC -lm

# MP2 out of a .mpg on a real big-endian target. The MPEG-1 source decodes
# audio with pl_mpeg's own integer Layer II code and hands it back as
# explicitly little-endian bytes (mr_mpeg1_audio()), which is precisely the
# kind of claim the host build cannot test.
echo "== building mr_mp2_check.m68k =="
test -f tests/assets/test_mpeg1_mp2.mpg -a -f tests/assets/ref_mpeg1_mp2.raw \
    || sh tests/gen_audio_assets.sh
# pl_mpeg's video path and MP2 audio path both call hand-written m68k helpers
# on this target, so keep the complete helper set in the focused decode link.
$CC -o "$BUILD/mr_mp2_check.m68k" tests/mr_mp2_check.c core/mr_mpeg1.c \
    core/mr_mpeg1_idct_m68k.S core/mr_mpeg1_blockset_m68k.S $MP2_ASM_SRC

echo "== building mr_mpeg1_decim_check.m68k (Fast MP2 mode vs a real elementary stream) =="
$CC -o "$BUILD/mr_mpeg1_decim_check.m68k" tests/mr_mpeg1_decim_check.c $CORE $LIBAVC_SRC \
    -Wl,--wrap=ih264d_decode_bin \
    -Wl,--wrap=ih264d_mvpred_nonmbaff \
    -Wl,--wrap=ih264d_mvpred_nonmbaffB \
    -Wl,--wrap=ih264d_parse_residual4x4_cabac \
    -Wl,--wrap=ih264d_read_coeff4x4_cabac

# Same test, -mcpu=68060 - proves the Fast MP2 dispatch inside
# plm_audio_decode_frame() picks plm_audio_idct36_m68k_060/
# plm_audio_synth_window_m68k_060 correctly end to end, not just that
# each kernel is bit-exact in isolation (mr_mp2_idct_060_asm_check.c/
# mr_mp2_synth_060_asm_check.c) or that the dispatch's own instructions are
# safe (check_m68060_asm.sh).
echo "== building mr_mpeg1_decim_check_060.m68k (same, -mcpu=68060 dispatch) =="
$CC_060 -o "$BUILD/mr_mpeg1_decim_check_060.m68k" tests/mr_mpeg1_decim_check.c $CORE_060 $LIBAVC_SRC \
    -Wl,--wrap=ih264d_decode_bin \
    -Wl,--wrap=ih264d_mvpred_nonmbaff \
    -Wl,--wrap=ih264d_mvpred_nonmbaffB \
    -Wl,--wrap=ih264d_parse_residual4x4_cabac \
    -Wl,--wrap=ih264d_read_coeff4x4_cabac

echo "== building mr_h264_m68k_check.m68k =="
$CC -o "$BUILD/mr_h264_m68k_check.m68k" tests/mr_h264_m68k_check.c \
    vendor/libavc_port/ih264_m68k_optim.c \
    vendor/libavc_port/ih264_m68k_interp.S \
    vendor/libavc_port/ih264_m68k_deblk.S \
    vendor/libavc_port/ih264_m68k_cabac.S \
    vendor/libavc_port/ih264_m68k_chroma_mc.S \
    vendor/libavc_port/ih264_m68k_weighted_pred.S \
    vendor/libavc_port/ih264_m68k_mvpred.S \
    vendor/libavc_port/ih264_m68k_iquant_itrans_recon.S \
    vendor/libavc_port/ih264_m68k_intra_pred.S

echo "== building mr_h264_cabac_coeff_check.m68k (real ih264d_read_coeff4x4_cabac vs asm) =="
$CC -o "$BUILD/mr_h264_cabac_coeff_check.m68k" tests/mr_h264_cabac_coeff_check.c $LIBAVC_SRC

echo "== building mr_h264_mvpred_dispatch_check.m68k (real ih264d_mvpred_nonmbaff/_nonmbaffB vs asm) =="
$CC -o "$BUILD/mr_h264_mvpred_dispatch_check.m68k" tests/mr_h264_mvpred_dispatch_check.c $LIBAVC_SRC

echo "== building mr_h264_mc_degrade_check.m68k (exact chroma/luma filter sets + bilinear vs spec) =="
$CC -o "$BUILD/mr_h264_mc_degrade_check.m68k" tests/mr_h264_mc_degrade_check.c $LIBAVC_SRC

echo "== building mr_h264_recon8x8_check.m68k (real ih264_iquant_itrans_recon_8x8/_dc vs asm) =="
$CC -o "$BUILD/mr_h264_recon8x8_check.m68k" tests/mr_h264_recon8x8_check.c $LIBAVC_SRC

echo "== building mr_h264_intra8x8_check.m68k (real luma 8x8 intra pred modes + ref filtering vs asm) =="
$CC -o "$BUILD/mr_h264_intra8x8_check.m68k" tests/mr_h264_intra8x8_check.c $LIBAVC_SRC

echo "== building mr_h264_intra_chroma_check.m68k (real chroma_8x8 intra pred modes vs asm) =="
$CC -o "$BUILD/mr_h264_intra_chroma_check.m68k" tests/mr_h264_intra_chroma_check.c $LIBAVC_SRC

echo "== building mr_h264_bs_check.m68k (real P-16x16 boundary strength vs asm) =="
$CC -o "$BUILD/mr_h264_bs_check.m68k" tests/mr_h264_bs_check.c $LIBAVC_SRC

echo "== building mr_yuv_check.m68k =="
$CC -o "$BUILD/mr_yuv_check.m68k" tests/mr_yuv_check.c core/mr_yuv.c \
    core/mr_yuv_m68k.S

echo "== building mr_scale_check.m68k / mr_c2p_check.m68k / mr_ham_check.m68k / mr_dither_check.m68k =="
$CC -o "$BUILD/mr_scale_check.m68k" tests/mr_scale_check.c core/mr_scale.c
$CC -o "$BUILD/mr_c2p_check.m68k" tests/mr_c2p_check.c core/mr_c2p.c \
    core/mr_c2p_m68k.S
$CC -o "$BUILD/mr_ham_check.m68k" tests/mr_ham_check.c core/mr_ham.c
$CC -o "$BUILD/mr_dither_check.m68k" tests/mr_dither_check.c core/mr_dither.c \
    core/mr_dither_m68k.S

echo "== building mr_cinepak_indexed_check.m68k =="
$CC -o "$BUILD/mr_cinepak_indexed_check.m68k" \
    tests/mr_cinepak_indexed_check.c core/mr_avi.c core/mr_rawvideo.c \
    core/mr_cinepak.c core/mr_dither.c core/mr_dither_m68k.S

echo "== building mr_mp2_idct_check.m68k =="
$M68K_CC -O2 -std=c99 -m68030 -static -DMR_M68K_ASM=1 -o "$BUILD/mr_mp2_idct_check.m68k" \
    tests/mr_mp2_idct_check.c core/plm_audio_idct36_m68k.S

echo "== building mr_mp2_synth_check.m68k =="
$M68K_CC -O2 -std=c99 -m68030 -static -DMR_M68K_ASM=1 -o "$BUILD/mr_mp2_synth_check.m68k" \
    tests/mr_mp2_synth_check.c core/plm_audio_synth_window_m68k.S

echo "== building mr_mpeg1_decim_synth_check.m68k (68030) =="
$M68K_CC -O2 -std=c99 -m68030 -static -o "$BUILD/mr_mpeg1_decim_synth_check.m68k" \
    tests/mr_mpeg1_decim_synth_check.c

echo "== building mr_mp2_scale_check.m68k =="
$M68K_CC -O2 -std=c99 -m68030 -static -DMR_M68K_ASM=1 -o "$BUILD/mr_mp2_scale_check.m68k" \
    tests/mr_mp2_scale_check.c core/plm_audio_scale_clamp_m68k.S

# 68060 has no hardware 64-bit-result MULS.L/MULU.L (see pl_mpeg.h's
# plm_audio_smul64_060 comment), so these build with -mcpu=68060 rather than
# -m68030 to actually exercise that CPU's code path, and deliberately do not
# use the MR_TEST_M68K_*/undef-MR_M68K_ASM isolation the 68040 checks above
# use - that trick would also disable the 68060 guard under test here.
echo "== building mr_mp2_mul64_060_check.m68k =="
$M68K_CC -O2 -std=c99 -mcpu=68060 -static -DMR_M68K_ASM=1 \
    -o "$BUILD/mr_mp2_mul64_060_check.m68k" \
    tests/mr_mp2_mul64_060_check.c $MP2_ASM_060_SRC

echo "== building mr_mp2_idct_060_check.m68k =="
$M68K_CC -O2 -std=c99 -mcpu=68060 -static -DMR_M68K_ASM=1 \
    -o "$BUILD/mr_mp2_idct_060_check.m68k" \
    tests/mr_mp2_idct_060_check.c $MP2_ASM_060_SRC

echo "== building mr_mp2_synth_060_check.m68k =="
$M68K_CC -O2 -std=c99 -mcpu=68060 -static -DMR_M68K_ASM=1 \
    -o "$BUILD/mr_mp2_synth_060_check.m68k" \
    tests/mr_mp2_synth_060_check.c $MP2_ASM_060_SRC

# Dedicated 68060 hand kernels (real hand-tuned asm, not the portable-C
# fallback the two checks above exercise) - see each .S file's own header.
echo "== building mr_mp2_idct_060_asm_check.m68k (dedicated 68060 IDCT kernel) =="
$M68K_CC -O2 -std=c99 -mcpu=68060 -static \
    -o "$BUILD/mr_mp2_idct_060_asm_check.m68k" \
    tests/mr_mp2_idct_060_asm_check.c core/plm_audio_idct36_m68k_060.S

echo "== building mr_mp2_synth_060_asm_check.m68k (dedicated 68060 synth kernel) =="
$M68K_CC -O2 -std=c99 -mcpu=68060 -static \
    -o "$BUILD/mr_mp2_synth_060_asm_check.m68k" \
    tests/mr_mp2_synth_060_asm_check.c core/plm_audio_synth_window_m68k_060.S

echo "== building mr_mpeg1_decim_synth_check.m68k (68060, exercises plm_audio_smul64_060) =="
$M68K_CC -O2 -std=c99 -mcpu=68060 -static -DMR_M68K_ASM=1 \
    -o "$BUILD/mr_mpeg1_decim_synth_check_060.m68k" \
    tests/mr_mpeg1_decim_synth_check.c $MP2_ASM_060_SRC

# amiga/audio_paula.c's session-invariant-divisor multiply/divide helpers
# (core/mr_muldiv64.h). Portable - no .S files to link - but its 68060 path
# only compiles in with both MR_M68K_ASM and -mcpu=68060, so build it at
# both CPU tiers actually used in production (Makefile.amiga's CPUFLAGS is
# -m$(CPU)) to exercise the plain-C hardware-muls.l path on 68040 and the
# mulu.w-based trap-free path on 68060.
echo "== building mr_muldiv64_check.m68k (68040) =="
$M68K_CC -O2 -std=c99 -mcpu=68040 -static -DMR_M68K_ASM=1 \
    -o "$BUILD/mr_muldiv64_check_040.m68k" tests/mr_muldiv64_check.c

echo "== building mr_muldiv64_check.m68k (68060) =="
$M68K_CC -O2 -std=c99 -mcpu=68060 -static -DMR_M68K_ASM=1 \
    -o "$BUILD/mr_muldiv64_check_060.m68k" tests/mr_muldiv64_check.c

# vendor/libavc_port/ih264_m68k_divmod.h's mr_ih264_divmod_u32() - only its
# -mcpu=68060 build actually exercises the inline-asm DIVU.L path (68040
# keeps the plain C fallback, same as the host); both are built to prove
# neither tier's codegen regresses.
echo "== building mr_ih264_divmod_check.m68k (68040) =="
$M68K_CC -O2 -std=c99 -mcpu=68040 -static -Ivendor/libavc/common \
    -o "$BUILD/mr_ih264_divmod_check_040.m68k" tests/mr_ih264_divmod_check.c

echo "== building mr_ih264_divmod_check.m68k (68060) =="
$M68K_CC -O2 -std=c99 -mcpu=68060 -static -Ivendor/libavc/common \
    -o "$BUILD/mr_ih264_divmod_check_060.m68k" tests/mr_ih264_divmod_check.c

echo "== building mr_yuv_dither_check.m68k =="
# Links against the real hand-asm mr_yuv420_to_rgb24_m68k/mr_dither_rgb8_m68k
# (via core/mr_yuv.c core/mr_dither.c's own MR_M68K_ASM dispatch, active in
# this build) as the reference composition mr_yuv420_dither8() is compared
# against - so on real m68k this also cross-checks the fused C path against
# the real accelerated three-stage pipeline, not just a host-only reference.
$CC -o "$BUILD/mr_yuv_dither_check.m68k" tests/mr_yuv_dither_check.c \
    core/mr_yuv_dither.c core/mr_yuv_dither_m68k.S core/mr_yuv.c \
    core/mr_yuv_m68k.S core/mr_scale.c core/mr_dither.c core/mr_dither_m68k.S

# The AGA direct-planar C2P kernel (mr_c2p_mode MR_C2P_DIRECT) - this is the
# only place its bit-exactness against the established
# dither-then-transpose-C2P composition is ever actually proven: the test
# itself is a no-op on the host build (see mr_yuv_planar_queue_check.c's own
# header comment - the centering it checks only exists inside the m68k asm
# dispatch, never reached without MR_M68K_ASM). core/mr_yuv_dither_planar_m68k.S
# wraps the established mr_yuv_dither_m68k.S under a renamed symbol and
# republishes mr_yuv420_dither8_m68k as a tiny runtime dispatcher, so the
# established core/mr_yuv_dither_m68k.S must NOT also be linked directly here
# (it would double-define that symbol) - mirrors Makefile.fused's own
# FUSED_CORE exclusion of it, for the same reason. Built and run at both
# 68030 (the shared baseline tier every other m68k build here exercises) and
# 68060, since the direct kernel's mulu.l/muls.l instructions need the same
# 68060-safety proof as everything else in this file - see
# check_m68060_asm.sh for the disassembly side of that proof.
echo "== building mr_yuv_planar_queue_check.m68k (68030) =="
$CC -o "$BUILD/mr_yuv_planar_queue_check_030.m68k" tests/mr_yuv_planar_queue_check.c \
    core/mr_yuv_dither.c core/mr_yuv_dither_planar_m68k.S \
    core/mr_yuv_dither_planar_direct_m68k.S core/mr_yuv_planar_queue.c \
    core/mr_c2p.c core/mr_c2p_m68k.S core/mr_yuv.c core/mr_yuv_m68k.S \
    core/mr_scale.c core/mr_dither.c core/mr_dither_m68k.S

echo "== building mr_yuv_planar_queue_check.m68k (68060) =="
$CC_060 -o "$BUILD/mr_yuv_planar_queue_check_060.m68k" tests/mr_yuv_planar_queue_check.c \
    core/mr_yuv_dither.c core/mr_yuv_dither_planar_m68k.S \
    core/mr_yuv_dither_planar_direct_m68k.S core/mr_yuv_planar_queue.c \
    core/mr_c2p.c core/mr_c2p_m68k.S core/mr_yuv.c core/mr_yuv_m68k.S \
    core/mr_scale.c core/mr_dither.c core/mr_dither_m68k.S

echo "== building mr_yuv_ham_check.m68k =="
# Same idea for HAM: the reference composition links the real hand-asm
# mr_yuv420_to_rgb24_m68k, so this cross-checks the fused encoder against the
# accelerated three-stage pipeline on real big-endian m68k.
$CC -o "$BUILD/mr_yuv_ham_check.m68k" tests/mr_yuv_ham_check.c \
    core/mr_yuv_ham.c core/mr_yuv.c core/mr_yuv_m68k.S core/mr_scale.c \
    core/mr_ham.c

echo "== building mr_mpeg1_blockset_check.m68k / mr_mpeg1_idct_check.m68k =="
$CC -o "$BUILD/mr_mpeg1_blockset_check.m68k" tests/mr_mpeg1_blockset_check.c \
    core/mr_mpeg1_blockset_m68k.S
$CC -o "$BUILD/mr_mpeg1_idct_check.m68k" tests/mr_mpeg1_idct_check.c \
    core/mr_mpeg1_idct_m68k.S

echo "== building mr_media_clock_check.m68k =="
$CC -o "$BUILD/mr_media_clock_check.m68k" tests/mr_media_clock_check.c \
    core/mr_media_clock.c

echo "== building mr_micro_rescue_check.m68k =="
$CC -o "$BUILD/mr_micro_rescue_check.m68k" tests/mr_micro_rescue_check.c

run() { echo "[qemu-m68k] $*"; "$QEMU_M68K" "$@"; }

run "$BUILD/mr_h264_m68k_check.m68k"
run "$BUILD/mr_h264_cabac_coeff_check.m68k"
run "$BUILD/mr_h264_mvpred_dispatch_check.m68k"
run "$BUILD/mr_h264_mc_degrade_check.m68k"
run "$BUILD/mr_h264_recon8x8_check.m68k"
run "$BUILD/mr_h264_intra8x8_check.m68k"
run "$BUILD/mr_h264_intra_chroma_check.m68k"
run "$BUILD/mr_h264_bs_check.m68k"
run "$BUILD/mr_yuv_check.m68k"
run "$BUILD/mr_scale_check.m68k"
run "$BUILD/mr_c2p_check.m68k"
run "$BUILD/mr_ham_check.m68k"
run "$BUILD/mr_dither_check.m68k"
run "$BUILD/mr_cinepak_indexed_check.m68k" tests/assets/test_cinepak.avi
run "$BUILD/mr_cinepak_indexed_check.m68k" tests/assets/test_cinepak_strips.avi
run "$BUILD/mr_mp2_synth_check.m68k"
run "$BUILD/mr_mp2_scale_check.m68k"
run "$BUILD/mr_mp2_idct_check.m68k"
run "$BUILD/mr_mp2_mul64_060_check.m68k"
run "$BUILD/mr_mp2_idct_060_check.m68k"
run "$BUILD/mr_mp2_synth_060_check.m68k"
run "$BUILD/mr_mp2_idct_060_asm_check.m68k"
run "$BUILD/mr_mp2_synth_060_asm_check.m68k"
run "$BUILD/mr_mpeg1_decim_synth_check.m68k"
run "$BUILD/mr_mpeg1_decim_synth_check_060.m68k"
run "$BUILD/mr_muldiv64_check_040.m68k"
run "$BUILD/mr_muldiv64_check_060.m68k"
run "$BUILD/mr_ih264_divmod_check_040.m68k"
run "$BUILD/mr_ih264_divmod_check_060.m68k"
run "$BUILD/mr_yuv_dither_check.m68k"
run "$BUILD/mr_yuv_planar_queue_check_030.m68k"
run "$BUILD/mr_yuv_planar_queue_check_060.m68k"
run "$BUILD/mr_yuv_ham_check.m68k"
run "$BUILD/mr_mpeg1_blockset_check.m68k"
run "$BUILD/mr_mpeg1_idct_check.m68k"
run "$BUILD/mr_media_clock_check.m68k"
run "$BUILD/mr_micro_rescue_check.m68k"

echo "[AC-3 vs ffmpeg, real m68k/big-endian]"
run "$BUILD/mr_ac3_check.m68k" tests/assets/test_ac3.ac3 \
    tests/assets/ref_ac3_mkv.raw 32000 1

echo "[AC-3 vs ffmpeg, real m68k/big-endian, CPU=68060 AMIGA_M68K_WMA_ASM fix]"
run "$BUILD/mr_ac3_check_060.m68k" tests/assets/test_ac3.ac3 \
    tests/assets/ref_ac3_mkv.raw 32000 1

echo "[MP2 from an MPEG-1 program stream vs ffmpeg, real m68k/big-endian]"
run "$BUILD/mr_mp2_check.m68k" tests/assets/test_mpeg1_mp2.mpg \
    tests/assets/ref_mpeg1_mp2.raw 22050 1

echo "[MP2 Fast decode mode (plm_audio_set_decim) vs a real elementary stream, real m68k/big-endian]"
run "$BUILD/mr_mpeg1_decim_check.m68k" tests/assets/test_mp2_stereo.ts

echo "[MP2 Fast decode mode, -mcpu=68060 dispatch (dedicated kernels), real m68k/big-endian]"
run "$BUILD/mr_mpeg1_decim_check_060.m68k" tests/assets/test_mp2_stereo.ts

echo "[H.264 High Profile avc1 + B-frames, real m68k/big-endian]"
run "$BUILD/mr_decode.m68k" tests/assets/test_h264_high.mp4 \
    --check tests/assets/ref_h264_high
echo "[H.264 borrowed YUV display-buffer lifecycle, real m68k/big-endian]"
run "$BUILD/mr_decode.m68k" tests/assets/test_h264_high.mp4 --h264-yuv \
    | grep -F "decoded 24 frames"
echo "[MPEG-TS: H.264 Annex-B direct decode, real m68k/big-endian]"
# The TS demuxer hands H.264 packets straight through as Annex-B
# (mr_ts.c's emit_pes()/pkt.is_annexb - see mr_h264_set_input_annexb()),
# skipping the AVCC round-trip the MOV/MP4 path above still needs. Real
# big-endian coverage for that path specifically, not just the MOV one.
run "$BUILD/mr_decode.m68k" tests/assets/test_h264_aac.ts \
    --check tests/assets/ref_h264_high
echo "[Cinepak AVI, real m68k/big-endian]"
run "$BUILD/mr_decode.m68k" tests/assets/test_cinepak.avi \
    --check tests/assets/ref_cinepak
echo "[Cinepak AVI multi-strip, real m68k/big-endian]"
run "$BUILD/mr_decode.m68k" tests/assets/test_cinepak_strips.avi \
    --check tests/assets/ref_cinepak_strips
# MSVideo1 reads its colour words little-endian out of a big-endian build and
# indexes an 8-bit palette, so both variants belong on the real m68k target.
echo "[Microsoft Video 1 RGB555, real m68k/big-endian]"
run "$BUILD/mr_decode.m68k" tests/assets/test_msvideo1.avi \
    --check tests/assets/ref_msvideo1
echo "[Microsoft Video 1 8-bit paletted, real m68k/big-endian]"
run "$BUILD/mr_decode.m68k" tests/assets/test_msvideo1_pal8.avi \
    --check tests/assets/ref_msvideo1_pal8
echo "[Microsoft RLE, real m68k/big-endian]"
run "$BUILD/mr_decode.m68k" tests/assets/test_msrle.avi \
    --check tests/assets/ref_msrle
echo "[MPEG-4 Part 2 Simple Profile, real m68k/big-endian]"
run "$BUILD/mr_decode.m68k" tests/assets/test_mp4v_sp.avi \
    --check tests/assets/ref_mp4v_sp
echo "[Microsoft MPEG-4 v2 / MP42, real m68k/big-endian]"
run "$BUILD/mr_decode.m68k" tests/assets/test_mp42.avi \
    --check tests/assets/ref_mp42
echo "[WMV1 / WMV7, real m68k/big-endian]"
run "$BUILD/mr_decode.m68k" tests/assets/test_wmv1.avi \
    --check tests/assets/ref_wmv1
run "$BUILD/mr_decode.m68k" tests/assets/test_wmv1_q20.avi \
    --check tests/assets/ref_wmv1_q20
echo "[WMV2 / WMV8, real m68k/big-endian]"
run "$BUILD/mr_decode.m68k" tests/assets/test_wmv2.avi \
    --check tests/assets/ref_wmv2
run "$BUILD/mr_decode.m68k" tests/assets/test_wmv2_q20.avi \
    --check tests/assets/ref_wmv2_q20
echo "[H.263 version 1, real m68k/big-endian]"
run "$BUILD/mr_decode.m68k" tests/assets/test_h263.avi \
    --check tests/assets/ref_h263
echo "[H.263+ custom picture format / slice structured, real m68k/big-endian]"
run "$BUILD/mr_decode.m68k" tests/assets/test_h263p.avi \
    --check tests/assets/ref_h263p
# The YUV handoff reads libmpeg2's planes at their macroblock-aligned pitch and
# packs them, so it belongs on the real big-endian target. Built here rather
# than with the shared CORE list because it drives the codec vtable directly.
echo "== building mr_mpeg2_yuv_check.m68k =="
$CC -o "$BUILD/mr_mpeg2_yuv_check.m68k" tests/mr_mpeg2_yuv_check.c \
    core/mr_mpeg2.c core/mr_ps.c core/mr_yuv.c core/mr_yuv_m68k.S \
    vendor/libmpeg2/libmpeg2/alloc.c vendor/libmpeg2/libmpeg2/cpu_accel.c \
    vendor/libmpeg2/libmpeg2/cpu_state.c vendor/libmpeg2/libmpeg2/decode.c \
    vendor/libmpeg2/libmpeg2/header.c vendor/libmpeg2/libmpeg2/idct.c \
    vendor/libmpeg2/libmpeg2/motion_comp.c vendor/libmpeg2/libmpeg2/slice.c
echo "[MPEG-1/2 YUV420P output handoff, real m68k/big-endian]"
run "$BUILD/mr_mpeg2_yuv_check.m68k" tests/assets/test_mpeg1_odd.mpg 134 100

# A PES timestamp is a 33-bit value assembled out of five bytes by shifting
# past marker bits, so it is exactly the kind of thing that can come out right
# on the host and wrong on a big-endian target.
echo "== building mr_ps_pts_check.m68k =="
$CC -o "$BUILD/mr_ps_pts_check.m68k" tests/mr_ps_pts_check.c core/mr_ps.c
echo "[MPEG-PS PES timestamps, real m68k/big-endian]"
run "$BUILD/mr_ps_pts_check.m68k" tests/assets/test_mpeg1_odd.mpg 540000 40000
run "$BUILD/mr_ps_pts_check.m68k" tests/assets/test_mpeg1_mp2.mpg \
    540000 40000 518188

echo "[MPEG-1, real m68k/big-endian - exercises the new motion-comp asm]"
run "$BUILD/mr_decode.m68k" tests/assets/test_mpeg1.mpg \
    --check tests/assets/ref_mpeg1

echo "== 68060 disassembly check: MP2 hot path free of extended MULS.L/MULU.L, =="
echo "== extended divide, and __muldi3/__divdi3/__udivdi3 =="
sh tests/check_m68060_asm.sh

echo "m68k/big-endian check: OK"
