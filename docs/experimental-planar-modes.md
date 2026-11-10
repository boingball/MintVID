# Shadow and Fused32 video experiments

Both are opt-in choices in the ReAction and GadTools **C2P** selector.
Existing choices and saved values are unchanged. Neither mode promises a
speedup: measure on the real Amiga with the same clip and playback policy.
Both require Fast RAM and use the native full-screen planar backend.

* **Shadow** (`--shadow`): retain the selected indexed, EHB, HAM6 or HAM8
  mode. Convert into retained Fast-RAM bitplanes using the existing RiVA
  converter, then compare longwords against a Fast-RAM shadow and write only
  changed words to the screen. The first presentation writes everything.
  The shadow belongs to the current screen and is discarded on close/reopen.
  This is inspired by Ami3D PLANAR5FS; no Ami3D renderer is imported.
* **Fused32** (`--fused32`): open a five-plane, 32-colour indexed screen,
  overriding HAM, EHB, pixel doubling and other colour-depth choices.
  H.264's YUV420 is resized, colour-converted, ordered-dithered and packed
  directly into five planes in the decode-ahead queue. RGB codecs use a
  corresponding RGB-to-five-plane path. No full chunky indexed image or
  separate frame C2P pass is produced. For widths divisible by 32 with no
  horizontal resize or padding, and identity/integer vertical sampling, a
  table-driven m68k assembly worker converts and transposes 32 pixels at a
  time. Other sizes use an internal portable fallback; the selectable C
  experiment was retired after real-68060 measurements. The startup path is
  reported as `fused32-asm` or `fused32-fallback`. Colour/dither and nearest-neighbour
  sampling match ECS32. The RGB fallback remains portable C.

Fused32 supports AGA and OCS/ECS. OCS/ECS uses the existing low-resolution
screen fitter (five planes cannot use its four-plane HIRES path). Fused32
is **not HAM6**; selecting it while HAM6 is selected opens ordinary 32-colour
output instead. Shadow can be compared in HAM6 without changing its colours.
The modes do not apply to RTG, native Workbench windows, or PiStorm players.
Copper-assisted doubling is not enabled for these experiments.

Example local-file tests:

```
mrplay video.mp4 --aga --ecs32 --riva-c2p --benchmark
mrplay video.mp4 --aga --ecs32 --shadow --benchmark
mrplay video.mp4 --aga --fused32 --benchmark
```

Keep scale, video frame policy, audio settings and clip identical. Compare
conversion plus presentation time and overall playback; a cheaper Chip-RAM
copy can be offset by extra Fast-RAM work. Compare Fused32 against ECS32,
not a higher-colour mode, to separate conversion savings from colour loss.
Use `--ham6 --shadow` to compare the shadow strategy in HAM6.

## Summary-only benchmarking

Use `--benchmark` instead of `--time` on the real 68060. Startup describes the
actual screen and conversion path. During normal playback there are no
periodic statistics, audio-gap, clock/rescue or pipeline trace messages.
Counters accumulate until EOF/Esc; after the video loop and audio drain, the
summary prints totals and per-operation timings. A finite clip closes by
itself. This mode requires video output; it rejects `--no-video`.

Timing collection still has overhead, so compare identical local clips,
audio settings and frame policy. Output includes conversion-frame counts
separately from presented frames (queued frames can be dropped), and counts
rare EOF-drained RGB frames separately. Elapsed playback ends when the main
video loop exits, excluding audio-tail drain and report output. Decode and
display operation counts refer to that main loop. Do not add averages with
different denominators or interpret final `encode=0` as zero YUV conversion.

```
mrplay Work:youtube-144p.mp4 --aga --ecs32 --riva-c2p --benchmark >NAS0:baseline32.txt
mrplay Work:youtube-144p.mp4 --aga --ecs32 --shadow --benchmark >NAS0:shadow32.txt
mrplay Work:youtube-144p.mp4 --aga --fused32 --benchmark >NAS0:fused32-asm.txt
```

For diagnosing a hang, verbose `--time` remains available. Its report window
now restarts after output and Flush complete, and its own audio servicing
cannot emit additional audio-gap messages while the report is being printed.

Validation: `make -C player check-planar-modes` compares fused YUV/RGB output
against existing resize/dither plus C2P, including odd sizes, padding, up/down
scaling and output guards. It checks first/unchanged/changed shadow copies
and five/eight-plane queue transitions. It also executes the real periodic
report block with a simulated nine-second report plus one-second Flush to
check the cadence regression and quiet-mode suppression.
`sh player/tests/run_planar_modes_m68k.sh` (from the repository root) executes
28 assembly cases per CPU tier under QEMU, covering all Bayer phases,
padded source strides, and identity/integer vertical reductions, while also
checking the C fallback and scanning the new kernel for forbidden 060 forms.
Hardware display, uninterrupted summary-mode playback and speed still need
real-Amiga testing.

## AAC decoder dependency

This branch pins MintAMP 2.0 plus its 68060 AAC scheduling change. It also
includes MintAMP's MP3 multiply fix from #522. Normal submodule updates use
that exact tested commit; they do not follow a moving upstream branch.
The AAC scheduling helper is enabled only for DCT4, IMDCT/windowing and TNS
on 060 builds. FFT/PNS/dequant retain the existing helper; 030/040 code is
unchanged. See the pinned MintAMP docs/aac-68060-speed.md for validation and
limitations. Real-060 speed still needs measuring; the exploratory gain is
about 3% fewer executed m68k instructions, not a promised playback speedup.

`AAC060_REG5=0` is a build-time reference switch for the per-stage helper.
The shared unsigned power-of-two TNS simplification remains enabled. An
updated configuration stamp rebuilds audio objects when the flags or pinned
MintAMP commit change, avoiding stale decoder objects during A/B builds.
The normal `--fused32` choice remains assembly-first; the selectable
`--fused32-c` experiment is retired. Portable reference conversion remains
internal for tests and unsupported layouts.
