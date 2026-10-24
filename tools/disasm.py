#!/usr/bin/env python3
"""Disassemble a VA range of an SE4 PE (capstone), annotating rip-relative targets with harvested names
and strings. Read-only.
usage: disasm.py <exe> <symbols.json> <va> [<count>=40]
"""
import json
import sys
import capstone
from se4pe import Image


def main():
    img = Image(sys.argv[1]); sym = json.load(open(sys.argv[2]))
    names = {int(v['va'], 16): k for k, v in sym['functions'].items()}
    names.update({int(v['va'], 16) if isinstance(v, dict) else int(v, 16): k
                  for k, v in sym.get('variables', {}).items() if (isinstance(v, dict) and 'va' in v) or isinstance(v, str)})
    va = int(sys.argv[3], 16); n = int(sys.argv[4]) if len(sys.argv) > 4 else 40
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64); md.detail = True
    o = img.off(va)
    for i, ins in enumerate(md.disasm(img.data[o:o + 16 * n], va)):
        if i >= n:
            break
        note = ''
        tgt = None
        if ins.mnemonic in ('call', 'jmp') or ins.mnemonic.startswith('j'):
            try:
                tgt = int(ins.op_str, 16)
            except ValueError:
                pass
        if 'rip' in ins.op_str:
            for op in ins.operands:
                if op.type == capstone.x86.X86_OP_MEM and op.mem.base == capstone.x86.X86_REG_RIP:
                    tgt = ins.address + ins.size + op.mem.disp
        if tgt is not None:
            note = names.get(tgt, '')
            s = img.cstr(tgt, 120)
            if s:
                note += f' "{s}"'
            if tgt != int(ins.op_str, 16) if ins.op_str.startswith('0x') else True:
                note = f'[{tgt:#x}] ' + note
        print(f'{ins.address:#x}: {ins.mnemonic:6s} {ins.op_str:45s} {note}')


if __name__ == '__main__':
    main()
