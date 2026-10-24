#!/usr/bin/env python3
"""List the functions (.pdata ranges) that reference each named cvar/global from a harvest JSON.

usage: cvar_readers.py <exe> <symbols.json> <name-or-0xVA> [...]
Prints per name: VA, number of code references, and each containing function (with harvested script
name when the function start is a registered script function) plus the reference sites inside it.
"""
import json, sys
from se4pe import Image
from riprefs import rip_refs
from pe_fninfo import Fns

if __name__ == '__main__':
    img = Image(sys.argv[1]); sym = json.load(open(sys.argv[2]))
    fnames = {int(v['va'], 16): k for k, v in sym['functions'].items()}
    vas = {}
    for n in sys.argv[3:]:
        if n.startswith('0x'):
            vas[n] = int(n, 16)
        elif n in sym['variables']:
            vas[n] = int(sym['variables'][n]['va'], 16)
        else:
            print('== %s: not in harvest' % n)
    fns = Fns(img)
    rr = rip_refs(img, list(set(vas.values())))
    for n, va in vas.items():
        hits = rr[va]
        print('== %s %s refs=%d' % (n, hex(va), len(hits)))
        by = {}
        for h in hits:
            s, _ = fns.containing(h)
            by.setdefault(s, []).append(h)
        for s, hs in sorted(by.items(), key=lambda x: x[0] or 0):
            print('   fn %s %-40s sites %s' % (hex(s) if s else '?', fnames.get(s, ''), ' '.join(hex(h) for h in hs)))
