#!/usr/bin/env python3
"""Check candidate ports of functions between two SE4 builds by comparing instruction shapes.

For each pair <src_va>:<dst_va>[=name] the tool checks that both VAs are .pdata function starts, decodes
the first .pdata range of both functions (MSVC splits some functions into chained fragments, so a short
size means only the first fragment was compared; at most 4000 instructions), and compares the mnemonic sequences
(difflib ratio, 1.0 = identical) and the sequences of small memory displacements ([reg+disp], disp < 0x10000,
so struct field offsets show up; differences there are listed). Use it to confirm a port found by call order or
by a delta from a neighbouring ported function when sigscan/port_list says 'not found' (typically because a
player field offset moved by 8 in SamSM). Read-only.
usage: port_check.py <src.exe> <dst.exe> <src_va>:<dst_va>[=name] [...]
"""
import difflib
import sys
import capstone
from capstone.x86 import X86_OP_MEM, X86_REG_RIP, X86_REG_RSP
from se4pe import Image
from pe_fninfo import Fns


def shape(img, fns, va):
    s, e = fns.containing(va)
    if s != va:
        return None, None, None
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64); md.detail = True
    sva, text = img.section_bytes('.text')
    mn, disp = [], []
    for n, ins in enumerate(md.disasm(text[s - sva:e - sva], s)):
        if n >= 4000:
            break
        mn.append(ins.mnemonic)
        for op in ins.operands:
            if op.type == X86_OP_MEM and op.mem.base not in (X86_REG_RIP, X86_REG_RSP, 0) \
                    and 0 < op.mem.disp < 0x10000:
                disp.append(op.mem.disp)
    return mn, disp, e - s


if __name__ == '__main__':
    a, b = Image(sys.argv[1]), Image(sys.argv[2])
    fa, fb = Fns(a), Fns(b)
    for arg in sys.argv[3:]:
        pair, _, name = arg.partition('=')
        sv, dv = (int(x, 16) for x in pair.split(':'))
        ma, da, la = shape(a, fa, sv)
        mb, db, lb = shape(b, fb, dv)
        if ma is None or mb is None:
            print('%-28s %s -> %s  not a .pdata function start (%s)' % (
                name, hex(sv), hex(dv), 'src' if ma is None else 'dst'))
            continue
        r = difflib.SequenceMatcher(None, ma, mb, autojunk=False).ratio()
        diffs = []
        for op, i1, i2, j1, j2 in difflib.SequenceMatcher(None, da, db, autojunk=False).get_opcodes():
            if op != 'equal':
                diffs.append('%s->%s' % ('/'.join(hex(x) for x in da[i1:i2]) or '-',
                                         '/'.join(hex(x) for x in db[j1:j2]) or '-'))
        print('%-28s %s -> %s  size %#x/%#x  mnemonic ratio %.3f  disp diffs: %s' % (
            name, hex(sv), hex(dv), la, lb, r, ', '.join(diffs[:8]) + (' ...' if len(diffs) > 8 else '') or 'none'))
