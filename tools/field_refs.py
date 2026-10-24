#!/usr/bin/env python3
"""Find instructions whose memory operand has a given displacement ([reg+disp], any base except rip/rsp),
e.g. every access to a member at +0x2260. Byte search for the disp32/disp8 value, then capstone decode
of each candidate start 1..10 bytes earlier; reports read/write, containing function and harvested name.
usage: field_refs.py <exe> <symbols.json> [--fn-range lo hi] <disp> [<disp> ...]
"""
import json, struct, sys
import capstone
from capstone.x86 import X86_OP_MEM, X86_REG_RIP, X86_REG_RSP, X86_REG_RBP
from se4pe import Image
from pe_fninfo import Fns

if __name__ == '__main__':
    img = Image(sys.argv[1]); sym = json.load(open(sys.argv[2]))
    args = sys.argv[3:]; lo, hi = 0, 1 << 64
    if args[0] == '--fn-range':
        lo, hi = int(args[1], 16), int(args[2], 16); args = args[3:]
    fnames = {int(v['va'], 16): k for k, v in sym['functions'].items()}
    fns = Fns(img)
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64); md.detail = True
    sva, text = img.section_bytes('.text')
    for d in args:
        disp = int(d, 16)
        needle = struct.pack('<i', disp)
        seen = set(); out = []
        o = text.find(needle)
        while o >= 0:
            cands = []
            for back in range(2, 11):
                st = o - back
                if st < 0: break
                ins = next(md.disasm(text[st:st + 15], sva + st), None)
                if not ins or not ins.disp_offset or st + ins.disp_offset != o: continue
                if not any(op.type == X86_OP_MEM and op.mem.disp == disp and op.mem.base not in (X86_REG_RIP, X86_REG_RSP, X86_REG_RBP) for op in ins.operands):
                    continue
                cands.append(ins)
            # several decodings can cover the same displacement; the longest plausible one is the real
            # instruction (shorter ones start inside its prefix/opcode bytes and decode as adc/sbb/..)
            good = [i for i in cands if i.mnemonic not in ('adc', 'sbb', 'add', 'or', 'and', 'xor', 'sub', 'cmp') or i.size >= 7]
            if good:
                ins = max(good, key=lambda i: i.size)
                if ins.address not in seen:
                    seen.add(ins.address)
                    f, _ = fns.containing(ins.address)
                    if f and lo <= f < hi:
                        w = (ins.operands[0].type == X86_OP_MEM and not ins.mnemonic.startswith(('cmp', 'test', 'ucomis', 'comis', 'push')))
                        out.append((ins.address, f, 'W' if w else 'r', '%s %s' % (ins.mnemonic, ins.op_str)))
            o = text.find(needle, o + 1)
        print('== +%s: %d sites' % (hex(disp), len(out)))
        for a, f, rw, t in out:
            print('   %s %s fn %s %-30s %s' % (rw, hex(a), hex(f), fnames.get(f, '')[:30], t))
