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

**`Makefile.amiga` was compiling MintAMP/liba52 at the wrong optimization
level.** MintAMP's own validated release recipe (`vendor/MintAMP/Makefile.amiga`,
`FAST030_CFLAGS`) builds its decoders with `-std=gnu89 -O3
-fomit-frame-pointer` — that's the actual, hardware-tested tuning behind e.g.
`ASM60_GROUPS="lowrate060 huffman midside planars8"` for CPU=60. `mrplay`
was instead compiling those same `.c` files straight into its own link line
under MintVID's `-O2 -std=c99`, so the polyphase/Huffman/IMDCT decode loops
these asm groups were tuned against never actually ran through the compiler
settings they were validated with. Fixed by giving MintAMP/liba52 sources
their own object files (`build/vendor/...`, via a `build/%.o: %.c`/`%.S`
pattern rule and `MINTAMP_CFLAGS := $(CPUFLAGS) -std=gnu89 -O3
-fomit-frame-pointer -noixemul $(MINTAMP_FLAGS) $(LIBA52_FLAGS)`) instead of
listing them as raw sources on `mrplay`'s/`mrplay-fused`'s compile-and-link
line — MintVID's own code is untouched and still builds at `-O2 -std=c99`.
Only `mrplay`/`mrplay-fused` link MintAMP/liba52 at all; the GUI targets
(`MintVID`, `iptvgui`, `ytgui`, their `-GT` variants) don't reference them.
Verified via `make -f Makefile.amiga -n mrplay CPU=68060
AMIGA_GCC=/fake/m68k-amigaos-gcc` dry-run output (real cross-compilation
needs a real Amiga toolchain, unavailable on this dev host); actual playback
impact — this was the leading suspect for a "Morse code"-pattern audio
stutter reported on real 68060 hardware that survived every 64-bit
multiply/divide trap fix above — still needs a real-hardware pass to
confirm.

**MintAMP's own MP3 asm/fast-path acceleration was compiled in on every
Amiga build and never once switched on.** `Makefile.amiga` already compiles
MintAMP's Huffman asm substitution and its trap-free polyphase kernel into
every default build (`FULL030` for CPU=68030/68040; `lowrate060`, in the
default `ASM60_GROUPS`, for CPU=68060 - it pulls in
`AMIGA_M68K_POLYPHASE_68060` specifically) - but both are gated behind a
runtime switch, `MP3SetExperimentalHuffman()`/`MP3SetExperimentalPolyphase()`,
that nothing in `player/` ever called. `MP3InitDecoder()` doesn't default
them on either; they're plain process-global statics initialised to 0. So
every MP3 decode, on every CPU tier, ran MintAMP's fully portable reference
path - the same shape of gap as the `-O2`/`-O3` finding above, but this one
means the accelerated code was never *reached* at all, not just compiled at
the wrong optimization level. Confirmed via `real/huffman.c`'s and
`real/polyphase_68060.h`'s own header comments (arithmetic substitutions
documented as producing bit-identical output to their reference
counterparts) and, since MintAMP's decoder is portable C with no AmigaOS
dependency, verified directly: a standalone probe linking `mp3dec.c`/
`mp3tabs.c`/`real/*.c` was cross-built for real m68k (`m68k-linux-gnu-gcc
-static`, both `-mcpu=68030` with the FULL030 flags and `-mcpu=68060` with
`AMIGA_M68K_POLYPHASE_68060`) and run under `qemu-m68k` against all three MP3
test fixtures, decoding each once with both setters off and once with both
on - **bit-identical PCM on every file, both CPU tiers** (maxdiff=0). Wired
into `audio/mr_audio_decode.c`'s `mp3_enable_verified_fast_paths()`, called
after every `MP3InitDecoder()` (there are two call sites - open and
`mr_audio_decoder_reset()` - and the flags are never reset by
`MP3InitDecoder()` itself, so one call per init is enough but harmless to
repeat). No effect on host builds, where `AMIGA_FAST_POLYPHASE`/
`AMIGA_M68K_ASM_HUFFMAN` aren't defined and both setters are no-op stubs -
`make check-audio` is unaffected byte for byte.

Two related MintAMP knobs were deliberately **not** wired in here:
`MP3SetExperimentalReducedTaps()`/`MP3SetExperimentalFDCT32Quarter()` and
`MP3SetFastLowrate()`/`MP3SetSuperfastLowrate()`/`MP3SetSubbandCap()`. Unlike
the two above, MintAMP's own CLI (`amiga_mp3dec.c`) documents these as lossy,
and - per its own runtime warnings - inert unless MintAMP's internal
`fastLowrate` decimation is also active (stride 2/3/4, tied to a *reduced
output rate*). That's a different mechanism from MintVID's own post-decode
decimation (`compute_stride()`/`stride` in `mr_audio_decode.c`, used for
`--audio-rate=low`): MintAMP's fastLowrate skips polyphase output samples
*inside* the decoder, changing how many PCM samples `MP3Decode()` itself
produces per frame, where MintVID's stride discards already-decoded samples
afterward. Wiring the two together to let `--audio-rate=low` skip the
decode-side work it currently throws away would be a real further win, but
needs its own careful stride/rate bookkeeping and its own bit-exactness
pass (`mr_audio_rate_check` pins the current 2x low-rate relationship
exactly) - not something to bundle into a same-breath default change.

**Correction: the two MintAMP MP3 findings above do not apply to MPEG-1/2
audio at all.** `mr_mpeg1_audio()` decodes through pl_mpeg's own integer
Layer II implementation (`plm_decode_audio()` -> `plm_audio_decode()` in
`pl_mpeg.h`), never through MintAMP's `mp3dec.c`. `MP3SetExperimentalHuffman`/
`Polyphase`/`FastLowrate`/`SuperfastLowrate`/`SubbandCap` are MintAMP-only
symbols with no reach into pl_mpeg's decoder state - useful and bit-exact for
actual MP3 playback (see above), but irrelevant to why an MPEG-1 `.mpg`/`.ts`
clip's audio might be slow. pl_mpeg needed its own equivalent, from scratch.

**pl_mpeg now has a genuine Fast MP2 decode mode - `plm_audio_set_decim()` -
and it replaces a decode-then-discard waste that both MPEG-1 integration
points had.** Before this, `mr_mpeg1_open()` computed `decim` (2 above 28kHz,
doubled again under `--audio-rate=low`) but `mr_mpeg1_audio()` called
`plm_decode_audio()` first and only kept every `decim`-th sample afterward -
`--audio-rate=low` saved Paula output bandwidth but essentially no MP2 decode
CPU, since `plm_audio_decode_frame()` still computed and threw away most of
what it synthesised.

The fix follows directly from how `plm_audio_synth_window()`'s windowed sum
is structured: `u[i]` (output lane `i` of 32) reads `d`/`v` only at a fixed
"+i" offset across 16 historical slices - never mixing with any other lane -
so once a decimation factor keeps the same subset of lanes in every
sub-block (true here because `out_pos` always advances by a multiple of 32,
so `j % decim` never shifts phase from one sub-block to the next), the
discarded lanes' tap-sums can be skipped outright rather than computed and
thrown away. The 32-point IDCT feeding it (`plm_audio_idct36`) **cannot** be
similarly pruned and still runs in full every sub-block regardless of decim:
its fast butterfly network shares intermediate values across all 32 outputs
right up to the final write-out stage, and more fundamentally its full
32-lane output is written into the `V` ring buffer that *every* future
sub-block's synthesis reads from, kept lanes included - skipping any of it
would corrupt future output, not just the discarded samples of this frame.
`plm_audio_synth_window_decim()` is the new, portable-C-only function that
computes just the surviving lanes, compacted in order so the caller's output
loop stays a plain contiguous copy; it is *not* used at decim=1, where the
original code path - existing 68040 asm kernels (`plm_audio_synth_window_m68k`
etc.) and `MR_CPU_68060`'s `plm_audio_smul64_060` widen included - runs
completely unchanged, byte for byte.

`MP3SetFastLowrate`-style **input-side** reduction (capping/zeroing high
subbands before synthesis, saving the dequantisation work while still
consuming the same bit count from the stream, so bit-position sync isn't
lost) was considered too, per the same design question that produced the
MintAMP knobs above - but not implemented. Unlike lane-skipping in the
synthesis stage, it changes actual output values (no longer bit-exact
against ffmpeg's own MP2 decode) and needs its own MAE-style
quality characterization the way the video decoders get, not just a
bit-exactness proof; a real "SuperFast" lossy tier is a plausible follow-up
but a separate piece of work from this one. Likewise, mono mode was already
skipping the second channel's synthesis entirely (`synth_channels = self->mono
? 1 : 2` predates this change) - decim composes with it for free, verified
by `tests/mr_mpeg1_decim_check.c`'s mono cases.

Verified bit-exact two ways: `tests/mr_mpeg1_decim_synth_check.c` checks
`plm_audio_synth_window_decim()` directly against `plm_audio_synth_window()`
with random and sign-extreme synthetic V/D history across all 16 ring-buffer
phases (no real bitstream involved - the lane-independence claim itself),
and `tests/mr_mpeg1_decim_check.c` decodes a real MP2 elementary stream
(extracted from `test_mp2_stereo.ts` via `mr_demux`) once per frame at
decim=1 and once at decim=2/4, requiring every kept sample to match exactly -
stereo and mono alike. Both pass on host and, cross-built for real m68k
under qemu at `-m68030` (`MR_M68K_ASM=1`) and `-mcpu=68060`, on real
big-endian codegen too (the 68060 build specifically exercises
`plm_audio_smul64_060` from inside the new decimated path). `mr_mpeg1_open()`
now calls `plm_set_audio_decim(m->plm, m->decim)` and `mr_mpeg1_audio()` no
longer strides over the output at all - `plm_decode_audio()` already hands
back only the kept samples.

**The same waste existed a second time, independently, in
`audio/mr_audio_decode.c`'s generic MP2 path** - the one used when a
container carries an MP2 audio track alongside a *different* video codec,
not muxed MPEG-1 PS/TS. `compute_stride()` there is a separate 1/2/4
decimation-factor computation (base 2 above Paula's ~28kHz ceiling, doubled
again under `--audio-rate=low`, so always a value `plm_audio_set_decim()`
accepts), applied generically to every codec's decoded PCM by
`emit_pcm()`/`decimate()` after the fact. `plm_audio_set_decim(d->mp2,
d->stride)` is now called at both places `d->mp2` is created (open and
`mr_audio_decoder_reset()`), and `emit_pcm()` was split into
`emit_pcm_stride(..., stride, ...)` taking the stride explicitly - every
other codec (PCM/MP3/AAC/AC-3) still calls it via the unchanged `emit_pcm()`
wrapper with `d->stride`, but `feed_mp2()` now calls `emit_pcm_stride(...,
1, ...)` directly, since pl_mpeg's polyphase stage already did the
decimation and `samples->count` is already the reduced count - passing
`d->stride` there again would decimate an already-decimated stream a second
time. Verified via `make check-audio` (identical frame counts/rates for
`test_mp2_stereo.ts`'s normal/low/mono cases as before) and a real-m68k
rebuild of `mr_ac3_check.m68k` (which links this file), matching its known
baseline exactly.

**Correction: the Fast MP2 mode's first cut left two real performance holes
on 68030/040 - decim>1 fell back to portable C for both the synthesis
window and the scale/clamp division, giving up the existing hand-tuned asm
kernels entirely instead of just doing less of their work.** `scale_clamp_m68k`
(the reciprocal-multiply division that keeps PCM output off the trap-prone
64-bit `DIVS.L`/`__divdi3` path - CPU-independent, used on every m68k tier
including 68060) and `plm_audio_synth_window_m68k` (the 68040-class kernel,
excluded on 68060 same as always) both now take the reduction directly as a
parameter instead of assuming a fixed 32 lanes:

- `scale_clamp_m68k(u, dst, channel_stride, count)` - `count` is the number
  of leading `u[]` lanes to process (32 at decim=1, 32/decim otherwise); the
  loop bound is computed from it instead of a hardcoded `lea 256(%a2),%a4`.
- `plm_audio_synth_window_m68k(d, v, v_pos, u, decim)` - `decim` steps the
  lane index (`add.l %a5,%d7`, decim held in the one register this function
  didn't already use) instead of always incrementing by one; since decim
  always divides 32 evenly, the existing `cmpi.l #32,%d7` / `bne` loop exit
  needed no change at all.

At decim=1 both are provably identical to the pre-existing behavior (a
register holding 1/32 instead of an immediate produces the same instruction
effect), so `plm_audio_decode_frame()` no longer needs two separate code
paths: it always calls `plm_audio_synth_window_m68k(..., self->decim)` and
`scale_clamp_m68k(..., lanes)` on `MR_M68K_ASM && !MR_CPU_68060` builds
(portable `plm_audio_synth_window_decim()` still backs the 68060/host case,
unchanged), for every decim value including 1 - one call shape, not an
`if (self->decim == 1)` branch to keep in sync. This also means 68030/040
decim=2/4 now does *less of the same asm work* rather than switching to a
slower portable path doing *more* work than a fallback comparison would
need to justify - not a memory-footprint trade-off in the qemu-vs-hardware
sense this file warns about elsewhere, just fewer iterations of an unchanged
per-lane cost, so no separate hardware benchmark is needed to trust the
direction of the win (unlike the dither-LUT case above, this is a straight
"same work, done less" reduction, not one that trades footprint for
arithmetic).

Verified bit-exact on real m68k: `tests/mr_mp2_synth_check.c` and
`tests/mr_mp2_scale_check.c` (both asm-only, `run_m68k_check.sh`) now cover
decim/count of 1/32, 2/16 and 4/8 against the same reference oracles as
before, and `tests/mr_mpeg1_decim_check.c` was rebuilt and re-run with
`MR_M68K_ASM=1` at `-m68030` - previously it only ever exercised the
portable-C decim path even on that build, since decim>1 always fell back to
C; it now exercises the real asm kernels end to end and still matches every
kept sample exactly.

A temporary, opt-in diagnostic (`-DMR_MPEG1_DECIM_DIAG=1`, off by default)
prints `MP2 decim=N lanes=N` from both integration points
(`mr_mpeg1_open()` in `core/mr_mpeg1.c` and the MP2 branch of
`mr_audio_decoder_open()` in `audio/mr_audio_decode.c`) so a real-hardware
test can confirm which path is actually active before/after this fix,
without depending on `--time`. Remove once that pass is done.

**68060 now has its own dedicated MP2 hot-path kernels -
`plm_audio_idct36_m68k_060`/`plm_audio_synth_window_m68k_060` -
`core/plm_audio_idct36_m68k_060.S`/`core/plm_audio_synth_window_m68k_060.S`
- instead of falling back to the portable C path every fix above this one
still routed 68060 through.** That portable path was always correct (it's
what proved every claim in this file), but it was an interim measure, not a
kernel of its own - the 68030/040 tier has hand-tuned `.S` files and 68060
didn't.

Both are **generated, not hand-typed**: a standalone C transliteration of
`plm_audio_idct36()`/`plm_audio_synth_window_decim()` (renamed, otherwise
identical), with the widening multiply as `__attribute__((always_inline))`
GCC inline asm (the same `mulu.w` four-partial-product technique as
`plm_audio_smul64_060`), compiled at `-mcpu=68060 -O2 -S`, then stripped of
only the compiler's own bookkeeping directives (`#APP`/`#NO_APP`, `.file`,
`.type`, `.size`, `.ident`, `.note.GNU-stack`) - verified byte-for-byte
identical machine code before and after stripping. This mirrors how
`plm_audio_idct36_m68k.S` itself already reads (mechanical `.LNNNN` labels,
compiler-shaped register allocation) - freezing validated compiler output
as a kernel is the established pattern here, not a new one.

The two kernels made *different* inlining choices, and the reason why is
itself worth recording: **idct36's butterfly network calls the multiply 33
times as straight-line, statically-unrolled C (no loop) - synth_window's
tap loops call it from only two static source sites** (the loop runs up to
16 times per lane at runtime, but the compiled code for the call exists
once per site, same as any loop body). Fully inlining the ~35-instruction
widen at all 33 idct36 sites measured out to a **~35 KB function** - several
times over the 68060's 8 KB icache, and a real regression by this file's
own "settle memory-bound trade-offs on hardware, not by instruction count"
rule. So idct36 instead keeps the widen as one local (non-exported)
subroutine, `smul64_060_shared`, that all 33 sites reach through a plain
`jsr`/`rts` - shrinking the kernel to ~12.5 KB, in the same ballpark as the
existing 68040 kernel's ~10.8 KB, at the cost of 33 call/return pairs
instead of zero. synth_window's two static sites cost nothing to inline
fully (~600 bytes total), so it does - no local subroutine, matching the
"inline the multiply" default this whole family of kernels aims for
wherever the code-size trade-off doesn't argue against it. Neither
`smul64_060_shared` nor its call sites cross into C at any point - it is
pure hand/compiler-generated assembly calling assembly within the same
file, the thing this design deliberately avoids being is a kernel that
`jsr`s out to compiled C (a real, different anti-pattern: that would add
C-ABI call overhead *and* make the `.S` file depend on how pl_mpeg.h
happens to compile that C function on a given day).

Both retain full `plm_audio_set_decim()` support (`decim` = 1, 2 or 4) via
the same interface as their 68040/portable counterparts - `synth_window`'s
signature carries `decim` straight through from the C source it was
generated from, and idct36 needs no decim parameter at all (it never
changes with decim - see the "Fast MP2 decode mode" note above for why).

Verified bit-exact on real m68k under qemu, both independently of the
wider dispatch and through it: `tests/mr_mp2_idct_060_asm_check.c`/
`tests/mr_mp2_synth_060_asm_check.c` compare each new kernel directly
against the same reference oracles the 68040/portable-060 checks already
use (decim=1/2/4 for synth_window); `tests/mr_mpeg1_decim_check.c`,
already proven against a real MP2 elementary stream, was rebuilt at
`-mcpu=68060 -DMR_M68K_ASM=1` (previously that combination only ever
exercised the portable-060 fallback, since the dispatch had nowhere else to
go) and still matches every kept sample exactly, now through the real
dispatch in `plm_audio_decode_frame()`
(`#if defined(MR_M68K_ASM) && defined(MR_CPU_68060)` picks the new kernels;
plain `MR_M68K_ASM` still picks the 68040 ones; neither picks the portable
C, which remains the host/68060-fallback-proof oracle all of this is
checked against).

**A disassembly-level CI gate, not a source-level one, closes the loop
this whole family of fixes has been making the same claim about since the
first `plm_audio_smul64_060`: that a kernel avoiding the trap-prone
extended `muls.l`/`divsl.l` forms and libgcc's 64-bit
`__muldi3`/`__divdi3`/`__udivdi3` helpers is provably true of the *emitted
machine code*, not just plausible from reading the C/asm source.**
`tests/scan_m68060_forbidden.py` disassembles a real object with
`m68k-linux-gnu-objdump`/`nm` and checks the actual instruction encodings:
extended-result `MULS.L`/`MULU.L` always shows 3 operands (there is no
2-register-safe degenerate case, unlike divide, so any 3-operand
`muls*`/`mulu*` is unconditionally forbidden); `DIVS.L`/`DIVU.L` also shows
3 operands for *both* the trapping 64-bit-dividend form and the ordinary
32-bit form hardware since 68020 - objdump's own disassembly distinguishes
them by whether the remainder and quotient registers are the same register
(safe) or different registers (the operands genuinely span a 64-bit
dividend - forbidden); libgcc calls are found via relocations against
`__muldi3`/`__divdi3`/`__udivdi3`. `tests/check_m68060_asm.sh` runs it two
ways: the whole object for each standalone kernel (nothing else lives in
those files), and, for `core/mr_mpeg1.c` built with the real production
flags, only the specific functions the MP2 hot path actually reaches
(`plm_audio_decode_frame`, `plm_decode_audio`, `plm_audio_decode`,
`mr_mpeg1_audio`) via `objdump --disassemble=<symbol>` - scoped, because
`mr_mpeg1.c` carries the *entire* pl_mpeg.h implementation
(`PL_MPEG_IMPLEMENTATION`), including video/seek/HTTP code this player
never calls and has never been claimed trap-free on 68060 (`plm_seek`
alone pulls in real `__muldi3`/`__divdi3` references); scanning the whole
object would flag genuine but irrelevant dead code and make the gate
useless. Two real objdump gotchas surfaced writing this and are worth
recording so they don't get rediscovered the hard way: `--disassemble=<sym>`
must be passed *without* a separate bare `-d` - combined, `-d` silently
wins and dumps the whole object regardless of the requested symbol (this
produced a false positive - reported violations in unrelated code - before
being caught by checking that the addresses reported didn't even fall
inside the target function's known range); `-r` (relocations) *does*
combine safely with `--disassemble=<sym>` and stays scoped to it, which is
what makes the per-function libgcc-reference check possible at all. Wired
into `run_m68k_check.sh`/`make check-m68k` (and so into CI - see
`.github/workflows/build.yml`'s `conformance` job) - the two new
`_060_asm_check` tests and this gate all run on every push and PR.

Bit-exactness and instruction-safety are proven on qemu and via
disassembly; the actual speedup on real 68060 silicon - the entire reason
for doing this - still needs a hardware pass to confirm, same as every
other 68060-specific claim in this file.

**The two new kernels shipped without their AmigaOS underscore-prefixed C
symbol aliases, and every check in this file's toolchain was blind to
it.** The real Amiga cross-compile (`fused-68060` CI, `m68k-amigaos-gcc`)
failed to link `mrplay-fused`: "undefined reference to
`plm_audio_idct36_m68k_060`"/`plm_audio_synth_window_m68k_060`, even
though both `.S` files compiled cleanly and were correctly listed on the
link line. AmigaOS's a.out/hunk-based toolchain prepends an underscore to
C-visible symbol names - exactly what the *existing* kernels already
handle (`plm_audio_synth_window_m68k.S` declares both
`plm_audio_synth_window_m68k` and `_plm_audio_synth_window_m68k` labels at
the same address; `plm_audio_scale_clamp_m68k.S` does `_scale_clamp_m68k =
scale_clamp_m68k`) - but the two new files only declared the bare name.
Every `m68k-linux-gnu-gcc`/`qemu-m68k` check this whole family of fixes
relies on was structurally incapable of catching this: that toolchain
targets ELF, which uses no symbol prefix at all, so the bare name resolved
just fine there regardless of whether the underscore alias existed. This
is the sharpest illustration yet of "no AmigaOS toolchain on this dev
host" from this file's very first section - qemu/ELF proves instruction
safety and bit-exactness, but a *linking* convention that's specific to
AmigaOS's object format can only ever be proven by attempting the real
Amiga link, which only CI's Docker-hosted `m68k-amigaos-gcc` step can do
here. Fixed by adding the matching `_`-prefixed `.globl`/label to both
files, mirroring the existing kernels exactly - both files still assemble
identically under `m68k-linux-gnu-gcc` (both symbol names resolve to the
same address) and every qemu-based check from this section still passes.

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

## MPEG-TS video PES notes
**A video PES's own declared `PES_packet_length` cannot be trusted, and
`mr_ts.c` used to trust it anyway - silently truncating every frame that
exceeded it, forever, on a live stream that never stops sending more frames
to fail the same way.** The MPEG-2 Systems spec's documented convention for
video is `PES_packet_length = 0` ("unbounded, read until the next PES start
code") specifically because compressed frames routinely exceed the 16-bit
field's 65535-byte ceiling. `mr_ts_next_packet()` already handled that
`0` case correctly (accumulate until the next PUSI/PID-matching start code),
but when an encoder declared a real, non-zero length for video anyway - and
the true access unit was bigger than that - the code clamped the current TS
packet's contribution to fit exactly that declared length, emitted the PES
the instant the accumulated byte count reached it, and then *silently
dropped* every following continuation TS packet for that access unit
(`a->active` gets reset by `emit_pes()`, so a non-PUSI continuation packet
hits the `else if (!a->active) continue;` branch and never reaches
`pes_append()` at all) right up to the next real PES start code. The
decoder then sees a NAL cut off mid-payload and fails - `h264-decode-error:
packet N len=...` in `mrplay.c`, repeating packet after packet, forever,
since a live stream never stops sending more frames for this to keep
happening to. From the player's own perspective this doesn't read as a
crash or a literal infinite loop: nothing ever stops running, video simply
never advances (a black/frozen display, unresponsive to input in practice
because the scheduler is fully consumed servicing this), which needed a
hardware reset to clear on real hardware - a `RAM:MintVID.log` survived one
such reset (moved to persistent storage - see the mrplay.c/*_gadtools.c/
*_reaction.c `MRPLAY_LOG_FILE` note) and showed hundreds of consecutive
`h264-decode-error` lines with `len=65477` recurring constantly - suspiciously
close to the 16-bit field's own 65535 ceiling minus this project's 9-byte
zero-PTS PES header overhead, confirming the mechanism. Observed from two
independent, unrelated IPTV re-stream providers, so this is a real-world-common
encoder behavior, not a one-off broken stream.

Fixed by always treating a *video* PES as unbounded in
`mr_ts_next_packet()`, regardless of what `PES_packet_length` the encoder
declared - relying purely on the next PUSI (or end-of-stream drain) to
know a video access unit is complete, which is provably safe because it is
already exactly how the pre-existing `packet_len == 0` case worked. Audio
(ADTS AAC, MPEG Layer II) keeps trusting its own declared length: audio
frames are always comfortably under the 16-bit limit, so there is nothing
to fix there, and forcing unbounded reassembly for audio too would only
add unnecessary one-frame latency. `tests/mr_ts_video_pes_check.c` pins
this directly: a synthetic video PES whose PUSI packet declares a length
far shorter than the real payload, followed by a non-PUSI continuation
packet, must reassemble to the *full* combined length - stashing the fix
out of `mr_ts.c` and rebuilding reproduces the exact truncated-length
failure this test exists to catch, run as part of `make check`.

This is also why `mr_ts_mp2_check.c`'s two-PES fixture (one video PES, one
audio PES, nothing after) now returns audio first and video second: the
video PES here has no second video PES after it to trigger emission via
the next-PUSI path, so it is only ever recognized as complete by the final
end-of-stream drain - after the audio PES (the very next, and only
remaining, PES in the stream) has already emitted through the ordinary
in-loop path. Before the fix, this fixture's declared video length
happened to exactly match its real payload, so video emitted eagerly and
arrived first - correct only by coincidence of the fixture's own numbers,
not a property the fix needed to preserve.

## AGA direct-planar C2P notes
`mr_c2p_mode MR_C2P_DIRECT` (`--direct-c2p`, GUI "Direct") is a single hand
kernel (`core/mr_yuv_dither_planar_direct_m68k.S`) that dithers straight to
the final eight-plane image, one 32-pixel block at a time, for the plain
1:1 8-plane AGA case - no chunky intermediate, no separate C2P pass, 040/060
only (same tier restriction as Kalms, gated on `MR_KALMS_040`). It started
as a standalone hardware-test build (`Makefile.fused`,
`amiga/display_aga_fused.c`) that predated this project's disassembly-audit
and cross-build-verification methodology and had never actually been run
through either - `tests/mr_yuv_planar_queue_check.c` existed but was wired
into no build at all, and immediately failed on a host build once it was:
the centering behaviour it checks for only exists inside the m68k assembly
dispatcher (`core/mr_yuv_dither_planar_m68k.S`'s runtime check of
`mr_yuv_planar_queue_active`), which the portable C dither path never
consults, so "actual" and "expected" were never comparable outside a real
`MR_M68K_ASM` build. Fixed by gating the real comparison on `MR_M68K_ASM`
(skips cleanly on host, matches `tests/mr_h264_m68k_check.c`'s own
pattern) - bit-exact on real m68k at both 68030 and 68060 under qemu once
actually run. Also found and removed a genuinely dead function,
`mr_yuv_planar_queue_m68k()` (`core/mr_yuv_planar_queue.c`) - an earlier
tile-based dither+separate-C2P approach the dispatcher was retargeted away
from in favour of the current self-contained single kernel, left in place
and unreachable ever since.
`core/mr_c2p_riva_native_m68k.S` - documented above as adapted, MIT-licensed
content still worth keeping - is that superseded approach's C2P kernel and
is now unused by any live code path; it was never touched by this change,
but is worth a look before deciding whether to wire it up elsewhere or
retire it.
Once fixed and audited (a seventh `check_m68060_asm.sh` disassembly-scan
target, alongside the existing MP2/H.264/AAC/AC-3 ones - clean, the
kernel's `mulu.l`/`muls.l` instructions are the ordinary two-operand
hardware form), the experiment became a real runtime option instead of a
separate build: `Makefile.fused`/`display_aga_fused.c` are retired, their
logic folded into `display_aga.c`'s real `aga_supports_yuv_indexed()`/
`aga_show_indexed()`/`aga_close()`, and the direct-planar source files
moved into `Makefile.amiga`'s normal `CORE` list (compiled into every
build, runtime-inactive unless `MR_C2P_DIRECT` is actually selected) -
matching Kalms/RiVA's own existing "one binary, several C2P choices"
shape rather than a second, parallel binary. `.github/workflows/
fused-68060.yml` is retired too: its bit-exactness check is superseded by
the fixed `run_m68k_check.sh` integration (now covering both 68030 and
68060, where before it silently tested nothing), and its real-AmigaOS-
toolchain build is subsumed by `build.yml`'s existing `build` job, which
already compiles `Makefile.amiga`'s `all` target end to end - the direct-
planar files being unconditionally part of `CORE` now means that job
proves their real-toolchain link for free.

## AGA copper-assisted vertical doubling notes
`--copper-vdouble` (`display_set_copper_vdouble()`, `g_aga_copper_vdouble`,
GUI "Copper 2x") is a genuinely different kind of scale-up from the
software `mr_scale2x_u8()` path `--2x` normally uses: instead of the CPU
duplicating every encoded row so both an even and an odd physical scanline
hold real pixel data, it dithers/C2Ps only the even rows and lets a copper
list - built once, when the screen opens, since the bitplane addresses it
pokes never move for the life of the screen - repeat each one a second time
on the real raster. The mechanism: Agnus auto-increments BPLxPT by one row
after every *displayed* scanline regardless of copper activity, so a single
WAIT+MOVE per source row, firing only on the "repeat" (odd) physical line
and rewinding BPLxPT back to the row just shown, is enough - the following
auto-increment lands correctly on the *next* source row with no further
help. Halves the rows `aga_show()` has to encode and C2P for the scale==2
case. Only qualifies for a plain `--c2p`/`--riva-c2p`/`--cd32` geometry, no
HAM, no `--lace`: Kalms' hand-tuned kernels compute their own row
addressing from the real `BytesPerRow` with no way to take an arbitrary
output stride, and WritePixelArray8's rectangle-of-Y-coordinates API has no
stride concept to double at all.

Getting this built at all needed correcting two wrong assumptions about
the actual NDK, not just the raster-timing math. `graphics/copper.h`'s
`CINIT`/`CWAIT`/`CMOVE`/`CEND` turned out to be a Commodore RKM *example*
convention this project wrongly assumed shipped as real macros - the real
header only provides the raw `struct CopIns`/`CopList`/`UCopList`
primitives, populated directly instead (one `struct CopIns` per WAIT or
MOVE, built into a caller-allocated array, wrapped in a `CopList`/
`UCopList` pair, attached via `UCopIns`). And `MrgCop()` takes `struct
View*`, not `struct ViewPort*` - GCC caught the original `&s->scr->ViewPort`
call as an incompatible-pointer-type *warning*, not an error, so it slipped
through compile-only verification; `MakeScreen()`+`RethinkDisplay()` (called
right after, for exactly this "a screen's ViewPort changed" case) call
`MrgCop()` internally at the correct scope anyway, so the fix was simply to
delete the miscast call rather than fix its argument. Both were only caught
because the user extracted and pasted in the actual NDK headers
(`graphics/copper.h`, `hardware/custom.h`) this Bebbo toolchain ships -
there is no AmigaOS toolchain on this dev host at all, so this file
couldn't even be syntax-checked, let alone cross-built for qemu, unlike
every other Amiga-only file in this tree.

The GUI side had its own real gap: neither ReAction (`mrgui.c`) nor
GadTools (`mrgui_gadtools.c`) ever exposed a c2p choice that actually
qualifies for Copper 2x on a non-CD32 machine - only "Standard" (`--wpa`),
Kalms, CD32/Akiko and Direct were selectable, and the portable `--c2p`
backend (confusingly, the enum value for it is `MR_C2P_WPA`, while
`MR_C2P_STANDARD` is the one that maps to `--wpa`) was never added as an
option. A real-hardware test that ticked Copper 2x under Standard or Kalms
"worked" in the sense that the picture was fine - because in both cases
`copper_vdouble` silently failed its eligibility check and fell back to
plain `--2x`, with no visible sign the copper path never actually ran.
Fixed by adding "Portable" as a selectable c2p option in both GUIs, and by
replacing the separate "2x"/"Copper 2x" checkboxes with one three-way Scale
chooser (None/2x/Copper 2x) whose "Copper 2x" entry `update_mode_controls()`
snaps back to plain 2x whenever the current c2p backend or display mode
(HAM6/HAM8 also don't qualify) wouldn't actually honour it - re-checked on
every Mode/C2P/Scale change so no order of clicks can leave it stuck
selected under a combination that was silently doing nothing.

Once a qualifying combination (`--c2p`/"Portable" + `--2x` + Copper 2x) was
actually reachable, real AGA hardware confirmed the picture correct for the
whole session - but closing the player crashed with Guru 81000005 (CPU Zero
Divide), not a bad picture, a hard crash on exit, and it took two attempts
to fix. `aga_close()` used to call `FreeVPortCopLists()`+`RethinkDisplay()`
on the screen while it was still fully open and active, ahead of the normal
`WaitBlit`/`CloseWindow`/`CloseScreen` sequence; the first fix removed that
call outright, on the theory that `CloseScreen()` (called a few lines
later, exercised safely by every AGA session ever) already tears down a
screen's `UCopIns` as part of its own teardown. It did not fix the crash -
a retest hit the same Guru, meaning `CloseScreen()` itself was choking on a
screen whose `ViewPort.UCopIns` still pointed at the custom list, not only
the explicit call that was removed. The actual fix is an explicit five-step
shutdown lifecycle in `aga_close()` (and mirrored in `aga_open()`'s `fail:`
path): (1) stop blits and close the window first; (2) detach the custom
list by clearing `ViewPort.UCopIns` directly, *not* via
`FreeVPortCopLists()` (which also frees - detach and free are kept as two
separate, ordered steps); (3) rebuild/restore the display with
`RethinkDisplay()` while the window is already gone and the ViewPort is
back to plain, *before* `CloseScreen()` ever runs; (4) only then close the
screen; (5) only then free the three manually `AllocMem()`'d blocks (the
`UCopList`, `CopList` and `CopIns` array - `aga_state` gained `ucop_cl`/
`ucop_ci`/`ucop_ninst` fields to make this possible, since previously only
the `UCopList` itself was tracked and the rest was implicitly left to
`FreeVPortCopLists()`) exactly once, now that nothing else frees them.
Confirmed on a real-hardware retest: no crash on exit.

Two attempts at the same real-hardware bug, both looking equally reasonable
until actually tested, is worth remembering as its own lesson: a
"this should fix it" explanation is not the same as a retest confirming it
did, especially for chipset-internal behaviour (`CloseScreen()`'s own
handling of a lingering `UCopIns`) that no amount of source-reading on this
dev host can substitute for. Not yet exercised on real hardware at all:
ECS/OCS chipsets, and `--riva-c2p`/`--cd32` as the qualifying c2p backend
(only `--c2p`/"Portable" has been tested); `--lace` is excluded from
eligibility entirely, so untested by construction rather than merely
unconfirmed.

**EXPERIMENTAL extension: the same mechanism now also covers HAM6/HAM8,
still gated behind the one `--copper-vdouble` opt-in and still off by
default.** The indexed case above is confirmed on real AGA hardware; this
extension is not - it rests on a correctness argument about HAM's own
semantics, not yet a real-hardware run, and the two are kept explicitly
distinguishable at runtime (see the diagnostics paragraph below) rather than
folded into one "copper active" claim.

Two things had to be true for repeating a HAM row on the real raster to be
safe, and both were checked against `core/mr_ham.c`'s actual semantics
rather than assumed:

- **Vertical (row) repeat**: is HAM's hold-and-modify state independent
  from one scanline to the next, so that redisplaying an unchanged row's
  bytes on the following physical line decodes to the same pixels there?
  Yes - `mr_ham_encode()` and `mr_ham_decode()` both reset held R/G/B to
  `(0,0,0)` at the start of every row (the encoder's own comment: "held
  colour (line start = 0)"), modelling real AGA hardware's per-scanline HAM
  reset. This project's existing ffmpeg-validated HAM dither already
  depends on that being true - it is not a new assumption introduced for
  Copper doubling, just one this feature now also leans on. Given a
  per-row-independent decode, the same byte sequence on two different
  scanlines must produce the same pixels on both, which is exactly what
  `build_copper_vdouble()`'s row-rewind already does for indexed output -
  the function itself needed no change at all, since it only ever repeats
  bitplane *addresses* for `depth` planes and has no notion of what those
  planes hold.
- **Horizontal (byte) repeat**: `--2x`'s width-doubling duplicates each
  encoded byte immediately after itself (`mr_scale2x_u8_horiz()` under
  `copper_vdouble`, `mr_scale2x_u8()` otherwise - both already generic over
  indexed vs. HAM bytes, unchanged by this extension). Is applying the same
  HAM control+data byte twice in a row safe? Yes, by cases on HAM's two
  control-code families: a "select" code (00) fully determines the new RGB
  from the byte alone, independent of prior state, so it is trivially
  idempotent under repetition; a "modify" code holds two channels and sets
  the third to an *absolute* data value (never a delta relative to the
  previous pixel), so re-applying it holds the same two channels (already
  equal to what the first application set, since only the modify code's own
  channel changed) and re-sets the same channel to the same absolute value -
  producing an identical result both times.

Both arguments are pure state-machine reasoning about `mr_ham.c`'s
documented semantics, checked against the actual encoder/decoder source
rather than assumed - but HAM8 in particular changes register-level colour
generation on real Denise/Lisa hardware, not just which bytes land in which
planes, so this is unverified below the level of "the state machine says
this must be correct" until a real A1200 run confirms it, exactly as the
indexed case was before its own hardware pass documented above.

Eligibility in `aga_open()` (`display_aga.c`) is the indexed case's own
condition with the `!s->ham` exclusion relaxed to allow HAM6 unconditionally
and HAM8 only with `chipset_has_aga()` true (redundant in practice with the
existing HAM8-needs-AGA downgrade earlier in `aga_open()`, but checked again
explicitly at the point that actually grants Copper eligibility rather than
relied upon implicitly): `--2x` active, non-interlaced, fixed (non-resize)
geometry, and `kalms_kind == KALMS_NONE` with an explicit `--c2p`/
`--riva-c2p`/`--cd32` backend - Kalms is excluded for HAM the same way it
always was, since its HAM6/HAM8 kernels compute their own row addressing
from the bitmap's real `BytesPerRow` with no arbitrary output stride to
double. `aga_show()`'s and `aga_blit()`'s encode/widen/blit code needed no
change at all beyond the eligibility gate - they were already written
generically over "chunky byte value semantics" (a palette index or a HAM
control byte look identical to C2P and to the copper list alike), so the
existing indexed-only restriction was purely a cautious eligibility check in
`aga_open()`, not a structural limit anywhere else in the pipeline.

Both GUIs' `copper_ok` gating (`mrgui.c`, `mrgui_gadtools.c`) dropped its
`!ham_mode` exclusion for the same reason: `aga_open()` already downgrades
HAM8 to HAM6 on a non-AGA chipset before its own copper eligibility check
runs, so either HAM selection in the Scale chooser always resolves to
something the backend can actually honour - there is no chipset case the
GUI needs to pre-filter that `aga_open()` doesn't already handle itself.

Diagnostics: `display_aga_describe()` gained a `copper` out-parameter
reporting whether `--copper-vdouble` is not just requested but actually
engaged for the current screen (`s->copper_vdouble`, captured at
`aga_open()` time) - mirrors the "requested but doesn't qualify" printf
`aga_open()` already prints at open time. `mrplay --time`'s "AGA path:" line
now prints `copper=0/1`, tagged `(HAM, EXPERIMENTAL)` when both `copper=1`
and `ham` is non-zero, so a real-hardware trace can answer "was HAM Copper
doubling really active" rather than merely "was it asked for" - the same
gap the GUI-exposure fix earlier in this file's indexed-case notes exists to
close for the indexed case.

Host-testable: `tests/mr_iptv_check.c` pins that `--display ham6`/`ham8`
plus `--scale-2x --copper-vdouble` round-trip through
`mr_play_options_parse()`/`mr_build_player_arguments()`/
`mr_build_iptv_arguments()` exactly like the existing indexed
AGA-display case, that a HAM display without `--scale-2x` never emits
`--copper-vdouble` in the normal (non-explicit) argument form even with
`copper_vdouble` set (matching the pre-existing `o->scale_2x &&
o->copper_vdouble` guard in `append_playback_flags()`), and that the
existing indexed `--aga` + `--2x` + `--copper-vdouble` case is unchanged.
None of this proves the *runtime* behaviour on real Denise/Lisa hardware -
only the options-layer plumbing, which was already generic across HAM and
indexed displays before this change and needed no edits itself.

**Do not treat this as confirmed until a real A1200 pass clears it.** The
real-hardware test plan for this extension: (A) normal HAM6/HAM8 with no
Copper, as a baseline; (B) the same clip with `--2x --copper-vdouble`; (C)
static colour bars/gradients, where any per-line HAM state leakage would be
most visible as banding; (D) fast scene changes and black frames; (E)
several minutes of continuous playback; (F) ESC exit and relaunch at least
ten times, confirming no Guru `81000005` on exit (the indexed case's own
shutdown-crash fix - see above - is chipset/ViewPort-level and applies
identically regardless of HAM, but repeated-exit testing is cheap insurance
given how real that crash turned out to be); (G) smoothness and audio sync
on a 68060/50 system. Until that passes, this extension ships opt-in and
off by default, with the normal (non-copper) HAM6/HAM8/Kalms path completely
unchanged - `g_aga_copper_vdouble` still defaults to 0 (`display_set_
copper_vdouble()` is never called unless `--copper-vdouble` is passed
explicitly), and every other HAM code path (Kalms HAM6/HAM8, HAM without
`--2x`, HAM with `--2x` and no Copper) runs through exactly the same
`mr_ham_encode()`/`mr_scale2x_u8()`/`aga_blit()` calls it always did.

## H.264 CABAC notes
**A real-hardware trace and a host callgrind profile agreed that CABAC/CAVLC
parsing plus MV prediction cost roughly 56% of H.264 decode time - about
twice motion compensation - but `ih264d_stage_profile.c`'s own mc/deblock/
recon/intra buckets had no way to say which part of that 56% actually
dominated.** `ih264d_cabac_profile.h`/`.c` add three more buckets alongside
those: `bin_us`/`bin_count` (every `ih264d_decode_bin()` call - mb_type,
cbp, ref_idx, mvd, intra pred modes, mb_qp_delta), `coeff_us`/`coeff_count`
(residual coefficient parsing, 4x4 and 8x8 alike), and `mvpred_us`/
`mvpred_count` (MV *prediction* - the median-of-neighbours arithmetic, not
entropy decoding; mvd itself is CABAC-coded and already counted under
`bin_us`). These three do not overlap each other - coefficient decode and MV
prediction are both self-contained arithmetic that never call back into
`ih264d_decode_bin()` - so `bin_us+coeff_us+mvpred_us` is a real, additive
subtotal, unlike (say) `mc_us` versus `core_us`. Reported via mrplay.c's new
"h264 cabac:" line, independent of `MR_H264_STAGE_PROFILE` (opt in with
`CABAC_PROFILE=1`).

There is deliberately no fourth "macroblock parsing" bucket. The mb_type/
cbp/ref_idx/mvd/intra-mode/mb_qp_delta syntax-element dispatch that drives
all three buckets above (`dec_struct_t::pf_parse_inter_mb`, assigned to
`ih264d_parse_pmb_cabac()`/`ih264d_parse_bmb_cabac()` once per slice) cannot
be intercepted the same way: unlike bin/coeff/mvpred (each called from a
*different* file than the one defining them, so --wrap has a normal
cross-object relocation to redirect), that assignment happens in the *same*
file that defines the target function - the same shape as the mvpred
dispatch's own already-documented same-object pitfall, but for a pointer
*assignment* rather than a *call*. Confirmed empirically with a minimal
repro compiled for m68k before trusting either way: a same-file function-
pointer assignment *does* leave a relocation against the target symbol
(`objdump -r` shows `R_68K_32 target_fn`, unlike a same-file direct call,
which resolves to a branch with no relocation left for the linker to
touch) - but linking a full end-to-end repro with `--wrap=target_fn` and
checking the actual patched value showed the reference still resolves to
the original function, not `__wrap_target_fn`: the reference is satisfied
against the object's own local definition before the wrap rename takes
effect. So a wrapper installed this way would link cleanly and silently
never fire - exactly the failure mode `ih264d_mvpred_dispatch_port.c`'s
header warns about for the call case, just reached from the opposite
direction (a data reference with a relocation, not a branch without one).
Reimplementing both ~200-line per-slice-type dispatchers from scratch (the
fix that file applied for a real optimisation) is not justified just to add
a diagnostic counter. The remaining cost is still derivable, just not
directly measured: `core_us - mc_us - deblock_us - recon_us - intra_us -
bin_us - coeff_us - mvpred_us` is that combined remainder - the same
unattributed-remainder idea `ih264d_stage_profile.h` already uses, just a
much smaller and more useful one now that three of its four components are
broken out.

**The CABAC bin wrapper's own overhead is a real, measurable cost - but the
first attempt at removing it broke the one build that actually matters, and
was reverted.** `ih264d_cabac_wrap.c`'s `__wrap_ih264d_decode_bin()` - the
GNU-ld `--wrap` trampoline redirecting every one of the ~40+ vendored call
sites for the single most-executed CABAC primitive - is a plain C function
whose entire body is `return mr_ih264d_decode_bin_m68k(u4_ctx_inc,
ps_src_bin_ctxt, ps_bitstrm, ps_cab_env);`. That is a second full
call/return layer - its own prologue/epilogue, its own reload of all four
arguments from its caller's stack frame to pass down again - wrapped around
a function whose own header comment already justifies hand-asm on the
strength of "keeping every live value pinned in registers across the whole
function body". Paid on every single decoded bin (tens of thousands of
calls per frame), this is exactly the kind of per-call tax that primitive
was hand-written to avoid one layer further out.

The first fix tried: `--wrap` only needs a symbol named
`__wrap_ih264d_decode_bin` to exist somewhere in the link with the right
calling convention, so `ih264_m68k_cabac.S` exported that name directly as
a second label at the exact same address as `mr_ih264d_decode_bin_m68k` -
no C code at all. This built, linked and decoded every H.264 fixture
correctly under `m68k-linux-gnu`/qemu (`make check-m68k`, unchanged
worst-frame MAE) - **but failed the real AmigaOS link**: `m68k-amigaos-gcc`/
Bebbo's `ld` reported `undefined reference to ih264d_decode_bin` building
for real, something the qemu/ELF toolchain this project's CI relies on for
everything else in this family of fixes was structurally unable to catch
(exactly the class of gap "Validate against ffmpeg" above already warns
about, just for a *linking* behaviour rather than instruction safety or
bit-exactness this time - the same shape as the AmigaOS-underscore lesson
in the 68060 MP2 kernel notes, though the mechanism isn't identical: adding
a `_`-prefixed alias is *not* what's missing here, since `--wrap`'s own
existing cross-object usages in this file's siblings
(`ih264d_mvpred_dispatch_port.c`, `ih264d_parse_cabac_coeff_port.c`,
`ih264d_update_qp_wrap.c`) already link on real Amiga hardware with a bare,
undecorated `--wrap=` argument same as here). What's different about this
one case is that its `__wrap_...` symbol was provided by hand-written
assembly with no C function at all, instead of a compiled C trampoline -
something about that specifically does not survive Bebbo's link. Root
cause not pinned down: there is no AmigaOS toolchain on this dev host to
iterate against (see "Validate against ffmpeg"), so this needed a real
build to catch and would need a real build to keep investigating.

Reverted rather than shipped broken for the one target that matters:
`ih264_m68k_cabac.S` no longer exports `__wrap_ih264d_decode_bin` at all,
and `ih264d_cabac_wrap.c`'s C trampoline unconditionally provides the
symbol again in every `MR_M68K_ASM` build, exactly as it always did. The
`MR_H264_CABAC_PROFILE` `bin_us`/`bin_count` timing (two `clock()` calls)
is now a runtime branch *inside* that one always-present function instead
of a second file competing to provide the symbol - a build with the flag
times the call, a build without does not, but both are the same trampoline
shape, the one already proven to link on real Amiga hardware.
`ih264d_parse_cabac_coeff_port.c`'s and `ih264d_mvpred_dispatch_port.c`'s
own wrap functions still use the rename-and-thin-trampoline split for their
`MR_H264_CABAC_PROFILE` timing (unaffected by this - those symbols were
never touched, only `ih264d_decode_bin`'s was) - their
mechanically-diffed-against-vendored bodies stay untouched either way, and
they were never the ones this specific link failure hit.

Net effect: the profiling counters (`bin_us`/`coeff_us`/`mvpred_us`) stand
as designed and verified. The wrapper-overhead *removal* does not - it is
back to paying the extra call/return layer on every decoded bin, same as
before this investigation started. A real fix needs either a real AmigaOS
toolchain session to iterate against directly, or a different mechanism
that doesn't route a hand-asm-only symbol through `--wrap` in the first
place (e.g. teaching `ih264d_cabac_wrap.c`'s trampoline itself to become a
tail call the compiler can eliminate, rather than trying to bypass it
entirely) - not attempted here.

Verified on real m68k/big-endian under qemu (both mechanisms, before the
revert and after): `tests/run_m68k_check.sh`'s default build links and
decodes every existing H.264 fixture with unchanged worst-frame MAE either
way, a `-DMR_H264_CABAC_PROFILE=1` build (`mr_decode_cabac_profile.m68k`,
one dedicated H.264 clip) decodes bit-for-bit identically, and the existing
differential CABAC/mvpred fuzz tests (`mr_h264_m68k_check`,
`mr_h264_cabac_coeff_check`, `mr_h264_mvpred_dispatch_check`) and the
68060 disassembly scan of `vendor/libavc_port` all pass unchanged on host
and m68k alike. None of that caught the AmigaOS link failure - only an
actual `m68k-amigaos-gcc` build did, which is exactly the gap this section
exists to record.

**Fast/Turbo's bilinear luma path had one genuinely hot branch doing per-
pixel general multiplication where the weights never change within a call -
now specialised into four constant-weight functions instead.** Full-quality
six-tap interpolation is hand-asm (`ih264_m68k_interp.S`), but Fast/Turbo's
degraded bilinear quarter-pel path (`ih264_mc_degrade.c`'s `luma_bilinear()`,
installed into all 15 non-copy `apf_inter_pred_luma[]` slots) is still
scalar C, and its two "one axis is whole/half-pel" branches already use the
file's own established packed-byte idiom (`copy_row_u8()`/`avg_row_u8()`,
four bytes at a time through a 32-bit register) - only the general "both
axes fractional" branch (slots 5/7/13/15, dx and dy both in {1,3}) was left
doing `inv*src[col] + dx*src[col+1]` with `dx`/`dy` as ordinary `WORD32`
runtime locals, so GCC had no constant to fold the multiply against.

Four new functions (`luma_bilinear_qpel_1_1`/`_3_1`/`_1_3`/`_3_3`,
`ih264_mc_degrade.c`) are byte-for-byte the same computation as that
branch - same rolling two-row buffer, same swap - with `dx`/`dy` baked in
as compile-time literals in four separate instantiations of one macro
instead. That alone is enough for GCC's own constant-multiply strength
reduction to turn every `MULS.L`/`MULU.L` into a plain move or a single
shift-and-add on m68k - confirmed by grepping each function's `-O2 -S`
output on `m68k-linux-gnu-gcc -mcpu=68030`: zero `muls`/`mulu` instructions
in any of the four, versus the generic branch which still has them. No
hand-written assembly needed for this part of the win - the multiply
disappears because the compiler can see it is multiplying by 1 or 3, not
because anything was manually scheduled into registers.

These four slots are exactly where BBC One and similar Fast/Turbo live
streams spend real bilinear-path decode time: real (non-integer,
non-half-pel) motion almost always lands on a fractional offset in *both*
axes, and `dx`/`dy`==0 or ==2 (the cases the packed-average/copy fast paths
already cover) are comparatively rare. Verified via the existing
`check_luma_bilinear()` in `tests/mr_h264_mc_degrade_check.c`, which already
iterates every `apf_inter_pred_luma[]` slot (0-15) against the spec formula
in `bilinear_reference()` across every H.264 partition geometry (4x4
through 16x16) - no test changes were needed, since installing these four
functions in place of `luma_bilinear()` at those slots is exactly what the
test already exercises. Passes bit-exact on host and on real m68k/big-endian
under qemu (`check-m68k`) alike.

A genuine "packed loads/stores" version of this same branch - processing
four pixels per iteration through 32-bit registers the way
`copy_row_u8()`/`avg_row_u8()` already do, rather than one pixel per loop
iteration - was investigated and deliberately not attempted here. Unlike
rounded averaging (`avg_u8x4()`'s `(a|b) - (((a^b)&mask)>>1)` identity,
which conveniently never leaves the 8-bit range at any intermediate step),
a general `weight_a*a + weight_b*b` needs a genuine multiply per lane and
headroom wider than 8 bits per lane for the intermediate sum (worst case
`3*255+3*255=1530`, 11 bits) before the final `>>2`/`>>4` and narrowing back
to a byte - a legitimate SWAR (SIMD-within-a-register) technique, using
16-bit lanes in a 32-bit register instead of `avg_u8x4()`'s 8-bit ones, but
one this pass did not attempt to hand-derive and verify without a reference
to check it against beyond first-principles reasoning. A real follow-up,
not bundled into this same-breath change.

Real-hardware speedup - the reason for doing this - still needs a
68030/68060 pass to confirm, per this file's standing qemu-vs-hardware
caveat; this section only proves the multiply is actually gone from the
generated code and that removing it changed nothing about correctness.

## In-app help (AmigaGuide) and remembered last folder
`MintVID.guide` (repo root) already existed - a polished 1.2.0-era manual
(Overview, What's New, Installation, Controls, GUI editions, IPTV, YouTube,
Formats, Troubleshooting, Reporting a problem, Licensing, Support - 12
nodes, each with a "Back to Contents" link) that `Makefile.amiga`'s
`release` target already copies into every packaged build
(`if [ -f ../MintVID.guide ]; then cp ... fi`). It was never actually wired
to anything at runtime, though: nothing in any GUI ever opened it. Two
things were missing, not the whole file - and the first pass at this
session's own change nearly deleted the existing manual outright by
writing over it without reading it first, caught only by the file showing
as modified rather than new in `git status`; the restore-then-extend
approach below is what actually shipped, not the original overwrite.

Four new nodes fill real content gaps the existing manual didn't cover at
all - `Audio` (rate/mono/no-audio/Fast buffer), `Live streaming and
networking` (`--net-queue`/`--live-resync`), `Command line (mrplay)`
(direct invocation examples) and `Codec support list` (an explicit
per-FourCC accepted/rejected table, condensed from README.md's own audit
table, cross-linked from the existing prose `Formats` node rather than
replacing it) - linked from both `MAIN` and `TOCPage`'s identical topic
lists (the file already duplicates that list in two nodes; both had to be
updated together to stay in sync). `Controls` gained two sentences noting
the new `Guide...` menu item and the remembered last-folder feature.
Every node/endnode pair, `@{b}`/`@{i}`/`@{u}` markup pair and `LINK` target
was checked mechanically against the raw text (18 nodes, all balanced, no
dangling link) before publishing, the same discipline the original
13-node draft of this section was checked with - real AmigaGuide/Multiview
parsing still cannot be exercised on this dev host.

The actual runtime gap that needed real code (not just guide content):
opened from every GUI's `MintVID` title-bar menu via a new `Guide...` item
(`mr_gui_open_guide()` in `amiga/mr_gui_menu.c`, shared by all six GUI
binaries the same way `mr_gui_show_about()` already is).

**First attempt used `amigaguide.library`'s `OpenAmigaGuideAsync()`
directly, compiled clean on the real `m68k-amigaos-gcc`/NDK CI build, and
still did not open the guide on real hardware.** This is exactly the gap
this file's own "Validate against ffmpeg" section warns about in its
sharpest form yet: a *compile* success proves nothing about *runtime*
correctness for an API this tree had never called before, and there was no
way to catch that gap without an actual AmigaOS run - which the user
provided, reporting the menu item simply did nothing. Only `nag_Name` was
ever set on a zero-initialised `struct NewAmigaGuide`, deliberately
minimising which struct fields the code depended on getting right - and
that minimalism is exactly why the bug is hard to pin down from here:
plausible causes include a wrong (if plausible-looking) field name that
happened to occupy space the library silently ignored rather than one CI's
compiler flagged, a genuinely different real prototype/tag convention for
`OpenAmigaGuideAsync()`, or simply needing more than a bare `nag_Name` to
actually launch (e.g. a screen or public-screen name) - none of which is
distinguishable from here, since there is still no AmigaOS toolchain/NDK on
this dev host to test any of them against.

**Fixed by not calling `amigaguide.library` at all - launching the
standard AmigaOS `AmigaGuide` command as a subprocess instead, the same
`LoadSeg()`+`CreateNewProcTags()` shape this project already uses (and has
confirmed working on real hardware) for `mrplay`/`iptvgui`/`ytgui`.** The
user's own steer: "should be the same as how MintPRINT opens etc" -
another of their AmigaOS applications, whose own Guide help apparently
already works this way. `mr_gui_open_guide()` now runs `AmigaGuide
PROGDIR:MintVID.guide` (normally `C:AmigaGuide`, present on the standard
command path, not shipped beside MintVID's own binaries) as a detached
process, with `MintVID.guide`'s presence still checked with `Lock()` first
for a clearer error message. This removes essentially all of the "genuinely
new NDK struct" risk the first attempt carried: `NP_Seglist`/
`NP_FreeSeglist`/`NP_Arguments`/`NP_StackSize`/`NP_Cli`/`NP_CommandName`/
`NP_Name` are the identical tags already proven, three times over, to both
compile *and run correctly on real hardware* in this exact file
(`mrgui.c`'s `open_iptv_browser()`/`open_youtube_browser()`/
`start_player()`) - the only genuinely new element is which external
command gets launched, not the launch mechanism itself. Still needs its own
real-hardware confirmation before being trusted the way the mrplay/iptvgui/
ytgui launches already are, but it no longer carries the first attempt's
specific, now-demonstrated failure mode.

The local-file browser's last-used drawer is now remembered across
relaunches - `ENVARC:MintVID.lastdir` (mirrored to `ENV:` at boot, so it
survives a reboot, unlike `mr_master_options.h`'s deliberately volatile `T:`
controller->browser snapshot) via a new `amiga/mr_last_dir.h`, using only
the same plain `Open`/`Read`/`Write`/`Close`/`Rename`/`DeleteFile`
dos.library calls `mr_master_options.h` already proves compile for AmigaOS
in this tree - no new API surface there. The two GUIs plug it in
differently, matching how each already gets a chosen path:
`mrgui_gadtools.c`'s `browse()` reads the ASL `FileRequester`'s own
`fr_Drawer` field directly (already used in this exact file for the
existing `fr_Drawer`/`fr_File` -> path join, so definitely real) and seeds
`ASLFR_InitialDrawer` on first allocation; `mrgui.c`'s embedded
`GETFILE_GetClass()` gadget has no equivalent read-back of "the current
drawer" it is proven to expose, so the drawer is instead derived by hand
from the already-proven-readable `GETFILE_FullFile` path
(`mr_last_dir_from_path()` - keep everything up to and including the last
`/` or `:`), and seeded back in via `GETFILE_Drawer` at gadget creation -
the one new, unverified tag name on that side, using the same
"real toolchain CI + real hardware" verification path as the AmigaGuide
piece above. Every buffer passed to a tag expecting a string that might
outlive the call it is set in (`initial_drawer` in `mrgui.c`'s `main()`,
`last_dir` as a `static` local in `mrgui_gadtools.c`'s `browse()`) is
deliberately given a lifetime spanning the whole relevant window's life,
sidestepping any question of whether ReAction/ASL copy the string at
Alloc/NewObject time or merely retain the pointer.

## Live HLS playback stall notes (IPTV/YouTube live)

Real A1200 regression report: BBC One (IPTV) and YouTube live both
sometimes display only 1-3 frames, then audio stutters and video stops -
under AGA + Kalms + TurboGT + mono audio + low audio rate + Fast buffer
auto, with Copper doubling not active (Kalms excludes it). Local file
playback is unaffected. This session audited the shared live/network path
(`amiga/hls_fetch.c`'s background worker, `core/mr_hls.c`'s segment
open/lookahead, `amiga/mrplay.c`'s queue-startup/audio-startup gating and
audio-rescue) end to end and added always-on diagnostics, but could not
reproduce the freeze itself - there is no AmigaOS toolchain, no live A1200,
and no real network stream on this dev host (see "Validate against ffmpeg"
above for the standing limitation this falls under). Everything below is
either a structural fact provable from the diff/source, or an explicitly
labelled hypothesis pending a real-hardware trace with the new diagnostics.

**PR #180 (Copper-assisted vertical doubling for HAM6/HAM8) cannot be the
cause - its diff never touches this path.** `git show --stat` on that
merge lists exactly `CLAUDE.md`, `amiga/amiga_display.h`,
`amiga/display_aga.c`, `amiga/display_cgx.c`, `amiga/mrgui.c`,
`amiga/mrgui_gadtools.c`, a 13-line `amiga/mrplay.c` hunk (the "AGA path:"
`--time` diagnostic line and a Scale-chooser wiring change, both inert
unless `--copper-vdouble` is passed), and `tests/mr_iptv_check.c`. None of
`hls_fetch.c`, `mr_hls.c`, `audio_paula.c`, or the queue/audio-startup
logic in `mrplay.c` appear in it at all - and the report's own repro notes
Copper is not even active (Kalms excludes it). This structurally rules out
PR180; whatever the cause is, it predates that PR or was introduced by
PR176/177.

**PR #176/#177 did touch this path substantially, but every functional
change found there is a fix, not a new regression, on its own terms:**
- `mr_ts.c`'s "stop trusting a video PES's own declared length" fix (see
  the "MPEG-TS video PES notes" section above) makes video PES
  reassembly *more* correct for exactly the live-IPTV case this report
  describes - it replaced a bug that silently truncated oversized video
  access units, not a bug that starves audio. Nothing about the fix makes
  `mr_ts_next_packet()` wait longer to emit an *audio* PES (audio still
  completes on its own declared length, independent of the video PES's
  accumulation state), so it should not, on its own, explain the audio
  stutter symptom.
- `mr_mpeg2_set_service()` wiring (`mr_mpeg2.c`/`mr_mpeg2.h`, then wired
  into every `mr_decoder_reset()`/reconnect site in `mrplay.c`) *adds*
  audio servicing during MPEG-2 decode that was previously missing,
  mirroring the H.264 path - another fix, not a new gap.
  BBC One and most UK DVB-derived IPTV rebroadcasts are MPEG-2, so this is
  the most on-topic change in the branch, but it is additive (more
  servicing, not less) and so is not an obvious source of a new stall.
- The Paula worker task priority was experimentally dropped from 5 to 0
  and then reverted back to 5 within this same branch (see
  `2070d32`/`0188216` in git log) *before* it reached this repo's main
  history - `amiga/audio_paula.c` currently still creates that task at
  `NP_Priority 5`, unchanged from before PR176. The revert commit records
  that the priority-0 build was followed by a real hard lockup needing a
  reset, with no `--time` log to explain it - worth remembering as a
  precedent (a live task-priority imbalance on this target *can* produce a
  total freeze with no diagnostic trail) even though the current code is
  back at the old, long-tested value.
- `hls_fetch.c`'s only change in this range is `strncpy`→`memcpy` for a
  `-Wstringop-truncation` warning - behaviourally inert.

**`core/mr_hls.c`'s lookahead is single-segment, not the
`HLS_FETCH_LOOKAHEAD_DEPTH=3` its own sibling comment implies - by
deliberate, pre-existing design, not a regression.** `amiga/hls_fetch.c`
provisions three lookahead slots, but `open_seg()` only ever calls
`mr_http_prefetch_hint()` once, for `i+1`. This used to hint several
segments ahead (`ff94726`, "Buffer several compressed HLS segments ahead
instead of just one") and was deliberately reverted to one (`bda717b`,
"Stabilize HLS shutdown by restoring single-segment lookahead") for
teardown stability - both commits predate PR176/177/180 by over a week.
The practical effect: there is normally at most one segment of compressed
lookahead cushioning a fetch stall, however long that segment's own fetch
takes. This is unchanged by anything in this investigation's date range,
so it is not "the regression", but it does mean a single slow segment
fetch (YouTube live has been observed to stall over a second - see
`hls_fetch.c`'s own design note) has less margin than the sibling comment
suggests, and is worth reconsidering as a real, separate improvement if
the new diagnostics show fetch stalls (not a hang) as the dominant cost.

**Leading hypothesis for a freeze with *no* recovery and *no* diagnostic
output (as opposed to ordinary jitter, which the existing
`present_service_frame()`/audio-rescue machinery already rides out): an
unbounded DNS lookup on a segment fetch, with no reachable path to cancel
it.** `core/mr_http.c`'s `connect_socket()` calls `gethostbyname()` fresh
on every connection (no keep-alive/connection reuse in this codebase -
confirmed by grep: every `connect_socket()` call path opens and closes its
own socket, matching the "two HTTP/S connections must never be open at
once" AmiSSL constraint documented in `hls_fetch.c`). Its own comment
documents this as "the one blocking bsdsocket call in this file's whole
call chain with no timeout of its own" - `connect()` is bounded by
`connect_with_timeout()`, `recv()`/`send()` by `SO_RCVTIMEO`/`SO_SNDTIMEO`,
but `gethostbyname()` has none, relying entirely on
`hls_fetch_cancel()`/`hls_fetch_kick()`'s `SIGBREAKF_CTRL_C` signal to
unstick it. That cancel is *only* ever called from two places:
`hls_fetch_stop()` (teardown) and the live-reconnect block in `mrplay.c`
(`input_eof && !qcount && !loop && network_source && live_resync`) - which
requires the *current* blocking fetch to have already returned before
reconnect logic can run at all. A DNS resolver stall (a flaky mobile/home
network path, a CDN edge host rotated per segment, a transient resolver
hiccup) hitting the fetch for segment 2 - right after segment 1's ~1-3
frames have already drained through the presentation queue - would freeze
the single task with no way to unstick itself, no error, and no recovery:
audio drains its cushion and stutters (Paula genuinely starves - nothing
is decoding), and video simply stops, exactly matching this report. This
condition predates PR176/177/180 entirely (the `connect_socket()` design
note is older code), so it is not a regression from this investigation's
date range either - it is offered as the most structurally plausible
*mechanism* for the reported symptom, not a proven cause. It is also
consistent with the fault being intermittent ("sometimes") and reproducing
on two otherwise-unrelated services (IPTV and YouTube live) that share
only this fetch path, while local file playback (no network fetch at all)
is unaffected.

**A related, previously-silent gap: the live-reconnect path's
resolution-mismatch bailout gave up with zero diagnostic output.**
`mrplay.c`'s reconnect block ends playback outright (`break`, no message
at all, `--time` or not) if a freshly reopened live URL's video dimensions
differ from the stream that just dropped - plausible if a live-edge
reconnect briefly lands on an ad/bumper/slate of a different resolution.
This does not match "1-3 frames on *first* play" (it can only fire after
at least one successful reconnect attempt), but it is a second, real way
this class of stream can go silently dark, and it now reports via
`--live-diag` (see below) rather than saying nothing.

**Diagnostics added this session, all opt-in and independent of `--time`'s
own per-frame `monotonic_us()`/`clock()` overhead**, so a real-hardware
repro run no longer needs full `--time` instrumentation (which this file's
own "qemu vs hardware" and MP2/H.264 sections already document as
measurably perturbing timing-sensitive playback) to localize a stall:
- `mr_hls_set_verbose()` (already existing, previously tied only to
  `want_time`) is now also engaged by a new `--live-diag` flag. Its
  existing `open_seg()` prints ("opening segment N of M" / "segment N
  ready (KB, ms)" / "segment N open failed (ms)") already are exactly
  "segment requested/completed" - a log where "opening segment N" has no
  matching completion line before it goes quiet names the exact segment a
  stall happened on. `hls_refetch_live()` gained matching prints for the
  live-edge playlist poll (start, growth, re-fetch failure, give-up).
  Segment-open timing now uses a `g_verbose`-gated `clock()` bracket
  independent of the fuller `--time`-gated `mr_source_timing_*` path, so
  elapsed ms is visible without it.
- `mrplay.c` gained `--live-diag`, `live_diag_report()`, and a periodic
  heartbeat inside `service_audio_for_display()` - the one thing still
  reachable while the main loop is parked deep inside a blocking fetch
  (via `service_player_during_io()`'s ~20 ms poll in
  `hls_fetch_wait_busy()`, or `mr_ts.c`'s own every-16-TS-packet service
  call), which is exactly what a hang like the `gethostbyname()` scenario
  above needs to become visible instead of silent. Rate-limited with
  `clock()` (not `monotonic_us()`) to about once a second. Reports
  `qcount`, audio-buffered ms, and cumulative decoded/queued/presented/
  dropped counts (`playback_stats` gained a `queued` counter - frames that
  actually made it into the ring buffer, distinct from `decoded`, which
  also counts frames immediately dropped for a full queue/stale-skip/OOM).
  Explicit one-shot reports fire at: playback start, the exact
  `input_eof` 0→1 transition (the moment the demuxer stopped delivering
  packets), every live-reconnect stage (begin/succeeded, and each of the
  TLS-disabled/no-progress/fetch-failed/shape-mismatch give-up reasons,
  each previously silent except under `--time`), and final loop exit
  (quit vs. natural end). The give-up points also `Flush(Output())` when
  `--live-diag` is on, matching this file's established RAM-log
  pattern (see the `RAM:MintVID.log` notes above) so the very last state
  before the process exits is not left sitting in a write-back cache.
  None of this is wired into the IPTV/YouTube GUI launchers yet (they
  build their command lines via `core/mr_play_options.c`, untouched here);
  add `--live-diag` there once a real-hardware run confirms this is the
  right lens, or pass it by hand via Shell in the meantime.

**Real-hardware test plan once this lands**: reproduce the BBC One/YouTube
live freeze with `--live-diag` (Shell-launched, or the log-capture Debug
toggle's `--time` swapped for `--live-diag` temporarily) and read the tail
of the log. An "opening segment N" line with nothing after it names a
hung fetch (most likely the DNS hypothesis above, or a wedged
`recv()`/`SO_RCVTIMEO` that isn't actually firing on this stack); a
"tick" heartbeat that keeps advancing while `qcount` stays 0 and
decoded/presented stop climbing, with no matching "opening segment"
line at all, points instead at the scheduler/decode side (`can_decode`
gating, `mr_ts.c`'s PES reassembly, or the audio-rescue state machine)
rather than the network fetch; a "demux-stopped" report followed by
nothing (no "reconnect-begin") means `live_resync` never triggered -
worth confirming `--live-resync` is actually reaching the process
(it defaults on via `core/mr_play_options.c`, but a Shell-launched repro
without it would misleadingly look identical to a reconnect that gave up
silently before this session's diagnostics).

**Correction: a real A1200 bisect (`gh pr checkout`, PR-by-PR) narrows the
regression to PR #174, not PR #176/#177 as guessed above.** The user
confirmed PR #173 ("codex/fast-mem-buffer") still plays IPTV/YouTube live
correctly on real hardware; PR #174
("codex/mpeg-skip-msmpeg4v2-corruption", merge `7f9e32d`) is the first one
that does not. A real-hardware log (`--time`, YouTube live, AGA+Kalms+
TurboGT) from the *broken* build shows `vpkts`/`apkts` going completely
flat (5 video/9 audio packets, unchanged) across a ~20 s stretch, `audio
rescue: ... packets=0 ...` repeating every ~10 s with zero packets
processed each episode, and `vdecode=1052-1945 ms` per frame against
`libavc-core=40-234 ms` - a real, still-unexplained gap between wall-clock
decode time and libavc's own self-reported cost, on a 256x144 stream on a
68060/50 (confirmed: local H.264 files decode fine on the same machine).
PR #174 touches none of `hls_fetch.c`/`mr_hls.c`/`mr_ts.c` at all - every
hypothesis above this correction (the `gethostbyname()` stall, the single-
segment lookahead margin) is therefore not the cause of *this* regression,
though they remain real, independently-true observations about the fetch
path worth keeping in mind for other failure modes.

**Leading hypothesis, replacing the network-focused ones above: PR #174's
own `mrplay.c` diff (`c3ee1d6..7f9e32d`) added exactly one *unconditional*
(not `want_time`-gated) behavioural change to the H.264 packet-scheduling
path - rebasing `pkt.pts_us` through `container_pts_adjust_us` before
comparing it against `mono_media_clock_us` in the `skip_stale_output`/
micro-rescue lateness check - and that fix, while itself correct, may be
what turned a pre-existing decode-speed shortfall into a permanent stall.**
Before PR #174, that check read `mono_media_clock_us - pkt.pts_us >
period_us` using `pkt.pts_us` **unrebased** - for a live TS stream this is
a large absolute 90 kHz PES clock value (hours of encoder uptime), while
`mono_media_clock_us` is a small, session-relative value, so the
subtraction was reliably a huge *negative* number and could never exceed
`period_us`. That clause was therefore silently dead for any live/network
source with real PES timestamps: frames were never marked stale purely for
running behind, only for a full queue or active micro-rescue, so the
decoder always attempted full output even on stale, decode-behind-schedule
frames. That is consistent with "half speed" as reported for PR #173 -
laggy, increasingly-behind, but still visibly advancing, since output was
never suppressed on lateness grounds. PR #174's fix (see the "MPEG-TS
video PES notes" section's own sibling fix for the *contents* of the
comparison, and note this is a *different* fix, in `mrplay.c`'s own
scheduler, not `mr_ts.c`) makes the comparison meaningful for the first
time - `pkt.pts_us + container_pts_adjust_us` now really is in the same
clock as `mono_media_clock_us`. Once decode cannot keep up in real time
(a characteristic this correction's own log shows is already true on this
hardware for this stream, independent of PR #174), the *now-correct*
lateness check has something real to fire on and marks essentially every
subsequent packet `skip_stale_output` - decoded reference-only, never
queued, forever, unless/until the clock and the packet stream are brought
back in sync. `queue_copy_*()` is only ever reached when
`skip_stale_output` is false, so a stream that falls behind once and never
recovers real-time throughput can go from "occasionally shows a late
frame" (PR #173) to "shows nothing again after the first few" (PR #174)
purely because the gating became accurate. This is offered as the
leading, code-grounded hypothesis for *why* PR #174 is where the bisect
landed - not yet proven, and deliberately not "fixed" by reverting the
rebase (that would reintroduce the real cross-clock comparison bug the fix
exists for) or by guessing at a workaround without hardware confirmation,
per this file's own standing rule about live-tested state on this target.

`playback_stats` gained three always-on counters to test this directly
without `--time`: `skip_queue_full`, `skip_pts_late`, `skip_micro_rescue` -
the `skip_stale_output` computation was split into its three named OR
conditions (no change to the combined value or to which frames are
skipped) so a drop can be attributed to exactly one, and all three are
printed in every `--live-diag` report line
(`skip-queue-full=`/`skip-pts-late=`/`skip-micro-rescue=`). If a real
A1200 run shows `skip-pts-late` climbing in lockstep with `dropped` while
`decoded`/`queued`/`presented` stay flat, that confirms this hypothesis
directly; if `skip-queue-full` or `skip-micro-rescue` dominates instead,
the cause is elsewhere (a genuinely oversized queue backlog, or
micro-rescue itself cycling) and this hypothesis is wrong. Either reading
is useful and was the point of adding the split rather than guessing
further from the existing combined `dropped` counter alone.

Still open, deliberately not guessed at further here: **why does a 68060/50
take 1-2 seconds of wall-clock time to decode one 256x144 H.264 frame from
a live TS source when the same machine decodes local H.264 files fine?**
`libavc-core` (the decoder's own self-reported cost) is only 40-234 us/ms
in the same log - a 5-10x gap from the wall-clock `vdecode` figure that
this session could not attribute by reading `core/mr_h264.c` alone
(`audio_service()` is a confirmed no-op; `present_service_frame()` is
guaranteed cheap during decode since `released` is 0 throughout;
`h264_diag_checkpoint()` and the quit-probe are both confirmed cheap/
inactive below 720p). The next real-hardware capture should be built with
`STAGE_PROFILE=1 CABAC_PROFILE=1` (`make -f Makefile.amiga mrplay
STAGE_PROFILE=1 CABAC_PROFILE=1 ...`) to get the `mc=`/`deblock=`/`recon=`/
`intra=` and `bin=`/`coeff=`/`mvpred=` breakdown lines and see whether they
sum close to the wall-clock figure (found inside libavc) or not (missing
time is in the wrapper/scheduler, needing a different kind of look). This
may be an independent, pre-existing performance characteristic that
PR #174's correctness fix merely exposed, in which case the real fix is
either speeding up decode for this stream shape or adding a bounded
"force at least one frame through" escape valve to the lateness check -
not something to guess at without that next capture.

**Fixed (product decision, not waiting on the STAGE_PROFILE capture above):
`--throughput` mode.** The user's own call, independent of ever pinning
down *why* decode can be this slow on this stream: on this target,
slow-but-moving video beats audio-with-silent-video, full stop. Whatever
turns out to be true about the 68060/50 decode-speed mystery above, a
source whose decode throughput cannot be guaranteed (any network/HLS
source, by construction, once the fetch itself is no longer the
bottleneck) should never let itself get locked into the failure mode the
`skip_pts_late` hypothesis describes - a frame marked stale once and then
every frame after it forever, because the check that marks it can never
be satisfied once the player is behind and decode cannot claw the deficit
back.

`throughput_mode` (defaults to `network_source`, override with the new
`--throughput`/`--no-throughput` flags) removes exactly the two PTS-
lateness signals identified above and nothing else:
- `skip_stale_output`'s `pts_late` clause (the `container_pts_adjust_us`-
  rebased comparison PR #174 made meaningful) is forced false in
  throughput mode - a frame is now skipped only when `video_cap` is
  genuinely full.
- Micro-rescue's own entry condition (`mr_micro_rescue_on_packet()`,
  driven by the identical `pkt_late_us` computation) is skipped
  altogether in throughput mode, so `micro_rescue.active` never becomes
  true from lateness and its own OR-term in `skip_stale_output` is moot
  for the same reason.

Left deliberately untouched, per the user's own scoping: the `queue_full`
(`qcount >= video_cap`) clause - the one safety check that must always
hold, so a decoder racing ahead of presentation still cannot grow the
video queue without bound; the existing `service_audio_for_display()`
wiring - Paula keeps being fed exactly as before, throughput mode changes
nothing about audio; and `core/mr_ps.c`'s MPEG-PS PTS fix (see the
"MPEG-PS timestamps come from the PES header" note above) - that is a
different container's timestamp-*presence* bug, not this one's lateness-
*gating* behaviour, and is not touched by anything in this change.

Two related mechanisms were surveyed and deliberately left as-is, since
the user's request named `skip_stale_output` and micro-rescue
specifically: `present_service_frame()`'s own catch-up loop (drops queued
frames from the front, skipping ahead within the backlog, while
`late_us > period_us && qcount > 1`) still runs - it always shows
*something* each time it is invoked as long as more than one frame is
queued, which is a different failure shape from the "nothing displays
again, ever" this change targets, so it was left alone rather than
folded into throughput_mode without being asked; and the catastrophic
"live-resync: N ms behind live, catching up" fast-forward path (`--live-
resync`, on by default via `core/mr_play_options.c`) still discards
audio and decodes reference-only to reach the live edge - a different,
already-opt-in mechanism for a different purpose (catching up to a live
edge, not per-frame pacing), also left untouched pending its own
real-hardware read once throughput mode's effect is confirmed.

Not wired into the IPTV/YouTube GUI launchers yet, same as `--live-diag`
- `core/mr_play_options.c` is untouched by this change. `make check`
passes unchanged (this is entirely inside `amiga/mrplay.c`, which cannot
be compiled on this dev host); the whole point of `throughput_mode` is a
real-A1200-testable claim ("does video keep moving on a stream that
falls behind, instead of going silent") that only a real-hardware run
with `--live-diag` (`decoded`/`queued`/`presented` should now keep
climbing instead of flatlining, and `skip-pts-late`/`skip-micro-rescue`
should stay at 0) can actually confirm.

**Confirmed on real A1200/68060 hardware: `--throughput` (network-source
default) fixes the reported freeze.** The user's own real-hardware retest
after this landed: "yes thats brung it back - perfect" - IPTV/YouTube live
now keeps playing video through a decode-behind-schedule stretch instead of
going silent after 1-3 frames. This is the first hardware confirmation in
this whole investigation chain (the PR174 bisect, the
`container_pts_adjust_us` mechanism, and `throughput_mode` itself were all
reasoned from source/logs until this point) - the fix is real, not just
plausible.

**Follow-up from that same real-hardware session: two more issues, both
now fixed.**

`--throughput`/`--no-throughput` is now also a GUI-facing choice, not just
an implicit per-source default - the user's own request: "a setting for the
end user as Video : Skip that turns on the throghput_mode", refined to the
label actually shipped, "Video - All Frames, Skip Frames". `mr_play_options`
(`core/mr_play_options.h`) gained a `throughput` field (default 1 - "All
Frames", matching `mrplay.c`'s own `network_source` default so a GUI launch
of a network stream behaves the same as a bare CLI launch with no explicit
flag either way);`append_playback_flags()` (`core/mr_play_options.c`) now
*always* emits one of `--throughput`/`--no-throughput` explicitly rather
than only sometimes emitting `--throughput` - a GUI-launched session's
choice needs to override `mrplay.c`'s own per-source default in *both*
directions (forcing "Skip Frames" on a network stream, or "All Frames" on
a local file, must both be expressible), which a conditionally-omitted flag
can't do. `mr_play_options_parse()`/`mr_play_options_summary()` and
`amiga/mr_master_options.h`'s `mr_master_options_apply()` (the `T:`-file
snapshot IPTV/YouTube browsers read a launched-from-mrgui session's options
through) all updated to match; `tests/mr_iptv_check.c`'s two exact-string
pinned assertions needed their expected strings updated for the
now-unconditional flag.

Both GUIs (`amiga/mrgui.c` ReAction, `amiga/mrgui_gadtools.c` GadTools) grew
a matching two-value chooser, "Video: All Frames" / "Video: Skip Frames",
wired the same way every other play-option control in each file already is
- read in `read_play_options()`/`read_options()`
(`options->throughput = <selected index> == 0`), published through
`publish_play_options()`/`publish_options()` on every change alongside the
existing H.264/audio-rate/fast-buffer/no-audio/mono-audio controls. ReAction
uses a `CHOOSER_GetClass()` object (`video_mode`/`video_mode_label`, disposed
alongside the rest at teardown, its chooser nodes freed via the existing
`free_chooser_nodes(&video_modes)` path); GadTools uses a `CYCLE_KIND`
gadget (`app->video_mode`, freed automatically with the rest of the chain
by `FreeGadgets(app->gadgets)` - GadTools has no per-gadget disposal call
the way ReAction's `DisposeObject()` chain does). Index 0 ("All Frames") is
both gadgets' natural default state and `mr_play_options_default()`'s
`throughput = 1`, so neither needs an explicit initial-value push the way
e.g. `app->h264`/`app->c2p` do in `build_window()` - a freshly created
gadget already agrees with the struct default.

`mrgui_gadtools.c`'s window had no free horizontal space left on the
audio-options row (`audio_rate`/`fast_buffer`/`no_audio`/`mono_audio`/
`scale` already fill 8-628px of the 632px-wide window), so the new cycle
gadget got its own row instead of being squeezed in sideways: inserted at
y=92 (`app->video_mode`, 180px wide - "Video: Skip Frames" is the widest
label it ever shows), with the transport strip, browser buttons and info
line each pushed down one row (92->116->140->164) and `WIN_H` grown from
180 to 204 to match. `mrgui.c`'s ReAction layout needed no equivalent
surgery - `LAYOUT_AddChild` auto-flows, so adding one more child to
`controls_bottom` just makes that row's `HorizLayout` group wrap/grow on
its own.

Neither GUI change has been run on real hardware yet - same standing
"amiga/*.c can only be reviewed, not compiled, on this dev host" limitation
as every other GUI change in this file. `make check` (host-buildable core)
is unaffected by any of this - `mr_play_options.c`/`mr_play_options.h` are
the only non-Amiga-only files touched, and both pass with the updated
`mr_iptv_check.c` expectations.

**The second reported issue - "the iptv takes a while to open, but the
status bar says failed, missing exe or crashed" - was a launch-timeout
false positive, not an actual failure.** Both GUIs poll for the IPTV
browser's status port to confirm it actually opened
(`IPTV_LAUNCH_TIMEOUT_TICKS`, polled every `STATUS_POLL_MICROS`), and both
had it set to 60 ticks at a 250ms poll interval - 15 seconds. On real
A1200/68060 hardware, a legitimately slow channel-directory load/cache
refresh can take longer than that, so the watchdog fired and reported "IPTV
browser did not open (missing binary or crash?)" for a browser that was
simply still starting, not one that had failed. Fixed by quadrupling
`IPTV_LAUNCH_TIMEOUT_TICKS` to 240 (60 seconds) in both `amiga/mrgui.c` and
`amiga/mrgui_gadtools.c` - a plain constant change, no new mechanism, so
nothing else in either file needed to change alongside it. Not yet
retested on real hardware.

**"Skip Frames" mode's own `mr_h264_set_skip_output()` skip turned out to
save almost nothing when the decoder itself is the bottleneck - reading
`core/mr_h264.c` confirmed it only skips the RGB conversion step
(`emit_rgb()`), not the actual decode.** `decode_annexb()` - CABAC parsing,
motion compensation, deblocking, reconstruction, the expensive part - runs
in full for every access unit regardless of `skip_output`; that flag only
decides whether the already-fully-decoded picture gets converted to RGB24
afterward or its buffer just released. This is necessary, not an oversight:
almost every H.264 picture is a reference for later pictures, so skipping
its reconstruction would corrupt everything decoded after it until the next
keyframe. Confirmed by a real report: WinUAE stress-testing YouTube Live at
720p (a resolution the emulated CPU cannot decode in real time even before
throughput mode existed) showed the same "1 frame then stutter" shape under
Skip Frames mode as the original PR174 regression, because CPU cost is
nearly unchanged whether or not the frame gets shown - only the RGB
conversion, a fraction of total decode cost, was ever being saved.

**Fixed with a real skip mechanism, not a display-only one: dynamic
escalation to libavc's `IVD_SKIP_PB` frame-skip mode.** This project's own
`mr_h264_set_speed_mode()` already uses libavc's real frame-skip control API
for Turbo (`IVD_SKIP_B`) and Turbo+ (`IVD_SKIP_PB`, "every displayed picture
is a keyframe" - see the H.264 CABAC notes section above), so the mechanism
was already proven; what was missing was reaching it dynamically from Skip
Frames mode's own lateness signal instead of only as a static, whole-session
performance-mode choice. Checked directly in
`vendor/libavc/decoder/ih264d_parse_slice.c`'s `u4_skip_pic` state machine
before relying on it: under `IVD_SKIP_PB`, a P/B slice reads only
`first_mb_in_slice`/`slice_type` from its header and returns immediately -
no CABAC coefficient decode, no motion compensation, no deblocking, no
reconstruction at all - until the next IDR resets it. That is a genuinely
near-zero-cost skip, unlike `skip_output`'s "decode everything, discard the
picture" shortcut.

`mr_h264_set_dynamic_skip(dec, skip_pb)` (`core/mr_h264.c`/`.h`) is a new,
narrower sibling of `mr_h264_set_speed_mode()`: it only reissues the
`IVD_CMD_CTL_SETPARAMS` frame-skip control call (`set_decode_mode()`,
already used at decoder open), never touching the degrade/MC-quality
settings `mr_h264_set_speed_mode()` also controls. `h264_state` gained a
`base_skip_mode` field, set whenever `mr_h264_set_speed_mode()` runs, so
de-escalating restores whatever the *current* H.264 performance mode
actually asked for (`IVD_SKIP_NONE` for Quality/Balanced/Fast, `IVD_SKIP_B`
for Turbo/TurboGT, `IVD_SKIP_PB` for Turbo+ - where de-escalating is
correctly a no-op) rather than always resetting to `IVD_SKIP_NONE`
regardless of the user's own performance choice.

Wired into `mrplay.c`'s existing lateness machinery rather than a new state
machine: `micro_rescue.active` already represents "persistently behind,
with entry/exit hysteresis" (`MICRO_RESCUE_ENTRY_US`/`_EXIT_US` - see the
Live HLS notes above), distinct from the two simpler one-frame checks
(`queue_full`, `pts_late`) that also feed `skip_stale_output` - reusing it
means dynamic skip only escalates once the player is genuinely,
persistently unable to keep up, not on every individual late or
full-queue frame those two already catch (which would otherwise thrash the
frame-skip control call on ordinary jitter). `micro_rescue.active` is
already never true in `throughput_mode` (its own entry block is
`!throughput_mode`-gated), so dynamic skip only ever engages under "Skip
Frames" mode, never under "All Frames" - matching the request this
implements ("could we not set IVD_SKIP_PB with our frame skip method in gui
instead"). A new `h264_dynamic_skip_active` local mirrors the escalated
state so the `IVD_CMD_CTL_SETPARAMS` call only fires on an actual
escalate/de-escalate transition, not every packet.

Net effect on a stream the decoder genuinely cannot keep up with (the
WinUAE 720p case above): instead of paying full per-frame decode cost while
merely not displaying the result, the player freezes on the last displayed
keyframe - real CPU freed back to the scheduler, keeping audio fed - and
jumps to the next one once it arrives, rather than one frame followed by
silence-adjacent stutter. How long that freeze lasts depends entirely on
the stream's own keyframe interval (GOP length), which this change has no
control over. `make check`/`make check-m68k` both pass unchanged (`mr_h264.c`
is host- and m68k-buildable core; `mrplay.c`'s own wiring is Amiga-only and
can only be reviewed here, not compiled or run, per this file's standing
limitation) - the real-hardware/WinUAE claim ("does audio stay smooth
through a keyframe-only freeze instead of stuttering") still needs its own
retest to confirm.

## P96 PIP overlay display, and two GUI Scale-control bugs

A customer report (68060/66MHz + Mediator + Voodoo3, H.264/AVC 540x360)
said current playback is slower than 1.2.0 and asked about "the planned
overlay display" this project's own docs had previously mentioned. That
phrase points at real graphics-card hardware video overlay - exactly what
Voodoo3/Permedia/BVision-class boards expose - which does colourspace
conversion and scaling on the card instead of the CPU, the thing most worth
having on precisely this slow-68k-plus-fast-RTG-board combination.

**`amiga/display_p96pip.c` (`backend_p96pip`) is a new display backend
built on Picasso96API.library's "PIP" (Picture-In-Picture) API -
`p96PIP_OpenTagList()` et al, declared in the vendored
`amiga/include/libraries/Picasso96.h`/`amiga/include/inline/Picasso96API.h`
headers `display_p96.c` already depends on.** It requests
`P96PIP_Type=PIPT_VideoWindow` (a real hardware overlay window on boards
that support one) first, falling back to `PIPT_MemoryWindow` (the
always-available software-composited PIP) only if that specific open fails
- so picking overlay mode never loses playback, only the chance at real
acceleration, the same fallback discipline `display_open()`'s whole backend
chain already uses. Two structural differences from `backend_p96`'s own
direct screen-bitmap-lock approach, both explained at length in the new
file's header: it is not fullscreen-only (a PIP owns its own dedicated
surface, so there is no equivalent of `backend_p96`'s "unclipped writes
corrupt sibling windows" hazard), and it does no CPU-side scaling at all (the PIP is
given the source size once, `P96PIP_Width`/`Height`/`Left`/`Top` describe a
separate, independently aspect-fitted destination rectangle, and Picasso96
- potentially the board itself for a real video window - does the resize;
`backend_p96`/`backend_cgx` both still pay for `mr_scale_resize_rgb24_strip()`
on the CPU whenever the window isn't the stream's native size).

**Deliberately not done in this first pass: the PIP's source format is
`RGBFB_B8G8R8`, reusing the exact BGR24 pixels `backend_p96`'s `show_bgr()`
already consumes - not one of the YUV `RGBFTYPE`s `Picasso96.h` documents as
"for use with a hardware window only".** That comment is the strongest hint
in the vendored headers that real overlay hardware expects YUV, not RGB, so
whether requesting `PIPT_VideoWindow` with an RGB source actually engages a
Voodoo3's overlay engine, or Picasso96 quietly falls back to software
compositing behind an identical-looking API, is unknown without a real
board to test against. Feeding real packed YUV into a video-window PIP -
skipping the H.264/MPEG-2 YUV->RGB24 conversion these decoders already pay
for, the same win `mr_mpeg2_set_yuv_output()` gets for the AGA indexed path
(see "The RGB24 round-trip is the expensive part" above) - is the natural,
larger follow-up once this base mechanism is confirmed working on real
hardware; this first step instead validates the open/write/resize/close
mechanics end to end using pixel data every other RTG backend already
proves correct against ffmpeg, and is something the reporting user can
actually test (unlike a from-scratch YUV overlay path, which would need its
own correctness pass before it could be trusted on their hardware at all).

Several coordinate-space and lifecycle details needed to be gotten right
from documentation alone, with no way to compile or run any of it here (no
AmigaOS toolchain, no Voodoo3/overlay-capable board, and not even a known
case of WinUAE's own P96/UAEGFX emulation implementing the PIP API at all -
this is a sharper version of this file's standing "Validate against
ffmpeg" limitation, since not even the emulator this user has access to is
known to exercise this specific API): `P96PIP_Relativity` defaults to
`PIPRel_Width|PIPRel_Height`, which (easy to miss, since `P96PIP_Width`'s
own doc comment reads "default: inner width of window" as if already
absolute) means `P96PIP_Width`/`Height` are by default interpreted as an
unused margin at the window's right/bottom edge, not an absolute size -
`open_pip()` explicitly clears it to 0 on every open and `SetTags` call.
Windowed placement needed `WA_InnerWidth`/`WA_InnerHeight` (content size,
matching `display_p96.c`'s/`display_cgx.c`'s own windowed opens) rather than
`WA_Width`/`WA_Height` (outer, border-inclusive), with a `sync_content_geometry()`
helper re-reading the real post-open/post-resize content rectangle from the
live window every time, mirroring those two files' own `bl`/`bt`/`iw`/`ih`
tracking; `P96PIP_Left`/`Top` are then set relative to that content origin,
the same "add `bl`/`bt` to every draw coordinate" convention every other
backend in this tree already uses - `Picasso96.h` does not actually document
which coordinate space `P96PIP_Left`/`Top` use, so this is the most
consistent assumption available, not a confirmed one. `P96PIP_SourceWidth`/
`Height` are Init-only (no settable equivalent), so a live source-resolution
change (an HLS stream's SPS changing resolution mid-segment, the same case
`display_p96.c`'s `p96_show_packed()` already handles for its own
screen-bitmap path) needs a full PIP close/reopen (`reopen_for_size()`)
rather than a `SetTags` update. `p96PIP_GetTagList()`'s own return-code
convention (count processed vs. count failed) isn't documented in the
vendored header either, so every `P96PIP_SourceBitMap` fetch checks the
retrieved pointer itself instead of trusting a guessed sign convention on
the return value.

**Real-hardware confirmation, then a merge.** The reporting user tested this
on their own Voodoo3 and confirmed overlay mode works ("overlay is good").
They then asked the natural follow-up: does plain P96 fullscreen (the
option that existed before any of this) get any of that benefit, or is it
"just bigger writepixel"? It was the latter - `backend_p96`'s direct
screen-bitmap lock is a lower-overhead way of doing the same fundamentally
CPU-bound thing `WritePixelArray` (CGX) does, with the identical
`mr_scale_resize_rgb24_strip()` CPU scaling cost, just skipping the RTG
driver's own copy/convert call. Their request, once that was clear: fold
the overlay backend into what "RTG (P96)" already means, rather than
keeping it as a separate menu entry someone has to know to pick instead of
the older option. So `MR_DISPLAY_P96_OVERLAY` (the separate enum value,
`--p96-overlay` CLI flag, `display_set_p96_overlay()`, and the third
"RTG (P96 Overlay)" entry in both GUIs' Display choosers) was removed again
- all of it lived for exactly one PR round-trip before being superseded,
never shipped as a released option, so this is a straight revert of that
plumbing rather than a deprecation.

`display_open()`'s backend-selection chain (`amiga/display.c`) now tries
`backend_p96pip` first whenever P96 mode is selected at all
(`display_set_force_p96(1)` - the one flag both backends now share), and
falls back to the older `backend_p96` only if the PIP backend's `open()`
fails, then `backend_cgx`, then `backend_aga` exactly as before - `order[]`
stays at 4 slots (unlike before, though, this is no longer "4 backends
each independently optional", but two backends serving one option plus the
two unconditional fallbacks). Unlike `backend_p96`, `backend_p96pip` needs
no `cybergraphics.library` at all (it never calls a CGX function, only
Picasso96API.library ones), so its own gate is `g_force_p96 && P96Base`
with no `CyberGfxBase` requirement - `backend_p96`'s gate still needs
`CyberGfxBase` too. `display_backend_name()` still reports which of the two
actually opened ("RTG (P96 Overlay)" vs "RTG (P96)"), so a `--time` log can
tell them apart even though the user only ever picks one "P96" option -
useful precisely because whether real hardware acceleration engaged for a
given board is still an open question (see below).

**Correction: the first cut of this merge kept forcing `--fullscreen` for
P96 in `mr_build_player_arguments()`, carried over unexamined from the old
contract - the user's own follow-up caught that this defeats the entire
point.** The real desired flow, exactly as they described it: P96 opens as
a normal window first (still hardware-accelerated if the board grants a
PIP video window), and pressing F is what takes it to fullscreen -
"falling back in software rendering if card can't do it - it shouldn't
just open full screen o play". `backend_p96` (the older backend) refuses
to open at all without `--fullscreen` (unclipped writes would corrupt
sibling windows - see its own file header); `backend_p96pip` has no such
restriction and opens windowed happily, so forcing fullscreen at launch
was never actually *required* once the overlay backend existed - it was
just leftover behaviour from when "RTG (P96)" meant only the old
direct-lock backend. `mr_build_player_arguments()`'s forced-`--fullscreen`
case for `MR_DISPLAY_P96` is removed entirely: P96 now opens windowed from
both GUIs by default, and F (`display_toggle_fullscreen()` ->
`p96pip_toggle_fullscreen()`) is what takes it fullscreen, trying real
hardware acceleration (`PIPT_VideoWindow`) again on every toggle and
falling back to software compositing (`PIPT_MemoryWindow`) only if the
board refuses - the exact "hardware if the card can do it, software if
not" contract the user asked for, symmetric between windowed and
fullscreen. `tests/mr_iptv_check.c` now pins the opposite of what it
would have pinned a moment earlier: building P96's player arguments must
NOT contain `--fullscreen`. A side effect worth naming: a direct `mrplay
--p96` invocation with no `--fullscreen`, which used to fail P96 entirely
and fall through to CGX (the old backend's own refusal), now opens
windowed via the PIP backend by default too - consistent with the new
contract, not a separate case to special-case around.

`mr_play_options.h`/`.c`, `tests/mr_iptv_check.c`, and both GUIs' mode
lists/`update_mode_controls()` all reverted to their plain two-way CGX/P96
shape (no third enum value or chooser entry to plumb through) - `make
check` passes with the reverted pinned strings; every Amiga-only file here
(the backend and both GUI files) can only be reviewed, not compiled or
run, on this dev host - the same standing limitation as everything else in
this section.

**Two separate, real-hardware-reported GUI bugs were fixed alongside this,
unrelated to overlay mode itself, spotted by the same user while looking at
these controls:**

- **GadTools "Copper 2x" (the third row of the Scale cycle gadget,
  `mrgui_gadtools.c`) did not actually show its text - the gadget's box was
  too narrow.** It was squeezed into 70px of leftover space on the audio
  options row (`558..628` of a 632px-wide window), sized by estimate against
  topaz 8pt with no real-hardware check - explicitly flagged as unconfirmed
  when that layout first shipped (see "AGA copper-assisted vertical
  doubling notes" above). Fixed by moving the Scale cycle onto its own row
  next to the Video Mode cycle (`8..188`), where `188..632` was entirely
  free, and widening it to 180px there - also renamed its three labels from
  bare "None"/"2x"/"Copper 2x" to "Scale: None"/"Scale: 2x"/"Scale: Copper 2x"
  for consistency with every other cycle gadget in this window (`Display:`,
  `C2P:`, `H.264:`, `Audio:`, `Fast buffer:`, `Video:`), which this control
  had been missing since the None/2x/Copper 2x cycle replaced the old
  separate checkboxes. `update_mode_controls()`'s own logic (disable/reset
  to None on CGX/P96, snap Copper back to plain 2x on an
  ineligible C2P/mode) was already correct and untouched - this was purely a
  layout/width fix. Not yet retested on real hardware.

- **ReAction's Scale chooser reportedly did not grey out or reset to None
  when switching to P96.** Source review found `mrgui.c`'s
  `update_mode_controls()` logically identical to GadTools' own (already
  working) equivalent - same three-way disabled check, same
  `SetGadgetAttrs(..., GA_Disabled, TRUE, CHOOSER_Selected, 0, TAG_DONE)`
  combined call for the disable path - so this could not be root-caused from
  source alone, the same class of gap this file's "Validate against
  ffmpeg" section exists to name: a plausible reading of the code is not the
  same as confirmed correct behaviour on real ReAction/BOOPSI gadget classes,
  and there is no way to exercise `chooser.class` here to find out which. The
  conservative fix applied: split that one combined `SetGadgetAttrs` call
  (`CHOOSER_Selected` and `GA_Disabled` together) into two separate calls,
  for the Scale gadget's disable path only - forcing two independent
  attribute-update/redraw passes on the gadget instead of relying on both
  tags being applied and rendered correctly from one combined taglist. Not
  yet retested on real hardware; if this does not fix it, the real cause is
  still unknown and would need a fresh real-hardware trace (e.g. does the
  Lace checkbox's own combined disable call, right next to Scale's, show the
  same symptom or not - that would show whether this is Scale-specific or a
  general combined-taglist issue this fix just happened not to also need for
  Lace).

**Overlay mode itself is confirmed working on real Voodoo3 hardware** (the
user's own "overlay is good") - the open/write/resize/close mechanics this
section's own design-rationale paragraphs above worried about all check out
in practice. Still open, now that P96 always tries it first: whether the
`--time`/`g_display_want_time` log's `p96pip: opened ... overlay` line
actually reports "hardware (PIPT_VideoWindow)" (real board acceleration)
rather than falling back to "software (PIPT_MemoryWindow)" for this board,
and whether that distinction measurably affects CPU-bound H.264 decode -
neither was reported one way or the other alongside the "overlay is good"
confirmation. The two GUI fixes above (Copper 2x layout, ReAction Scale
greying) still each need their own real-hardware retest, on their own
editions.

**A separate real-hardware crash report, GadTools edition: Guru 8100 0005
(CPU Zero Divide, this codebase's established Guru-number convention - see
the "AGA copper-assisted vertical doubling notes" section above for the
other confirmed instance of this exact code), reported while browsing for
a file: "if it trys to open a folder - assign thats not already there, I
got the requestor and this happened when I clicked after pressing ignore
on the NAS1 is not mounted".** Read literally, two different things could
produce that sequence, and which one actually happened changes what is
fixable here:

1. **Our own remembered-last-folder seeding** (`amiga/mr_last_dir.h`,
   `ENVARC:MintVID.lastdir` - see the "In-app help (AmigaGuide) and
   remembered last folder" section above) hands `ASLFR_InitialDrawer`/
   `GETFILE_Drawer` a path from a *previous* session without ever checking
   it is still reachable *now*. `NAS1` reads exactly like a network-share
   volume AmigaDOS still has a live `DosList` entry for (so it is
   "recognised", not simply unknown) but that is currently offline - and
   `Lock()`/`Examine()` on a path naming a recognised-but-absent volume is
   documented AmigaDOS behaviour to trigger the OS's own "Please insert
   volume NAS1" system requester itself, independent of anything specific
   to `asl.library`'s directory listing. If browsing with `NAS1` offline
   reproduces the requester *the first time Browse is opened* (before
   navigating anywhere inside the file requester), this is almost
   certainly it.
2. **Live navigation inside the already-open ASL/GadTools file requester**
   into an assign/volume that turns out to be unmounted, unrelated to
   anything this codebase seeded. That would be `asl.library`'s/
   `dos.library`'s own internal directory-listing code choking on the
   unmounted target, entirely outside code this project writes or can
   patch - the exact same class of "no way to reach this from application
   source" gap the "Validate against ffmpeg" section's standing limitation
   already names for chipset-internal behaviour, just for DOS/ASL internals
   here instead of graphics.library.

**Fixed for case 1**, which is both the more likely reading (the customer's
own wording opens with "if it trys to open a folder", suggesting the very
act of opening the browser, not something navigated to afterward) and the
only one actually reachable from this codebase: `mr_last_dir.h` gained
`mr_last_dir_reachable()`, and `mr_last_dir_load()` now calls it before
ever reporting a saved drawer as usable. It checks reachability with
`SetProcWindow((APTR)-1)` held around a `Lock()`/`UnLock()` pair -
documented, standard dos.library behaviour (not a guess) for making a
`Lock()`/`Open()` on a recognised-but-absent volume fail silently instead
of prompting, restoring the process's previous `pr_WindowPtr` immediately
after. A saved drawer that fails this check is treated exactly like "no
saved value" for that call (falls back to opening the requester with no
initial drawer, the pre-existing behaviour) rather than being deleted from
`ENVARC:` - a network share going offline intermittently is normal, and the
remembered drawer is still worth keeping for when it is back. Since both
GUIs' browse code already goes through this one shared function
(`mrgui_gadtools.c`'s `browse()`, `mrgui.c`'s `GETFILE_Drawer` seeding),
fixing it here covers both editions in one place, not just the GadTools
edition the report came from.

This fix directly prevents the *system requester itself* from being
triggered by our own seeding, which is the mechanism case 1 describes -
but it cannot address case 2, and there is currently no way to tell from
here which one the report actually was. **Still needed from the user to
close this out**: does the Zero Divide reproduce the *very first* time
Browse is opened with NAS1 offline (case 1 - this fix should now prevent
it), or only after navigating further inside an already-open requester
into a different unmounted assign (case 2 - unrelated to this fix, and
likely outside anything this codebase can patch at all)? Either way, this
- like every other Amiga-only change in this file - can only be reviewed,
not compiled or run, on this dev host; it needs a real-hardware retest
with NAS1 offline to confirm the requester no longer appears on a bare
Browse click.

**The `mr_last_dir_reachable()` fix above shipped with a real link failure
this dev host had no way to catch, caught instead by the repo's own CI
`build` job on the real `m68k-amigaos-gcc` toolchain: `undefined reference
to SetProcWindow`.** `<proto/dos.h>` on this Bebbo NDK image doesn't even
declare the function (GCC's own warning: "implicit declaration of function
'SetProcWindow'"), and whatever auto-linked import library this toolchain
provides for dos.library calls doesn't stub it either - unlike `Lock()`/
`UnLock()`/`Open()`/`FindTask()`, all already proven to link (FindTask() in
particular is the exact same call `find_player()` elsewhere in both GUIs
already uses successfully). This is another instance of this file's
standing "no AmigaOS toolchain on this dev host" gap in its sharpest
form: `SetProcWindow()`'s documented behaviour is real and correct, but
whether a *specific toolchain image* actually provides a linkable stub for
a given dos.library call can only be proven by attempting the real link,
the same lesson the 68060 MP2 kernels' missing underscore aliases and the
`__wrap_ih264d_decode_bin` hand-asm-symbol saga both already taught (see
their own sections above) - each one a different specific mechanism, same
root gap. Fixed by writing `pr_WindowPtr` directly on the `struct Process`
(`<dos/dosextens.h>`) instead of calling `SetProcWindow()` at all - a
struct field write needs no library stub, sidestepping the question of
which dos.library functions this exact toolchain happens to auto-link,
rather than hunting for whichever alternate call or header this
distribution actually wants. The reachability logic itself (Lock()/UnLock()
bracketed by the sentinel, previous value restored after) is unchanged;
only the mechanism for setting/restoring `pr_WindowPtr` moved from a
library call to direct struct access.

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

**A real-hardware/WinUAE report ("didn't seem to do anything") on a 720p
YouTube Live stream needed a host-side measurement to actually settle,
not more source reading.** The shared log showed `micro-rescue: entering`
firing repeatedly (so the escalation trigger condition was real) with
`late=` climbing without bound across the whole session (757ms -> 9686ms)
and `phase=h264-decode phase-duration=` staying at 60-97ms throughout, with
no visible drop after entering micro-rescue - looking, from the log alone,
like the escalation wasn't reducing decode cost at all.

Rather than guess further from source, two ad-hoc host probes (not checked
in - one-off verification against this repo's real, patched libavc, linked
the same way `mr_decode` is via `make mr_decode TESTSRC=...`) settled
whether `mr_h264_set_dynamic_skip()` genuinely does anything on this
codebase's actual decoder build. The first probe (escalate cold, before any
packet) decoded zero frames start-to-finish - initially alarming, until
checked against `test_h264_high.mp4`'s own shape: 24 frames, effectively
one GOP, no second IDR to "wake up" on - exactly the single-GOP case this
mechanism is expected to freeze through, not a bug. The second probe
escalated mid-stream instead (decode packets 1-4 normally, escalate, decode
the rest under `MR_H264_SPEED_TURBO_GT` both before and after, matching the
real log's own performance mode) and timed each `mr_decoder_decode()` call
directly: **0.175 ms/packet before escalation, 0.021 ms/packet after -
roughly 8x cheaper**, confirming the mechanism itself is real and correctly
wired on this repo's own libavc, not just plausible from reading
`ih264d_parse_slice.c` in isolation.

That leaves two explanations for the real-hardware report, and there was no
way to add a printf and re-derive it, this time - the reasonable working
guesses are: (1) the shared log predates this fix landing (built from a
branch/commit without it) - plausible and simple, or (2) on a genuinely
720p/30fps stream, whatever is costing 60-97ms per iteration is dominated
by something dynamic skip cannot touch - TS demux reads, PES/Annex-B NAL
reassembly, or simply enough P/I-slice macroblocks even after B-skip that
the picture is still expensive - rather than by macroblock reconstruction
dynamic skip actually removes. `late=` never recovering even once across
the whole session is also consistent with 720p simply exceeding this
target's real-time budget regardless of skip strategy, the same
"`--throughput` exists because slow-but-moving beats silent, not because
it makes the stream decodable in real time" position this file already
takes.

Added a `--time`-gated `h264-dynamic-skip: engaging/releasing (IVD_SKIP_PB)`
printf (`mrplay.c`, right where `mr_h264_set_dynamic_skip()` is called) -
there was previously no way to tell from a log whether escalation actually
fired at all, which is exactly the gap that made this report ambiguous to
diagnose. A fresh real-hardware trace with this line present will show
directly whether escalation is engaging on that build, closing off
explanation (1) - and if it is engaging and decode cost still doesn't drop,
that's real evidence for (2) worth its own `STAGE_PROFILE=1` capture rather
than further guessing.

**Confirmed on a real retest: "Skip Frames" mode is now working correctly.**
The user's own follow-up after rebuilding with the diagnostic print above:
"excellent that skip frames is perfect now!" - the first real confirmation
(as opposed to the host-side per-packet timing measurement above, which
only proved the mechanism *could* work, not that it *did* on the actual
target) that dynamic `IVD_SKIP_PB` escalation fixes the "didn't seem to do
anything" report. Consistent with explanation (1) above being the real
one: the original log was from a build predating this fix, not evidence of
a deeper 720p-specific bottleneck. The `h264-dynamic-skip: engaging/
releasing` printf did its job as a diagnostic even though the follow-up
report didn't come with a fresh log attached - the fix itself is what
mattered, and it is now real-hardware-confirmed the same way `--throughput`
mode was confirmed earlier in this file.

## Release packaging notes
`Makefile.amiga`'s `build_release_cpu` was packaging the guide icon from a
stale root-level `../MintVID.guide.info` (last touched by the 1.3.0 release
commit) instead of the current one added later at `player/icons/
MintVID.guide.info` (the "Icon folder" commit) - the two differ (7182 vs
6096 bytes), and only the `player/icons/` copy was ever meant to be current,
matching where every other packaged icon in this target (`amiga/icons/*`)
already lives relative to `Makefile.amiga`. Fixed by pointing the `release`
target at `icons/MintVID.guide.info` (relative to `player/`, where
`Makefile.amiga` runs from) instead of the root-level file. Also removed
`player/icons/MintVID.guide.info:Zone.Identifier`, a stray Windows NTFS
Alternate-Data-Stream/Mark-of-the-Web marker that had been committed
alongside the real icon file - inert on AmigaOS/Linux but not something
that belongs in the tree.

## GadTools YouTube quality selector notes
A user report: the YouTube-GT quality control never actually changes from
"Quality: Low" when clicked, even though the effect is real - the "Playback
options" summary line right below it *does* update to reflect the new
`hls_max_width`/`hls_max_height`, confirming `set_quality()` and
`app.quality_index` were both cycling correctly. Only the button's own
on-screen label was stuck.

Root cause: `amiga/youtube_gadtools.c`'s `G_QUALITY` (and `G_LOG`) were
plain `BUTTON_KIND` gadgets, manually relabelled at runtime via a
`set_button_text()` helper (`GT_SetGadgetAttrs(..., GTTX_Text, ...)` +
`RefreshGList()`). That is the *only* place in this entire codebase that
tries to change a GadTools gadget's displayed text this way - every other
multi-state or toggle control in every GadTools GUI (`mrgui_gadtools.c`'s
Display/C2P/H.264/Audio/Fast buffer/Video/Scale cycles, `iptv_gadtools.c`'s
own `G_DEBUG` Log toggle, and this exact file's own `G_TYPE` search-type
selector) uses `CYCLE_KIND` with `GTCY_Labels`/`GTCY_Active` instead, which
gadtools.library redraws internally on click with no manual relabelling
code at all. That strongly points at manual `BUTTON_KIND` relabelling being
the actual defect, not the index/options bookkeeping around it (which was
already proven correct by the summary line).

Fixed by converting both `G_QUALITY` and `G_LOG` to `CYCLE_KIND` (`quality_
labels[]`/`log_labels[]`, `NULL`-terminated `STRPTR` arrays matching every
other cycle gadget's own label array shape), seeding `GTCY_Active` to the
right starting index right after `OpenWindow()` (mirroring `G_TYPE`'s own
existing `MR_YOUTUBE_SEARCH_LIVE` seed a few lines above it), and replacing
both click handlers' manual increment-and-relabel with a plain
`value(&app, gadget, GTCY_Active)` read - the same `value()` helper this
file's own `search()` already uses for `G_TYPE`, and the same shape
`iptv_gadtools.c`'s `G_DEBUG` handler uses for its own Log toggle. The
now-fully-unused `set_button_text()` helper was removed rather than left as
dead code. `G_LOG` was fixed alongside `G_QUALITY` even though only the
quality control was reported broken, since it shared the exact same
`set_button_text()` mechanism and so was presumed equally affected, not
because it was independently confirmed broken.

This file (`amiga/youtube_gadtools.c`) can only be reviewed, not compiled
or run, on this dev host (see "Validate against ffmpeg" above) - the root
cause is inferred from the total absence of any other confirmed-working
runtime-`BUTTON_KIND`-relabel precedent anywhere else in the codebase, not
from a reproduced/compiled repro of the bug itself. Needs a real-hardware
retest to confirm the Quality and Log cycle gadgets both now visibly
advance on click.

## H.264 TurboGT retirement
A user question - "turbo and turbogt are the same thing now, why did that
happen?" - traced back to `8c7a290` ("Fix mixed-degrade deblocking and
implement libavc's dead MC degrade bits"). That commit found a real
correctness bug: libavc's per-macroblock deblocking state (`ps_deblk_pic`)
persists across pictures, so a *mixed* degrade policy - some pictures
degraded, some not, which is what `i4_degrade_pics` values 1 and 3 asked
for - left an undegraded picture deblocked against the previous, degraded
picture's stale boundary strengths and QPs: wrong output, and Fast actually
ran ~47% *slower* than Quality because of it. The fix forced every
degrading H.264 speed mode onto the same all-or-nothing `i4_degrade_pics=4`
policy. TurboGT's *only* distinction from Turbo, before that fix, was
exactly `i4_degrade_pics=4` instead of 3 (disabling keyframe deblocking too,
on top of Turbo's B-skip) - so once every mode had to use 4 for correctness,
Turbo and TurboGT became bit-for-bit the same policy. A replacement lever
for TurboGT (truncating motion vectors to whole samples) was measured and
rejected at the time: 3-4% faster for a 17 dB PSNR loss. TurboGT was kept
selectable anyway, in the GUIs, `--h264-speed=`, and as the default, purely
so nothing broke for anyone already using it.

Asked directly whether TurboGT should still exist given it does nothing
different from Turbo, the user's call: retire it, but keep `--h264-speed=
turbogt`/`turbo-gt` parseable and aliased to Turbo, since it is a real,
long-shipped (pre-1.2.0) option someone could have in a script. There is no
binary/persisted-settings compatibility concern to weigh against that: the
only place `mr_play_options` is ever written to disk is
`mr_master_options.h`'s `T:` snapshot, already documented elsewhere in this
file as deliberately volatile and session-scoped, not something upgraded
across a rebuild - so the enum values themselves were free to renumber
(`MR_H264_SPEED_TURBO_GT`/`MR_H264_PERF_TURBO_GT` removed outright, both
enums' last entries), with only the *text* CLI keyword needing a back-compat
alias.

Removed: both enum values; `core/mr_h264.c`'s duplicate `TURBO_GT` switch
case (its policy was identical to `TURBO`'s, so nothing else needed
changing there); TurboGT's entries from both GUI choosers
(`mrgui.c`/`mrgui_gadtools.c` - so it can no longer be newly selected) and
their `<= MR_H264_PERF_TURBO_GT` bounds checks, now `<= MR_H264_PERF_
TURBO_PLUS`; the default `CHOOSER_Selected`/`GTCY_Active` seed in both GUIs,
now `MR_H264_PERF_TURBO`; `mrplay.c`'s `effective_h264_speed()` Auto
resolution and its naming ternary; `--h264-speed=turbogt` from the usage
string (still accepted, just no longer advertised). Kept, as an explicit
alias mapped straight to `MR_H264_PERF_TURBO`/`MR_H264_SPEED_TURBO` at the
parse boundary rather than as a live enum value threaded through the rest
of the codebase: the `turbogt`/`turbo-gt` string in `core/mr_play_options.c`
(GUI/IPTV argument parsing), `amiga/mrplay.c` (the CLI parser), and
`tests/mr_decode.c` (the host test-harness CLI parser, which had its own,
separate copy of this same parsing). `mr_play_options_default()`'s default
changed from `MR_H264_PERF_TURBO_GT` to `MR_H264_PERF_TURBO` - identical
runtime policy, so this is a naming-only change, not a behavior change.

`tests/mr_iptv_check.c` needed the most rework: its dedicated "set
`h264_performance` to `MR_H264_PERF_TURBO_GT` directly and check the
`--h264-speed=turbogt`/`\"H264 TurboGT\"` output" block no longer compiles
(the enum value is gone), so it was replaced with alias-behavior coverage
instead - parsing both `--h264-speed=turbogt` and `--h264-speed=turbo-gt`
from argv and asserting the result is `MR_H264_PERF_TURBO`, alongside the
existing `turbo`/`turbo+` parse-and-resolve cases. Every other pinned string
that used to expect `turbogt` (the default-options build-arguments cases)
now expects `turbo`, with an added `!strstr(args, "turbogt")` check so a
regression back to the old default would fail loudly rather than just
matching a differently-worded string.

Verified via `make check` end to end (host build, `mr_iptv_check` and
`mr_h264_mc_degrade_check` rebuilt and rerun individually first to isolate
them) - all pass, including a manual `mr_decode --h264-speed=turbogt` vs.
`--h264-speed=turbo` smoke test producing byte-identical decode output.
Not re-verified under `make check-m68k`: this change is a pure enum-value
removal and switch-case deletion with no new arithmetic, no new asm, and no
endianness/alignment surface - the class of change `make check-m68k` exists
to catch (see "Validate against ffmpeg" above) - so the host build already
fully exercising the same, unchanged `core/mr_h264.c` code path is
sufficient here, unlike e.g. the 68060 kernel work elsewhere in this file.

## Display-mode default priority and persisted controller settings
Two related end-of-PR requests: should P96 be preferred over plain RTG
WritePixel when both are available, and does the software remember what a
user last set at all?

**The RTG default was picking WritePixel, not P96, and had been since P96
was added as a chooser entry.** Both `mrgui.c` and `mrgui_gadtools.c`
compute a `default_mode` index into their own chipset-dependent mode list
when RTG is detected (`default_screen_is_rtg()`/`screen_is_rtg()`) - but
the arithmetic pointed at the *second-to-last* added entry
(`mode_count - 2` in `mrgui.c`; `mrgui_gadtools.c` captured the index
right after adding WritePixel, before P96 was even appended). Since the
list is always built AGA/ECS...HAM6/HAM8...WritePixel...P96 in that
order, both landed on WritePixel, one off from P96 - a plain off-by-one
in intent, not a hardware-detection bug (RTG detection itself was already
correct). Given P96's own hardware-overlay-first backend chain (see the
P96 PIP overlay section above), P96 is now genuinely the fastest option
when a board grants it, so it should be preferred - fixed by pointing
`default_mode` at the last-added entry instead (`mode_count - 1` in
`mrgui.c`; moving the same capture to after P96 is appended in
`mrgui_gadtools.c`). Priority is now P96 > RTG WritePixel > AGA/HAM,
falling further back through the existing runtime chain
(`display_open()`) if a board can't actually open what the GUI selected -
this only changes which mode the GUI *offers first*, not the safety net
underneath it.

**No settings were ever remembered - every launch reset to
`mr_play_options_default()`'s hardcoded values.** The only persistence
anywhere in the tree before this was `mr_last_dir.h`'s single remembered
file-browser drawer path; display mode, C2P, H.264 speed, audio
rate/mono/no-audio, fast buffer, scale/lace and the Skip Frames toggle
all came back to the same defaults on every relaunch, regardless of what
was last chosen.

New `amiga/mr_saved_options.h` persists the main controller's own options
to `ENVARC:MintVID.settings` (survives a reboot, unlike
`mr_master_options.h`'s deliberately volatile `T:` controller->browser
snapshot this file sits alongside). Unlike that `T:` snapshot, this file
is also expected to survive a MintVID *upgrade*, and `mr_play_options`'s
layout has already changed more than once in this project's history (this
session's own TurboGT removal included) - a raw binary struct dump alone
can't tell "written by an older but layout-compatible build" from "written
by a build whose fields don't line up any more", so a small header (a
magic value plus the exact `sizeof(mr_play_options)` the writer was built
with) goes in front of the struct, and any mismatch on load - including a
short/missing file - is treated as "nothing saved yet" rather than risking
a partially-overlaid struct. Same crash-safe write-to-tmp-then-rename
idiom as `mr_last_dir.h`, plus a read-before-write skip when nothing
actually changed (this file is meant to be written on every option
change, not just on request).

Wired into both GUIs identically: `read_play_options()`/`read_options()`
- already the single choke point every option-change/Play/IPTV/YouTube
call path runs through to assemble a `mr_play_options` from the current
gadget states - calls `mr_saved_options_save()` once at the end, so
saving needs no new call sites anywhere. Loading happens once in each
GUI's window-building function, after every chooser's label list is
built (so the reverse-lookup helpers below have something to search) and
before any gadget is created: a new `mode_row()` in each file (alongside
the pre-existing `c2p_row()`) maps a saved `mr_display_mode` back to a
row index, but deliberately does not share `c2p_row()`'s "fall back to
row 0 when not found" behaviour: a C2P value missing from this session's
list falling back to row 0 (Standard, always available) is a tolerable
worst case, but the same fallback for display mode would force row 0
(AGA) over a legitimately-detected P96/WritePixel default, defeating the
priority fix above. `mode_row()` instead returns -1 when the saved mode
isn't in this session's list (e.g. saved on an RTG boot, loaded on a
plain-AGA one), and the caller leaves its own just-computed
hardware-detected `default_mode` untouched in that case. H.264 performance needs no
such lookup - its chooser rows are fixed and match the enum ordinals
directly, same as `read_play_options()`/`read_options()` already assume
when writing the struct back. Every resolved initial value flows into the
same `NewObject()`/`add_gadget()` tags (`mrgui.c`) or the same post-open
`GT_SetGadgetAttrs()`/`GTCY_Active` push (`mrgui_gadtools.c`) the
hardcoded defaults used before - no new gadget-attribute mechanism, just
a different source for the value already being set there. A restored
combination that isn't actually eligible this session (e.g. Copper 2x
with a display mode that doesn't support it) needs no special handling
either: `update_mode_controls(..., TRUE)`, already called once right
after the window opens to correct exactly this class of inconsistency
for a manual click, runs against whatever the gadgets' initial state
turns out to be, restored or hardcoded alike.

Deliberately not persisted: HLS/live fields (`hls_low`, `hls_max_width`/
`hls_max_height`, `live_resync`) - neither controller GUI has a widget for
any of them, so `read_play_options()`/`read_options()` never set them to
anything but `mr_play_options_default()`'s own values in the first place;
saving/restoring the whole struct is a no-op for those fields, not a risk
of clobbering something the IPTV/YouTube browsers manage separately
through their own, unrelated `T:` handoff path.

Both `amiga/mrgui.c` and `amiga/mrgui_gadtools.c` can only be reviewed,
not compiled or run, on this dev host (see "Validate against ffmpeg"
above); `make check` (host-buildable core, untouched by this change)
passes unchanged. Needs a real-hardware pass to confirm: the P96-first
default on an actual RTG boot, and that settings genuinely survive a
relaunch (and a reboot) in both GUI editions.

## 720p H.264 decode investigation (WinUAE)
A user report opening the next round of work: 720p H.264 on WinUAE
(68040, JIT, "full speed" - no artificial CPU throttling) decodes at a
consistent ~50% of the throughput needed for real time, both for a local
file and for YouTube (where it additionally buffers/freezes) - not
occasional stutter, a steady half-speed ceiling. That the local-file case
shows the identical ratio rules out anything HLS/live-fetch-specific
(buffering, live-resync, reconnect) as the cause: this is the core H.264
decode path itself, on this specific emulated setup.

**"JIT, full speed" changes what's plausible here versus every previous
68060-hardware performance note in this file.** WinUAE's JIT compiles 68k
code to native host instructions rather than interpreting it, so raw ALU
throughput should be very fast unless something forces a fallback to
interpretation for specific instruction sequences (self-modifying code,
certain addressing modes, chip-RAM access patterns) - a real, different
failure mode from "a real 68040/68060 is just slow at this," and one this
project has no way to confirm or measure directly: there is no WinUAE
instance on this dev host, and qemu-m68k (an interpreter itself, and
explicitly documented elsewhere in this file as a poor proxy for
cache/memory-bound behaviour) cannot stand in for a JIT's own instruction
coverage either. A suspiciously clean ~2x ratio, rather than a vaguer
"kind of slow," is also more consistent with something structural than
with simply needing more raw cycles.

Checked what could be confirmed from source alone before speculating
further: `Makefile.amiga`'s `CPU=68040` build already compiles with
`MR_M68K_ASM=1` and the 68040-class hand-tuned kernels
(`ih264_m68k_interp.S` etc., the same ones `MintVID040` - the documented
PiStorm/Emu68 recommendation - already uses), so this is not a case of
the wrong CPU tier or a missing asm path being silently selected for
68040 specifically.

**Added `M68K_ASM=0` to `Makefile.amiga`** (`make -f Makefile.amiga
mrplay CPU=68040 M68K_ASM=0`), mirroring the existing `STAGE_PROFILE`/
`CABAC_PROFILE` opt-in pattern: forces every `#if defined(MR_M68K_ASM)`
site in `core/`/`vendor/libavc_port/` onto its portable C path instead of
the hand-tuned `.S` kernel, on an otherwise normal build. This is a
diagnostic, not a fix - it directly tests the JIT-instruction-coverage
hypothesis above without needing WinUAE access from this dev host: if a
WinUAE run with `M68K_ASM=0` decodes at the *same* half-speed ratio, the
hand-tuned asm isn't the differentiator and the bottleneck is elsewhere
(CABAC/coefficient parsing, the scheduler, something codec-agnostic); if
it's reliably slower still, the asm is doing real work here as intended
and the investigation moves to *which* kernel and why it isn't buying
enough; if it's actually *faster*, that would directly confirm the asm is
JIT-hostile on this specific host. Verified the flag itself does what it
claims: `MR_M68K_ASM` is checked via `#if defined(...)` everywhere (not
`#if MR_M68K_ASM`), so `-DMR_M68K_ASM=0` would not have disabled anything
- confirmed with a grep across every call site - which is why
`M68K_ASM_FLAGS` omits the `-D` entirely rather than defining it to 0,
verified via `make -f Makefile.amiga -n mrplay CPU=68040
AMIGA_GCC=/fake/m68k-amigaos-gcc` dry-run output showing `-DMR_M68K_ASM=1`
present by default and absent under `M68K_ASM=0`. `make check` (host
build, `MR_M68K_ASM` never defined there either way) passes unchanged -
this only touches `Makefile.amiga`'s own build-line composition.

The other concrete next step, not yet taken since the user chose to
describe the symptom rather than gather it first: a
`STAGE_PROFILE=1 CABAC_PROFILE=1` capture (see the H.264 CABAC notes
section above) from the actual laggy WinUAE run, which would show
directly whether the ~2x cost is inside libavc's own reported
mc=/deblock=/recon=/intra=/bin=/coeff=/mvpred= breakdown (real algorithmic
cost, further profiling tells you where) or split between that and the
wall-clock vdecode=/libavc-core= gap (overhead in the wrapper/scheduler
outside libavc) - exactly the same unresolved "still open" question a
real-hardware 68060/50 report already raised earlier in this file, now
recurring on a completely different platform (JIT-emulated 68040), which
makes it more likely to be a real, codec/scheduler-level cost than
something specific to one CPU tier's silicon quirks.

## 720p H.264 decode: a real per-macroblock cost, not a decoder-scaling bug
Follow-up to the WinUAE 720p investigation above. The user's own steer once
the JIT/asm-hostility angle was on the table: don't chase whether WinUAE's
JIT accelerates the hand-tuned `.S` kernels well - "if WinUAE can't run our
ASM good, that's their issue" - instead look for a real, decoder-internal
cause. The `M68K_ASM=0` `Makefile.amiga` diagnostic from that earlier
section was reverted outright (the JIT-hostility question it existed to
answer was explicitly deprioritized, not investigated further) - CFLAGS is
back to a hardcoded `-DMR_M68K_ASM=1` with no build-time toggle.

**Two measurement pitfalls surfaced before any real finding, both worth
recording since they'd otherwise mislead a future profiling pass on this
codebase specifically.**

First: `tests/mr_decode.c`'s `--time` output is only representative of real
playback when paired with `--h264-yuv`. Without it, every profiled frame
pays `emit_rgb()`'s RGB24 conversion (`mr_yuv420_to_rgb24()` alone was 36%
of total instructions in an early host callgrind capture) - but
`amiga/mrplay.c` already routes essentially every real H.264 display path
(RTG CGX/P96, and the AGA RGB fallback) through `mr_h264_set_yuv_output()`
instead, exactly as this file's own "RGB24 round-trip is the expensive
part" note already established for MPEG-2. The test harness's default output
mode and real playback's actual output mode had quietly diverged; `--h264-yuv`
is the flag that puts them back in step, and any future host-side H.264
profiling on this codebase needs it or the numbers describe a cost nothing
in `mrplay.c` actually pays.

Second: the existing `STAGE_PROFILE`/`CABAC_PROFILE` clock()-based
per-primitive breakdown (mc/deblock/recon/intra, bin/coeff/mvpred) is not
trustworthy under qemu-m68k once macroblock count gets large. At 720p
(3600 MB/frame, vs. the existing fixture's 48 MB/frame), the reported
`mc_us + deblock_us + recon_us + intra_us` summed to roughly *13x* the
enclosing `core_us` span they are nested inside of - physically impossible
for a real measurement, since core_us wraps the exact call that contains
all of the others. Removing `CABAC_PROFILE` (dropping ~483,000 clock()
calls/decode) barely moved the numbers, ruling out CABAC's own call volume
as the cause. The real explanation: each `clock()` call is a real syscall
qemu-user has to trap, and that trap cost is roughly constant per call -
paid twice for the single core_us span, but paid once per MB-level
primitive call for the nested timers, so at 3600 MB/frame the nested sum is
dominated by profiling overhead, not real work. Comparing wall time with
instrumentation on vs. off confirmed this isn't free (76% more wall time,
mostly `sys` time) even though the *shape* of the distortion (13x) is far
worse than the wall-time-overhead ratio (1.75x) would suggest on its own.
Conclusion: this instrumentation is fine for small fixtures (the existing
128x96 clip) but unusable for judging *relative stage cost* at 720p-class
macroblock counts; a host callgrind profile (instruction-count based, no
per-call syscall trap) is the right tool instead, matching exactly how this
file's own H.264 CABAC notes section already characterized the CABAC/MV-
prediction split via "a real-hardware trace and a host callgrind profile."

**With both pitfalls avoided, two real, load-bearing findings came out of
qemu-m68k wall-clock timing and host callgrind profiling of a generated
1280x720 clip (ad hoc, not checked in - same "not committed" precedent as
the earlier `mr_h264_set_dynamic_skip()` host probes in this file):**

1. **No algorithmic complexity blow-up.** Real (non-instrumented) qemu-m68k
   wall time across four resolutions (320x240 through 1280x720, same
   encode settings) fits a clean `total_us ≈ 3.18ms fixed + 15.8us/MB`
   linear model (predicted vs. actual differ by well under 1ms across all
   four points) - macroblock count, not pixel count non-linearly, is what
   decode cost tracks, exactly as a per-macroblock pipeline should scale.
   Per-MB cost actually *drops slightly* at 720p vs. the tiny existing
   fixture (fixed per-frame overhead amortizing over more MBs), the
   opposite of what a superlinear "decoder limitation" would look like.
   This directly answers "are we hitting a limitation of the decoder" in
   the complexity sense: no.

2. **A real, previously undocumented inefficiency: explicit weighted
   prediction is dispatched from the PPS capability bit, not from whether
   the slice's actual signalled weights are non-default.**
   `ih264d_inter_pred.c` sets `u1_wght_pred_type` for P/SP slices straight
   from `ps_cur_pps->u1_wted_pred_flag`, and for B slices from
   `u1_wted_bipred_idc` - both are per-PPS/per-slice-type capability
   flags, set once by the encoder for the whole stream, saying "this
   stream *may* signal explicit per-reference weights," not "this
   particular slice's weights are actually non-default." A host callgrind
   profile of the generated 720p clip (`--h264-yuv`, matching real
   playback - see above) showed `ih264_weighted_pred_luma`/`_chroma`
   consuming **~31% of total decode instructions even with zero B-frames**
   - yet x264's own encoder log for that exact clip reported
   `Weighted P-Frames: Y:0.0% UV:0.0%`: the encoder never used a non-
   default weight, but every P-slice inter macroblock still paid the
   expensive explicit-weight multiply/round/clip path regardless, because
   `weighted_pred_flag=1` is x264's own default for High-profile P-slices
   (`weightp=2`, "smart" analysis) independent of whether any block ends
   up using a non-trivial weight. High profile is the most common H.264
   profile in real broadcast/streaming encodes, so this is very unlikely
   to be specific to the synthetic test clip.

   **Fixed in the `vendor/libavc` fork** (`boingball/libavc`, branch
   `claude/weighted-pred-trivial-skip`, commit `cb8d7c3` - kept off
   `main` deliberately: `main` had independently diverged with a large,
   unrelated upstream ARM/encoder/mem_fns sync in the time between
   branching and finishing this fix, and merging into it would have
   pulled in changes never validated against this project's own patches
   - the parent repo's submodule gitlink points straight at the fix
   commit instead of at `main`, which is a normal, fully-supported way to
   pin a submodule; later merged into `main` - see "Baseline H.264
   profile" below). `ih264d_parse_pred_weight_table()` (the function that
   implements `pred_weight_table()` of spec section 7.3.3.2, called
   exactly when `weighted_pred_flag`/`weighted_bipred_idc==1` requires it)
   now computes, once per slice, whether every parsed luma/chroma
   weight/offset across every active reference in every list equals the
   implicit default (`weight == 1<<log2_denom`, `offset == 0` - the same
   packed representation the parser already uses for both the explicit and
   implicit-default cases, so one integer comparison per reference covers
   both) and stores the result as a new `dec_slice_params_t` field,
   `u1_wts_ofst_trivial`. `ih264d_inter_pred.c`'s dispatch downgrades
   `u1_wght_pred_type` to 0 (the cheap default/unweighted path, which
   already exists and is already used for the genuinely-unweighted case)
   whenever that flag is set - for P/SP slices unconditionally, and for B
   slices only when `weighted_bipred_idc==1` (explicit). Implicit weighted
   bi-prediction (`idc==2`) is deliberately untouched: those weights are
   derived per-MB from POC distance, never come through
   `pred_weight_table()` at all, and can be genuinely non-trivial frame to
   frame - `u1_wts_ofst_trivial` doesn't apply to it and the existing
   `idc==2` handling is unchanged. The flag can never cause an incorrect
   downgrade from a stale previous value: `pred_weight_table()` is
   unconditionally re-parsed (and the flag freshly recomputed) on every
   single slice where the PPS/idc condition would otherwise set a nonzero
   `u1_wght_pred_type` in the first place, so by the time the flag is ever
   consulted it always reflects the current slice.

   Verified bit-exact three ways before trusting it: `make check` (host,
   full ffmpeg-oracle suite, unchanged worst-frame MAE on every existing
   H.264 fixture) and `make check-m68k` (real m68k/big-endian under qemu,
   both the plain and `MR_H264_CABAC_PROFILE=1` builds, unchanged
   worst-frame MAE) both pass; a direct before/after stash-and-rebuild
   comparison decoded the generated 720p clip to PPM with the fix
   reverted and re-applied and diffed the two output directories
   byte-for-byte - identical. The actual win: a re-profile of the fixed
   decoder shows `ih264_weighted_pred_luma`/`_chroma` gone entirely from
   the callgrind top-functions list, **31% fewer total instructions** on
   the no-B-frame 720p clip and **12% fewer** on the B-frame one (whose
   dominant weighted-bipred cost there is the untouched `idc==2` implicit
   case, not the explicit one this fix addresses).

   Not yet confirmed: the real-hardware/WinUAE speedup this predicts.
   Every number above is host-instruction-count or qemu-m68k wall-clock,
   proving the fix is correct and that it removes real, measured work -
   not a 68040/68060 timing claim, per this file's standing qemu-vs-
   hardware caveat. The original WinUAE "50% of real-time decoding power"
   report used YouTube Live content, whose actual encoder settings
   (profile, `weightp`) are unknown from here; if that stream's PPS
   doesn't set `weighted_pred_flag`/`weighted_bipred_idc==1` at all, this
   fix buys it nothing, though High profile with default `weightp`
   settings (as used here) is a common enough encoder default that it is
   a reasonable first thing to have fixed.

## Local-disk video queue growth (real A1200/68060 report)
A real-hardware report on the 720p work above's branch, on a genuinely
different clip than the 720p one: a local H.264 mp4 on an A1200 68060/50
plays perfectly for the first ~10 frames (a different clip: ~3 seconds),
then settles into a visible "decode, present, decode, present" stepping
pattern that never recovers. The user's own read of it, and the right one:
this is exactly the shape a too-small decode-ahead buffer produces once it
drains, not a hard freeze or a network symptom (this is a local file, no
fetch involved at all).

`amiga/mrplay.c` already has the mechanism this needed - it just wasn't
applied evenly. `video_cap` (the modulus of the `queued_video vq[]` ring -
see the file's own top-of-file comment block) starts at a small default
per source kind (`VIDEO_QUEUE_NET_DEPTH`/`VIDEO_QUEUE_DISK_DEPTH`, both 16)
and decode already races ahead to fill it whenever `qcount < video_cap`
(`queue_full = qcount >= video_cap` only gates *output* - see
`skip_reason_queue_full` - it does not pause decode itself), so the ring
banks a free head start before playback visibly needs it and keeps
refilling in the background afterward. But the RAM-budget growth logic
that lets that ring grow past its 16-frame default when the machine has
room to spare (`budget_frames`, derived from `AvailMem(MEMF_ANY)` minus a
floor, clamped to `VIDEO_QUEUE_CAP`=48) was gated `if (network_source &&
...)` - added for a real, different problem (live HLS segment fetch
stalls, see the Live HLS notes above) but never extended to local disk
files, which stayed hardcoded at 16 forever regardless of free Fast RAM.
16 frames is well under a second of cushion at any real frame rate - a
plausible match for "perfect for a few seconds, then permanent stutter"
once a clip's average per-frame decode cost sits at or a little past one
frame period on a real 68060/50 and that thin cushion drains.

Fixed by dropping the `network_source &&` gate - the same `budget_frames`/
`VIDEO_QUEUE_CAP` growth now applies to disk sources too, with the same
safety clamps already in place (a RAM-tight machine still gets clamped
back down; `video_cap` still cannot exceed the fixed 48-slot `vq[]` array).
This is a pure sizing change - the ring's actual fill/drain/present logic,
`target_depth` (still 3 for local files, governing only when playback
*starts*, not how much can bank ahead of it), and every skip/throughput
decision elsewhere in the file are all untouched. Per-slot RGB/indexed/
YUV buffers are allocated lazily per slot index the first time the ring
actually reaches it (`realloc` in the handful of `q->rgb = ...` sites),
not up front for all 48 array slots, so this costs no memory on a stream
that never needs the extra depth - it only grows RAM use on a machine that
both has the room (`budget_frames`) and a clip that actually drains the
ring that far.

**This cannot fix a clip whose *average* decode cost is steadily below
real time - only variance and a one-time startup shortfall.** A ring
buffer trades time, not work: it banks a surplus while decode is briefly
ahead of the display clock and spends it back during a briefly-behind
stretch, but if the *long-run average* decode rate never catches back up
to real time, the surplus can only shrink, never regrow, so any fixed
buffer size just delays the point where the same steady stutter resumes,
never removes it. Worth confirming which case this clip is with a
`--time` trace (the existing `video-queue: cap=... cushion=...` printf,
already `want_time`-gated, now reports the grown number) rather than
assuming - if the stutter recurs at a proportionally later point rather
than going away, that is itself the evidence this is the steady-average
case, not the variance case, and the real fix is decode-side (per-frame
profiling, as the 720p section above already does for a different
symptom), not more buffer.

**A true "decode the whole file ahead, like MintAMP's own decode-then-play
audio mode" was raised and is not feasible for video on this hardware -
the RAM math rules it out outright, not a design taste call.** MintAMP's
audio equivalent works because PCM is tiny (16-bit 44.1kHz stereo is
~172 KiB/s, so a whole 4-minute track is ~41 MB - a real, affordable
prebuffer). A decoded *video* frame is the queue's own `frame_bytes` -
width*height for the AGA indexed path, ~1.5x that for YUV420-indexed,
3x that for RGB24/RTG - and a 4-minute 360p (640x360) clip at a real H.264
frame rate is thousands of frames: at 25fps (6000 frames) that is roughly
1.3 GB indexed, 1.9 GB YUV420-indexed, or 3.9 GB RGB24, held as raw pixels
simultaneously; even at this project's own lower 12fps test-clip rate
(2880 frames) that is still ~630 MB / ~950 MB / ~1.85 GB respectively -
one to several orders of magnitude past any real A1200's Fast RAM, expanded
or not. The now-larger *rolling* buffer above is the actually-tractable
version of the same instinct: it banks tens of frames (a few seconds,
bounded by real free RAM via the same `budget_frames` math this section's
fix reuses) rather than the whole file, needs no "please wait, decoding"
startup phase or status-bar notification before the window opens (playback
already starts as soon as `qcount > 0` - the very first decoded frame -
and the ring keeps filling in the background from there, exactly as it
already did before this change, just deeper), and costs proportionally
bounded RAM instead of gigabytes.

Not yet retested on real hardware - `amiga/mrplay.c` is Amiga-only and can
only be reviewed on this dev host (see "Validate against ffmpeg" above);
`make check`/`make check-m68k` are unaffected (neither touches this file).
Needs the same clip that showed the original report, watching whether the
smooth opening stretch measurably lengthens (confirms the ring is now
banking more, whatever the eventual verdict on steady-vs-variance above)
and whether `--time`'s `video-queue: cap=...` line now reports a
meaningfully larger `cap=` than 16 on this machine's actual free RAM.

## Default CPU=68060 mrplay build never actually linked MintAMP's polyphase asm
A real toolchain hit on the first actual attempt to link `mrplay` for
CPU=68060 with the default `ASM60_GROUPS`, while gathering an
`STAGE_PROFILE=1 CABAC_PROFILE=1` trace for the 720p/Turbo investigation
above: `undefined reference to AmigaM68KPolyphaseMonoFast`/
`MonoFastPolyphaseStride4_Amiga_m68k`/dozens more, all from
`build/vendor/MintAMP/real/polyphase.o` at the final link.

Root cause, found by reading `Makefile.amiga`'s own `ASM60_GROUPS` table
rather than guessing: `ASM60_FLAGS_poly060`/`ASM60_FLAGS_lowrate060` both
set `-DAMIGA_M68K_POLYPHASE_68060`, which makes MintAMP's `real/polyphase.c`
call straight into the hand-asm symbols `real/amiga_m68k_polyphase.S`
defines - the exact same `.S` file the separate `asm_polyphase` group's own
`ASM60_SRC_asm_polyphase` entry already points at. But `poly060`/
`lowrate060` never had a matching `ASM60_SRC_poly060`/`ASM60_SRC_lowrate060`
entry, so `MINTAMP_ASM_SOURCES` (built only from the groups actually present
in `ASM60_GROUPS`) never pulled that file in for the *default*
`ASM60_GROUPS ?= lowrate060 huffman midside planars8`. The C dispatch code
that calls those symbols was correctly compiled in and reachable - the
symbols it calls just never existed in the link. `huffman`/`midside`
(also in the default set) don't need a source-list entry at all - they're
inline asm inside plain `.c` files already on the normal source list, not a
separate `.S` - so the gap was specific to the polyphase-family groups, and
only the one group (`asm_polyphase`) that happens not to be in the default
selection was ever correctly wired.

This means every default-flags `CPU=68060 mrplay`/`mrplay` release build
was always going to fail this exact link, on any real `m68k-amigaos-gcc`
toolchain - it simply hadn't been attempted on one until now. Exactly the
class of gap this file's "Validate against ffmpeg" section exists to name:
`make -f Makefile.amiga -n mrplay CPU=68060 AMIGA_GCC=/fake/...` dry-runs
elsewhere in this file's history checked *which flags* land on the compile
line, but nothing before this had checked whether the *source file list*
computed from `ASM60_GROUPS` was actually complete - qemu/ELF can't catch
this either, since it's a link-time source-selection gap in this Makefile,
not an instruction-safety or bit-exactness question `check-m68k` covers.

Fixed by adding the two missing entries (`ASM60_SRC_poly060`/
`ASM60_SRC_lowrate060 := $(MINTAMP_ROOT)/real/amiga_m68k_polyphase.S`,
mirroring `ASM60_SRC_asm_polyphase`'s existing line). Verified with a
before/after dry-run diff (`make -f Makefile.amiga -n mrplay CPU=68060
AMIGA_GCC=/fake/m68k-amigaos-gcc`, grepping for `amiga_m68k_polyphase.S` on
the resulting compile/link line): 0 occurrences before this fix, 1 after,
for the exact default `ASM60_GROUPS` a plain `CPU=68060` build uses;
`CPU=68030` (the separate `MINTAMP_ASM_SOURCES_FULL030` path, untouched by
this table at all) still shows 1 either way, confirming the fix is scoped
to the 68060 branch and changes nothing for 68030/040. Not yet confirmed
by an actual completed real link - the user is rebuilding with this fix on
their own `m68k-amigaos-gcc 13.2.0` toolchain now.

## Real-hardware STAGE_PROFILE/CABAC_PROFILE capture: YouTube 360p, Turbo, A1200 68060/50
The first real payoff from the `ASM60_SRC_poly060`/`lowrate060` link fix
above: a GadTools "Log: on" capture (`RAM:MintVID.log`, `mrplay` rebuilt
`CPU=68060 STAGE_PROFILE=1 CABAC_PROFILE=1`) of a real YouTube 360p
(640x360, progressive MP4, H.264/AAC) session, Turbo performance mode,
92 real decoded frames over the capture. This is the actual real-hardware
data the "still open" wall-clock-vs-libavc-core question (raised twice
earlier in this file - the 68060/50 live-TS 256x144 case, and the WinUAE
720p case) had been waiting on.

**Finding 1: the wall-clock-vs-libavc-core gap from the earlier live-TS
report does not reproduce here - `core` now tracks `vdecode` almost
exactly.** Across every sampled report in the capture, `libavc-core`'s own
self-reported average is 98-99% of `vdecode`'s wall-clock average (e.g.
one representative sample: `vdecode=968.41 ms`, `libavc-core=956.333 ms`;
another: `vdecode=1165.56 ms`, `core=1152.5 ms`) - `input`/`rgb-output`
are both ~0. So for this stream/path, essentially all of the wall-clock
decode time really is inside libavc's own reported cost, not lost in the
wrapper/scheduler around it the way the earlier 5-10x-gap live-TS report
showed. That mystery either doesn't apply to this progressive-MP4 path or
was specific to that other stream's demux/reassembly shape - it is not a
general property of this target.

**Finding 2: within libavc's own reported cost, the single largest bucket
is the one that has never been directly measured - the un-instrumented
macroblock-header/syntax-element parsing dispatch the H.264 CABAC notes
section above already named (`pf_parse_inter_mb`, the same-file function-
pointer-assignment case `--wrap` cannot intercept) - and it is not a small
remainder, it is roughly half of total decode time.** Computing
`core - (mc+deblock+recon+intra) - (bin+coeff+mvpred)` from several
representative samples: 956.3-(119.3+0+111.7+39.0)-(82.0+52.0+26.7) =
525.7 ms (55% of core); 1152.5-(199.0+0+118.0+32.0)-(81.5+72.0+34.0) =
616.0 ms (53%); 1057.5-(185.0+0+103.5+50.5)-(81.0+50.5+31.0) = 556.0 ms
(53%) - consistently 52-55% across the capture, bigger than mc+deblock+
recon+intra combined (~27-30% of core) and bigger than bin+coeff+mvpred
combined (~15-18%). `deblock` reads exactly 0 us in every single report -
direct confirmation that Turbo's all-or-nothing `i4_degrade_pics=4` policy
(see the H.264 TurboGT retirement section above) really is disabling
deblocking for every frame on this real target, not just in theory.

**Caveat that has to be stated before either finding above gets used to
justify real work: this capture pays for both `MR_H264_STAGE_PROFILE` and
`MR_H264_CABAC_PROFILE` at once, and the CABAC wrapper overhead question
earlier in this file is not hypothetical - it is exactly what
`bin_count`/`coeff_count`/`mvpred_count` here show paying for, at real
volume.** `bin_count` alone runs 3,450-8,196 calls *per single decoded
frame* in this capture (coeff_count 1,400-3,700, mvpred_count 350-2,600) -
each a `clock()`-bracketed call under `MR_H264_CABAC_PROFILE`, and mc/
deblock/recon/intra are separately wrapped under `MR_H264_STAGE_PROFILE`.
None of that instrumentation cost is free on real hardware, even without
qemu's syscall-trap-specific inflation (see the qemu-vs-hardware note at
the top of this file) - a `ReadEClock()`-class timer read still costs real
cycles, tens of thousands of times per frame. So the *proportions* above
(core tracks vdecode; the syntax-dispatch remainder dominates within core)
are trustworthy, structural findings, but the *absolute* numbers in this
capture - `vdecode` averaging ~1000-1200 ms/frame, `decoded=0.45-0.70 fps`
throughout the session, the final `timing/92 frames: decode=101817 ms`
summary (1106.7 ms/frame average, cross-checking the per-report samples
closely) - almost certainly overstate how slow the real, non-instrumented
production `mrplay` is on this same clip. Whether that gap is small or
large is itself unmeasured here; a plain `--time`-only capture (no
`STAGE_PROFILE`/`CABAC_PROFILE`) on the identical clip/settings is the
natural next real-hardware data point, to separate "how slow is decode"
from "where does decode time go" instead of conflating them in one
capture.

Not yet acted on: the syntax-dispatch bucket dominating decode time on
real hardware is new information the H.264 CABAC notes section's own
closing line ("[reimplementing the dispatcher] is not justified just to
add a diagnostic counter") was written without - now that it is
structurally the largest cost, not a small unattributed remainder,
whether it is worth reaching for direct measurement (or a real
optimisation) is an open question for the next round of this
investigation, not decided here.

## A fourth CABAC-profile bucket after all: per-MB neighbour-info setup (`mbinfo_us`)
Direct follow-up to the ~52-55% unattributed remainder found in the real
A1200 capture above. The CABAC notes section's own "no fourth macroblock
parsing bucket" reasoning is about one specific function pointer,
`pf_parse_inter_mb` (the mb_type/cbp/ref_idx/mvd/intra-mode/mb_qp_delta
syntax dispatch, assigned to `ih264d_parse_pmb_cabac()`/
`ih264d_parse_bmb_cabac()` in the *same* file that defines them, which is
exactly why `--wrap` cannot intercept it) - not a blanket claim that every
remaining per-MB function pointer is equally unreachable. Reading
`ih264d_parse_pslice.c`'s main per-MB loop
(`ih264d_parse_pslice_data_cabac()`) turned up a second, structurally
different function pointer sitting right next to it:
`dec_struct_t::pf_get_mb_info`, called once for *every* macroblock in a
slice - skip or not, unlike `pf_parse_inter_mb` which only runs for
non-skip MBs - to compute neighbour availability and CABAC context
pointers before the syntax dispatch even begins (`ih264d_get_mb_info_
cabac_nonmbaff()` in `ih264d_mb_utils.c`).

The key difference from `pf_parse_inter_mb`: `pf_get_mb_info` is
*assigned* in `ih264d_parse_pslice.c`/`_islice.c`/`_bslice.c`, but
*defined* in the separate `ih264d_mb_utils.c` - a genuine cross-object
relocation, the same shape that already lets `--wrap` work for
`ih264d_decode_bin`/`ih264d_mvpred_nonmbaff`/
`ih264d_parse_residual4x4_cabac`, not the same-file case `pf_parse_
inter_mb` fails on. Confirmed by grep before touching anything, not
assumed from the general pattern. Only the non-MBAFF CABAC variant is
wrapped (`ih264d_get_mb_info_cabac_nonmbaff`) - this project has no MBAFF
test content and no CAVLC fixture, mirroring `ih264d_mvpred_dispatch_
port.c`'s own precedent for leaving MBAFF alone.

`vendor/libavc_port/ih264d_mbinfo_wrap_port.c` is new, and deliberately
the simplest possible wrap in this whole family: a pure timing pass-
through via GNU ld's `__real_ih264d_get_mb_info_cabac_nonmbaff` symbol
(automatically defined for any `--wrap=X` target), not a reimplementation.
Every other `--wrap` site in this port exists to swap in an m68k asm
primitive and picked up timing as a side benefit; this one has no asm
behind it at all - there is nothing to gain from wrapping `pf_get_mb_info`
outside of measuring it, so `libavc.mk`'s `LIBAVC_M68K_LDFLAGS` only adds
`-Wl,--wrap=ih264d_get_mb_info_cabac_nonmbaff` when `CABAC_PROFILE=1` is
what defined `MR_H264_CABAC_PROFILE` in the first place, unlike the
always-on wraps for bin/mvpred/coeff/update_qp. A normal playback build
never links this symbol at all - zero cost, not even an extra call/return,
matching this file's own repeated caution (the reverted `__wrap_
ih264d_decode_bin` C-trampoline saga) about not paying for an unwanted
call layer in production. Verified with a before/after `Makefile.amiga -n`
dry-run diff: the `--wrap` flag is present exactly once under
`CABAC_PROFILE=1` and absent entirely by default.

New `mbinfo_us`/`mbinfo_count` bucket wired through the same path as
bin/coeff/mvpred: `ih264d_cabac_profile.h`/`.c` (fourth accumulator),
`core/mr_h264.h`/`.c` (`mr_h264_timing`, accumulated in the same
`s->timing_enabled` block as the other three), `amiga/mrplay.c`
(`playback_stats.h264_mbinfo_us/count`, accumulated alongside the other
three, added to the `"h264 cabac:"` printf line). One small, honestly
documented overlap (see `ih264d_cabac_profile.h`'s updated header): when
the current MB is a P/B-skip run, `ih264d_get_mb_info_cabac_nonmbaff()`
decodes the one `mb_skip_flag` CABAC bin inline - already counted under
`bin_us` too - so `mbinfo_us` is not perfectly disjoint from `bin_us` the
way bin/coeff/mvpred are from each other and from it. One bin's cost is
negligible next to the rest of the function, so this does not meaningfully
inflate the reported total, but it is a real, small double-count worth
stating rather than silently claiming perfect additivity.

No new bit-exactness test was written, because there is nothing new to
prove bit-exact: the wrap changes no behaviour by construction (a pass-
through to the real, unmodified vendored function, not a rewrite).
Correctness of the *wiring* - the wrap fires, with the real function's
return value and every side effect on `ps_dec`/`ps_cur_mb_info` intact -
is exactly what the existing `mr_decode_cabac_profile.m68k` conformance
run already proves: `tests/run_m68k_check.sh` now also links
`-Wl,--wrap=ih264d_get_mb_info_cabac_nonmbaff` into that one build
(`vendor/libavc_port/ih264d_mbinfo_wrap_port.c` added to its `LIBAVC_SRC`
list too), and the H.264 High Profile fixture decoded through it at
worst-frame MAE=0.705 - identical to the same clip decoded through the
default (non-profiling) build with no `--wrap` on this symbol at all.
`make check` (host, where `MR_H264_CABAC_PROFILE` is never defined and
the new file compiles to an empty translation unit, same as every other
port file guarded this way) passes unchanged. `tests/check_m68060_asm.sh`
also builds and scans this file alongside the rest of `vendor/libavc_port`
at real production flags (where it is empty and contributes nothing to
scan, since `CABAC_PROFILE` is off there) - added for consistency with
every other port file in that list, not because it currently has anything
to check.

Not yet done: an actual real-hardware `CABAC_PROFILE=1 STAGE_PROFILE=1`
retest with this new bucket, to see how large `mbinfo_us` actually is
against the ~52-55% remainder the previous A1200 capture measured before
this bucket existed - the whole point of adding it. If `mbinfo_us` turns
out to explain most of that remainder, `ih264d_get_mb_info_cabac_
nonmbaff()`'s own ~90-line body (read in full while tracing this - see
`ih264d_mb_utils.c`) is portable C with no obvious wasted work at a glance
(neighbour-mask arithmetic, a few pointer/struct-field writes, one
conditional CABAC bin for skip runs) - a real optimisation there, if one
exists, is a separate follow-up from this measurement change, not
something to guess at without the retest's numbers in hand.

## mbinfo_us retest: real but modest - the syntax dispatch is still the dominant unmeasured cost
The real-hardware retest the previous section asked for: a fresh
`CPU=68060 STAGE_PROFILE=1 CABAC_PROFILE=1` GadTools Log capture, same
setup as before (YouTube 360p progressive MP4, Turbo, A1200 68060/50,
this time 114 decoded frames / 54 paired `h264 stages:`+`h264 cabac:`
reports - a larger sample than the earlier 3-sample-by-hand estimate).
Parsed and averaged all 54 reports (not eyeballed) as a fraction of
`libavc-core`:

| bucket | avg % of core | range |
|---|---|---|
| mc | 13.8% | 0.6-21.2% |
| recon | 11.2% | 6.5-23.8% |
| intra | 3.9% | 0.3-12.7% |
| deblock | 0.0% | (Turbo disables it - unchanged from before) |
| bin | 7.2% | 3.3-8.7% |
| coeff | 5.4% | 4.3-7.8% |
| mvpred | 2.4% | 0.1-4.1% |
| **mbinfo** | **8.2%** | **4.1-9.7%** |
| **remainder** | **47.8%** | **44.3-51.4%** |

`mbinfo_us` is real - a consistent ~8% of total decode time, comparable in
size to `bin_us` and bigger than `mvpred_us` - so `pf_get_mb_info` was
genuinely worth measuring, not a rounding error. But it does not explain
the earlier ~52-55% remainder the way the "if `mbinfo_us` turns out to
explain most of that remainder" note above was hedging: carving ~8 points
out of that ~55% (the same reports, same clip, same performance mode)
leaves the remainder at ~48% - `55 - 8 ≈ 48`, exactly consistent with
`mbinfo_us` being newly-separated-out from what used to be lumped into the
remainder, not with it having been most of that remainder. `pf_parse_
inter_mb` - the mb_type/cbp/ref_idx/mvd/intra-mode/mb_qp_delta syntax
dispatch itself, still the one genuinely unwrap-able function pointer in
this whole chain (same-file assignment, see the CABAC notes section above)
- remains the single largest cost in H.264 decode on this real target by a
wide margin: bigger than mc+recon+intra combined (~29%), bigger than
bin+coeff+mvpred+mbinfo combined (~23%), and roughly double the next
largest named bucket (`mc` at 13.8%).

This closes out the "if mbinfo turns out to explain most of that
remainder" branch from the previous section with a real answer (no, not
most of it) rather than leaving it open, and reconfirms - now with actual
per-bucket real-hardware proportions instead of a single unattributed
number - the CABAC notes section's own conclusion that reaching for direct
measurement of `pf_parse_inter_mb` itself (which needs reimplementing the
~200-line dispatcher, the same shape of fix already applied for `--wrap`-
reachable functions like `ih264d_mvpred_dispatch_port.c`) is the only way
to attribute the remaining ~48% further, not something derivable from
wrapping more adjacent function pointers - `pf_get_mb_info` was the one
other genuinely wrap-able per-MB pointer in the whole call graph, and it
has now been tried.

Also visible in this capture, unrelated to the mbinfo question but worth
recording: `decoded`/`presented` stayed at 0.5-0.9 fps throughout (target
25 fps) and `hw-starvations` climbed to 143 by the session's end (114
frames, `timing/114 frames: decode=132208 ms` - 1160 ms/frame average) -
this specific test run was plain Turbo with no Skip Frames/dynamic-skip
and no `--throughput` override visible in the log, so it is not a
regression report, just confirmation that a 360p YouTube stream is still
nowhere near real-time on this target under Turbo alone, consistent with
every other 360p/A1200 capture already on record in this file.

## Reaching pf_parse_inter_mb after all: a struct-field swap, not `--wrap`
Direct follow-up to "the syntax dispatch is still the dominant unmeasured
cost" above - the user's own call once that was clear: reimplement
`ih264d_parse_pmb_cabac()`/`ih264d_parse_bmb_cabac()` and go after the
real number, not just the derived one.

`--wrap` was re-confirmed a dead end before trying anything else, this
time by actually enumerating every reference to both symbols across the
whole vendored tree (`grep -rn`), not just re-trusting the earlier finding
by assumption: both functions are declared in `ih264d_parse_islice.h` and
referenced from `vendor/libavc/decoder/mvc/`/`svc/` (unused - `LIBAVC_
DECODER`'s wildcard is non-recursive, those subdirectories are never
compiled here), but every reference in the *actual* compiled source is
exactly the same-file shape already established: defined and assigned to
`ps_dec->pf_parse_inter_mb` both inside `ih264d_parse_pslice.c` (for
`ih264d_parse_pmb_cabac`) and both inside `ih264d_parse_bslice.c` (for
`ih264d_parse_bmb_cabac`). A tempting-looking escape hatch was checked and
rejected: `ih264d_parse_inter_slice_data_cabac` (the per-MB loop function
one level up, which is what actually calls `pf_parse_inter_mb`) *is*
referenced cross-file - defined in `ih264d_parse_pslice.c` but also
assigned to `ps_dec->pf_parse_inter_slice` from `ih264d_parse_bslice.c` -
but P slices assign it from *within* `ih264d_parse_pslice.c` itself (same
file, same dead end), and P slices are the dominant, non-skipped cost
under Turbo (B is skip-decoded - see the H.264 CABAC notes section), so
even a successful wrap there would have measured nothing for the case that
actually matters.

A force-included preprocessor rename (`vendor/libavc_port/compat.h`
already does exactly this for one libc symbol - `#define strnlen
mr_libavc_strnlen` - applied via `-include compat.h` on every `LIBAVC_SRC`
file) was considered and rejected too, for a sharper reason than "seems
risky": it cannot work *in principle* for this specific case. A `#define
ih264d_parse_pmb_cabac ih264d_parse_pmb_cabac_vendored` would rename
*every* textual occurrence of that identifier in the whole translation
unit uniformly - both the definition *and* the same-file assignment that
was supposed to keep pointing at "the real one" so our replacement could
take over the public name. There is no way to select "rename only the
definition, leave that one other reference alone" via a macro that's
already active for the entire file by the time either line is
preprocessed, without editing `ih264d_parse_pslice.c` itself to `#undef`
between them - forbidden, and pointless anyway since the same problem
would recur for `ih264d_parse_bmb_cabac` in `ih264d_parse_bslice.c`.

**The mechanism that actually works reaches in from outside the same-file
relocation problem entirely, using infrastructure already built for
`mbinfo_us`.** `__wrap_ih264d_get_mb_info_cabac_nonmbaff()` (`ih264d_
mbinfo_wrap_port.c`) already runs, with a live `dec_struct_t*`, on *every*
macroblock - skip or not - strictly *before* that same macroblock's
`ps_dec->pf_parse_inter_mb` is looked up and called (confirmed by reading
the per-MB loop in `ih264d_parse_pslice_data_cabac()`: `pf_get_mb_info()`
runs first each iteration, `pf_parse_inter_mb()` - for non-skip MBs only -
after). So instead of trying to intercept libavc's own address-of
assignment, the mbinfo wrapper now *also* does a plain C struct-field
swap, one step later: capture whatever real function `ps_dec->pf_parse_
inter_mb` currently holds (freshly reassigned by every slice's own header
parse - P slices get `ih264d_parse_pmb_cabac`, B get `ih264d_parse_bmb_
cabac`) into a static, then overwrite the field with a local timing
wrapper. No linker trick, no relocation, no vendored-file edit - just a
normal write to a struct field the vendored code itself exposes and
reassigns. The very next call through that field - this MB or a later one
in the same slice - runs the wrapper, times the call through to whatever
was captured, and is otherwise fully transparent.

Idempotent by construction: the swap only fires when `ps_dec->pf_parse_
inter_mb != mr_wrap_parse_inter_mb` - true exactly once per slice boundary
(a fresh real P/B assignment), false for every other MB in that same
slice (the field already holds the wrapper, so nothing is rewritten,
and there is no risk of the wrapper capturing *itself* and recursing).
Handles P/B intermixing and any number of slices per picture for free,
since each slice's own header parse naturally re-triggers the capture on
the very next macroblock. Only the non-MBAFF CABAC path is covered -
same "no MBAFF/CAVLC test content, no verified primitive to compare
against" precedent `ih264d_mvpred_dispatch_port.c` already set - so a
non-CABAC or MBAFF slice's `pf_parse_inter_mb` runs completely unwrapped
and unmeasured, never incorrectly.

New `mbparse_us`/`mbparse_count` bucket (`ih264d_cabac_profile.h`/`.c`,
`core/mr_h264.h`/`.c`, `amiga/mrplay.c`'s `"h264 cabac:"` line) is
deliberately documented as *not* a clean fifth additive bucket the way
bin/coeff/mvpred/mbinfo are: `pf_parse_inter_mb` itself calls `ih264d_
decode_bin()` (bin_us), `ih264d_parse_residual4x4_cabac()` (coeff_us) and
the mv-predictor dispatch (mvpred_us), so `mbparse_us` necessarily
contains all three, on top of whatever previously-unmeasured C-level glue
exists between them (the partition loops, sub_mb_type/ref_idx/CBP/
transform8x8-flag/mb_qp_delta bookkeeping `ih264d_parse_pmb_cabac()`'s own
body is full of - see the H.264 CABAC notes section's summary of that
function). That overlap is the entire point, not a flaw: `mbparse_us`,
summed over a frame, is a *direct* wall-clock measurement of the same
quantity `core_us - mc - deblock - recon - intra - bin - coeff - mvpred -
mbinfo` has only ever been able to *derive* by subtraction. A real
hardware capture with this wired in can finally settle whether that ~48%
remainder genuinely *is* `pf_parse_inter_mb` (`mbparse_us` tracks it
closely) or there is further, still-unattributed cost beyond it
(`mbparse_us` reads meaningfully smaller).

No new bit-exactness test, for the same reason `mbinfo_us` needed none:
the swap changes no decode behaviour by construction - it is a captured
function pointer called through unchanged, not a reimplementation of the
parsing logic itself, so there is nothing new to prove bit-exact. What
*is* worth verifying is that the wiring doesn't break anything, and it
doesn't: `make check` (host, `MR_H264_CABAC_PROFILE` never defined there)
passes unchanged, and `tests/run_m68k_check.sh`'s existing `mr_decode_
cabac_profile.m68k` build (the one CABAC_PROFILE=1 build in that suite)
decodes the H.264 High Profile fixture through the *entire* mbinfo+
mbparse machinery active at once and matches the same worst-frame MAE as
every other build - the swap logic runs on real m68k/big-endian for every
single macroblock of a real multi-slice-capable decode and changes
nothing about the output.

Real "glory" - a concrete fix inside `ih264d_parse_pmb_cabac()`/`ih264d_
parse_bmb_cabac()`, or confirmation that the whole ~48% really is that one
function and nothing more - still needs a real-hardware `CABAC_PROFILE=1
STAGE_PROFILE=1` capture with `mbparse_us` in it, the same way `mbinfo_us`
itself needed one before its actual size was known. Until that capture
exists, reimplementing the two dispatchers wholesale (the only way to
change what they *do*, rather than just measure them) stays exactly as
unjustified as the CABAC notes section always said it was for a diagnostic
alone - now with a direct number to decide it by, once that capture lands,
rather than a subtraction-derived guess.

## mbparse_us retest: the hypothesis was wrong - pf_parse_inter_mb is not the remainder
The real-hardware retest the previous section asked for landed
(`CPU=68060 STAGE_PROFILE=1 CABAC_PROFILE=1`, YouTube 360p progressive
MP4, Turbo, A1200 68060/50, 60 decoded frames / 28 paired `h264 stages:`+
`h264 cabac:` reports). Parsed and summed all 28 reports (both per-report-
averaged and totals-weighted, which agreed within 0.2 points):

| bucket | avg % of core |
|---|---|
| mc | 10.2% |
| recon | 13.2% |
| intra | 4.8% |
| bin | 7.3% |
| coeff | 5.4% |
| mvpred | 1.8% |
| mbinfo | 8.2% |
| **mbparse** | **6.2%** (0.2-10.7% range) |
| remainder before mbparse | 49.0% |
| **remainder after subtracting mbparse** | **~43%** |

`mbparse_us` came back real but *small* - only ~6% of core_us, and it
accounts for just ~12% of the ~49% remainder that motivated building it
(`sum(mbparse)/sum(remainder)` across the whole capture = 12.2%, matching
the per-report average of 12.6% closely). This directly contradicts the
working hypothesis stated in the previous section and in `ih264d_mbinfo_
wrap_port.c`'s own header: `pf_parse_inter_mb` is *not* the dominant
unattributed cost. After adding a real, direct measurement of it, ~43% of
total decode time is *still* completely unattributed - barely smaller than
before mbparse_us existed.

**Why `mbparse_us` reads this small has a concrete, source-grounded
explanation, not just "the measurement must be wrong": `mbparse_count` is
consistently a small fraction of `mbinfo_count`.** `mbinfo_count` (5520,
2760, 1840, ... per frame) counts *every* macroblock via `pf_get_mb_info`,
skip or not, intra or not. `mbparse_count` (345, 361, 507, 847, ...)
counts only calls that actually reach `pf_parse_inter_mb` - which, per the
per-MB loop read while building the mbinfo swap (`ih264d_parse_pslice.c`'s
`while(!u1_slice_end)` loop), is gated behind *two* conditions: not a skip
MB, *and* `u1_mb_type < u1_mb_threshold` (P/B-inter, not intra - an intra
MB embedded in a P/B slice is dispatched elsewhere entirely, never through
`pf_parse_inter_mb`). Averaged across the capture, `mbparse_count/
mbinfo_count` = 35.1% (range 1.2-54.5%) - meaning on average **65% of
macroblocks in this real content are either skip or intra-coded**,
bypassing `pf_parse_inter_mb` completely. For a fairly static YouTube
360p talking-head-style clip that is entirely plausible (skip runs are the
cheapest thing an H.264 encoder can emit for unchanged background), and it
means the earlier ~48-55% remainder was never really "the cost of parsing
inter macroblocks" - most macroblocks in this stream aren't going through
that path at all.

**The real, still-unmeasured cost is most likely the per-MB loop's own
skip-path and intra-dispatch handling - neither of which any bucket built
so far touches.** Two concrete, previously-unconsidered candidates, both
visible in the same per-MB loop already read while building `pf_get_mb_
info`/`pf_parse_inter_mb`'s wraps:
- **Skip-MB bookkeeping**: for every skip MB (the majority here), the loop
  itself does a `memset(ps_dec->ps_curr_ctxt_mb_info, 0, ...)`, sets
  `pu1_left_mv_ctxt_inc`/`pi1_left_ref_idx_ctxt_inc`/`pu1_left_yuv_dc_csbp`
  to zero, writes `ps_part_info` (direct/skip partition bookkeeping), and
  calls `ih264d_update_nnz_for_skipmb()` - none of this runs inside
  `pf_get_mb_info` (already measured) or `pf_parse_inter_mb` (now
  measured, but never called for a skip MB at all) - it is loop-body C
  code with no function-pointer indirection of its own to hook.
- **Intra-MB dispatch inside P/B slices**: `ih264d_parse_mb_type_cabac()`
  (called for every non-skip MB, its own bin-decode cost already inside
  `bin_us`, but its C-level dispatch is not) decides intra vs. inter, and
  an intra result is parsed through whatever the intra equivalent of
  `pf_parse_inter_mb` is - not `pf_parse_inter_mb` itself, so `mbparse_us`
  never sees it, and it has not been identified or measured at all yet.

Neither of these has a clean, already-proven interception point the way
`pf_get_mb_info`/`pf_parse_inter_mb` did - they would need their own
investigation (does the intra path go through another dec_struct_t
function pointer that happens to be cross-file? is skip-MB bookkeeping
worth a dedicated `clock()` bracket around the `if(u4_mb_skip){...}` arm
specifically, which - unlike the two function-pointer cases so far - has
no vendored function boundary to hook at all, only a block of inline
loop-body code) before assuming either is buildable the same way mbinfo/
mbparse were. Not attempted in this round; this section exists to correct
the record, not to guess at the next fix without checking source first the
way every other addition in this chain did.

Verification for this round is identical to the mbinfo/mbparse rounds
before it - no new code was written, this is a documentation-only update
recording a real-hardware measurement result. The capture itself
(`RAM:MintVID.log`, GadTools "Log: on") is the same build already pushed
and verified (`e8194eb`) - nothing to rebuild or re-verify.

## intramb_us: the other half of the skip-or-intra gap, and it's a plain --wrap this time
Direct follow-up to the mbparse_us correction above - the user's own call:
keep chasing. Of the two candidates named there (per-MB-loop skip-path
bookkeeping, intra-MB dispatch), intra-MB dispatch turned out to have a
real, clean interception point, found by reading the same per-MB loop the
mbinfo/mbparse work already lives in one more time: `ih264d_parse_
pslice.c`'s shared per-MB loop (`ih264d_parse_inter_slice_data_cabac()`,
used by both P and B slices - see the mbparse_us section above for why it
is "shared") has an `else` branch alongside its `pf_parse_inter_mb` call,
for exactly the `u1_mb_type >= u1_mb_threshold` (intra) case:
`ret = ih264d_parse_imb_cabac(ps_dec, ps_cur_mb_info, ...)` - a *plain
direct function call*, not a function-pointer assignment at all.

Checked before assuming it would be wrap-able, the same discipline as
every other addition in this chain: `ih264d_parse_imb_cabac()` is
*defined* in `ih264d_parse_islice.c`, a completely different file from
where this call site lives (`ih264d_parse_pslice.c`) - an ordinary
cross-object relocation, the textbook `--wrap` case (the same shape that
already lets bin/coeff/mvpred/mbinfo work, not the same-file case
`pf_parse_inter_mb` itself fails on). No struct-field-swap trick needed
this time - a plain `-Wl,--wrap=ih264d_parse_imb_cabac` reaches it
directly. The one gap: `ih264d_parse_islice.c`'s *own* per-MB loop (a
whole I slice/I frame) calls `ih264d_parse_imb_cabac()` from within the
same file that defines it - the identical same-file dead end
`pf_parse_inter_mb` hits, left unwrapped deliberately. I frames are a
small minority of pictures (one per GOP); the P/B-embedded case this wrap
*does* reach is exactly where the mbparse_us retest's "~65% of
macroblocks are skip or intra" finding lives.

New `vendor/libavc_port/ih264d_intramb_wrap_port.c` is the intra sibling
of `ih264d_mbinfo_wrap_port.c` - same pure timing pass-through via GNU
ld's `__real_ih264d_parse_imb_cabac` symbol, same `CABAC_PROFILE=1`-only
`--wrap` flag (`libavc.mk`), same zero cost in a normal build, no
reimplementation so no new bit-exactness claim to prove. New `intramb_us`/
`intramb_count` bucket wired through the identical path as mbinfo/mbparse
(`ih264d_cabac_profile.h`/`.c`, `core/mr_h264.h`/`.c`, `amiga/mrplay.c`'s
`"h264 cabac:"` line) - documented with the same non-disjoint-bucket
caveat as `mbparse_us`: `ih264d_parse_imb_cabac()` itself calls
`ih264d_decode_bin()` (`bin_us`) and `ih264d_parse_residual4x4_cabac()`
(`coeff_us`), so `intramb_us` necessarily overlaps both, on top of
whatever previously-unmeasured intra-mode-signalling/CBP/mb_qp_delta glue
is in its own body.

Verified via `tests/run_m68k_check.sh`: `mr_decode_cabac_profile.m68k`
now runs the mbinfo/mbparse struct-field-swap *and* the new intramb
`--wrap` together on every macroblock of a real decode, and still decodes
the H.264 High Profile fixture at worst-frame MAE=0.705 - unchanged from
every other build. The full m68k/big-endian conformance suite, including
the 68060 disassembly scan (which now also builds and scans `ih264d_
intramb_wrap_port.c`, empty at production flags same as `ih264d_mbinfo_
wrap_port.c`), passes clean.

Not yet known: how much of the remaining ~43% unattributed cost (the
figure left over after the mbparse_us retest) `intramb_us` actually
explains. Needs the same thing every bucket in this chain has needed
before its real size was known: a real A1200 `CABAC_PROFILE=1
STAGE_PROFILE=1` capture with `intramb_us` in the log. Given the
mbparse_us retest's own finding (mbparse_count only 35% of mbinfo_count,
i.e. ~65% skip-or-intra) and mbinfo_count/mbparse_count's own per-report
numbers, a rough expectation can be formed once a capture lands: if
`intramb_count` comes back close to `mbinfo_count - mbparse_count`, that
confirms most of the "skip or intra" 65% was actually intra (not skip),
and `intramb_us` should explain a correspondingly large share of the
remainder - if it comes back much smaller than that difference, most of
those macroblocks were genuinely skip, and the still-unmeasured skip-path
per-MB-loop bookkeeping (memset/`ih264d_update_nnz_for_skipmb()`, no
function boundary to hook - see the mbparse_us correction's own note)
becomes the leading remaining suspect. Not guessed at further here without
that data.

## intramb_us retest, and terminate_us/mbtype_us: two more clean --wrap targets, still ~36% left over
The real-hardware retest the previous section asked for landed (57 paired
`h264 stages:`+`h264 cabac:` reports, `timing/117 frames: decode=139306 ms`,
same A1200 68060/50 / YouTube 360p / Turbo setup as every capture in this
chain). Sum-weighted across all 57 reports:

| bucket | % of core |
|---|---|
| mbparse | 8.2% |
| intramb | 5.0% |
| remainder before mbparse+intramb | 49.5% |
| **remainder after subtracting both** | **36.2%** |

`intramb_us` answers the question the previous section left open, and the
answer is "mostly skip, not mostly intra": `intramb_count` averaged only
42.2% of the `mbinfo_count - mbparse_count` "skip-or-intra" gap (range
0-90.9%), not the "close to 100%" that would confirm intra dominates. So
per that section's own decision rule, the still-unmeasured skip-path
per-MB-loop bookkeeping (`memset`, `ih264d_update_nnz_for_skipmb()` - no
function-pointer or cross-file-call boundary to hook, unlike every bucket
built so far) is a real candidate for (part of) the remaining ~36%, not
ruled out the way it would have been had intramb_us come back large.

**Two more genuinely wrap-able per-MB costs, found by re-reading the same
shared per-MB loop (`ih264d_parse_inter_slice_data_cabac()` in
`ih264d_parse_pslice.c`) once more with the ~36% remainder as the target:**

- **`ih264d_decode_terminate()`** (defined in `ih264d_cabac.c`) - the
  CABAC "termination" bin (spec 9.3.3.2.2.3): `end_of_slice_flag`, decoded
  once per macroblock **regardless of skip/inter/intra** at the end of the
  shared per-MB loop, plus the I16x16-vs-I_PCM bin inside
  `ih264d_parse_mb_type_intra_cabac()` (`ih264d_parse_mb_header.c`). Read
  its body before assuming it was worth measuring: pure inline arithmetic
  (CLZ, range update, conditional renorm) with no other function calls at
  all - not a thin wrapper around `ih264d_decode_bin()`, so `terminate_us`
  is not folded into `bin_us` already. All three real call sites
  (`ih264d_parse_pslice.c`, `ih264d_parse_islice.c`,
  `ih264d_parse_mb_header.c`) are in different files from the one that
  defines it - an ordinary cross-object relocation, the same `--wrap`
  shape as mbinfo/intramb, not the same-file dead end `pf_parse_inter_mb`
  hits. Because it runs on every macroblock unconditionally, this is the
  one bucket in the whole chain besides `mbinfo_us` that reaches the full
  population, not just a subset - and it is fully disjoint from every
  other bucket (bin/coeff/mvpred/mbinfo/mbparse/intramb), a real additive
  measurement, not another overlapping one.

- **`ih264d_parse_mb_type_cabac()`** (defined in `ih264d_parse_mb_header.c`,
  called from `ih264d_parse_pslice.c`) - the `mb_type` syntax-element
  dispatch for every **non-skip** P/B macroblock (inter and intra alike),
  run strictly before that macroblock's `pf_parse_inter_mb`/
  `ih264d_parse_imb_cabac` dispatch. Same cross-object shape, checked the
  same way, plain `--wrap` target. Its body calls only
  `ih264d_decode_bin()`/`ih264d_decode_bins()` (confirmed by reading it) -
  never `ih264d_decode_terminate()` - so `mbtype_us` overlaps `bin_us` the
  same way `mbparse_us`/`intramb_us` do, but is disjoint from
  `terminate_us`.

Both are pure `--wrap` timing pass-throughs
(`vendor/libavc_port/ih264d_terminate_wrap_port.c`,
`ih264d_mbtype_wrap_port.c`), gated identically to mbinfo/intramb: the
`-Wl,--wrap=` flags for both only exist under `CABAC_PROFILE=1`
(`libavc.mk`), so a normal build links zero bytes of either. Wired through
the same accumulator/plumbing path every prior bucket used
(`ih264d_cabac_profile.h`/`.c` now track eight buckets; `core/mr_h264.h`/
`.c`; `amiga/mrplay.c`'s `"h264 cabac:"` printf line; both m68k test
scripts' `LIBAVC_SRC` lists and `--wrap` link flags).

Verified via `tests/run_m68k_check.sh`: `mr_decode_cabac_profile.m68k` now
runs all four `CABAC_PROFILE`-only wraps (mbinfo struct-field-swap,
mbparse struct-field-swap, intramb `--wrap`, terminate `--wrap`, mbtype
`--wrap` - five mechanisms across four distinct measured functions, since
mbinfo's wrap also does the mbparse swap) together on every macroblock of
a real decode, and still decodes the H.264 High Profile fixture at
worst-frame MAE=0.705 - unchanged from every other build in this whole
chain. The full m68k/big-endian conformance suite, including the 68060
disassembly scan (which now also builds and scans
`ih264d_terminate_wrap_port.c`/`ih264d_mbtype_wrap_port.c`, both empty at
production flags same as their mbinfo/intramb siblings), reports
`m68k/big-endian check: OK` end to end. `make check` (host,
`MR_H264_CABAC_PROFILE` never defined there) passes unchanged.

Not yet known: how much of the ~36% remaining unattributed cost
`terminate_us`/`mbtype_us` actually explain, and - since `terminate_us`
reaches every macroblock the same way `mbinfo_us` does - whether the
skip-path bookkeeping suspect named above is still needed once it's
measured. Needs the same thing every bucket in this chain has needed
before its real size was known: a real A1200 `CABAC_PROFILE=1
STAGE_PROFILE=1` capture with `terminate=`/`mbtype=` in the log. The
user has offered to send another one from their own A1200 as needed.

## terminate_us/mbtype_us retest: remainder down to ~28%, but the arithmetic itself needs a caveat now
The real-hardware retest landed (31 paired `h264 stages:`+`h264 cabac:`
reports, `timing/62 frames: decode=81449 ms`, same A1200 68060/50 /
YouTube 360p / Turbo setup, this capture's clip a different session than
the one behind the ~36% figure above). Sum-weighted across all 31 reports:

| bucket | % of core |
|---|---|
| mc | 9.1% |
| recon | 11.6% |
| intra | 4.5% |
| bin | 6.2% |
| coeff | 4.9% |
| mvpred | 1.6% |
| mbinfo | 7.3% |
| mbparse | 5.6% |
| intramb | 6.8% |
| **terminate** | **2.9%** |
| **mbtype** | **11.1%** |
| remainder before terminate+mbtype | 42.4% |
| **remainder after subtracting both** | **28.4%** |

Both new buckets are real. `mbtype_us` is not a small addition - at
11.1% of core it is the single largest bucket added in this entire
chain besides `mc`/`recon` themselves, bigger than `mbinfo_us` and
nearly double `mbparse_us`. `terminate_us` is small (2.9%), as expected
for a bucket whose own body is a handful of CLZ/compare/renorm
instructions with no other function calls.

**`terminate_count` (83,443) exceeds `mbinfo_count` (60,720) - a ratio of
137%, worth explaining rather than treating as a red flag.** `mbinfo_us`
and `terminate_us` were both documented as "reaches every macroblock,"
but not through the same set of call sites: `mbinfo`'s wrap only covers
the shared P/B per-MB loop's `pf_get_mb_info` (see the mbinfo_us
section above), while `terminate_us` wraps `ih264d_decode_terminate()`
directly and is reached from three places - the shared P/B loop's own
end-of-slice check (the population `mbinfo_us` also covers), the
*separate* I-slice per-MB loop in `ih264d_parse_islice.c` (macroblocks
`mbinfo_us`'s wrap never sees, since that loop doesn't go through
`pf_get_mb_info` the same way), and the extra I16x16-vs-I_PCM bin inside
`ih264d_parse_mb_type_intra_cabac()` for every intra macroblock. The
excess over 100% is exactly those two additional sources, not double
counting within a single call site.

**A methodological point worth stating plainly now that a genuinely large
bucket (`mbtype_us`) overlaps `bin_us`: the "remainder = core minus every
named bucket" arithmetic this whole chain has used since the CABAC notes
section is not a strict partition, and it gets *less* accurate, not more,
as overlapping buckets pile up.** `mbparse_us`, `intramb_us` and
`mbtype_us` each wrap a function whose own wall-clock time already
includes calls into `ih264d_decode_bin()` (counted again, separately,
under `bin_us`) and - for mbparse/intramb - `ih264d_parse_residual4x4_
cabac()`/mv-prediction (`coeff_us`/`mvpred_us`). Subtracting all of
`bin_us`+`coeff_us`+`mvpred_us`+`mbparse_us`+`intramb_us`+`mbtype_us`
from `core` therefore subtracts the shared, nested portion more than
once - the "remainder" figure is a real, useful *directional* signal
(it has correctly tracked every genuinely new measurement finding real
cost in this chain so far, mbtype_us included), but as an absolute
number it is now more likely to **understate** the true still-unattributed
cost than to overstate it, precisely because more of what gets subtracted
is double-counted overlap rather than newly-measured, actually-disjoint
work. This was already true, in smaller degree, from the moment
`mbparse_us` was added; it is worth calling out now because `mbtype_us`'s
11.1% is large enough that the effect is no longer negligible. A properly
disjoint accounting would need *exclusive* (self) time for each wrapped
function - time spent in the function's own body minus time spent in any
nested wrapped call - which none of the `clock()`-bracketed wraps in this
chain currently attempt; that is a real methodology upgrade to consider
before adding further overlapping buckets, not something to guess at
without deciding whether it is worth the added instrumentation
complexity.

This capture's own skip/intra split, for reference: `mbparse_count +
intramb_count` = 44,494 of `mbinfo_count`'s 60,720 (73.3% non-skip) -
notably different from the ~35% non-skip (65% skip-or-intra) the earlier
`mbparse_us` retest measured. Both numbers are real; they describe
different sessions/clips, and this is a reminder that the skip/intra
mix is content-dependent, not a fixed property of "YouTube 360p Turbo on
this hardware" to expect a single number for.

No code changed this round - this is a documentation-only update
recording a real-hardware measurement result, the same as the
mbparse_us-retest section above. `make check`/`make check-m68k` are
unaffected. The leading still-open question is unchanged in kind from
before this retest, only smaller in size: the skip-path per-MB-loop
bookkeeping (`memset`, `ih264d_update_nnz_for_skipmb()`) remains
completely unmeasured (it has no function-pointer or cross-file-call
boundary `--wrap` or a struct-field swap can reach), and - per the
methodological point above - the true remainder it would need to explain
is arguably still closer to the ~42% pre-terminate/mbtype figure than the
~28% post figure, since a meaningful share of that drop is overlap
double-subtraction rather than newly-attributed disjoint cost.

## Plain --time capture: instrumentation really was costing ~3x, confirmed near-cleanly
The user's own closing capture for this investigation round: a plain
`--time` run (no `STAGE_PROFILE`/`CABAC_PROFILE`) on the same A1200
68060/50, YouTube 360p, containing two back-to-back sessions in one log.
The first (`H.264 performance: Turbo+`, PB-skip/keyframes-only) was a
throwaway - one frame only (`timing/1 frames: decode=872 ms`) before the
user restarted with the intended settings. **The real capture is the
second session**, explicitly re-printing `H.264 performance: Turbo
(B-skip, bilinear MC)` before playback - the identical performance mode
every `CABAC_PROFILE`/`STAGE_PROFILE` capture in the sections above used.
`timing/74 frames: decode=29725 ms` - 401.7 ms/frame average (387.4 ms
sum-weighted across the 19 `rtg timing` lines from this session), against
the ~1100-1360 ms/frame seen throughout the CABAC/STAGE-instrumented
captures at the same Turbo mode. A first pass at this write-up mistakenly
averaged in the one-frame Turbo+ session and mis-stated the performance
mode for the whole capture - corrected here, since the real 74-frame
block was Turbo all along.

The one setting that does still differ is C2P backend: `c2p=wpa`
(Standard/portable) here versus `c2p=kalms-040` in every prior profiled
capture. That is not a meaningful confound for this comparison - C2P
converts already-decoded pixels for display and is entirely downstream
of `vdecode`/`libavc-core`, the figures both this number and every
profiled one are built from; nothing about which C2P kernel is running
changes what libavc itself does. So, mode held constant and the one
remaining difference being causally irrelevant to the measured quantity,
this is a genuinely clean confirmation: **real production H.264 decode
on this content is roughly 2.7-3.4x faster than every CABAC_PROFILE/
STAGE_PROFILE number in this file's own investigation chain reported** -
the instrumentation-tax warning made when the first such capture landed
("almost certainly overstate how slow the real, non-instrumented
production `mrplay` is") was not just directionally right but
substantially so.

This does not retroactively invalidate the *proportions* measured inside
those captures (mc/recon/bin/coeff/mbinfo/mbparse/intramb/terminate/
mbtype's relative shares of `core_us`, and the remaining unattributed
~28-42%) - those are ratios internal to one instrumented run and stay
valid for what they showed about relative cost. It does mean the
*absolute* millisecond figures quoted alongside them throughout this
whole chain were never a good proxy for real playback smoothness on
their own, and any future real-hardware capture aimed at "how slow is
this stream really" should default to a plain `--time` run first, per
this section, rather than reaching for `CABAC_PROFILE`/`STAGE_PROFILE`
out of habit.

No code was changed for this capture - it is a measurement result,
recorded here rather than acted on.

## HAM8 + Kalms mouse-lag report: investigated from source, closed by the user independently
**Resolved - the user identified the actual cause themselves, separately
from this investigation.** Everything below is what this session checked
before that happened (all real, none of it wrong, just superseded as the
active lead) - kept as a record of what was ruled out at the source
level, not as an open thread needing further data.
A separate real-hardware report from the same session: HAM8 with Kalms
C2P shows mouse-pointer lag during playback, which the user's own
Amiga-experience read as a likely sign of hitting `68060.lib`'s
unimplemented-instruction emulation (a real, well-known 68060 symptom -
see this file's own extensive `plm_audio_smul64_060`/`mr_u64_div_u16`
family of fixes for the general mechanism). Investigated as far as this
dev host allows, and explicitly **not** turned into a code change, because
nothing found rises to this file's own bar for trusting a fix.

Checked and ruled out at the *source* level: the four Kalms kernels
actually wired into `Makefile.amiga`'s `KALMS_OBJ`
(`c2p1x1_8_c5_040.s`, `c2p1x1_8_c5_bm_040.s`, `c2p2x2_8_c5_bm.s`,
`c2p1x1_6_c5_bm_040.s` - the ones a HAM8 session under Kalms actually
runs, since `aga_open()`'s `kalms_kind` selection is keyed on plane
`depth` alone, and HAM8's 8 real bitplanes select the same kernel a
plain 256-colour session would; this is architecturally expected, not a
gap - 8-plane C2P transposition doesn't care what the 8-bit chunky value
means, only how many bitplanes it fans out to, which is exactly why HAM6
gets its own dedicated `KALMS_1X1_6` kernel while HAM8 correctly shares
the 8-plane ones) contain no extended `MULS.L`/`MULU.L`, no `DIVS.L`/
`DIVU.L` at all, and no `TAS`/`MOVEP`/`CAS2`/`CHK2` - only `mulu.w`
(2-operand, hardware since the 68000, the same safe form this project's
own MP2/68060 kernels already rely on). The classic trap mechanism this
file documents at length elsewhere doesn't show up in these kernels'
source. `vendor/kalms-c2p/ham8/*.s` (a different, unused set of files
with "h8" in the name) turned out to be a red herring - reading their
actual operation, they are an RGB→HAM8 *encoder*, not a C2P kernel, and
are referenced by nothing in `Makefile.amiga` at all; this project's own
portable-C `mr_ham_encode()` does that job instead.

**A source grep is not the level of proof this file normally insists on,
though, and getting further needs real work this dev host cannot
finish in one pass.** No Kalms kernel of any kind has ever been run
through `tests/check_m68060_asm.sh`'s real disassembly scan - a genuine,
previously-undocumented coverage gap, unlike every project-authored `.S`
file in `core/`/`vendor/libavc_port/`. Attempting to close it directly:
`m68k-linux-gnu-as` refuses these files outright - they use Amiga-
assembler syntax (`;` line comments, `section code,code`, colon-less
`XDEF`-exported labels) GNU as does not understand. A first mechanical
translation pass (strip `;` comments, `section` → `.text`, `XDEF` →
`.globl`) got further but still failed on ordinary instructions like
`swap d4` and `movem.l d2-d7/a2-a6,-(sp)` with "operands mismatch" -
GNU as's default m68k dialect in this cross toolchain evidently
disagrees with something more structural than comments (colon-less
label syntax is the leading suspect, since PhxAss/vasm-style assemblers
accept a bare label at column 0 where GNU as wants `label:`, and a
misparsed label can desynchronise everything that follows it on the same
statement). Turning this into a fully working GNU-as translation - and
then a real disassembly answer via the existing `scan_m68060_forbidden.py`
- is real, bounded engineering work, just not work this pass finished;
it was not pushed further once it became clear "confirm or deny the
68060.lib theory" needed a genuine syntax port, not a quick fix.

**No fix was made for this report**, on purpose: every candidate this
session considered (excluding Kalms from HAM8 the way the AGA copper-
vdouble section's own prose already claims happens, when the code
actually doesn't do that) would be a guess dressed up as a fix, exactly
the thing this file's own discipline throughout the H.264 CABAC
investigation above refuses to do without a number in hand first. What
*is* useful right now, needing no code change and no more of the user's
time than one side-by-side comparison: play the same clip as HAM8 with
Kalms C2P, then again as HAM8 with `--c2p`/"Portable" (Standard), same
resolution and scale. If the lag tracks Kalms specifically (present with
Kalms, gone with Standard), that is real, actionable evidence pointing
back at the Kalms kernel; if HAM8 lags similarly either way, the cause is
upstream of C2P entirely (most likely `mr_ham_encode()`'s own dither cost,
or simply CPU saturation from full-screen HAM8 encode+C2P+blit at every
frame, not a trap at all) and the Kalms kernel is cleared. Either result
is more useful than a guessed patch, and neither needs a rebuild.

## Chasing an actual speedup from the known hot buckets: a real coverage gap closed, no free lunch found
Direct follow-up to the whole CABAC-profiling chain above, now with a
mandate to act on it rather than keep measuring: "you know the hot
buckets, get working." Two things were checked with intent to find a
concrete fix, in order of expected payoff, given the corrected plain
`--time` finding above ruled out "it's all profiling overhead" as the
answer.

**First, and highest-leverage given precedent: does the per-macroblock
CABAC hot path hide the same class of bug the per-slice divmod fix
already found and fixed in `boingball/libavc`?** That fix (see
`vendor/libavc_port/ih264_m68k_divmod.h`, already merged into this
repo's pinned submodule commit `cb8d7c3` - confirmed via
`git merge-base HEAD <that-branch>`, it was not sitting unmerged) found
GCC fusing an ordinary `%`/`/` pair against a runtime divisor into the
68060's trap-prone extended-dividend `DIVSL.L`/`DIVUL.L` at five
per-*slice* call sites - real, since a trap into 68060.lib's software
emulation is drastically more expensive than the instruction it replaces,
and per-slice still means at least once per frame. The natural next
question: does the same fusion happen anywhere in the per-*macroblock*
loop, which runs thousands of times more often per frame than the
per-slice sites did? `tests/check_m68060_asm.sh`'s own disassembly gate
had never actually checked - its H.264 `vendor/libavc` coverage was
scoped to exactly the four already-fixed per-slice symbols, never the
CABAC per-MB functions this session's whole `mbinfo_us`/`mbparse_us`/
`intramb_us`/`terminate_us`/`mbtype_us` chain had been measuring as real
cost on real hardware for weeks.

Built and scanned for real (not just grepped) with the project's own
`tests/scan_m68060_forbidden.py`, at the real `-mcpu=68060` production
flags: `ih264d_parse_pmb_cabac`/`ih264d_update_nnz_for_skipmb`/
`ih264d_parse_inter_slice_data_cabac` (`ih264d_parse_pslice.c`),
`ih264d_parse_bmb_cabac` (`ih264d_parse_bslice.c`),
`ih264d_parse_imb_cabac` (`ih264d_parse_islice.c`),
`ih264d_get_mb_info_cabac_nonmbaff` (`ih264d_mb_utils.c`),
`ih264d_parse_mb_type_cabac`/`ih264d_parse_mb_type_intra_cabac`
(`ih264d_parse_mb_header.c`), `ih264d_decode_bin`/`ih264d_decode_bins`/
`ih264d_decode_terminate` (`ih264d_cabac.c`),
`ih264d_parse_residual4x4_cabac`/`ih264d_read_coeff4x4_cabac`
(`ih264d_parse_cabac.c`), and `ih264d_mvpred_nonmbaff`/
`ih264d_mvpred_nonmbaffB` (`ih264d_mvpred.c`) - every function behind
every named CABAC-profile bucket in this whole chain, in one pass.
**Clean.** No extended `MULS.L`/`MULU.L`, no extended-dividend divide, no
`__muldi3`/`__divdi3`/`__udivdi3` reference anywhere in this set. A real,
useful negative result, not a shrug: the leading hypothesis for "one more
big win like the per-slice fix" is ruled out with actual evidence, not
assumed clean because it looked fine in source (this is precisely the
distinction the per-slice fix itself proved matters - a source read alone
would have missed it too).

Wired into `tests/check_m68060_asm.sh` permanently rather than left as a
one-off manual check - a new build+scan block covering all eight files
above, documented in the script's own header alongside the existing
per-slice coverage. This closes a real, previously-undocumented gap in
this project's own audit methodology (every hand-asm `.S` file has always
been scanned; the vendored C this session's whole profiling investment
was measuring never had been) for free going forward - any future libavc
bump or MintVID-side change to this hot path gets checked automatically,
the same safety net every other 68060-specific claim in this file already
relies on. Verified via the full `tests/run_m68k_check.sh` (including
this expanded gate) passing end to end, unchanged worst-frame MAE on
every H.264 fixture including the `CABAC_PROFILE=1` build.

**Second: hand-reading the two largest named C-level dispatch buckets
(`mbtype_us` 11.1%, `mbinfo_us` 7.3%) and the previously-unmeasured
skip-MB bookkeeping for the kind of waste the weighted-pred fix found -
a computation happening unconditionally when it is provably almost
always unnecessary.** `ih264d_parse_mb_type_cabac()`
(`ih264d_parse_mb_header.c`) is a straight binary-tree decode of the
`mb_type` syntax element per spec table 9-37 - a handful of
`ih264d_decode_bin()`/`ih264d_decode_bins()` calls and integer
arithmetic on their results, nothing computed that isn't immediately used
to pick the next branch. `ih264d_get_mb_info_cabac_nonmbaff()`
(`ih264d_mb_utils.c`) is neighbour-availability mask arithmetic and
struct-pointer bookkeeping - also inherently O(1) per call, nothing
speculative or redundant. The skip-MB bookkeeping flagged as unmeasured
several sections above (`ih264d_parse_inter_slice_data_cabac`'s own
`if(u4_mb_skip)` arm, `ih264d_parse_pslice.c` lines ~918-949) is the same
shape: a `memset`, two 16-byte context-reset writes, a partition-info
struct write, and the `ih264d_update_nnz_for_skipmb()` call - real,
necessary CABAC neighbour-context state for the *next* macroblock's
decode, not overhead a smarter check could skip. None of the three reads
like the weighted-pred case, where an expensive path ran on a capability
bit instead of an actual-need check; there is no equivalent "is this
really necessary" question with an obviously-usually-false answer sitting
in any of them.

**Conclusion, stated plainly rather than papered over with a change for
its own sake: there is no algorithmic free lunch left in this hot path,
and no hidden 68060 trap either - both real possibilities, both checked
with actual evidence, both ruled out.** The remaining ~28-42% unattributed
cost (see the terminate_us/mbtype_us retest's own methodological caveat
about double-subtraction inflating that estimate) is very likely the
accumulated real cost of a great many small, individually-necessary
operations spread across thousands of per-MB calls a frame, not a single
fixable hotspot - consistent with every named bucket already checking out
as lean, purposeful C. The one lever this investigation *has* established,
with real measurement now in hand to justify it (unlike when the CABAC
notes section first raised and set aside the idea): hand-writing 68k
assembly for the CABAC per-MB dispatch functions themselves, the same way
`ih264_m68k_cabac.S` already exists for `ih264d_decode_bin`. That is a
real, bounded, but substantial undertaking - multiple ~100-300 line
functions, each needing the same register-scheduling care and bit-exact
differential fuzzing (`mr_h264_m68k_check`-style) every other hand kernel
in this tree got - not something to start speculatively inside a
same-session "get working" ask. Deliberately not begun here; a decision
for the user to make explicitly, with this section's own honest
"no shortcut found" as the reason it would be starting from scratch,
not from a known 2x-in-a-day win the way the per-slice divmod fix was.

## Tried and reverted: calling mr_ih264d_decode_bin_m68k directly from ih264d_parse_mb_type_cabac()
Direct follow-up, on explicit instruction to stop investigating and ship
something: implement and benchmark one concrete optimisation of
`ih264d_parse_mb_type_cabac()` against the same test clip, keep the
original available for comparison, revert and explain if it doesn't help.

**The change**: all 13 single-bin `ih264d_decode_bin()` call sites inside
`ih264d_parse_mb_type_cabac()` (SI/P/B branches; the 2 multi-bin
`ih264d_decode_bins()` calls in the deep B-slice branches were left alone
- no direct asm primitive exists for those) were redirected to call
`mr_ih264d_decode_bin_m68k()` - the existing, already-verified hand-asm
CABAC bin decoder `ih264_m68k_cabac.S` provides - directly, instead of
through the vendored `ih264d_decode_bin()` symbol. On a real m68k link
that symbol resolves via `--wrap` to `__wrap_ih264d_decode_bin()`
(`ih264d_cabac_wrap.c`), a C function whose entire body is
`return mr_ih264d_decode_bin_m68k(...)`. That file's own header
documents this as "a second full call/return layer... paid on every
single decoded bin" and records that removing it *globally* (exporting
`__wrap_ih264d_decode_bin` straight from the `.S` file, no C code) broke
the real AmigaOS link and was reverted for that reason. This was a
narrower, different mechanism: change what ONE caller calls, never
touching `--wrap` or the trampoline itself - a new self-contained header,
`vendor/libavc_port/ih264_m68k_mbtype_bin.h` (`MR_M68K_ASM` calls the
primitive directly; the portable fallback calls `ih264d_decode_bin()`
unchanged), included from a one-line change to `vendor/libavc`'s
`ih264d_parse_mb_header.c` (on a new branch,
`claude/mbtype-cabac-direct-asm-call`, off the currently-pinned `cb8d7c3`).

**Correctness, checked before any timing number was trusted**: host
`make check` (portable fallback path, unaffected either way) passed
unchanged. Two full m68k builds - one with the change, one built from a
`git stash`-restored original source, both at the exact same production
flags `tests/run_m68k_check.sh` uses - decoded `test_h264_high.mp4` (and
`test_h264_aac.ts`/`test_h264_ac3.ts`/`test_h264_aac.mkv`, different
container/mux shapes over the same content) to **byte-for-byte identical
PPM output** and identical worst-frame MAE (0.705) against `ref_h264_high`.
Expected, not a surprise: the change calls the exact same
already-bit-exact-verified primitive `__wrap_ih264d_decode_bin` itself
already called, just one call-frame closer - no new arithmetic, nothing
to newly prove bit-exact, only the wiring to get right.

**The actual finding, from disassembling `ih264d_parse_mb_type_cabac`
in both builds before trusting a timing number at all**: `__wrap_
ih264d_decode_bin` compiles, at this toolchain's `-O2`, to a **single
`bral` (branch-always-long) instruction straight to
`mr_ih264d_decode_bin_m68k`** - GCC's own sibling/tail-call optimisation
recognising `return f(args);` with identical argument shapes needs no
call frame at all. `objdump --disassemble=ih264d_parse_mb_type_cabac` on
both builds came back **byte-identical instruction counts (219
instructions, 18 jsr/bsr sites in each)** - the only difference anywhere
in the function is which symbol the one `jsr` that hits `SI_SLICE`'s b0
call targets (`__wrap_ih264d_decode_bin` vs `mr_ih264d_decode_bin_m68k`
directly); every other call site already compiles to an indirect
`jsr %a4@` through a register GCC loads once and reuses, identical in
both builds. So the "second full call/return layer" this file's own
`ih264d_cabac_wrap.c`/`ih264_m68k_cabac.S` headers describe **already
does not exist in the compiled code at this optimisation level** - it is
one `bral` (a handful of cycles at most, not a stack frame, argument
reload, or `rts`) per bin, not a real function call. There was
essentially nothing left to remove.

**Benchmark, run only after that finding explained why to expect little**:
7 qemu-m68k wall-clock runs each of `test_h264_high.mp4 --check`,
identical `-m68030` build flags both sides. Baseline mean 0.11906 s
(stdev 0.00086); with the change, mean 0.11960 s (stdev 0.00269) - a
nominal **+0.46% slower**, but the "with the change" run's own variance
is more than 3x the baseline's, so this reads as pure scheduling/qemu
noise, not a real regression, consistent with the disassembly showing
no instruction-count difference to produce one either way.

**Reverted, per the standing instruction to revert and explain rather
than ship a change that doesn't help**: `vendor/libavc`'s working tree
restored to the exact pinned `cb8d7c3` (`git checkout --`, branch
deleted), the new header removed, nothing left in either tree. Explained
above, not just asserted: the mechanism this change targeted (a real,
documented, and previously load-bearing concern elsewhere in this file -
see the CABAC notes section's own account of the *global* wrap-removal
attempt) turned out to already be closed by the compiler at this
optimisation level for *this* specific trivial-tail-call shape, which
the earlier, larger attempt's real AmigaOS link failure never actually
disproved or confirmed either way (that attempt failed to *link*, not
because the removed layer turned out to be free). One genuine residual
uncertainty, stated plainly rather than glossed over: this was verified
against `m68k-linux-gnu-gcc` at `-O2`, not the real `m68k-amigaos-gcc`
toolchain (still unavailable on this dev host) - GCC's sibling-call
optimisation is standard and has applied to this exact code shape across
many GCC generations, so there is no specific reason to expect Bebbo's
toolchain to differ, but "no specific reason to expect otherwise" is not
the same standard of proof this file holds every other 68060-specific
claim to, and is recorded as a gap rather than papered over as certainty.

The practical upshot for anyone revisiting "make mbtype_us faster" next:
the call-overhead angle this section chased is a dead end, checked and
closed with real evidence, not left as an assumption. The only lever
that remains real, per the section above this one, is a genuine hand-asm
reimplementation of the CABAC binarisation logic itself (not just
redirecting which primitive gets called) - a substantially larger
undertaking than what this section attempted, with its own differential
fuzz-testing needs, not something to reach for again without deciding
that undertaking is worth it explicitly.

## Tried and reverted: a genuine hand-asm reimplementation of ih264d_parse_mb_type_cabac()
Direct follow-up, taking the larger lever the section above named and
declined to start without an explicit go-ahead: implement, verify
bit-exact, and benchmark a real hand-written m68k reimplementation of the
CABAC mb_type binarization control flow itself (spec table 9-37), not
just a call-forwarding trick. This is the fuller, more honest test of
whether that whole angle helps at all - and the answer turned out to be
no, decisively, once actually measured.

**The change**: a new `vendor/libavc_port/ih264_m68k_mbtype_cabac.S`
reimplemented `ih264d_parse_mb_type_cabac()`'s full SI/P/B dispatch by
hand, keeping the CABAC engine state (range, offset, the cabac_table
pointer, the bitstream pointer) pinned in registers across every bin one
macroblock's mb_type decode needs, instead of round-tripping each value
through `ps_dec->s_cab_dec_env`/`ps_bitstrm` on every individual
`ih264d_decode_bin()` call the way the C version does. A new `--wrap`
port file (`ih264d_mbtype_cabac_wrap_port.c`, same GNU ld mechanism as
every other wrap in this chain) redirected the one real call site
(`ih264d_parse_pslice.c`'s shared per-MB loop - confirmed by grep to be
the only cross-file reference, the same check every wrap in this family
starts with) to the new asm, unconditionally under `MR_M68K_ASM` - a
production optimisation, not a CABAC_PROFILE-gated diagnostic. The
pre-existing `ih264d_mbtype_wrap_port.c` (a pure timing pass-through
feeding the `mbtype_us` CABAC-profiling bucket from earlier in this file)
was folded into the new wrap file rather than kept alongside it, since
both wanted to provide the same `__wrap_ih264d_parse_mb_type_cabac`
symbol and only one function can.

**Two real bugs, both caught by differential testing before any timing
number was trusted - worth recording in full since both are exactly the
class of mistake this style of hand-asm is prone to, and neither was
visible from reading the code:**

1. The differential test (`tests/mr_h264_mbtype_cabac_check.c`, modelled
   on `mr_h264_mvpred_dispatch_check.c`'s real-`dec_struct_t` pattern) was
   *first* built with `--wrap=ih264d_parse_mb_type_cabac` on its own link
   line - which redirected the test's own plain-named "real" call to the
   asm wrapper too, silently comparing the asm against itself instead of
   the genuine vendored C. Every initial run "passed" for the wrong
   reason. Caught by checking `mr_h264_mvpred_dispatch_check.m68k`'s own
   build line (no `--wrap` at all - the real symbol and the `__wrap_`
   symbol are both called by their own plain names with no linker
   redirection active) and matching that precedent instead.
2. Once genuinely comparing against the real C, the test failed ~78,000
   of 60,000 iterations (more than one failure per iteration in places)
   with wildly out-of-domain range/val_ofst values. Two distinct causes,
   found by reading the failing leaf paths against the asm, not guessed:
   - Several leaf paths (SI_FLAT, P_INTER, B_SKIP, four B-slice
     sub-branches) computed the correct mb_type return value into d0 but
     never flushed the still-live d0/d1 (range/val_ofst) to
     `ps_dec->s_cab_dec_env` before overwriting d0 with that return value
     - only the paths that call out to `ih264d_parse_mb_type_intra_cabac`/
     `ih264d_decode_bins` flushed, since those genuinely need fresh memory
     state for the callee. Every non-call leaf needed its own flush
     inserted immediately before d0 was repurposed.
   - The P_SLICE inter path held b1's decoded value in d5 across the
     subsequent bin-decode call that reads b2 - but that shared bin-decode
     core's own documented clobber list includes d5, so the second call
     silently destroyed it, corrupting the final `(b1<<1)+b2` computation
     with whatever the b2 decode's own internal scratch use had left in
     d5. Fixed by saving b1 on the stack across that one call instead of
     in a register the shared core might clobber.
   Both fixes brought the differential test (60,000 iterations, all 11
   leaf branches hit well above the minimum coverage bar) to a clean pass
   with zero failures - genuinely bit-exact against the real C this time,
   confirmed by the corrected build.

**Full correctness re-verified with the real bugs fixed**: `make check`
(host) passed unchanged. `make check-m68k` (the full conformance suite,
including the new differential test wired into `tests/run_m68k_check.sh`
the same no-`--wrap`-for-the-function-under-test way as its mvpred
sibling) passed end to end - `test_h264_high.mp4`'s worst-frame MAE
unchanged at 0.705, identical to every build in this whole investigation
chain. `tests/check_m68060_asm.sh`'s disassembly scan (the new `.S`/port
file added to the existing whole-object H.264 scan list) reported clean -
no extended MULS.L/MULU.L, no extended-dividend divide, no libgcc 64-bit
calls.

**The benchmark, and the actual finding**: qemu-m68k wall-clock, same
methodology as the section above - a baseline build (real C, no
`--wrap=ih264d_parse_mb_type_cabac`) against the asm build (production
default, wrap active), both at identical `-m68030` flags otherwise.

- `test_h264_high.mp4` (the standard 128x96/24-frame conformance fixture,
  small MB count): baseline mean **0.1206 s**, asm mean **0.1376 s** -
  **~14% slower**, consistent across 5 runs each side (baseline range
  0.1176-0.1271 s, asm range 0.1366-0.1390 s - non-overlapping).
- A generated 640x360/25fps/8s synthetic clip (`libx264 -profile high`,
  ad hoc, not checked in - same "not committed" precedent as the earlier
  `mr_h264_set_dynamic_skip()`/weighted-pred host probes in this file),
  chosen specifically to raise the macroblock count and so the relative
  weight of `mb_type_cabac` itself: baseline mean **1.835 s**, asm mean
  **2.524 s** - **~38% slower**, an even larger regression, not a smaller
  one, as MB density (and so call frequency) went up.

That second result is the one that matters: if the slowdown were fixed
per-call overhead unrelated to how often the function runs, a
denser clip would dilute it, not amplify it. Instead the gap *grew*
substantially with call frequency, which is the signature of a real,
scaling per-call cost, not noise or a fixed one-time tax.

**Why, read from the two builds' own structure rather than guessed**: the
"genuine reimplementation" approach replaces a per-bin call chain that -
per the section directly above this one - was *already* effectively
free (`__wrap_ih264d_decode_bin` compiles to a single `bral` straight
into the same hand-tuned bin-decode core this new function's own `.Lbin`
copies). So there was less real overhead to remove than the design
assumed going in. Meanwhile the new function pays two costs the compiled
C path did not have to: `mr_ih264d_parse_mb_type_cabac_m68k` saves/
restores eleven registers (`movem.l %d2-%d7/%a2-%a6`, 44 bytes each way)
on *every* call regardless of which leaf branch actually runs, including
the cheapest ones (SI_FLAT, B_SKIP - a single bin decode) - a fixed tax
GCC's own per-branch register allocation for the original C function has
no equivalent of paying unconditionally; and every one of this function's
own internal bin decodes now goes through a real `bsr`/`rts` pair to its
local `.Lbin` subroutine, where the baseline's C path reaches the same
underlying asm core via what is, per the section above, already a free
tail call with no call frame at all - so this rewrite traded a chain that
was free for one that is not. Both costs scale with how often the
function is called (more macroblocks, more `movem.l` pairs and more
`bsr .Lbin` round trips), matching the observed direction exactly. This
was not measured or ruled out at implementation time - it follows
directly from the structure now that the benchmark's direction is known,
the same kind of after-the-fact structural explanation the section above
already modelled correctly.

**Reverted, per the same standing instruction as every attempt in this
chain**: `ih264_m68k_mbtype_cabac.S`, `ih264d_mbtype_cabac_wrap_port.c`
and `tests/mr_h264_mbtype_cabac_check.c` deleted; `ih264d_mbtype_
wrap_port.c` restored; `vendor/libavc_port/libavc.mk`, `tests/
run_m68k_check.sh` and `tests/check_m68060_asm.sh` reverted to their
pre-change state (`git checkout --`). `make check-m68k` re-run after the
revert to confirm it reproduces the exact pre-existing passing state, not
just that the revert applied cleanly.

The practical upshot for anyone revisiting "make mbtype_us faster" yet
again: both real levers this file identified for this specific function -
redirecting the bin-decode call chain, and a full register-cached
reimplementation of the dispatch itself - have now been tried, verified
correct (the second one only after real differential-testing work
surfaced and fixed two genuine bugs), benchmarked, and found to make
real production decode measurably *slower*, not faster, with the
evidence pointing at fixed per-call register-save/call-frame overhead in
a hand-written multi-branch dispatch outweighing whatever memory-
round-trip savings it was designed to capture. A future attempt would
need a fundamentally different structure - e.g. only entering the
register-cached path for the multi-bin branches that actually chain
several bin decodes together (B-slice's deeper sub-branches), falling
straight through to the plain C/already-free-tail-call path for the
cheap single-bin leaves that dominate real content (P_INTER, B_SKIP,
SI_FLAT - see the mbparse_us retest's own "~65-73% of macroblocks are
skip or intra" findings elsewhere in this file for why those leaves
dominate) - not a blanket reimplementation of every branch. Not attempted
here; this section's job was to test the blanket version honestly and
report what it actually measured, not to iterate further within the same
session.

## DV (IEC 61834/SMPTE 314M) video decoder
A real user report: a DV-PAL AVI from a camcorder (720x576, `DV Video`
codec, PCM audio already demuxed into its own WAVEFORMATEX stream - a
"type-2" DV-AVI) would not play at all - Fast Buffer filled (reading the
whole ~2 GB file into Fast RAM), then nothing: no error, no video, and the
process had to be closed by hand to release the memory, rather than the
usual ~2 s auto-exit `mrplay.c`'s "no decoder" path already does for an
unsupported codec. Root cause, confirmed from source before writing
anything: `core/mr_codec.c`'s registry had no DV decoder at all - this file
could never have played, buffer size aside - and the apparent hang was a
separate, real gap in `core/mr_source.c`'s `open_local_file()`: the
whole-file Fast Buffer cache is a single blocking `fread()` of the entire
file *before* the codec is ever even looked at, so a slow NAS/SMB read of a
multi-gigabyte file has nothing to do with codec support and stalls ahead
of every fast-fail/status-reporting path that would otherwise report it
promptly. That second issue is real but out of scope for this change - not
investigated or fixed here, since DV wasn't decodable regardless of how
fast the file opened.

**Decision: reuse a proven decoder rather than reimplement DV's DCT/VLC/
quantisation bitstream from scratch.** DV's macroblock-adaptive coding
(8x8 vs. 2-4-8 DCT block selection per macroblock, its own weighting/
quantisation tables, and a bit-exact VLC scheme) is comparable in scope to
the H.264/MPEG work already in this tree, and this project's own precedent
(vendoring Ittiam's libavc for H.264, VideoLAN's libmpeg2 and a Rockbox/
a52dec AC-3 core, both GPL-2.0-or-later - see THIRD-PARTY-LICENSES.txt) is
to vendor a mature reference decoder and write a thin port layer, not to
hand-roll a new one. `player/vendor/libdv/` is a decode-only subset of
libdv (Quasar DV Codec, LGPL-2.1-or-later - a more permissive licence than
either GPL component already vendored here), fetched from its
`deepin-community/libdv` GitHub mirror. Only the files the decode path
actually needs are carried: `dv.c/h`, `dv_types.h`, `parse.c/h`, `place.c/h`,
`weighting.c/h`, `quant.c/h`, `idct_248.c/h`, `dct.c/h`, `bitstream.c/h`,
`vlc.c/h`, `audio.c/h` (needed only for `dv_audio_new()`/`dv_parse_header()`'s
own internal call into it, not for PCM decode - see below), and `YV12.c/h`.
Dropped entirely: the encoder, the popt CLI helper, and the RGB/YUY2
colour-space output paths (rgb.c/YUY2.c) - MintVID only ever wants planar
YV12/4:2:0, and `dv.c`'s own pre-existing `YUV_420_USE_YV12` build switch
(present in upstream, just never turned on) already retargets the standard
`e_dv_color_yuv` dispatch at `dv_mb420_YV12()` instead of a packed-YUY2
renderer, so no new dispatch enum/case was needed - just enabling the
switch that was already there and deleting the two colour-space paths this
build doesn't use.

**Three portability bugs found and fixed while adapting the vendored
code, one of them silent-but-would-have-been-wrong-on-real-hardware -
exactly the class of gap this file's "Validate against ffmpeg" section
exists to catch, this time before it ever reached a commit:**

- **Endian detection was structurally broken for a buildless vendor copy,
  and it mattered for more than a metadata field.** `dv_types.h`/
  `bitstream.h` derive `LITTLE_ENDIAN_BITFIELD`/`BIG_ENDIAN_BITFIELD` and
  `bitstream.h`'s `swab32()` (the macro that un-reverses a native 32-bit
  word load back into the DV bitstream's MSB-first bit order) from
  `BYTE_ORDER`/`LITTLE_ENDIAN`/`BIG_ENDIAN`, normally supplied by
  `<endian.h>` behind `HAVE_ENDIAN_H`/`HAVE_MACHINE_ENDIAN_H` - autoconf
  macros this tree's buildless vendor copy never defines. With both macros
  undefined, the preprocessor treats them as `0`, so `#if (BYTE_ORDER ==
  LITTLE_ENDIAN)` reads as `0 == 0` and is always true - silently forcing
  little-endian behaviour on every target, m68k included. `swab32()`'s
  `BIG_ENDIAN` branch is a no-op (a real big-endian CPU's native word load
  already matches the bitstream's own MSB-first packing) - getting this
  wrong doesn't just mis-tag an audio pack, it breaks VLC decode outright.
  Fixed with a new `player/vendor/libdv/mr_dv_endian.h`, derived from the
  compiler's own `__BYTE_ORDER__`/`__ORDER_BIG_ENDIAN__` builtins instead
  of a libc `<endian.h>` - defined by every GCC-family cross compiler this
  project targets (host gcc, m68k-linux-gnu-gcc, and - unverified so far,
  see below - m68k-amigaos-gcc), independent of whether the target libc
  even has `<endian.h>` at all (AmigaOS's clib2/newlib do not).
- **A latent `int */uint16_t*` pointer-type mismatch in `dv_decode_full_frame()`'s
  own `pitches` parameter, upstream, not introduced here** - `dv.h` declares
  `int *pitches`, but `dv_mb420_YV12()` (YV12.h) - the renderer this build's
  `YUV_420_USE_YV12` switch now actually routes through - takes `uint16_t
  *pitches` and reads it as such. On a little-endian host this silently
  reads the correct low 16 bits of each `int` regardless; on big-endian
  m68k it would read the *wrong* 16 bits of every stride value. Fixed by
  narrowing `dv_decode_full_frame()`/`dv_render_video_segment_yuv()`/
  `dv_render_macroblock_yuv()`'s own `pitches` parameter to `uint16_t *`
  throughout, matching what the renderer they call actually expects, and
  `core/mr_dv.c` passes a real `uint16_t pitches[3]` array, not an `int`
  one.
- **`M_PI` is a POSIX/BSD `<math.h>` extension, not exposed under
  `-std=c99`** (weighting.c/idct_248.c/dct.c's one-time table-init `cos()`
  calls) - normally supplied via config.h/autoconf feature-test macros this
  buildless copy has none of. Added to the same `mr_dv_endian.h` compat
  header rather than a separate file, since it's the same "autoconf-shaped
  gap in a no-configure vendor copy" problem.

Also dropped, for portability rather than correctness: the
`pthread_mutex_t` guarding `dv_decode_full_frame()` - mrplay/`mr_decode`
only ever call into this decoder from one task at a time, so libdv's own
defence against concurrent decoder use is dead weight here, and AmigaOS
has no pthreads to link against at all. And the `dv_init()` calls into
`dv_rgb_init()`/`dv_YUY2_init()` (rgb.c/YUY2.c, not vendored) and libdv's
own encoder-side table builders (`_dv_init_vlc_test_lookup()` etc.) are
removed from the one-time init function, matching the render paths they
existed to support also being removed.

**`core/mr_dv.c`** is the `mr_codec` plugin: `dv_open()` scope-checks
`dec->width == 720 && dec->height == 576` before doing anything else - DV/
PAL, IEC 61834, 4:2:0 (the common consumer camcorder case, and the one the
bug report was about) is all this decoder supports for now. NTSC/DVCPRO's
4:1:1 sampling (`e_dv_sample_411`) is a real, common format this does not
yet handle - `dv_decode()` also re-checks `dv->system`/`dv->sampling`
against what each individual frame's own header actually declares (not
just the container's width/height) and refuses rather than misdecode.
Registered fourccs: `dvsd`/`DVSD` (standard consumer DV), `dvc `/`DVC `
(space-padded, some capture tools' spelling), `CDVC`/`cdvc` (Canopus's own
tag, same bitstream), `dvsl`/`DVSL` (DVCPRO, still 4:2:0 at 625/50).
`dvhd`/`DVHD` (DVCPRO50/HD) are deliberately not listed - those profiles
are always 4:1:1 or higher-resolution, and routing them to this decoder
would turn a clean "no decoder" report into a confusing "decoder init
failed" one instead. Matches the case-insensitive fourcc-matching note
above - each tag is listed once, in `mr_codec_dv`'s own case.

**Defaults to RGB24, matching every other codec's host-testable default -
not YUV420P, despite libdv decoding into planar Y/Cb/Cr internally.**
`tests/mr_decode.c`'s `write_ppm()`/`check_ppm()` only ever read
`dec->frame.data` as packed RGB24 (see their own implementation - neither
checks `frame.fmt`), the same way H.264/MPEG-2 only opt into YUV420P
output via `mr_h264_set_yuv_output()`/`mr_mpeg2_set_yuv_output()` for the
Amiga display path and default to RGB24 for host validation otherwise.
The first cut of this decoder skipped that and handed back
`MR_PIX_YUV420P` directly - every frame decoded structurally fine (real,
varying Y/Cb/Cr bytes, confirmed with an ad-hoc debug dump before assuming
anything about the RGB path), but every output pixel came back pure
grayscale (R==G==B exactly), because the harness was reading three
consecutive Y-plane bytes as one RGB24 pixel. `mr_dv.c` now always decodes
into internal Y/U/V planes (libdv's own real output shape) and, by
default, converts them to RGB24 via `core/mr_yuv.c`'s existing
`mr_yuv420_to_rgb24()` - already relied on for this exact studio-range
YUV420 shape elsewhere in this file - immediately after
`dv_decode_full_frame()`. `mr_dv_set_yuv_output(dec, enabled)` (mirroring
`mr_h264_set_yuv_output()`/`mr_mpeg2_set_yuv_output()`) is the opt-in an
Amiga display path would use later to skip that conversion and consume the
planes directly - added now since the shape was already there, but not
yet wired into `amiga/mrplay.c`.

**Verified two ways, both passing clean:** a new `test_dv_pal.avi` fixture
(`tests/gen_assets.sh`, `ffmpeg -c:v dvvideo -pix_fmt yuv420p`, 720x576/
25fps/1s, `dvsd` fourcc) checked against ffmpeg's own dvvideo decode
(`ref_dv_pal/`) via the standard `mr_decode --check` path - worst-frame
MAE=1.615 (comfortably under the suite's 6.0 "high" threshold; higher than
Cinepak's ~0.2 benchmark elsewhere in this file, consistent with DV's own
DCT quantisation and the chroma-upsample rounding in
`mr_yuv420_to_rgb24()`, not a decode bug). `make check` passes unchanged
end to end. The same fixture also passes bit-for-bit-identical (MAE=1.615,
same worst-frame number) cross-built for real m68k/big-endian under qemu
(`tests/run_m68k_check.sh` - `core/mr_dv.c` and `player/vendor/libdv/`
added to that script's `$CORE`/`$CORE_060` lists and linked with `-lm`,
since libdv's table-init `cos()`/`tan()` calls need it) - real
confirmation that the endian fixes above are actually correct on
big-endian hardware, not just plausible from reading the source.
`mr_codec_registry_check` (a separate, narrower registry test that stubs
out most codecs to test routing in isolation) needed one matching
`STUB(dv);` line added alongside its existing stubs for the same reason.

**Not done in this pass, staged deliberately rather than guessed at:** the
local-file Fast Buffer hang for large files over a slow network share
(`core/mr_source.c`'s single blocking whole-file `fread()`, described
above - real, but unrelated to DV support itself and not investigated
further here); type-1 DV-AVI (audio embedded in the DIF blocks themselves,
no separate WAVEFORMATEX stream - would need `dv_decode_full_audio()`
wired into `mr_avi.c`, out of scope per this file's standing "Audio is
MintAMP... do not add an in-tree audio codec" principle, and this
project's audio registry is separate from the video `mr_codec.h` one this
change touches); `amiga/mrplay.c`'s own wiring of `mr_dv_set_yuv_output()`
(the Amiga display path still gets DV via the default RGB24 conversion,
same as every other codec before its own YUV opt-in was wired in); and
`tests/check_m68060_asm.sh`'s disassembly scan (libdv's dequantisation/
IDCT integer multiplies have not been checked for the extended-`MULS.L`/
libgcc-64-bit-call patterns this file's other 68060 sections spend so much
effort avoiding - a real gap, not yet closed).

**Immediate correction: this shipped a real CI break, caught and fixed
within the hour by CI itself, not by anything on this dev host.**
`core/mr_codec.c`'s registry references `&mr_codec_dv` unconditionally
(no `#ifdef` gate, matching every other always-on codec here except
H.264's `MR_HAVE_H264`) - but `Makefile.amiga`'s `CORE` list never gained
`core/mr_dv.c`/the vendored `libdv` sources, so every Amiga target failed
the real `m68k-amigaos-gcc` link with `undefined reference to
mr_codec_dv`. This is exactly the class of gap the "Validate against
ffmpeg" section names: `make check`/`make check-m68k` (host + qemu-m68k,
both green) prove the portable core decodes correctly, but neither one
compiles a single line of `Makefile.amiga` - only CI's real AmigaOS
toolchain step can catch a *link*-level gap in that separate build file,
the same lesson the 68060 MP2 kernel underscore-alias saga and the
`__wrap_ih264d_decode_bin` link failure both already taught earlier in
this file. Fixed by adding `core/mr_dv.c` and `player/vendor/libdv/*.c` to
`Makefile.amiga`'s `CORE` (mirroring the host Makefile exactly) and `-lm`
to `LDFLAGS` (libdv's one-time `dv_init()` table setup calls `cos()`/
`tan()` - no other `CORE` file needs libm, and nothing in this tree had
ever linked it before, so there was no existing precedent to check
against on this dev host, which has no real `m68k-amigaos-gcc` to test a
link against at all - `-lm` was pushed as a real bet, not a confirmed
fact, and CI's `build` job (`sacredbanana/amiga-compiler:m68k-amigaos`,
`m68k-amigaos-gcc` 6.5.0b) is what actually confirmed it: green on the
very next push, for every Amiga target (`mrplay`, `MintVID`, `iptvgui`,
`ytgui`, their `-GT` variants, `mr_decode`) in one pass. Not attempted for
the separate `vbcc` toolchain path in the same Makefile (its own `mr_decode`
rule also now transitively pulls in `mr_dv.c`/libdv via the shared `CORE`
list, but uses neither `$(LDFLAGS)` nor `-lm` - unlike the gcc path, CI
never exercises `TOOLCHAIN=vbcc` at all, so there is no CI feedback loop
to confirm or deny a fix there, and vc's own math-library linking
convention is unknown from this dev host).

**NTSC/PAL-SMPTE 314M DVCPRO (4:1:1) support added, once the CI fix
above was confirmed - the user's own direct follow-up ("add NTSC in, why
not?").** libdv has no planar 4:1:1 renderer (only `dv_mb420_YV12()` for
4:2:0) - its only 4:1:1 output path is packed YUY2/4:2:2
(`dv_mb411_YUY2()`/`dv_mb411_right_YUY2()`, YUY2.c/h, now vendored
alongside YV12.c/h with the same `pitches` `int*`->`uint16_t*` adaptation
YV12's own functions already needed - see the type-mismatch note above,
the same latent upstream bug in a second file). `dv.c`'s YUV dispatch
(`dv_render_macroblock_yuv()`), stripped down to 420-only in the first
pass, is restored to branch on `dv->sampling` again - `e_dv_sample_420`
still goes to `dv_mb420_YV12()`, `e_dv_sample_411` (and its `mb->x >= 704`
right-edge case, DV's odd 720-not-a-multiple-of-32 macroblock geometry)
now goes to the YUY2 renderer instead of being unreachable; `dv_init()`
regained its `dv_YUY2_init()` call alongside `dv_YV12_init()`.

The genuinely new piece is `core/mr_dv.c`'s own `unpack_yuy2_to_yuv420()`:
libdv's packed YUY2 output already has the hard, DV-specific part right
(chroma correctly placed per real macroblock geometry, upsampled from
DV's native 4:1 horizontal-only subsampling to YUY2's 2:1 - not something
this change had to re-derive), so reaching `MR_PIX_YUV420P` from there
needed only one further, completely generic step: averaging vertically
adjacent chroma sample pairs (4:2:2 has no vertical subsampling at all;
4:2:0 needs 2:1 both ways). `dv_open()` now accepts 720x480 alongside
720x576 (anything else - DVCPRO50/HD's larger/faster profiles, a
different geometry entirely - is still rejected, MR_EFORMAT); `dv_ctx`
gained an `is_411` flag and, only when set, an extra packed-YUY2
intermediate buffer the 420 path never allocates. `dv_decode()` also
re-checks each frame's own parsed `sampling` against what `dv_open()`
committed to (720x480 could, in principle, still carry a 420-sampled
frame) and refuses rather than misdecode, mirroring the existing 420-path
check.

Verified the same two ways as the PAL case, plus a direct pixel-level
sanity check the PAL case didn't need (since a wrong chroma-plane mapping
would pass a coarse MAE check while still being visibly broken - see the
chroma-swap-vs-stride-bug distinction in the MPEG-2 YUV notes above): a
new `test_dv_ntsc.avi` fixture (`tests/gen_assets.sh`, `ffmpeg -c:v
dvvideo -pix_fmt yuv411p`, 720x480/29.97fps/1s) checked against ffmpeg's
own decode - worst-frame MAE=3.163, comfortably under the 6.0 threshold
but genuinely higher than PAL's 1.615, expected and not a bug signature:
mr_dv.c's box-filter vertical average and ffmpeg/swscale's own 411->RGB
chroma interpolation are two different, equally valid filters over the
same native 4:1:1 data, not a correctness disagreement. Spot-checked
individual pixels directly (red stayed red, blue stayed blue, cyan-ish
stayed cyan-ish, just off by single-digit-to-tens per channel) to rule
out exactly the failure mode a coarse MAE pass could hide - a real chroma
swap or plane-order bug would show as wrong hues entirely, not small
per-channel deltas. Passes bit-for-bit identically (MAE=3.163, same
worst-frame number) cross-built for real m68k/big-endian under qemu
(`tests/run_m68k_check.sh`, `YUY2.c` added to its `LIBDV_SRC` list
alongside the host Makefile and `Makefile.amiga`) - the same real
big-endian confirmation the PAL case got, now covering the pitches-type
fix's second occurrence (YUY2.c's own `int*`->`uint16_t*` adaptation) as
well as the first (YV12.c's).

Not yet done: `Makefile.amiga`/CI confirms the NTSC-capable build still
links (same `CORE`/`LIBDV_SRC` list, no new source files beyond `YUY2.c`
already added), but no real hardware has played back an actual DV-NTSC or
DVCPRO camcorder file through this path yet - only the synthetic ffmpeg
fixture above, on host and qemu-m68k.

**Real-hardware crash on first playback: AmigaOS Guru `8000000B` on a
68040 WinUAE build, the moment Play was pressed on a real DV-PAL AVI.**
`8000000B` is AmigaOS's own encoding for the CPU's "Line 1111 emulator"
exception (vector 11, offset `$02C`) - an F-line instruction the CPU
doesn't implement in hardware, normally trapped and emulated in software
by `fpsp040.library`/`fpsp060.library`, which a real playback system may
not have loaded. 68040/68060 hardware FPUs implement ordinary arithmetic
(`FADD`/`FMUL`/`FDIV`/`FSQRT` on the 040) but *not* the transcendental
instructions (`FCOS`/`FSIN`/`FTAN`/`FLOGN`/`FETOX` etc.) - exactly what
`cos()`/`tan()`/`sqrt()`/`pow()` compile down to when the target has a
hardware FPU at all (this cross-compiler does, by default, for
`-mcpu=68040`/`68060`). Root-caused directly from the crash code alone,
before touching any file: `player/vendor/libdv/weighting.c`'s
`_dv_weight_init()`, `idct_248.c`'s `dv_dct_248_init()`, and `dct.c`'s
`_dv_dct_init()` all call `cos()`/`sqrt()`/`pow()` to build one-time
constant tables, and all three run unconditionally from `dv_init()` the
very first time a DV decoder is created (`dv_decoder_new()` -> `dv_open()`
in `core/mr_dv.c`) - i.e. exactly when Play is first pressed on a DV file,
matching the report precisely. `dct.c`'s `_dv_idct_88()` (the non-x86
brute-force 8x8 IDCT, actually reachable during real decode - `_dv_idct_248()`
is a separate, always-integer 2-4-8 path, see the DV decoder section
above) went further: it did double-precision floating-point
multiply-accumulate *every macroblock*, not just at init, using the
cos()-built `KC88[8][8][8][8]`/`C[8]` tables - basic `FADD`/`FMUL` are
hardware-safe on 68040/68060 so this specific part wasn't the crash's own
mechanism, but it is real FPU dependency in the hot decode path on a
target this project otherwise keeps rigorously integer-only (see the
whole 68060 MP2/H.264 kernel family elsewhere in this file) - explicitly
called out by the user mid-fix ("should be no FPU in code, only integer
btw"), so it was converted too, not left as "technically not the crash".

Every one of these tables is a pure function of the DV standard's own
constants with no runtime decoder state, so each was precomputed once,
offline (Python, this dev host's own libm, replicating the original C
expressions and their exact truncation/rounding rules - `player/tools/
gen_dv_tables.py` is the generator, kept in the tree so the hardcoded
arrays can be reproduced/audited rather than trusted as opaque numbers)
and hardcoded as `static const` arrays, removing every `cos()`/`sin()`/
`tan()`/`sqrt()`/`pow()` call from the three files' active code paths
entirely: `weighting.c`'s `dv_weight_inverse_88_matrix[64]` (the only
one of its several tables actually consumed by real decode - `postSC88`/
`postSC248`/`dv_weight_88_matrix`/`dv_weight_248_matrix`, and the
`postscale88_init()`/`weight_88_float()`/etc. helpers that built them,
turned out to be dead code even before this fix: those only feed
`_dv_dct_88()`/`_dv_dct_248()`, the forward-DCT AAN encoder path, which
nothing in this decode-only tree calls - confirmed by grepping the
whole vendored source for call sites, not assumed - so they were deleted
outright rather than also converted); `idct_248.c`'s `beta0..beta4`
(Q30) and `dv_idct_248_prescale[64]` (already folding in what used to be
`dv_weight_inverse_248_matrix[]`, itself now unneeded at runtime since
the one thing it fed is precomputed); and, for the actual hot-path
rewrite, `dct.c`'s `dv_idct88_basis[8][8]` - `KC88[x][y][h][v] =
cos(pi*v*(2y+1)/16)*cos(pi*h*(2x+1)/16)` is separable into
`COS8[v][y]*COS8[h][x]`, so folding in the per-index `C(k)` scale factor
collapses the whole 4096-entry double table into one 8x8 Q14
fixed-point basis matrix, and `_dv_idct_88()`'s brute-force loop becomes
plain `int64_t` multiply-accumulate (`block[v*8+h] * A[v][y] * A[h][x]`,
summed, then one rounded shift on the way out) - overflow-checked by
hand (16-bit block value + two 14-bit table entries per term, 64 terms
summed, comfortably inside int64's 63 usable bits) rather than just
trusted. `_dv_dct_init()`/`dv_dct_248_init()`/`_dv_weight_init()`
themselves are kept as callable no-ops (`dv.c`'s `dv_init()` still calls
all three unconditionally) rather than removing the calls, to keep this
a minimal, targeted diff.

Verified against the real bar (ffmpeg's own dvvideo decode), not just
"matches the old formula": worst-frame MAE actually *improved* slightly
on both fixtures (PAL 1.615->1.139, NTSC 3.163->2.901) - expected, since
the fixed-point path rounds to nearest on the way out where the original
`block[i] = temp[i];` truncated a double straight to `int16_t` (toward
zero) with no rounding at all. `make check` (host) and
`tests/run_m68k_check.sh` (real m68k/big-endian under qemu, both
`-mcpu=68040`/`68060`) both pass with these exact MAE figures reproduced
bit-for-bit on m68k, and a direct disassembly check (`m68k-linux-gnu-gcc
-mcpu=68040`/`68060 -O2`, `objdump -d`/`-r` on all three touched `.c`
files) confirms zero FPU mnemonics and zero libm/soft-float symbol
references anywhere in the compiled output - not just "no cos() calls in
the source" but "no way to reach a Line-1111 trap from this code at all,
on either CPU tier". `audio.c`'s `dv_audio_deemphasis()` has an identical
`tan()` call in the same family, left deliberately unconverted with a
comment: it is dead code in this build (never called - DV audio is
out of scope, see the DV decoder section above), so it costs nothing
today, but would need the same treatment before ever being wired up.

**Correction: it wasn't fixed - the FPU fix above was real but incomplete,
and the actual mechanism turned out to be a linking problem, not a
per-file code problem.** A real-hardware retest after both fixes above
merged still crashed with the identical Guru `8000000B`, but this time
on *any* file (an unrelated MP4 crashed the same way) and *before*
Play - specifically when `amiga/mrgui.c`'s `update_file_info()` reads a
newly-selected file's size and formats the status line
(`snprintf(..., "%s | type: %s | %ld bytes", ...)` - no float format
specifier anywhere in it). Confirmed via direct questioning that this
was a fresh rebuild from the actually-merged fix, ruling out "the user
tested a stale binary" before looking further. `update_file_info()`
itself has never had anything to do with DV or floating point - the
puzzle was why a totally unrelated, unchanged function started crashing
with an FPU trap right after landing an all-`cos()`/`tan()`-removed DV
fix.

The two prior PRs' fixed dct.c/idct_248.c/weighting.c genuinely had zero
FPU instructions left (re-confirmed by disassembly again here) - but
`Makefile.amiga`'s `LDFLAGS = -noixemul -lgcc -lm ...` is the single
variable *every* AmigaOS link target uses, GUI binaries included, and
-lm was still there because libdv still had two things left that
referenced libm even though nothing ever called them: `dct.c`'s dead
forward-DCT/encode path (`_dv_dct_88()`/`_dv_dct_248()`/`postscale88()`/
`postscale248()`, called only from the encoder this decode-only tree
never uses - `postscale88()`/`postscale248()` themselves call `pow()`)
and `audio.c`'s `dv_dump_aaux_as()` (a debug dump with a real `"%.1f kHz"`
float-format `printf()`, its one call site already commented out - the
same dead-code shape as `dv_test12bit_conv()`, found by grepping for
every remaining `%f`/`%e`/`%g` format specifier in the whole vendored
tree, not just cos/sin/tan/sqrt/pow call sites, once source-level FPU
avoidance stopped being enough to explain the symptom). Neither of these
runs, on any file, DV or not - but the mere presence of an unresolved (or
resolved-via-`-lm`) reference to `pow()`/`printf`-with-`%f` in the final
linked binary is enough for *this specific toolchain* to select a
different, floating-point-capable variant of `snprintf`/`vsnprintf` for
the *entire binary* - including every caller, whether or not that caller
ever touches a float. That silently swapped-in variant is what actually
traps: `update_file_info()`'s own call, formatting nothing but a string,
an enum-like extension, and an integer size, executes through
floating-point-capable formatting machinery it never asked for and
crashes on the same missing-fpsp040.library gap the original DV fix
already diagnosed correctly - just from a different, indirect trigger.
This is a genuinely new class of gap for this file's whole "no AmigaOS
toolchain on this dev host" theme: not an instruction-safety question
`check-m68060_asm.sh`-style disassembly answers, not a link-succeeds-or-
fails question CI's real `m68k-amigaos-gcc` `build` job answers, but
which *library variant gets silently selected* by the presence of an
unrelated symbol reference anywhere in the link - unreachable from
qemu/ELF (different C library, different symbol-selection behaviour
entirely) and invisible to a `-n` dry-run (which shows flags and object
lists, not which library implementation the linker actually resolves a
weak/overloaded symbol to).

Fixed two ways, both confirmed via disassembly/dry-run rather than
assumed: (1) every remaining libm reference deleted from libdv outright
- `_dv_dct_88()`/`_dv_dct_248()`/`postscale88()`/`postscale248()`/
`dct88_aan()`/`dct44_aan_line()`/`dct248_aan()` (dct.c, encode-only, the
`postSC88`/`postSC248` arrays they alone consumed removed from
weighting.c too) and `dv_dump_aaux_as()` (audio.c, its one commented-out
call site in dv.c removed alongside it) - and `dv_audio_deemphasis()`
(audio.c) actually converted this time rather than left as a documented
landmine: `a1`/`b0`/`b1` depend only on the DV standard's three legal
audio sample rates (32000/44100/48000 Hz), so `dv_deemphasis_coeffs()`
precomputes all three offline (`tools/gen_dv_tables.py`, cross-checked
against the file's own pre-existing 44.1/48kHz reference comment to
double-precision, and filling in the 32kHz case that comment had marked
"?") instead of calling `tan()` at runtime, with an unrecognised
frequency falling back to the 48kHz coefficients rather than guessing
further - the per-sample recursive filter itself keeps plain
double `+`/`*` (hardware `FADD`/`FMUL`, never a library call, so this
doesn't reintroduce the same problem). A whole-tree grep for every
remaining `cos`/`sin`/`tan`/`sqrt`/`pow`/`log`/`exp`/... call, `<math.h>`
include, and `%f`/`%e`/`%g` format specifier confirms libdv now has zero
libm dependency anywhere, reachable or not - `quant.c`'s own leftover
`#include <math.h>` (already unused before this) removed too, for the
same "prove it by grepping, don't just trust the header removed above"
discipline. (2) `-lm` removed from `Makefile.amiga`'s shared `LDFLAGS`
entirely and re-added only on `mrplay`'s own link line, since `mrplay` -
unlike every GUI target - genuinely links MintAMP/liba52's real DSP code
(confirmed by grep: `real/imdct.c`, `decoders/wma/mdct.c`, etc. all use
double-precision trig/log for filter-table generation) and is the only
target that should ever need it; a before/after `make -f Makefile.amiga
-n <target> -DAMIGA_GCC=/fake/...` dry-run confirms `-lm` now appears
exactly once for `mrplay` and zero times for `MintVID`/`MintVID-GT` (and,
by the same shared-LDFLAGS mechanism, `iptvgui`/`ytgui`/their `-GT`
variants).

Verified on host and real m68k/big-endian under qemu: `make check` (DV/PAL
MAE=1.139, DV/NTSC MAE=2.901 - unchanged) and the full
`tests/run_m68k_check.sh` (identical MAE, `m68k/big-endian check: OK`,
68060 disassembly scan clean) both pass exactly as before this round of
deletions - confirming the removed code was truly dead, not silently
load-bearing. `m68k-linux-gnu-nm`/`objdump -T` on the resulting qemu-target
binary show zero dynamic references to any libm symbol. **Not yet
confirmed on the actual real-hardware WinUAE repro** that started this
correction - the `-lm`-changes-snprintf-variant mechanism is a strong,
evidence-based hypothesis (matches every symptom: any file, before Play,
right at the one plain-string `snprintf()` call, appearing only after
`-lm` first got linked) but, per this file's own standing limitation,
only a real rebuild-and-retest on the reporting user's own WinUAE/68040
setup can actually prove the crash is gone - qemu/ELF and CI's real
toolchain link both structurally cannot exercise this specific
library-variant-selection behaviour.

**Real `m68k-amigaos-gcc 13.2.0` link attempt found a third bug, immediately
- and it invalidated a specific assumption this whole file otherwise
relies on.** Merging the `-lm`-scoping fix above and building `mr_decode`
for real failed with `undefined reference to __subdf3`/`__gtdf2`/
`__adddf3`/`__muldf3` inside `dv_audio_deemphasis()` - libgcc's
soft-float double-arithmetic helpers, not libm. That function's per-sample
filter recurrence (`lastout = *pmm*b0 + lastin*b1 - lastout*a1;`) is
plain `+`/`-`/`*`/`>` on `double`s, no library call anywhere in the
source - and it was left that way on the reasoning "`FADD`/`FMUL` are
hardware-safe on 68040/68060, so this doesn't reintroduce the trap the
`cos()`/`tan()` fixes exist for." That reasoning holds for
`m68k-linux-gnu-gcc` (this dev host's own cross-compiler, used for every
qemu/disassembly check in this whole investigation) but **not** for
Bebbo's real `m68k-amigaos-gcc`, which evidently defaults to software
floating point for `-mcpu=68040`/`68060` here - meaning ordinary `double`
arithmetic, not just transcendental library calls, lowers to unresolved
libgcc calls on the actual target. This is the sharpest version yet of
this file's own standing "no AmigaOS toolchain on this dev host" gap:
not a missing library or a wrong flag, but two different cross-compilers
disagreeing about a basic ABI default (hardware vs. software float) for
the *same* `-mcpu=68040`/`68060` flags, invisible to every qemu-based
check in this entire file because they all run through the *other*
compiler.

Fixed by removing `double`/`float` from `dv_audio_deemphasis()` entirely
rather than chasing which library provides these symbols: `a1`/`b0`/`b1`
become Q16.16 fixed-point `int32_t` constants (same three DV audio
rates, same offline-precompute discipline as every other table in this
whole investigation - `tools/gen_dv_tables.py`), the per-sample
recurrence becomes `int64_t` multiply-accumulate with one final rounded
shift (overflow-checked: a full-scale int16 sample times a Q16.16
coefficient is ~2.1e9, comfortably inside int64's 63 usable bits), and
`dv_audio_t::lastout[4]` (`dv_types.h`) changes from `double[4]` to
`int64_t[4]` to carry the filter's Q16.16 state between calls - confirmed
via grep that nothing else in the vendored tree reads that field, so the
type change is contained to this one function. A fresh whole-tree grep
for every remaining `double`/`float` (not just math-function calls this
time) confirms the only survivors are inside code already proven dead by
a `#if <never-defined-macro>` guard (`idct_248.c`'s unit-test data,
`dct.c`'s `BRUTE_FORCE_248`-gated `_dv_idct_248(double*)`) - genuinely
zero reachable floating-point arithmetic anywhere in libdv now, on either
compiler's ABI defaults. Directly verified against the actual failure
mode this time, not just reasoned about: `m68k-linux-gnu-gcc -c -O2
-mcpu=68040`/`68060` on the fixed `audio.c`, `nm`-scanned for
`__adddf3`/`__subdf3`/`__muldf3`/`__divdf3`/`__gtdf2`/`__ltdf2`/`__eqdf2`/
`__nedf2`/`__gedf2`/`__ledf2` and every libm symbol from before - clean at
both tiers. `make check`/`tests/run_m68k_check.sh` both still pass with
the exact same DV/PAL and DV/NTSC MAE as every prior round (this function
is dead code in the current integration, so nothing decode-visible could
have changed) - the real test this needed, a clean `m68k-amigaos-gcc`
link, is what the user's own next build attempt confirms or denies, not
anything provable from this dev host.

## mrplay crashes on Play (any file, audio on or off) after the -lm/soft-float fixes landed
A real-hardware/WinUAE report immediately after the previous section's two
fixes (the shared-LDFLAGS `-lm` printf-variant bug, and the real-toolchain
soft-float `dv_audio_deemphasis` link failure) both merged: the GUI's own
file-info display now works correctly (confirmed by the screenshot's status
line reading right), but pressing Play now crashes `mrplay` itself - not
the GUI - with the same Guru `8000000B` (Line-1111/FPU emulator trap).
Confirmed via two follow-up questions: this happens for **any** file, not
just the one in the report, and it **still crashes with "No audio"
ticked** - ruling out both a codec-specific cause and MintAMP's audio
decode path as the sole trigger, and pointing instead at something in the
codec/audio-agnostic part of the Play sequence.

**The printf-variant-selection mechanism from the previous section's fix
cannot be the cause here, on its own terms.** `mrplay` (unlike the GUI
binaries) has always linked real `-lm` for MintAMP, and has always
printed real `%f`/%.2f` values throughout its `--time` diagnostics (every
`vdecode=`/`libavc-core=` line quoted throughout this whole file's H.264
investigation chain came from `mrplay`/`mr_decode`) - if merely linking
`-lm` alongside a live `%f` reference broke plain-string `snprintf()`
calls, `mrplay` would have shown this on every real-hardware session ever
captured in this file, not just now. Whatever changed, it is something
new to `mrplay`'s own link or Play-time code path, not the general
mechanism the GUI fix addressed.

**Leading, evidence-based suspect: `amiga/display_p96pip.c` (the P96 PIP
overlay backend, merged via PR #198/`codex/p96-video-compatible-screen`),
selected here because the screenshot shows `Display: RTG (P96)`.** Three
things point at it rather than being a guess dressed up as a lead:
timing (this crash happens exactly when `display_open()` runs, which is
Play-time, not file-selection time - matching "loads file info now and
then crashes when I press play" precisely); it is codec- and audio-
agnostic (`display_open()` runs once per Play regardless of which codec
or whether audio is enabled, matching both confirmed facts); and, per
this file's own already-in-place header comment, it is the single least-
verified code in this whole investigation - "not even a known case of
WinUAE's own P96/UAEGFX emulation implementing the PIP API at all". If
WinUAE's bundled Picasso96API.library predates the PIP API, or only
partially implements it, `p96PIP_OpenTags()`/`p96PIP_GetTags()` could
resolve through a library jump-table entry that doesn't do what this code
expects - `display.c`'s own backend-selection chain (`order[]`) only opens
`P96Base = OpenLibrary("Picasso96API.library", 0)` - version 0, i.e. "any
version at all" - so an old, PIP-less library still opens successfully and
`backend_p96pip`'s `open()` still gets called and still tries every PIP
call unconditionally.

**Not fixed by guessing a minimum library version, though - the vendored
`libraries/Picasso96.h` carries no version/history markers at all, so any
specific version number `OpenLibrary()` might be raised to would be an
invented number, exactly the kind of unverified patch this file's own
discipline (see the H.264 mbtype-cabac and HAM8/Kalms-mouse-lag sections)
refuses to ship without evidence.** What is real and shippable without
that evidence: this whole path had literally zero durable diagnostic
output before this fix - `display_p96pip.c`'s existing `--time`-gated
`printf()`s (already present from the original PR) were never followed by
a `Flush(Output())`, so a hard Guru crashing the task mid-open could lose
every line still sitting in stdio's buffer, and `amiga/mrplay.c`'s own
"%dx%d, opening display..." printf right before `display_open()` was only
ever flushed *after* `display_open()` returned - useless for exactly the
"it crashed inside display_open() and never returned" case this report
describes.

Fixed two ways, both plumbing/instrumentation, not a guess at the actual
P96/PIP root cause:
- `amiga/mrplay.c`: added a `Flush(Output())` immediately after the
  "opening display..." printf and *before* calling `display_open()`,
  alongside the pre-existing one after it returns - so a crash inside
  `display_open()` still leaves that line on disk.
- `amiga/display_p96pip.c`: every existing `--time`-gated diagnostic
  `printf()` in the open/resize path (`log_screen_target()`,
  `close_video_screen()`, `open_video_screen()`'s four sites,
  `rebuild_geometry()`, `p96pip_open()`'s entry and exit, `reopen_pip()`'s
  three sites) now calls `Flush(Output())` right after printing, and two
  new checkpoints were added around the single most likely trap site -
  immediately before and after the actual `p96PIP_OpenTags()` call inside
  `open_pip()` - plus one at `p96pip_open()`'s very entry (before any P96
  call at all, to distinguish "never even got here" from "crashed inside").
  `<proto/dos.h>` added for `Flush()`/`Output()`.

This does not fix the crash - there is no confirmed root cause yet, only
a strong, evidence-based lead. What it does is turn the next real-hardware
`--time` capture from "no log at all, just a Guru screen" into one that
should show exactly which P96 PIP call was last reached before the trap -
`RAM:MintVID.log`'s tail will read either "p96pip: calling p96PIP_OpenTags
..." with no matching "... returned win=..." line after it (the call
itself traps), or it will get past that and further checkpoints narrow it
from there, or the log will show the crash happening *after* every P96 PIP
checkpoint already printed cleanly (pointing away from this backend
entirely, back to something else in the Play sequence this investigation
hasn't found yet). A fast, code-free way to narrow this further without
even needing `--time`: try Play with a non-P96 Display mode (CGX/AGA) on
the same file - if that plays fine, `backend_p96pip` is confirmed as the
site; if it also crashes, the cause is elsewhere in `display_open()`'s
shared setup or earlier in the Play sequence, and this whole P96-PIP lead
is wrong.

Verified only that this instrumentation compiles logically (brace-balance
checked) and that `make check` (host, unaffected - both touched files are
Amiga-only) still passes unchanged; neither `mrplay.c` nor
`display_p96pip.c` can be compiled on this dev host at all, per this
file's standing "no AmigaOS toolchain" limitation - the real test is
whether the next real-hardware Play attempt, run with `--time`, produces
a log that pinpoints the crash.

**Retraction: the P96-PIP-specific hypothesis above is wrong - the user's
direct follow-up test confirms the identical crash happens with AGA
selected too.** AGA display mode never opens `Picasso96API.library` at
all (`display_open()`'s own gating: `P96Base` is only opened `if
(g_force_p96 && !g_force_aga)`, and every P96-mode `order[]` entry -
`backend_p96pip` included - is itself gated `!g_force_aga`), so
`display_p96pip.c` cannot be involved when AGA is selected. This rules the
whole backend out as the cause, cleanly - not "less likely," structurally
impossible for this report. The diagnostics added to it above are kept
(harmless, zero cost in a normal build, and still useful if P96 is
revisited later), but the search moves on.

**Re-scoped the search using what's actually new versus what's shared.**
`git log` across every commit in this whole DV/-lm/soft-float investigation
(from `eb72015`, the DV-CI-fix commit, through the P96 diagnostics commit
just above) shows `amiga/mrplay.c`, `amiga/display.c`,
`amiga/display_aga.c`, `core/mr_codec.c` and `audio/mr_audio_decode.c` were
*never touched* by any of it, except the display-open diagnostics commit
itself. `core/mr_codec.c`'s `mr_codec_find()` is confirmed pure FourCC
tag-matching with no probing (read in full: it folds case and compares
`fourcc` fields, then calls exactly one codec's `open()` - the one whose
tag matched, nothing else) - so a Cinepak file's Play never touches
`core/mr_dv.c`/`vendor/libdv` at all, at the C-control-flow level. Two
real conclusions follow: the crash is not "DV code running when it
shouldn't" (no call path reaches it for a non-DV file), and whatever *is*
new is confined to what actually changed in this whole investigation:
`Makefile.amiga` (the DV/libdv `CORE` additions, and the `-lm`
scoping/reordering), and the new `core/mr_dv.c`/`vendor/libdv/*` object
files themselves - present in every Amiga link target's `CORE` list, *just
sitting there unlinked-to-by-name* for a non-DV file, not something
reachable via source alone.

That "just sitting there" phrasing matters given this file's own already-
proven precedent: the exact mechanism that broke `mrgui.c`'s plain-string
`snprintf()` (see the "Correction: it wasn't fixed" section) was *linked
but never called* code changing which library variant the whole binary
resolves a shared symbol to - not anything reachable from the crashing
function's own control flow. The same class of mechanism remains a live,
unruled-out suspect here: `mrplay` links every one of the same
`core/mr_dv.c`/`vendor/libdv` object files the GUI targets do (same shared
`CORE` list), and the file-info-display crash is confirmed fixed only for
the GUI binaries (`MintVID`/`MintVID-GT`) - nothing in this whole
investigation has yet linked and Played through `mrplay` itself
specifically, on the real toolchain, since libdv was added to its `CORE`
list. Every "real hardware confirmed" Play session anywhere earlier in
this file predates the DV feature's `Makefile.amiga` changes entirely, so
this may not be a regression introduced by anything reachable from
`mrplay.c`'s own source at all - it could be the first time `mrplay`
itself has actually been linked-and-Played since those `Makefile.amiga`
changes landed.

**Broadened the diagnostics accordingly, rather than guessing further at
which specific mechanism it is.** The P96-PIP checkpoints only cover the
tail of the Play sequence (`display_open()` and after); if the crash is
instead in the *shared* front half - `mr_demux_open_file_ex()`/
`mr_demux_open()`, the codec-probe/`mr_codec_find()` step, or
`mr_decoder_open_config()` itself, all of which run identically for every
display mode and every codec - none of that had any `Flush(Output())` at
all before now, unconditionally printed lines included (`"streaming ...
from ..."`, `"loaded %ld bytes"`, `"codec probe: ..."`, the `want_time`-
gated `"video fourcc=..."` line). Added `Flush(Output())` after each of
those, plus a new checkpoint immediately before `mr_decoder_open_config()`
is called (mirroring the same "flush before, not just after" reasoning
already applied to `display_open()`) and a new `"decoder open: %s\n"`
line immediately after it succeeds. This is the same
instrumentation-not-guess discipline as the P96 diagnostics above, just
now covering the part of the sequence AGA's own crash proves is at least
as plausible a site.

A real-hardware retest with `--live-diag` or `--time` (either engages
enough of this path's own gated lines; the unconditional ones show
regardless) should now show one of: a log ending right after `"codec
probe: ..."` with no `"decoder open: ..."` line following (crash inside
`mr_decoder_open_config()`/`codec->open()` - worth checking with a codec
*other* than Cinepak too, to see whether it's decoder-specific or
universal even there); a log ending right after `"decoder open: ..."`
with no `"%dx%d, opening display..."` line following (crash somewhere in
the h264-speed/timing-setup glue between decoder-open and display-open,
none of which any bucket here yet checkpoints); or a log that reaches
`"opening display..."` and stops there even under AGA (crash inside
`backend_aga`'s own `open()` in `display_aga.c` - a file this whole DV/-lm
investigation never touched, so if this is where it lands, the cause is
either pre-existing and only now being reached for the first time, or a
`Makefile.amiga`-level effect exactly like the printf-variant mechanism,
not anything in `display_aga.c`'s own source). Whichever of these the next
capture shows narrows the remaining search space a lot further than
guessing does.

## Regression: mrplay crashed on Play, every codec, every display backend - `-lm` itself was the trigger
A real-hardware report after the DV/libm-and-soft-float saga above landed:
`mrplay` itself (not a GUI's file-info line this time) crashed with Guru
`8000000B` on pressing Play, for a Cinepak file under RTG (P96) + Kalms
C2P - a stream nowhere near DV, on the codec this whole tree was built
around first. A first hypothesis (the new P96 PIP overlay backend) was
tested and killed in one message: the user reproduced the identical crash
under plain AGA, which never opens `Picasso96API.library` at all,
structurally ruling out anything display-backend-specific.

The user's own next instruction set the actual investigation's shape and
its bar for being considered resolved, quoted because it is exactly what
was followed: rebuild the exact pre-DV revision (PR #195's base commit,
`939618a90fd04e78f73b72fd9737322e0cadeb27`) as a known-good baseline;
build current source with DV's codec-registry entry and Amiga sources
excluded but every other linker flag untouched, to isolate DV's object
presence from everything else that changed alongside it; if that doesn't
restore playback, look at linker flags specifically - `-lm` and which C
runtime gets selected; get a real linker map and disassembly from the
actual `m68k-amigaos-gcc` toolchain and find the faulting PC; keep the
decoder integer-only, no new FPU dependency; and do not open another
speculative PR - report the controlled builds' results first.

**Two real network-policy walls hit while trying to get a real toolchain
locally, neither routed around, per this environment's own agent-proxy
README.** `docker pull sacredbanana/amiga-compiler:m68k-amigaos` (after
manually starting `dockerd`, which this dev host allows) failed with a
CONNECT 403 from `production.cloudfront.docker.com` - a policy-level
block, confirmed via the proxy's own status endpoint, not a transient
failure. Fetching the presigned Azure Blob Storage URLs GitHub's own API
returns for workflow-run artifacts (`productionresultssa*.blob.core.windows.net`)
hit the identical CONNECT 403 pattern. Both are exactly the class of
"report the blocked host, don't retry" case that README exists for, so
neither was retried or routed around; GitHub Actions CI - already
configured to run the exact same `sacredbanana/amiga-compiler:m68k-amigaos`
image with no such restriction - became the real-toolchain proxy instead,
with job *console logs* (reachable via the GitHub API, unlike the blob
storage CDN) standing in for direct artifact downloads wherever a log
line already contained the needed fact (a binary size, a disassembly line
count).

**Four diagnostic branches, each with CI's `build` job (the real
`m68k-amigaos-gcc` link step) extended with a linker-map
(`-Wl,-Map=mrplay.map`) and a real `m68k-amigaos-objdump -d -r`/`-h` pass,
uploaded as their own artifact separate from the compiled binaries:**

1. `diag/baseline-pre-dv` - PR #195's exact base commit, rebuilt as-is.
   Confirmed its `Makefile.amiga` already has `LDFLAGS = -noixemul -lgcc
   $(LIBAVC_M68K_LDFLAGS)` with **no `-lm` anywhere** - not on the shared
   variable, not appended to `mrplay`'s own recipe. Links clean.
2. `diag/dv-excluded-flags-unchanged` - current source, DV's registry
   entry (`&mr_codec_dv,` in `core/mr_codec.c`, gated behind a new,
   purely-diagnostic `MR_DIAG_NO_DV` macro) and Amiga build sources
   (`core/mr_dv.c`, `$(LIBDV_SRC)`) excluded from the `CORE` list, every
   linker flag - `-lm` included - left exactly as current main has it.
   Isolates "does the mere presence of DV's object code matter" from
   "did something about the flags themselves change". Links clean, same
   `-lm` on `mrplay`'s link line as main.
3. `diag/current-main-diagnostics` - current main, unmodified except for
   the linker-map/disassembly CI step. The actual failing configuration,
   real-toolchain-linked for the first time with a map/disassembly to
   inspect.
4. `diag/no-lm-with-dv` (branched from #3) - current main, DV fully
   present, MintAMP/AAC/liba52 DSP code fully present, every other fix in
   this file's whole history intact - with exactly one change: `-lm`
   removed from `mrplay`'s own link line. Links clean.

Before reaching for a real-hardware retest, `mr_codec_find()`
(`core/mr_codec.c`) was re-read in full to rule out one tempting but
structurally impossible mechanism: it is pure case-folded FourCC tag
matching, a linear scan with no speculative probing or calling of every
registered codec's own `open()` - so a Cinepak file's Play sequence
cannot reach `core/mr_dv.c`/`vendor/libdv` at the C-control-flow level no
matter what got linked in alongside it. A `git log` across every shared
file this crash could plausibly touch (`mrplay.c`, `display.c`,
`display_aga.c`, `mr_codec.c`, `mr_audio_decode.c`) confirmed none of them
were touched anywhere in the whole DV/-lm/soft-float investigation except
by this session's own (already-superseded) P96 diagnostics commit -
whatever the mechanism was, it had to be confined to `Makefile.amiga`
itself and the new object files' mere presence in the link, not a shared
C-level logic change.

**The user built and tested build #4 (`diag/no-lm-with-dv`, PR #203) on
real WinUAE/68040 hardware: "PR203 does not crash!"** - the first, and
only, actually-decisive data point in this whole controlled investigation
sequence, and it lands squarely on step 4 of the user's own numbered
plan ("if it still crashes [after excluding DV], investigate the changed
linker flags separately, particularly `-lm`"). This is the identical
mechanism the earlier GUI file-info-line crash (see the DV decoder
section above) had already proven once: on this real `m68k-amigaos-gcc`
toolchain, merely linking `-lm` - regardless of whether anything in the
binary actually calls a libm function - changes which `snprintf()`/
`vsnprintf()` (or other shared runtime helper) variant the linker resolves
for the *entire binary*, and that altered variant traps with a Line-1111
FPU exception on code paths that have nothing to do with floating point
at all. The GUI fix scoped `-lm` down to `mrplay`'s own link line on the
theory that MintAMP/liba52's DSP code genuinely needed it there; this
investigation shows that theory was never actually tested and is false -
`diag/baseline-pre-dv` links that exact same MintAMP/AAC/liba52 object set
with no `-lm` at all, and `diag/no-lm-with-dv` (current source, same DSP
code, DV included) links and - now confirmed - runs correctly with no
`-lm` either. Nothing in this codebase actually needs `-lm`.

**The fix, once the real-hardware confirmation was in hand**: `-lm`
removed from `Makefile.amiga`'s `mrplay` recipe entirely - the same,
already-proven-correct change as `diag/no-lm-with-dv`, applied to the
real designated branch rather than a throwaway diagnostic one. No other
line changed: DV stays fully wired in (`core/mr_codec.c`'s registry entry,
`Makefile.amiga`'s `CORE`/`LIBDV_SRC` lists), MintAMP/AAC/liba52 stay
exactly as linked before. `LDFLAGS`'s own comment block (which asserted
mrplay "genuinely uses trig/log for filter table generation" and
therefore needed its own `-lm`) is corrected in place to record this
finding rather than repeat the now-disproven claim. `make check` (host,
unaffected by an Amiga-only Makefile change) passes unchanged.

Per the user's own bar for closing this out ("verify Cinepak, MPEG-1,
H.264 and DV playback before considering the regression resolved"): the
confirmed build already covers the exact codec (Cinepak) and exact
display/C2P combination (RTG/P96 and AGA, Kalms) the original report was
filed against - MPEG-1, H.264 and DV playback on this same fixed binary
still need their own explicit real-hardware confirmation before this is
fully closed, not yet obtained as of this writing.

The four `diag/*` branches and their auto-created PRs (#202, #203) were
throwaway by design, per the user's own "do not make another speculative
PR" instruction covering exactly this kind of branch - worth closing
without merging once the real fix above is confirmed end to end, rather
than left open as stray history.

## DV decode speed: a real, measured 1.9x from libdv's own quality knob
A user report on real WinUAE: DV video playback ("very slow") after the
decoder landed. `core/mr_dv.c`'s `dv_open()` had always hardcoded
`dv_set_quality(c->dv, DV_QUALITY_BEST)` with its own comment already
flagging the gap ("not yet tuned for slower Amiga targets") - libdv's
quality bits are a real, well-established decode-cost lever (the same
kind of idea as H.264's speed modes elsewhere in this file), just never
exposed by this port.

Read `vendor/libdv/parse.c`'s `dv_parse_video_segment()` before assuming
this would help: `DV_QUALITY_AC_MASK == DV_QUALITY_DC` skips
`dv_parse_ac_coeffs_pass0()`/`dv_parse_ac_coeffs()` - the whole 3-pass AC
coefficient VLC decode - entirely, for every block, reading only the 9-bit
DC coefficient per block instead. `dv_decode_macroblock()`'s IDCT then has
at most one nonzero coefficient to transform instead of however many AC
terms the encoder emitted; `_dv_idct_88()` (already fixed-point, see the
DV decoder section above) already skips a zero coefficient's whole inner
8x8 accumulation loop (`if (!bvh) continue;`), so DC-only input is cheap
there too, not merely "fewer VLC bits". Colour is kept independent of AC
quality (`DV_QUALITY_COLOR` is a separate bit) - `MR_DV_SPEED_FAST` uses
`DV_QUALITY_DC | DV_QUALITY_COLOR`, so chroma is still decoded and placed,
just DC-only (flat per-8x8-block colour) - never dropped to monochrome,
since libdv's own quality-bit design has no half-measure between "quality"
and "no colour at all" and dropping colour wasn't asked for.

`mr_dv_set_speed_mode(dec, MR_DV_SPEED_QUALITY|MR_DV_SPEED_FAST)`
(`core/mr_dv.h`/`.c`) is the new opt-in, mirroring `mr_h264_set_speed_mode()`'s
shape; default stays `MR_DV_SPEED_QUALITY` (`dv_open()`'s existing
unconditional choice), so nothing changes unless a caller asks.
`dv_set_quality()` just stores a bitmask `dv_decode_full_frame()` reads
fresh every frame, so this can be toggled live with no decoder reset
needed - unlike H.264/MPEG-2's own speed-mode plumbing, no per-picture
state depends on which mode decoded the previous frame.

Measured, not assumed: qemu-m68k/big-endian wall time (5 runs each,
`-m68030`, real cross-built `mr_decode.m68k`) on both conformance
fixtures - `test_dv_pal.avi` (720x576/25fps): quality mean 1.742 s, fast
mean 0.899 s (**1.94x**); `test_dv_ntsc.avi` (720x480/29.97fps, the 4:1:1
path): quality mean 1.742 s, fast mean 0.920 s (**1.89x**). This is a fair
qemu comparison per this file's own standing qemu-vs-hardware caveat -
real work removed (fewer VLC bits parsed, fewer IDCT terms per block), not
a memory-footprint trade the "dither LUT" note at the top of this file
warns qemu scores backwards.

`tests/mr_decode.c` gained `--dv-speed=quality|fast` (mirroring
`--h264-speed=`) so this is exercisable from `make check`/`make check-m68k`
without a GUI: a new "DV speed-mode separation" smoke check decodes both
fixtures at `--dv-speed=fast` and confirms the same frame count as the
existing `--check` runs (25/30 - DV always decodes every frame regardless
of quality bits, unlike H.264 Turbo+'s frame-skip, so this is a "still
decodes cleanly, end to end, both the 4:2:0 and 4:1:1 code paths" check,
not a MAE one - DC-only necessarily reads much higher MAE against
ffmpeg's full-quality reference than the suite's normal threshold, by
design, so `--check` itself isn't the right tool to validate this mode).
Both pass on host and real m68k/big-endian under qemu.

`amiga/mrplay.c` gained a matching `--dv-speed=quality|fast` CLI flag and
an `apply_dv_speed()` helper (mirroring `apply_h264_speed()`'s shape,
including its own no-op guard for any other codec), called once at
decoder open and again after each of the three `mr_decoder_reset()` sites
(seek, `--loop` restart, live/EOF reconnect) - `dv_open()` always starts a
fresh `dv_decoder_t` at `DV_QUALITY_BEST`, so every reset needs the
reapply, the same reason `apply_h264_speed()` itself is re-called at each
of those sites. Default stays Quality (unchanged behaviour for anyone not
passing the flag). Not yet wired into `core/mr_play_options.c` or either
GUI's chooser list - DV files are opened through the same local-file
browser both GUIs already have, so this is reachable today only via a
direct Shell/`mrplay --dv-speed=fast` invocation, matching this file's own
repeated pattern of staging Amiga-only, dev-host-unverifiable GUI wiring
as an explicit follow-up rather than guessing at ReAction/GadTools layout
changes with no way to test them here. `amiga/mrplay.c` itself can only be
reviewed, not compiled, on this dev host (see "Validate against ffmpeg"
above) - the qemu numbers above prove the underlying decode-cost claim,
but the flag's actual real-hardware effect on WinUAE (the report that
started this) still needs a retest to confirm.

The RGB24 conversion `dv_decode()` still unconditionally pays for every
frame (`mr_dv_set_yuv_output()` exists - see the DV decoder section above -
but was never wired into `amiga/mrplay.c`'s display path) is a separate,
larger lever, deliberately not attempted in this same pass: H.264/MPEG-2's
own YUV-indexed-queue integration (`use_yuv_indexed_queue`/
`use_yuv_rgb_queue` in `amiga/mrplay.c`) touches display-mode detection,
queue-slot format selection, and every one of the same four decoder-open/
-reset sites this change touched for the speed knob - real, substantial
surface area to get right with no way to compile or run any of it here,
unlike the speed knob above (a single boolean read fresh every frame, with
no queue-format implications). A future pass extending that exact
machinery to `codec == &mr_codec_dv` (DV's `y_stride == width` exactly, no
macroblock-padding quirk to account for, unlike H.264/MPEG-2's aligned-vs-
visible-width distinction) is the natural next lever if the quality-mode
opt-in alone doesn't resolve the WinUAE report.

## The GUI VQ chooser: the H.264 speed gadget generalized to drive DV too
Direct follow-up to the DV decode speed section above - the user's own
suggestion, once `--dv-speed=` existed but had no GUI control: reuse the
existing H.264 performance chooser (both GUIs already have one) as a
generic "video quality" preference instead of adding a second, DV-specific
gadget, since a real gadget slot is scarce in both windows and the two
controls express the same underlying idea (trade picture quality for
decode speed) for two different codecs.

**The mapping the user asked for - "auto and anything else does fast,
best does quality" - falls directly out of the existing enum, no new
field needed.** `MR_H264_PERF_QUALITY` is the one mode a user picks
specifically for full quality; every other value - `MR_H264_PERF_AUTO`
included, since Auto's own H.264 resolution already defaults to Turbo
(`mrplay.c`'s `effective_h264_speed()`) - already means "prefer speed".
`mr_video_quality_prefers_fast(mr_h264_performance mode)`
(`core/mr_play_options.c`, static) is exactly `mode != MR_H264_PERF_QUALITY`
- no new struct field, no new enum, `h264_performance` itself becomes the
shared VQ value. `append_playback_flags()` now emits `--dv-speed=fast`/
`--dv-speed=quality` derived from that same field, unconditionally and
explicitly in both directions right alongside the conditional
`--h264-speed=` emission - the same "always emit explicitly, like
`--throughput`" discipline this file's own Display-mode/persisted-settings
section already established, since a GUI-launched session's VQ choice
needs to override `amiga/mrplay.c`'s own `MR_DV_SPEED_QUALITY` default
regardless of direction. Safe to always emit both flags regardless of
which codec a launched file turns out to be: `apply_h264_speed()`/
`apply_dv_speed()` in `mrplay.c` both already no-op for a mismatched
codec (`dec->codec != &mr_codec_h264`/`&mr_codec_dv`), the exact same
"emit the flag, let the player ignore it if irrelevant" pattern
`--h264-speed=` itself already relied on before any of this.

**Only the on-screen label changed, not the underlying identifiers.**
`h264_performance`/`G_H264`/`MR_H264_PERF_*`/the `h264_label`/`h264_labels`
variable names are all untouched - renaming those would be a much larger,
purely-cosmetic diff for no functional benefit, and this file's own
"match the scope of the change to what was asked" discipline argues
against it. `mrgui.c`'s ReAction `h264_label` (`LABEL_GetClass()`,
independent of the chooser's own "Auto"/"Quality"/.../"Turbo+" option
list) changes from `"H.264"` to `"VQ"`. `mrgui_gadtools.c`'s GadTools
`h264_labels[]` (`CYCLE_KIND`, each label embeds the full text since
GadTools cycle gadgets have no separate caption) changes each `"H.264:
X"` to `"VQ: X"` - shorter, not longer, than the string it replaces
(`"VQ: Turbo+"` at 10 characters vs. `"H.264: Turbo+"` at 13), so the
existing 144px gadget width needs no resize - deliberately avoiding the
exact class of real, reported bug the AGA copper-doubling section's own
"Copper 2x" GadTools label overflow already hit in this codebase (a
label too long for its box, invisible until a real-hardware report caught
it). No other GUI file (`iptv_gadtools.c`/`youtube_gadtools.c` and their
ReAction counterparts) has its own H.264/VQ chooser - both browsers
inherit play options from the main controller via `mr_master_options.h`'s
`T:` snapshot, so nothing else needed touching.

`tests/mr_iptv_check.c`'s two exact-string `mr_build_player_arguments()`
pins needed updating for the new flag's position in the argument string
(inserted between `--h264-speed=`/`--fast-buffer=` and `--throughput`,
matching `append_playback_flags()`'s own emission order) - both directions
verified explicitly, not just one: `MR_H264_PERF_TURBO`/`_AUTO` now expect
`--dv-speed=fast` in the emitted string (Auto's own case gets a comment
explaining why it's still "fast", not a silent pass), and a new
`MR_H264_PERF_QUALITY` case (added alongside the existing Fast/Turbo/
Turbo+ coverage) checks `--dv-speed=quality` is emitted and
`--dv-speed=fast` is not. `make check` (host, `core/mr_play_options.c`/
`tests/mr_iptv_check.c` are the only non-Amiga-only files this change
touches) and `make check-m68k` both pass unchanged otherwise.

Not yet done, same standing limitation as every other GUI change in this
file: `amiga/mrgui.c`/`amiga/mrgui_gadtools.c` can only be reviewed, not
compiled or run, on this dev host - needs a real-hardware/WinUAE pass to
confirm the "VQ:" label actually renders correctly in both editions (the
GadTools width math above is arithmetic, not a rendered screenshot) and
that picking a VQ mode other than Quality genuinely speeds up DV playback
end to end through the GUI launch path, not just via the CLI flag this
section's own predecessor already measured under qemu.

## MPEG-1/2 B-frame skip, and a real iptvgui/ytgui launch-failure bug caught along the way
Two requests in the same message: "is there any other codecs we have that
could benifit from [the DV speed-mode] settings?" and, once MPEG-1/2 was
picked, a real-hardware bug report (screenshot) - both `iptvgui` and
`ytgui` failing to launch with `invalid playback option near
--dv-speed=fast` and hanging.

**Survey before picking a codec, not a guess.** Cinepak/MSVideo1/MSRLE
have no DCT/transform stage at all - nothing to degrade. MJPEG/MPEG-4
Part 2/H.263/WMV/MSMPEG4v2 are each this project's own from-scratch VLC
decoders with no existing partial-decode/skip mechanism built in - adding
one would be new decoder work, not wiring up an existing lever, unlike
DV's `dv_set_quality()` or H.264's `IVD_SKIP_B`/`IVD_SKIP_PB`. MPEG-1/2
(`vendor/libmpeg2`) does have exactly that existing, unused lever:
`mpeg2_skip(mpeg2dec_t*, int skip)`, libmpeg2's own public API for
skipping a picture's macroblock/slice decode entirely - never wired into
`core/mr_mpeg2.c` before this.

**Same safety argument as H.264's B-skip, real for MPEG-2 by spec design
rather than assumed**: a B picture is never a reference for any later
picture in MPEG-1/2 (that's what "B" means), so skipping its decode can
never corrupt the I/P chain everything else depends on - only I and P
frames ever get referenced, and those are never touched by
`MR_MPEG2_SPEED_FAST`. `mr_mpeg2_set_speed_mode(dec, MR_MPEG2_SPEED_FAST)`
(`core/mr_mpeg2.h`/`.c`) stores a `skip_b` flag on the adapter's own state
and calls `mpeg2_skip(s->decoder, is_b)` once per picture, right after
each `STATE_PICTURE`/`STATE_PICTURE_2ND` return - unconditionally, not
just for B, since libmpeg2 never resets `nb_decode_slices` between
pictures on its own (only at sequence-header time), so leaving a previous
B's skip=1 in effect would silently skip the next I/P too.

**The first cut looked complete and was actually broken - caught by
testing, not by re-reading the code more carefully.** `mpeg2_skip()` was
being called correctly (confirmed with a temporary debug `fprintf`) and
`pump()`'s existing display-trigger condition
(`state == STATE_SLICE || STATE_END || STATE_INVALID_END`) seemed like it
should naturally exclude a picture whose slices were never processed. It
didn't: frame count stayed at 50 (unchanged from Quality mode) instead of
dropping by the B count, and a `--check` run against the ffmpeg reference
showed MAE spikes of 13-114 at exactly 32 frame positions -
`ffprobe -show_entries frame=pict_type` confirmed 32 B/5 I/13 P in this
fixture, an exact match. So skipped B pictures *were* reaching the
display queue, just with corrupted/stale pixel data, not simply being
counted wrong.

Root cause, found by reading `vendor/libmpeg2/libmpeg2/header.c`: after a
picture's header is parsed, `mpeg2dec->state` is set to `STATE_SLICE`
*before* any slice is actually decoded - it means "ready to decode
slices," not "a slice was decoded" - so `mpeg2_parse()` returns
`STATE_SLICE` for a fully-skipped picture exactly the same as for a
normally-decoded one, with `display_fbuf` already pointing at that
picture's buffer (in the skipped case, one nothing ever wrote this frame
into - stale data from whatever decode last used that buffer slot).
Trusting `display_fbuf` alone, as the first cut did, cannot tell a
skipped picture from a real one. libmpeg2 itself already flags this exact
situation, though: `header.c` sets `PIC_FLAG_SKIP` on the picture struct
whenever `nb_decode_slices` came back 0. `pump()`'s display-trigger
condition now also requires
`!(s->info->display_picture && (s->info->display_picture->flags &
PIC_FLAG_SKIP))` - checking the picture actually about to be displayed,
not just its buffer pointer. `STATE_INVALID_END` (libmpeg2's own way of
handing back a final display picture when a stream ends with no explicit
sequence-end code) needed the identical check, for the same reason.

Verified correct three ways once fixed, not just "frame count now looks
right": a quick `--ppm` dump + SHA256 subsequence match on host confirmed
byte-exact correctness at exactly the expected non-B frame indices; a new
`tests/mr_mpeg2_bskip_check.c` decodes `test_mpeg2.ts` twice (Quality and
Fast) and asserts every Fast-mode frame is an exact byte match to *some*
frame of the Quality decode, in order - 50 frames -> 18, 32 B pictures
dropped, zero mismatches - deliberately *not* a `--check`-against-ffmpeg
test, since `--check` compares by sequential frame index and a
frame-dropping mode desyncs that index the moment even one frame is
skipped (this was tried first and produced a misleading, steadily-growing
MAE that looked like accumulating corruption but was really just
index drift against the wrong reference frame); and the same test passes
bit-for-bit identically cross-built for real m68k/big-endian under qemu
(`tests/run_m68k_check.sh`), the first time `test_mpeg2.ts` has been
exercised on m68k at all. `mr_mpeg2.h`'s own header comment on
`mr_mpeg2_speed_mode` was written before this debugging happened and
undersells the fix (it describes the display-buffer handoff as safe via
"pump() only queues on STATE_SLICE/STATE_END" without mentioning the
`PIC_FLAG_SKIP` check that's the actual reason `STATE_SLICE` alone isn't
enough) - accurate in the implementation's own comment in `mr_mpeg2.c`,
just not yet corrected in the header's.

Wired into `amiga/mrplay.c` exactly like DV's own speed lever:
`apply_mpeg2_speed()` mirrors `apply_dv_speed()`'s shape (no-op for a
mismatched codec, called at decoder-open and after all three
`mr_decoder_reset()` sites - seek, `--loop` restart, live/EOF reconnect -
since `mpeg2_open_decoder()` always starts a fresh state at
`MR_MPEG2_SPEED_QUALITY`), a new `--mpeg2-speed=quality|fast` CLI flag,
and `tests/mr_decode.c` gained the matching flag for host-side testing.
Reuses the same shared "VQ" chooser both GUIs already have for H.264/DV
(see the GUI VQ chooser section above) - no new gadget, `core/
mr_play_options.c`'s `append_playback_flags()` emits
`--mpeg2-speed=fast|quality` unconditionally and explicitly, derived from
the identical `mr_video_quality_prefers_fast(o->h264_performance)` value
`--dv-speed=` already uses (Quality picks the codec's own quality mode,
every other VQ choice - Auto included - picks fast).

**The reported iptvgui/ytgui bug turned out to be the exact same class of
gap `--mpeg2-speed=` was about to reintroduce if left unfixed - caught
and fixed for both flags in the same pass.** `append_playback_flags()`
had already been unconditionally emitting `--dv-speed=` (from the earlier
DV work), but `mr_play_options_parse()` - the function both `iptvgui` and
`ytgui` use to re-parse their own inherited launch argv (confirmed via
grep across `amiga/iptv_gadtools.c`/`iptv_reaction.c`/
`youtube_gadtools.c`/`youtube_reaction.c`, all four call it) - had never
been taught the flag. An unrecognized flag makes the whole parse fail
("invalid playback option"), which is exactly the reported symptom: the
browser launches `mrplay`/itself with a self-built argv it then can't
parse back, and hangs waiting for a status port that never reports ready.
Fixed by adding a recognize-but-discard case for `--dv-speed=` to
`mr_play_options_parse()` (it carries no information not already present
in `--h264-speed=`/the struct's own VQ default, so there's nothing to do
with it but accept it) - and, since `--mpeg2-speed=` was about to be
emitted unconditionally by the same function for the same reason, its own
matching parse case was added proactively in the same change, rather than
waiting to reproduce the identical bug a second time for a second flag.
User confirmed the report covered `ytgui` too ("ytgui does teh same
thing") - already covered by construction, since both browsers share this
one parse function.

`tests/mr_iptv_check.c` gained: direct regression pins for both flags
(`--dv-speed=fast/quality/bogus`, `--mpeg2-speed=fast/quality/bogus`,
each checked for accept/accept/reject-with-a-useful-error); a generic
round-trip test that builds a real `mr_build_iptv_arguments()` string
from default options, tokenizes it, and re-parses it through
`mr_play_options_parse()` end to end - a guard against this whole *class*
of gap (a flag added to the emission side with no matching parse case)
recurring for some future flag, not just these two; and every existing
pinned exact-string `mr_build_player_arguments()`/`mr_build_iptv_arguments()`
assertion elsewhere in the file updated to expect `--mpeg2-speed=`
alongside the `--dv-speed=` it already expected, in both the fast and
quality directions.

`make check` (host) and `make check-m68k` (real m68k/big-endian under
qemu) both pass end to end with the full change in place - the decoder
logic, the CLI/GUI-launch-string plumbing, and the regression tests all
verified together, not just the isolated new test in each. `amiga/
mrplay.c`/`mrgui.c`/`mrgui_gadtools.c` remain Amiga-only and can only be
reviewed, not compiled or run, on this dev host (see "Validate against
ffmpeg" above) - the launch-failure fix's actual effect on a real
iptvgui/ytgui session, and the MPEG-2 Fast mode's real-hardware speedup,
both still need a real-hardware retest to confirm, the same standing
caveat as DV's own speed mode before it.

## YouTube below 360p, Smoosh, RTG Half, and the Skip Frames trigger
A real PiStorm log (YouTube 360p, Turbo, P96 overlay refused with
`PIPERR_OUTOFPENS` so it fell back to CGX) and a real A1200/68060 log (same
source, AGA, Turbo+) prompted three requests: a lower YouTube quality, a VQ
mode between Turbo and Turbo+, and a cheaper RTG path.

**YouTube has no MintVID-playable format below 360p any more.** Probed
live from this dev host against `youtubei.googleapis.com` (youtube.com and
googlevideo.com are blocked here, the API host is not) with every client
identity yt-dlp 2026.08.19 uses: WEB with the Safari UA returns
UNPLAYABLE - yt-dlp's own source documents that since 2026.07 its muxed
144p-1080p HLS (itags 91-96) is "only returned with some logged-in or
trusted sessions"; ANDROID offers muxed itag 18 (360p) only, with 144p
existing solely as video-only adaptive itag 160 behind a signatureCipher;
IOS returns video-only/audio-only adaptive formats and a `demuxed=1` HLS,
both of which yt-dlp marks as needing a GVS PO token, and which would need
two simultaneous HTTPS connections that the AmiSSL design rules out.
yt-dlp also notes ANDROID_VR 1.65.10 has been 403'd for every format since
2026.08.17 (MintVID still tries it as a fallback). Nothing was changed in
the resolver.

**Why Turbo and Turbo+ are so far apart on YouTube: itag 18 is H.264
Baseline (`avc1.42001E`).** No B-frames, so Turbo's `IVD_SKIP_B` skips
nothing, and every P picture is a reference, so the only clean choices are
"decode everything" or "keyframes only". The "Skip Frames" micro-rescue
escalation (`IVD_SKIP_PB` until the next IDR) is the clean middle ground:
bursts of motion, then a freeze.

**Smoosh (`--h264-speed=smoosh`, `MR_H264_PERF_SMOOSH`, GUI "VQ: Smoosh")
is the deliberately unclean one.** Turbo's decode policy, plus: an ordinary
P/B access unit more than one frame period late (the same
`mono_media_clock_us - (pts + container_pts_adjust_us) > period_us` signal
as `pts_late`) is never handed to libavc
(`mr_h264_set_drop_nonsync()`, returns `MR_SKIPPED`). The next surviving P
picture predicts from a stale reference, so motion smears ("datamosh")
until the next keyframe restores the picture exactly. The classifier
(`h264_au_droppable()` in `core/mr_h264.c`) walks both AVCC and Annex-B
NALs and never drops an IDR, an I/SI slice (first two slice-header ue(v)
fields, emulation-prevention aware) or anything carrying SPS/PPS; anything
it cannot parse counts as a keyframe. Smoosh applies regardless of "All
Frames", and disables micro-rescue's entry (its `IVD_SKIP_PB` escalation
would freeze the very thing Smoosh keeps moving). Live-resync's
reference-only catch-up clears the drop flag. An earlier ad-hoc host
experiment (dropping 1-in-2..1-in-5 P frames of a 640x360 Baseline clip)
measured MAE 30-78 against ffmpeg until the next keyframe - visibly
smeared, which is the point; it is not a quality mode.

`tests/mr_h264_smoosh_check.c` (host + `check-m68k`, also clean under
ASan/UBSan) pins it differentially against our own full decode on a new
3-keyframe Baseline fixture (`test_h264_gop.mp4` and its TS remux, so both
input paths) and on the High-profile B-frame fixtures: dropping everything
droppable leaves exactly the keyframes, byte-identical; a 2-in-3 drop
pattern really drops pictures and every keyframe afterwards is byte-exact
again. Baseline decodes with zero libavc errors under drops; the B-frame
streams get one refused picture (a P whose references were dropped -
mrplay logs `h264-decode-error` and carries on) and still resynchronise at
the next keyframe.

**RTG (Half) (`--rtg-half`, `MR_DISPLAY_RTG_HALF`).** The PiStorm log
showed `yuv-rgb=57 ms` against `vdecode=72 ms` per frame: the CPU colour
conversion for the CGX fallback cost nearly as much as decoding. Half mode
opens the window at (w/2)x(h/2) and converts straight to that size with
`mr_yuv420_to_rgb24_half()`/`_bgr24_half()` (rounded 2x2 luma average plus
that block's own 4:2:0 chroma sample, same table formula as the full-size
converter), so both conversion and blit shrink ~4x; under qemu-m68k at
640x360 it measured 1.25-1.41 ms/frame against 3.44-3.74 ms for the
full-size converter (instruction count only - the 4x smaller write
traffic, which qemu does not model, should add to that on hardware).
Byte-exact against an independent reference in `tests/mr_yuv_check.c`,
including padding bytes and odd sizes. H.264 only; it engages only when
the opened backend is an RGB RTG one - a P96 overlay (YUV, board-scaled)
or an AGA screen is closed and reopened at full size. Enlarging the half
window, or F for fullscreen, goes through CGX's CPU scaler and gives some
of the saving back.

**Skip Frames trigger (`--skip-trigger=200..2000`, GUI "Skip after").**
`MICRO_RESCUE_ENTRY_US` (700 ms) is now only the default; the exit
threshold is `min(200 ms, trigger/2)` (`skip_trigger_exit_us()`) so a
0.2 s trigger cannot flap. `mr_play_options.skip_trigger_ms` is always
emitted (clamped) and re-parsed like `--throughput`, and both GUIs grey
the chooser out under "All Frames" or VQ Smoosh, where it has no effect.
`mr_play_options` grew a field, so a previously saved
`ENVARC:MintVID.settings` is discarded once (the size check in
`mr_saved_options.h`), falling back to defaults.

**First real-hardware report (A1200/68060, IPTV BBC One 192x108 AAC-LC,
AGA + Kalms, `--throughput`): Smoosh keeps the picture moving at near real
time, but playback periodically "disconnected, buffered, reconnected".**
That cycle is live-resync, not the network (segments 2-5 each arrived in
3-4 ms): 24 Paula starvations in ~13 s, audio-rescue episodes mostly
exiting on their time limit, the audio clock stalling, and after 4 s of
that `mono_media_clock_us > audio_media_clock_us + LIVE_RESYNC_BEHIND_US`
fired the flush-and-refill. Rescue itself was still fully decoding every
video packet it read past (~50 ms each at 192x108 here). Under Smoosh only,
the drop condition now also covers `rescue_active` and an audio cushion
below `AUDIO_RESCUE_ENTRY_MS` (audio first, video takes what is left), and
the live-resync fast-forward drops non-keyframes too instead of decoding
every reference, shortening the "Buffering..." gap. The final `--time`
line reports how many drops were for audio. Not yet retested on hardware.

**That audio-first rule exposed a latent input bug: a YouTube session under
Smoosh stalled and ESC stopped working (the mouse still moved).** The main
loop only read input in audio-rescue, live-resync/reconnect and the
presentation block, and the presentation block needs a queued, due frame.
Dropping every P picture to protect audio on a long-GOP YouTube stream can
keep the queue empty indefinitely, so nothing read ESC. Two fixes: the loop
now polls input itself (every `IDLE_EVENT_POLL_US`, 40 ms) whenever no
frame is queued, reading fresh events with the same defer rule
`service_player_during_io()` already uses (quit acted on at once, anything
else left in `deferred_player_event` for the normal handler); and Smoosh
never drops for longer than `SMOOSH_MAX_DROP_RUN_US` (1 s) in a row, so the
picture keeps moving at >= ~1 fps whatever audio or lateness say. The
matching IPTV log (audio drops working: 90 of 170 drops were for audio)
also showed one unexplained ~4 s scheduler gap before a live-resync
(`audio-gap=4121 ms`, previous phase `cgx-prepare/transfer`, no network
block or long decode reported) - not diagnosed; the pre-change log had a
7.3 s `longest-service-gap` too, so it is not new with these changes.

The ReAction GUI keeps the new chooser in a file-level `g_skip_after`
instead of threading another `Object *` through every
`read_play_options()` caller. `mrplay.c`, `mrgui.c` and `mrgui_gadtools.c`
cannot be compiled on this dev host (CI's real AmigaOS `build` job is the
compile check); none of Smoosh, RTG Half or the trigger has run on real
hardware yet.

## High-motion H.264: two-lane SWAR motion compensation
A PiStorm 600 report (YouTube 360p, RTG Half, Turbo + Skip Frames): smooth
on stage, falling apart on fast crowd shots. YouTube's only playable 360p
format (itag 18) is **Baseline, so CAVLC** - none of the CABAC asm/profiling
above applies to it. A calm-vs-busy comparison of Baseline 640x360 clips
(testsrc2 vs a zooming, noisy mandelbrot, same bitrate cap) showed the extra
cost of motion is almost entirely motion compensation: under Turbo, luma
bilinear was ~22% and general-case chroma ~8% of busy-clip decode.

Timing method worth reusing: wall-clock under qemu on this shared host varies
by +/-10%, which is useless for a 5% change. `valgrind --tool=callgrind
--smc-check=all qemu-m68k ./decoder ...` counts the host instructions of the
whole emulated run and is deterministic to the megainstruction. Instruction
count is also a fair proxy for Emu68 (a JIT like qemu), unlike the real-060
cache questions the qemu note at the top warns about. It is repeatable, not
exact: relinking with code at different addresses moved whole-decode counts by
2-3% with no real change (68040 Quality flipped from -1% to +2% that way), so
settle anything that small with a microbenchmark of the kernel itself.

`ih264_mc_degrade.c` now filters two samples per 32-bit register: split a
longword load into two 16-bit lanes with `0x00ff00ff`, apply the weights
with ordinary multiplies (a tap sum never leaves its lane), shift, mask,
recombine. Luma bilinear (all 15 fractional slots) and chroma eighth-pel
(`chroma_1d()`/`chroma_general()`, every speed mode, replacing
`ih264_m68k_chroma_mc.S` on the production path - that kernel is now only
exercised by `mr_h264_m68k_check`) are both exact, pinned by
`mr_h264_mc_degrade_check` and by byte-identical `--ppm` output before and
after, Turbo and Quality alike. Result (m68k code, 68060 flags): Turbo -21%
on the busy clip, -8% on the calm one; Quality -7% on the busy clip. At
68040 flags (the MintVID040 build PiStorm runs): Turbo -16% busy, -5% calm.
Every multiply is the ordinary 32x32->32 `muls.l`, hardware on 68020-68060
(the 060 only lacks the 64-bit-result forms); the 68060 disassembly gate
covers this file. A direct kernel benchmark, all 245 chroma cases, identical
checksums: SWAR chroma costs 326M against 933M for `ih264_m68k_chroma_mc.S`,
at both `-mcpu=68040` and `-mcpu=68060` (GCC emits the same code for both).

The obvious alternative measured *worse*: per-slot constant-weight copies of
the old per-sample loop (GCC turns `3*x` into shift+add) cost +2%, because
under qemu and Emu68 a multiply is a single translated instruction. The four
existing `luma_bilinear_qpel_*` functions of that kind were removed. On a
real 68030/040 multiplies are slow, so SWAR (half the multiplies) should win
there too, but that is unmeasured. Nothing here has run on a PiStorm yet.

## YUV420 -> RGB24 (RTG): clip table and a single-block kernel
The RTG queue path (`queue_copy_yuv_rgb24()` in mrplay.c) spends nearly as
long converting colour as decoding: a PiStorm log showed `yuv-rgb=57 ms`
against `vdecode=72 ms`. Two changes in `core/mr_yuv.c`/`mr_yuv_m68k.S`,
both byte-exact:
- Saturation is a byte table (`g_clip`, index -258..534, the full range the
  `>>8` channel sums reach) instead of two compares and branches per
  channel. `check_clip_extremes()` in `tests/mr_yuv_check.c` runs every U x V
  pair at the Y extremes, and ASan fails it if the table is one entry short
  at either end.
- The m68k kernel reads every coefficient from one 8 KB block through one
  address register: d6 is loaded with a slot base, `move.b` drops the sample
  into its low byte, and `(a5,d6.l*8)`/`(4,a5,d6.l*8)` fetch both addends of
  a pair. Per pixel it is 13 instructions. BGR24 reuses it with the planes and
  table slots swapped.

Host instructions per 640x360 frame (callgrind over qemu-m68k, 68040 flags):
full size 34.6M -> 20.0M (asm; the new C is 21.8M), half size (RTG Half, C
only) 13.1M -> 10.4M. A half-size kernel was written and measured 2% slower
than the C, so it was dropped.

**On a real PiStorm it was ~5x, not 1.7x.** Same YouTube 360p/Turbo/CGX
windowed setup as the 57 ms log: `yuv-rgb=11.7 ms` per frame, identical on
the MintVID040 and 060 builds (Emu68 JIT). qemu predicted 34.6M -> 20.0M.
The likely reason is the removed compare-and-branch clipping: two
data-dependent branches per channel, six per pixel. qemu counts them as a
few host instructions each. Emu68 evidently charges far more. Treat qemu
counts as a floor for branchy code on PiStorm, not a prediction.

The same logs give the rest of the frame budget (640x360, 30 fps source):
`vdecode` ~26 ms, `yuv-rgb` 11.7, `display` (CGX WritePixelArray to a
B8G8R8A8 screen) ~4.3, AAC ~2.5 per video frame. That is ~45 ms against
33.3, so it plays at ~17.5 fps with audio rescue firing continuously. The
P96 PIP overlay is unavailable on the PiStorm's RTG (`PIPERR_NOTAVAILABLE`
for every MemoryWindow format), so CGX is the path there.

**Page-align code before comparing qemu counts.** qemu does not chain
translated blocks across a 4 KB page, so a hot loop that straddles one pays a
TB lookup every iteration. The same kernel read 20M in one link and 45M in
another. Build benchmarks with `-falign-functions=4096` and a `.balign 4096`
before asm entry points. This is a much bigger layout effect than the 2-3%
noted in the motion-compensation section above.

Two qemu costs also steered the kernel, and both plausibly apply to Emu68:
word-sized ALU results on a data register (`add.w #256,d6`) and
postincrement stores. Each costs several extra host instructions. The kernel
loads slot bases from the stack and advances pointers once per quad.

## Baseline H.264 profile: dead deblocking work and the chroma split
YouTube's only playable format is Baseline/CAVLC (see above), and on the
PiStorm its decode was ~26 of ~45 ms per 640x360 frame. Profiled with
`tools/qemu_tbprof.sh`: it counts **m68k instructions per function** from
qemu's own block log (`-d in_asm,exec,nochain`), so unlike callgrind over
qemu it is exact and immune to the page-layout effect above. Unexported
labels in a `.S` file are charged to the nearest preceding symbol. In
this build `__wrap_ih264d_read_coeff4x4_cabac` really meant the inverse
transform kernel's helpers. Profiled clips were x264 Baseline, level 3.0,
640x360, 30 fps, ~600 kbps, 120 frames under Turbo: a calm testsrc2 and
a busy mandelbrot-plus-noise.

Three lossless fixes, in the libavc fork (`boingball/libavc` branch
`claude/baseline-decode-speedups`, one commit on top of the previously
pinned `cb8d7c3`). That branch was then merged into the fork's `main`
(`28825e1`) and the submodule now tracks `main`. This also brought in
`main`'s upstream sync, which had never been tested with MintVID. It
merged cleanly. Frames were byte-identical to the branch-only build in
all four speed modes on every clip above. Guest instruction counts
matched to within 200. `make check` and `make check-m68k` pass. The
three fixes:
- **Turbo still ran the per-MB deblocking pass.** With deblocking
  disabled by the app (`i4_degrade_type` bit 1), the slice parsers skip
  setting each MB's deblocking mode and boundary strengths, but
  `ih264d_deblock_mb_nonmbaff()` still ran for every MB. It re-derived
  alpha/beta per edge on stale data and found bs == 0. That was ~9% of
  the calm clip. It now treats such pictures as `MB_DISABLE_FILTERING`
  and keeps only the pointer bookkeeping. A stale non-zero strength left
  from an earlier Quality-mode picture can no longer filter a Turbo
  picture either.
- **The intra-pred line copy is off for those pictures.** It saves each
  row's pre-deblocking pixels. Unfiltered, the frame holds the same
  pixels, and `ih264d_process_intra_mb()` reads them from there (1.5%).
- **The 420SP -> 420P chroma split** (`ih264d_fmt_conv_420sp_to_420p`,
  8.5%) walked both planes by `j * 2` index: three m68k instructions per
  byte. One post-incremented source pointer, unrolled, is about two. It
  is 41% cheaper.

Output is byte-identical to the old build in Quality/Balanced/Fast/Turbo
on both clips and the repo's H.264 fixtures. `make check` and
`make check-m68k` pass. Guest instructions: calm 491.1M -> 425.0M
(-13.5%), busy 728.8M -> 662.7M (-9.1%).

What is left, busy clip after the fixes: `luma_bilinear` 21%, chroma MC
~16%, residual decode + inverse transform ~20%, MV bookkeeping
(`form_mb_part_info_bp`, `mv_pred_ref_tfr_nby2_pmb`, `rep_mv_colz`) ~11%.
The MC loops are already two-lanes-per-register with one multiply per
lane. Further MC savings would have to trade quality, e.g. snapping
quarter-pel vectors to half-pel so the cheap averaging paths run.
`rep_mv_colz` is not B-only waste: the same copy fills the MV bank that
later MBs read as neighbours. The remaining 3.7% chroma split could go
entirely: request `IV_YUV_420SP_UV` with shared display buffers and
libavc decodes straight into our buffers. But every YUV consumer in
mrplay.c (RGB24, Half, Y4U2V2, HAM, AGA dither, direct planar) reads
planar chroma today.

## Git
Work happens on branch `claude/amiga-video-player-riva-9pz78q`. Commit with
clear messages; do not open a PR unless asked.

## AAC reduced-size IMDCT (decimated output)
AAC used to be decoded at full rate and then decimated by dropping samples
(`emit_pcm_stride()`), which aliases everything above the new Nyquist. Helix
now has `AACSetOutputDecimation(h, 2|4)` (boingball/ESP8266Audio branch
`claude/aac-decimated-imdct`, pinned through MintAMP's branch of the same
name, compiled only with `-DAAC_ENABLE_DECIM`, which every MintVID build line
sets). It runs the IMDCT at 1/2 or 1/4 size on the lowest part of the
spectrum and emits band-limited PCM at the lower rate directly.
`aac_apply_decim()` in `audio/mr_audio_decode.c` turns it on whenever the
adapter's stride is 2 or 4, and falls back to dropping samples if Helix
refuses.

The tables come from `gen_decim_tabs.py` next to the Helix sources.
`--check` reproduces the existing 128/1024 tables bit for bit. Output is a
constant (d-1)/2-input-sample time offset from the full-rate decode.

Measured under qemu (instruction counts, 2 s of 48 kHz stereo):
- 68060: −41% at 1/2 rate, −63% at 1/4 rate.
- 68040: −31% at 1/2 rate, −47% at 1/4 rate.

m68k output is bit-exact with the host. `tests/mr_aac_decim_check.c`
(`make check-audio`) compares against ffmpeg's decode, ideally resampled:
- 70/56 dB in-band SNR, 25/21 dB overall.
- Dropping samples scores 4/−2 dB overall on the same fixture.

PNS, dequant and stereo processing still run on the discarded upper band.
TNS filters across frequency, so skipping them needs care, and it has not
been done. Real-hardware speed is still unconfirmed.

## Turbo+ skipped ~10 s of audio at the first keyframe
A YouTube 360p log under Turbo+ (keyframes only) showed `fifo-dropped=
219541` samples: almost exactly 10 s at 22050 Hz, heard as the audio
jumping forward to the next keyframe. With no displayable picture until
that keyframe the video queue stays empty, and `can_decode` in mrplay.c
reads whenever `qcount < target_depth`. So the loop read packets flat out.
The audio filled Paula's ~4 s software FIFO, and `fifo_push()` silently
dropped the rest until the keyframe arrived.

The loop now stops reading once buffered audio is within
`AUDIO_FIFO_HEADROOM_MS` (1 s) of `audio_capacity_ms()`, the FIFO's size.
That leaves room for the in-flight Paula requests and one more packet.
It only applies after playback has started: before that Paula does not
drain, so holding reads could wait forever for a first picture. The
threshold (3 s) sits above `AUDIO_CUSHION_TARGET_MS` (2.5 s), so normal
cushion top-up never reaches it. Coarsely interleaved files that used to
lose the tail of an audio burst now pause reading until it drains
instead. Amiga-only code, not compiled here; `fifo-dropped` in a `--time`
log should now stay at 0.
