#!/usr/bin/env python3
"""Map vtable slots called on the engine's global VR device pointer.

For every instruction that loads the global into a register, follow the register through a short forward
window: `mov vt, [dev]` then `call [vt+slot]`. Each hit records the containing function (from .pdata) and the
harvested script name of that function when it is one.
"""
import bisect, json, struct, sys
import capstone
from capstone.x86 import X86_OP_MEM, X86_OP_REG, X86_REG_RIP
from se4pe import Image


def pdata_functions(img):
    sva, raw = img.section_bytes('.pdata')
    starts, ends = [], []
    for o in range(0, len(raw) - 12, 12):
        b, e, _ = struct.unpack_from('<III', raw, o)
        if b == 0:
            break
        starts.append(img.base + b); ends.append(img.base + e)
    return starts, ends


def main(exe, symbols_json, global_va):
    img = Image(exe)
    names = {int(v['va'], 16): k for k, v in json.load(open(symbols_json))['functions'].items()}
    starts, ends = pdata_functions(img)
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64); md.detail = True
    sva, text = img.section_bytes('.text')
    slots = {}
    for fi, (fs, fe) in enumerate(zip(starts, ends)):
        if fs < sva or fe > sva + len(text):
            continue
        body = text[fs - sva:fe - sva]
        if struct.pack('<I', 0)[:0] is None:
            continue
        dev, vt = set(), set()
        for ins in md.disasm(body, fs):
            ops = ins.operands
            if ins.mnemonic == 'mov' and len(ops) == 2 and ops[0].type == X86_OP_REG and ops[1].type == X86_OP_MEM:
                m = ops[1].mem
                if m.base == X86_REG_RIP and ins.address + ins.size + m.disp == global_va:
                    dev.add(ops[0].reg); continue
                if m.base in dev and m.disp == 0 and m.index == 0:
                    vt.add(ops[0].reg); continue
            if ins.mnemonic == 'call' and len(ops) == 1 and ops[0].type == X86_OP_MEM and ops[0].mem.base in vt:
                slot = ops[0].mem.disp
                slots.setdefault(slot, []).append({'site': hex(ins.address), 'fn': hex(fs),
                                                   'name': names.get(fs)})
            # registers overwritten by other writes are not tracked further; this is a heuristic map
    return slots


if __name__ == '__main__':
    exe, sym, g = sys.argv[1], sys.argv[2], int(sys.argv[3], 16)
    slots = main(exe, sym, g)
    out = {('0x%x' % s): v for s, v in sorted(slots.items())}
    json.dump(out, open(sys.argv[4], 'w'), indent=1)
    for s, v in sorted(slots.items()):
        nm = sorted({x['name'] for x in v if x['name']})
        print('slot 0x%03x (#%d): %d calls  %s' % (s, s // 8, len(v), ', '.join(nm)[:200]))
