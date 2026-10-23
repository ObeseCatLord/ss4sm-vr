#!/usr/bin/env python3
"""For given addresses in a PE, print the containing function (.pdata) and the strings/cvars it references."""
import bisect, json, struct, sys
import capstone
from capstone.x86 import X86_OP_MEM, X86_REG_RIP
from se4pe import Image


class Fns:
    def __init__(self, img):
        sva, raw = img.section_bytes('.pdata')
        self.s, self.e = [], []
        for o in range(0, len(raw) - 12, 12):
            b, en, _ = struct.unpack_from('<III', raw, o)
            if b == 0:
                break
            self.s.append(img.base + b); self.e.append(img.base + en)

    def containing(self, va):
        i = bisect.bisect_right(self.s, va) - 1
        return (self.s[i], self.e[i]) if i >= 0 and va < self.e[i] else (None, None)


def fn_refs(img, md, start, end, varnames):
    sva, text = img.section_bytes('.text')
    strs, cvars = [], []
    for ins in md.disasm(text[start - sva:end - sva], start):
        for op in ins.operands:
            if op.type == X86_OP_MEM and op.mem.base == X86_REG_RIP:
                t = ins.address + ins.size + op.mem.disp
                if t in varnames and varnames[t] not in cvars:
                    cvars.append(varnames[t])
                s = img.cstr(t, 160)
                if s and len(s) >= 3 and s not in strs:
                    strs.append(s)
    return strs, cvars


if __name__ == '__main__':
    img = Image(sys.argv[1]); sym = json.load(open(sys.argv[2]))
    varnames = {int(v['va'], 16): k for k, v in sym['variables'].items()}
    fnames = {int(v['va'], 16): k for k, v in sym['functions'].items()}
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64); md.detail = True
    fns = Fns(img); seen = set()
    for a in sys.argv[3:]:
        s, e = fns.containing(int(a, 16))
        if s is None or s in seen:
            continue
        seen.add(s)
        strs, cv = fn_refs(img, md, s, e, varnames)
        print('fn %s-%s %s size=%d' % (hex(s), hex(e), fnames.get(s, ''), e - s))
        print('   cvars:', ', '.join(cv)[:400])
        print('   strings:', ' | '.join(x[:60] for x in strs[:14]))
