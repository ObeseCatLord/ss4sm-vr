#!/usr/bin/env python3
"""Per-call-site map of the VR device object in an SE4 PE (stricter successor of device_slots.py).

For every function (from .pdata) that references the device global, linear-sweep the body and track
  D = registers / stack slots holding the device pointer (loaded from the global, or copied),
  V = registers holding its vtable (`mov V, [D]`).
A register leaves D/V when it is written by anything else; calls clobber rax, rcx, rdx, r8-r11, xmm0-5.
Reported per site:
  * vcalls `call [V+slot]` with whether rcx==device at the call, plus the last writer of rdx/r8/r9/
    xmm1-3 and of [rsp+0x20..0x48] since the previous call (argument setup evidence),
  * direct field accesses `[D+disp]` (disp != 0) with the access width and direction.
Linear sweep ignores control flow, so this is evidence for a human, not proof.
usage: device_calls.py <exe> <symbols.json> <global_va> <out.json>
"""
import bisect, json, struct, sys
import capstone
from capstone.x86 import X86_OP_MEM, X86_OP_REG, X86_REG_RIP, X86_REG_RSP
from se4pe import Image
from riprefs import rip_refs

VOLATILE = {'rax', 'rcx', 'rdx', 'r8', 'r9', 'r10', 'r11'}
XVOL = {'xmm%d' % i for i in range(6)}
ARGREGS = ['rdx', 'r8', 'r9', 'xmm1', 'xmm2', 'xmm3']
FAM = {}
for r64, r32, r16, r8 in [('rax', 'eax', 'ax', 'al'), ('rbx', 'ebx', 'bx', 'bl'), ('rcx', 'ecx', 'cx', 'cl'),
                          ('rdx', 'edx', 'dx', 'dl'), ('rsi', 'esi', 'si', 'sil'), ('rdi', 'edi', 'di', 'dil'),
                          ('rbp', 'ebp', 'bp', 'bpl'), ('rsp', 'esp', 'sp', 'spl')]:
    for x in (r64, r32, r16, r8):
        FAM[x] = r64
for i in range(8, 16):
    for x in ('r%d' % i, 'r%dd' % i, 'r%dw' % i, 'r%db' % i):
        FAM[x] = 'r%d' % i
for i in range(16):
    for x in ('xmm%d' % i, 'ymm%d' % i):
        FAM[x] = 'xmm%d' % i


def fam(md, reg):
    n = md.reg_name(reg)
    return FAM.get(n, n)


def pdata(img):
    sva, raw = img.section_bytes('.pdata')
    s, e = [], []
    for o in range(0, len(raw) - 12, 12):
        b, en, _ = struct.unpack_from('<III', raw, o)
        if b == 0:
            break
        s.append(img.base + b); e.append(img.base + en)
    return s, e


def analyse(img, md, fs, fe, g, sva, text, fname, volatile=VOLATILE, this='rcx', argregs=ARGREGS):
    VOLATILE, ARGREGS_ = volatile, argregs
    D, V, Dstk = set(), set(), set()
    lastw, stk = {}, {}
    calls, fields, passes = [], [], []
    pending, dead = {}, False
    for ins in md.disasm(text[fs - sva:fe - sva], fs):
        ops = ins.operands
        # --- merge state carried by branches to this address (over-approximation)
        if ins.address in pending:
            pD, pV, pS = pending.pop(ins.address)
            if dead:
                D, V, Dstk = set(pD), set(pV), set(pS)
            else:
                D |= pD; V |= pV; Dstk |= pS
        elif dead:
            D, V, Dstk = set(), set(), set()
        dead = False
        if ins.mnemonic.startswith('j') and ops and ops[0].type == capstone.x86.X86_OP_IMM:
            t = ops[0].imm
            if fs <= t < fe and t > ins.address:
                q = pending.setdefault(t, (set(), set(), set()))
                q[0].update(D); q[1].update(V); q[2].update(Dstk)
            if ins.mnemonic == 'jmp':
                dead = True
            continue
        if ins.mnemonic in ('ret', 'int3'):
            dead = True
            continue
        txt = '%x: %s %s' % (ins.address, ins.mnemonic, ins.op_str)
        # --- classify memory operand relative to D
        memop = next((o for o in ops if o.type == X86_OP_MEM), None)
        if memop is not None:
            m = memop.mem
            base = fam(md, m.base) if m.base else None
            if base in D and m.index == 0 and m.disp != 0 and ins.mnemonic not in ('lea',):
                wr = ops[0].type == X86_OP_MEM and ins.mnemonic not in ('cmp', 'test', 'ucomiss', 'comiss')
                fields.append({'site': hex(ins.address), 'disp': hex(m.disp), 'size': memop.size,
                               'write': wr, 'ins': txt})
        if ins.mnemonic in ('call', 'jmp'):
            if ins.mnemonic == 'jmp':
                dead = True
            if ops and ops[0].type == X86_OP_MEM and ops[0].mem.base and fam(md, ops[0].mem.base) in V \
                    and ops[0].mem.index == 0:
                calls.append({'tail': ins.mnemonic == 'jmp', 'site': hex(ins.address), 'slot': hex(ops[0].mem.disp), 'fn': hex(fs), 'name': fname,
                              'this_is_device': this in D,
                              'args': {r: lastw[r] for r in ARGREGS_ if r in lastw},
                              'stack': {hex(k): v for k, v in sorted(stk.items())}})
            elif this in D:
                passes.append({'site': hex(ins.address), 'ins': txt})
            D -= VOLATILE; V -= VOLATILE
            for r in list(lastw):
                if r in VOLATILE or r in XVOL:
                    del lastw[r]
            stk.clear()
            continue
        # --- data flow
        written = set()
        try:
            _, w = ins.regs_access()
            written = {fam(md, r) for r in w}
        except capstone.CsError:
            pass
        newD, newV = None, None
        if ins.mnemonic == 'mov' and len(ops) == 2 and ops[0].type == X86_OP_REG:
            dst = fam(md, ops[0].reg)
            if ops[1].type == X86_OP_MEM:
                m = ops[1].mem
                if m.base == X86_REG_RIP and ins.address + ins.size + m.disp == g:
                    newD = dst
                elif m.base and fam(md, m.base) in D and m.disp == 0 and m.index == 0:
                    newV = dst
                elif m.base and fam(md, m.base) in ('rsp', 'rbp') and (fam(md, m.base), m.disp) in Dstk:
                    newD = dst
            elif ops[1].type == X86_OP_REG and fam(md, ops[1].reg) in D:
                newD = dst
            elif ops[1].type == X86_OP_REG and fam(md, ops[1].reg) in V:
                newV = dst
        if ins.mnemonic == 'mov' and len(ops) == 2 and ops[0].type == X86_OP_MEM and ops[1].type == X86_OP_REG:
            m = ops[0].mem
            if m.base and fam(md, m.base) in ('rsp', 'rbp'):
                key = (fam(md, m.base), m.disp)
                if fam(md, ops[1].reg) in D:
                    Dstk.add(key)
                else:
                    Dstk.discard(key)
                if fam(md, m.base) == 'rsp' and 0x20 <= m.disp <= 0x60:
                    stk[m.disp] = txt
        elif ops and ops[0].type == X86_OP_MEM and ops[0].mem.base and fam(md, ops[0].mem.base) == 'rsp' \
                and 0x20 <= ops[0].mem.disp <= 0x60 and ins.mnemonic.startswith(('mov', 'lea')):
            stk[ops[0].mem.disp] = txt
        for r in written:
            D.discard(r); V.discard(r)
            if r in ARGREGS_ or r in VOLATILE or r.startswith('xmm'):
                lastw[r] = txt
        if newD:
            D.add(newD)
        if newV:
            V.add(newV)
    return calls, fields, passes


def main(exe, symjson, g, out):
    img = Image(exe)
    sym = json.load(open(symjson))
    names = {int(v['va'], 16): k for k, v in sym['functions'].items()}
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64); md.detail = True
    s, e = pdata(img)
    sva, text = img.section_bytes('.text')
    refs = rip_refs(img, [g])[g]
    fns = sorted({(s[i], e[i]) for i in (bisect.bisect_right(s, r) - 1 for r in refs) if i >= 0})
    res = {'global': hex(g), 'functions': {}, 'slots': {}, 'fields': {}}
    for fs, fe in fns:
        calls, fields, passes = analyse(img, md, fs, fe, g, sva, text, names.get(fs))
        res['functions'][hex(fs)] = {'end': hex(fe), 'name': names.get(fs), 'ncalls': len(calls),
                                     'nfields': len(fields), 'passes_device_in_rcx': passes}
        for c in calls:
            res['slots'].setdefault(c['slot'], []).append(c)
        for f in fields:
            f['fn'] = hex(fs); f['name'] = names.get(fs)
            res['fields'].setdefault(f['disp'], []).append(f)
    res['slots'] = dict(sorted(res['slots'].items(), key=lambda kv: int(kv[0], 16)))
    res['fields'] = dict(sorted(res['fields'].items(), key=lambda kv: int(kv[0], 16)))
    json.dump(res, open(out, 'w'), indent=1)
    print('%d functions reference %s' % (len(fns), hex(g)))
    for k, v in res['slots'].items():
        th = sum(1 for c in v if c['this_is_device'])
        nm = sorted({c['name'] for c in v if c['name']})
        print('slot %6s: %3d calls (%d with rcx=device) %s' % (k, len(v), th, ', '.join(nm)[:150]))
    for k, v in res['fields'].items():
        print('field %6s: %3d accesses (%d writes) in %s' % (k, len(v), sum(f['write'] for f in v),
                                                          ', '.join(sorted({f['fn'] for f in v}))[:150]))


if __name__ == '__main__':
    main(sys.argv[1], sys.argv[2], int(sys.argv[3], 16), sys.argv[4])
