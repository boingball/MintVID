#!/usr/bin/env python3
"""build_berrycore32.py ORIGINAL_KERNEL OUTPUT_KERNEL

Builds Emu68-pistorm32lite-BerryCore from berrycore32_patch.S and the
original Emu68 1.0.7 PiStorm32 lite kernel ("Emu68-pistorm32lite",
08.12.2025, git 305f686).  Same method as build_berrycore.py (Pi 3).

Needs: python3, clang, llvm-objdump, llvm-objcopy (LLVM 14 or newer).
"""
import hashlib, os, re, subprocess, sys, tempfile

ORIGINAL_SHA256 = '262afc1473299d1e95cffd6ebff6427b4b737776e8380d264cf52bdb1878a9e0'
HERE = os.path.dirname(os.path.abspath(__file__))

def run(*cmd):
    subprocess.run(cmd, check=True)

def main(src, dst):
    data = bytearray(open(src, 'rb').read())
    if hashlib.sha256(data).hexdigest() != ORIGINAL_SHA256:
        sys.exit('ERROR: input is not the original Emu68 1.0.7 PiStorm32 lite kernel')
    with tempfile.TemporaryDirectory() as tmp:
        obj = os.path.join(tmp, 'patch.o')
        run('clang', '--target=aarch64-none-elf', '-c',
            os.path.join(HERE, 'berrycore32_patch.S'), '-o', obj)
        heads = subprocess.run(['llvm-objdump', '-h', obj], check=True,
                               capture_output=True, text=True).stdout
        sections = re.findall(r'^\s*\d+\s+(\.p_([0-9a-f]+))\s', heads, re.M)
        for name, off in sections:
            out = os.path.join(tmp, name + '.bin')
            run('llvm-objcopy', '-O', 'binary', '--only-section=' + name, obj, out)
            blob = open(out, 'rb').read()
            o = int(off, 16)
            if o >= 0x21b000:   # service: must land in the zero padding
                if any(data[o:o + len(blob)]):
                    sys.exit('ERROR: target area 0x%X is not empty' % o)
            print('  %-10s -> offset 0x%06X, %4d bytes' % (name, o, len(blob)))
            data[o:o + len(blob)] = blob
    open(dst, 'wb').write(data)
    print('SHA-256:', hashlib.sha256(data).hexdigest())

if __name__ == '__main__':
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    main(sys.argv[1], sys.argv[2])
