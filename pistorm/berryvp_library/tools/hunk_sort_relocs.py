#!/usr/bin/env python3
"""hunk_sort_relocs.py IN OUT

Rewrites an AmigaOS hunk executable so that every relocation list
(HUNK_RELOC32 / HUNK_RELOC32SHORT / HUNK_DREL32) is sorted by ascending
offset.  The loaded program is unchanged - only the order of entries in the
file differs.  vasm emits relocations in instruction-operand order; the
historical berryvp.library binaries were built from a binary-derived source
whose relocations are in address order, so sorting reproduces them exactly.

  hunk_sort_relocs.py IN OUT --keep-order SYMBUILD START END [START END ...]

keeps vasm's operand order for the code between label pairs (SYMBUILD is
the same source assembled without -nosym; END '-' = end of that hunk).
P32 needs this for the code that was already written as real instructions
in the historical P32 build: the AC3/E-AC3 module loaders
(ac307_prepare_ac3 .. fast13_h0_trampoline) and the audio-hunk additions
(ac307_ConfigAudio .. end of hunk).
"""
import struct, sys

def read_symbols(path):
    """Return {name: (hunk_index, offset)} from HUNK_SYMBOL blocks."""
    d = open(path, 'rb').read(); p = 4; syms = {}
    L = lambda q: struct.unpack('>I', d[q:q+4])[0]
    while L(p): p += 4 + 4*L(p)
    p += 4; f = L(p+4); l = L(p+8); p += 12 + 4*(l-f+1); h = 0
    while p < len(d):
        t = L(p) & 0x3fffffff; p += 4
        if t in (0x3e9, 0x3ea): p += 4 + 4*L(p)
        elif t == 0x3eb: p += 4
        elif t == 0x3ec:
            while L(p): p += 8 + 4*L(p)
            p += 4
        elif t in (0x3f7, 0x3fc):
            q = p
            while True:
                c = struct.unpack('>H', d[p:p+2])[0]; p += 2
                if c == 0: break
                p += 2 + 2*c
            if (p - q) % 4: p += 2
        elif t == 0x3f0:
            while L(p):
                c = L(p); name = d[p+4:p+4+4*c].rstrip(b'\0').decode()
                syms[name] = (h, L(p+4+4*c)); p += 8 + 4*c
            p += 4
        elif t == 0x3f2: h += 1
        else: raise SystemExit('unsupported hunk type %#x' % t)
    return syms

def main(src, dst, keep=()):
    """keep: list of (hunk, start, end) - relocations whose offset lies in
    [start, end) of that hunk keep the assembler's original order (they
    come from code that was hand-written as real instructions in the
    historical build); everything else is sorted."""
    d = open(src, 'rb').read()
    out = bytearray(); p = 0
    def L():
        nonlocal p
        v = struct.unpack('>I', d[p:p+4])[0]; p += 4; return v
    def put(v): out.extend(struct.pack('>I', v))
    assert L() == 0x3f3; put(0x3f3)
    while True:                         # resident library names
        n = L(); put(n)
        if n == 0: break
        out.extend(d[p:p+4*n]); p += 4*n
    n = L(); f = L(); l = L(); put(n); put(f); put(l)
    for _ in range(l - f + 1): put(L())
    hunk_no = 0
    while p < len(d):
        raw = L(); t = raw & 0x3fffffff
        if t in (0x3e9, 0x3ea):                     # CODE / DATA
            sz = L(); put(raw); put(sz); out.extend(d[p:p+sz*4]); p += sz*4
        elif t == 0x3eb:                            # BSS
            put(raw); put(L())
        elif t == 0x3ec:                            # RELOC32 (long)
            put(raw)
            while True:
                c = L(); put(c)
                if c == 0: break
                h = L(); raw_offs = [L() for _ in range(c)]; put(h)
                offs = sorted(raw_offs)
                for kh, k0, k1 in keep:
                    if kh != hunk_no: continue
                    it = iter([o for o in raw_offs if k0 <= o < k1])
                    offs = [next(it) if k0 <= o < k1 else o for o in offs]
                for o in offs: put(o)
        elif t in (0x3f7, 0x3fc):                   # RELOC32SHORT (word)
            put(raw); st = len(out); q0 = p
            def W():
                nonlocal p
                v = struct.unpack('>H', d[p:p+2])[0]; p += 2; return v
            while True:
                c = W(); out.extend(struct.pack('>H', c))
                if c == 0: break
                h = W(); raw_offs = [W() for _ in range(c)]
                offs = sorted(raw_offs)
                for kh, k0, k1 in keep:
                    if kh != hunk_no: continue
                    it = iter([o for o in raw_offs if k0 <= o < k1])
                    offs = [next(it) if k0 <= o < k1 else o for o in offs]
                out.extend(struct.pack('>H', h))
                for o in offs: out.extend(struct.pack('>H', o))
            if (p - q0) % 4: p += 2; out.extend(b'\0\0')
        elif t == 0x3f2:                            # END
            put(raw); hunk_no += 1
        elif t == 0x3f0:                            # SYMBOL
            put(raw)
            while True:
                c = L(); put(c)
                if c == 0: break
                out.extend(d[p:p+c*4+4]); p += c*4+4
        else:
            raise SystemExit('unsupported hunk type %#x' % t)
    open(dst, 'wb').write(out)

if __name__ == '__main__':
    a = sys.argv[1:]
    keep = []
    if len(a) >= 6 and a[2] == '--keep-order' and len(a) % 2 == 0:
        # --keep-order SYMBOL_BUILD START END [START END ...]
        # END may be '-' for "up to the end of the hunk".
        syms = read_symbols(a[3])
        for st, en in zip(a[4::2], a[5::2]):
            hs, s0 = syms[st]
            if en == '-': he, s1 = hs, 1 << 32
            else: he, s1 = syms[en]
            assert hs == he, 'labels must be in the same hunk'
            keep.append((hs, s0, s1))
    elif len(a) != 2:
        raise SystemExit(__doc__)
    main(a[0], a[1], keep)
