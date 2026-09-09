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
  `mr_codec.c` — never special-case a codec in the player skeleton. Codec tags
  are matched **case-insensitively** (`mr_codec_find`), so a decoder lists each
  tag once; muxers stamp them in whatever case they like and AVI's `fccHandler`
  is the worst offender. Non-letter bytes are untouched, so numeric `BI_*`
  tags (BI_RLE8 is `1`) still compare exactly.
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
Strip and chunk headers are **1 byte of id + a 24-bit big-endian size**, not two
16-bit fields — reading them as 16/16 only appears to work while everything
stays under 64KB, then silently truncates. Those sizes are **byte-exact**:
rounding a strip up to an even boundary shifts every strip below it the first
time an encoder emits an odd-sized one, which turns the lower part of the frame
to noise (`ffmpeg -c:v cinepak` does this routinely above 128x96).

Chunk-id flag bits, on that one-byte id: `0x01`=selective/inter,
`0x02`=V1-codebook / **V1-only**-vectors, `0x04`=grayscale. Note the asymmetry:
`0x32` vectors are V1-only (one index byte per MB, no type flags), *not*
V4-only. Codebooks and the output framebuffer **persist across frames** (inter
frames patch in place and may selectively update codebooks), and a strip also
inherits the *previous strip's* codebooks within a frame when the frame header's
flag bit 0 is clear. Getting any of this wrong shows up as error that
*accumulates* between keyframes, not as an immediate failure.

Single-strip clips hide most of it, so `make check` decodes a 320x240 clip
(`test_cinepak_strips.avi`) as well as the 128x96 one.

## Microsoft Video 1 (MSVC/CRAM) notes
The format is **bottom-up in both dimensions**: the bottom row of 4x4 blocks is
coded first, and within a block the bottom pixel row comes first. The per-pixel
selector bit is **inverted** — a set bit picks the lower-numbered colour. The
8-colour block is signalled differently per depth: header byte `b >= 0x90` in
8-bit mode, but bit 15 of the *first colour word* in 16-bit mode. Decoding an
8-colour block as a flat one leaves its extra colour words in the stream, so
everything after it is garbage — and because the frame builds bottom-up that
reads as a correct strip along the bottom under a screen of noise.

ffmpeg's msvideo1 **encoder only emits the 16-bit variant**, so the 8-bit
paletted fixture is generated by `tests/make_msvideo1_pal8.py` and then checked
against ffmpeg's decoder like everything else.

## MPEG-1 program stream (play_mpeg1) notes
`play_mpeg1()` in `amiga/mrplay.c` is the old pl_mpeg path and, unlike the
generic streaming player, has **no queue of decoded frames**: it decodes one
frame, paces it against the Paula clock, shows it, repeats. Two consequences
bite, and both did on an A1200/AGA:

- **A frame drop buys nothing but the display cost.** The generic scheduler
  only drops while `*qcount > 1` — it skips a frame to reach a *newer* one it
  already has. Here there is no newer frame, so an uncapped "drop when more
  than one period late" rule never clears once the machine falls behind: the
  audio clock stays ahead of video pts, every frame after the first is dropped,
  and the picture freezes on frame 1 for the whole clip. Cap the run
  (`MPEG1_MAX_DROP_RUN`).
- **Audio top-up must be measured in milliseconds, never in MP2 frames.** An
  MP2 frame is always 1152 samples, but that is 26 ms of a 44.1 kHz stream
  decimated to 22.05 kHz for Paula and **52 ms of a stream already at
  22.05 kHz**, which is not decimated at all. A fixed two-frames-per-video-frame
  top-up therefore queues 104 ms per 40 ms of 25 fps video — 2.6x real time —
  loading the whole track into the FIFO early (overrunning the 4 s ring past
  ~6 s of clip) and leaving the rest of the file with nothing queued, which
  Paula plays as its last buffer repeating. On a machine slower than real time
  the same fixed count *under*-feeds. A cushion in ms self-corrects both ways.

The policy lives in `core/mr_mpeg1_sched.c` precisely so it is host-testable —
`tests/mr_mpeg1_sched_check.c` replays the real loop around it with a modelled
Paula device at two machine speeds, and asserts the pre-fix policy still fails,
so the test cannot quietly stop proving anything.

## Microsoft RLE (BI_RLE8) notes
Running out of data is a **normal end of frame**, not an error: encoders often
omit the end-of-bitmap escape, and AVI's zero-length chunk (an unchanged frame)
reaches the decoder as an empty packet. ffmpeg warns and keeps the frame in
both cases; returning `MR_EFORMAT` instead aborts playback of a perfectly good
file. Only out-of-range runs and truncated copies are real errors.

An AVI carrying the numeric `BI_RLE8` in `biCompression` puts the codec tag in
`fccHandler` as lower-case `'mrle'`, which is how these files reached the
registry — see the case-insensitive matching note above. `make check` decodes a
66x50 clip (`test_msrle.avi`); the width deliberately is not a multiple of four.

## Build / test commands
- `cd player && make` — build host harness `mr_decode`
- `cd player && make check` — full conformance suite (Cinepak, H.264, MPEG-4
  Part 2, MSMPEG4v2, MPEG-1/2, MJPEG, ...) vs ffmpeg, on the host CPU
- `cd player && make check-audio` — MP3/MP2/AAC/LATM/AC-3 decode checks,
  including `mr_ac3_check` and `mr_mp2_check`, which diff decoded AC-3 and
  decoded .mpg MP2 against ffmpeg's own PCM (the rest of that suite only counts
  non-silent samples, which is why two AC-3 defects survived in it for so long,
  and why MPEG-2 Layer II audio was silently unsupported)
- `cd player && make check-m68k` — the same conformance suite cross-built for
  m68k-linux-gnu and run under qemu-m68k (real big-endian execution; see
  above)
- `./mr_decode <avi>` / `--ppm <dir>` / `--check <refdir>`

## Git
Work happens on branch `claude/amiga-video-player-riva-9pz78q`. Commit with
clear messages; do not open a PR unless asked.
