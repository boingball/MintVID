/*
 * m68k linker-level override for ih264d_update_qp() - see
 * ih264d_cabac_wrap.c's header comment for why this technique (GNU ld
 * --wrap, not a vendored-source edit) is how this port fixes a function
 * that vendor/libavc/ calls directly by name from many sites and that
 * lives inside a pinned upstream git submodule.
 *
 * ih264d_update_qp() (vendor/libavc/decoder/ih264d_utils.c) is called once
 * per macroblock that carries a coded mb_qp_delta - in practice on
 * essentially every non-skip macroblock of every I/P/B slice, so on any
 * real frame this runs hundreds to thousands of times per second. Its
 * first line:
 *
 *     i_temp = (ps_dec->u1_qp + i1_qp + 52) % 52;
 *
 * disassembles, at -mcpu=68060, to an extended-dividend DIVSL.L
 * <ea>,Dr:Dq (objdump: `divsll %d2,%d3,%d1`, remainder and quotient in
 * *different* registers - see tests/scan_m68060_forbidden.py's header
 * comment for why that register-equality distinction is exactly what
 * separates the hardware-since-68020 form from the one 68060 does not
 * implement and traps into software emulation for) - confirmed both in
 * isolation and inside the real vendored function; the quotient is
 * discarded (only the remainder is used) but GCC's m68k backend has no
 * "remainder only" pattern that avoids the extended form. This is real
 * per-macroblock cost on every 68060 build, not a corner case.
 *
 * The rest of the function - the two divisions by 6 for the luma and
 * chroma QP (`ps_dec->u1_qp % 6` / `/ 6`, via the MOD()/DIV() macros) -
 * already compiles to a magic-number reciprocal multiply
 * (`muluw #-21845` + `bfextu` + `mulsw #6` + `subw`, no divide instruction
 * of any kind) on this target and needed no change; only the %52 line is
 * touched here, kept as close to the original as possible.
 *
 * %52's dividend, `ps_dec->u1_qp + i1_qp + 52`, is bounded by the operand
 * types alone regardless of stream validity: u1_qp is UWORD8 (promotes to
 * a non-negative int in [0,255]) and i1_qp is WORD8, i.e. the *raw*,
 * not-yet-range-checked bitstream value (int in [-128,127]) - the range
 * check on i1_qp happens on the line *after* this computation, deliberately
 * preserved below in the same order, so a malformed i1_qp must still land
 * on the same error path via i_temp falling outside [0,51]. That bounds
 * the dividend to exactly [-76, 434], which is replaced below with an
 * explicit add/subtract reduction - never more than one addition (x is
 * always > -104) or a handful of subtractions (x is always < 9*52) - that
 * reproduces C's truncating '%' exactly (sign preserved for negative x,
 * so the same "i_temp < 0" error path still triggers on out-of-spec input)
 * without ever emitting a division instruction. Bit-exact for both the
 * well-formed case (i1_qp in [-26,25]) and the malformed case the
 * subsequent range check exists to catch.
 */
#include "ih264_typedefs.h"
#include "ih264_macros.h"
#include "ih264d_structs.h"
#include "ih264d_tables.h"
#include "ih264d_error_handler.h"
#include "ih264d_defs.h"

#if defined(MR_M68K_ASM)

WORD32 __wrap_ih264d_update_qp(dec_struct_t *ps_dec, const WORD8 i1_qp)
{
    WORD32 i_temp;

    i_temp = (WORD32)ps_dec->u1_qp + (WORD32)i1_qp + 52;
    if (i_temp < 0)
        i_temp += 52;
    else
        while (i_temp >= 52)
            i_temp -= 52;

    if ((i_temp < 0) || (i_temp > 51) || (i1_qp < -26) || (i1_qp > 25))
        return ERROR_INV_RANGE_QP_T;

    ps_dec->u1_qp = i_temp;
    ps_dec->u1_qp_y_rem6 = ps_dec->u1_qp % 6;
    ps_dec->u1_qp_y_div6 = ps_dec->u1_qp / 6;
    i_temp = CLIP3(0, 51, ps_dec->u1_qp +
                   ps_dec->ps_cur_pps->i1_chroma_qp_index_offset);
    ps_dec->u1_qp_u_rem6 = MOD(gau1_ih264d_qp_scale_cr[12 + i_temp], 6);
    ps_dec->u1_qp_u_div6 = DIV(gau1_ih264d_qp_scale_cr[12 + i_temp], 6);

    i_temp = CLIP3(0, 51, ps_dec->u1_qp +
                   ps_dec->ps_cur_pps->i1_second_chroma_qp_index_offset);
    ps_dec->u1_qp_v_rem6 = MOD(gau1_ih264d_qp_scale_cr[12 + i_temp], 6);
    ps_dec->u1_qp_v_div6 = DIV(gau1_ih264d_qp_scale_cr[12 + i_temp], 6);

    ps_dec->pu2_quant_scale_y = gau2_ih264_iquant_scale_4x4[ps_dec->u1_qp_y_rem6];
    ps_dec->pu2_quant_scale_u = gau2_ih264_iquant_scale_4x4[ps_dec->u1_qp_u_rem6];
    ps_dec->pu2_quant_scale_v = gau2_ih264_iquant_scale_4x4[ps_dec->u1_qp_v_rem6];
    return OK;
}

#endif
