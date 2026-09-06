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
CC="$M68K_CC -O2 -std=c99 -m68030 -static -g -DMR_HAVE_H264 -DMR_M68K_ASM=1 -DMR_H264_STAGE_PROFILE=1 $LIBAVC_FLAGS $LIBMPEG2_FLAGS $WARN_SILENCE"

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

test -d tests/assets/ref_h264_high || sh tests/gen_assets.sh

BUILD=/tmp/mr_m68k_check_build
mkdir -p "$BUILD"

CORE="core/mr_codec.c core/mr_source.c core/mr_http.c core/mr_hls.c \
      core/mr_youtube.c core/mr_demux.c core/mr_latm.c core/mr_mkv.c \
      core/mr_avi.c core/mr_mov.c core/mr_ts.c core/mr_ps.c \
      core/mr_raw_mjpeg.c core/mr_raw_mpeg4.c core/mr_cinepak.c \
      core/mr_dither.c core/mr_dither_m68k.S core/mr_ham.c core/mr_scale.c \
      core/mr_c2p.c core/mr_c2p_m68k.S \
      core/mr_mjpeg.c core/picojpeg.c core/mr_mpeg1.c core/mr_mpeg1_blockset_m68k.S \
      core/mr_mpeg1_idct_m68k.S \
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
$CC -DMR_HAVE_MPEG1 -o "$BUILD/mr_decode.m68k" tests/mr_decode.c $CORE $LIBAVC_SRC \
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

echo "== building mr_yuv_dither_check.m68k =="
# Links against the real hand-asm mr_yuv420_to_rgb24_m68k/mr_dither_rgb8_m68k
# (via core/mr_yuv.c core/mr_dither.c's own MR_M68K_ASM dispatch, active in
# this build) as the reference composition mr_yuv420_dither8() is compared
# against - so on real m68k this also cross-checks the fused C path against
# the real accelerated three-stage pipeline, not just a host-only reference.
$CC -o "$BUILD/mr_yuv_dither_check.m68k" tests/mr_yuv_dither_check.c \
    core/mr_yuv_dither.c core/mr_yuv_dither_m68k.S core/mr_yuv.c \
    core/mr_yuv_m68k.S core/mr_scale.c core/mr_dither.c core/mr_dither_m68k.S

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
run "$BUILD/mr_yuv_dither_check.m68k"
run "$BUILD/mr_yuv_ham_check.m68k"
run "$BUILD/mr_mpeg1_blockset_check.m68k"
run "$BUILD/mr_mpeg1_idct_check.m68k"
run "$BUILD/mr_media_clock_check.m68k"

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
echo "[MPEG-1, real m68k/big-endian - exercises the new motion-comp asm]"
run "$BUILD/mr_decode.m68k" tests/assets/test_mpeg1.mpg \
    --check tests/assets/ref_mpeg1

echo "m68k/big-endian check: OK"
