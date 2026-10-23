#!/usr/bin/env python3
"""Locate Itanium-ABI vtables in a non-PIE ELF via typeinfo name strings."""
import struct, sys
from elftools.elf.elffile import ELFFile


class Elf:
    def __init__(self, path):
        self.f = open(path, 'rb'); self.data = open(path, 'rb').read()
        self.elf = ELFFile(self.f)
        self.secs = [(s.name, s['sh_addr'], s['sh_size'], s['sh_offset'], s['sh_type'])
                     for s in self.elf.iter_sections() if s['sh_addr']]

    def off(self, va):
        for n, a, sz, o, t in self.secs:
            if t != 'SHT_NOBITS' and a <= va < a + sz:
                return o + va - a

    def sec(self, va):
        for n, a, sz, o, t in self.secs:
            if a <= va < a + sz:
                return n

    def q(self, va):
        return struct.unpack_from('<Q', self.data, self.off(va))[0]

    def find_all(self, needle, secnames):
        res = []
        for n, a, sz, o, t in self.secs:
            if n not in secnames or t == 'SHT_NOBITS':
                continue
            blob = self.data[o:o + sz]; i = blob.find(needle)
            while i >= 0:
                if (o + i) % 8 == 0 or len(needle) != 8:
                    res.append(a + i)
                i = blob.find(needle, i + 1)
        return res


def vtable_for(e, cls_mangled):
    s = e.find_all(cls_mangled.encode() + b'\0', ['.rodata'])
    s = [x for x in s if e.data[e.off(x) - 1] == 0 or True]
    out = []
    for sva in s:
        for ref in e.find_all(struct.pack('<Q', sva), ['.data.rel.ro', '.data', '.rodata']):
            ti = ref - 8
            for vref in e.find_all(struct.pack('<Q', ti), ['.data.rel.ro', '.data', '.rodata']):
                if e.q(vref - 8) != 0:
                    continue
                fns = []; p = vref + 8
                while e.sec(e.q(p)) == '.text' or (e.q(p) == 0 and len(fns) < 2):
                    fns.append(e.q(p)); p += 8
                out.append({'typeinfo': ti, 'vtable': vref + 8, 'fns': fns})
    return out


if __name__ == '__main__':
    e = Elf(sys.argv[1])
    for cls in sys.argv[2:]:
        for v in vtable_for(e, cls):
            print(cls, 'typeinfo', hex(v['typeinfo']), 'vtable', hex(v['vtable']), 'slots', len(v['fns']))
