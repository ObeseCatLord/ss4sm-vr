#!/usr/bin/env python3
"""callers.py for the Fusion 2017 Linux ELF: E8/E9 rel32 sites targeting given VAs, with the containing
function from .eh_frame FDEs, plus 8-byte absolute pointers in data sections (vtables).
usage: elf_callers.py <elf> <va> [<va> ...]
"""
import bisect, sys
import numpy as np
from elf_vtables import Elf
from elf_slot_dump import fde_ranges

if __name__ == '__main__':
    e = Elf(sys.argv[1])
    fr = sorted(fde_ranges(e).items())  # {start: end}
    starts = [a for a, b in fr]
    text = next(s for s in e.secs if s[0] == '.text')
    _, sva, size, off, _ = text
    a = np.frombuffer(e.data[off:off + size], dtype=np.uint8)
    n = len(a) - 5
    rel = (a[1:n+1].astype(np.int64) | (a[2:n+2].astype(np.int64) << 8) |
           (a[3:n+3].astype(np.int64) << 16) | (a[4:n+4].astype(np.int64) << 24))
    rel = np.where(rel >= 2**31, rel - 2**32, rel)
    tgt = np.arange(n, dtype=np.int64) + sva + 5 + rel
    op = a[:n]
    for t in [int(x, 16) for x in sys.argv[2:]]:
        idx = np.nonzero((tgt == t) & ((op == 0xE8) | (op == 0xE9)))[0]
        print('== %s: %d direct' % (hex(t), len(idx)))
        for i in idx:
            site = sva + int(i); k = bisect.bisect_right(starts, site) - 1
            fn = fr[k][0] if k >= 0 and site < fr[k][1] else None
            print('   %s %s in fn %s' % ('call' if a[i] == 0xE8 else 'jmp ', hex(site), hex(fn) if fn else '?'))
        for name, sa, sz, so, st in e.secs:
            if name in ('.text',) or st == 'SHT_NOBITS':
                continue
            buf = e.data[so:so + sz]; m = len(buf) // 8 * 8
            q = np.frombuffer(buf[:m], dtype=np.uint64)
            for j in np.nonzero(q == t)[0][:8]:
                print('   ptr  %s (%s)' % (hex(sa + int(j) * 8), name))
