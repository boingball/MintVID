#!/bin/sh
# Exact m68k instruction profile of one qemu-m68k run (see qemu_tbprof.c).
#   tools/qemu_tbprof.sh ./mr_decode.m68k clip.mp4 --h264-speed=turbo > prof.txt
# Slow (the trace streams every executed block through a fifo): a few
# minutes for 120 frames of 640x360 H.264. Keep the run short.
set -eu
here=$(cd "$(dirname "$0")" && pwd)
bin=$1; shift
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT HUP INT TERM
cc -O2 -w -o "$tmp/tbprof" "$here/qemu_tbprof.c"
${M68K_NM:-m68k-linux-gnu-nm} -n "$bin" > "$tmp/nm"
mkfifo "$tmp/log"
"$tmp/tbprof" "$tmp/log" "${TBPROF_TOP:-20}" "$tmp/nm" &
${QEMU_M68K:-qemu-m68k} -d in_asm,exec,nochain -D "$tmp/log" "$bin" "$@" \
    > /dev/null 2>&1 || true
wait
