/*
 * m68k-safe divide/modulo pair for a runtime (non-constant) divisor.
 *
 * A handful of libavc H.264 decode sites need BOTH the quotient and the
 * remainder of the same division - most commonly "macroblock x/y from a
 * linear macroblock address", `mb_x = addr % pic_width_in_mbs; mb_y = addr
 * / pic_width_in_mbs;` (ih264d_parse_slice.c, ih264d_thread_compute_bs.c,
 * ih264d_thread_parse_decode.c), each computed once per slice - i.e. at
 * least once per frame for a typical single-slice stream. Disassembling
 * the real -mcpu=68060 object confirms GCC recognises the "compute both
 * quotient and remainder from one dividend" pattern and fuses it into a
 * single extended-dividend DIVSL.L/DIVUL.L <ea>,Dr:Dq - the register-pair
 * form 68060 does not implement in hardware and traps into software
 * emulation for (see tests/scan_m68060_forbidden.py's header comment for
 * why register equality is what distinguishes that form from the
 * ordinary, hardware-everywhere-since-68020 single-quotient one). This
 * happens even when only the *remainder* is wanted (the quotient is
 * simply discarded) - GCC's m68k backend has no "remainder only" pattern
 * that avoids the extended form (confirmed both in isolation and inside
 * real vendored functions - see vendor/libavc_port/ih264d_update_qp_wrap.c
 * for the first instance of this exact shape).
 *
 * Splitting the computation into "quotient first, then dividend minus
 * quotient*divisor" in plain C is NOT sufficient: GCC's divmod-fusion
 * optimisation recognises that reconstruction (`a - (a/b)*b == a%b`) and
 * re-fuses it back into the same forbidden instruction wherever both
 * appear in one function, regardless of how the source expresses it -
 * confirmed directly by compiling the "split" form and disassembling the
 * result. The only reliable way to prevent the fusion is to make the
 * division itself opaque to the compiler: mr_ih264_divmod_u32() computes
 * the quotient with inline asm (a real 32-bit/32-bit hardware DIVU.L,
 * genuinely safe on every 68020+ tier including 68060 - the *result*
 * register is deliberately the same as the *remainder-would-be* slot, per
 * the register-equality rule above), so GCC has no C-level division
 * expression left to recognise; the remainder falls out of an ordinary,
 * unrelated-looking multiply and subtract on that opaque value.
 *
 * Only 68060 needs this: 68020-68040 execute the extended-dividend form
 * in hardware, so they keep the plain C `%`/`/` GCC already compiles
 * efficiently there (and host/non-m68k builds are unaffected either way).
 * Bit-exact with the portable `dividend % divisor` / `dividend / divisor`
 * for any divisor > 0 (unsigned division/modulo is defined the same way
 * regardless of instruction sequence) - verified by
 * tests/mr_ih264_divmod_check.c and, for the actual MB x/y call shape
 * specifically, a real-m68k differential test under qemu.
 */
#ifndef MR_IH264_M68K_DIVMOD_H
#define MR_IH264_M68K_DIVMOD_H

#include "ih264_typedefs.h"

#if defined(__GNUC__) && (defined(__mc68060__) || defined(mc68060))

static __inline UWORD32 mr_ih264_divmod_u32(UWORD32 dividend, UWORD32 divisor,
                                             UWORD32 *rem)
{
    UWORD32 q = dividend;
    __asm__ ("divu.l %1,%0" : "+d" (q) : "d" (divisor));
    *rem = dividend - q * divisor;
    return q;
}

#else

static __inline UWORD32 mr_ih264_divmod_u32(UWORD32 dividend, UWORD32 divisor,
                                             UWORD32 *rem)
{
    *rem = dividend % divisor;
    return dividend / divisor;
}

#endif

#endif /* MR_IH264_M68K_DIVMOD_H */
