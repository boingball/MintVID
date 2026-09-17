/*
 * MintVID adaptation notice: this header is new, added when vendoring
 * libdv's decode-only sources into MintVID (see player/vendor/libdv and
 * THIRD-PARTY-LICENSES.txt - libdv is LGPL-2.1-or-later).
 *
 * libdv's own dv_types.h/bitstream.h detect endianness via autoconf
 * (HAVE_ENDIAN_H/HAVE_MACHINE_ENDIAN_H, set by its configure script) and,
 * failing that, an unconditional `#if (BYTE_ORDER == LITTLE_ENDIAN)` /
 * `#if (BYTE_ORDER == BIG_ENDIAN)`. This tree has no configure step, so
 * those HAVE_* macros are never defined and BYTE_ORDER/LITTLE_ENDIAN/
 * BIG_ENDIAN are all simply undefined - which the preprocessor treats as 0,
 * making every one of those comparisons true and silently selecting
 * little-endian behaviour on every target, including big-endian m68k.
 * bitstream.h's swab32() in particular depends on this for basic bitstream
 * correctness (its BIG_ENDIAN branch is a no-op, relying on a big-endian
 * CPU's native word load already matching the DV bitstream's MSB-first
 * packing) - getting this wrong breaks VLC decode outright, not just a
 * metadata field.
 *
 * Fixed by deriving BYTE_ORDER from the compiler's own __BYTE_ORDER__/
 * __ORDER_BIG_ENDIAN__ builtins instead of a libc <endian.h> - defined by
 * every GCC-family cross compiler this project targets (host gcc,
 * m68k-linux-gnu-gcc, m68k-amigaos-gcc), independent of whether the target
 * libc even has <endian.h> (AmigaOS's clib2/newlix do not).
 */
#ifndef MR_DV_ENDIAN_H
#define MR_DV_ENDIAN_H

#ifndef LITTLE_ENDIAN
#define LITTLE_ENDIAN 1234
#endif
#ifndef BIG_ENDIAN
#define BIG_ENDIAN 4321
#endif
#ifdef BYTE_ORDER
#undef BYTE_ORDER
#endif
#if defined(__BYTE_ORDER__) && defined(__ORDER_BIG_ENDIAN__) && \
    (__BYTE_ORDER__ == __ORDER_BIG_ENDIAN__)
#define BYTE_ORDER BIG_ENDIAN
#else
#define BYTE_ORDER LITTLE_ENDIAN
#endif

/* MintVID adaptation: several one-time table-init functions (weighting.c,
 * idct_248.c, dct.c) use M_PI, which -std=c99 doesn't expose from <math.h>
 * (it's a POSIX/BSD extension, gated behind _XOPEN_SOURCE/_DEFAULT_SOURCE
 * on glibc) - libdv's own build normally supplies it via config.h/autoconf
 * feature-test defines this buildless copy doesn't have. */
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#endif /* MR_DV_ENDIAN_H */
