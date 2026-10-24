#!/usr/bin/env python3
"""Port a list of Sam4.exe function VAs to other SE4 builds by the shortest unique wildcarded signature
(sigscan.signature: rel32 call/jump targets and RIP displacements wildcarded), trying 24..256 bytes.
usage: port_list.py <src.exe> <target.exe> <va>[=name] [...]
Prints the target VA or 'not unique'/'not found'. Read-only.
"""
import sys
from se4pe import Image
from sigscan import signature, find

if __name__ == '__main__':
    src, dst = Image(sys.argv[1]), Image(sys.argv[2])
    for a in sys.argv[3:]:
        va, _, name = a.partition('=')
        va = int(va, 16); res = None
        for ln in (24, 32, 48, 64, 96, 128, 192, 256):
            code, mask = signature(src, va, ln)
            hits = find(dst, code, mask)
            if len(hits) == 1:
                # confirm with a longer signature (up to 160 bytes) that still matches only there
                code2, mask2 = signature(src, va, max(ln, 160))
                h2 = find(dst, code2, mask2)
                res = (hex(hits[0]) + ('' if h2 == hits else ' [longer sig: %s]' % (' '.join(map(hex, h2)) or 'no match')), ln); break
            if not hits:
                res = ('not found', ln); break
        print('%-14s %-40s -> %s' % (hex(va), name, '%s (sig %d bytes)' % res if res else 'not unique at 256'))
