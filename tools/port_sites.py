#!/usr/bin/env python3
"""Port instruction sites (and the globals / call targets they reference) from one SE4 build to another.

For each source instruction VA, build a wildcarded signature (sigscan.signature: rel32 call/jump targets and
RIP displacements wildcarded) of a window that starts at or a little before the site, try 24..192 bytes,
and accept the shortest window that matches exactly once in the target .text. The target site is the match
plus the same back-offset. The instruction there is decoded; if it has a RIP-relative memory operand, the
referenced target VA is printed (a global/cvar/vtable), and for call/jmp rel32 the call target is printed.
The source instruction is decoded the same way; "MISMATCH" is printed when the mnemonics differ or, for a
window that started before the site, when the 32 masked bytes from the site itself differ.
usage: port_sites.py <src.exe> <target.exe> <site-va>[=name][@disp] [...]
Use a function start as the site to port a function (prints the target function VA). Append "@disp" to the
name when the VA is a displacement position as printed by cvar_readers.py/riprefs.py. Read-only.
"""
import sys
import capstone
from capstone.x86 import X86_OP_MEM, X86_OP_IMM, X86_REG_RIP
from se4pe import Image
from sigscan import signature, find

MD = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
MD.detail = True


def decode(img, va):
    o = img.off(va)
    if o is None:
        return None, None
    ins = next(MD.disasm(img.data[o:o + 16], va), None)
    if ins is None:
        return None, None
    ref = None
    for op in ins.operands:
        if op.type == X86_OP_MEM and op.mem.base == X86_REG_RIP:
            ref = ins.address + ins.size + op.mem.disp
        elif op.type == X86_OP_IMM and ins.mnemonic.startswith(('call', 'j')):
            ref = op.imm
    return ins, ref


def insn_start(img, va):
    """riprefs/cvar_readers report the displacement position; map it back to the instruction start."""
    best = va
    for back in range(1, 9):  # keep the longest decode (REX/prefix bytes included) that puts disp at va
        o = img.off(va - back)
        ins = next(MD.disasm(img.data[o:o + 16], va - back), None) if o is not None else None
        if ins is not None and ins.disp_offset == back and any(
                op.type == X86_OP_MEM and op.mem.base == X86_REG_RIP for op in ins.operands):
            best = va - back
    return best


def port_site(src, dst, site):
    for back in (0, 8, 16, 32, 48):
        start = site - back
        for ln in (24, 32, 48, 64, 96, 128, 192):
            if ln <= back + 4:
                continue
            code, mask = signature(src, start, ln)
            hits = find(dst, code, mask)
            if len(hits) == 1:
                return hits[0] + back, back, ln
            if not hits:
                break
    return None, None, None


if __name__ == '__main__':
    src, dst = Image(sys.argv[1]), Image(sys.argv[2])
    for a in sys.argv[3:]:
        va, _, name = a.partition('=')
        va = int(va, 16)
        if name.endswith('@disp'):  # site is a displacement position (cvar_readers/riprefs output)
            name = name[:-5]
            va = insn_start(src, va)
        s_ins, s_ref = decode(src, va)
        t, back, ln = port_site(src, dst, va)
        if t is None:
            print('%-14s %-34s -> not unique / not found' % (hex(va), name))
            continue
        t_ins, t_ref = decode(dst, t)
        same = s_ins is not None and t_ins is not None and s_ins.mnemonic == t_ins.mnemonic
        # when the unique window started before the site, confirm the 32 bytes from the site itself agree too
        if back:
            code, mask = signature(src, va, 32)
            o = dst.off(t)
            body = dst.data[o:o + len(code)] if o is not None else b''
            if len(body) != len(code) or any(m and b != c for b, c, m in zip(body, code, mask)):
                same = False
        print('%-14s %-34s -> %-14s (sig %d, back %d)%s  %s %s%s' % (
            hex(va), name, hex(t), ln, back, '' if same else ' MISMATCH',
            t_ins.mnemonic if t_ins else '?', t_ins.op_str if t_ins else '',
            ('  ref %s -> %s' % (hex(s_ref), hex(t_ref))) if s_ref is not None and t_ref is not None else ''))
