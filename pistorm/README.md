# MintVID for PiStorm – BerryVP / BerryCore (v97.1)

Hardware-accelerated playback for MintVID on a **PiStorm with Emu68** (PiStorm Classic with Raspberry Pi 3A+ / BCM2837, and since v96.97 PiStorm32 lite with Compute Module 4 / BCM2711):

- **H.264 up to 1080p**, decoded by the VideoCore of the Raspberry Pi (MMAL/VCHIQ).
  - Output as a hard full-screen picture on the Pi's HDMI output, or in a P96 window on the Workbench (both mrberry).
- **AAC, AC3 and E-AC3 audio**, decoded on the second ARM core (CPU1) of the Pi.
  - Output as PCM over the Pi's HDMI audio.
- **Paula sound** in full screen (mrberry `PAULA`, Paula player by Darren 'boingball' Banfi): AAC, MP3, MP2, AC3 and PCM decoded on the Amiga, synchronised to the Pi's video clock.
  - Pause with Paula: mrberry stops at the picture on screen and writes the spot to `film.t`; MintVID-pi shows it as a still picture (mrberry `W=50 STILL`: window without sound) and continues in full screen with Paula.

> [!IMPORTANT]
> **HDMI sound needs the patched Emu68 kernel `Emu68-pistorm-BerryCore`.** The
> unmodified Emu68 kernel has no BerryCore service on CPU1, so the AAC/AC3/E-AC3
> decoders for HDMI cannot run. Video with Paula sound (`PAULA`) also works with
> the original Emu68 1.0.7 kernel (tested on hardware, 23.10.2026).
>
> **Supported: PiStorm Classic with Raspberry Pi 3A+ (`Emu68-pistorm-BerryCore`)
> and PiStorm32 lite with CM4 (`Emu68-pistorm32lite-BerryCore`), both Emu68 1.0.7.**
> Each patch is made for exactly one kernel file. Other PiStorm boards and newer
> Emu68 versions are not supported yet. v96.97 was tested on the PiStorm32 lite
> (A1200, CM4 1 GB); v97.1 is also tested on the PiStorm Classic (A600 / PiStorm600 / Pi 3A+, by Coachman).
>
> **`config.txt` on the SD card must contain `gpu_mem=64` or more** (CM4:
> `gpu_mem=128` or more, tested with 128 and 256). The VideoCore needs this memory for hardware decoding.

This directory holds the complete source of all PiStorm parts of the v97.1 release (v96.98 = v96.97 with "3A+ / CM4" in the About text of MintVID-pi; v96.99 = v96.98 with the CM4 attach fix in berryvp.library; v97.1 = v96.99 with enough stack for mrberry from the Shell; v97.0 was not released). The MintVID-pi GUI changes themselves live in `player/` (see the commit history of this fork). VPberry (`vpberry/`) is kept as source only; since v96.76 the window mode is played by mrberry.

> Deutsch: siehe [unten](#deutsch).

**Picture path (berryvp.library N83/N84):** in full screen a film of 1280x720 or
larger goes from the decoder straight to the display hardware (HVS), or through
the ISP if its coded width is not a multiple of 64. A film smaller than 1280x720
goes through the ISP, which upscales it to the full 1080 lines: 16:9 and up to
2 % wider exactly to 1920x1080, narrower films (4:3) to 1080 lines with the
width rounded to 128. The renderer fills the screen, and every enlargement in
the HVS draws green lines (N83 test 27.10.: lines at 1.33-2.4x; N84 28.10.:
720x404 upscaled to 1920x1076 and then stretched 1076 -> 1080 lines gave four
green stripes). Films more than 2 % wider than 16:9 (cinema format) are not
upscaled yet (old N79J path). mrberry's test switches: `ISP` = every
full-screen film through the ISP, `ISPOUT=OFF` = no upscale, `ISPOUT=WxH` =
fixed ISP output size.

**v97.1 (08.11.2026):** from the Shell mrberry used to get only the Shell's stack (often 4096 bytes) and crashed on MKV/MP4 films. When its stack is smaller than 300000 bytes, mrberry now loads itself once more and runs that copy with 320000 bytes of stack through dos.library `RunCommand()` (the same as `Stack 320000` before the command). MintVID-pi already started it with 320000 bytes, so nothing changes there. A missing file only prints `Error: file not found`. (v97.0 used the libnix stack swap instead; that crashed when mrberry ended and was not released.) Thanks to Coachman (EAB) for the test. Tested on the A1200 / CM4.

**v96.99 (06.11.2026):** when VCHIQ was already running (MintVID-pi quit and started again after the library was reloaded, or another program such as the FFmpeg test tool `ami264test` started VCHIQ first), berryvp.library skipped the board query and fell back to the Pi 3 layout; on the CM4 the start stopped after `N79 OUT PRE`. The attach path now runs the same board detection. Tested on the A1200 / CM4.

**Pi 4 / CM4 (BCM2711, v96.89-v96.97):** berryvp.library reads the board
revision from the firmware (mailbox tag `$00010002`, SoC 3 = BCM2711). On the
BCM2711 it
- writes the VCHIQ pagelists in the 36-bit form the Pi 4 firmware expects
  (`addrs[k] = ((phys >> 4) & ~0xFF) | (pages - 1)`, at most 256 pages per entry,
  as Linux `vchiq_arm.c` does with `use_36bit_addrs`); with the Pi 3 form the
  VideoCore read outside the RAM and the decoder never got data,
- reads the Workbench plane from the HVS5 display list at `$F2404000` in the
  VideoCore.card VC6 layout,
- asks the firmware for the screen size (tag `$00040003`) instead of reading
  DISPCTRL1,
- tells video_render to use display 2 (HDMI0 on the Pi 4).

The Pi 3 code paths are unchanged.

## How it fits together

```
MintVID-pi (GUI, 68k) --starts--> mrberry (68k) --uses--> berryvp.library (68k)
                                    |                       |
                          demux MKV/MP4/TS          VCHIQ/MMAL: H.264 -> VideoCore -> HDMI
                                                            |
                                         BerryCore mailbox (Fast-RAM)
                                                            v
                         Emu68 BerryCore kernel, CPU1 service (AArch64)
                           loads berry_faad03 / berry_ac3 / berry_eac3.module
```

| Part | Directory | Language | Rebuild |
|---|---|---|---|
| MintVID-pi GUI | `../player/amiga/mrgui*.c`, `mr_gui_menu.c` | C (68k) | byte-identical to v97.1 |
| mrberry | `mrberry/` | C (68k) | byte-identical to v97.1 (v97.1: enough stack also from the Shell) |
| VPberry (VPr8 P48, old window player, not in the release since v96.76) | `vpberry/` | 68k assembler (vasm) | byte-identical to P35 |
| berryvp.library | `berryvp_library/` | 68k assembler (vasm) | byte-identical to v97.1 (unchanged since v96.99: board detection also when VCHIQ is already running) |
| berry_ac3.module | `modules/ac3/` | C, liba52 (AArch64 BE) | same size, codegen differs, hardware-tested * |
| berry_faad03.module | `modules/aac_faad03/` | C, FAAD2 2.11.2 (AArch64 BE) | byte-identical to v96.88 / v96.97 (5.1 mixed down to stereo) |
| berry_eac3.module | `modules/eac3/` | C, FFmpeg 9.0.2 (AArch64 BE) | same size, codegen differs, hardware-tested * |
| Emu68 BerryCore kernel (Pi 3) | `emu68_berrycore/` | AArch64 assembler patch | byte-identical to P32 |
| Emu68 BerryCore32 kernel (PiStorm32 lite) | `emu68_berrycore/` (`berrycore32_patch.S`, `README32.md`) | AArch64 assembler patch | byte-identical to v96.97 |

\* The historical CPU1 modules were built with clang 17. With clang 18 the machine code differs slightly. The rebuilt modules (clang 18) were tested on the hardware on 2026-10-14 (PiStorm Classic, Pi 3A+): AAC, AC3 and E-AC3 all play the same as the P32 originals.

## Building

Tools (in `PATH`):

- `m68k-amigaos-gcc` (bebbo toolchain, tested with 16.2): MintVID, mrberry
- `vasmm68k_mot` (vasm 2.0f): berryvp.library
- `clang`, `ld.lld`, `llvm-objcopy`, `llvm-nm`, `llvm-readelf` (LLVM 17 or 18): CPU1 modules, Emu68 kernel
- `python3`, `make`, `tar`, `patch`

```sh
sh pistorm/build_all.sh
```

Everything ends up in `pistorm/out/v97.1/`, and each file is compared against `v97.1_SHA256SUMS` (berry_ac3/berry_eac3 in the release are still the P35 binaries, see * above). Every part also has its own `build.sh`.

## Installation (Amiga side)

1. Copy `MintVID-pi`, `mrberry`, `berryvp.library`, `berry_faad03.module`, `berry_ac3.module` and `berry_eac3.module` into one drawer.
2. **For HDMI sound:** use the BerryCore kernel for your board on the SD card: `Emu68-pistorm-BerryCore` instead of `Emu68-pistorm` (PiStorm Classic / Pi 3A+), or `Emu68-pistorm32lite-BerryCore` instead of `Emu68-pistorm32lite` (PiStorm32 lite / CM4; CaffeineOS has two `kernel=` lines). Both are Emu68 1.0.7. Paula sound works with the original kernels.
3. **Required:** in `config.txt` on the SD card, set `gpu_mem=64` or more (CM4: `gpu_mem=128` or more, tested with 128 and 256).

The modules must have the exact sizes that berryvp.library P32 expects. The build scripts check this.

## Where the sources come from

The PiStorm parts were developed by Michael with AI assistance (ChatGPT) between September and October 2026. For several stages only binaries were kept. Their source was reconstructed later, and every step was checked byte for byte against the binary of that time. See [`docs/SOURCE_RECONSTRUCTION.md`](docs/SOURCE_RECONSTRUCTION.md).

## Licences

See [`LICENSES.md`](LICENSES.md) and [`LICENSE`](LICENSE) (MIT, © Michael / MichiB210). In short:

- MintVID and the PiStorm 68k parts: MIT
- berry_ac3.module (liba52) and berry_faad03.module (FAAD2): GPL-2.0-or-later
- berry_eac3.module (FFmpeg): LGPL-2.1-or-later
- Emu68 BerryCore kernel: MPL-2.0

---

## Deutsch

Hardware-beschleunigte Wiedergabe für MintVID auf einem **PiStorm mit Emu68** – PiStorm Classic mit Raspberry Pi 3A+ und seit v96.97 auch **PiStorm32 lite mit Compute Module 4** (getestet im A1200, CM4 1 GB; v97.1 auch auf dem PiStorm Classic getestet: A600 / PiStorm600 / Pi 3A+, Coachman):

- H.264 bis 1080p über die VideoCore des Raspberry Pi
  - Bild als Vollbild am HDMI-Ausgang des Pi oder im P96-Fenster auf der Workbench (beides mrberry)
- Ton in AAC, AC3 und E-AC3 auf dem zweiten ARM-Kern (CPU1)
  - Ausgabe über HDMI-Audio
- Im Vollbild Ton auch über Paula (Paula-Player von Darren 'boingball' Banfi)

**Untertitel (nur Vollbild):** Text-Untertitel aus der MKV (SRT, ASS/SSA als reiner Text) erscheinen als „<Sprache> … (MKV)“. Oder eine SRT-Datei mit dem Namen des Films daneben legen (`film.srt` oder `film.<Name>.srt`, z. B. `film.de_forced.srt`). Auswahl „Subtitles“ in MintVID-pi; beim ersten Start die forced-Datei in der Sprache der Tonspur, sonst die erste forced, sonst keine; die Wahl wird in der .t gemerkt. Hinweis: Die Untertitel-Schrift hat nur die Latin-1-Zeichen (westeuropäische Sprachen mit ä ö ü ß é ñ …); andere Buchstaben – osteuropäisch (ł č ş ő), Griechisch, Kyrillisch, asiatische Schriften, Arabisch, Hebräisch – erscheinen als „?“. Taste S = nächste Untertitel-Datei während des Films (Off, 1, 2 …; der Name wird 2 s eingeblendet), Taste O = aus/an, +/- = 0,1 s verschieben (HDMI-Ton).

**Tab** = nächstes Kapitel (MKV), **Shift-Tab** = Kapitelanfang bzw. voriges Kapitel; Filme ohne Kapitel springen 10 Minuten vor/zurück. Im Vollbild und im Fenster, wenn der Film aus MintVID-pi gestartet wurde (der Film startet an der neuen Stelle neu).

**Taste M** = Ton aus/an (Vollbild und Fenster). Über HDMI läuft der Ton als Stille weiter (Bild und Ton bleiben synchron), bei Paula geht die Lautstärke auf 0. Filme mit Untertiteln zeigen 2 s „Mute“ / „Sound on“.

**Nicht unterstützter Ton:** MintVID-pi lehnt Filme ab, deren Ton mit der gewählten Ausgabe nicht spielt, und nennt den Grund (z. B. „Not playable: audio format DTS is not supported“ – DTS, Opus, FLAC, Vorbis; MP3/MP2/PCM nur mit Paula; E-AC3 nur über HDMI). Bei mehreren Tonspuren nur, wenn keine passt. Filme ganz ohne Tonspur spielen ohne Ton.

**Kleine Filme** (unter 1280×720) rechnet der Pi seit v96.88 auf die echte Größe des HDMI-Bildschirms hoch (vorher immer 1920×1080 – bei Bildschirmen unter 1080p, z. B. 1280×1024, gab das grüne Linien).

**Im Fenster:** Leertaste = Pause, **F** = weiter im Vollbild an derselben Stelle (wenn aus MintVID-pi gestartet), Esc oder 0 = Stopp.

**Tipps:** Filme am besten von der SD-Karte starten. Auf einem NTFS-Stick Dateien nicht auf dem Amiga umbenennen (der NTFS-Treiber ist dabei abgestürzt), sondern am PC. Empfohlen ist das Vollbild: dort entschlüsselt und zeigt der Pi das Bild selbst. Im P96-Fenster wird jedes Bild zum Amiga kopiert und von ihm gezeichnet; je nach Auflösung, Datei, enthaltenen Tonspuren und Tonformat (z. B. AC3) läuft die Fenster-Wiedergabe dann nicht immer flüssig.

**Pi 4 / CM4:** Die berryvp.library fragt die Firmware, auf welcher Platine sie läuft. Auf dem CM4 schreibt sie die Adressen für die Datenübertragung zur VideoCore im Pi-4-Format („36-Bit-Pagelists“, ohne das gab es kein Bild), liest das Workbench-Bild aus der Pi-4-Bildliste und fragt die Firmware nach der Bildschirmgröße. Auf dem Pi 3 bleibt alles wie vorher. H.265 und 4K gehen nicht: Auch der Pi 4 decodiert H.264 nur bis 1080p, und sein H.265-Baustein ist über die Firmware nicht erreichbar.

**Bauen:** `sh pistorm/build_all.sh`. Benötigt werden m68k-amigaos-gcc, vasm, clang/LLVM 17 oder 18 und python3. Das Ergebnis liegt in `pistorm/out/v97.1/`.

**Ergebnis der Neubauten:**
- exakt gleich (bytegleich) zu v97.1: MintVID-pi, mrberry, berryvp.library, berry_faad03.module und beide Emu68-BerryCore-Kerne
- berry_ac3 und berry_eac3 (im Release weiterhin die P35-Dateien) funktionieren gleich, ihr Maschinencode ist aber etwas anders, weil damals clang 17 statt clang 18 benutzt wurde. Die Neubauten wurden am 14.10.2026 auf der Hardware getestet: AAC, AC3 und E-AC3 laufen genau wie die P32-Originale.

**Wichtig:** Ohne den gepatchten Kern für die eigene Karte gibt es keinen Ton über HDMI: `Emu68-pistorm-BerryCore` (PiStorm Classic / Pi 3A+) bzw. `Emu68-pistorm32lite-BerryCore` (PiStorm32 lite / CM4), beide Emu68 1.0.7. Bild mit Ton über Paula läuft auch mit den Original-Kernen.

**Installation:**
1. Die sechs Dateien `MintVID-pi`, `mrberry`, `berryvp.library` und die drei `.module`-Dateien in eine Schublade kopieren. VPberry wird nicht mehr gebraucht.
2. Den passenden BerryCore-Kern in den Ordner `KERNEL` der SD-Karte legen und in der `config.txt` eintragen (`kernel=kernel/Emu68-pistorm-BerryCore` bzw. `kernel=kernel/Emu68-pistorm32lite-BerryCore`; bei CaffeineOS beide `kernel=`-Zeilen). Den alten Kern aufheben.
3. In der `config.txt` auf der SD-Karte `gpu_mem=64` oder mehr eintragen (CM4: `gpu_mem=128` oder mehr, getestet mit 128 und 256).
