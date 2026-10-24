#!/usr/bin/env python3
"""Map vtables to class names in an SE4 PE (no RTTI). Read-only heuristic.

Every vtable start (constructor-style store, see vtable_slot.vtable_starts) that holds <fn> in any slot is
listed with the nearest preceding class-name string ('C...' + Entity/Params/Mode/...) in .rdata. SE4 emits a
class's name string, its member/meta records and then its vtable in one .rdata block, so the preceding
name is usually the owning class. Confirm with the class's member offsets used by the vtable's functions
(meta_class.py) before relying on it.
usage: class_vtables.py <exe> <fn-va> [<slot-off>]   vtables holding fn (optionally only at slot-off)
       class_vtables.py <exe> --name <vtable-va> ...   nearest preceding class name for given vtables
"""
import bisect, re, sys
from se4pe import Image
from vtable_slot import vtable_starts, data_ptrs

NAME = re.compile(rb'\0(C[A-Z][A-Za-z0-9_]{3,80}(?:Entity|Params|Mode|Holder|Handler|Data|Element))\0')


def names(img):
    rsva, rdata = img.section_bytes('.rdata')
    return [(rsva + m.start(1), m.group(1).decode()) for m in NAME.finditer(rdata)]


def owner(nm, va):
    i = bisect.bisect_right([a for a, _ in nm], va) - 1
    return nm[i] if i >= 0 else (0, '?')


def main():
    img = Image(sys.argv[1]); nm = names(img)
    if sys.argv[2] == '--name':
        for v in sys.argv[3:]:
            a, n = owner(nm, int(v, 16))
            print('vt %s  <- %s (name at %s)' % (v, n, hex(a)))
        return
    fn = int(sys.argv[2], 16); slot = int(sys.argv[3], 16) if len(sys.argv) > 3 else None
    starts = vtable_starts(img)
    for p in data_ptrs(img, fn):
        i = bisect.bisect_right(starts, p) - 1
        if i < 0:
            continue
        off = p - starts[i]
        if slot is not None and off != slot:
            continue
        a, n = owner(nm, starts[i])
        print('vt %s slot +%s  <- %s (name at %s)' % (hex(starts[i]), hex(off), n, hex(a)))


if __name__ == '__main__':
    main()
