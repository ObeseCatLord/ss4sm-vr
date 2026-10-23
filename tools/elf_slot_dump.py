#!/usr/bin/env python3
"""Dump each vtable slot body of a class in a non-PIE ELF: size, strings referenced, calls, cvar loads."""
import bisect, struct, sys
import capstone
from capstone.x86 import X86_OP_MEM, X86_OP_IMM, X86_REG_RIP
from elf_vtables import Elf, vtable_for


def fde_ranges(e):
    di = e.elf.get_dwarf_info()
    r = {}
    for ent in di.EH_CFI_entries():
        h = getattr(ent, 'header', None)
        if h is not None and 'initial_location' in h:
            r[h['initial_location']] = h['initial_location'] + h['address_range']
    return r


def strref(e, va):
    if e.sec(va) != '.rodata':
        return None
    o = e.off(va); end = e.data.find(b'\0', o, o + 200)
    if end <= o:
        return None
    try:
        s = e.data[o:end].decode()
    except UnicodeDecodeError:
        return None
    return s if s.isprintable() and len(s) >= 3 else None


def describe(e, md, ranges, fn, maxlen=0x1200):
    end = ranges.get(fn, fn + 0x200)
    body = e.data[e.off(fn):e.off(fn) + min(end - fn, maxlen)]
    strs, calls, vcalls, n = [], [], [], 0
    for ins in md.disasm(body, fn):
        n += 1
        for op in ins.operands:
            tgt = None
            if op.type == X86_OP_MEM and op.mem.base == X86_REG_RIP:
                tgt = ins.address + ins.size + op.mem.disp
            elif op.type == X86_OP_IMM and 0x2800000 < op.imm < 0x3200000:
                tgt = op.imm
            if tgt:
                s = strref(e, tgt)
                if s and s not in strs:
                    strs.append(s)
            if ins.mnemonic == 'call' and op.type == X86_OP_MEM and op.mem.base != X86_REG_RIP:
                vcalls.append(op.mem.disp)
            if ins.mnemonic == 'call' and op.type == X86_OP_IMM:
                calls.append(op.imm)
    return {'size': end - fn, 'insns': n, 'strings': strs, 'vcalls': vcalls, 'calls': len(calls)}


if __name__ == '__main__':
    e = Elf(sys.argv[1]); cls = sys.argv[2]
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64); md.detail = True
    ranges = fde_ranges(e)
    base = vtable_for(e, sys.argv[3])[0]['fns'] if len(sys.argv) > 3 else None
    for v in vtable_for(e, cls):
        for i, fn in enumerate(v['fns']):
            d = describe(e, md, ranges, fn) if fn else {}
            inh = ' (inherited)' if base and i < len(base) and base[i] == fn else ''
            print('#%02d +0x%03x %s%s size=%s ins=%s vcalls=%s' % (i, i * 8, hex(fn), inh, d.get('size'), d.get('insns'),
                  [hex(x) for x in d.get('vcalls', [])][:8]))
            for s in d.get('strings', [])[:6]:
                print('        "%s"' % s[:110])
