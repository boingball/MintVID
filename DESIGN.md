# MintVID — design & roadmap

A codec-agnostic video player for 68k AmigaOS, in the spirit of **MintAMP**
(the libhelix-based audio player): small, portable C at the core and thin
Amiga-specific layers around it. MP3/AAC decoding reuses the proven
MintAMP/Helix code, with audio output through Paula.

The project was inspired by **RiVA 0.54** (Fellner / Török / Richter) — the
fastest 68k MPEG-1 player, written in hand-tuned 68k/AMMX assembly. Its
renderers (AGA C2P, CyberGraphX/P96 chunky) and IDCT/motion macros were a
goldmine of ideas during design, but its source was never vendored into this
repository and MintVID does not build on or ship any of it — the new player
is a *fresh* codebase, not an extension of that assembly. See the original
RiVA 0.54 release on Aminet if you want the reference source itself.

## Why not "just extend RiVA"?

MintAMP was easy because libhelix is portable C — you wrap a codec. RiVA has no
C codec to wrap: its MPEG-1 video decoder *is* 22k lines of assembly, and
MPEG-1 is its ceiling. To go beyond MPEG-1 we need a decoder we can actually
plug in and swap, so the core is portable C with a decoder vtable
(`player/core/mr_codec.h`).

## The hardware spread drives everything

One codebase and feature set must span a huge range of accelerated 68k
hardware:

| Tier | Machine | Reality |
|------|---------|---------|
| Floor | 68030+ accelerated ECS/AGA | No SIMD and limited integer throughput. Lightweight codecs, modest frame sizes and efficient chipset conversion matter most. |
| Mid | 68040/060, AGA or RTG | Older codecs become increasingly practical, but modern H.264 streaming remains well below real time on classic CPUs. A tested 68060 using AGA has managed roughly 7 fps at best at the lowest YouTube/IPTV resolutions. |
| Moon | PiStorm / Emu68 | An ARM runs the 68k. Use the MintVID040 build; heavier codecs become physically practical, with results depending on the host Pi, display path and stream. |
| Unverified | Vampire / Apollo 68080 | Not yet validated by the MintVID project. No optimised release build is officially recommended yet. |

Design consequence: **codec choice is a tier, not a fixed decision.** The
player picks/loads a decoder; a weaker machine sticks to Cinepak or another
lightweight codec, while a PiStorm can run something much heavier from the
same codebase. Supporting a codec means MintVID can decode it; it does not mean
that codec is real-time on every supported CPU.

## Codec strategy

"Beyond MPEG-1 on a 68030" does **not** mean a newer codec — H.264/VP8/etc.
cost *more* CPU, not less. On weak hardware you win by matching the codec to
the CPU, which usually means an older, decode-cheap codec.

- **Cinepak (CVID)** — base tier. Vector quantisation: codebook lookups + block
  copies, no DCT. Designed for 386/68030-class CD-ROM playback. **Implemented
  and validated** (`player/core/mr_cinepak.c`).
- **Motion-JPEG** — mid tier. Intra-only; core is 8×8 IDCT + Huffman.
  (MintVID's shipped MJPEG decoder uses a picojpeg-based adapter rather than
  porting RiVA's hand-tuned 68k IDCT — see the roadmap below.) Better
  quality, heavier than Cinepak.
- **MPEG-1** — mid tier. Could wrap a portable decoder, or bridge to RiVA's
  engine as reference.
- **Moon-shot** — H.264 Baseline/Main/High through Ittiam libavc's portable
  integer C decoder. The codec is supported on classic 68k, but practical
  online H.264 viewing is aimed at PiStorm/Emu68 rather than 68040/060. MP4
  `avc1`, avcC setup, CABAC, B-frames and display reordering are implemented;
  exact performance depends strongly on resolution, stream complexity,
  display path and hardware.

Container: **AVI** (RIFF) and **QuickTime MOV** are both implemented behind one
auto-detecting front end (`mr_demux.h`), so the player is container-blind —
`mr_avi.c`, `mr_mov.c`. Their file-backed path retains only headers/sample
tables plus one reusable compressed-packet buffer, allowing media much larger
than available RAM. Adding a container is a backend, like adding a codec.

## Architecture

```
             +------------------ platform (Amiga) ------------------+
  file/async | RTG chunky blit   | AGA C2P + dither | Paula audio   |
   reader ---+-------------------+------------------+---------------+
      |            ^ frames             ^ frames          ^ pcm
      v            |                    |                 |
  +--------+   +-----------------------------+     +--------------+
  |  demux | ->|  decoder (vtable, per-codec)| ... |  a/v sync    |
  | mr_avi |   |  cinepak / mjpeg / ...       |     | (audio clock)|
  +--------+   +-----------------------------+     +--------------+
       \__________________ portable core (C) __________________/
```

- **Portable core** (`player/core/`): demux, decoder registry + decoders, pixel
  formats. Builds and is tested on the dev host; no Amiga dependencies.
- **Platform layer** (`player/amiga/`): screen/RTG setup and chunky blit, AGA
  C2P + dither, file-backed packet IO, Paula output, and the MintAMP/Helix
  decoder integration used for MP3/AAC.
- **Sync**: audio is the master clock; video drops/repeats frames to track it —
  the same broad principle RiVA settled on.

## Validation approach

The portable core is regression-tested against **ffmpeg** on the development
host where applicable, and is also cross-built and exercised on a big-endian
m68k target under QEMU to catch endianness and alignment bugs that a
little-endian host can hide. Cinepak currently matches to a worst per-frame
mean-absolute-error of **~0.13/255** (last-LSB YUV→RGB rounding).
`cd player && make check`.

## Roadmap

- [x] Decoder vtable + registry (`mr_codec.h`)
- [x] Container-agnostic demux front end (`mr_demux.h`)
- [x] AVI demuxer (video + audio stream discovery) (`mr_avi.c`)
- [x] QuickTime MOV demuxer (stbl sample-table frames) (`mr_mov.c`)
- [x] File-backed AVI/MOV packet streaming: metadata + one compressed packet
      in RAM instead of loading the complete media file
- [x] Cinepak decoder, ffmpeg-validated on AVI + MOV (`mr_cinepak.c`)
- [x] Microsoft Video 1 (MSVC/CRAM, plus compatible WHAM) in AVI
- [x] Microsoft RLE8 palettised AVI (BI_RLE4 is the next extension and remains
      explicitly unsupported rather than partially decoded)
- [x] Uncompressed packed UYVY422 (`2vuy`/`UYVY`) in QuickTime/MOV
- [x] Amiga (m68k) build + verified decoding on real hardware
- [x] `mrplay`: RTG window output via cybergraphics WritePixelArray
      (`player/amiga/`) - **video playing on real hardware**
- [x] AGA fallback: custom screen via WritePixelArray8, auto RTG->AGA
      selection by screenmode (`display.c`, `display_aga.c`) - on hardware
- [x] AGA colour modes: 256-colour ordered dither (`mr_dither.c`) and
      HAM8/HAM6 near-truecolour (`mr_ham.c`); 2x scaling (`mr_scale.c`).
      Encoders host-validated (HAM8 round-trip MAE 2.05/255 vs dither 6.77)
- [x] Encoder speed: divide-free dither/HAM via lookup tables (2fps -> 6fps
      on 030); fast 2x by doubling the chunky, not the RGB (`mr_scale2x_u8`)
- [~] AGA C2P: built-in 8x8-transpose chunky->planar straight to bitplanes
      (`mr_c2p.c`), default over WritePixelArray8 (--wpa to compare). Transpose
      + round-trip host-verified; on-hardware speed measured with --time
- [x] Faster HAM encoder (divide-free, table-driven set error); --time splits
      encode vs blit. Measured: for HAM8 the encode dominates, blit is minor
- [x] Dirty-row rendering: the Cinepak decoder reports the changed-row span
      (host-verified to cover every changed pixel); the display re-encodes and
      re-blits only those rows, so mostly-static video skips most of the encode
- [x] Playback controls: pause (space), loop (--loop), quit; input is now an
      event stream (`display_poll_event`) so seek (cursor keys) can slot in
- [x] Offline-file seek: QuickTime MOV/MP4 tracks are already stored as a
      flat, presentation-time-sorted sample index, so `mr_mov_seek()` parses
      the `stss` sync-sample table into a keyframe flag per video sample and
      binary-searches the index for the nearest keyframe at or before the
      target time (`mr_demux_seek`/`mr_demux_can_seek` in `mr_demux.h`).
      Wired to cursor left/right (10 s steps) in `mrplay` for local, non-
      streamed MOV files - decoder/audio state resets exactly like the
      existing loop-restart and live-reconnect paths do, then the normal
      startup logic re-primes from the new position. Host-validated
      (`player/tests/mr_mov_seek_check.c`, in `make check`) against a
      2-keyframe fixture. AVI (has no timestamp/keyframe table at all today)
      and MKV (Cues unread) remain unseekable and keep the fast-forward
      toggle; network/live sources are deliberately excluded.
- [ ] Optional asm C2P hot loop; CD32 Akiko hardware C2P path (--akiko)
- [x] MJPEG decoder (picojpeg adapter, `mr_mjpeg.c`) - ffmpeg-validated
      (worst MAE 0.5/255); proves the codec interface with a 2nd decoder
- [x] AGA auto-fit: oversized clips (e.g. 640x480) are integer box-downscaled
      to fit a non-interlaced screen (`mr_scale_down_rgb24`); bad frames skip
      instead of stopping playback
- [x] MPEG-1 decode via pl_mpeg (`mr_mpeg1.c`, MIT single-file lib) -
      ffmpeg-validated on host (worst MAE ~0.9/255). .mpg is a self-contained
      stream so it gets a source wrapper, not the demux+codec split.
- [x] MPEG-1 in the Amiga player: .mpg/.mpeg play through pl_mpeg (video + MP2
      audio -> Paula), reusing the display/audio backends. The 68k build links
      a fixed-point decode path with no libm/soft-float dependency. pause/loop
      apply. (Cinepak/MJPEG path stays integer.) The Layer II decoder takes
      both MPEG-1 audio (32/44.1/48 kHz) and MPEG-2 "low sampling frequency"
      audio (16/22.05/24 kHz); the latter is what an encode aimed at Paula
      normally produces, and `mr_mp2_check` holds its PCM against ffmpeg's.
- [x] MPEG-2 program streams (`.mpg`/`.mpeg`, including DVD-style files) are
      demuxed in-tree and decoded by the existing integer libmpeg2 adapter; no
      additional video decoder dependency is required. AC-3 audio is decoded
      by the vendored fixed-point Rockbox/a52dec core and downmixed to stereo.
- [x] Microsoft MPEG-4 v2 (`MP42`/`DIV2`) in AVI: separate H.263-derived decoder
      plugin with I/P pictures, slice/DC/AC prediction, skip macroblocks and
      half-pel motion compensation. Host-validated against ffmpeg on the full
      1,983-frame BFHL sample (worst per-frame RGB MAE 1.83/255).
- [x] WMV1/WMV2 (Windows Media Video 7/8, `WMV1`/`WMV2`) in AVI: two more
      H.263-derived decoder plugins built on the same macroblock skeleton as
      MP42. WMV1 (`mr_wmv.c`) adds selectable RL/DC/MV VLC table sets,
      coded-block-pattern prediction on intra blocks, an adaptive
      "escape 3" coefficient coding mode and flipflop MC rounding. WMV2
      (`mr_wmv2.c`) builds on WMV1's exact same coefficient/DC/MV grammar
      and adds a container-level extension header, bitplane-coded
      macroblock skip, three qscale-selected CBP VLC tables, adaptive
      top-left motion-vector prediction, MSPEL motion compensation, the
      Adaptive Block Transform (8x4/4x8 split residual blocks with their
      own IDCTs), and an H.263 Annex-J-style in-loop deblocking filter.
      Both reject (rather than approximate) the low-bitrate spatial
      intra/inter-prediction variant, and WMV2 additionally rejects
      IntraX8 ("J-frame") coding - a separate sub-codec shared with VC-1.
      Host- and m68k/qemu-validated against ffmpeg (worst per-frame RGB
      MAE under ~1.2/255 across several qscales and, for WMV2, both with
      and without the loop filter enabled).
- [x] H.264/AVC Baseline/Main/High (`avc1`) via pinned Apache-2.0 Ittiam
      libavc, using its generic integer C backend: avcC/AVCC conversion, CABAC,
      B-slices and DPB display reordering. Exact 640x360 High Profile/B-frame
      sample decodes 171/171 frames and matches ffmpeg at worst MAE 1.06/255.
      Classic 68040/060 systems can decode H.264 but should not be expected to
      stream it in real time; the tested 68060/AGA system has been around 7 fps
      at best at the lowest YouTube/IPTV resolutions. Practical H.264 streaming
      is targeted at PiStorm/Emu68, using the MintVID040 build. On the tested
      Pi3-based PiStorm 600, low-resolution H.264 YouTube/IPTV streams below
      roughly 200p have played well. A faster PiStorm32/CM4 should provide more
      headroom, but that has not been quantified yet. Vampire/Apollo 68080 has
      not yet been validated.
- [x] Internet streaming: HTTP/HTTPS URL input, HLS live/VOD, and native
      YouTube resolution (see README)
- [~] RTG fullscreen toggle is implemented with a borderless screen-sized CGX
      window; a direct RGB565 CGX render path remains future work
- [x] Paula audio backend + audio-master A/V sync (`audio_paula.c`) - PCM,
      MP2, MP3, AAC-LC (raw/ADTS/LATM) and fixed-point AC-3 stereo downmix.
      MP3/AAC use the pinned MintAMP/Helix sources through
      `player/audio/mr_audio_decode.c`; host regression tests pass, and
      AC-3/general audio quality is confirmed good on real hardware.
- [x] Moon-shot heavier codec, gated to PiStorm-class practical streaming:
      delivered as H.264/AVC above
```

## Legacy codec compatibility policy

The H.263 implementation is a separate codec plugin (`mr_h263.c`); AVI and MOV
continue to pass packets and stream metadata through the generic codec API.
Its picture-layer syntax and reconstruction path are isolated from feature
checks so future H.263+ annex work can be added one tool at a time. It accepts
both picture headers (version 1 PTYPE and version 2 PLUSPTYPE), the standard
and custom picture formats, the custom picture clock, GOB and slice-structured
segment headers, unrestricted motion vectors and the per-picture rounding type,
and rejects the annexes that would change reconstruction - SAC, advanced
prediction, advanced intra coding, deblocking, PB/B pictures, modified
quantisation, the alternative inter VLC and the rest listed in README.md -
instead of silently approximating them. Decoder reset reopens the plugin and
consequently discards the reference frame.

Compatibility roadmap: H.263 version 1 and the H.263+ picture syntax are
supported; the remaining H.263+ annexes are explicitly rejected as documented
in README.md; H.261 is not yet supported;
WMV1/WMV2 are supported (see above; WMV2's IntraX8 mode is explicitly
rejected); Indeo 3, Sorenson Video 1, and VP3/Theora are planned.
# IPTV directory

`player/iptv/` is deliberately independent of ReAction and of the media
pipeline.  Its bounded token reader extracts only channel and stream metadata,
joins exact IDs before `@` feed variants, and keeps at most eight candidates.
The lightweight filter excludes NSFW, closed, and streamless records.  The M3U
fallback recognizes only `EXTM3U`/`EXTINF`, `tvg-id`, `tvg-name`, and
`group-title`; it is not a vendor IPTV-client implementation.

The ReAction browser is a separate process/window. Directory network work is
therefore isolated from the main transport event loop, while selection is sent
back through the same URL launch path used by command-line playback. Downloads
use the existing HTTP/AmiSSL implementation and a temporary-parse-rename cache
transaction, retaining a previously valid cache after any failure. Directory
JSON never enters the preferences file.

`iptvgui` reads `cache.meta` at startup and automatically refreshes channel and
stream JSON after 24 hours. It downloads through the shared HTTP/AmiSSL source
into bounded `.tmp` files, parses both files before installation, and keeps
`.old` files until both renames succeed so an interrupted or malformed refresh
cannot destroy the last usable directory.
The complete-response helper in `mr_http.c` accepts both Content-Length and
chunked/length-less responses, which avoids treating GitHub Pages' transfer
framing as a cache failure.
The browser opens before starting an expired-cache refresh and preserves the
HTTP/AmiSSL diagnostic with its failing stage. Cache setup probes
`PROGDIR:Cache/IPTV/` for writes and falls back to `T:MintVID-IPTV/` rather
than silently attempting downloads into a read-only program directory.
Optional iptv-org scalar metadata accepts JSON `null`; retained nullable strings
become empty values. Parser failures include the file, one-based object number,
field, byte offset, expected type, and leading token. The failed JSON file is
kept as `channels.failed.json` or `streams.failed.json` for Amiga-side diagnosis.
Candidate stream arrays are allocated only after a channel joins successfully;
channel-directory parsing therefore no longer reserves eight 1 KiB URLs per
channel. This avoids a large allocation jump at channel 1025 on classic systems.
Alternate-name and category arrays are likewise allocated only when present,
reducing the fixed channel record to roughly 432 bytes on the host build. The
20,000-channel table therefore needs about 8.6 MiB plus only the metadata that
actually exists, rather than a large contiguous maximum-field reservation.
The parser validates up to 100,000 JSON channel objects but retains at most
20,000 useful metadata records. NSFW, closed, replaced, unnamed, and ID-less
records are discarded immediately; after stream joining the table is compacted
again to channels with a valid URL. Crossing the retained limit is counted and
reported as a warning while syntax validation continues to the end of the file.
The runtime browser no longer uses that global-table validation path to load a
country. `mr_iptv_streaming.c` scans cached arrays with a 16 KiB stdio buffer,
retains only the selected country's compact metadata, builds an open-addressed
ID hash, and validates streams one object at a time. Unrelated streams are never
stored. Channels keep at most two alternate names/categories and four preferred
streams, then streamless channels are removed in place. Changing country
reprocesses the cache without another network request.
The ReAction chooser owns a single label/code mapping table; iptv-org's `UK`
code is authoritative for United Kingdom filtering (not ISO `GB`).
The country loader parses each 64 KiB-bounded object directly into caller-owned
records. It no longer constructs a temporary directory or pending stream table
per object. Nonmatching countries complete the required-field pass without
allocating alternate names or categories; retained channels grow geometrically,
and the first matching stream allocates four final candidate slots once. A
successful refresh transfers its already-validated directory into the GUI after
the atomic cache rename instead of reparsing both downloaded files.
Classic builds log total and largest Fast RAM before loading, after country
selection, after stream joining, and after visible ListBrowser nodes are built.
`mr_play_options` is the process-boundary contract shared by `mrgui` and
`iptvgui`. The controller captures its gadgets when IPTV is opened and passes
explicit display/C2P/lace/scale/HLS arguments to the child. The browser parses
that immutable snapshot, displays a summary, and uses the same bounded player
argument builder as ordinary controller playback when it adds a stream URL and
typed HTTP metadata. Direct browser launches initialize the documented safe
defaults rather than depending on cross-process globals.
IPTV request metadata crosses the player boundary only as the typed
`--user-agent` and `--referer` options. `mr_http_options` rejects CR/LF and
overlong values, is copied into each HTTP/HLS source instance, and is reused by
redirects, range reconnects, master/media playlist fetches, live refreshes, and
segment requests. The default remains `MintVID/0.1 AmigaOS` with no Referer;
there is deliberately no arbitrary-header command-line interface.

# YouTube search

`ytgui` is a separate ReAction process launched by `mrgui`, mirroring the IPTV
browser boundary so a blocking search request cannot stall the main transport
controls. It fetches YouTube's public search results page with the shared
HTTP/AmiSSL layer and parses bounded `videoRenderer`/`gridVideoRenderer` JSON
objects embedded in that page. The compact result model retains an 11-byte
video ID, title, channel, bounded channel ID, and live flag for at most 100
entries; duplicate IDs are discarded.
No account, cookies, Google API key, JavaScript engine, or remote resolver is
required.

The live-only checkbox is selected by default and adds YouTube's live search
filter while also verifying each result's live badge locally. A selected ID is
converted back to a canonical watch URL and launched through `mrplay`. Live
results use the existing native HLS resolution. Recorded results accept a
direct muxed 360p MP4 (`itag 18`) or optional 720p MP4 (`itag 22`), both H.264
plus AAC. The latter is preferred for 720p-or-higher GUI settings and falls
back to 360p when absent. Ciphered, foreign-host, and unsolved `n`-challenge
URLs are rejected before the HTTP demux path sees them.
The controller passes the same immutable `mr_play_options` snapshot used by
`iptvgui`, so display and HLS policy remain consistent across child windows.
The channel ID provides a direct no-key `/channel/<id>/videos` follow-up whose
uploads reuse the same parser. A typed command word in the published player
control block drives pause, fast mode, volume and RTG fullscreen; replacing a
video uses the established clean-stop then Ctrl-C escalation before launching
the next `mrplay` process.