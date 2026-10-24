#!/usr/bin/env python3
"""Harvest named addresses from Serious Engine 4 registration tables.

Each console variable and script function is registered by a 64-byte record in .rdata:
    +0x00 u64 0 | pointer into the image (callback or default value)
    +0x08 u64 kind
    +0x10 char* name
    +0x18 char* help ("" when absent)
    +0x20 void* address   (.data/.bss for variables, .text for functions)
    +0x28 u64 count
    +0x30 char* type name (variables) | u64 signature (functions)
    +0x38 u64 flags
Only addresses read from these records are emitted; nothing is inferred.
"""
import json, re, struct, sys
from se4pe import Image
from fingerprint import fingerprint

IDENT = re.compile(r'^[A-Za-z_][A-Za-z0-9_:<>]*$')


def harvest(path):
    img = Image(path)
    rva, rdata = img.section_bytes('.rdata')
    out = {'variables': {}, 'functions': {}}
    for o in range(0, len(rdata) - 64, 8):
        zero, kind, name_p, help_p, addr, count, typ, flags = struct.unpack_from('<8Q', rdata, o)
        if kind > 16 or count > 0x10000:
            continue
        # +0x00 normally holds 0 or an image pointer, but string bytes have been seen there (SamSM
        # vr_bForceImmersiveMode); such records are accepted only as variables, whose name, help, address
        # and type name are all validated below.
        odd_aux = zero != 0 and img.section_of(zero) is None
        name = img.cstr(name_p, 128)
        if not name or not IDENT.match(name):
            continue
        if img.cstr(help_p, 4096) is None:
            continue
        sec = img.section_of(addr)
        rec = '0x%x' % (rva + o)
        if sec == '.text' and not odd_aux:
            out['functions'].setdefault(name, {'va': '0x%x' % addr, 'record': rec, 'kind': kind,
                                               'count': count, 'signature': typ, 'flags': flags})
        elif sec in ('.data', '.bss') and (not odd_aux or kind == 2):
            tname = img.cstr(typ, 64)
            if tname is None or not IDENT.match(tname):
                continue
            out['variables'].setdefault(name, {'va': '0x%x' % addr, 'record': rec, 'kind': kind,
                                               'count': count, 'type': tname, 'flags': flags,
                                               'aux': '0x%x' % zero})
    return out


if __name__ == '__main__':
    exe = sys.argv[1]
    result = {'fingerprint': fingerprint(exe), **harvest(exe)}
    json.dump(result, open(sys.argv[2], 'w') if len(sys.argv) > 2 else sys.stdout, indent=1, sort_keys=True)
