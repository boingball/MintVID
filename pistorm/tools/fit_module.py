#!/usr/bin/env python3
"""fit_module.py MODULE SIZE

berryvp.library P32 loads every CPU1 module with an exact, hard-coded size
(berry_ac3 0x8360, berry_faad03 0x22109, berry_eac3 0x1C3CC).  A module
built with another compiler version can come out a little smaller; it is
padded with zero bytes at the end (outside the module's code and data).
A module that is larger than the slot is an error.
"""
import sys
path, size = sys.argv[1], int(sys.argv[2], 0)
data = open(path, 'rb').read()
if len(data) > size:
    sys.exit('%s: 0x%X bytes, larger than the P32 slot 0x%X' % (path, len(data), size))
if len(data) < size:
    open(path, 'wb').write(data + b'\0' * (size - len(data)))
    print('%s: padded 0x%X -> 0x%X bytes' % (path, len(data), size))
else:
    print('%s: 0x%X bytes' % (path, size))
