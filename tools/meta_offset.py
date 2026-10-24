#!/usr/bin/env python3
"""Reverse of meta_members.py: list SE4 reflection member records with a given byte offset, optionally
filtered by a regex on the owning class or member name (e.g. which CPlayerActorPuppetEntity member is +0xf90).
usage: meta_offset.py <exe> <class-or-member-regex> <off> [<off> ...]
"""
import re, sys
import numpy as np
from se4pe import Image
from meta_members import is_member_record, owner

if __name__ == '__main__':
    img = Image(sys.argv[1]); rx = re.compile(sys.argv[2])
    sva, vs, raw, rs = img.sections['.rdata']
    buf = img.data[raw:raw + min(vs, rs)]
    a = np.frombuffer(buf[:len(buf) // 8 * 8], dtype=np.uint64)
    for o in (int(x, 16) for x in sys.argv[3:]):
        for i in np.nonzero(a == o)[0]:
            rec = sva + int(i) * 8 - 0x38          # +0x38 holds the offset
            if rec < sva:
                continue
            try:
                if not is_member_record(img, rec):
                    continue
                name = img.cstr(img.q(rec + 0x20), 80)
                _, cls = owner(img, rec)
            except Exception:
                continue
            own = cls[1] if len(cls) > 1 else '?'
            if rx.search(own) or rx.search(name or ''):
                print('+%s %-40s %s (rec %s)' % (hex(o), name, own, hex(rec)))
