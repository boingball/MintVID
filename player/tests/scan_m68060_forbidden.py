#!/usr/bin/env python3
"""Scan m68k object files' emitted machine code (not their C/asm source) for
instructions a real 68060 cannot execute in hardware:

  - the extended-result MULS.L/MULU.L (32x32->64, register-pair result) -
    always 3 operands; there is no 2-register "safe" degenerate case, unlike
    divide below, so any 3-operand muls*/mulu* is unconditionally forbidden.
  - the extended-dividend DIVS.L/DIVU.L (64-bit dividend, register-pair
    input) - also 3 operands, but the *same* mnemonic and operand count also
    covers the ordinary, hardware-everywhere-since-68020 32-bit-dividend
    form: objdump renders that safe form with its remainder and quotient
    operands equal (e.g. "divsl %d2,%d0,%d0"), and the trapping 64-bit form
    with them different (e.g. "divsl %d2,%d1,%d0"). Only the latter is
    forbidden.
  - any reference to libgcc's __muldi3/__divdi3/__udivdi3 (the 64-bit
    multiply/divide helpers GCC reaches for when it can't use the above
    hardware forms - the trap-avoidance these kernels exist for would be
    pointless if the compiler quietly routed back through a software
    64-bit multiply/divide anyway).

This is CLAUDE.md's "verify the emitted 68060 machine code, not just that
the C source looks safe" gate (see the MPEG-1/2 notes on
plm_audio_idct36_m68k_060.S/plm_audio_synth_window_m68k_060.S) - it is not
a comment/source scanner, it disassembles the real compiled object with
m68k-linux-gnu-objdump/nm and inspects the actual instruction encodings and
undefined symbol references.

Usage:
  scan_m68060_forbidden.py <object-file> [<object-file> ...]
  scan_m68060_forbidden.py --symbols sym1,sym2,... <object-file>
    Scans only the named functions (via objdump --disassemble=<sym>, one
    call per symbol) instead of the whole object - use this on a
    multi-function translation unit (e.g. mr_mpeg1.c, which drags in all of
    pl_mpeg.h) to check only the functions the MP2 hot path actually
    reaches, not unrelated dead code (seek/HTTP/etc.) compiled into the same
    object that was never proven trap-free and isn't this kernel's concern.

Exits 1 (and prints every violation found) if any object fails; 0 if all
objects are clean.
"""
import re
import subprocess
import sys


def split_top_level_commas(operand_str):
    """Split an objdump operand string on commas that separate real operands,
    not the commas inside a parenthesised indexed addressing mode (GNU/AT&T
    syntax renders e.g. "%sp@(74,%d3:l:4)" - displacement, index register,
    scale - as ONE effective-address operand with an internal comma). A
    naive str.split(',') would misparse that single-operand indexed EA as
    three operands and misidentify an ordinary, hardware-safe two-operand
    `MULS.L <ea>,Dn` as the forbidden three-operand extended-result form.
    Bracket-depth tracking (only '(' / ')' appear in m68k operand syntax)
    keeps commas inside any parenthesised group with the operand they
    belong to."""
    ops = []
    depth = 0
    current = []
    for ch in operand_str:
        if ch == '(':
            depth += 1
            current.append(ch)
        elif ch == ')':
            depth -= 1
            current.append(ch)
        elif ch == ',' and depth == 0:
            ops.append(''.join(current))
            current = []
        else:
            current.append(ch)
    if current:
        ops.append(''.join(current))
    return ops


def scan_disasm(dis_text, label):
    bad = []
    for line in dis_text.splitlines():
        parts = line.split('\t')
        if len(parts) < 3:
            continue
        insn = parts[-1].strip()
        m = re.match(r'^([a-zA-Z][a-zA-Z0-9.]*)\s*(.*)$', insn)
        if not m:
            continue
        mnem, operand_str = m.group(1).lower(), m.group(2)
        ops = [o.strip() for o in split_top_level_commas(operand_str)] if operand_str else []
        if mnem.startswith(('muls', 'mulu')) and len(ops) == 3:
            bad.append(f"{label}: extended-result multiply: {line.strip()}")
        elif mnem.startswith(('divs', 'divu')) and len(ops) == 3:
            if ops[1] != ops[2]:
                bad.append(f"{label}: extended-dividend divide: {line.strip()}")
    return bad


FORBIDDEN_LIBGCC = ('__muldi3', '__divdi3', '__udivdi3')


def scan_undefined(nm_text, label):
    """Whole-object undefined-symbol scan - used only in whole-object mode.
    Scoped (--symbols) mode uses scan_relocations instead, since nm -u
    cannot tell which function referenced a given undefined symbol and
    would flag unrelated dead code (e.g. plm_seek) compiled into the same
    translation unit as pl_mpeg.h's other, unreached functions."""
    bad = []
    for line in nm_text.splitlines():
        cols = line.split()
        if len(cols) == 2 and cols[0] == 'U':
            sym = cols[1]
        elif len(cols) >= 2 and cols[-2] == 'U':
            sym = cols[-1]
        else:
            continue
        if sym in FORBIDDEN_LIBGCC:
            bad.append(f"{label}: libgcc reference: {line.strip()}")
    return bad


def scan_relocations(reloc_dis_text, label):
    """Scans one function's disassembly (objdump --disassemble=<sym> -r,
    relocations shown inline) for a relocation against a forbidden libgcc
    symbol - the per-function-scoped equivalent of scan_undefined."""
    bad = []
    for line in reloc_dis_text.splitlines():
        for sym in FORBIDDEN_LIBGCC:
            if sym in line and 'R_68K' in line:
                bad.append(f"{label}: libgcc reference: {line.strip()}")
    return bad


def main(argv):
    if len(argv) < 2:
        print("usage: scan_m68060_forbidden.py <object-file> [...]", file=sys.stderr)
        return 2
    args = argv[1:]
    symbols = None
    if args and args[0] == '--symbols':
        symbols = args[1].split(',')
        args = args[2:]
    if not args:
        print("usage: scan_m68060_forbidden.py [--symbols a,b,c] <object-file> [...]",
              file=sys.stderr)
        return 2

    all_bad = []
    scanned = 0
    for path in args:
        if symbols:
            for sym in symbols:
                # --disassemble=<sym> must be passed WITHOUT a separate -d:
                # combined, -d wins and dumps the whole object regardless of
                # the requested symbol (a real objdump gotcha - verified by
                # hand, not assumed). -r (relocations, shown inline) combines
                # fine with --disassemble=<sym> and stays scoped to it.
                dis = subprocess.run(
                    ['m68k-linux-gnu-objdump', f'--disassemble={sym}', path],
                    capture_output=True, text=True).stdout
                if f'<{sym}>:' not in dis:
                    all_bad.append(f"{path}: symbol '{sym}' not found in object")
                    continue
                reloc_dis = subprocess.run(
                    ['m68k-linux-gnu-objdump', f'--disassemble={sym}', '-r', path],
                    capture_output=True, text=True).stdout
                all_bad += scan_disasm(dis, f"{path}:{sym}")
                all_bad += scan_relocations(reloc_dis, f"{path}:{sym}")
                scanned += 1
        else:
            nm = subprocess.run(['m68k-linux-gnu-nm', '-u', path],
                                 capture_output=True, text=True).stdout
            all_bad += scan_undefined(nm, path)
            dis = subprocess.run(['m68k-linux-gnu-objdump', '-d', path],
                                  capture_output=True, text=True).stdout
            all_bad += scan_disasm(dis, path)
            scanned += 1
    for b in all_bad:
        print(b)
    if all_bad:
        print(f"FAIL: {len(all_bad)} forbidden instruction(s)/reference(s) found")
        return 1
    print(f"OK: {scanned} function/object scan(s) clean of extended MULS.L/MULU.L, "
          "extended divide, and __muldi3/__divdi3/__udivdi3")
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
