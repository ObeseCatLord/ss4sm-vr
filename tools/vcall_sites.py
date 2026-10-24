#!/usr/bin/env python3
"""Find indirect calls/jumps through a vtable displacement: `call/jmp qword ptr [reg+disp32]`
(FF /2 or FF /4 with mod=10), e.g. every `call [rax+0x1290]`. Prints containing .pdata function.
usage: vcall_sites.py <exe> <symbols.json> <disp> [<disp> ...]
"""
import json, re, struct, sys
from se4pe import Image
from pe_fninfo import Fns

if __name__ == '__main__':
    img = Image(sys.argv[1]); sym = json.load(open(sys.argv[2]))
    fnames = {int(v['va'], 16): k for k, v in sym['functions'].items()}
    fns = Fns(img)
    sva, text = img.section_bytes('.text')
    for d in sys.argv[3:]:
        disp = int(d, 16)
        if disp < 0x80:  # disp8 form: modrm mod=01
            pat = re.compile(b'\xff[\x50-\x57\x60-\x67]' + re.escape(bytes([disp])), re.S)
        else:
            pat = re.compile(b'\xff[\x90-\x97\xa0-\xa7]' + re.escape(struct.pack('<i', disp)), re.S)
        hits = [sva + m.start() for m in pat.finditer(text)]
        # rsp-based (modrm 0x94/0xa4 + SIB) is not a vtable call; r8-r15 need a REX prefix (41 ff ..)
        print('== [reg+%s]: %d sites' % (hex(disp), len(hits)))
        for h in hits:
            s, _ = fns.containing(h)
            m = text[h - sva + 1]; kind = 'call' if (m & 0x38) == 0x10 else 'jmp '
            print('   %s %s in fn %s %s' % (kind, hex(h), hex(s) if s else '?', fnames.get(s, '')))
