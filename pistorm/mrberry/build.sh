#!/bin/sh
# Builds mrberry (the BerryVP/PiStorm playback backend that MintVID starts).
# mrberry uses the MintVID core from ../../player/core plus the changed and
# new files in ./core (overlay), so the upstream mrplay is not affected.
# Needs m68k-amigaos-gcc (bebbo toolchain, tested with 16.2) and make in PATH,
# and the MintAMP submodule (git submodule update --init --recursive).
#
# --paula plays the sound through the Amiga with MintVID's own decoders, so
# MintAMP, liba52 and pl_mpeg are built in as well. The source lists and
# flags come from player/Makefile.amiga (print-audio-build), the same ones
# mrplay is built with.
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
PLAYER=$(cd "$HERE/../../player" && pwd)
OUT=${1:-$HERE/out}
mkdir -p "$OUT"
OUT=$(cd "$OUT" && pwd)
: "${CC:=m68k-amigaos-gcc}"
: "${DEBUG_LOGS:=1}"
CPU=68040

rm -rf "$OUT/src" "$OUT/obj"
mkdir -p "$OUT/src/core" "$OUT/src/amiga" "$OUT/src/iptv" "$OUT/src/audio" \
         "$OUT/obj"
cp "$PLAYER"/core/* "$OUT/src/core/"
cp "$HERE"/core/* "$OUT/src/core/"
cp "$HERE"/amiga/* "$OUT/src/amiga/"
# MintVID's player status port (shared with mrplay and the GUIs).
cp "$PLAYER/amiga/mr_player_status.h" "$OUT/src/amiga/"
cp "$PLAYER/iptv/mr_iptv.h" "$OUT/src/iptv/"
# MintVID's audio: packet decoders and the Paula backend.
cp "$PLAYER"/audio/* "$OUT/src/audio/"
cp "$PLAYER/amiga/audio_paula.c" "$PLAYER/amiga/mr_audio.h" \
   "$PLAYER/amiga/mr_audio_rate.h" "$OUT/src/amiga/"
cp -r "$PLAYER/amiga/include" "$OUT/src/amiga/include"
cp "$HERE/net_stubs.c" "$OUT/src/"

AUDIO_VARS=$(make -s -C "$PLAYER" -f Makefile.amiga print-audio-build \
             CPU=$CPU AMIGA_GCC="$CC")
MINTAMP_SRC=$(printf '%s\n' "$AUDIO_VARS" | sed -n 's/^MINTAMP_SRC=//p')
MINTAMP_FLAGS=$(printf '%s\n' "$AUDIO_VARS" | sed -n 's/^MINTAMP_FLAGS=//p')
if [ -z "$MINTAMP_SRC" ] || [ ! -f "$PLAYER/vendor/MintAMP/pub/mp3dec.h" ]; then
  echo "mrberry: MintAMP sources missing (git submodule update --init --recursive)" >&2
  exit 1
fi
# The same flags with paths that work from the build directory.
MINTAMP_FLAGS_ABS=$(printf '%s\n' "$MINTAMP_FLAGS" |
  sed "s#-I\([^/]\)#-I$PLAYER/\1#g; s#-include #-include $PLAYER/#g")

# MintAMP/liba52 at MintAMP's own validated settings, as Makefile.amiga does.
AUDIO_OBJS=
for src in $MINTAMP_SRC; do
  obj="$OUT/obj/$(printf '%s' "$src" | sed 's#[/.]#_#g').o"
  (cd "$PLAYER" && $CC -m$CPU -std=gnu89 -O3 -fomit-frame-pointer -noixemul \
     $MINTAMP_FLAGS -c "$src" -o "$obj")
  AUDIO_OBJS="$AUDIO_OBJS $obj"
done

cd "$OUT/src"
$CC -O2 -std=c99 -m$CPU -noixemul -fomit-frame-pointer -Wall -Wextra \
  -DAMIGA_M68K -DMR_YUV_NO_ASM -DMR_DEBUG_LOGS=${DEBUG_LOGS} \
  -DMR_PL_MPEG_SKIP_AUDIO_TIME=1 -Iamiga/include $MINTAMP_FLAGS_ABS \
  -Wno-unused-parameter -Wno-unused-variable -Wno-unused-function \
  -o "$OUT/mrberry" \
  amiga/mrberry.c amiga/mr_berryvp.c amiga/mr_berryvp_stubs.S \
  amiga/mrb_paula.c amiga/mrb_pl_mpeg.c amiga/mrb_sub.c amiga/audio_paula.c \
  audio/mr_audio_decode.c audio/mr_pcm.c \
  core/mr_avc_annexb.c \
  core/mr_demux.c core/mr_mkv.c core/mr_mov.c core/mr_ts.c core/mr_ps.c \
  core/mr_avi.c core/mr_raw_mjpeg.c core/mr_raw_mpeg4.c \
  core/mr_source.c core/mr_latm.c core/mr_mpeg4.c core/mr_rawvideo.c core/mr_yuv.c \
  net_stubs.c $AUDIO_OBJS \
  -noixemul
echo "mrberry: $OUT/mrberry"
