#!/usr/bin/env python3
"""Vtable starts and slot lookups for an SE4 PE (no RTTI in these builds).

A vtable start is the target of a constructor-style store: `lea r64,[rip+X]` followed within 8 bytes by
`mov qword ptr [reg], r64` (offset 0), where X is in .rdata and *X points into .text.
  vtable_slot.py <exe> slots <vtable> <off> [<off> ...]   print vt+off values
  vtable_slot.py <exe> of <fn> [<fn> ...]                  every vtable (and slot) holding a function pointer
  vtable_slot.py <exe> at <slot-off> <fn>                  vtables that hold <fn> exactly at <slot-off>
Read-only; byte-pattern heuristics, so check unexpected hits in Ghidra.
"""
import bisect, re, struct, sys
import numpy as np
from se4pe import Image


def vtable_starts(img):
    sva, text = img.section_bytes('.text')
    rsva, rdata = img.section_bytes('.rdata')
    out = set()
    # REX.W/REX.WR lea r64,[rip+disp32]: 48|4c 8d modrm(00 reg 101)
    for m in re.finditer(rb'[\x48\x4c]\x8d[\x05\x0d\x15\x1d\x25\x2d\x35\x3d]', text):
        i = m.start()
        tgt = sva + i + 7 + struct.unpack_from('<i', text, i + 3)[0]
        if not (rsva <= tgt < rsva + len(rdata)):
            continue
        q = struct.unpack_from('<Q', rdata, tgt - rsva)[0]
        if not (sva <= q < sva + len(text)):
            continue
        tail = text[i + 7:i + 15]
        # mov qword ptr [reg], r64 : 48|49|4c|4d 89 modrm(mod=00, rm!=100/101)
        for k in range(0, 6):
            b = tail[k:k + 3]
            if len(b) == 3 and b[0] in (0x48, 0x49, 0x4c, 0x4d) and b[1] == 0x89 and (b[2] >> 6) == 0 \
                    and (b[2] & 7) not in (4, 5):
                out.add(tgt)
                break
    return sorted(out)


def data_ptrs(img, target):
    res = []
    for name, (sva, vsize, raw, rsize) in img.sections.items():
        if name == '.text':
            continue
        buf = img.data[raw:raw + min(vsize, rsize)]
        a = np.frombuffer(buf[:len(buf) // 8 * 8], dtype=np.uint64)
        res += [sva + int(i) * 8 for i in np.nonzero(a == target)[0]]
    return res


if __name__ == '__main__':
    img = Image(sys.argv[1]); cmd = sys.argv[2]; args = sys.argv[3:]
    if cmd == 'slots':
        vt = int(args[0], 16)
        for o in args[1:]:
            print('vt %s +%s = %s' % (hex(vt), o, hex(img.q(vt + int(o, 16)))))
        sys.exit(0)
    starts = vtable_starts(img)
    if cmd == 'of':
        for f in args:
            print('== %s' % f)
            for p in data_ptrs(img, int(f, 16)):
                i = bisect.bisect_right(starts, p) - 1
                if i >= 0:
                    print('   vt %s slot +%s' % (hex(starts[i]), hex(p - starts[i])))
    elif cmd == 'at':
        off, f = int(args[0], 16), int(args[1], 16)
        for s in starts:
            if img.q(s + off) == f:
                print('vt %s' % hex(s))
