#!/usr/bin/env python3
"""Synthesise an 8-bit (paletted) Microsoft Video 1 AVI.

ffmpeg's msvideo1 *encoder* only emits the RGB555 variant, so `make check`
would otherwise never exercise the 8-bit block layouts - which is exactly
where the 8-colour opcode differs from 16-bit (a `byte_b >= 0x90` selector
rather than a flag bit in the first colour word). ffmpeg's *decoder* handles
both, so a hand-built stream still gets validated against the same oracle.

The stream is deterministic: every block encoding (1-, 2- and 8-colour, plus
skip runs of one and of several blocks) appears in a fixed pattern.
"""
import struct, sys

W, H, FRAMES, RATE = 64, 48, 8, 12
BW, BH = W // 4, H // 4


def palette():
    """256 BGRA entries: a 6x6x6 colour cube then a grey ramp."""
    pal = []
    for i in range(256):
        if i < 216:
            r, g, b = i // 36, (i // 6) % 6, i % 6
            pal.append((b * 51, g * 51, r * 51, 0))
        else:
            v = (i - 216) * 6
            pal.append((v, v, v, 0))
    return b"".join(struct.pack("4B", *e) for e in pal)


class Rnd:
    """Tiny deterministic LCG so the fixture never depends on host RNG."""
    def __init__(self, seed): self.s = seed
    def __call__(self, n):
        self.s = (self.s * 1103515245 + 12345) & 0x7FFFFFFF
        return (self.s >> 8) % n


def encode_frame(rnd, frame_no):
    """Emit blocks bottom row first, left to right - the format's order."""
    out = bytearray()
    pending_skip = 0

    def flush_skip():
        nonlocal pending_skip
        while pending_skip:
            n = min(pending_skip, 0x3FF)
            out.append(n & 0xFF)              # byte_a
            out.append(0x84 + (n >> 8))       # byte_b
            pending_skip -= n

    for by in range(BH - 1, -1, -1):
        for bx in range(BW):
            kind = (bx + by * 3 + frame_no * 5) % 5
            if frame_no > 0 and kind == 0:
                pending_skip += 1             # keep previous frame's block
                continue
            flush_skip()
            if kind == 1:                     # flat 1-colour
                out.append(rnd(216))          # byte_a = palette index
                out.append(0x80)              # byte_b in 0x80..0x8f, not 0x84
            elif kind in (2, 3):              # 2-colour, flags in (b<<8)|a
                flags = rnd(0x8000)           # byte_b must stay < 0x80
                out.append(flags & 0xFF)
                out.append(flags >> 8)
                out.append(rnd(216))
                out.append(rnd(216))
            else:                             # 8-colour, byte_b >= 0x90
                flags = 0x9000 | rnd(0x1000)
                out.append(flags & 0xFF)
                out.append(flags >> 8)
                out += bytes(rnd(216) for _ in range(8))
    flush_skip()
    return bytes(out)


def chunk(tag, body):
    return tag + struct.pack("<I", len(body)) + body + (b"\0" if len(body) & 1 else b"")


def build():
    rnd = Rnd(20260908)
    frames = [encode_frame(rnd, i) for i in range(FRAMES)]

    bih = struct.pack("<IiiHH4sIiiII", 40, W, H, 1, 8, b"MSVC",
                      W * H, 0, 0, 256, 256) + palette()
    strh = struct.pack("<4s4sIHHIIIIIIIIhhhh", b"vids", b"MSVC", 0, 0, 0, 0,
                       1, RATE, 0, len(frames), max(len(f) for f in frames),
                       0, 0, 0, 0, W, H)
    strl = chunk(b"strh", strh) + chunk(b"strf", bih)
    avih = struct.pack("<IIIIIIIIIIIIIIII", 1000000 // RATE, 0, 0, 0x10,
                       len(frames), 0, 1, W * H, W, H, 0, 0, 0, 0, 0, 0)
    hdrl = b"hdrl" + chunk(b"avih", avih) + b"LIST" + \
        struct.pack("<I", 4 + len(strl)) + b"strl" + strl

    movi, idx, off = b"movi", b"", 4
    for f in frames:
        movi += chunk(b"00dc", f)
        idx += b"00dc" + struct.pack("<III", 0x10, off, len(f))
        off += 8 + len(f) + (len(f) & 1)

    body = b"AVI " + b"LIST" + struct.pack("<I", len(hdrl)) + hdrl + \
        b"LIST" + struct.pack("<I", len(movi)) + movi + chunk(b"idx1", idx)
    return b"RIFF" + struct.pack("<I", len(body)) + body


if __name__ == "__main__":
    path = sys.argv[1] if len(sys.argv) > 1 else "test_msvideo1_pal8.avi"
    with open(path, "wb") as f:
        f.write(build())
    print("wrote %s (%d frames, %dx%d, 8-bit paletted MSVC)" % (path, FRAMES, W, H))
