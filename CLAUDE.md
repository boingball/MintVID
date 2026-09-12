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
  `--live-diag` is on, matching this file's established durable-log
  pattern (see the `NAS0:MintVID.log` notes above) so the very last state
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
