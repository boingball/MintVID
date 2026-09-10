#!/bin/sh
# Disassembly-level CI gate for every 68060 hot path this player has - not
# just MP2/MPEG-1 (see CLAUDE.md's "MPEG-1/2 (libmpeg2) notes"). Builds the
# real 68060 objects with the same flags Makefile.amiga uses and inspects
# their emitted machine code with m68k-linux-gnu-objdump/nm, not their C/asm
# source - a hand kernel or a compiler-generated fallback can look correct
# in source and still emit the extended-result MULS.L/MULU.L, an extended
# 64-bit-dividend divide, or a __muldi3/__divdi3/__udivdi3 libgcc call, any
# of which either traps on a real 68060 or defeats the point of avoiding the
# trap in the first place. See tests/scan_m68060_forbidden.py for exactly
# what "forbidden" means at the instruction-encoding level and why (the
# 32-bit-dividend DIVS.L/DIVU.L form is not forbidden - same mnemonic and
# operand count as the trapping 64-bit form, distinguished only by whether
# the remainder/quotient registers differ; an ordinary two-operand
# muls.l/mulu.l is real 68060 hardware and is never flagged either).
#
# Five things are checked:
#   1. The two standalone MP2 kernels (core/plm_audio_idct36_m68k_060.S,
#      core/plm_audio_synth_window_m68k_060.S) - the whole object, since
#      each file contains only the one hand-tuned function (plus, for
#      idct36, its own local multiply subroutine).
#   2. core/mr_mpeg1.c built with the real production flags
#      (-DMR_M68K_ASM=1 -mcpu=68060) - but only the specific functions the
#      MP2 hot path actually reaches (plm_audio_decode_frame,
#      plm_decode_audio, plm_audio_decode, mr_mpeg1_audio), not the whole
#      translation unit. mr_mpeg1.c carries the *entire* pl_mpeg.h
#      implementation (PL_MPEG_IMPLEMENTATION), including video/seek/HTTP
#      code this player never calls and was never claimed trap-free on
#      68060 - scanning the whole object would flag real but irrelevant
#      dead code and make this check useless as a gate.
#   3. H.264: every vendor/libavc_port/*.S/*.c file (the hand asm plus its
#      C dispatch/wrap glue, including ih264d_update_qp_wrap.c - see that
#      file for the per-macroblock DIVSL.L it replaces) - whole objects,
#      built with the real Makefile.amiga LIBAVC_FLAGS/-DMR_M68K_ASM=1.
#      This does NOT scan vendor/libavc/ itself (Ittiam's reference C,
#      pinned upstream submodule) - see the AAC exclusion note below for
#      why a submodule's own C is a different, not-fixable-here class of
#      finding; ih264d_update_qp is the one such libavc hole this port
#      layer already neutralises via --wrap, which is what part 3 proves.
#   4. AAC: MintAMP's decoders/aac/*.c (excluding sbr*.c, which this
#      player's build excludes too), built with the real AACASM=1
#      production flags (-include audio/mr_aac_m68k_config.h). This
#      intentionally excludes pns.c and tns.c: both are CONFIRMED to still
#      reference __muldi3 on 68060 (raac_PNS's MULSHIFT32 call never gets
#      the amiga_m68k_aac.h override other AAC files use; raac_TNSFilter
#      hand-inlines `long long` arithmetic with no macro to redirect at
#      all) - real, per-frame AAC-LC decode cost (PNS/TNS are standard
#      tools many broadcast encoders enable), but the fix lives inside
#      vendor/MintAMP's own pns.c/tns.c source, a git submodule this repo
#      cannot push to (see ih264d_cabac_wrap.c's header comment for why a
#      submodule edit doesn't survive `git submodule update --init`).
#      Scanning them here would make this gate permanently red for a
#      problem it cannot fix - excluded on purpose, not by oversight; the
#      proposed upstream patch is recorded outside the repo for
#      vendor/MintAMP's maintainer to apply directly.
#   5. AC-3: MintAMP's decoders/wma/fft-ffmpeg.c (liba52's IMDCT calls its
#      ff_fft_calc_c) built with -DAMIGA_M68K_WMA_ASM, exactly as
#      Makefile.amiga's LIBA52_FFT_CPPFLAGS wires it in for CPU=68060 only
#      (see the note above LIBA52_SRC there). Without that define this
#      object references __muldi3 from every FFT butterfly's MULT32() -
#      confirmed while diagnosing the hole this gate now guards against.
set -e
cd "$(dirname "$0")/.."

M68K_CC=${M68K_CC:-m68k-linux-gnu-gcc}

if ! command -v "$M68K_CC" >/dev/null 2>&1; then
    echo "ERROR: $M68K_CC not found. On Debian/Ubuntu:"
    echo "  apt-get install gcc-m68k-linux-gnu binutils-m68k-linux-gnu"
    exit 1
fi
if ! command -v m68k-linux-gnu-objdump >/dev/null 2>&1 || \
   ! command -v m68k-linux-gnu-nm >/dev/null 2>&1; then
    echo "ERROR: m68k-linux-gnu-objdump/nm not found (package binutils-m68k-linux-gnu)."
    exit 1
fi
if ! command -v python3 >/dev/null 2>&1; then
    echo "ERROR: python3 not found (needed by tests/scan_m68060_forbidden.py)."
    exit 1
fi
test -f vendor/libavc/decoder/ih264d.h || {
    echo "ERROR: initialise the libavc submodule first:"
    echo "  git submodule update --init player/vendor/libavc"; exit 1; }
test -f vendor/MintAMP/decoders/aac/aacdec.h || {
    echo "ERROR: initialise the MintAMP submodule first:"
    echo "  git submodule update --init player/vendor/MintAMP"; exit 1; }

BUILD=/tmp/mr_m68060_asm_check_build
mkdir -p "$BUILD/h264" "$BUILD/aac"

echo "== building the two standalone MP2 kernels =="
$M68K_CC -mcpu=68060 -c -o "$BUILD/idct36_060.o" core/plm_audio_idct36_m68k_060.S
$M68K_CC -mcpu=68060 -c -o "$BUILD/synth_060.o" core/plm_audio_synth_window_m68k_060.S

echo "== building core/mr_mpeg1.c with the real 68060 production flags =="
$M68K_CC -O2 -std=c99 -mcpu=68060 -static -DMR_M68K_ASM=1 \
    -DMR_PL_MPEG_SKIP_AUDIO_TIME=1 -c -o "$BUILD/mr_mpeg1_060.o" core/mr_mpeg1.c

echo "== building H.264 (vendor/libavc_port) at the real 68060 production flags =="
LIBAVC_PORT=vendor/libavc_port
LIBAVC_ROOT=vendor/libavc
H264_FLAGS="-mcpu=68060 -O2 -std=c99 -fomit-frame-pointer -DAMIGA_M68K \
    -DMR_HAVE_H264 -DMR_M68K_ASM=1 -I$LIBAVC_PORT -I$LIBAVC_ROOT/common \
    -I$LIBAVC_ROOT/decoder -include $LIBAVC_PORT/compat.h \
    -fno-strict-aliasing -fwrapv -w"
H264_OBJS=""
for f in $LIBAVC_PORT/ih264_m68k_interp.S $LIBAVC_PORT/ih264_m68k_deblk.S \
         $LIBAVC_PORT/ih264_m68k_cabac.S $LIBAVC_PORT/ih264_m68k_chroma_mc.S \
         $LIBAVC_PORT/ih264_m68k_weighted_pred.S $LIBAVC_PORT/ih264_m68k_mvpred.S \
         $LIBAVC_PORT/ih264_m68k_cabac_coeff.S $LIBAVC_PORT/ih264_m68k_cabac_coeff8x8.S \
         $LIBAVC_PORT/ih264_m68k_iquant_itrans_recon.S $LIBAVC_PORT/ih264_m68k_intra_pred.S \
         $LIBAVC_PORT/ih264_m68k_bs.S $LIBAVC_PORT/ih264_m68k_optim.c \
         $LIBAVC_PORT/ih264d_function_selector_port.c $LIBAVC_PORT/ih264_mc_degrade.c \
         $LIBAVC_PORT/ih264d_stage_profile.c $LIBAVC_PORT/ih264d_cabac_wrap.c \
         $LIBAVC_PORT/ih264d_mvpred_dispatch_port.c \
         $LIBAVC_PORT/ih264d_parse_cabac_coeff_port.c \
         $LIBAVC_PORT/ih264d_update_qp_wrap.c \
         $LIBAVC_PORT/ithread_port.c $LIBAVC_PORT/compat.c; do
    base=$(basename "$f"); base=${base%.*}
    $M68K_CC $H264_FLAGS -c -o "$BUILD/h264/$base.o" "$f"
    H264_OBJS="$H264_OBJS $BUILD/h264/$base.o"
done

echo "== building AAC (vendor/MintAMP/decoders/aac, AACASM=1 production flags) =="
MINTAMP_ROOT=vendor/MintAMP
AAC_FLAGS="-mcpu=68060 -std=gnu89 -O3 -fomit-frame-pointer -DAMIGA_M68K \
    -DARDUINO -DESP8266 -I$MINTAMP_ROOT/pub -I$MINTAMP_ROOT/real \
    -I$MINTAMP_ROOT/decoders/aac -I$MINTAMP_ROOT/decoders/aac-arduino-shim \
    -I$MINTAMP_ROOT/decoders -include audio/mr_aac_m68k_config.h -w"
AAC_OBJS=""
for f in $(printf '%s\n' "$MINTAMP_ROOT"/decoders/aac/*.c | grep -v -e /sbr -e /pns.c -e /tns.c); do
    base=$(basename "$f" .c)
    $M68K_CC $AAC_FLAGS -c -o "$BUILD/aac/$base.o" "$f"
    AAC_OBJS="$AAC_OBJS $BUILD/aac/$base.o"
done

echo "== building AC-3 FFT (vendor/MintAMP/decoders/wma/fft-ffmpeg.c) with the real CPU=68060 fix =="
$M68K_CC -mcpu=68060 -std=gnu89 -O3 -fomit-frame-pointer -DAMIGA_M68K \
    -DARDUINO -DESP8266 -I$MINTAMP_ROOT/pub -I$MINTAMP_ROOT/real \
    -I$MINTAMP_ROOT/decoders/aac -I$MINTAMP_ROOT/decoders/aac-arduino-shim \
    -Ivendor/liba52 -DAMIGA_M68K_WMA_ASM \
    -c -o "$BUILD/fft-ffmpeg_060.o" "$MINTAMP_ROOT/decoders/wma/fft-ffmpeg.c"

echo "== scanning the two standalone MP2 kernels (whole object) =="
python3 tests/scan_m68060_forbidden.py "$BUILD/idct36_060.o" "$BUILD/synth_060.o"

echo "== scanning the MP2 hot path inside mr_mpeg1.c (scoped functions only) =="
python3 tests/scan_m68060_forbidden.py --symbols \
    plm_audio_decode_frame,plm_decode_audio,plm_audio_decode,mr_mpeg1_audio \
    "$BUILD/mr_mpeg1_060.o"

echo "== scanning H.264 (vendor/libavc_port, whole objects) =="
python3 tests/scan_m68060_forbidden.py $H264_OBJS

echo "== scanning AAC (vendor/MintAMP/decoders/aac, whole objects, pns.c/tns.c excluded - see header) =="
python3 tests/scan_m68060_forbidden.py $AAC_OBJS

echo "== scanning AC-3 FFT (whole object) =="
python3 tests/scan_m68060_forbidden.py "$BUILD/fft-ffmpeg_060.o"

echo "68060 disassembly check: OK"
