#!/usr/bin/env python3
"""Find exact C strings in an SE4 PE and every reference to them.

For each string: its VA(s), RIP-relative code references (with the containing .pdata function and its
harvested name), and absolute 8-byte pointers to it in data sections (with the neighbouring qwords, which
is how SE4 reflection/meta records reference member and class names).
usage: strrefs.py <exe> <symbols.json> [--ctx N] <string> [<string> ...]
"""
import json, struct, sys
import numpy as np
from se4pe import Image
from riprefs import rip_refs
from pe_fninfo import Fns


def find_strings(img, s):
    pat = s.encode() + b'\0'
    out, o = [], img.data.find(pat)
    while o >= 0:
        if o == 0 or img.data[o - 1] == 0:
            va = img.va(o)
            if va: out.append(va)
        o = img.data.find(pat, o + 1)
    return out


def abs_ptrs(img, targets):
    res = {t: [] for t in targets}
    for name, (sva, vsize, raw, rsize) in img.sections.items():
        if name == '.text':
            continue
        buf = img.data[raw:raw + min(vsize, rsize)]
        n = len(buf) // 8 * 8
        a = np.frombuffer(buf[:n], dtype=np.uint64)
        for t in targets:
            for i in np.nonzero(a == t)[0]:
                res[t].append(sva + int(i) * 8)
        # unaligned pointers are not expected for compiler-emitted tables
    return res


if __name__ == '__main__':
    img = Image(sys.argv[1]); sym = json.load(open(sys.argv[2]))
    args = sys.argv[3:]; ctx = 0
    if args and args[0] == '--ctx':
        ctx = int(args[1]); args = args[2:]
    fnames = {int(v['va'], 16): k for k, v in sym['functions'].items()}
    fns = Fns(img)
    for s in args:
        vas = find_strings(img, s)
        print('== %r at %s' % (s, ' '.join(hex(v) for v in vas) or 'NOT FOUND'))
        if not vas:
            continue
        rr = rip_refs(img, vas)
        ap = abs_ptrs(img, vas)
        for v in vas:
            for h in rr[v]:
                fs, fe = fns.containing(h)
                print('  code %s in fn %s %s' % (hex(h), hex(fs) if fs else '?', fnames.get(fs, '')))
            for p in ap[v]:
                q = [img.q(p + 8 * k) for k in range(-ctx, ctx + 1)]
                print('  data %s [%s]' % (hex(p), ' '.join('%x' % (x or 0) for x in q)))
