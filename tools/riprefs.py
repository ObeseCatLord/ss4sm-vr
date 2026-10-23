#!/usr/bin/env python3
"""Find RIP-relative references in .text to given absolute addresses (disp32 followed by 0..4 immediate bytes)."""
import sys
import numpy as np
from se4pe import Image


def rip_refs(img, targets, imm_max=4):
    sva, text = img.section_bytes('.text')
    a = np.frombuffer(text, dtype=np.uint8)
    n = len(a) - 4
    disp = (a[0:n].astype(np.int64) | (a[1:n+1].astype(np.int64) << 8) |
            (a[2:n+2].astype(np.int64) << 16) | (a[3:n+3].astype(np.int64) << 24))
    disp = np.where(disp >= 2**31, disp - 2**32, disp)
    pos = np.arange(n, dtype=np.int64) + sva
    res = {}
    for t in targets:
        hits = []
        for k in range(imm_max + 1):
            idx = np.nonzero(pos + 4 + k + disp == t)[0]
            hits += [int(sva + i) for i in idx]
        res[t] = sorted(set(hits))
    return res


if __name__ == '__main__':
    img = Image(sys.argv[1])
    for t, hits in rip_refs(img, [int(x, 16) for x in sys.argv[2:]]).items():
        print(hex(t), len(hits), ' '.join(hex(h) for h in hits[:400]))
