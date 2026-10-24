#!/usr/bin/env python3
"""Harvest named addresses from the Serious Engine registration tables of a non-PIE x86-64 ELF
(Serious Sam Fusion 2017 Linux build).

Same 64-byte record as the SE4 PE builds (see harvest_symbols.py):
    +0x00 aux | +0x08 kind | +0x10 char* name | +0x18 char* help | +0x20 address | +0x28 count
    +0x30 char* type name (variables) / signature (functions) | +0x38 flags
Only addresses read from these records are emitted; nothing is inferred.
usage: elf_harvest.py <elf> [out.json]
"""
import json, re, struct, sys
from elf_vtables import Elf

IDENT = re.compile(r'^[A-Za-z_][A-Za-z0-9_:<>]*$')


def cstr(e, va, limit=512):
    if not isinstance(va, int) or va == 0:
        return None
    o = e.off(va)
    if o is None:
        return None
    end = e.data.find(b'\0', o, o + limit)
    if end < 0:
        return None
    try:
        s = e.data[o:end].decode('utf-8')
    except UnicodeDecodeError:
        return None
    return s if (s == '' or s.isprintable()) else None


def harvest(path):
    e = Elf(path)
    out = {'variables': {}, 'functions': {}}
    for n, a, sz, o, t in e.secs:
        if n not in ('.rodata', '.data.rel.ro', '.data') or t == 'SHT_NOBITS':
            continue
        blob = e.data[o:o + sz]
        for r in range(0, len(blob) - 64, 8):
            aux, kind, name_p, help_p, addr, count, typ, flags = struct.unpack_from('<8Q', blob, r)
            if kind > 16 or count > 0x10000:
                continue
            name = cstr(e, name_p, 128)
            if not name or not IDENT.match(name) or cstr(e, help_p, 4096) is None:
                continue
            sec = e.sec(addr)
            rec = '0x%x' % (a + r)
            if sec == '.text':
                out['functions'].setdefault(name, {'va': '0x%x' % addr, 'record': rec, 'kind': kind,
                                                   'count': count, 'signature': typ, 'flags': flags})
            elif sec in ('.data', '.bss'):
                tname = cstr(e, typ, 64)
                if tname is None or not IDENT.match(tname):
                    continue
                out['variables'].setdefault(name, {'va': '0x%x' % addr, 'record': rec, 'kind': kind,
                                                   'count': count, 'type': tname, 'flags': flags})
    return out


if __name__ == '__main__':
    res = harvest(sys.argv[1])
    json.dump(res, open(sys.argv[2], 'w') if len(sys.argv) > 2 else sys.stdout, indent=1, sort_keys=True)
