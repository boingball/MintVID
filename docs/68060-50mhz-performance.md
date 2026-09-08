# MintVID — 68060 @ 50 MHz Codec Performance & Compatibility

This page records a real-hardware MintVID playback test pass on an Amiga 1200
with a Motorola 68060 running at 50 MHz. It is intended as a practical snapshot
of what the machine actually did with a fixed set of short codec samples, not a
claim that every file using the same codec will behave identically.

The important distinction is **decoder support vs. real-hardware behaviour**.
A codec can be present and host-validated while still exposing a performance,
display, demux, timing, or big-endian/68k-specific problem on a real A1200.
Likewise, a result marked **Too Slow** means the sample decoded but was not
practical at the tested size; it does not mean the codec is unsupported.

## Test system

| Item | Test configuration |
|---|---|
| Machine | Amiga 1200 |
| CPU | Motorola 68060 @ 50 MHz |
| Video hardware | AGA |
| MintVID build | 68060 build under active development |
| Test date | 8 September 2026 |
| Sample duration | 5 seconds |
| Nominal sample rate | 25 fps from the generated test filenames |
| Test heights | 50, 100, 150, 200 and 360 pixels |

> **Note on the source sheet:** the raw export contains an unlabeled four-column
> result area. This page preserves it as **Result 1 / Mode 1 / Result 2 / Mode 2**
> rather than guessing what the missing headings originally were. The export also
> shows MPEG-1 as `50/1` in its FPS field and many other 25 fps values as
> `25-Jan`; the tables below use the filenames as the nominal 25 fps test label
> and do not reinterpret those probe/export values.

The `50p`, `100p`, `150p`, `200p`, and `360p` names below refer to the target
**image height**, not the frame rate.

## Result legend

| Result | Meaning |
|---|---|
| **Perfect** | Played cleanly at the tested settings |
| **Near Perfect** | Essentially usable with a small imperfection |
| **Slight Stutter** | Usable but not completely smooth |
| **Stuttering** | Clearly below smooth playback |
| **Too Slow** | Decoding/playback was not practical at this size |
| **Video and Audio out of sync** | Playback ran but A/V timing was incorrect |
| **Shows 1 frame - audio plays** | Video stopped after the first displayed frame while audio continued |
| **Corrupted Output** | Decoder produced visibly invalid video output |
| **Not Supported** | MintVID reported no decoder for that sample during this test pass |

## Quick findings

- **Cinepak is the strongest all-round result in this test set.** It was Perfect
  at 68x52 and 132x100 in Result 1, with the second observation moving from
  Perfect to only Slight Stutter at 132x100. It remained in the Stuttering
  range at roughly 150p and 200p before becoming Too Slow at 360p.
- **Microsoft MPEG-4 v2 (MP42), WMV1, WMV2 and raw UYVY422 all reached Perfect
  at the smallest 50p sample**, but their 100p samples were already either
  Too Slow or Stuttering.
- **H.263+ was close to the lightweight-codec leaders at 50p**: Near Perfect in
  Result 1 and Perfect in the second observation, then Stuttering at 100p.
- **H.264, MPEG-4 Part 2 and MJPEG were already stressed at the smallest sample.**
  Their 50p tests stuttered, and larger samples were generally Too Slow.
- **MPEG-1 exposed an A/V synchronisation problem across every tested size**,
  so this pass does not provide a clean MPEG-1 speed ceiling.
- **MPEG-2 exposed a functional problem across every tested size**: one video
  frame was shown while audio continued.
- **Microsoft Video 1 produced corrupted output at every tested size.**
- **Microsoft RLE8 was reported as unsupported in this real-hardware pass.**
  This is recorded as an observed test result; it should not be treated as a
  permanent statement about the current decoder registry because codec support
  can change after this snapshot.

## Practical 68060 @ 50 MHz view

Based only on this test pass, the most promising formats for low-resolution
classic-060 playback are:

1. **Cinepak** — best scaling across the tested resolution ladder.
2. **H.263+** — very good at the smallest size, with 100p already stuttering.
3. **MP42 / WMV1 / WMV2** — excellent at 50p, but a sharp performance drop by
   100p in these samples.
4. **Raw UYVY422** — decode cost is low at 50p, but bandwidth/file size grows
   very quickly and 100p already stuttered.

These results are a useful baseline for future optimisation work. A later
MintVID build should be compared against the same generated samples rather than
replacing this snapshot, so regressions and genuine speedups remain visible.

## Detailed results

### MPEG-1

| File | Size | Bytes | Result 1 | Mode 1 | Result 2 | Mode 2 |
|---|---:|---:|---|---|---|---|
| `mintvid_mpeg1_50p_25fps.mpg` | 66x50 | 94,208 | Video and Audio out of sync | — | — | — |
| `mintvid_mpeg1_100p_25fps.mpg` | 134x100 | 335,872 | Video and Audio out of sync | — | — | — |
| `mintvid_mpeg1_150p_25fps.mpg` | 200x150 | 487,424 | Video and Audio out of sync | — | — | — |
| `mintvid_mpeg1_200p_25fps.mpg` | 266x200 | 630,784 | Video and Audio out of sync | — | — | — |
| `mintvid_mpeg1_360p_25fps.mpg` | 480x360 | 1,155,072 | Video and Audio out of sync | — | — | — |

### MPEG-2

| File | Size | Bytes | Result 1 | Mode 1 | Result 2 | Mode 2 |
|---|---:|---:|---|---|---|---|
| `mintvid_mpeg2_50p_25fps.mpg` | 66x50 | 96,256 | Shows 1 frame - audio plays | — | — | — |
| `mintvid_mpeg2_100p_25fps.mpg` | 134x100 | 339,968 | Shows 1 frame - audio plays | — | — | — |
| `mintvid_mpeg2_150p_25fps.mpg` | 200x150 | 491,520 | Shows 1 frame - audio plays | — | — | — |
| `mintvid_mpeg2_200p_25fps.mpg` | 266x200 | 634,880 | Shows 1 frame - audio plays | — | — | — |
| `mintvid_mpeg2_360p_25fps.mpg` | 480x360 | 1,165,312 | Shows 1 frame - audio plays | — | — | — |

### H.264 / AVC

| File | Size | Bytes | Result 1 | Mode 1 | Result 2 | Mode 2 |
|---|---:|---:|---|---|---|---|
| `mintvid_h264_50p_25fps.mp4` | 66x50 | 53,974 | Stuttering | AGA | Stuttering | HAM8 |
| `mintvid_h264_100p_25fps.mp4` | 134x100 | 138,965 | Stuttering | AGA | Stuttering | HAM8 |
| `mintvid_h264_150p_25fps.mp4` | 200x150 | 181,518 | Too Slow | AGA | Too Slow | HAM8 |
| `mintvid_h264_200p_25fps.mp4` | 266x200 | 235,572 | Too Slow | AGA | Too Slow | HAM8 |
| `mintvid_h264_360p_25fps.mp4` | 480x360 | 455,257 | Too Slow | AGA | Too Slow | HAM8 |

### MPEG-4 Part 2 / XVID

| File | Size | Bytes | Result 1 | Mode 1 | Result 2 | Mode 2 |
|---|---:|---:|---|---|---|---|
| `mintvid_mpeg4_50p_25fps.avi` | 66x50 | 250,828 | Stuttering | AGA | Stuttering | HAM8 |
| `mintvid_mpeg4_100p_25fps.avi` | 134x100 | 449,520 | Too Slow | AGA | Too Slow | HAM8 |
| `mintvid_mpeg4_150p_25fps.avi` | 200x150 | 586,106 | Too Slow | AGA | Too Slow | HAM8 |
| `mintvid_mpeg4_200p_25fps.avi` | 266x200 | 680,832 | Too Slow | AGA | Too Slow | HAM8 |
| `mintvid_mpeg4_360p_25fps.avi` | 480x360 | 1,165,404 | Too Slow | AGA | Too Slow | HAM8 |

### Microsoft MPEG-4 v2 / MP42

| File | Size | Bytes | Result 1 | Mode 1 | Result 2 | Mode 2 |
|---|---:|---:|---|---|---|---|
| `mintvid_msmpeg4v2_50p_25fps.avi` | 66x50 | 248,330 | Perfect | AGA | Perfect | HAM6 |
| `mintvid_msmpeg4v2_100p_25fps.avi` | 134x100 | 444,402 | Too Slow | AGA | Too Slow | HAM8 |
| `mintvid_msmpeg4v2_150p_25fps.avi` | 200x150 | 580,994 | Too Slow | AGA | Too Slow | HAM8 |
| `mintvid_msmpeg4v2_200p_25fps.avi` | 266x200 | 674,540 | Too Slow | AGA | Too Slow | HAM8 |
| `mintvid_msmpeg4v2_360p_25fps.avi` | 480x360 | 1,160,876 | Too Slow | AGA | Too Slow | HAM8 |

### WMV1 / Windows Media Video 7

| File | Size | Bytes | Result 1 | Mode 1 | Result 2 | Mode 2 |
|---|---:|---:|---|---|---|---|
| `mintvid_wmv1_50p_25fps.avi` | 66x50 | 248,640 | Perfect | AGA | Perfect | HAM6 |
| `mintvid_wmv1_100p_25fps.avi` | 134x100 | 443,596 | Too Slow | AGA | Too Slow | HAM8 |
| `mintvid_wmv1_150p_25fps.avi` | 200x150 | 581,612 | Too Slow | AGA | Too Slow | HAM8 |
| `mintvid_wmv1_200p_25fps.avi` | 266x200 | 675,174 | Too Slow | AGA | Too Slow | HAM8 |
| `mintvid_wmv1_360p_25fps.avi` | 480x360 | 1,164,194 | Too Slow | AGA | Too Slow | HAM8 |

### WMV2 / Windows Media Video 8

| File | Size | Bytes | Result 1 | Mode 1 | Result 2 | Mode 2 |
|---|---:|---:|---|---|---|---|
| `mintvid_wmv2_50p_25fps.avi` | 66x50 | 250,578 | Perfect | AGA | Perfect | HAM6 |
| `mintvid_wmv2_100p_25fps.avi` | 134x100 | 472,172 | Too Slow | AGA | Too Slow | HAM8 |
| `mintvid_wmv2_150p_25fps.avi` | 200x150 | 629,302 | Too Slow | AGA | Too Slow | HAM8 |
| `mintvid_wmv2_200p_25fps.avi` | 266x200 | 737,576 | Too Slow | AGA | Too Slow | HAM8 |
| `mintvid_wmv2_360p_25fps.avi` | 480x360 | 1,313,228 | Too Slow | AGA | Too Slow | HAM8 |

### H.263+

| File | Size | Bytes | Result 1 | Mode 1 | Result 2 | Mode 2 |
|---|---:|---:|---|---|---|---|
| `mintvid_h263plus_50p_68x52_25fps.avi` | 68x52 | 254,304 | Near Perfect | AGA | Perfect | HAM6 |
| `mintvid_h263plus_100p_25fps.avi` | 132x100 | 459,156 | Stuttering | AGA | Stuttering | HAM8 |
| `mintvid_h263plus_150p_204x152_25fps.avi` | 204x152 | 587,492 | Too Slow | AGA | Too Slow | HAM8 |
| `mintvid_h263plus_200p_25fps.avi` | 268x200 | 686,992 | Too Slow | AGA | Too Slow | HAM8 |
| `mintvid_h263plus_360p_25fps.avi` | 480x360 | 1,207,438 | Too Slow | AGA | Too Slow | HAM8 |

### Cinepak

| File | Size | Bytes | Result 1 | Mode 1 | Result 2 | Mode 2 |
|---|---:|---:|---|---|---|---|
| `mintvid_cinepak_50p_68x52_25fps.avi` | 68x52 | 269,792 | Perfect | AGA | Perfect | HAM8 |
| `mintvid_cinepak_100p_25fps.avi` | 132x100 | 527,618 | Perfect | AGA | Slight Stutter | HAM8 |
| `mintvid_cinepak_150p_204x152_25fps.avi` | 204x152 | 728,028 | Stuttering | AGA | Stuttering | HAM8 |
| `mintvid_cinepak_200p_25fps.avi` | 268x200 | 914,702 | Stuttering | AGA | Stuttering | HAM8 |
| `mintvid_cinepak_360p_25fps.avi` | 480x360 | 1,528,726 | Too Slow | AGA | Too Slow | HAM8 |

### MJPEG

| File | Size | Bytes | Result 1 | Mode 1 | Result 2 | Mode 2 |
|---|---:|---:|---|---|---|---|
| `mintvid_mjpeg_50p_25fps.avi` | 66x50 | 454,334 | Stuttering | AGA | Stuttering | HAM8 |
| `mintvid_mjpeg_100p_25fps.avi` | 134x100 | 779,786 | Too Slow | AGA | Too Slow | HAM8 |
| `mintvid_mjpeg_150p_25fps.avi` | 200x150 | 1,079,968 | Too Slow | AGA | Too Slow | HAM8 |
| `mintvid_mjpeg_200p_25fps.avi` | 266x200 | 1,395,318 | Too Slow | AGA | Too Slow | HAM8 |
| `mintvid_mjpeg_360p_25fps.avi` | 480x360 | 1,948,264 | Too Slow | AGA | Too Slow | HAM8 |

### Microsoft Video 1 / MSVC

| File | Size | Bytes | Result 1 | Mode 1 | Result 2 | Mode 2 |
|---|---:|---:|---|---|---|---|
| `mintvid_msvideo1_50p_68x52_25fps.avi` | 68x52 | 250,390 | Corrupted Output | — | — | — |
| `mintvid_msvideo1_100p_25fps.avi` | 132x100 | 432,872 | Corrupted Output | — | — | — |
| `mintvid_msvideo1_150p_204x152_25fps.avi` | 204x152 | 602,242 | Corrupted Output | — | — | — |
| `mintvid_msvideo1_200p_25fps.avi` | 268x200 | 764,690 | Corrupted Output | — | — | — |
| `mintvid_msvideo1_360p_25fps.avi` | 480x360 | 1,383,282 | Corrupted Output | — | — | — |

### Microsoft RLE8

| File | Size | Bytes | Result 1 | Mode 1 | Result 2 | Mode 2 |
|---|---:|---:|---|---|---|---|
| `mintvid_msrle8_50p_25fps.avi` | 66x50 | 275,104 | Not Supported: Microsoft RLE (`mrle`) has no decoder | — | — | — |
| `mintvid_msrle8_100p_25fps.avi` | 134x100 | 635,678 | Not Supported: Microsoft RLE (`mrle`) has no decoder | — | — | — |
| `mintvid_msrle8_150p_25fps.avi` | 200x150 | 921,552 | Not Supported: Microsoft RLE (`mrle`) has no decoder | — | — | — |
| `mintvid_msrle8_200p_25fps.avi` | 266x200 | 1,249,178 | Not Supported: Microsoft RLE (`mrle`) has no decoder | — | — | — |
| `mintvid_msrle8_360p_25fps.avi` | 480x360 | 2,491,858 | Not Supported: Microsoft RLE (`mrle`) has no decoder | — | — | — |

### Raw UYVY422

| File | Size | Bytes | Result 1 | Mode 1 | Result 2 | Mode 2 |
|---|---:|---:|---|---|---|---|
| `mintvid_uyvy422raw_50p_25fps.avi` | 66x50 | 1,061,082 | Perfect | AGA | Perfect | HAM6 |
| `mintvid_uyvy422raw_100p_25fps.avi` | 134x100 | 3,586,082 | Stuttering | AGA | Stuttering | HAM8 |
| `mintvid_uyvy422raw_150p_25fps.avi` | 200x150 | 7,736,082 | Too Slow | AGA | Too Slow | HAM8 |
| `mintvid_uyvy422raw_200p_25fps.avi` | 266x200 | 13,536,082 | Too Slow | AGA | Too Slow | HAM8 |
| `mintvid_uyvy422raw_360p_25fps.avi` | 480x360 | 43,436,082 | Too Slow | AGA | Too Slow | HAM8 |

## Keeping this useful over time

When repeating this test suite after decoder or renderer optimisation:

- keep the original sample files unchanged;
- record the MintVID version or commit SHA;
- record CPU, clock speed, display mode and C2P choice;
- add a new dated result rather than silently rewriting this baseline;
- treat functional failures separately from speed failures; and
- where possible, capture measured FPS/timing alongside the human playback
  rating so future comparisons can be quantitative.

That will let this page evolve from a compatibility snapshot into a repeatable
real-hardware benchmark history for MintVID.