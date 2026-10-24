#!/usr/bin/env python3
"""Direct callers of functions in an SE4 PE: every E8/E9 rel32 in .text whose target is a given VA,
plus 8-byte absolute pointers to it in data sections (vtables, registration records).
Byte-pattern scan: an E8/E9 inside another instruction can false-positive; containing functions from
.pdata are printed so hits are easy to check in Ghidra.
usage: callers.py <exe> <symbols.json> <va> [<va> ...]
"""
import json, sys
import numpy as np
from se4pe import Image
from pe_fninfo import Fns
from strrefs import abs_ptrs

if __name__ == '__main__':
    img = Image(sys.argv[1]); sym = json.load(open(sys.argv[2]))
    fnames = {int(v['va'], 16): k for k, v in sym['functions'].items()}
    fns = Fns(img)
    sva, text = img.section_bytes('.text')
    a = np.frombuffer(text, dtype=np.uint8)
    n = len(a) - 5
    op = a[:n]
    rel = (a[1:n+1].astype(np.int64) | (a[2:n+2].astype(np.int64) << 8) |
           (a[3:n+3].astype(np.int64) << 16) | (a[4:n+4].astype(np.int64) << 24))
    rel = np.where(rel >= 2**31, rel - 2**32, rel)
    tgt = np.arange(n, dtype=np.int64) + sva + 5 + rel
    targets = [int(x, 16) for x in sys.argv[3:]]
    ap = abs_ptrs(img, targets)
    for t in targets:
        idx = np.nonzero((tgt == t) & ((op == 0xE8) | (op == 0xE9)))[0]
        print('== %s %s: %d direct' % (hex(t), fnames.get(t, ''), len(idx)))
        for i in idx:
            site = sva + int(i); s, _ = fns.containing(site)
            print('   %s %s in fn %s %s' % ('call' if a[i] == 0xE8 else 'jmp ', hex(site), hex(s) if s else '?', fnames.get(s, '')))
        for p in ap[t][:8]:
            print('   ptr  %s (%s)' % (hex(p), img.section_of(p)))
        if len(ap[t]) > 8:
            print('   ... %d data pointers in total' % len(ap[t]))
