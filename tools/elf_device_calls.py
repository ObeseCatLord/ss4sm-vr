#!/usr/bin/env python3
"""device_calls.py for the Fusion 2017 Linux ELF (SysV ABI: this in rdi, functions from .eh_frame FDEs).

usage: elf_device_calls.py <elf> <fusion_symbols.json> <global_va> <out.json>
Output has the same shape as device_calls.py, so Fusion and SS4 callers of same-named script
functions can be compared slot by slot (Fusion offset - 8 == SS4 offset).
"""
import bisect, json, sys
import capstone
from elf_vtables import Elf
from elf_slot_dump import fde_ranges
from riprefs import rip_refs
from device_calls import analyse

SYSV_VOLATILE = {'rax', 'rcx', 'rdx', 'rsi', 'rdi', 'r8', 'r9', 'r10', 'r11'}
SYSV_ARGS = ['rsi', 'rdx', 'rcx', 'r8', 'r9', 'xmm0', 'xmm1', 'xmm2']


class ElfImg:
    def __init__(self, e):
        self.e = e

    def section_bytes(self, name):
        for n, a, sz, o, t in self.e.secs:
            if n == name:
                return a, self.e.data[o:o + sz]


def main(path, symjson, g, out):
    e = Elf(path)
    img = ElfImg(e)
    names = {int(v['va'], 16): k for k, v in json.load(open(symjson))['functions'].items()}
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64); md.detail = True
    rng = fde_ranges(e)
    starts = sorted(rng)
    sva, text = img.section_bytes('.text')
    refs = rip_refs(img, [g])[g]
    fns = set()
    for r in refs:
        i = bisect.bisect_right(starts, r) - 1
        if i >= 0 and r < rng[starts[i]]:
            fns.add((starts[i], rng[starts[i]]))
    res = {'global': hex(g), 'functions': {}, 'slots': {}, 'fields': {}}
    for fs, fe in sorted(fns):
        calls, fields, passes = analyse(img, md, fs, fe, g, sva, text, names.get(fs),
                                        volatile=SYSV_VOLATILE, this='rdi', argregs=SYSV_ARGS)
        res['functions'][hex(fs)] = {'end': hex(fe), 'name': names.get(fs), 'ncalls': len(calls),
                                     'nfields': len(fields), 'passes_device_in_rdi': passes}
        for c in calls:
            res['slots'].setdefault(c['slot'], []).append(c)
        for f in fields:
            f['fn'] = hex(fs); f['name'] = names.get(fs)
            res['fields'].setdefault(f['disp'], []).append(f)
    res['slots'] = dict(sorted(res['slots'].items(), key=lambda kv: int(kv[0], 16)))
    res['fields'] = dict(sorted(res['fields'].items(), key=lambda kv: int(kv[0], 16)))
    json.dump(res, open(out, 'w'), indent=1)
    print('%d functions reference %s' % (len(fns), hex(g)))
    for k, v in res['slots'].items():
        th = sum(1 for c in v if c['this_is_device'])
        nm = sorted({c['name'] for c in v if c['name']})
        print('slot %6s (ss4 %6s): %3d calls (%d this=device) %s' % (k, hex(int(k, 16) - 8), len(v), th,
                                                                      ', '.join(nm)[:150]))
    for k, v in res['fields'].items():
        print('field %6s: %3d accesses (%d writes) in %s' % (k, len(v), sum(f['write'] for f in v),
                                                          ', '.join(sorted({f['fn'] for f in v}))[:150]))


if __name__ == '__main__':
    main(sys.argv[1], sys.argv[2], int(sys.argv[3], 16), sys.argv[4])
