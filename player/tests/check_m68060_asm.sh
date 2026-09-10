#!/bin/sh
# Disassembly-level CI gate for the 68060 MP2 hot path (see CLAUDE.md's
# "MPEG-1/2 (libmpeg2) notes"): builds the real 68060 objects and inspects
# their emitted machine code with m68k-linux-gnu-objdump/nm, not their C/asm
# source - a hand kernel or a compiler-generated fallback can look correct
# in source and still emit the extended-result MULS.L/MULU.L, an extended
# 64-bit-dividend divide, or a __muldi3/__divdi3/__udivdi3 libgcc call, any
# of which either traps on a real 68060 or defeats the point of avoiding the
# trap in the first place. See tests/scan_m68060_forbidden.py for exactly
# what "forbidden" means at the instruction-encoding level and why (the
# 32-bit-dividend DIVS.L/DIVU.L form is not forbidden - same mnemonic and
# operand count as the trapping 64-bit form, distinguished only by whether
# the remainder/quotient registers differ).
#
# Two things are checked:
#   1. The two standalone kernels (core/plm_audio_idct36_m68k_060.S,
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

BUILD=/tmp/mr_m68060_asm_check_build
mkdir -p "$BUILD"

echo "== building the two standalone 68060 kernels =="
$M68K_CC -mcpu=68060 -c -o "$BUILD/idct36_060.o" core/plm_audio_idct36_m68k_060.S
$M68K_CC -mcpu=68060 -c -o "$BUILD/synth_060.o" core/plm_audio_synth_window_m68k_060.S

echo "== building core/mr_mpeg1.c with the real 68060 production flags =="
$M68K_CC -O2 -std=c99 -mcpu=68060 -static -DMR_M68K_ASM=1 \
    -DMR_PL_MPEG_SKIP_AUDIO_TIME=1 -c -o "$BUILD/mr_mpeg1_060.o" core/mr_mpeg1.c

echo "== scanning the two standalone kernels (whole object) =="
python3 tests/scan_m68060_forbidden.py "$BUILD/idct36_060.o" "$BUILD/synth_060.o"

echo "== scanning the MP2 hot path inside mr_mpeg1.c (scoped functions only) =="
python3 tests/scan_m68060_forbidden.py --symbols \
    plm_audio_decode_frame,plm_decode_audio,plm_audio_decode,mr_mpeg1_audio \
    "$BUILD/mr_mpeg1_060.o"

echo "68060 disassembly check: OK"
