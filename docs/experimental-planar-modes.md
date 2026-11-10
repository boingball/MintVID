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
  separate frame C2P pass is produced. This first implementation is portable
  C, not an assembly kernel; packing may cost more than an existing tuned
  C2P path. Colour/dither and nearest-neighbour sampling match ECS32.

Fused32 supports AGA and OCS/ECS. OCS/ECS uses the existing low-resolution
screen fitter (five planes cannot use its four-plane HIRES path). Fused32
is **not HAM6**; selecting it while HAM6 is selected opens ordinary 32-colour
output instead. Shadow can be compared in HAM6 without changing its colours.
The modes do not apply to RTG, native Workbench windows, or PiStorm players.
Copper-assisted doubling is not enabled for these experiments.

Example local-file tests:

```
mrplay video.mp4 --aga --ecs32 --riva-c2p --time
mrplay video.mp4 --aga --ecs32 --shadow --time
mrplay video.mp4 --aga --fused32 --time
```

Keep scale, video frame policy, audio settings and clip identical. Compare
conversion plus presentation time and overall playback; a cheaper Chip-RAM
copy can be offset by extra Fast-RAM work. Compare Fused32 against ECS32,
not a higher-colour mode, to separate conversion savings from colour loss.
Use `--ham6 --shadow` to compare the shadow strategy in HAM6.

Validation: `make -C player check-planar-modes` compares fused YUV/RGB output
against existing resize/dither plus C2P, including odd sizes, padding, up/down
scaling and output guards. It checks first/unchanged/changed shadow copies
and five/eight-plane queue transitions. Hardware display and speed still
need real-Amiga testing.
