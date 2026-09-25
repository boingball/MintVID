<h1>
  <img src="player/amiga/icons/MintVID.png" width="48" alt="MintVID icon">
  MintVID
</h1>

![AmigaOS](https://img.shields.io/badge/AmigaOS-3.0%2B-orange)
![CPU](https://img.shields.io/badge/CPU-68030%20%7C%20040%20%7C%20060-blue)
![Video](https://img.shields.io/badge/Video-H.264%20%7C%20MPEG%20%7C%20WMV-purple)
![YouTube](https://img.shields.io/badge/YouTube-Native%20Playback-red)
![Streaming](https://img.shields.io/badge/Streaming-HLS%20%7C%20IPTV-green)
![Display](https://img.shields.io/badge/Display-AGA%20%7C%20HAM%20%7C%20RTG-blue)
![AI](https://img.shields.io/badge/AI-assisted%20coding-6e7781)

[![Support](https://img.shields.io/badge/Support-Buy%20Me%20a%20Coffee-FFDD00?logo=buymeacoffee&logoColor=000000)](https://buymeacoffee.com/boingball)

![GitHub stars](https://img.shields.io/github/stars/boingball/MintVID)
![GitHub last commit](https://img.shields.io/github/last-commit/boingball/MintVID)

**Modern video playback and streaming for classic accelerated 68k Amigas.**

Play local video, HTTP/HTTPS streams, HLS, IPTV and public YouTube content
with native Amiga playback across AGA, HAM and RTG systems.
Performance scales strongly with CPU, codec, resolution and display mode;
format support is not a promise of real-time playback on every 68k.

MintVID 1.4.0 brings stereo audio through a synchronized Paula channel
pair. YouTube's 360p stream now plays on a real A1200 with a 68060/50 in
VQ Turbo+, with correct high-quality stereo sound, and on a Pi3-based
PiStorm 600 with VQ Turbo and the new RTG (Half) display mode, even though
the PiStorm's RTG has no P96 overlay. It also adds a Window display mode
for the Workbench, VQ Smoosh, audio-only playback (Display: No Video), an
HTTPS TLS 1.2/1.3 choice, and faster H.264, MPEG-1/2, AAC, AGA and P96
paths, and makes the P96 hardware video overlay work on real
Voodoo3/Permedia-class boards.

![MintVID playing an LGR YouTube video on AmigaOS](player/amiga/art/MintVID-YouTube.png)

## What's new in 1.4.0

- **Stereo audio:** Paula now plays through a synchronized left/right
  channel pair, so stereo sources keep their stereo image instead of being
  mixed down to one channel. Mono sources, and `--audio-mono`, play the
  same signal on both speakers.
- **A1200 68060/50 plays YouTube:** in VQ Turbo+ (keyframes only),
  YouTube's 360p stream plays with correct high-quality stereo audio on a
  real A1200 with a 68060 at 50 MHz. Turbo+ no longer skips audio while it
  waits for keyframes, HTTPS segments resume their TLS session instead of
  paying a full handshake, and AGA colour conversion takes about a third
  of the 68k instructions it did.
- **PiStorm 600 plays YouTube without the P96 overlay:** on the tested
  Pi3-based PiStorm 600 (MintVID040), YouTube's 360p stream plays through
  CGX with VQ Turbo and RTG (Half). The YUV->RGB24 step fell from 57 ms to
  about 12 ms per 640x360 frame there, and H.264 decode is faster too.
- **Window** (`--aga-window`) and **Window Half** (`--aga-window-half`):
  play in a sizeable window on a native-chipset (AGA/ECS/OCS) Workbench,
  matching the dither palette to the screen's pens so Workbench's own
  colours never change.
- **RTG (Half)** (`--rtg-half`): the window opens at half the video's width
  and height and H.264 is converted straight to that size, so conversion
  and the copy to the card each do about a quarter of the work.
- **VQ: Smoosh** (`--h264-speed=smoosh`): Turbo's policy, plus P/B
  pictures that are already late are not decoded. Motion smears until the
  next keyframe, but the video keeps moving and audio comes first.
- **No Video** (`--no-video`, last row of the Display chooser): plays only
  the soundtrack, with no video decode, for machines too slow for the
  picture.
- **Skip after** (`--skip-trigger=200..2000`): how far behind Video: Skip
  Frames may fall before it starts skipping.
- **Turbo+ keeps its audio:** it no longer skips about 10 s of audio while
  waiting for the next keyframe.
- **Faster decoding:** H.264 motion compensation filters two samples per
  register, Turbo skips dead deblocking work, and AAC decodes directly at
  a halved or quartered output rate (31-63% less AAC work).
- **P96 hardware overlay on real Voodoo3/P96 2.x boards:** it opens with
  `RGBFB_Y4U2V2` like the historical RiVA driver path, with the right
  chroma order (the new *P96 Output Format* menu picks YVYU for WinUAE or
  YUYV for Voodoo) and studio-range clamping. The overlay window resizes
  with the card doing the scaling, and fullscreen falls back to CGX if a
  board refuses it.
- **Bigger buffers:** Fast buffer adds 32 and 64 MB, and YouTube/IPTV HLS
  downloads several segments ahead of playback.
- **HTTPS menu:** the MintVID menu's HTTPS submenu picks TLS 1.2 (the
  faster default: a repeat connection resumes with no key exchange) or
  TLS 1.3. `mrplay --tls=1.2|1.3` overrides it for one run.
- **Faster P96 on 16-bit screens:** when the overlay is refused, H.264 is
  converted straight to the 16-bit screen format and copied a row at a
  time. On WinUAE 720p YouTube the display step fell from about 34 ms to
  about 17 ms per frame.
- **Faster MPEG-1/2:** four-pixel motion compensation and a sparse-block
  IDCT cut decode by 19-29% of 68k instructions, with byte-identical
  output.
- **Live HLS/IPTV fixes:** Turbo+ no longer stalls on streams with
  B-frames (BBC One), live playback starts about 30 seconds from the live
  edge instead of hours back, and the playlist is re-read before playback
  reaches its end.
See [CHANGELOG.md](CHANGELOG.md) for the complete release notes, including
the 1.3.2 DV decoder/EHB/MPEG-1/2 work and the original 1.3.1 P96 overlay
introduction this release builds on.

## Video frame policy

Both GUI editions expose **Video: All Frames** and **Video: Skip Frames**.
Audio-only playback (`--no-video`) is the Display chooser's **No Video**
row; the Video, Skip after and VQ controls grey out under it.
All Frames is the default and preserves every decoded picture. For most
codecs, Skip Frames discards late decoded output. For H.264, sustained
lateness also escalates libavc to `IVD_SKIP_PB`, avoiding most P/B-picture
decode work until the next IDR; this was confirmed with an overloaded 720p
stream under WinUAE. Skip Frames
allows the scheduler to drop pictures that are already late, so a slower
Amiga can catch up while audio and timestamps continue normally. It is a
presentation policy, not a codec or bitstream change.

The command-line equivalents are `--throughput` and `--no-throughput`;
`--skip-trigger=MS` (GUI "Skip after") sets how far behind Skip Frames may
fall before it starts skipping.
Use Skip Frames when a demanding source is falling behind; keep All Frames
when playback is already smooth or every decoded picture matters. See the
[AmigaGuide manual](MintVID.guide) for hardware-specific starting points.

## About MintVID

A codec-agnostic video player for 68k AmigaOS — built in the spirit of
MintAMP (the libhelix audio player): a small, portable C core with thin
Amiga-specific layers. MP3/AAC decoding reuses the proven MintAMP/Helix code,
with audio output through Paula.

The goal is to go **beyond MPEG-1** on accelerated 68k Amigas — from
68030-class ECS/AGA systems through 68040/060 machines to PiStorm/RTG.
MintVID provides a broad range of codecs, but what is practical in real time
depends heavily on CPU speed, codec complexity, resolution, bitrate and
display mode. Codec support does not imply real-time playback on every CPU.
See **[DESIGN.md](DESIGN.md)** for the full architecture and roadmap.
For a repeatable real-hardware baseline, see **[68060 @ 50 MHz codec performance & compatibility](docs/68060-50mhz-performance.md)**.

### Hardware and performance expectations

- **68030-class ECS/AGA:** best suited to lightweight codecs and modest frame
  sizes. Cinepak is the natural starting point; heavier formats may decode
  correctly without being practical in real time.
- **68040/060:** older codecs such as Cinepak, MJPEG, MPEG-1/2, MPEG-4 Part 2
  and WMV7/8 become more practical at modest resolutions, especially with RTG.
  H.264/AVC remains extremely demanding, but the 1.2.0 H.264 and AAC work moved
  the lowest-resolution BBC One HLS stream close to real time on a tested real
  68060 using AGA/HAM8. Results remain highly dependent on clock speed, stream,
  audio, resolution and display mode; higher resolutions are not expected to
  be real-time on classic CPUs. Turbo+ deliberately favours continuous audio
  and occasional keyframes when the full video rate is beyond the machine:
  in 1.4.0 that plays YouTube's 360p stream with full-quality stereo sound
  on a real A1200 with a 68060/50.
- **PiStorm/Emu68:** use the **MintVID040** build. This is the release build
  targeted for the Emu68/PiStorm environment. H.264 becomes much more practical;
  on a tested Pi3-based PiStorm 600, YouTube's 360p stream plays with VQ Turbo
  and RTG (Half). Faster PiStorm hardware should provide more headroom, but
  results still depend on the source and configuration.
- **Vampire/Apollo 68080:** currently unvalidated by the MintVID project. No
  optimised build is officially recommended yet, and the 68060 build should not
  be assumed to be the right choice solely from the CPU name.

This repository began life inspired by **RiVA 0.54**, the fastest 68k MPEG-1
player (Stephen Fellner, László Török, Henryk Richter). RiVA's assembly source
was studied for ideas during design but was never built on or shipped as part
of MintVID, so it is not carried in this tree — see the original RiVA release
on Aminet for that source and its own GPL-2.0/dual GPL-MIT licensing.

## Status

| Component | State |
|-----------|-------|
| Decoder interface + registry | ✅ |
| Container-agnostic demux (auto-detect) | ✅ |
| AVI, QuickTime MOV/MP4, Matroska/MKV and MPEG-TS/M2TS demuxers | ✅ packet-streamed from disk or HTTP(S); no whole-file allocation |
| HTTP/HTTPS URL input | ✅ redirects, byte-range seeking and 256 KiB rewind cache |
| Public YouTube URLs | ✅ live HLS plus experimental muxed 360p/720p H.264/AAC playback for compatible uploads |
| YouTube search | ✅ no-key ReAction and OS 3.0 GadTools browsers; All/Videos/Live/Shorts/Hashtags modes and native playback handoff |
| Cinepak (CVID) decoder | ✅ ffmpeg-validated (AVI + MOV) |
| Microsoft Video 1 — MSVC/CRAM AVI | ✅ native 8/16-bit RGB24 decoder; compatible WHAM streams accepted |
| Microsoft RLE8 — palettised AVI | ✅ native palette and delta-frame decoder (RLE4 deferred) |
| Windows Media Video 7/8 — WMV1/WMV2 AVI | ✅ native decoders; ffmpeg + big-endian m68k/QEMU validated |
| Raw UYVY422 (`2vuy`/`UYVY`) | ✅ uncompressed QuickTime/MOV video |
| Runs on real 68k hardware | ✅ decode verified |
| MJPEG / MPEG-1 / MPEG-4 Part 2 / Microsoft MP42/DIV2 decoders | ✅ ffmpeg-validated |
| MPEG-2 Main Profile video | ✅ libmpeg2; TS + B-frames ffmpeg-validated |
| H.264 High Profile (`avc1`, CABAC, B-frames) | ✅ libavc; ffmpeg-validated |
| MPEG-TS/M2TS MPEG-1/2 or H.264 + AAC/MP2/AC-3 | ✅ ADTS or LATM AAC; 188/192-byte packets; ffmpeg-validated |
| Matroska/MKV | ✅ H.264/MPEG-4/MPEG-2/MJPEG video; AAC/MP3/MP2/AC-3/PCM audio; common lacing supported |
| Raw MJPEG + raw MPEG-4 Visual streams | ✅ |
| DV (IEC 61834/SMPTE 314M) — PAL 4:2:0 and NTSC/DVCPRO 4:1:1 | ✅ native decoder (libdv-derived); ffmpeg-validated; Quality/Fast (DC-only, ~1.9x) speed modes |
| Amiga RTG / AGA output | ✅ |
| ReAction + GadTools controllers | ✅ matching file, IPTV and YouTube frontends for modern and OS 3.0 systems |
| IPTV directory core | ✅ bounded iptv-org JSON/M3U parsing, joining and local filters |
| PCM / MP2 / MP3 / AAC-LC / AC-3 audio to Paula | ✅ host-validated; MP2 covers MPEG-1 and MPEG-2 Layer II (16 kHz upwards) and MP3 all three Layer III versions including MPEG-2.5 (8/11.025/12 kHz); AC-3 uses fixed-point stereo downmix. AC-3 and .mpg MP2 are checked against ffmpeg's own decode (`mr_ac3_check`, `mr_mp2_check`), on the host and on m68k |
| Mono decode (`--audio-mono`) | ✅ decoder-side for MP3/MP2/AC-3, post-decode for AAC; host-validated against the stereo decode |

## Building & testing the portable core (dev host)

The `player/core` code is plain C99 with no Amiga dependencies, so it builds and
is validated on a normal machine before it ever meets a 68k toolchain.

The H.264 tier uses GCC (including the m68k GCC build); the legacy vbcc target
continues to build the lighter codecs without libavc.

`mrplay` carries a `$STACK:320000` AmigaOS stack cookie because libavc needs
substantially more stack than the classic Shell default. On systems that do
not honour stack cookies, run `Stack 320000` before starting the player.

The normal Amiga build remains 68030-compatible. Optimised 68040 and 68060
builds can be selected explicitly, or packaged together in `player/release/`:

```sh
cd player
make -f Makefile.amiga all SSL=1 SSLCERTS=1 CPU=68030
make -f Makefile.amiga all SSL=1 SSLCERTS=1 CPU=68040
make -f Makefile.amiga all SSL=1 SSLCERTS=1 CPU=68060
make -f Makefile.amiga release SSL=1 SSLCERTS=1   # release/MintVID030, 040 and 060
```

MintVID compiles its MintAMP/Helix AAC decoder directly into `mrplay`.
`AACASM=1` is the default: 68030/040 builds use the hardware full-result
`MULS.L` path, while the 68060 build reconstructs the same result with
hardware two-operand partial products and avoids the emulated register-pair
instruction. For a portable-C comparison, clean and rebuild with
`AACASM=0`:

```sh
make -f Makefile.amiga clean
make -f Makefile.amiga mrplay CPU=68060 AACASM=0
```

For normal classic systems use the build matching the CPU. **PiStorm/Emu68
users should use MintVID040.** Vampire/Apollo 68080 is not yet validated, so
there is no official optimised-build recommendation for it. Use MintVID060 only
on systems where a 68060-targeted build is known to be appropriate.
`release/MintVID030`, `release/MintVID040` and `release/MintVID060` are
ready-to-run sets with ordinary unsuffixed program names. Each contains
`mrplay`, the ReAction `MintVID`/`iptvgui`/`ytgui` set, the GadTools
`MintVID-GT`/`iptvgui-GT`/`ytgui-GT` set, and the command-line `mr_decode`
codec probe/test harness. `MintVID` is the flagship binary (built from
`mrgui.c`, whose Makefile target and output are named `MintVID`) - it is the one
meant to carry the Workbench icon and be double-clicked, with the rest alongside
it in the same directory as support binaries it loads on demand. If
`player/amiga/icons/MintVID.info` and the matching
`MintVID030.info`/`MintVID040.info`/`MintVID060.info` drawer icons are present,
the release target also copies them in: `MintVID.info` goes inside each
`MintVID0xx/` directory next to the `MintVID` executable, and each
`MintVID0xx.info` goes into `release/` itself, next to (not inside)
`MintVID0xx/`, as its drawer icon - the normal AmigaOS convention of a
`<name>.info` file living beside the `<name>` it decorates. The release target
finishes by restoring the working binaries to the baseline 68030 build.

The release target also creates `release/LICENSES/` and copies MintVID's own
licence plus the available libmpeg2, libavc and MintAMP/Helix licence/notices
from the checked-out dependencies. It fails rather than silently producing a
binary release when the required notice files are missing. Binary distributors
must still provide the corresponding source required by the licences; use a
recursive checkout (`git clone --recurse-submodules`) so the pinned MintAMP and
libavc sources are included.

```sh
git submodule update --init --recursive
cd player
make            # builds ./mr_decode
make check      # decodes a Cinepak clip and diffs against ffmpeg (needs ffmpeg)
make check-audio # MP3, AAC ADTS/LATM and fixed-point AC-3 decoder checks,
                 # including AC-3 PCM compared against ffmpeg sample by sample
make check-http # local HTTP range/redirect integration tests
make check-https # the same tests over TLS (needs OpenSSL development files)
```

Inspect or dump any AVI/MOV/MP4/MKV/TS/M2TS:

```sh
./mr_decode file.avi                 # stream info + frame count
./mr_decode file.avi --ppm outdir    # write decoded frames as PPM
```

`mrplay` streams AVI, MOV/MP4, Matroska/MKV and MPEG-TS/M2TS packets from disk or a direct
`http://`/`https://` file URL. Apart from the optional Fast buffer below,
its RAM use is set by container metadata, the largest compressed packet, a
4 MB network rewind cache, and the active decoder/display buffers rather than
by the media file size. HTTP redirects and byte-range seeking are supported:

```sh
mrplay "http://example.net/video.avi"
mrplay "https://example.net/video.mp4"
mrplay --user-agent "Mozilla/5.0" --referer "https://example.net/" \
  "https://example.net/live/master.m3u8"
mrplay --hls-max-width=640 --hls-max-height=360 \
  "https://www.youtube.com/watch?v=LIVE_STREAM_ID"
mrplay --fast-buffer=8 "DH0:Videos/movie.avi"
```

Plain HTTP is present in the normal Amiga build. HTTPS uses
`amisslmaster.library`/AmiSSL v5 and must be enabled when compiling:

```sh
make -f Makefile.amiga mrplay SSL=1
```

For compatibility with typical classic Amiga AmiSSL installations, that mode
uses TLS and SNI but does not verify the server certificate by default. Build
with `SSL=1 SSLCERTS=1` to enable the default CA roots and hostname
verification; this is the recommended setting for packaged online-enabled
release builds.

### Fast RAM buffer

`--fast-buffer=auto|off|4|8|16|32|64` sets how much Fast RAM `mrplay` uses to
buffer the compressed input. What the buffer does depends on the source:

| Source | What the Fast buffer does |
|--------|---------------------------|
| Local file that fits in the buffer | The whole file is read into Fast RAM when it opens, and playback never touches the disk again. |
| Larger local file | The buffer becomes a large read-ahead window, so demuxing goes to AmigaDOS far less often. |
| Direct/progressive HTTP (including YouTube 360p MP4) | Rewind/read-ahead cache that also soaks up data already waiting on the socket while the CPU decodes. |
| HLS (IPTV, YouTube HLS) | Bounds a queue of downloaded segments: up to eight ahead for recorded streams, three for live streams. |

`auto` picks 16, 8 or 4 MB from the largest free Fast RAM block while leaving
24 MB free for the decoder, display, TLS, audio and frame queue; it never goes
above 16 MB. 32 and 64 MB are explicit choices for machines with plenty of
Fast RAM. A fixed size keeps at least 8 MB free and halves itself until it
fits. `off` keeps the small normal file buffer and HTTP's 4 MB compatibility
cache, and HLS then fetches only the next segment ahead. `mrplay` prints the
size it actually got at startup. The buffer is always allocated from Fast
RAM, never Chip RAM.

Loading a whole file takes as long as reading it once, and that read happens
before the first frame, so a big file on a slow drive or network share can
take a while to start. Choose a size smaller than the file, or Off, if the
wait matters more than disk access during playback.

The HLS segment queue starts filling as soon as the first segment opens, so
playback does not wait for the buffer to fill. One network worker fetches
segments one at a time, within the byte budget. At the live edge a segment
can only be fetched once the playlist advertises it.

The Fast buffer holds compressed input only. Decoded pictures go into a
separate frame queue of up to 48 frames, which grows with free RAM for local
files and network streams alike.

### Live streaming resilience

Live HLS (`.m3u8`) playback on constrained hardware has a few extra controls.
The AmiSSL library, TLS context, and TLS session are initialised once and reused
across segments, so each segment boundary reconnects with an abbreviated
handshake instead of the full per-segment bring-up. With Fast buffer Off,
only the next segment is fetched ahead. Any Fast buffer size enables a
RAM-bounded queue of up to three known segments for live streams (eight for
recorded ones), combined with the no-abandon worker shutdown and AmiSSL
lifecycle hardening.

- `--net-queue=N` — request a decoded-frame read-ahead target for network
  playback. The default scheduling target is 1 frame and the hard ceiling is
  48; the RAM-bounded frame ring may reserve more slots internally. A few
  frames absorb per-frame decode jitter; a deep target (for example
  `--net-queue=24`) lets video sit ahead of the audio clock and present in
  order, keeps the loop demuxing so the audio FIFO stays fed, and rides across a
  segment-boundary refetch. Costs one RGB frame of RAM per used slot.
- `--live-resync` — recover from big disruptions. If a stall leaves playback
  more than ~4 s behind the wall clock, it fast-consumes the buffered backlog
  (decode reference-only, discard audio) and re-primes near the live edge; and
  if the stream drops out entirely it reopens the URL and resumes rather than
  ending. It never fires in normal playback. GUI-launched playback (`MintVID`, the
  IPTV browser) enables this by default since IPTV streams are always live; a
  direct `mrplay <url>` leaves it off. Use `--no-live-resync` to opt out.

```sh
mrplay --net-queue=24 --live-resync "https://example.net/live/master.m3u8"
```

### YouTube

`mrplay` can resolve a public YouTube Live watch/share URL natively. It fetches
the watch page with a browser user agent, extracts and JSON-decodes the signed
`hlsManifestUrl`, validates that it is an HTTPS `manifest.googlevideo.com`
playlist, then hands it to the normal HLS variant/segment pipeline. Resolution
happens on the Amiga itself, so IP-bound signed URLs are not borrowed from a
remote service. `--hls-low` and the HLS quality ceilings still apply.
The HTTP/HLS path accepts signed URLs up to 4095 bytes, since current YouTube
manifest URLs can exceed the older 1 KiB media-URL limit.

For ordinary uploads, the resolver also experiments with YouTube's muxed
360p MP4 (`itag 18`) and, where still supplied, muxed 720p MP4 (`itag 22`).
Those formats contain H.264 video and AAC audio together,
so it can use MintVID's existing seekable HTTP/MP4 path without downloading or
merging separate streams. Only a direct signed HTTPS Google Video URL is
accepted; ciphered URLs and unresolved player `n` challenges are rejected.
Selecting 720p, 1080p, or Best makes recorded playback try 720p first and fall
back to 360p automatically. Low, 360p, and 480p retain the 360p muxed format.
There is no standard muxed 480p or 1080p target here: dependable higher
resolutions require separate adaptive video and audio streams and are deferred
to the next phase.

On classic 68040/060 hardware, successful H.264 decoding should not be read as
a claim of smooth YouTube playback. A tested 68060 using AGA has managed around
7 fps at best at the lowest online resolutions. PiStorm/Emu68 is the practical
H.264 streaming target.

This remains intentionally narrow: age/login/region-restricted videos, DRM,
uploads without a usable muxed 360p/720p format, and private-schema changes can
all produce a clean unsupported error. YouTube can change these internal clients
and responses, so the resolver may require maintenance.

The ReAction controller's **YouTube...** button opens the separate `ytgui`
search window. It searches YouTube's public results page without an API key,
shows the title and channel, and starts the selected result through the same
native resolver. The **Quality** button cycles through Low, 360p, 480p, 720p,
1080p, and unrestricted Best. For recorded videos, 720p/1080p/Best try the
compatible muxed 720p format and fall back to 360p; the other choices use 360p.
The search-type selector defaults to **Live** and also offers **All**,
**Videos**, **Shorts**, and **Hashtags**. Hashtags mode accepts one tag with or
without its leading `#` and opens YouTube's dedicated `/hashtag/<tag>` page.
Build with `SSL=1` and keep `ytgui` beside `MintVID` and `mrplay`. As with watch-page
resolution, this deliberately small parser may need maintenance if YouTube
changes its private page schema.

**Already have a YouTube link?** Paste it directly into the `ytgui` or
`ytgui-GT` search field and run the search. MintVID recognises normal
`youtube.com/watch?v=...`, `/live/`, `/shorts/`, `/embed/`, and `youtu.be/...`
links, including common scheme/mobile prefixes and extra query parameters. A
recognised URL becomes one selected result ready for **Play**, so there is no
need to search YouTube again for a video you already found elsewhere. URL
recognition itself is local; playback still uses MintVID's normal YouTube
resolver and HTTPS/AmiSSL support.

Selecting a result and pressing **Channel videos** follows its bounded channel
ID to the public channel `/videos` page and lists that channel's uploads. The
transport row controls the separate player process: Play first cleanly replaces
the current video, Pause and Fast toggle their modes, Vol -/+ adjusts Paula in
steps, Fullscreen toggles the RTG window, and Stop exits the player.
Double-clicking a result plays it directly, without a separate Play press.

## GUI editions

The Amiga build creates two Workbench-friendly GUI sets over the same player,
parsers, playback settings, and status/control protocol:

- `MintVID` (from `mrgui.c`), `iptvgui`, `ytgui` use ReAction V44.
- `MintVID-GT` (from `mrgui_gadtools.c`), `iptvgui-GT`, `ytgui-GT` use only
  GadTools/Intuition V37 and are intended for a standard AmigaOS 3.0
  installation. Start `MintVID-GT`; it opens the matching `-GT` browsers
  automatically.

Keep one complete GUI set beside `mrplay` (or put `mrplay` on the command
path), run the controller, choose a
movie and select **AGA**, **EHB**, **HAM6**, **HAM8**, or **CGX**. **Laced** and **2x**
apply to the chipset modes, including HAM6 and HAM8. A laced screen is opened
when the source height after the requested 2x scale exceeds the non-laced
256-line canvas. Exact 2x eight-plane output, including HAM8, uses the fused
Kalms 2x2 converter; widths that are not a multiple of 16 are safely padded to
keep that fast path. On 68040/68060 builds, substantially narrower 1x output
uses Kalms' bitmap converter so the black side borders are not needlessly
transposed. Other sizes are fitted while preserving aspect ratio.
CGX playback opens a size-gadget window and scales the video as that window is
resized. The **C2P** chooser selects the standard graphics.library path, CD32 Akiko
hardware, or the Kalms converter for chipset playback. Kalms is the default;
unsupported geometry or bitmap layouts fall back safely to graphics.library.
For H.264 on a HAM6 or HAM8 screen whose height is an exact whole fraction of
the decoded height — the 640x360-into-640x180 shape a non-laced AGA fit
normally produces — the player converts the decoder's YUV planes straight to
HAM pixel bytes instead of building a full-resolution RGB24 frame and encoding
that, converting only the rows the downscale keeps and cutting the conversion
by about 45%. Other HAM geometries keep the established RGB24 route.
Changing output mode restores Kalms whenever the new mode has a matching
kernel. CD32 is only offered when Akiko's hardware ID is detected; an explicit
Akiko selection is preserved. The chooser is disabled for CGX. Play starts the
selected movie, Pause toggles playback, Stop exits it, and Fast forward toggles
unpaced decode.

**Audio controls**

The **Audio** chooser picks the Paula output rate (Normal, or Low to halve it
again), **No audio** skips the decoder and Paula entirely, and **Mono audio**
(`--audio-mono`) asks the codec for one channel instead of two. Paula plays
stereo through a synchronized left/right channel pair; in mono the one decoded
channel plays on both speakers. MP3, MP2 and AC-3 then skip roughly half of
their per-channel synthesis. Helix AAC has no mono mode, so an AAC track only
saves the downmix. Worth a try when a heavy H.264 stream is starving the audio
FIFO.

**Fast RAM buffering**

The **Fast buffer** chooser (Auto, Off, 4, 8, 16, 32 or 64 MB) is in both the
ReAction and GadTools editions and defaults to Auto. It applies to ordinary
**Play**, IPTV and YouTube launches, and is the `--fast-buffer` option
described under [Fast RAM buffer](#fast-ram-buffer): a local file that fits is
loaded whole into Fast RAM, larger files and progressive HTTP get a read-ahead
window, and HLS gets a queue of downloaded segments. If a large local file on
a slow drive takes a long time to start, pick a smaller size or Off.

**VQ (video quality) modes**

The **VQ** chooser trades picture quality for decode speed. It mostly sets
H.264's performance mode (`--h264-speed=`). For DV and MPEG-1/2, Quality
decodes in full and every other choice picks that codec's fast mode: DC-only
DV (`--dv-speed=fast`) or MPEG-1/2 B-frame skipping (`--mpeg2-speed=fast`).
Turbo is the default:

| Mode | H.264 decoder policy | When to use it |
|------|----------------------|----------------|
| **Auto** | Resolves to Turbo. | Keep the release default. |
| **Quality** | Full filtering; no deliberate frame skipping. | Quality comparisons or very fast systems. |
| **Balanced** | In-loop deblocking disabled; motion compensation stays spec-exact. | Mild quality/performance trade-off. |
| **Fast** | Balanced plus bilinear rather than six-tap interpolation; keeps every frame. | Prefer this when avoiding deliberate frame skips matters more than maximum speed. |
| **Turbo** | Fast policy plus B-frame skipping. | The default: extra speed while preserving the P-frame reference chain. |
| **Turbo+** | Skips both P- and B-frames. | Last-resort keyframe/slideshow mode; not recommended for normal viewing. |
| **Smoosh** | Turbo, plus P/B pictures that are already late are not decoded. | Streams with no B-frames, such as YouTube 360p. Motion smears until the next keyframe, but the video keeps moving and audio comes first. |

Every mode from Balanced down also disables deblocking on keyframes: leaving
some pictures undegraded made the decoder filter the others against stale
per-macroblock deblocking parameters, which was both wrong and slow enough that
Fast ran *slower* than Quality. Every mode below Quality is markedly faster
than in 1.2.0; measured on a 320x180 CABAC stream, Fast by 53%, Turbo by 49%
and Balanced by 30%, with Quality itself 8% faster at bit-identical output.

TurboGT was retired in 1.3.1, because its policy had become identical to
Turbo's. `--h264-speed=turbogt` (and `turbo-gt`) still work on the command
line as aliases for Turbo, but neither GUI offers it any more.

In RTG/CGX mode, `F` switches the live player between its resizeable window and
a borderless public-screen-sized view without restarting decoding; `--fullscreen`
starts in that view. Press `F` again—or use ytgui's **Fullscreen** button—to
restore the previous window geometry. AGA display modes remain hotkey-driven
and ignore the RTG-only fullscreen command. Cursor left/right seek 10 seconds
at a time for local QuickTime MOV/MP4 files, landing on the nearest keyframe
via the sample index rather than pretending that fast decode is a seek
operation. AVI, MKV and network/live sources don't have a keyframe index yet
and keep cursor-right as the fast-forward toggle instead. Cursor up/down adjust
Paula's volume in the same 8/64 steps as ytgui's **Vol -**/**Vol +** buttons.

Every GUI has a **MintVID** title-bar menu with **Guide...** (opens
[`MintVID.guide`](MintVID.guide) via AmigaGuide, with full in-app help and a
codec support list - keep the file beside the binaries; the release target
copies it in automatically), **About MintVID...** and **Quit**, which closes
that frontend cleanly.

The controller's file gadget identifies the selected file, and both the
ReAction and GadTools controllers now remember the last drawer a video was
picked from (`ENVARC:MintVID.lastdir`) and reopen there next time. On launch, `mrplay`
also reports the container type, video codec/FourCC, dimensions, frame rate and
audio format to its console, which is useful metadata when testing unfamiliar
files. Once a file starts playing, the same Info: field also mirrors a live
playhead - `H:MM:SS`/`M:SS` in the current stream's own timeline, refreshed
about once a second - for local files and any stream played through the
IPTV/YouTube browsers, so seeking with cursor left/right shows where you
actually landed. On RTG (CGX/P96), the same playhead also appears in the
video window's own title bar, so it stays visible even with the controller
window elsewhere.

Direct AVI/MOV/MP4 URL input still needs a finite, byte-addressable resource:
the server must supply `Content-Length` or `Content-Range`, and must honour byte
ranges when the container seeks. MPEG-TS also accepts a forward-only chunked
response, while HLS playlists use the dedicated live/VOD source. Fragmented MP4
is not supported yet.

TS currently supports MPEG-1/2 or AVC/H.264 video with MP2, ADTS/LATM AAC or
AC-3 audio. Raw MJPEG/M4V and MPEG-1 program streams still use the original
whole-file input path and therefore do not accept URLs.

### IPTV browser

Both controllers include an **IPTV...** launcher for their matching directory
window. Build all editions together using
`make -f Makefile.amiga all SSL=1 SSLCERTS=1`; keep `MintVID`, `iptvgui`,
`ytgui`, the three `-GT` programs, and `mrplay` together. `SSL=1` enables AmiSSL for YouTube searches and
the iptv-org directory download; `SSLCERTS=1` enables certificate verification.
A browser built
without HTTPS support remains usable for cached data and manual URLs, but a
refresh explicitly reports that it must be rebuilt with `SSL=1`.
The browser immediately reads valid cached `channels.json` and `streams.json`
from `PROGDIR:Cache/IPTV/`. Its default public directory is iptv-org
(`channels.json`, `streams.json`, `countries.json`, and `categories.json`).
MintVID does not host or redistribute television channels: iptv-org is a
collection of publicly available links, and individual links may be offline,
geo-blocked, or require request headers.

The directory reader is bounded and retains only the metadata used for local
country/category/search filtering.  Cached JSON is used immediately, refreshed
after 24 hours, and replaced only after a complete download parses successfully;
a failed refresh leaves the prior cache intact.  Manual HTTP/HTTPS media URLs,
M3U8 playlists, and simple `#EXTM3U` lists use the normal MintVID URL/player
pipeline.  Playback still depends on the existing demuxers and codecs. HLS
prefers supported low-resolution variants (maximum width 640 by default), and
cannot make DRM, login-only, unsupported-codec, or dead streams playable.

Classic 68040/060 H.264 results depend heavily on the exact stream and output
mode. With the 1.2.0 optimisations, the lowest-resolution BBC One HLS stream
has approached real time on a tested real 68060 using AGA/HAM8, while higher
resolutions remain beyond normal classic hardware. PiStorm/Emu68 remains the
practical tier for broader H.264 IPTV viewing.

Cached JSON is processed incrementally with a 16 KiB buffer. Only the selected
country is held in RAM; unrelated global streams are validated and discarded.
Changing country rebuilds the compact directory from cache without downloading
the API files again. Each channel retains at most four preferred stream URLs.
Country filtering uses the directory's own codes (`UK` for United Kingdom and
`US` for United States), rather than deriving ISO codes from display labels.

Per-stream `Referer` and `User-Agent` values are retained by the IPTV model and
passed as typed, bounded `mrplay` options. They follow redirects, HLS variant
playlists, live-playlist refreshes, segments, and range reconnects. Values with
CR/LF or values exceeding their fixed limits are rejected, and the options are
owned by one playback source so they cannot leak into a later channel. The IPTV
window's **Next Stream** button advances through the retained alternatives
without silently looping. Double-clicking a channel plays it directly, without
a separate Play press.

IPTV playback inherits a snapshot of the controller's display, C2P, lace, 2x,
audio, VQ, Video and Fast buffer selections when **IPTV...** is pressed. The IPTV
window shows that snapshot beside its status; close and reopen it after changing controller
settings. A Shell-launched `iptvgui` uses safe AGA/Standard, lace-off, 2x-off,
low-bandwidth HLS defaults. The shared bounded argument builder is also used by
the main controller's ordinary **Play** action, so both paths map identical
settings to identical `mrplay` display flags.

For a real-hardware fragmentation check, record both `AvailMem(MEMF_FAST)` and
`AvailMem(MEMF_FAST|MEMF_LARGEST)`, then open `iptvgui`, wait for the list, and
close it ten times. The loader prints those values around each loading phase and
after ListBrowser construction; neither total Fast RAM nor the largest block
should show a meaningful downward trend across completed open/close cycles.

## Layout

```
DESIGN.md            architecture & roadmap
player/core/         portable C core: demux + video decoders
player/audio/        packet adapter for MP2, MintAMP MP3/AAC and fixed AC-3
player/amiga/        RTG/AGA display, Paula output and player frontend
player/tests/        host test harness + fixtures
player/vendor/       pinned/vendored build dependencies
```

## Support MintVID

MintVID is made by Darren “boingball” Banfi, with a frankly unreasonable
number of classic-Amiga test runs and LLM-assisted development sessions. If the
player is useful—or if YouTube on an Amiga made you laugh—you can help keep the
hardware experiments and token fund moving at
[buymeacoffee.com/boingball](https://buymeacoffee.com/boingball).

## Licensing

MintVID's own code (`player/core/`, `player/amiga/`, `player/audio/`,
`player/iptv/`, `player/youtube/`, and the GUI frontends) is [MIT](LICENSE).

Several vendored/pinned dependencies keep their own upstream licences, and
distributing a *built binary* means complying with all of them at once, not
just MIT:

- **libmpeg2** (VideoLAN) and the fixed-point **Rockbox/a52dec AC-3** core are
  GPL-2.0-or-later, and are statically linked into `mrplay`. That makes the
  compiled binary a combined work under GPL-2.0-or-later — redistributing
  binaries obliges you to also offer corresponding source, per the GPL, even
  though MintVID's own contribution is MIT.
- **Ittiam libavc** (H.264) is Apache-2.0.
- **MintAMP/Helix** is a separately licensed submodule; retain its notices.
  In particular the Helix AAC decoder path it pulls in is licensed under
  RealNetworks' RPSL, not GPL — keep that distinct when redistributing.

`THIRD-PARTY-LICENSES.txt` indexes the binary's third-party components and the
`release` target copies the available upstream licence texts/notices into
`release/LICENSES/`. Retain those files with binary distributions. RiVA 0.54,
which inspired this project's design but is not included in this repository,
is itself GPL-2.0 with dual GPL/MIT renderers — see the original RiVA release
on Aminet.

## VLC-era video compatibility (wave 1)

H.263 video in AVI, QuickTime MOV and 3GP is supported, in both the version 1
(H.263-1996) and version 2 (H.263+/H.263-1998) picture syntaxes: the standard
sub-QCIF/QCIF/CIF/4CIF/16CIF formats and H.263+ custom picture formats (any
multiple of 4, such as the 68x52 produced by `ffmpeg -c:v h263p`), the custom
picture clock frequency, GOB headers and slice-structured mode (Annex K, with
ordered non-rectangular slices), unrestricted motion vectors (Annex D), the
per-picture rounding type that encoders flip-flop between P pictures, skipped
macroblocks, half-pixel motion compensation and persistent reference frames.
The decoder rejects malformed or truncated syntax and refuses the remaining
H.263+ tools rather than producing corrupt output, naming the one it found:
SAC, advanced prediction/4MV, advanced intra coding, the deblocking filter,
PB/improved-PB and B pictures, reference-picture selection, independent segment
decoding, the alternative inter VLC, modified quantisation, reference picture
resampling, reduced-resolution update, rectangular or unordered slices,
continuous-presence multipoint and scalability. H.263+ is therefore still
**partial**. H.261 is not yet supported. WMV1 and WMV2 (Windows Media Video 7/8) are supported - see below;
WMV2's IntraX8 ("J-frame") mode, a separate sub-codec shared with VC-1, is
explicitly rejected rather than approximated. Indeo 3, Sorenson Video 1, and
VP3/Theora are planned.

The FourCC audit below is deliberately conservative. “Registry” means the alias
is covered by the deterministic routing test; a named clip means its bitstream
was also decoded by the existing conformance suite.

| FourCC | Codec family | MintVID decoder | Status | Tested sample |
|---|---|---|---|---|
| `DIVX`, `DX50`, `XVID`, `xvid`, `FMP4`, `MP4V`, `mp4v` | ISO MPEG-4 Part 2 | `mpeg4` | accepted | `test_mp4v_sp.avi`; registry |
| `3IV2`, `3iv2`, `3IVX` | 3ivX / ISO MPEG-4 Part 2 | `mpeg4` | accepted | registry; upstream sample inspection pending |
| `RMP4`, `BLZ0`, `SEDG`, `M4S2`, `MP4S` | ISO MPEG-4 Part 2 vendor aliases | `mpeg4` | accepted | registry |
| `DIV2`, `MP42` | Microsoft MPEG-4 v2 | `msmpeg4v2` | accepted, kept separate | `test_div2.avi`, `test_mp42.avi` |
| `DIV1`, `MP41` | Microsoft MPEG-4 v1 | none | unsupported | registry rejection |
| `DIV3`, `MP43`, `AP41`, `COL1`, `COL0` | Microsoft MPEG-4 v3 / DivX 3 | none | unsupported; never routed to ISO ASP | registry rejection |
| `DIV4`, `DIV5`, `DIV6` | ambiguous DivX-era vendor tags | none | unsupported pending sample verification | registry rejection |
| `H263`, `h263`, `I263`, `i263` | H.263 | `H.263` | accepted | `test_h263.avi`, `test_h263_gob.avi` |
| `s263`, `S263` | H.263 in QuickTime/3GP | `H.263` | accepted | `test_h263.3gp` |
| `U263`, `u263`, `T263`, `X263` | vendor H.263 / frequently H.263+ | `H.263` | accepted; unsupported annex flags are rejected | registry; `test_h263p.avi`, `test_h263p_umv.avi` cover the H.263+ syntax |
| `WMV1`, `wmv1` | Windows Media Video 7 | `wmv1` | accepted | `test_wmv1.avi`, `test_wmv1_q20.avi` |
| `WMV2`, `wmv2` | Windows Media Video 8 | `wmv2` | accepted; IntraX8 ("J-frame") mode rejected | `test_wmv2.avi`, `test_wmv2_q20.avi` |
