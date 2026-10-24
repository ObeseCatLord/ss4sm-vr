#!/usr/bin/env python3
"""List every CMenuScreen-derived vtable of an SE4 PE with the three VR presentation slots the VR view reads
(R11): +0x1a8 "show on the cinema screen" (0x1401dcbe0 / 0x1401da9d0 / 0x1401d96a0), +0x1b0 "fixed screen"
(viewer+0xd0 in 0x1401dcbe0) and +0x48 "monitor only" (0x140062050: no cursor rescale, cinema shows the
"This menu screen should only be viewed on a monitor!" text in 0x1401d96a0).

Menu vtables are found as the qword runs that end with the same two CMenuScreen base slots (+0x1b8, +0x1c0)
as a known menu vtable passed on the command line (Sam4 CMSLoading3 0x141b9c3f8, SamSM 0x141b80c48). Slot
bodies that are the shared `mov eax,1; ret` / `xor eax,eax; ret` stubs are printed as 1 / 0, anything else
as its address (a per-class override, e.g. one that asks the menu below it).
usage: menu_vr_modes.py <exe> <known-menu-vtable> [--all]
Read-only.
"""
import struct
import sys
from collections import Counter
from se4pe import Image


def main():
    img = Image(sys.argv[1]); known = int(sys.argv[2], 16); show_all = '--all' in sys.argv
    q = lambda va: struct.unpack('<Q', img.data[img.off(va):img.off(va) + 8])[0]
    b1b8, b1c0 = q(known + 0x1b8), q(known + 0x1c0)

    def stub(fn):
        o = img.off(fn); code = img.data[o:o + 6]
        if code.startswith(b'\xb8\x01\x00\x00\x00\xc3'): return '1'
        if code.startswith(b'\x33\xc0\xc3') or code.startswith(b'\x31\xc0\xc3'): return '0'
        return hex(fn)

    pat = struct.pack('<Q', b1c0)
    found = []
    for name, (sva, vsize, raw, rsize) in img.sections.items():
        if name == '.text':
            continue
        buf = img.data[raw:raw + min(vsize, rsize)]
        o = buf.find(pat)
        while o >= 0:
            if o % 8 == 0 and o >= 0x1c0:
                vt = sva + o - 0x1c0
                if q(vt + 0x1b8) == b1b8:
                    found.append(vt)
            o = buf.find(pat, o + 1)
    rows = [(vt, stub(q(vt + 0x1a8)), stub(q(vt + 0x1b0)), stub(q(vt + 0x48)), hex(q(vt + 0x190))) for vt in found]
    hist = Counter((c, f, m) for _, c, f, m, _ in rows)
    print('%d menu vtables (base slots +0x1b8 %#x, +0x1c0 %#x)' % (len(rows), b1b8, b1c0))
    print('count  cinema(+0x1a8) fixed(+0x1b0) monitor(+0x48)')
    for (c, f, m), n in sorted(hist.items(), key=lambda kv: -kv[1]):
        print('%5d  %-14s %-13s %s' % (n, c, f, m))
    for vt, c, f, m, step in rows:
        if show_all or (c, f, m) != ('1', '0', '0'):
            print('vt %#x  cinema %-12s fixed %-12s monitor %-12s step %s' % (vt, c, f, m, step))


if __name__ == '__main__':
    main()
