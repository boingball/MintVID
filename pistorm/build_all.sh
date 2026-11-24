#!/bin/sh
# Builds all PiStorm/BerryCore runtime files of MintVID-PiStorm v97.1 into
# pistorm/out/v97.1 and compares them with the released binaries
# (pistorm/v97.1_SHA256SUMS).
#
# Tools (in PATH):
#   m68k-amigaos-gcc (bebbo, 16.2)      MintVID, mrberry
#   vasmm68k_mot (vasm 2.0f)            berryvp.library
#   clang, ld.lld, llvm-objcopy, llvm-nm, llvm-readelf (LLVM 17/18)
#                                       CPU1 modules, Emu68 BerryCore kernel
#   python3, tar, patch, make
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
OUT="$HERE/out"; DIST="$OUT/v97.1"
rm -rf "$OUT"; mkdir -p "$DIST"

echo "== MintVID-pi (GUI)"
make -C "$HERE/../player" -f Makefile.amiga \
     AMIGA_GCC="$(command -v m68k-amigaos-gcc)" MintVID >/dev/null
mv "$HERE/../player/MintVID" "$DIST/MintVID-pi"

echo "== mrberry"
sh "$HERE/mrberry/build.sh" "$OUT/mrberry" >/dev/null 2>&1
cp "$OUT/mrberry/mrberry" "$DIST/"

echo "== berryvp.library"
sh "$HERE/berryvp_library/build.sh" "$OUT/library" >/dev/null
cp "$OUT/library/berryvp.library" "$DIST/"

echo "== berry_ac3.module"
sh "$HERE/modules/ac3/build.sh" "$OUT/ac3" >/dev/null
cp "$OUT/ac3/berry_ac3.module" "$DIST/"

echo "== berry_faad03.module"
bash "$HERE/modules/aac_faad03/build.sh" "$OUT/faad03" >/dev/null 2>&1
cp "$OUT/faad03/berry_faad03.module" "$DIST/"

echo "== berry_eac3.module"
sh "$HERE/modules/eac3/build.sh" "$OUT/eac3" >/dev/null 2>&1
cp "$OUT/eac3/berry_eac3.module" "$DIST/"

echo "== Emu68-pistorm-BerryCore"
python3 "$HERE/emu68_berrycore/build_berrycore.py" "$DIST/Emu68-pistorm-BerryCore" >/dev/null

echo "== Emu68-pistorm32lite-BerryCore"
python3 "$HERE/emu68_berrycore/build_berrycore32.py" "$HERE/emu68_berrycore/upstream32/Emu68-pistorm32lite" \
        "$DIST/Emu68-pistorm32lite-BerryCore" >/dev/null

echo
echo "Result in $DIST:"
cd "$DIST"
while read -r sum name; do
  [ -f "$name" ] || { echo "  MISSING     $name"; continue; }
  now=$(sha256sum "$name" | cut -d' ' -f1)
  if [ "$now" = "$sum" ]; then echo "  IDENTICAL   $name"
  else echo "  REBUILT     $name ($(wc -c < "$name") bytes, compiler differs from the release)"; fi
done < "$HERE/v97.1_SHA256SUMS"
