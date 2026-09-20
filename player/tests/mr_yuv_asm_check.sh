#!/bin/sh
# Focused big-endian/68k YUV420 RGB24 and BGR24 correctness test.
# Run from any working directory. Requires m68k-linux-gnu-gcc and qemu-m68k.
# The existing mr_yuv_check.c covers odd dimensions, padded destination
# strides, exact clipping, and a callback which clobbers caller-saved regs.
set -eu
cd "$(dirname "$0")/.."
CC=${M68K_CC:-m68k-linux-gnu-gcc}
QEMU=${QEMU_M68K:-qemu-m68k}
command -v "$CC" >/dev/null 2>&1 || { echo "Missing $CC" >&2; exit 1; }
command -v "$QEMU" >/dev/null 2>&1 || { echo "Missing $QEMU" >&2; exit 1; }
BUILD=$(mktemp -d)
trap 'rm -rf "$BUILD"' EXIT HUP INT TERM
for variant in asm c; do
    if [ "$variant" = asm ]; then flags=-DMR_M68K_ASM=1; else flags=-DMR_YUV_NO_ASM; fi
    "$CC" -O2 -std=c99 -m68030 -static -g $flags \
        -o "$BUILD/mr_yuv_$variant" \
        tests/mr_yuv_check.c core/mr_yuv.c core/mr_yuv_m68k.S
    echo "== YUV420 RGB24/BGR24 conformance: $variant =="
    "$QEMU" "$BUILD/mr_yuv_$variant"
done
