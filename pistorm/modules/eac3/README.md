# berry_eac3.module – E-AC3 decoder for BerryCore CPU1

Builds the E-AC3 module fully from source. This includes `core_eac302b.o`, which earlier releases contained only as a prebuilt object.

    sh build.sh [output-dir]

Needs clang, ld.lld and llvm-objcopy (LLVM 17 or 18), plus python3, tar and patch.

## Steps
1. FFmpeg 9.0.2 (`ffmpeg-9.0.2.tar.xz`, LGPL-2.1+) with `patches/0001–0005`, `ffconfig/` and `fakeinclude/`.
   - 16 units are compiled and combined into `core_eac302b.o` (`ld.lld -r`).
2. `glue/` – module header, CPU1 entry, heap/runtime and the TX shim.
   - These are linked with the core into the base module, which may be at most 0x1A000 bytes.
3. `fast/` – FASTIMDCT03, an integer Q31 IMDCT for 128/256 points.
   - It is appended at 0x1A000.
   - The first instruction of FFmpeg's naive IMDCT (always at module offset 0x70) is replaced by a branch to it.

The result is exactly 0x1C3CC bytes, the size that berryvp.library P32 expects.

## Patches
| Patch | Purpose |
|---|---|
| 0001 ac3dec | Berry entry points, staged init (EAC302B), fixed KBD window table |
| 0002 ac3dec_fixed | declarations and KBD table; no FFCodec object in the standalone build |
| 0003 ac3_parser | no parser object (it would pull in the AVParser framework) |
| 0004 tx | codelet list: int32 only |
| 0005 tx_template | tables and codelets only up to 256 points |

Patches 0003–0005 were already contained in the historical `core_eac302b.o`, but their source was never kept. They were recovered from that object file.

## Configuration
`ffconfig/config.h` comes from FFmpeg's configure and was then adjusted for this target:
- `CONFIG_SMALL=1`
- big-endian (`HAVE_BIGENDIAN`, `AV_HAVE_BIGENDIAN`)
- no assembler optimisations
- no libm (`HAVE_<func>=0`, so `libavutil/libm.h` supplies these functions)
- no OS headers

## Comparison with the historical object
- same 16 source units
- same functions and the same global symbols
- 82 of 138 functions have identical machine code
- The remaining functions differ only because the historical build used clang 17 (swiftlang build). With clang 18, `-fomit-frame-pointer` is needed to get the same code.
