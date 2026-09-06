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

![MintVID playing an LGR YouTube video on AmigaOS](player/amiga/art/MintVID-YouTube.png)

## What's new in 1.2.0

- **Faster H.264 on classic m68k:** inverse-transform/reconstruction hot loops
  inline their tiny per-coefficient helpers, removing large numbers of
  subroutine calls without changing decoded pixels.
- **68060-tuned H.264 deblocking:** luma filtering uses a branchless absolute-
  difference primitive selected only by the 68060 build; the shorter 030/040
  sequence remains unchanged.
- **CPU-aware AAC acceleration:** 68030/040 builds use their hardware full-
  result multiply path, while 68060 reconstructs the same result from hardware
  partial products instead of trapping into software-emulated register-pair
  `MULS.L`. The result is bit-exact and is enabled by default.
- **Shared accelerated YUV output:** H.263, MPEG-2, MPEG-4 Part 2, MP42/DIV2,
  WMV1 and WMV2 now use MintVID's existing table-driven C converter and m68k
  assembly path instead of six private multiply-heavy loops.
- Real-A1200 testing confirmed clean stereo AAC on the 68060 and showed the
  lowest-resolution BBC One H.264/HLS stream approaching real time in AGA/HAM8;
  Turbo+ remains a useful audio-first keyframe/slideshow fallback.

See [CHANGELOG.md](CHANGELOG.md) for the complete release notes.

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
  and occasional keyframes when the full video rate is beyond the machine.
- **PiStorm/Emu68:** use the **MintVID040** build. This is the release build
  targeted for the Emu68/PiStorm environment. H.264 becomes much more practical;
  on a tested Pi3-based PiStorm 600, low-resolution H.264 streams below roughly
  200p have played well. Faster PiStorm hardware should provide more headroom,
  but results still depend on the source and configuration.
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
| YouTube search | ✅ no-key ReAction and OS 3.0 GadTools browsers; All/Videos/Live/Shorts filters and native playback handoff |
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
| Amiga RTG / AGA output | ✅ |
| ReAction + GadTools controllers | ✅ matching file, IPTV and YouTube frontends for modern and OS 3.0 systems |
| IPTV directory core | ✅ bounded iptv-org JSON/M3U parsing, joining and local filters |
| PCM / MP2 / MP3 / AAC-LC / AC-3 audio to Paula | ✅ host-validated; AC-3 uses fixed-point stereo downmix |

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
make check-audio # MP3, AAC ADTS/LATM and fixed-point AC-3 decoder checks
make check-http # local HTTP range/redirect integration tests
make check-https # the same tests over TLS (needs OpenSSL development files)
```

Inspect or dump any AVI/MOV/MP4/MKV/TS/M2TS:

```sh
./mr_decode file.avi                 # stream info + frame count
./mr_decode file.avi --ppm outdir    # write decoded frames as PPM
```

`mrplay` streams AVI, MOV/MP4, Matroska/MKV and MPEG-TS/M2TS packets from disk or a direct
`http://`/`https://` file URL. Its RAM use is therefore set by container
metadata, the largest compressed packet, a 256 KiB network rewind cache, and
the active decoder/display buffers rather than by the media file size. HTTP
redirects and byte-range seeking are supported:

```sh
mrplay "http://example.net/video.avi"
mrplay "https://example.net/video.mp4"
mrplay --user-agent "Mozilla/5.0" --referer "https://example.net/" \
  "https://example.net/live/master.m3u8"
mrplay --hls-max-width=640 --hls-max-height=360 \
  "https://www.youtube.com/watch?v=LIVE_STREAM_ID"
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

### Live streaming resilience

Live HLS (`.m3u8`) playback on constrained hardware has a few extra controls.
The AmiSSL library, TLS context, and TLS session are initialised once and reused
across segments, so each segment boundary reconnects with an abbreviated
handshake instead of the full per-segment bring-up. Since 1.1.1, compressed
lookahead intentionally hints only the next segment: this is the stable 1.0.0
scheduling policy, combined with the newer no-abandon worker shutdown and
AmiSSL lifecycle hardening.

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
**Videos**, and **Shorts**. Build with `SSL=1` and keep `ytgui` beside `MintVID`
and `mrplay`. As with watch-page
resolution, this deliberately small parser may need maintenance if YouTube
changes its private page schema.

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
movie and select **AGA**, **HAM6**, **HAM8**, or **CGX**. **Laced** and **2x**
apply to the chipset modes, including HAM6 and HAM8. A laced screen is opened
when the source height after the requested 2x scale exceeds the non-laced
256-line canvas. Exact 2x eight-plane output, including HAM8, uses the fused
Kalms 2x2 converter; other sizes are fitted while preserving aspect ratio.
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

**H.264 performance modes**

TurboGT remains the default. The choices trade picture quality and/or decoded
frames for throughput:

| Mode | Decoder policy | When to use it |
|------|----------------|----------------|
| **Auto** | Resolves to TurboGT. | Keep the release default. |
| **Quality** | Full filtering; no deliberate frame skipping. | Quality comparisons or very fast systems. |
| **Balanced** | In-loop deblocking disabled; motion compensation stays spec-exact. | Mild quality/performance trade-off. |
| **Fast** | Balanced plus bilinear rather than six-tap interpolation; keeps every frame. | Prefer this when avoiding deliberate frame skips matters more than maximum speed. |
| **Turbo** | Fast policy plus B-frame skipping. | Extra speed while preserving the P-frame reference chain. |
| **Turbo+** | Skips both P- and B-frames. | Last-resort keyframe/slideshow mode; not recommended for normal viewing. |
| **TurboGT** | Same policy as Turbo. | Kept as a selectable name; see below. |

Turbo and TurboGT are now the same setting. TurboGT used to differ by
disabling deblocking on keyframes too, and every mode from Balanced down now
does that unconditionally: leaving some pictures undegraded made the decoder
filter the remaining ones against stale per-macroblock deblocking parameters,
which was both wrong and slow enough that Fast ran *slower* than Quality. The
one candidate replacement lever for TurboGT - truncating motion vectors to
whole samples - was implemented and measured at 3-4% for a 17 dB PSNR loss, so
it is not shipped. Every mode below Quality is markedly faster than in 1.2.0;
measured on a 320x180 CABAC stream, Fast by 53%, Turbo by 49%, Balanced by 30%
and TurboGT by 15%, with Quality itself 8% faster at bit-identical output.

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

Every GUI has a **MintVID > About MintVID...** menu containing the project
credits and support link. **MintVID > Quit** closes that frontend cleanly.

The controller's file gadget identifies the selected file. On launch, `mrplay`
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

IPTV playback inherits a snapshot of the ReAction controller's display, C2P,
lace, and 2x selections when **IPTV...** is pressed. The IPTV window shows that
snapshot beside its status; close and reopen it after changing controller
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

H.263 baseline video in AVI and QuickTime MOV is supported for QCIF and CIF,
including intra/inter pictures, skipped macroblocks, half-pixel motion
compensation and persistent reference frames. The decoder rejects malformed or
truncated syntax and refuses H.263+ tools rather than producing corrupt output.
H.263+ is therefore **partial**: UMV, SAC, advanced prediction, PB/improved-PB,
deblocking, slice structure, reference-picture selection, independent segments,
alternative inter VLC, modified quantisation, data partitioning, custom clock
frequency and scalability remain explicitly unsupported. H.261 is not yet
supported. WMV1 and WMV2 (Windows Media Video 7/8) are supported - see below;
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
| `H263`, `h263`, `I263`, `i263` | H.263 | `H.263 baseline` | accepted for baseline QCIF/CIF | registry; `h263.mov` conformance pending |
| `U263`, `u263`, `T263`, `X263` | vendor H.263 / frequently H.263+ | `H.263 baseline` | registered, but annex flags are rejected | registry; upstream sample inspection pending |
| `WMV1`, `wmv1` | Windows Media Video 7 | `wmv1` | accepted | `test_wmv1.avi`, `test_wmv1_q20.avi` |
| `WMV2`, `wmv2` | Windows Media Video 8 | `wmv2` | accepted; IntraX8 ("J-frame") mode rejected | `test_wmv2.avi`, `test_wmv2_q20.avi` |
