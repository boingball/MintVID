#!/usr/bin/env python3
"""build_berrycore.py [ORIGINAL_KERNEL] OUTPUT_KERNEL

ORIGINAL_KERNEL defaults to upstream/Emu68-pistorm next to this script.

Builds the Emu68 BerryCore kernel from readable source:
  1. assembles berrycore_patch.S (clang, target aarch64),
  2. copies every section .p_<hexoffset> to that file offset of the
     original Emu68 1.0.7 PiStorm kernel,
  3. checks the SHA-256 of input and output.

Needs: python3, clang and llvm-objcopy (LLVM 14 or newer).
"""
import hashlib, os, re, subprocess, sys, tempfile

ORIGINAL_SHA256 = '3126f461807f75c51977291d49d331f47fd3b5daf732ae8be35cbc14cbc4d3ac'
TARGET_SHA256   = '9b2ef4ded101d6aa70752714767651dc70fb777c28a33796c59dfff6cf26e57c'
HERE = os.path.dirname(os.path.abspath(__file__))

def run(*cmd):
    subprocess.run(cmd, check=True)

def main(src, dst):
    data = bytearray(open(src, 'rb').read())
    if hashlib.sha256(data).hexdigest() != ORIGINAL_SHA256:
        sys.exit('ERROR: input is not the original Emu68 1.0.7 PiStorm kernel')
    with tempfile.TemporaryDirectory() as tmp:
        obj = os.path.join(tmp, 'patch.o')
        run('clang', '--target=aarch64-none-elf', '-c',
            os.path.join(HERE, 'berrycore_patch.S'), '-o', obj)
        heads = subprocess.run(['llvm-objdump', '-h', obj], check=True,
                               capture_output=True, text=True).stdout
        sections = re.findall(r'^\s*\d+\s+(\.p_([0-9a-f]+))\s', heads, re.M)
        for name, off in sections:
            out = os.path.join(tmp, name + '.bin')
            run('llvm-objcopy', '-O', 'binary', '--only-section=' + name, obj, out)
            blob = open(out, 'rb').read()
            o = int(off, 16)
            print('  %-10s -> offset 0x%06X, %4d bytes' % (name, o, len(blob)))
            data[o:o + len(blob)] = blob
    digest = hashlib.sha256(data).hexdigest()
    open(dst, 'wb').write(data)
    print('SHA-256:', digest)
    if digest != TARGET_SHA256:
        sys.exit('WARNING: result differs from the reference BerryCore kernel')
    print('OK: identical to Emu68-pistorm-BerryCore (13.09.2026)')

if __name__ == '__main__':
    if len(sys.argv) == 2:
        main(os.path.join(HERE, 'upstream', 'Emu68-pistorm'), sys.argv[1])
    elif len(sys.argv) == 3:
        main(sys.argv[1], sys.argv[2])
    else:
        sys.exit(__doc__)
