# MintVID changelog

## 1.3.0 - 2026-09-12

### Performance

- Simplify the MP2 IDCT's Q15 multiply rounding to a sign-dependent bias
  and unsigned shift, preserving half-away-from-zero results. Add rounding
  residue and full-transform equivalence tests to the audio and m68k suites.
  Playback speed has not yet been measured for this change.

- Select the indexed YUV dithering palette kernel once per conversion,
  removing repeated per-pixel mode branches while retaining the compact
  4 KB quantizer. Complete calls sampled in Copperline used about 13.6%
  fewer instructions; this is not a whole-playback timing result.
- Accumulate MP2 synthesis one output lane at a time, retaining signed
  64-bit products and the original tap order. Copperline traces confirm
  eight instructions per inner-loop tap versus twelve previously. Exact
  regression checks cover all 16 synthesis phases and run in the host audio
  and m68k suites. Real 68060 timing remains unverified; cache behavior can
  affect the benefit of the changed access order.


### Added

- Every GUI's title-bar **MintVID** menu gained a **Guide...** item, opening
  `MintVID.guide` by launching the standard `AmigaGuide` command as a
  subprocess (the same launch mechanism already used for mrplay/iptvgui/
  ytgui), alongside the existing About/Quit items - the release target
  already copies this file into each packaged build, but nothing ever
  actually opened it. `MintVID.guide` itself also gained four nodes the
  existing manual didn't have: Audio, Live streaming and networking,
  Command line (mrplay), and an explicit per-codec support list. Falls back
  to an EasyRequest telling the user where to find the plain-text file if
  the AmigaGuide command or the guide itself isn't available. An earlier
  version of this called amigaguide.library directly; it compiled and
  passed CI but did not actually open the guide on real hardware, so it was
  replaced with this subprocess approach - see CLAUDE.md.
- Both the ReAction and GadTools main controllers now remember the local
  file browser's last-used drawer (`ENVARC:MintVID.lastdir`, so it survives
  a reboot) and reopen there next time, instead of always starting fresh.
- `make check-audio` gained an MPEG-2.5 MP3 fixture (11.025 kHz stereo in AVI),
  which covers both the newly reachable Helix path and MintVID's own
  `mp3_frame_bytes()`, whose MPEG-2.5 frame-length arithmetic had never run.
- `make check-audio` gained `mr_mp2_check`, which decodes a .mpg through the
  MPEG-1 program-stream source - the path `play_mpeg1()` uses, and the only
  audio path in the player that does not go through the MintAMP adapter - and
  diffs its PCM against ffmpeg's decode of the same track. `make check-m68k`
  runs it too, where `mr_mpeg1_audio()`'s explicit little-endian output can
  actually be checked. The bar is relative rather than absolute, because
  pl_mpeg's fixed-point Layer II synthesis runs about 2% quieter than ffmpeg's
  float decode on MPEG-1 and MPEG-2 streams alike; silence, a slipped frame or
  a wrong bit-allocation table are all far outside it. The check also holds the
  `--audio-mono` and `--audio-rate=low` runs against the normal one.
- `make check` gained five H.263 conformance paths against ffmpeg, where it
  previously had none: version 1 sub-QCIF, the same bitstream remuxed into a
  3GP/QuickTime track as `s263`, version 1 with GOB headers, H.263+ with a
  custom 68x52 picture format and custom picture clock, and H.263+ with
  unrestricted motion vectors and several slices per picture. `make check-m68k`
  runs the first and fourth of those on real big-endian m68k. The four defects
  above all sat in code the suite never executed.
- Mono sound mode: `--audio-mono`, and a **Mono audio** tickbox beside **No
  audio** in both GUI editions. Paula's output has always been a single 8-bit
  channel, so this is not a change of what the machine plays - it moves the
  fold from "decode both channels, then average them per sample" to "ask the
  codec for one channel", which is where the saving is. MP3 uses MintAMP's
  `MP3SetOutputMono()`/`MP3SetMonoMSSideSkip()`, so on mid/side frames the side
  channel's Huffman decode, dequant, IMDCT and subband synthesis are skipped
  outright and the coded mid channel is used as-is (it already carries the
  1/sqrt(2) scale, so it *is* (L+R)/2). MP2 gains `plm_audio_set_mono()` in
  pl_mpeg, which runs the polyphase synthesis - the dominant cost of Layer II
  decode - once per frame instead of twice and emits channel 0. AC-3 asks
  liba52 for `A52_MONO`, which folds the channels in the frequency domain and
  so runs one IMDCT per block instead of one per channel. Helix AAC has no mono
  mode, so there the second channel is dropped after decode: the downmix and
  the decimation copy get cheaper, the decode itself does not.
- `make check-audio` gained stereo MP3, MP2 and AC-3 fixtures and checks the
  mono runs against the stereo ones: same rate and same frame count (mono
  changes channels per frame, never the timeline), single-channel buffers at
  the sink, and PCM that matches either the left channel or the (L+R)/2
  average of the stereo run - the two folds a decoder can legitimately hand
  back. The stereo fixtures are the point: with the previous mono-source
  fixtures, "kept one channel" and "kept both" look identical.

### Fixed

- MPEG-2 video in `.mpg`/MPEG-PS files stopped after the first picture while
  audio continued. At the very small resolutions useful on a classic Amiga,
  one PES packet can contain several complete pictures; the adapter only had
  two RGB return buffers and treated a third picture as corrupt input. The PS
  demuxer now feeds picture-sized chunks and the libmpeg2 adapter keeps a
  reusable FIFO for the occasional pair of display-order outputs caused by
  B-frame reordering. MPEG-2 PS samples at 50p through 360p now decode all 125
  frames of each five-second test clip.
- MPEG-1 video could drift progressively behind its MP2 audio whenever decode
  and display exceeded the frame budget. Its separate legacy player displayed
  every late picture, unlike the main player scheduler, so it could never
  recover. MPEG-1 pacing now uses pl_mpeg's frame timestamps against the played
  Paula audio clock and drops a picture only after it is more than one frame
  late, keeping sound and the pictures that are shown on the same timeline.
- MP3 at 8, 11.025 or 12 kHz decoded to silence. Below 16 kHz, Layer III is
  MPEG-2.5 - a third header version - and the vendored Helix decoder shipped
  with the 12-bit syncword, which matches only MPEG-1 and MPEG-2 frames. An
  MPEG-2.5 frame's second header byte is `0xe2`/`0xe3`, so `MP3FindSyncWord()`
  never found one and every frame was rejected: no error, no samples. Fixed in
  MintAMP (boingball/MintAMP#498, submodule bumped here) by enabling the 11-bit
  syncword the decoder's own tables were always ready for; MPEG-1 and MPEG-2
  streams decode byte-for-byte as before. This is the same shape as the MP2
  defect below, in the other of the two Layer decoders.
- `mr_audio_check` required the low-rate run's output to be exactly half the
  normal run's. The decoder divides integers, so an odd rate reports 5512 Hz
  against 11025 and the check failed on a correct decode - which is exactly the
  rate an MPEG-2.5 fixture has.
- MPEG-1 files with 16, 22.05 or 24 kHz audio played silently, and `.mpg`
  clips encoded for Paula land on exactly those rates. Below 32 kHz, Layer II
  is MPEG-2 audio (ISO/IEC 13818-3, "low sampling frequency"), and pl_mpeg's
  header decoder refused any frame that was not MPEG-1 - so `plm_get_samplerate()`
  returned 0, `mr_mpeg1_samplerate()` reported no audio track, and
  `play_mpeg1()` never opened Paula at all (it also fell back to pacing video
  on a timer rather than on the audio clock). MPEG-2 Layer II is now decoded:
  it halves the three MPEG-1 sample rates, has its own bitrate list, and uses
  one fixed bit-allocation table instead of choosing between 3-B.2a..d. The
  tables for all of that were already vendored - only the version gate kept
  them out of reach - so the fix is the gate, the index offsets into the second
  half of the rate and bitrate tables, and selecting the fixed allocation table
  for MPEG-2. MPEG-1 Layer II decodes byte-for-byte as before. This also
  reaches the general audio adapter, so MP2 at those rates now works in MPEG-TS,
  Matroska and AVI too, not just in `.mpg`.
- While in that header: a free-format Layer II frame (bitrate index 0) read
  `PLM_AUDIO_BIT_RATE[-1]`. Both the free and the forbidden index are now
  rejected before the lookup.
- `mr_decode` reports a `.mpg`'s audio track and how much of it decoded. It
  pulled video only, so "no audio" and "audio that decodes to nothing" looked
  identical in the harness - which is how the defect above stayed invisible on
  the dev host.
- H.263 files carrying an extended PTYPE (H.263+/H.263-1998) played as a black
  window: the decoder stopped at the first picture with `H.263+: unsupported
  feature extended PTYPE`. Anything `ffmpeg -c:v h263p` writes lands there,
  because that encoder signals a custom picture format for any non-standard
  size and a custom picture clock for any frame rate that is not 30000/1001 -
  a 68x52 25 fps clip needs both. The picture layer now decodes PLUSPTYPE
  (UFEP/OPPTYPE/MPPTYPE), custom picture formats and pixel aspect ratios, the
  custom picture clock and its extended temporal reference, the slice-structured
  headers of Annex K, unrestricted motion vectors (Annex D), and the rounding
  type that an encoder flip-flops between P pictures. A picture whose size
  disagrees with the container is still refused, since the frame buffers are
  sized when the decoder is opened.
- H.263 escape-coded coefficients were decoded with MPEG-4's three-form escape
  instead of the single H.263 one, so every escape consumed two bits too many
  and desynchronised the rest of the picture. The same file also mis-handled
  `INTRADC`: 255 means 1024, not "invalid", and 0 is not a synonym for 128.
  Both had gone unnoticed because no fixture had ever driven this decoder -
  `make check` only checked that the FourCCs routed to it.
- Motion vectors wrapped modulo the wrong range (+-64 rather than the +-32
  half-pel range an f_code of 1 gives), which corrupted any macroblock whose
  predictor and difference straddled the wrap. Macroblock stuffing codes
  (`MCBPC` 9-bit escape) are now skipped rather than decoded as an intra
  macroblock, and GOB headers are accepted with the zero stuffing bits that
  precede them when an encoder byte-aligns the header.
- H.263 in QuickTime/3GP (`s263`) had no decoder: the demuxer named the codec
  but the FourCC was missing from the registry, so those files stopped with
  "no decoder for this fourcc" despite the documented MOV support.
- Local playback now bounds the audio cushion by the decoded-video queue's
  time span. This prevents fast low-resolution codecs such as Cinepak from
  filling the default 16-frame queue, discarding subsequent pictures while
  buffering 2.5 seconds of audio, and presenting visible gaps. At 25 fps the
  default cushion is now 560 ms.
- AC-3 played 6 dB hot on every target, the Amiga included: `feed_ac3()` asks
  liba52 for `LEVEL(0.25)` output, which puts a full-scale sample at `1<<28`,
  but shifted it down by 12 rather than 13. Anything but quiet material spent
  its peaks clamped at the rails - distortion, not loudness. With the shift
  corrected the decoder matches ffmpeg's own decode to a worst-case 2 LSB and a
  mean of 0.50.
- The **host** build decoded AC-3 to full-scale noise. It compiled MintAMP's
  vendored Rockbox FFT - which liba52's IMDCT calls through `ff_fft_calc_c` -
  with `-DAMIGA_M68K`. MintAMP supports that define on a non-m68k host so its
  decoders exercise the same C backend the Amiga runs, but
  `decoders/wma/platform.h` also derives `ROCKBOX_BIG_ENDIAN` from it, and
  `codeclib_misc.h`'s `MULT32` - the multiply in every FFT butterfly - takes
  the high half of a 64-bit product through a union whose field order follows
  that. On a little-endian host it therefore returned the *low* half, and every
  AC-3 IMDCT came back as hash. Those two translation units now follow the
  host's real byte order (`WMA_FFT_FLAGS`). The Amiga and m68k builds define it
  truthfully and were never affected - only the test oracle was, which is why
  this went unnoticed.
- `make check-audio` had no oracle for decoded audio at all: it asserted that a
  plausible number of non-silent samples came out, which full-scale hash passes
  as readily as music. `mr_ac3_check` now decodes through the real adapter and
  compares against ffmpeg's PCM sample by sample, thinning the reference by the
  same Paula decimation ratio the decoder applies rather than resampling it. It
  fails on both defects above (worst 4249 and 37010 respectively, against a
  budget of 64). `make check-m68k` runs it too, built without the demuxer so it
  needs no container or H.264 cross-build - that run is what actually
  substantiates the endianness claim, since m68k is where the define is true.
  With AC-3 decoding correctly, mono mode's fold check no longer has to skip
  it either: liba52's `A52_MONO` output now measures within 1 LSB of the
  (L+R)/2 average of the stereo decode, as it always should have.

- H.264 Balanced, Fast and Turbo asked libavc to degrade only *some* pictures
  (`i4_degrade_pics` 1 and 3). libavc skips `ih264d_set_deblocking_parameters()`
  and `pf_compute_bs()` per macroblock on a degraded picture, but the
  per-macroblock deblocking descriptor array they fill in persists across
  pictures - so the next undegraded picture was deblocked against the previous
  pictures' stale `MB_DISABLE_FILTERING` flags, boundary strengths and QPs.
  The result was both wrong output and far more edges filtered than the
  picture contains: measured on a 320x180 CABAC stream, Fast spent 51% of the
  entire decode inside `ih264d_deblock_mb_nonmbaff()` and ran ~47% *slower*
  than Quality, which deblocks every picture properly. Every degrading mode
  now uses the all-or-nothing policy that Turbo+ and TurboGT already used.

### Improved

- Cinepak on a compatible native indexed display now decodes its reusable V1
  and V4 codebook tiles directly to the final 4-, 5- or 8-plane palette
  indices. The previous AGA path wrote a three-byte RGB framebuffer and then
  read and dithered that entire frame into a second one-byte queue buffer;
  direct indexed output removes both the RGB traffic and the per-frame dither
  loop while remaining byte-identical to it. RTG, HAM and resized modes retain
  the established RGB24 path.
- Local media files now use a 64 KiB fully-buffered stdio window instead of
  libnix's 1 KiB default, reducing the number of small AmigaDOS reads needed
  for interleaved AVI video and audio packets. Allocation failure safely keeps
  the original libc buffer.
- Both shared YUV420-to-RGB converters now walk the picture a row *pair* at a
  time. 4:2:0 gives one chroma sample per 2x2 luma quad, so the three
  chroma-derived addends are shared by four output pixels; stepping single
  rows recomputed them for the second row of every pair, doubling the chroma
  work for output that was identical either way. This applies to the portable
  C (`mr_yuv.c`, where `emit_pixel()` is now also force-inlined - GCC had been
  emitting an indirect call twice per pixel) and to the hand-written m68k
  kernel (`mr_yuv_m68k.S`), which was restructured to emit the whole quad,
  streaming row 0 through the postincrement path and reaching row 1 from the
  same pointers with the strides in `d3`/`d7`. Measured under qemu-m68k on
  352x288, the C form is 27% faster than before. Every codec that reaches the
  display through RGB24 benefits, MPEG-1 and MPEG-2 included.
- `make -f Makefile.amiga YUV_ASM=0` now builds that one converter from
  portable C while leaving every other hand-asm path enabled, so the two can
  be A/B'd on real hardware. They are bit-identical, so it only ever changes
  speed. The switch exists because the answer is genuinely open: the
  assembly was written when GCC was not inlining `emit_pixel()`, which is no
  longer true, and under qemu-m68k the C now measures about 23% faster - but
  qemu costs instructions rather than cycles and models neither the 68030's
  memory system nor its lack of branch prediction, which is most of what that
  kernel is tuned around.
- MPEG-1 keeps pl_mpeg's own colour converter rather than moving to the shared
  one like every other codec. Moving it was tried and measured worse: GCC
  strength-reduces pl_mpeg's constant multiplies into shift/add chains, so
  both are multiply-free and both step 2x2 quads, but on 352x288 under
  qemu-m68k pl_mpeg's costs 649ms/300 iterations against the shared
  converter's 821ms with the assembly active, and its output is marginally
  closer to the ffmpeg reference too. `core/mr_mpeg1.c` now records that,
  including the condition under which the switch becomes worth making.
- AGA HAM playback of H.264 can now convert straight from the decoder's
  YUV420P planes to HAM pixel bytes (`core/mr_yuv_ham.c`), where previously
  HAM was the one display mode still materialising a full-resolution RGB24
  frame per picture. Hold-and-modify resets at every scanline start, so the
  greedy encoder fuses with the YUV->RGB pass exactly as the ordered dither
  already does for the indexed modes, and the HAM bytes it produces are
  chunky bytes reaching the screen through the same C2P and blit as palette
  indices.

  This is enabled for the exact vertical-downscale geometry only - the
  640x360-into-a-640x180-non-laced-screen shape an AGA fit normally produces -
  and that restriction is a measured result rather than a structural limit.
  Removing the RGB24 intermediate is worth less than it looks, because the
  three-stage path's YUV->RGB stage is hand-written m68k assembly and the
  fused encoder is portable C: at 1:1 the two are a wash, swinging either way
  by more with a change of inlining than the fusion itself is worth. What
  does not depend on out-coding that assembly is converting fewer samples,
  and on a vertical downscale the old path converted every source row to RGB
  before throwing half of them away. Measured under qemu-m68k on a 640x360
  HAM8 frame, the whole conversion drops 45%, consistently across every code
  layout tried. The other HAM geometries stay on the proven three-stage path;
  a hand-written assembly fused encoder, as `mr_yuv_dither_m68k.S` already is
  for the indexed modes, is what would open them up.
- `tests/mr_yuv_ham_check.c` checks the fused encoder sample-for-sample
  against the real `mr_yuv420_to_rgb24()` -> `mr_scale_resize_rgb24()` ->
  `mr_ham_encode()` composition, for HAM6 and HAM8 across seven geometries,
  padded and unpadded plane strides, and random and extreme content. It runs
  in both `make check` and `make check-m68k`, the latter comparing against
  the accelerated assembly pipeline on real big-endian m68k.
- The inter-prediction half of libavc's degrade control was dead code:
  `i4_degrade_type` bits 2 and 3 ("faster"/"fastest inter prediction filters")
  set `ps_dec->i4_mv_frac_mask`, and nothing in the vendored decoder reads that
  field - `ih264d_form_mb_part_info_*()` extracts the fractional motion vector
  with a hardcoded `& 0x3`. Fast, Turbo, Turbo+ and TurboGT were therefore only
  ever getting the deblocking half of what they requested. MintVID now supplies
  the missing filters itself, in `vendor/libavc_port/ih264_mc_degrade.c`,
  selected the same way the rest of the port swaps in m68k assembly - by
  rewriting `apf_inter_pred_luma[]` / `pf_inter_pred_chroma`, with the vendored
  submodule untouched. Fast and below now interpolate quarter-pel luma
  bilinearly instead of with the separable six-tap filter.
- Chroma motion compensation - previously the single hottest function in an
  H.264 decode at 21% of executed instructions - now takes exact shortcuts when
  `dx` or `dy` is zero. Substituting into spec equation (8-266) collapses those
  cases to a block copy or a two-tap average with no rounding difference, and
  the `CLIP_U8` can never fire, so this is bit-identical and applies to every
  speed mode including Quality. Chroma inherits the luma motion vector, so the
  zero-motion background most streams are largely made of takes the copy path.
- Measured on a 320x180 High Profile CABAC stream, cross-built for m68k and run
  under qemu-m68k with the hand-written assembly active: Quality -7%
  (bit-identical output), Balanced -30%, Fast -52%, Turbo -48%, TurboGT -8%.
  At 640x360 the same modes are -9%, -29%, -56%, -54% and -11%.
- `tests/mr_h264_mc_degrade_check.c` checks the exact filter set sample-for-
  sample against Ittiam's reference for every `dx`/`dy` and block geometry, and
  the bilinear set against a direct transcription of its documented formula.
  It runs in both `make check` and `make check-m68k`, the latter covering the
  unaligned longword loads the packed paths make on real big-endian m68k.

### Changed

- Turbo and TurboGT are now the same setting. TurboGT previously differed only
  by disabling deblocking on keyframes as well, and every mode from Balanced
  down now does that unconditionally - see the fix above. The obvious
  replacement lever, truncating motion vectors to whole samples so prediction
  becomes a block copy, was implemented and measured at 3-4% for a 17 dB PSNR
  loss (23.1 dB against bilinear's 40.5 dB, and 17.8 dB on low-detail content),
  so it is not shipped. TurboGT remains selectable everywhere it was - GUI
  choosers, `--h264-speed=turbogt`, saved settings - and is both faster and
  cleaner than the TurboGT of 1.2.0.
- Balanced now means "in-loop deblocking off, motion compensation exact",
  and Fast means "Balanced plus bilinear interpolation". Both descriptions are
  what the modes actually do; the previous per-picture wording described a
  policy the decoder was not carrying out.

## 1.2.0 - 2026-09-05

### Improved

- H.264 m68k inverse-quantisation, inverse-transform and reconstruction hot
  loops inline their tiny coefficient/pixel helpers, removing up to 64
  `BSR`/`RTS` pairs from an 8x8 block while retaining bit-exact truncation,
  rounding and clipping.
- The 68060 H.264 luma deblocking path replaces unpredictable sign branches
  with an exact branchless absolute-difference sequence. The existing compact
  implementation remains selected for 68030 and 68040 builds.
- MintVID now enables MintAMP's CPU-aware AAC Huffman, dequantisation, stereo
  and IMDCT helpers by default. The 68030/040 builds use hardware full-result
  multiplication; the 68060 uses a bit-exact partial-product implementation
  and avoids software emulation of register-pair `MULS.L`.
- H.263, MPEG-2, MPEG-4 Part 2, Microsoft MP42/DIV2, WMV1 and WMV2 now share
  MintVID's validated YUV420-to-RGB converter. Amiga builds consequently use
  the existing hand-written m68k output loop rather than six scalar,
  multiply-heavy private implementations.
- CI exercises one million random and edge-case 68060 AAC multiply pairs and
  retains host plus big-endian m68k codec conformance coverage.
- Release metadata, Aminet text, AmigaGuide and licence index are refreshed
  for 1.2.0.

### Real-hardware validation

- Stereo AAC/AAC+ playback was confirmed clean through MintAMP on a real
  68060, and the same embedded AAC path remained stable alongside H.264 in
  MintVID.
- On the tested A1200 68060 with AGA/HAM8, the lowest-resolution BBC One HLS
  stream approached real time with audio. Turbo+ maintained continuous audio
  with keyframe/slideshow video when full decoding could not keep pace.
- These results do not promise real-time H.264 at higher resolutions or on
  every accelerator; CPU clock, memory, stream profile and display mode remain
  significant.

### Compatibility

- Use the release matching a real 68030, 68040 or 68060. The 030/040 AAC
  full-result multiply path is deliberately not suitable for a real 68060,
  where that instruction form is software-emulated. PiStorm/Emu68 continues
  to use MintVID040.
- `AACASM=0` remains available for portable-C A/B and troubleshooting builds.

## 1.1.1 - 2026-09-03

### Added

- Native WMV1 / Windows Media Video 7 decoding for AVI files, including the
  codec's selectable run/level, DC and motion-vector VLC tables, coded-block
  prediction and adaptive escape coding.
- Native WMV2 / Windows Media Video 8 decoding for AVI files, including its
  extension header, bitplane skip coding, adaptive motion prediction, MSPEL,
  adaptive block transforms and in-loop deblocking.
- WMV1 and WMV2 decoder coverage in the portable and big-endian m68k/QEMU
  conformance suites against ffmpeg-generated reference output.

### Fixed

- Live HLS ESC/Stop shutdown is stabilised by restoring the released 1.0.0
  single-next-segment lookahead policy. This removes the aggressive three-
  segment scheduling introduced late in 1.1.0 while retaining the hard worker
  join/no-abandon shutdown protection, preventing the `MintVID HLS fetch`
  `#80000004` worker failure seen on real A1200/WinUAE testing.
- H.264 Turbo+ now applies TurboGT's all-picture degradation (disabling
  I-frame deblocking) to the keyframes it still fully decodes, instead of
  keeping them at Fast's non-key-only degrade policy. Turbo+ skips every P-
  and B-frame, so the keyframe decode is the one blocking call left between
  displayed frames; on a slow CPU (reported on a stock 66 MHz 68060 A1200)
  that call could run long enough to drain Paula's hardware buffer with no
  audio service in between, heard as a laggy half-rate echo. Shortening the
  keyframe decode keeps audio fed through it.

### Improved

- The shared GUI About requester now calls out local/HLS/IPTV/YouTube playback
  and the WMV7/WMV8 additions alongside the existing MPEG/H.264 codec family.
- Release metadata, Aminet text and licence index are refreshed for 1.1.1.

### Compatibility notes

- WMV1's low-bitrate spatial intra/inter prediction mode is deliberately
  rejected rather than decoded approximately.
- WMV2 IntraX8 (J-frame) coding is not supported and is likewise rejected
  cleanly. Normal WMV2 I/P streams remain supported.
- The HLS worker still contains the newer lifecycle hardening from 1.1.0; only
  the number of future compressed segments actively hinted by HLS is returned
  to one for release stability.

## 1.1.0 - 2026-09-02

### Added

- Separate **TurboGT** H.264 performance mode. TurboGT keeps Turbo's B-frame
  skip policy and P-frame reference chain, but applies the strongest practical
  libavc degradation policy to every decoded picture.
- CPU-aware Kalms C2P selection: the 68030 release keeps the 030 kernel, while
  the 68040 and 68060 releases use Kalms' kernel designed for those CPUs.
- Fused Kalms 2x2 scaling and C2P for eligible eight-plane display geometry.
- Direct H.264 YUV420P-to-indexed input for the fused Kalms 2x2 path, avoiding
  both the RGB24 intermediate and a separate RGB-to-indexed pass.
- Direct six-plane Kalms HAM6 output in the 68040 and 68060 releases.
- AmigaOS `$VER:` identities for every shipped executable.

### Changed defaults

- TurboGT is now the default H.264 policy in the GUIs and for bare `mrplay`
  Auto mode. Explicit Quality, Balanced, Fast, Turbo, and Turbo+ choices remain
  available.
- CPU-matched Kalms conversion is now the default AGA/HAM C2P path. Unsupported
  geometry or bitmap layouts still fall back safely to `WritePixelArray8`;
  RTG playback ignores the C2P setting.

### Improved

- H.264 decoding now uses libavc shared display buffers, decoding luma directly
  into the display picture and avoiding one full-frame luma copy per output.
- Kalms conversion now processes only dirty row bands instead of converting
  the entire persistent chunky frame for every update.
- Kalms input buffers are explicitly 16-byte aligned.
- ReAction and GadTools GUIs expose the supported Kalms paths consistently;
  040/060 builds permit AGA, HAM8, and HAM6, while 030 correctly excludes the
  040-only HAM6 kernel.
- `--time` identifies the active implementation as `kalms-030`, `kalms-040`,
  `kalms-2x2`, or `kalms-ham6`.
- YouTube requests for 720p/1080p/Best continue searching other clients after
  an early 360p-only response, retaining 360p as a final fallback.
- General AGA H.264 upscaling reuses each repeated source pixel's YUV-to-RGB
  result while retaining destination-specific dithering; 256-to-640 fitting
  now performs 256 colour conversions per row instead of 640.
- Live HLS playback can buffer several compressed segments ahead instead of
  only one. This provides substantially more network-jitter margin per byte
  than storing the same duration as decoded RGB frames.
- Network playback grows its decoded-frame queue from available RAM while
  retaining the existing safety limits for large frames or tight systems.
- H.264 frames already more than one frame period late can skip their expensive
  RGB conversion/display output while still decoding reference state, allowing
  demux and audio work to catch up.
- Live resync now aims to return about 2.5 seconds behind the live edge, trading
  a little latency for useful margin against the next segment-fetch stall.
- Paula hardware requests grow from 100 ms to 200 ms per buffer, and the audio
  rescue entry, target, and time budget are retuned to match.
- The P96 RTG backend's direct-lock fast path now covers 16-bit RGB565 and
  32-bit ARGB as well as 24-bit BGR, and tries depths in 16/24/32 preference
  order when opening its private screen.
- The CGX RTG backend gains an equivalent direct-lock fast path for its own
  private fullscreen screen through cybergraphics.library, including genuine
  CyberGraphX-only boards without Picasso96API.library.

### Fixed

- P96 fullscreen mode selection now prefers a screen with the video's aspect
  ratio before the smallest spare area, so 854x480 no longer selects a 4:3
  1024x768 scanout when a 16:9 1280x720 mode is available.
- P96 presentation now rebuilds its fitted rectangle from live decoded-frame
  dimensions when an HLS segment changes size, preventing stale metadata from
  stretching a 16:9 frame to 4:3.
- ESC/Stop now joins the in-process HLS fetch worker before `mrplay` exits,
  preventing the worker from continuing in an unloaded code segment and
  raising an `#80000004` illegal-instruction alert. Pending lookahead is no
  longer promoted during shutdown.
- AmiSSL shutdown no longer calls both `CleanupAmiSSL()` and `CloseAmiSSL()`
  for a session opened with `AmiSSL_InitAmiSSL=TRUE`, avoiding duplicate TLS
  cleanup when the HLS worker exits.

### Compatibility

- The public feature set remains available in the 68030, 68040, and 68060
  release drawers; performance-specific assembly is selected at build time.
- Both new RTG direct-lock paths fail closed onto the existing
  `WritePixelArray` path for any unrecognised screen format or geometry
  (windowed mode, or a shared Workbench screen), never onto garbled output.
- Turbo+ remains available for last-resort keyframe-only playback, but TurboGT
  is the normal aggressive setting because it preserves the P-frame chain.

## 1.0.0 - 2026-08-23

- Initial public MintVID release.
