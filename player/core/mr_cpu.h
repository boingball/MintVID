/* mr_cpu.h - hardened m68k CPU-tier detection.
 *
 * GCC forks/versions have used more than one spelling for the 68060
 * predefine over the years - this m68k-linux-gnu-gcc happens to define all
 * three (__mc68060__, __mc68060, mc68060) for -mcpu=68060, but
 * vendor/MintAMP's own real-hardware-tested code defensively checks all
 * three too (see e.g. vendor/MintAMP/real/assembly.h), which means it's a
 * real cross-toolchain concern, not a hypothetical one - a different
 * compiler generation (Bebbo's Amiga-gcc included) is not guaranteed to
 * define the same set.
 *
 * Getting this test wrong is not symmetric. A false positive (treating a
 * non-68060 as a 68060) just takes the slower, portable-multiply fallback
 * path - annoying, not wrong. A false negative sends a real 68060 down the
 * `!MR_CPU_68060` branch, which is exactly the branch that uses the
 * trap-prone extended muls.l/divsl.l instructions this whole family of
 * fixes exists to keep off that CPU. So: union every spelling into one
 * macro, and check only that macro everywhere in this codebase (pl_mpeg.h,
 * mr_muldiv64.h, and their tests) - one place to extend if a future
 * toolchain needs a fourth spelling, and no call site can drift out of
 * sync with the others.
 */
#ifndef MR_CPU_H
#define MR_CPU_H

#if defined(__mc68060__) || defined(__mc68060) || defined(mc68060)
#define MR_CPU_68060 1
#endif

#endif /* MR_CPU_H */
