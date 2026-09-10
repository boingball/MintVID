# CLAUDE.md — working notes for this repo

## What this is
MintVID: a codec-agnostic 68k AmigaOS video player. New code is portable C in
`player/`. RiVA 0.54's assembly was mostly studied for design ideas (renderers,
IDCT/motion macros) rather than vendored — there is no `src/` to extend. The
one exception is `core/mr_c2p_riva_native_m68k.S`, which **adapts** RiVA's
`GrayC2P` register-scheduled 32-pixel kernel from `RendererAGAC2P.i` under the
MIT licence, carrying its original copyright alongside the adaptation notice.
Anything else derived from RiVA must do the same: reproduce the licence, keep
the original copyright, and say in the header what was changed. Read
`DESIGN.md` before making structural decisions.

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

**qemu-m68k proves correctness, not speed — and it scores cache work
backwards.** qemu-user models instruction execution, not the 68060's caches, so
it is a fair proxy only for changes that alter *how much work* is done. Any
change trading memory footprint for arithmetic will read the wrong way round.
The compact 8-bit dither LUT is the worked example: replacing three 16x256
tables (~12 KB, far over the 060's 8 KB data cache) with one 4 KB quantiser
plus a couple of weight multiplies measures **0.44 -> 0.53 ms/frame (20%
*slower*) under qemu**, because qemu sees only the added multiplies — and
**25.10 -> 23.09 ms/frame (8% *faster*) on a real 68060/50**, because the
working set now fits. Taking the qemu number would have rejected a real win.
So: use qemu for bit-exactness and for instruction-count questions, and settle
anything memory-bound on hardware.

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

## MPEG-1/2 (libmpeg2) notes
**Portable C beat the hand-written m68k assembly it replaced.** Measured on a
134x100 clip, 125 frames, real m68k codegen under qemu: pl_mpeg *with*
`mr_mpeg1_idct_m68k.S` and `mr_mpeg1_blockset_m68k.S` active cost 1.22 ms/frame
to decode; libmpeg2, which has no 68k acceleration at all, costs 0.51 ms/frame
— **2.4x faster**. Reach for asm here only with a measurement in hand, and note
those two .S files are now dead on the video path (still linked by
`Makefile.amiga`, never called, since pl_mpeg only decodes MP2 audio now).

**MP2 (Layer II) audio, unlike the video path above, does have a measured
asm win — and its 040/060 split is not optional.** `plm_audio_idct36_m68k.S`
and `plm_audio_synth_window_m68k.S` are 040-class kernels: both use the
extended `muls.l <ea>,Dh:Dl` (32x32->64) form, hardware on 68020/030/040 but
an "unimplemented integer instruction" trap on the 68060 (which, like its
missing 64-bit DIVS.L/DIVU.L — see the scale/clamp note below — only
implements the 32x32->32 forms in silicon). Both are gated
`!defined(__mc68060__)` for exactly that reason and 68060 keeps running the
portable C `plm_audio_idct36`/`plm_audio_synth_window`. That C path was
never actually trapping, but it wasn't free either: compiling
`(int64_t)value * coefficient` with `m68k-linux-gnu-gcc -mcpu=68060` lowers
it to `jsr __muldi3` — GCC already knows about the missing hardware
widen and reaches for libgcc's generic 64x64->64 routine (full sign-extend
of both operands, then a call). `plm_audio_smul64_060()` in `pl_mpeg.h`
replaces just that one call with an inline-asm 32x32->64 widen built from
`mulu.w` (16x16->32, hardware everywhere including the 060) via the
four-partial-product schoolbook multiply, wired into `plm_audio_mul_q15`
and `plm_audio_synth_window` behind `defined(__mc68060__)` — no change to
either function's C control flow, no new dispatch, and the 68040 kernels
above are untouched. Verified bit-exact against the C `*` operator by
`tests/mr_mp2_mul64_060_check.c`, with `tests/mr_mp2_idct_060_check.c` and
`tests/mr_mp2_synth_060_check.c` re-running the existing whole-transform
oracles through the 68060 path end to end — all three `-mcpu=68060` builds
in `make check-m68k`. `plm_audio_scale_clamp_m68k.S`'s division (PCM
output scaling by the fixed `-66562`) sidesteps the same missing-hardware
problem from the other direction: it never lets the compiler emit a 64-bit
divide at all, computing the quotient via a reciprocal-multiply constant
built entirely from `mulu.w`, so it needs no CPU split and runs unchanged
on 68040 and 68060 alike.

**`plm_audio_decode()`'s own internal time bookkeeping was a second, bigger
tax on the same audio path - and it was computing a value nobody reads.**
Its `self->time = (int64_t)samples_decoded * PLM_TIME_SCALE /
PLM_AUDIO_SAMPLE_RATE[self->samplerate_index]` ran on *every* decoded MP2
frame (~38/sec at 44.1kHz), and unlike the two multiplies above, GCC can't
fold this one at all - confirmed with `m68k-linux-gnu-gcc -S`, both
`-mcpu=68040` and `-mcpu=68060` lower it to `jsr __muldi3` + `jsr __divdi3`
(only the multiply drops to a single hardware `muls.l` on 68040; the divide
is `__divdi3` everywhere, since no m68k tier has a 64-bit-quotient divide
instruction at all - and, corrected below, that holds even for a
compile-time-constant divisor, so `self->samplerate_index` being
runtime-only was never the reason). Two
software library calls per frame - and `mr_mpeg1.c` never calls
`plm_audio_get_time()` or reads a decoded sample's `.time`: MintVID gets
audio PTS from the container's own PES timestamps (see the PTS note below),
not from pl_mpeg's internal clock, so the whole computation is dead weight
on this target. `plm_audio_decode()` skips it under `MR_M68K_ASM` instead of
speeding it up; host builds keep pl_mpeg's original behaviour byte for byte,
since nothing there reads it either and the cost is one native
divide/multiply, not worth diverging from upstream over.

## Amiga Paula audio clock notes
**`amiga/audio_paula.c`'s audio master clock paid the same libgcc tax as the
MP2 decode math above - and it's hotter.** `audio_elapsed_us()` (called at
least once per displayed video frame while audio is playing - see mrplay.c -
and never at all for video-only playback) computed `completed_samples *
1000000ULL / output_rate`, where `completed_samples` is a running total for
the whole session (needs real 64-bit range) and `output_rate` is only known
at runtime. `audio_now_us()`, `request_duration_us()` and
`request_estimated_played()` had the identical shape underneath it. Checked
with `m68k-linux-gnu-gcc -S`: **this is not a 68060-only problem** - GCC
never emits a hardware wide-divide instruction for 64-bit C division syntax
on *any* m68k tier, 68020 included, always calling libgcc's
`__udivdi3`/`__umoddi3` (the multiply half drops to a single hardware
`muls.l` on 68020-68040, same as the MP2 case, but is `__muldi3` on 68060).
So this was two libgcc calls, several times a frame, for the entire time
audio was on - a much hotter site than pl_mpeg's own decode math, and the
likely explanation for "smooth video-only, jerky with audio" reports on real
68060 hardware even after the MP2-side fixes above landed.

`core/mr_muldiv64.h` fixes this properly instead of chasing it call site by
call site: `mr_u64_mul_u32()` is the same trap-free `mulu.w`-based
64x32->64 widen as `plm_audio_smul64_060` (unsigned here, so no sign
handling needed at all), used only on 68060 since 68020-68040 already get a
hardware `muls.l` from plain C. `mr_u64_div_u16()`/`mr_u64_div_u24()` are
the actual fix for the divide side, on *every* CPU tier: base-65536/base-256
schoolbook long division using nothing but the ordinary 32-bit/32-bit
hardware `DIVU.L` (ISA since the 68000, unlike the extended
64-bit-dividend form) - no magic reciprocal constants, no wide anything,
just never handing GCC a 64-bit divisor-unknown-at-compile-time expression
to lower into a libgcc call. The 16-bit-divisor variant covers every real
Paula output rate (`MIN_PERIOD` keeps it far under 65536); the 24-bit one
covers `ReadEClock()`'s tick frequency (a few hundred kHz on real hardware).
Both are pure portable C with no Amiga dependency, verified bit-exact
against the native `*`/`/` operators - including the fused
counter-times-1e6-divided-by-rate shape actually used, run out to a
simulated multi-hour session so the counter genuinely needs full 64-bit
range - by `tests/mr_muldiv64_check.c`, on host and cross-built for both
`-mcpu=68040` and `-mcpu=68060` under `make check-m68k`. `amiga/audio_paula.c`
itself can only be reviewed, not compiled, on this dev host (see "Validate
against ffmpeg" above) - the arithmetic is proven exact, but the actual
speedup on real 68060 silicon still needs a hardware pass to confirm.

This is a shared backend, not an MPEG-specific one: `audio_paula.c` is the
one Paula output path every audio codec plays through (H.264+AAC, MP3, AC-3,
LATM, MP2 alike), so this fix changes the playback clock for all of them,
not just the MP2 case that surfaced it. Give at least one non-MP2 clip
(H.264+AAC/MP3) a real-hardware sanity pass alongside the MP2 one before
relying on this.

**68060 detection is hardened against compiler-spelling drift.**
`core/mr_cpu.h` defines one macro, `MR_CPU_68060`, unioning every predefine
spelling in use (`__mc68060__`, `__mc68060`, `mc68060` - this m68k-linux-gnu-gcc
defines all three for `-mcpu=68060`, confirmed with `-dM -E`, but
`vendor/MintAMP`'s own real-hardware-tested code checks all three
defensively too, meaning a different GCC generation is not guaranteed to
agree) and every 68060-conditional site in `pl_mpeg.h`/`mr_muldiv64.h` (and
their tests) checks only that macro. The asymmetry that makes this worth
hardening: a false positive just takes the slower fallback, but a false
negative sends a real 68060 straight into the trap-prone extended
`muls.l`/`divsl.l` path this entire family of fixes exists to avoid.

**`plm_audio_smul64_060`'s bit-pattern assembly had a latent UB wart.**
`((int64_t)hi << 32) | lo` left-shifts a *signed* `int32_t` that is
frequently negative (whenever the product is negative) - technically
undefined behaviour in C, even though every compiler this has been tested
with does the intended thing. Fixed to build the pattern unsigned first:
`(int64_t)(((uint64_t)(uint32_t)hi << 32) | (uint32_t)lo)`.
`mr_muldiv64.h`'s own widen (`mr_u32_mul_u32_wide`) never had this problem -
its halves are `uint32_t` throughout, so the shift was always well-defined.

**The audio-time skip's "nobody reads this" claim needed a correction, not
just a rewording.** `mr_mpeg1_audio()` calls `plm_decode_audio(plm_t*)` -
not `plm_audio_decode(plm_audio_t*)`, a level down - and that function
*does* read the very `samples->time` this optimization stops updating,
copying it into the `plm_t`'s own `.time` field. The claim only becomes true
one hop further: that field's only other readers in `pl_mpeg.h` are
`plm_get_time()`, `plm_decode()`'s own scheduling loop, and
`plm_seek()`/`plm_seek_frame()`, none of which `mr_mpeg1.c` ever calls (it
drives `mr_mpeg1_next()`/`mr_mpeg1_audio()` directly). The skip is now its
own flag, `MR_PL_MPEG_SKIP_AUDIO_TIME` (Makefile.amiga sets it alongside
`MR_M68K_ASM`, decoupled on purpose so it's independently testable), and
`tests/mr_mpeg1_audio_time_check.c` builds `mr_mpeg1.c` both ways and diffs
every video frame's pts/pixels and every decoded PCM byte between them -
proving the skip is invisible through the one interface this player
actually uses, rather than resting on a chain of "and nothing reads *that*
either" reasoning.

**GCC never folds *any* 64-bit division into a reciprocal multiply on m68k
- constant divisor or not.** A correction to every claim above that pinned
the cost on the divisor being "only known at runtime": tested directly with
`m68k-linux-gnu-gcc -S`, `x / 90000ULL` and `x / CLOCKS_PER_SEC` (both
compile-time constants) lower to `jsr __divdi3` exactly like a runtime
divisor does, at every CPU tier including 68020. m68k's backend appears to
simply lack the divide-by-constant DImode optimization other targets have -
this is not a 68060 quirk, or even an "unknown divisor" quirk, it is
apparently a blanket fact about 64-bit division on this target. That widens
where this class of fix applies: anywhere doing 64-bit arithmetic with a
64-bit divisor, constant included, on any m68k build.

**`mrplay.c` had its own, separately-introduced copy of the exact same
`ticks * 1000000ULL / frequency` bug in `monotonic_us()` - and this one is
the player's main clock, not a Paula-specific helper.** Same shape as
`audio_paula.c`'s `audio_now_us()` (fixed above) - ReadEClock ticks times a
constant divided by ReadEClock's own runtime tick frequency - but
`monotonic_us()` has 56 call sites in `mrplay.c`, most of them unconditional
(real scheduling/pacing decisions: the deadline-drop check, pause/resume,
live-resync, sleep pacing - not just `--time` diagnostics), driving both
video and audio scheduling. A real-hardware `--time` trace is what surfaced
this: `hw-starvations` climbing, the Paula FIFO staying at 0ms buffered for
the whole run, and `adecode` readings up to 118ms for one MP2 frame that
should cost low single-digit milliseconds - all symptoms of the scheduler
loop's own per-iteration overhead crowding out the time available to keep
Paula fed, not of MP2 decode itself being slow. Fixed with the same
`mr_u64_mul_u32`/`mr_u64_div_u24` pair from `core/mr_muldiv64.h`. Three more
sites in the same file recompute a video frame's period
(`(scale/rate)*1e6`, `vi->rate`/`vi->scale` fixed per stream but only known
at runtime) from scratch on *every main-loop iteration* - not gated by
`--time`, not audio-specific, so this one also taxes video-only playback,
just proportionally less since audio mode iterates that loop far more often
(servicing the audio FIFO), which is the likely reason "video alone is
smooth, video+audio is jerky" survived every audio-decode-side fix above:
the dominant cost was in the scheduler's own clock, paid every iteration
regardless of what's being scheduled. Hoisted into one `video_frame_period_us()`
helper, fixed the same way; a fourth site (`synthetic_pts`, used when a
decoded frame arrives without its own container PTS) needed the multiply
done before the single final divide to match the original expression's
truncation order exactly, not a separately-rounded period_us multiplied by
the frame index, which would round twice instead of once. `mrplay.c` cannot
be compiled on this dev host (see "Validate against ffmpeg" above) - this
needs a real-hardware pass to confirm, but the bug and the fix are both the
same well-verified shape as the already-proven `audio_paula.c` case.

**The MPEG-PS/TS PTS-to-microseconds conversion (`ticks * 1000000ULL /
90000ULL`) had the identical divide-by-constant cost, in portable code this
time.** `core/mr_ps.c` (twice) and `core/mr_ts.c` (once) convert a 33-bit
90kHz PES timestamp to microseconds on every packet that carries one - a
compile-time-constant divisor, but per the correction above that's no
protection on m68k. Fixed with `mr_u64_div_u24`/`mr_u64_mul_u32` (a PTS
tick value is always well under `mr_u64_mul_u32`'s 64-bit-dividend room and
90000 comfortably fits the 2^24 divisor bound) - verified against the exact
existing PTS pinning tests (`tests/mr_ps_pts_check.c`, `tests/mr_ps_check.c`,
`tests/mr_ts_mp2_check.c`), which still match ffprobe's timestamps exactly.

**The RGB24 round-trip is the expensive part, not the decode.** The adapter's
`emit_rgb()` was about a third of its own decode time, and the display's
RGB→indexed dither cost about as much again — so an AGA session paid for RGB24
on the way out and a second full pass to get back down to palette indices.
`mr_mpeg2_set_yuv_output()` (mirroring `mr_h264_set_yuv_output()`) hands
libmpeg2's planes over packed instead, feeding the player's existing
`queue_copy_yuv_indexed()`. The copy itself cannot be skipped — libmpeg2 cycles
its framebuffers between reference and display use — but it moves 1.5 bytes per
pixel with no arithmetic instead of writing 3 with a colour transform.

**MPEG-PS timestamps come from the PES header, and only for the *first*
picture that starts in each PES packet.** `mr_ps` used to walk straight past
those fields without setting `mr_packet::has_pts`, so every MPEG-PS clip
reached the player untimed: `mr_mpeg2_set_input_pts()` was always told "no
timestamp", `mr_mpeg2_output_pts()` always answered 0, and A/V sync fell back
to the synthetic display-order clock with no audio anchor at all. Nothing
failed — it just drifted. Note the shape of the real data before "fixing" a
gap in it: where one PES starts two pictures the second genuinely has no
timestamp in the container, so the stamps step by two frame periods there.
ffprobe interpolates those, so its `packet=pts_time` list is longer than the
set of stamps that actually exist (25 vs 22 on `test_mpeg1_mp2.mpg`); every
stamp we do emit must match it exactly. `tests/mr_ps_pts_check.c` pins that
against the fixtures and `tests/mr_ps_check.c` pins the header-walking rules
(both PES forms, and the one-tag-per-PES rule) on synthetic streams.

Two things about that handoff are silent when wrong, so `tests/mr_mpeg2_yuv_check.c`
pins both: rows must be read at libmpeg2's **macroblock-aligned pitch**
(`seq->width`, 144 for a 134-wide picture), not the visible width, and
`MR_PIX_YUV420P` is Y, **Cb, Cr** — `u` is `planes[1]`, `v` is `planes[2]`. A
swap only shifts the colour; a visible-width stride only skews the picture. The
check requires the two paths to be **bit-identical** (both run the same
converter over the same pixels), so any slip shows instantly — the stride bug
scores MAE 50, the chroma swap 99. It refuses to run on a clip whose width is a
multiple of 16, where the aligned pitch equals the visible width and no stride
bug can show; `test_mpeg1_odd.mpg` exists for that reason.

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
