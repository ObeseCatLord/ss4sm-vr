#!/usr/bin/env python3
"""Dump SE4 reflection enum tables: {qword value, name ptr, description ptr} records (0x18 each).
Given one enumerator name (e.g. WF_GRIPPED_WITH_TWO_HANDS), finds the 8-byte pointers to it in the data
sections, walks back and forward over consecutive records whose name pointer is a C identifier, and prints
the whole run with values. Read-only.
usage: meta_enum.py <exe> <enumerator-name> [...]
"""
import re
import sys
from se4pe import Image
from strrefs import find_strings, abs_ptrs

IDENT = re.compile(r'^[A-Za-z_][A-Za-z0-9_]*$')


def name_at(img, p):
    try:
        if not p or p < img.base:
            return None
        s = img.cstr(p, 96)
    except Exception:
        return None
    return s if s and IDENT.match(s) else None


def is_rec(img, r):
    return name_at(img, img.q(r + 8)) is not None and img.q(r) < 1 << 40


def main():
    img = Image(sys.argv[1])
    for want in sys.argv[2:]:
        svas = find_strings(img, want)
        for sva, ptrs in abs_ptrs(img, svas).items():
            for ptr in ptrs:
                r = ptr - 8
                if not is_rec(img, r):
                    continue
                lo = r
                while is_rec(img, lo - 0x18):
                    lo -= 0x18
                hi = r
                while is_rec(img, hi + 0x18):
                    hi += 0x18
                print(f'== {want}: enum run 0x{lo:x}..0x{hi:x}')
                for x in range(lo, hi + 0x18, 0x18):
                    print(f'   0x{x:x}  value 0x{img.q(x):x}  {name_at(img, img.q(x + 8))}')


if __name__ == '__main__':
    main()
