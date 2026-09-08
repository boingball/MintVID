# CLAUDE.md — working notes for this repo

## What this is
MintVID: a codec-agnostic 68k AmigaOS video player. New code is portable C in
`player/`. RiVA 0.54's assembly was studied for design ideas (renderers,
IDCT/motion macros) but is not vendored in this repository — there is no
`src/` to extend. Read `DESIGN.md` before making structural decisions.

## Core principles
- **Portable core, thin platform layer.** `player/core/` must stay
  Amiga-independent and host-buildable (C99, fixed-width ints, big-endian-safe
  via `mr_rl*`/`mr_rb*` helpers). Amiga-specific output/audio/IO lives in
  `player/amiga/` (`display.c`/`display_aga.c`/`display_cgx.c`/`display_p96.c`,
  `audio_paula.c`, `mrplay.c`, the GUI files) - chipset-aware AGA/ECS/HAM
  mode selection and multiple c2p backends (this project's own portable
  8x8-transpose by default, RiVA's hand-tuned variant, and an opt-in
  Kalms c2p for one fixed 8-plane/256-colour AGA layout) are already
  implemented there, see `display_backend.h`/`display_aga.c`.
- **Codecs plug in behind `mr_codec.h`.** Add a decoder, register it in
  `mr_codec.c` — never special-case a codec in the player skeleton.
- **Audio is MintAMP.** Do not add an in-tree audio codec; the audio backend
  will call MintAMP/libhelix. Audio is the master clock for A/V sync.

## Validate against ffmpeg — always
There is no AmigaOS toolchain on the dev host, so correctness is proven by
decoding on the host and diffing against ffmpeg frame-by-frame:
```sh
cd player && make check      # Cinepak vs ffmpeg, expect worst MAE < ~0.2/255
```
When adding a decoder, add an equivalent `make check` path with an
ffmpeg-generated fixture (`player/tests/gen_assets.sh`). ffmpeg is the oracle.
ffmpeg and the git submodules (`libavc`, `MintAMP`) are installed by
`.claude/hooks/session-start.sh` on Claude Code web sessions; elsewhere run
`apt-get install ffmpeg` and `git submodule update --init --recursive`
yourself first.

There is still no m68k-amigaos-gcc (hunk format, clib2/newlib, dos.h/exec.h)
on the dev host, so `mrplay.c` and anything else under `player/amiga/` can
only be reviewed, not compiled, here — that needs a real Amiga/WinUAE/Pistorm
pass. But `player/core/` and the libavc/libavc_port pieces it links against
*are* portable C with no AmigaOS dependency, and for those there is a real
m68k target available: `gcc-m68k-linux-gnu` + `qemu-user` (Linux/m68k, ELF,
glibc — not AmigaOS, but real big-endian m68k codegen and execution).
```sh
cd player && make check-m68k   # same conformance suite, run on real m68k/big-endian
```
This exists because the host build alone (x86-64, little-endian, relaxed
alignment) cannot catch an endianness bug in a demuxer or an alignment bug in
`vendor/libavc_port/ih264_m68k_optim.c`'s packed-word tricks — `make check`
passing on the host is not proof those are safe on the actual target. See
`player/tests/run_m68k_check.sh`.

## Cinepak notes (hard-won)
Chunk-id flag bits live in the **high** byte: `0x0100`=selective/inter,
`0x0200`=V1-codebook / V4-only-vectors, `0x0400`=grayscale. Codebooks and the
output framebuffer **persist across frames** (inter frames patch in place and
may selectively update codebooks). Getting these bit positions wrong shows up as
error that *accumulates* between keyframes, not as an immediate failure.

## Build / test commands
- `cd player && make` — build host harness `mr_decode`
- `cd player && make check` — full conformance suite (Cinepak, H.264, MPEG-4
  Part 2, MSMPEG4v2, MPEG-1/2, MJPEG, ...) vs ffmpeg, on the host CPU
- `cd player && make check-audio` — MP3/AAC/LATM/AC-3 decode checks, including
  `mr_ac3_check`, which diffs decoded AC-3 against ffmpeg's own PCM (the rest
  of that suite only counts non-silent samples, which is why two AC-3 defects
  survived in it for so long)
- `cd player && make check-m68k` — the same conformance suite cross-built for
  m68k-linux-gnu and run under qemu-m68k (real big-endian execution; see
  above)
- `./mr_decode <avi>` / `--ppm <dir>` / `--check <refdir>`

## Git
Work happens on branch `claude/amiga-video-player-riva-9pz78q`. Commit with
clear messages; do not open a PR unless asked.
