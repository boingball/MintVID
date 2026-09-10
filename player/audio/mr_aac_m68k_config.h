/*
 * MintVID's Amiga build compiles C and preprocessed assembly sources in one
 * GCC driver invocation. A raw command-line -include of MintAMP's C helper
 * would consequently feed its inline C functions to the assembler as well.
 * Keep the forced include assembly-safe and expose the helper only to C
 * translation units.
 */
#ifndef MR_AAC_M68K_CONFIG_H
#define MR_AAC_M68K_CONFIG_H

#ifndef __ASSEMBLER__
#include "aac_m68k_aac_optimized.h"

#define AMIGA_M68K_ASM_AAC_HUFFMAN 1
#define AMIGA_M68K_ASM_AAC_DEQUANT 1
#define AMIGA_M68K_ASM_AAC_STEREO 1
#define AMIGA_M68K_ASM_AAC_IMDCT 1
/* PNS's InvRootR/ScaleNoiseVector and TNS's DecodeLPCCoefs/FilterRegion
 * (pns.c/tns.c) previously had no AMIGA_M68K_ASM_AAC_* coverage at all -
 * every other AAC file's 32x32->64 multiply already routed through
 * AAC_M68K_MULSHIFT32/AAC_M68K_MADD64, but pns.c never included
 * amiga_m68k_aac.h to override MULSHIFT32, and tns.c hand-inlined its own
 * (long long)x*y instead of using the macro at all. Confirmed via 68060
 * disassembly to reference __muldi3 on every PNS/TNS-active frame - both
 * are standard AAC-LC tools, not corner cases. Fixed upstream in
 * decoders/esp8266audio (see vendor/MintAMP's submodule pointer).
 */
#define AMIGA_M68K_ASM_AAC_PNS 1
#define AMIGA_M68K_ASM_AAC_TNS 1
#endif

#endif
