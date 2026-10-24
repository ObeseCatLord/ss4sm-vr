#!/usr/bin/env python3
"""Name static CName/identifier globals in an SE4 PE: finds the static initialiser
`lea rdx,[rip+str]; lea rcx,[rip+global]; jmp <ctor>` and prints the string (e.g. ray-cast categories
such as "camera_aim_ray" passed to the native ray cast).
usage: cname_globals.py <exe> <0xVA> [...]
"""
import struct, sys
from se4pe import Image
from riprefs import rip_refs

if __name__ == '__main__':
    img = Image(sys.argv[1]); sva, text = img.section_bytes('.text')
    ts = [int(x, 16) for x in sys.argv[2:]]
    rr = rip_refs(img, ts, 0)
    for t in ts:
        out = []
        for site in rr[t]:                       # site = address of the disp32 of `lea rcx,[rip+t]`
            i = site - sva
            if text[i - 10:i - 7] == b'\x48\x8d\x15' and text[i - 3:i] == b'\x48\x8d\x0d':
                s = site - 7
                out.append(img.cstr(s + 4 + struct.unpack_from('<i', text, i - 7)[0]))
        print(hex(t), out)
