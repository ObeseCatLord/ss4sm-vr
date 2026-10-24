#!/usr/bin/env python3
"""Resolve SE4 reflection member records (class member name -> owning class, type, byte offset).

SE4 meta member records are 0x60 bytes: +0x00 kind, +0x08 type-name ptr (or type record), +0x10 id,
+0x18 "" , +0x20 member-name ptr, +0x28 id-string ptr, +0x30 "", +0x38 offset, +0x40 flags.
(Layout read off the CWeaponEntity table at Sam4 0x141710820..; verified on several members.)
The owning class is found by walking back over consecutive member records to the class header and
printing the class-like names referenced just before the first member.
usage: meta_members.py <exe> <member-name> [...]
"""
import sys
import numpy as np
from se4pe import Image
from strrefs import find_strings, abs_ptrs


def is_member_record(img, rec):
    n = img.q(rec + 0x20)
    s = img.cstr(n, 80) if n else None
    return bool(s) and '_' in s and s.split('_', 1)[0].isalpha() and s.split('_', 1)[0].islower()


def owner(img, rec):
    r = rec
    while is_member_record(img, r - 0x60):
        r -= 0x60
    names = []
    for k in range(1, 120):
        q = img.q(r - 8 * k)
        s = img.cstr(q, 80) if q and q > img.base else None
        if s and s[:1] == 'C' and s[1:2].isupper() and s not in names:
            names.append(s)
        if len(names) >= 2:
            break
    return r, names


if __name__ == '__main__':
    img = Image(sys.argv[1])
    for m in sys.argv[2:]:
        vas = find_strings(img, m)
        if not vas:
            print('%-40s NOT FOUND' % m); continue
        ap = abs_ptrs(img, vas)
        for v in vas:
            for p in ap[v]:
                rec = p - 0x20
                t = img.q(rec + 8); ts = img.cstr(t, 60) if t and t > img.base else None
                off = img.q(rec + 0x38); fl = img.q(rec + 0x40)
                first, cls = owner(img, rec)
                own = cls[1] if len(cls) > 1 else '?'
                base = cls[0] if cls else '?'
                print('%-40s rec %s type %-22s off 0x%x flags 0x%x class %s (base %s)' % (
                    m, hex(rec), ts or hex(t or 0), off, fl, own, base))
