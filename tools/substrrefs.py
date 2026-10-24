#!/usr/bin/env python3
"""Find C strings that CONTAIN a substring (e.g. log messages ending in "\\n" or with format arguments)
in an SE4 PE, and every code/data reference to the start of each string. Read-only.

Complements strrefs.py (exact strings). Prints each matching string, its VA, RIP-relative code references
(containing .pdata function and harvested name) and absolute 8-byte pointers in data sections.
usage: substrrefs.py <exe> <symbols.json> [--max N] <substring> [<substring> ...]
"""
import json
import sys
from se4pe import Image
from riprefs import rip_refs
from pe_fninfo import Fns
from strrefs import abs_ptrs


def find_containing(img, sub, limit=64):
    pat = sub.encode()
    out, o = [], img.data.find(pat)
    while o >= 0 and len(out) < limit:
        s = o
        while s > 0 and 0x20 <= img.data[s - 1] < 0x7f or (s > 0 and img.data[s - 1] in (9, 10, 13)):
            s -= 1
        e = img.data.find(b'\0', o)
        va = img.va(s)
        if va and va not in [v for v, _ in out]:
            out.append((va, img.data[s:e].decode('latin-1')))
        o = img.data.find(pat, o + 1)
    return out


if __name__ == '__main__':
    img = Image(sys.argv[1]); sym = json.load(open(sys.argv[2]))
    args = sys.argv[3:]; mx = 64
    if args and args[0] == '--max':
        mx = int(args[1]); args = args[2:]
    fnames = {int(v['va'], 16): k for k, v in sym['functions'].items()}
    fns = Fns(img)
    for sub in args:
        hits = find_containing(img, sub, mx)
        print('== %r: %d string(s)' % (sub, len(hits)))
        vas = [v for v, _ in hits]
        rr = rip_refs(img, vas) if vas else {}
        ap = abs_ptrs(img, vas) if vas else {}
        for v, s in hits:
            print('  %s %r' % (hex(v), s[:120]))
            for h in rr[v]:
                fs, fe = fns.containing(h)
                print('    code %s in fn %s %s' % (hex(h), hex(fs) if fs else '?', fnames.get(fs, '')))
            for p in ap[v]:
                print('    data %s' % hex(p))
