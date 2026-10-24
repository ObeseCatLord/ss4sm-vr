#!/usr/bin/env python3
"""Port a function address from one SE4 build to others by a wildcarded byte signature.

The signature is the first <len> bytes of the source function with every RIP-relative displacement
and rel32 call/jump target wildcarded (capstone decides which bytes those are). A port is accepted only
if the signature matches exactly once in the target's .text.
usage: sigscan.py <src.exe> <src_va> <len> <target.exe> [<target.exe> ...]
"""
import re, sys
import capstone
from capstone.x86 import X86_OP_MEM, X86_OP_IMM, X86_REG_RIP
from se4pe import Image


def signature(img, va, length):
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64); md.detail = True
    o = img.off(va); code = img.data[o:o + length]
    mask = [True] * len(code)
    for ins in md.disasm(code, va):
        start = ins.address - va
        wild = ins.mnemonic.startswith(('call', 'j')) and any(op.type == X86_OP_IMM for op in ins.operands) and ins.size >= 5
        if wild:
            for i in range(ins.size - 4, ins.size): mask[start + i] = False
        if any(op.type == X86_OP_MEM and op.mem.base == X86_REG_RIP for op in ins.operands):
            d = ins.disp_offset
            for i in range(d, d + 4): mask[start + i] = False
    return code, mask


def find(img, code, mask):
    pat = b''.join(re.escape(bytes([b])) if m else b'.' for b, m in zip(code, mask))
    sva, text = img.section_bytes('.text')
    return [sva + m.start() for m in re.finditer(pat, text, re.S)]


if __name__ == '__main__':
    src = Image(sys.argv[1]); va = int(sys.argv[2], 16); n = int(sys.argv[3], 0)
    code, mask = signature(src, va, n)
    self_hits = find(src, code, mask)
    print('source', hex(va), 'self-matches', [hex(x) for x in self_hits])
    for t in sys.argv[4:]:
        hits = find(Image(t), code, mask)
        print(t.rsplit('/', 1)[-1], 'UNIQUE ' + hex(hits[0]) if len(hits) == 1 else 'AMBIGUOUS/NONE ' + str([hex(h) for h in hits[:5]]))
