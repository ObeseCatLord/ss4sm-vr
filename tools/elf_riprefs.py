#!/usr/bin/env python3
"""cvar_readers.py for the Fusion 2017 Linux ELF: RIP-relative references in .text to given VAs (or harvested
variable names from a symbols JSON), with the containing function from .eh_frame FDEs.
usage: elf_riprefs.py <elf> [--sym symbols.json] <name-or-0xVA> [...]
Matches disp32 followed by 0..4 immediate bytes, so a neighbouring global can show up; check in Ghidra.
"""
import bisect, json, sys
import numpy as np
from elf_vtables import Elf
from elf_slot_dump import fde_ranges

if __name__ == '__main__':
    e = Elf(sys.argv[1]); args = sys.argv[2:]; sym = {}
    if args and args[0] == '--sym':
        sym = json.load(open(args[1])).get('variables', {}); args = args[2:]
    fr = sorted(fde_ranges(e).items()); starts = [a for a, b in fr]
    _, sva, size, off, _ = next(s for s in e.secs if s[0] == '.text')
    a = np.frombuffer(e.data[off:off + size], dtype=np.uint8)
    n = len(a) - 4
    disp = (a[0:n].astype(np.int64) | (a[1:n+1].astype(np.int64) << 8) |
            (a[2:n+2].astype(np.int64) << 16) | (a[3:n+3].astype(np.int64) << 24))
    disp = np.where(disp >= 2**31, disp - 2**32, disp)
    pos = np.arange(n, dtype=np.int64) + sva
    for x in args:
        t = int(x, 16) if x.startswith('0x') else int(sym[x]['va'], 16)
        hits = set()
        for k in range(5):
            hits |= {int(sva + i) for i in np.nonzero(pos + 4 + k + disp == t)[0]}
        print('== %s %s: %d refs' % (x, hex(t), len(hits)))
        for h in sorted(hits):
            i = bisect.bisect_right(starts, h) - 1
            fn = hex(fr[i][0]) if i >= 0 and h < fr[i][1] else '?'
            print('   site %s in fn %s' % (hex(h), fn))
