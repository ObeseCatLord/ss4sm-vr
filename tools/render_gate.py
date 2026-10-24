#!/usr/bin/env python3
"""Gate R10: locate the main-loop project-instance render call and the CProjectInstance Render/RenderVR
pair in SE4 PE builds. Read-only.

For each exe:
  * main loop  = function referencing "Exit signaled by windowing system message queue" (.pdata chunk);
  * call site  = the unique `mov rcx,[rip+G]; mov rax,[rcx]; call qword [rax+0x50]` (48 8B 0D d32 48 8B 01
                 FF 50 50) at most 0x400 bytes after that reference, with `xor r8d,r8d` (45 33 C0) in the
                 0x80 bytes before it; G = the project-instance global;
  * Render / RenderVR = Sam4 0x140053b70 / 0x1400552e0 ported by wildcarded signature (tools/sigscan.py);
  * vtable     = the .rdata qword pair {Render, RenderVR} (vtable+0x50 / +0x58);
  * VR view    = RenderVR's tail jump; viewer field = disp32 of its first `mov rax,[rcx+disp32]`.
usage: render_gate.py <Sam4.exe (signature source)> <exe> [<exe> ...]
"""
import struct, sys
import capstone
from se4pe import Image
from sigscan import signature, find
from pe_fninfo import Fns
from riprefs import rip_refs

MSG = b'Exit signaled by windowing system message queue\0'
SRC_RENDER, SRC_RENDER_VR = 0x140053b70, 0x1400552e0


def one(img, src, va, n):
    code, mask = signature(src, va, n)
    h = find(img, code, mask)
    return h[0] if len(h) == 1 else None


def main():
    src = Image(sys.argv[1])
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
    for path in sys.argv[1:]:
        img = Image(path); fns = Fns(img); sva, text = img.section_bytes('.text')
        print('==', path.rsplit('/', 1)[-1])
        s = img.data.find(MSG); s_va = img.va(s)
        refs = rip_refs(img, [s_va], imm_max=0)[s_va] if s_va else []
        for r in refs:
            lo, hi = fns.containing(r)
            print('   message ref  %#x (pdata chunk %#x..%#x)' % (r, lo or 0, hi or 0))
            o = r - sva; win = text[o:o + 0x400]; hits = []
            j = win.find(b'\x48\x8b\x0d')
            while j >= 0:
                if win[j + 7:j + 13] == b'\x48\x8b\x01\xff\x50\x50' and b'\x45\x33\xc0' in win[max(0, j - 0x80):j]:
                    hits.append(r + j)
                j = win.find(b'\x48\x8b\x0d', j + 1)
            for h in hits:
                g = h + 7 + struct.unpack_from('<i', text, h - sva + 3)[0]
                after = next(md.disasm(text[h - sva + 13:h - sva + 13 + 8], h + 13))
                print('   render call  %#x (call [rax+0x50] at %#x, disp8 byte at %#x); project-instance global %#x; next: %s %s'
                      % (h, h + 10, h + 12, g, after.mnemonic, after.op_str))
            if len(hits) != 1:
                print('   render call  NOT UNIQUE: %s' % [hex(x) for x in hits])
        rd = one(img, src, SRC_RENDER, 64); rv = one(img, src, SRC_RENDER_VR, 92)
        print('   Render   %s   RenderVR %s' % (hex(rd) if rd else None, hex(rv) if rv else None))
        if rd and rv:
            rs = img.sections['.rdata']; ro = rs[2]
            blob = img.data[ro:ro + rs[3]]; pair = struct.pack('<QQ', rd, rv); k = blob.find(pair); vts = []
            while k >= 0:
                if k % 8 == 0:
                    vts.append(rs[0] + k - 0x50)
                k = blob.find(pair, k + 1)
            print('   vtable (Render at +0x50, RenderVR at +0x58): %s' % [hex(v) for v in vts])
            o = rv - sva; tail = None; field = None
            for ins in md.disasm(text[o:o + 0x80], rv):
                if field is None and ins.mnemonic == 'mov' and ins.op_str.startswith('rax, qword ptr [rcx + '):
                    field = ins.op_str.split('+ ')[1].rstrip(']')
                if ins.mnemonic == 'jmp' and ins.op_str.startswith('0x'):
                    tail = ins.op_str
                if ins.mnemonic == 'int3':
                    break
            print('   RenderVR viewer field rcx+%s, tail jmp (VR view) %s' % (field, tail))


if __name__ == '__main__':
    main()
