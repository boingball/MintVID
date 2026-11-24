#!/usr/bin/env bash
# Builds berry_faad03.module (FAAD2 2.11.2 AAC-LC decoder for BerryCore CPU1).
# Needs clang, ld.lld, llvm-objcopy, llvm-nm, llvm-readelf (LLVM 17/18),
# python3, tar and patch.
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
OUT=${1:-$HERE/out}
CLANG=${CLANG:-clang}; LD=${LD:-ld.lld}; OBJCOPY=${OBJCOPY:-llvm-objcopy}
NM=${NM:-llvm-nm}; READELF=${READELF:-llvm-readelf}
rm -rf "$OUT"; mkdir -p "$OUT/obj"
tar xzf "$HERE/faad2_2.11.2.orig.tar.gz" -C "$OUT"
PSRC=$(ls -d "$OUT"/faad2-*/ | head -1); PSRC=${PSRC%/}
patch -s -d "$PSRC" -p1 < "$HERE/patches/faad2-2.11.2-berry-raw-module.patch"
# -fomit-frame-pointer: clang 17 (historical build) omitted it by default.
CFLAGS=( -target aarch64_be-none-elf -mcpu=cortex-a53 -O3 -ffreestanding -fno-builtin
  -fno-stack-protector -fno-unwind-tables -fno-asynchronous-unwind-tables
  -fno-zero-initialized-in-bss -fno-pic -fno-pie -fomit-frame-pointer -DNDEBUG
  -DPACKAGE_VERSION=\"2.11.2\" -DFAAD2_VERSION=\"2.11.2\" -DHAVE_STDINT_H=1
  -DHAVE_STRING_H=1 -DHAVE_STRINGS_H=0 -DHAVE_SYS_TYPES_H=0 -DHAVE_SYS_STAT_H=0
  -DHAVE_UNISTD_H=0 -DFIXED_POINT -DLC_ONLY_DECODER -DDISABLE_SBR
  -DBERRY_FAAD_RAW_MODULE=1 -I"$HERE/runtime/include" -I"$HERE/runtime" -I"$HERE"
  -I"$PSRC/include" -I"$PSRC/libfaad" -I"$PSRC/libfaad/codebook" )
SOURCES=(bits cfft common decoder drc error filtbank hcr huffman ic_predict is
  lt_predict mdct mp4 ms output pns pulse rvlc specrec syntax tns)
$CLANG "${CFLAGS[@]}" -c "$HERE/berry_faad03_bridge.c" -o "$OUT/obj/bridge.o"
$CLANG "${CFLAGS[@]}" -c "$HERE/runtime/berry_runtime.c" -o "$OUT/obj/runtime.o"
$CLANG -target aarch64_be-none-elf -mcpu=cortex-a53 -c "$HERE/berry_faad03_entry.S" -o "$OUT/obj/entry.o"
for s in "${SOURCES[@]}"; do
  $CLANG "${CFLAGS[@]}" -c "$PSRC/libfaad/$s.c" -o "$OUT/obj/$s.o"
done
$LD -T "$HERE/berry_faad03.ld" -nostdlib -e berry_faad_image "$OUT"/obj/*.o -o "$OUT/berry_faad03.elf"
$OBJCOPY -O binary "$OUT/berry_faad03.elf" "$OUT/berry_faad03.module"
if [ -n "$($NM -u "$OUT/berry_faad03.elf")" ]; then
  $NM -u "$OUT/berry_faad03.elf" >&2; echo "Unresolved symbols remain." >&2; exit 3
fi
# Exactly the 79 known FAAD2 pointer-table entries (ABS64) must be present;
# the module relocates them itself at INIT.
COUNT=0
for f in "$OUT"/obj/*.o; do
  n=$($READELF -rW "$f" | grep -c 'R_AARCH64_ABS64' || true); COUNT=$((COUNT+n))
done
[ "$COUNT" -eq 79 ] || { echo "Unexpected ABS64 relocation count: $COUNT (expected 79)" >&2; exit 4; }
python3 "$HERE/../../tools/fit_module.py" "$OUT/berry_faad03.module" 0x22109
