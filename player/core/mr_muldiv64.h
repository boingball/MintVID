/* mr_muldiv64.h - exact (value * mul) / div for a growing 64-bit counter,
 * without ever letting the compiler emit a 64-bit-wide multiply or divide.
 *
 * Why this exists: amiga/audio_paula.c's audio clock is exactly the shape
 * `cumulative_samples * 1000000ULL / output_rate` - a counter that grows
 * for the whole playback session (so it needs real 64-bit range) divided by
 * a value only known at runtime (the stream's output rate). Confirmed with
 * m68k-linux-gnu-gcc -S: on m68k this always lowers to libgcc calls -
 * jsr __muldi3 for the multiply (on 68060 only; 68020-68040 get a single
 * hardware muls.l) and jsr __udivdi3/__umoddi3 for the divide on *every*
 * m68k tier, 68020 included - GCC never emits a hardware wide-divide
 * instruction for plain C 64-bit division syntax, regardless of CPU. That
 * makes this pattern expensive everywhere the divide is concerned, not just
 * on 68060, and it runs on essentially every displayed video frame while
 * audio is active (see mrplay.c's per-frame audio_elapsed_us() call) - a
 * much hotter and more consequential site than the MP2 decode math in
 * pl_mpeg.h (see CLAUDE.md's MPEG-1/2 notes).
 *
 * mr_u64_mul_u32(): 64x32->64 truncating unsigned multiply. On 68020-68040
 * the plain C `(uint64_t)a * b` already compiles to one hardware muls.l, so
 * this only special-cases 68060, using an unsigned 32x32->64 widen (built
 * from mulu.w 16x16->32 partial products, hardware everywhere including the
 * 060 - no sign handling needed at all here, unlike plm_audio_smul64_060 in
 * pl_mpeg.h, since every input is already an unsigned magnitude: there is
 * no "negative" bit pattern to detect or correct for).
 *
 * mr_u64_div_u16(): divides a 64-bit dividend by a divisor that fits in 16
 * bits - true of every real Paula output rate (Paula's colour clock is a
 * few MHz and its minimum period clamps the rate well under 65536, see
 * MIN_PERIOD in audio_paula.c) - using ONLY the ordinary 32-bit/32-bit
 * hardware divide (the plain 32-bit-dividend DIVU.L/DIVS.L, a 68020-and-up
 * ISA addition - not in base 68000/010, which only has the 16-bit
 * DIVU.W/DIVS.W, but MintVID's baseline is 68030 regardless; hardware
 * everywhere from there up including 68060. Only the *extended*
 * 64-bit-dividend DIVU.L/DIVS.L form is the trap-prone/libgcc-routed one).
 * It does this via
 * base-2^16 schoolbook long division: the running remainder is always
 * smaller than a 16-bit divisor, so appending the next 16-bit digit of the
 * dividend always keeps the trial value inside 32 bits, safe for a plain
 * hardware divide at every step. No magic reciprocal constants, no wide
 * multiply, portable C - identical on host and target, so what
 * tests/mr_muldiv64_check.c verifies here is exactly what runs in
 * audio_paula.c.
 */
#ifndef MR_MULDIV64_H
#define MR_MULDIV64_H

#include <stdint.h>
#if defined(MR_M68K_ASM)
#include "mr_cpu.h"    /* MR_CPU_68060 - see mr_cpu.h for why this matters */
#endif

#if defined(MR_M68K_ASM) && defined(MR_CPU_68060)
/* Unsigned 32x32->64 widen: d0:d1 = a * b, via the four-partial-product
 * schoolbook multiply (mulu.w only - hardware on every m68k, 68060
 * included). No sign step: both operands are plain unsigned magnitudes. */
static inline uint64_t mr_u32_mul_u32_wide(uint32_t a, uint32_t b)
{
    uint32_t hi, lo;
    __asm__ __volatile__(
        "move.l %2,%%d0\n\t"            /* d0 = a */
        "move.l %3,%%d1\n\t"            /* d1 = b */
        "move.l %%d0,%%d3\n\t"
        "swap   %%d3\n\t"
        "and.l  #0xffff,%%d3\n\t"       /* d3 = ah */
        "and.l  #0xffff,%%d0\n\t"       /* d0 = al */
        "move.l %%d1,%%d4\n\t"
        "swap   %%d4\n\t"
        "and.l  #0xffff,%%d4\n\t"       /* d4 = bh */
        "and.l  #0xffff,%%d1\n\t"       /* d1 = bl */
        "move.l %%d0,%%d5\n\t"
        "mulu.w %%d1,%%d5\n\t"          /* d5 = al*bl */
        "move.l %%d0,%%d6\n\t"
        "mulu.w %%d4,%%d6\n\t"          /* d6 = al*bh */
        "move.l %%d3,%%d7\n\t"
        "mulu.w %%d1,%%d7\n\t"          /* d7 = ah*bl */
        "mulu.w %%d4,%%d3\n\t"          /* d3 = ah*bh */
        "move.l %%d5,%%d1\n\t"
        "moveq  #0,%%d0\n\t"
        "move.l %%d6,%%d5\n\t"
        "swap   %%d5\n\t"
        "and.l  #0xffff0000,%%d5\n\t"   /* d5 = (al*bh<<16) mod 2^32 */
        "add.l  %%d5,%%d1\n\t"
        "moveq  #0,%%d5\n\t"
        "addx.l %%d5,%%d0\n\t"
        "swap   %%d6\n\t"
        "and.l  #0xffff,%%d6\n\t"       /* d6 = (al*bh)>>>16 */
        "add.l  %%d6,%%d0\n\t"
        "move.l %%d7,%%d5\n\t"
        "swap   %%d5\n\t"
        "and.l  #0xffff0000,%%d5\n\t"   /* d5 = (ah*bl<<16) mod 2^32 */
        "add.l  %%d5,%%d1\n\t"
        "moveq  #0,%%d5\n\t"
        "addx.l %%d5,%%d0\n\t"
        "swap   %%d7\n\t"
        "and.l  #0xffff,%%d7\n\t"       /* d7 = (ah*bl)>>>16 */
        "add.l  %%d7,%%d0\n\t"
        "add.l  %%d3,%%d0\n\t"          /* d0 += ah*bh (bit offset 32) */
        "move.l %%d0,%0\n\t"
        "move.l %%d1,%1\n\t"
        : "=a"(hi), "=a"(lo)
        : "a"(a), "a"(b)
        : "d0", "d1", "d3", "d4", "d5", "d6", "d7", "cc");
    return ((uint64_t)hi << 32) | (uint64_t)lo;
}

static inline uint64_t mr_u64_mul_u32(uint64_t a, uint32_t b)
{
    uint32_t a_hi = (uint32_t)(a >> 32);
    uint32_t a_lo = (uint32_t)a;
    /* a*b truncated to 64 bits: a_hi*b lands at bit offset 32, so only its
     * low 32 bits survive; a_lo*b needs its full 64-bit widen. */
    uint32_t hi_contribution = (uint32_t)mr_u32_mul_u32_wide(a_hi, b);
    return mr_u32_mul_u32_wide(a_lo, b) + ((uint64_t)hi_contribution << 32);
}
#else
static inline uint64_t mr_u64_mul_u32(uint64_t a, uint32_t b)
{
    return a * (uint64_t)b;
}
#endif

/* divisor must be > 0 and fit in 16 bits (< 65536) - true of every real
 * Paula output rate. Undefined (well, wrong) otherwise; there is no
 * fallback here because every caller in this codebase only ever passes a
 * Paula output rate. */
static inline uint64_t mr_u64_div_u16(uint64_t n, uint32_t divisor)
{
    uint32_t d3 = (uint32_t)(n >> 48) & 0xffffu;
    uint32_t d2 = (uint32_t)(n >> 32) & 0xffffu;
    uint32_t d1 = (uint32_t)(n >> 16) & 0xffffu;
    uint32_t d0 = (uint32_t)n & 0xffffu;
    uint32_t rem = 0, q3, q2, q1, q0;
    uint32_t trial;

    trial = (rem << 16) | d3; q3 = trial / divisor; rem = trial % divisor;
    trial = (rem << 16) | d2; q2 = trial / divisor; rem = trial % divisor;
    trial = (rem << 16) | d1; q1 = trial / divisor; rem = trial % divisor;
    trial = (rem << 16) | d0; q0 = trial / divisor;

    return ((uint64_t)q3 << 48) | ((uint64_t)q2 << 32) |
           ((uint64_t)q1 << 16) | (uint64_t)q0;
}

/* Same idea as mr_u64_div_u16, but for a divisor that only fits in 24 bits
 * (< 16777216) - amiga/audio_paula.c's other runtime-but-session-invariant
 * divisor, ReadEClock's tick frequency (a few hundred kHz on real Amiga
 * hardware, comfortably clear of 2^24). Eight base-256 digits instead of
 * four base-65536 digits: the running remainder stays below the divisor
 * (< 2^24), so appending the next 8-bit digit keeps every trial value
 * inside 32 bits, safe for the same ordinary hardware divide. */
static inline uint64_t mr_u64_div_u24(uint64_t n, uint32_t divisor)
{
    uint32_t rem = 0;
    uint64_t q = 0;
    for (int shift = 56; shift >= 0; shift -= 8) {
        uint32_t digit = (uint32_t)(n >> shift) & 0xffu;
        uint32_t trial = (rem << 8) | digit;
        q = (q << 8) | (trial / divisor);
        rem = trial % divisor;
    }
    return q;
}

#endif /* MR_MULDIV64_H */
