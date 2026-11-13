# Where the PiStorm sources come from

The PiStorm parts of MintVID were developed between September and October 2026, in many small hardware-test stages. Each stage was a ZIP with a binary and usually its source.

For a few stages only the binary was kept. In other cases the source shipped with a binary was a byte image (`dc.b` lines dumped from the binary) rather than real code. For this repository the missing sources were rebuilt. Every rebuilt step was checked byte for byte against the binary of that stage.

## MintVID GUI
- **Source:** the MintVID PiStorm06 source tree (four changed files on top of upstream 1.4.0, commit b7fbf3c).
- **Check:** builds byte-identical to the P32 `MintVID` binary with `make -f Makefile.amiga MintVID`.

## mrberry
- **Source:** the P32 source package.
- **Check:** builds byte-identical to the P32 `mrberry` (`DEBUG_LOGS=1`, gcc 16.2).

## berryvp.library
The git history of `berryvp_library/berryvp.library.s` shows every step. Each commit builds byte-identical to the binary named in its `EXPECTED_SHA256`.

| Stage | Source | SHA-256 of the binary |
|---|---|---|
| N79 | original source (last stage with real source) | 88110f98… |
| N79H | rebuilt from binary | bdda4847… |
| N79I | rebuilt | ff53f9e1… |
| N79J | rebuilt | e355460c… |
| N79K | rebuilt | 44431eb6… |
| N79L | rebuilt | 2e27b84c… |
| N79M | rebuilt | 53f3fb8b… |
| N79N | rebuilt | 00e4a1d3… |
| N79O | rebuilt | 423e088b… |
| N79P | rebuilt | 48e89288… |
| N79Q | rebuilt (= the hardware-tested N79Q backup) | 14103c4a… |
| P32 | N79Q plus the P32 changes as real code | 79ad7494… |

The test packages N79AA … N79P96G were only intermediate steps on the way to N79H and are not recorded separately.

The historical P32 binary was assembled from a byte image, so its relocation tables are sorted by address. vasm writes the relocations of real code in operand order. `tools/hunk_sort_relocs.py` restores the historical order and makes the file identical. The loaded program is the same either way.

## VPberry (P35)
- **Source:** `VPr8_P48_AC3` from the MintVID PiStorm35 package, real assembler of the same line as berryvp.library.
- **Check:** the version with branch sizes normalised to `.l` assembles with vasm (`-m68030`) byte-identical to the released VPberry.

## MintVID GUI (P35)
- **Source:** `mrgui.c` and `mr_play_options.h` from the PiStorm35 package (new display mode "PiStorm / VC Hard Fenster").
- **Check:** builds byte-identical to the P35 `MintVID`.

## CPU1 modules
- **berry_ac3** (liba52) and **berry_faad03** (FAAD2): the sources were complete.
- **berry_eac3**: shipped with the prebuilt object `core_eac302b.o` and no build recipe.
  - The recipe (FFmpeg 9.0.2, which units, configuration, flags) was recovered from the object's symbol and FILE records.
  - Three source changes that only existed inside the object were recovered as `patches/0003–0005`.
- **Comparison:** all three were built with clang 17 (a swiftlang build). With clang 18 the code differs slightly; `-fomit-frame-pointer` reproduces clang 17's frame handling.
- **Size:** berryvp.library P32 checks each module's exact size. `tools/fit_module.py` pads a smaller rebuild with zeros at the end.

## Emu68 BerryCore kernel
- **What was kept:** only the patched kernel and a byte-level patch script.
- **What was rebuilt:** the five patched areas were disassembled and written down as commented AArch64 assembler (`berrycore_patch.S`).
- **Check:** assembling it and writing it into the original Emu68 1.0.7 kernel gives the shipped BerryCore kernel byte for byte.
