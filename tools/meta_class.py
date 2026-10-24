#!/usr/bin/env python3
"""Dump every reflection member record of a class (SE4 meta member records, 0x60 bytes: +0x08 type,
+0x20 name, +0x38 offset, +0x40 flags; see meta_members.py).
  meta_class.py <exe> <member-name> [...]      class that owns each member
  meta_class.py <exe> --class <ClassName> [...] every member run whose owner (meta_members.owner) is that class
The --class mode indexes all member-record runs in the data sections (qwords at rec+0x20 that point to a
'prefix_Name' string), so it also finds classes whose members are not known in advance.
"""
import re
import sys
import numpy as np
from se4pe import Image
from strrefs import find_strings, abs_ptrs
from meta_members import is_member_record, owner


def dump(img, rec):
    first, cls = owner(img, rec)
    print('-- class %s (base %s), first record %s' % (cls[1] if len(cls) > 1 else '?',
                                                       cls[0] if cls else '?', hex(first)))
    r = first
    while is_member_record(img, r):
        t = img.q(r + 8)
        ts = img.cstr(t, 60) if t and t > img.base else None
        print('   %-48s +0x%-5x %-28s flags 0x%x  rec %s' % (img.cstr(img.q(r + 0x20), 80), img.q(r + 0x38),
                                                          ts or hex(t or 0), img.q(r + 0x40), hex(r)))
        r += 0x60
    return first


def all_runs(img):
    """First record of every member-record run."""
    names = set()
    for m in re.finditer(rb'(?<=\0)[a-z]{1,8}_[A-Za-z0-9_]{2,80}\0', img.data):
        va = img.va(m.start())
        if va:
            names.add(va)
    names = np.array(sorted(names), dtype=np.uint64)
    recs = []
    for name, (sva, vsize, raw, rsize) in img.sections.items():
        if name == '.text':
            continue
        buf = img.data[raw:raw + min(vsize, rsize)]
        n = len(buf) // 8 * 8
        a = np.frombuffer(buf[:n], dtype=np.uint64)
        for i in np.nonzero(np.isin(a, names))[0]:
            recs.append(sva + int(i) * 8 - 0x20)
    rs = set(recs)
    return sorted(r for r in recs if (r - 0x60) not in rs and is_member_record(img, r))


if __name__ == '__main__':
    img = Image(sys.argv[1])
    args = sys.argv[2:]
    if args and args[0] == '--class':
        want = set(args[1:])
        for first in all_runs(img):
            _, cls = owner(img, first)
            if len(cls) > 1 and cls[1] in want:
                print('== %s' % cls[1])
                dump(img, first)
        sys.exit(0)
    for m in args:
        vas = find_strings(img, m)
        if not vas:
            print('== %s NOT FOUND' % m)
            continue
        ap = abs_ptrs(img, vas)
        done = set()
        for v in vas:
            for p in ap[v]:
                first, _ = owner(img, p - 0x20)
                if first in done:
                    continue
                done.add(first)
                print('== %s' % m)
                dump(img, p - 0x20)
