# Licences of the PiStorm parts

| Part | Licence | Upstream / notes |
|---|---|---|
| MintVID GUI changes (`player/amiga/mrgui*.c`, `player/core/mr_play_options.*`) | MIT (as MintVID, see `../LICENSE`) | MintVID © Darren "boingball" Banfi |
| mrberry (`mrberry/`) | MIT | uses MintVID core (MIT) |
| berryvp.library (`berryvp_library/`) | MIT | 68k assembler |
| build tools (`tools/`, `berryvp_library/tools/`) | MIT | |
| berry_ac3.module (`modules/ac3/`) | GPL-2.0-or-later | contains liba52 / a52dec 0.7.x © Aaron Holtzman, Michel Lespinasse (`modules/ac3/liba52/`) |
| berry_faad03.module (`modules/aac_faad03/`) | GPL-2.0-or-later | contains FAAD2 2.11.2 © M. Bakker / Nero AG (`faad2_2.11.2.orig.tar.gz`, `COPYING_FAAD2`) |
| berry_eac3.module (`modules/eac3/`) | LGPL-2.1-or-later | contains FFmpeg 9.0.2 (`ffmpeg-9.0.2.tar.xz`, `COPYING.LGPLv2.1`); the patches in `patches/` are under the same licence |
| Emu68 BerryCore patch (`emu68_berrycore/`) | MPL-2.0 | modification of Emu68 © Michal Schulz, https://github.com/michalsc/Emu68 (v1.0.7); `upstream/Emu68-pistorm` is the unmodified Emu68 1.0.7 PiStorm kernel |

The CPU1 modules are separate programs: they are loaded at run time by berryvp.library and run on another CPU core. Their GPL/LGPL licences therefore apply to the modules themselves, not to the 68k programs that load them.

Whoever distributes the module binaries must also make the corresponding source available. This directory contains that source.

The Picasso96 headers used by mrberry come from upstream MintVID (`player/amiga/include`). This fork does not add any copy of them.
