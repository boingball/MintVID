#!/usr/bin/env python3
"""Reject the libnix ReAction auto-openers seen in broken GUI builds."""
import pathlib
import sys

if len(sys.argv) < 2:
    sys.exit("usage: check_gui_libraries.py <Amiga GUI binary> ...")

failed = False
for name in sys.argv[1:]:
    data = pathlib.Path(name).read_bytes()
    for library in (b"window.library\0", b"gadgets/label.gadget\0"):
        if library in data:
            print(f"{name}: unexpected auto-opener for {library[:-1].decode()}")
            failed = True
if failed:
    sys.exit(1)
print("GUI library startup check: ok")
