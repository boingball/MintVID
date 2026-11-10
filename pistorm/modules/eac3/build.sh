#!/bin/sh
# Builds berry_eac3.module (BerryCore CPU1 E-AC3 decoder, MintVID P32 layout)
# completely from source:
#   1. FFmpeg 9.0.2 + patches  -> core_eac302b.o (16 FFmpeg units, ld -r)
#   2. Berry glue + core       -> base module (<= 0x1A000 bytes)
#   3. FASTIMDCT03 fragment    -> appended at 0x1A000, branch at 0x70
# Result: berry_eac3.module, exactly 0x1C3CC bytes (size checked by
# berryvp.library P32).
# Needs: clang + ld.lld + llvm-objcopy (LLVM 17 or 18), python3, tar, patch.
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
OUT=${1:-$HERE/out}
CLANG=${CLANG:-clang}
LD=${LD:-ld.lld}
OBJCOPY=${OBJCOPY:-llvm-objcopy}
rm -rf "$OUT"; mkdir -p "$OUT/obj"
cd "$OUT"

# ---- 1. FFmpeg core -------------------------------------------------------
tar xf "$HERE/ffmpeg-9.0.2.tar.xz"
FF="$OUT/ffmpeg-9.0.2"
for p in "$HERE"/patches/*.patch; do patch -s -p1 -d "$FF" < "$p"; done
cp "$HERE/ffconfig/config.h" "$HERE/ffconfig/config_components.h" "$FF/"
cp "$HERE/ffconfig/libavutil/avconfig.h" "$FF/libavutil/"

# -fomit-frame-pointer: the historical build used clang 17, which omitted
# the frame pointer here by default; clang 18 needs it spelled out.
CFLAGS="--target=aarch64_be-none-elf -Oz -mcpu=cortex-a53 -ffreestanding \
 -fno-builtin -fno-stack-protector -fno-pic -ffunction-sections -fdata-sections \
 -ffixed-x18 -fno-zero-initialized-in-bss -fomit-frame-pointer"
FFFLAGS="$CFLAGS -DBERRY_EAC3_STANDALONE -DHAVE_AV_CONFIG_H -I$HERE/fakeinclude -I$FF"

# Order = order of the FILE records in the historical core_eac302b.o
UNITS="libavcodec/ac3dec_fixed.c libavcodec/ac3.c libavcodec/ac3_parser.c
 libavcodec/ac3dec_data.c libavcodec/ac3dsp.c libavcodec/ac3tab.c
 libavcodec/ac3_channel_layout_tab.c libavcodec/bswapdsp.c
 libavcodec/eac3_data.c libavcodec/mathtables.c libavutil/fixed_dsp.c
 libavutil/crc.c libavutil/tx.c libavutil/tx_int32.c libavcodec/log2_tab.c
 libavcodec/reverse.c"
OBJS=""
for u in $UNITS; do
  o="obj/$(basename "$u" .c).o"
  # compile with relative names: __FILE__ in av_assert texts = "libavutil/tx.c"
  (cd "$FF" && $CLANG $FFFLAGS -fmacro-prefix-map="$FF/"= -Wno-macro-redefined -c "$u" -o "$OUT/$o")
  OBJS="$OBJS $o"
done
$LD -m aarch64elfb -r $OBJS -o core_eac302b.o

# ---- 2. base module -------------------------------------------------------
G="$HERE/glue"
$CLANG $CFLAGS -mgeneral-regs-only -c "$G/berry_eac3_compat.c" -o obj/berry_eac3_compat.o
$CLANG --target=aarch64_be-none-elf -mcpu=cortex-a53 -ffreestanding -fno-pic -ffixed-x18 \
  -c "$G/berry_eac3_compat_entry.S" -o obj/berry_eac3_compat_entry.o
$CLANG $CFLAGS -c "$G/berry_tx_direct.c" -o obj/berry_tx_direct.o
$CLANG $FFFLAGS -fmacro-prefix-map="$FF/"= -Wno-macro-redefined -c "$G/berry_eac3_decode.c" -o obj/berry_eac3_decode.o
$CLANG $FFFLAGS -fmacro-prefix-map="$FF/"= -Wno-macro-redefined -c "$G/berry_ff_runtime.c" -o obj/berry_ff_runtime.o
$LD -m aarch64elfb -T "$G/berry_eac3.ld" --gc-sections --no-undefined \
  --wrap=av_tx_init --wrap=cos \
  obj/berry_eac3_compat_entry.o obj/berry_eac3_compat.o obj/berry_eac3_decode.o \
  obj/berry_ff_runtime.o obj/berry_tx_direct.o core_eac302b.o -o base_berry_eac3.elf
$OBJCOPY -O binary base_berry_eac3.elf base_berry_eac3.module

# ---- 3. FASTIMDCT03 -------------------------------------------------------
F="$HERE/fast"
# -falign-loops=1 keeps the clang 18 code inside the 0x3CC-byte P32 slot.
$CLANG --target=aarch64_be-none-elf -O2 -mcpu=cortex-a53 -mgeneral-regs-only \
  -ffreestanding -fno-builtin -fno-stack-protector -fno-pic \
  -ffunction-sections -fdata-sections -ffixed-x18 -fomit-frame-pointer -falign-loops=1 \
  -I"$F" -c "$F/fast_mdct.c" -o obj/fast_mdct.o
$LD -m aarch64elfb -T "$F/fast.ld" --gc-sections --no-undefined obj/fast_mdct.o -o fast_mdct.elf
$OBJCOPY -O binary fast_mdct.elf fast_mdct.bin

python3 - base_berry_eac3.elf base_berry_eac3.module fast_mdct.bin berry_eac3.module <<'PY'
import subprocess, sys
elf, base, frag, out = sys.argv[1:]
base = open(base, 'rb').read(); frag = open(frag, 'rb').read()
nm = subprocess.run(['llvm-nm', elf], capture_output=True, text=True, check=True).stdout
hook = [int(l.split()[0], 16) for l in nm.splitlines()
        if l.endswith(' __berry_ff_tx_mdct_naive_inv_int32_c')]
assert hook == [0x70], 'naive IMDCT is not at module offset 0x70: %r' % hook
assert len(base) <= 0x1A000, 'base module too large: 0x%X > 0x1A000' % len(base)
assert len(frag) == 0x23CC, 'fast_mdct fragment has wrong size: 0x%X' % len(frag)
target, pc = 0x1A000, 0x70
insn = 0x14000000 | (((target - pc) // 4) & 0x03ffffff)   # B 0x1A000
m = bytearray(base)
m[pc:pc+4] = insn.to_bytes(4, 'little')    # AArch64 BE8: instructions are little-endian
m += b'\0' * (target - len(m)) + frag
assert len(m) == 0x1C3CC
open(out, 'wb').write(m)
print('berry_eac3.module: 0x%X bytes (base 0x%X bytes)' % (len(m), len(base)))
PY
