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
