#!/usr/bin/env python3
"""Locate the vtable(s) that contain a function pointer and report the slot offset.
A vtable is taken as a maximal run of qwords pointing into .text; the qword before it is the MSVC RTTI
complete-object-locator pointer, whose type descriptor name (+0x10 of the TD) is printed when present.
Also prints which code loads the vtable start (lea reg,[rip+vt]) = constructors/destructors.
Caveat: SE4 vtables are often adjacent with no separator, so the backward walk can run into the previous
vtable and report a start that is too early (slot offsets then too large). Confirm the start with the
vptr a constructor stores (e.g. Sam4 CVehicleEntity: ctor 0x140757230 stores 0x141a88380, while the walk
from its step slot reports 0x141a88148).
usage: vtable_of.py <exe> <fn-va> [...]      or   vtable_of.py <exe> --slot <vtable-va> <offset> [...]
"""
import sys
from se4pe import Image
from strrefs import abs_ptrs
from riprefs import rip_refs
from pe_fninfo import Fns


def in_text(img, v):
    s = img.sections['.text']
    return v is not None and s[0] <= v < s[0] + s[1]


def vt_start(img, p):
    while in_text(img, img.q(p - 8)):
        p -= 8
    return p


def rtti_name(img, vt):
    col = img.q(vt - 8)
    if not col or img.section_of(col) != '.rdata':
        return None
    td_rva = img.q(col + 12) & 0xffffffff
    name = img.cstr(img.base + td_rva + 0x10, 200)
    return name


if __name__ == '__main__':
    img = Image(sys.argv[1]); fns = Fns(img)
    a = sys.argv[2:]
    if a and a[0] == '--slot':
        vt = int(a[1], 16)
        for o in a[2:]:
            print('%s +%s -> %s' % (hex(vt), o, hex(img.q(vt + int(o, 16)) or 0)))
        sys.exit(0)
    targets = [int(x, 16) for x in a]
    ap = abs_ptrs(img, targets)
    for t in targets:
        print('== %s' % hex(t))
        for p in ap[t]:
            if img.section_of(p) != '.rdata':
                continue
            vt = vt_start(img, p)
            loads = rip_refs(img, [vt], 0)[vt]
            print('   vtable %s slot +0x%x  rtti %s  loaded by %s' % (
                hex(vt), p - vt, rtti_name(img, vt), ' '.join('%s(fn %s)' % (hex(l), hex(fns.containing(l)[0] or 0)) for l in loads[:6])))
