# Emu68 1.0.7 BerryCore kernel

> [!IMPORTANT]
> MintVID-PiStorm needs this patched kernel for HDMI sound (Paula sound works
> with the original kernel). It is made for the
> Emu68 1.0.7 PiStorm kernel only, as used on PiStorm Classic with a
> Raspberry Pi 3A+. The patch writes into fixed file offsets, so it cannot be
> applied to any other Emu68 version or build. `build_berrycore.py` refuses
> any other input file.

`berrycore_patch.S` is the complete, commented source of the BerryCore change to Emu68 1.0.7 (PiStorm build, git 305f686). On CPU1, Emu68 normally runs only `serial_writer()` (boot argument `async_log`). BerryCore uses CPU1 for a native AArch64 service instead. That service loads the CPU1 decoder modules (AAC, AC3, E-AC3) and calls them.

    python3 build_berrycore.py Emu68-pistorm-BerryCore

The script assembles the patch with clang and writes the five patched areas into the original kernel `upstream/Emu68-pistorm`:

| Area | What it does |
|---|---|
| version string | two copies of the `$VER` text |
| $654 | CPU1 skips the `async_log` boot-argument check |
| $8A00 | former `serial_writer()`, now a branch to the service (plus an old, unreachable test service) |
| $20F500 | BerryCore service in a previously zero-filled area |

The script checks the input (sha256 `3126f461…`) and the output (sha256 `9b2ef4de…`). The output is identical to the BerryCore kernel shipped with P32.

How the mailbox and the module interface work is explained in the comments at the top of `berrycore_patch.S`.

Emu68 © Michal Schulz, MPL-2.0. Upstream source: https://github.com/michalsc/Emu68 (tag v1.0.7). This patch is a modification of Emu68 and is therefore also MPL-2.0. `upstream/Emu68-pistorm` is the unmodified PiStorm kernel of Emu68 1.0.7.
