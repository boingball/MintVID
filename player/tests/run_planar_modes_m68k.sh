#!/bin/sh
# Execute the new assembly and its C fallback against the existing colour,
# resize and C2P composition, on both m68k CPU tiers. No AmigaOS SDK needed.
set -eu
cd "$(dirname "$0")/.."
M68K_CC=${M68K_CC:-m68k-linux-gnu-gcc}
QEMU_M68K=${QEMU_M68K:-qemu-m68k}
build_dir=$(mktemp -d "${TMPDIR:-/tmp}/mintvid-planar-XXXXXX")
trap 'rm -rf "$build_dir"' EXIT HUP INT TERM
sources='tests/mr_planar_modes_check.c core/mr_yuv_dither.c core/mr_dither.c core/mr_fused32.c core/mr_c2p.c core/mr_scale.c core/mr_yuv_planar_queue.c core/mr_play_options.c'
# Use the original indexed worker here, without the separate eight-plane
# queue dispatcher: this test exercises five-plane assembly and C fallback.
assembly='core/mr_yuv_dither_m68k.S core/mr_yuv_fused32_m68k.S core/mr_dither_m68k.S core/mr_c2p_m68k.S'
for cpu in 68030 68060; do
    echo "Planar modes: compile and execute CPU=$cpu"
    "$M68K_CC" -O2 -std=c99 -static -m"$cpu" -DMR_M68K_ASM=1 \
        $sources $assembly -o "$build_dir/check-$cpu"
    "$QEMU_M68K" "$build_dir/check-$cpu"
    "$M68K_CC" -O2 -m"$cpu" -DMR_M68K_ASM=1 \
        -c core/mr_yuv_fused32_m68k.S -o "$build_dir/fused32-$cpu.o"
    python3 tests/scan_m68060_forbidden.py "$build_dir/fused32-$cpu.o"
done
