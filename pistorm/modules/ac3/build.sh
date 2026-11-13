#!/bin/sh
# Builds berry_ac3.module (liba52 AC3 decoder for BerryCore CPU1).
# Needs clang, ld.lld, llvm-objcopy (LLVM 17/18) and python3.
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
OUT=${1:-$HERE/out}
CLANG=${CLANG:-clang}; LD_LLD=${LD_LLD:-ld.lld}; OBJCOPY=${OBJCOPY:-llvm-objcopy}
mkdir -p "$OUT/obj"; cd "$HERE"
# -fomit-frame-pointer: clang 17 (historical build) omitted it by default.
CFLAGS="--target=aarch64_be-none-elf -c -O2 -mgeneral-regs-only -ffixed-x18 -ffreestanding \
 -fno-builtin -fno-stack-protector -fno-pic -fomit-frame-pointer -fno-unwind-tables \
 -fno-asynchronous-unwind-tables -Iinclude -Iliba52"
$CLANG $CFLAGS berry_ac3_entry.S -o "$OUT/obj/berry_ac3_entry.o"
$CLANG $CFLAGS berry_ac3.c -o "$OUT/obj/berry_ac3.o"
$CLANG $CFLAGS berry_runtime.c -o "$OUT/obj/berry_runtime.o"
for f in bit_allocate bitstream downmix imdct parse; do
  $CLANG $CFLAGS liba52/$f.c -o "$OUT/obj/$f.o"
done
$LD_LLD -m aarch64elfb -T berry_ac3.ld --no-undefined -o "$OUT/berry_ac3.elf" \
  "$OUT"/obj/berry_ac3_entry.o "$OUT"/obj/berry_ac3.o "$OUT"/obj/berry_runtime.o \
  "$OUT"/obj/bit_allocate.o "$OUT"/obj/bitstream.o "$OUT"/obj/downmix.o \
  "$OUT"/obj/imdct.o "$OUT"/obj/parse.o
$OBJCOPY -O binary "$OUT/berry_ac3.elf" "$OUT/berry_ac3.module"
python3 "$HERE/../../tools/fit_module.py" "$OUT/berry_ac3.module" 0x8360
