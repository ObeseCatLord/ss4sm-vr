#!/usr/bin/env python3
"""Resolve SE4 player/system command handle globals from their static registration stubs, and list the
functions that reference each handle.
A registration stub is `lea rdx,[rip+name]; lea rcx,[rip+handle]; jmp register` (Sam4 616154: 0x140014250).
usage: plcmd_handles.py <exe> <symbols.json> [--no-readers] <command-name> [...]
"""
import json
import sys
import capstone
from se4pe import Image
from strrefs import find_strings
from riprefs import rip_refs
from pe_fninfo import Fns


def handle_of(img, md, name):
    out = []
    for sva in find_strings(img, name):
        for site in rip_refs(img, [sva], 0)[sva]:
            st = site - 3  # lea rdx,[rip+disp32] = 48 8d 15 <disp32>
            o = img.off(st)
            ins = list(md.disasm(img.data[o:o + 32], st))
            if len(ins) >= 3 and ins[0].mnemonic == 'lea' and ins[1].mnemonic == 'lea' \
                    and ins[1].op_str.startswith('rcx, [rip') and ins[2].mnemonic == 'jmp':
                disp = int(ins[1].op_str.split('+ ')[1].rstrip(']'), 16)
                out.append((ins[1].address + ins[1].size + disp, int(ins[2].op_str, 16), st))
    return out


if __name__ == '__main__':
    img = Image(sys.argv[1]); sym = json.load(open(sys.argv[2]))
    args = sys.argv[3:]
    readers = True
    if args and args[0] == '--no-readers':
        readers = False; args = args[1:]
    fnames = {int(v['va'], 16): k for k, v in sym['functions'].items()}
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
    fns = Fns(img)
    res = {n: handle_of(img, md, n) for n in args}
    allh = sorted({h for v in res.values() for h, _, _ in v})
    rr = rip_refs(img, allh) if readers else {}
    for n, hs in res.items():
        if not hs:
            print('== %-36s no registration stub' % n); continue
        for h, reg, st in hs:
            print('== %-36s handle %s (stub %s, register %s)' % (n, hex(h), hex(st), hex(reg)))
            if not readers:
                continue
            by = {}
            for site in rr[h]:
                s, _ = fns.containing(site)
                by.setdefault(s, []).append(site)
            for s, sites in sorted(by.items(), key=lambda x: x[0] or 0):
                print('     fn %s %-40s %s' % (hex(s) if s else '?', fnames.get(s, ''), ' '.join(hex(x) for x in sites)))
