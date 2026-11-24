#!/usr/bin/env python3
"""Check the PC-relative relocations of a linked AmigaOS executable.

usage: check_pcrel_relocs.py <exe> <exe.map>    (run where the link ran)

The Amiga hunk linker resolves a PC-relative relocation against a *section*
short of its target. GCC emits one when -ffunction-sections puts a function
in its own section and a tail call to it (a static function) becomes a
`bra.l` from another section. MintAMP's FlacClose -> FlacFreeState landed
6 bytes early and every FLAC play leaked 139,816 bytes. Relocations against
global symbols, and absolute ones, link correctly.

For each object the -Wl,-Map file lists, every PC-relative relocation
(RELRELOC*) in its .text and .text.* sections must name a global symbol and
land exactly on it. One against a section is reported with where it landed,
relative to that section's start. Exit status 1 if any fails. Sections with
other names (vasm's "code") link into separate hunks and are not checked.

The objects must still exist. Makefile.amiga compiles mrplay's sources on
the link line, so their objects are deleted: compile each source with -c and
relink from the objects to check it. Archive members are extracted."""
import os
import re
import struct
import subprocess
import sys
import tempfile

OBJDUMP = 'm68k-amigaos-objdump'
AR = 'm68k-amigaos-ar'


def read_map(path):
    """Return ([(section, address, object)], {symbol: address})."""
    lines = open(path).read().splitlines()
    start = next(i for i, l in enumerate(lines)
                 if l.startswith('Linker script and memory map'))
    contribs, syms, pending = [], {}, None
    for line in lines[start:]:
        if pending:  # a long section name wraps its address onto the next line
            m = re.match(r'^\s+(0x[0-9a-f]+)\s+(0x[0-9a-f]+)\s+(\S+)\s*$', line)
            if m and int(m.group(2), 16):
                contribs.append((pending, int(m.group(1), 16), m.group(3)))
            pending = None
            if m:
                continue
        m = re.match(r'^ (\.\S+)\s*$', line)
        if m:
            pending = m.group(1)
            continue
        m = re.match(r'^ (\.\S+)\s+(0x[0-9a-f]+)\s+(0x[0-9a-f]+)\s+(\S+)', line)
        if m:
            if int(m.group(3), 16):
                contribs.append((m.group(1), int(m.group(2), 16), m.group(4)))
            continue
        m = re.match(r'^\s+(0x[0-9a-f]+)\s+(\S+)\s*$', line)
        if m and not m.group(2).startswith('0x'):
            syms[m.group(2)] = int(m.group(1), 16)
    return contribs, syms


def read_code(exe):
    """Bytes of the first code hunk: the one the map describes. Hand-written
    assembler objects can add more code hunks, also called .text."""
    out = subprocess.run([OBJDUMP, '-s', '-j', '.text', exe],
                         capture_output=True, text=True, check=True).stdout
    image, blocks = {}, 0
    for line in out.splitlines():
        if line.startswith('Contents of section'):
            blocks += 1
            continue
        if blocks != 1:
            continue
        m = re.match(r'^ ([0-9a-f]{4,})((?: [0-9a-f]{2,8}){1,4})', line)
        if not m:
            continue
        addr = int(m.group(1), 16)
        for word in m.group(2).split():
            for j in range(0, len(word), 2):
                image[addr] = int(word[j:j + 2], 16)
                addr += 1
    return image


def main():
    exe, mapfile = sys.argv[1], sys.argv[2]
    contribs, syms = read_map(mapfile)
    image = read_code(exe)
    tmp = tempfile.mkdtemp()

    def object_file(name):
        m = re.match(r'(.*\.a)\((.*)\)$', name)
        if not m:
            return name
        d = os.path.join(tmp, '%x' % abs(hash(m.group(1))))
        path = os.path.join(d, m.group(2))
        if not os.path.exists(path):
            os.makedirs(d, exist_ok=True)
            subprocess.run([AR, 'x', os.path.abspath(m.group(1)), m.group(2)],
                           cwd=d, check=True)
        return path

    def word32(addr):
        return struct.unpack('>i', bytes(image[addr + i] for i in range(4)))[0]

    code = [(sec, base, obj) for sec, base, obj in contribs
            if sec == '.text' or sec.startswith('.text.')]
    section_base = {}
    for sec, base, obj in code:
        section_base.setdefault((obj, sec), base)  # first part of each section
    total = bad = 0
    for (obj, sec), base in section_base.items():
        path = object_file(obj)
        if not os.path.exists(path):
            continue
        relocs = subprocess.run([OBJDUMP, '-r', '-j', sec, path],
                                capture_output=True, text=True).stdout
        for line in relocs.splitlines():
            m = re.match(r'^([0-9a-f]+)\s+(RELRELOC\S*)\s+(\S+)$', line)
            if not m:
                continue
            total += 1
            off, kind, sym = int(m.group(1), 16), m.group(2), m.group(3)
            where = '%s %s+0x%x' % (obj, sec, off)
            if kind != 'RELRELOC32':
                bad += 1
                print('  %s: %s against %s, not checked' % (where, kind, sym))
                continue
            site = base + off
            landed = site + word32(site)
            name = sym[1:] if sym.startswith('_') and sym[1:] in syms else sym
            if name in syms:
                if landed != syms[name]:
                    bad += 1
                    print('  %s: lands at 0x%x, %s is at 0x%x (off by %d)'
                          % (where, landed, sym, syms[name], landed - syms[name]))
                continue
            bad += 1
            target = section_base.get((obj, sym))
            if target is None:
                print('  %s: against %s, not a global symbol' % (where, sym))
            else:
                print('  %s: against section %s, lands at %s%+d'
                      % (where, sym, sym, landed - target))
    print('%s: %d PC-relative relocations, %d bad' % (exe, total, bad))
    sys.exit(1 if bad else 0)


if __name__ == '__main__':
    main()
