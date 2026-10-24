#!/usr/bin/env python3
"""Slots where a derived vtable differs from a base vtable (= the derived class's overrides). Read-only.
usage: vtable_diff.py <exe> <base-vt> <derived-vt> [<max-off>=0xd00]
"""
import sys
from se4pe import Image

img = Image(sys.argv[1]); b, d = int(sys.argv[2], 16), int(sys.argv[3], 16)
mx = int(sys.argv[4], 16) if len(sys.argv) > 4 else 0xd00
sva, text = img.section_bytes('.text')
for o in range(0, mx, 8):
    x, y = img.q(b + o), img.q(d + o)
    if not (sva <= y < sva + len(text)):
        print('end of derived at +%s' % hex(o)); break
    if x != y:
        print('+%-6s base %s  derived %s' % (hex(o), hex(x), hex(y)))
