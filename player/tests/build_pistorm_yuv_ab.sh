#!/bin/sh
# Produce two otherwise-identical 68040/PiStorm mrplay builds, without
# --time or other profiling flags. The normal Amiga/release default already
# is YUV_ASM=1; set it explicitly here so an A/B is unambiguous.
# Usage: sh tests/build_pistorm_yuv_ab.sh [output-directory]
set -eu
cd "$(dirname "$0")/.."
OUT=${1:-"$PWD/../mintvid-yuv-ab"}
mkdir -p "$OUT"
OUT=$(cd "$OUT" && pwd)
for variant in asm c; do
    if [ "$variant" = asm ]; then yuv_asm=1; else yuv_asm=0; fi
    make -f Makefile.amiga clean
    make -f Makefile.amiga mrplay TOOLCHAIN=gcc CPU=68040 \
        SSL=1 SSLCERTS=1 YUV_ASM="$yuv_asm"
    cp mrplay "$OUT/mrplay-040-yuv-$variant"
done
printf 'PiStorm A/B builds: %s\n' "$OUT"
printf '  mrplay-040-yuv-asm: hand-written 68k RGB24 converter\n'
printf '  mrplay-040-yuv-c:   portable C RGB24 converter\n'
printf 'Use the same 360p clip/section, screen mode and Turbo option.\n'
printf 'First compare WITHOUT --time; collect short --time samples separately.\n'
