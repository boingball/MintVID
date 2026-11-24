#!/bin/sh
# Builds berryvp.library from berryvp.library.s with vasm.
# Needs vasmm68k_mot (vasm 2.0f, Motorola syntax) and python3 in PATH.
#
# If reloc_keep_order.txt exists, the relocation tables are written in the
# order of the historical build (see tools/hunk_sort_relocs.py); this only
# changes the file layout, not the loaded program.
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
OUT=${1:-$HERE/out}
mkdir -p "$OUT"
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
vasmm68k_mot -m68000 -phxass -Fhunkexe -nosym -o "$TMP/raw.lib" "$HERE/berryvp.library.s" >"$TMP/log" 2>&1 \
  || { cat "$TMP/log"; exit 1; }
if [ -f "$HERE/reloc_keep_order.txt" ]; then
  vasmm68k_mot -m68000 -phxass -Fhunkexe -o "$TMP/sym.lib" "$HERE/berryvp.library.s" >/dev/null 2>&1
  python3 "$HERE/tools/hunk_sort_relocs.py" "$TMP/raw.lib" "$OUT/berryvp.library" \
    --keep-order "$TMP/sym.lib" $(grep -v "^#" "$HERE/reloc_keep_order.txt")
else
  cp "$TMP/raw.lib" "$OUT/berryvp.library"
fi
SUM=$(sha256sum "$OUT/berryvp.library" | cut -d' ' -f1)
echo "berryvp.library: $OUT/berryvp.library"
echo "SHA-256: $SUM"
if [ -f "$HERE/EXPECTED_SHA256" ]; then
  if [ "$SUM" = "$(cat "$HERE/EXPECTED_SHA256")" ]; then echo "OK: identical to the released binary"
  else echo "NOTE: differs from EXPECTED_SHA256"; fi
fi
