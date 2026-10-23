#!/usr/bin/env python3
"""Derive the VR device global per build: the first RIP-relative qword load in vrGetDeviceName,
vrGetDeviceAPI and vrIsEyeTracking must agree. Writes docs/re/builds/manual.json."""
import json, pathlib
import capstone
from capstone.x86 import X86_OP_MEM, X86_REG_RIP
from se4pe import Image

ROOT = pathlib.Path(__file__).resolve().parents[1]
G = '/home/obesecatlord/FasterGames/SteamLibrary/steamapps/common/'
EXES = {'Sam4': G + 'Serious Sam 4/Bin/x64/Sam4.exe', 'Sam4_Unrestricted': G + 'Serious Sam 4/Bin/x64/Sam4_Unrestricted.exe',
        'SamSM': G + 'Siberian Mayhem/Bin/x64/SamSM.exe', 'SamSM_Unrestricted': G + 'Siberian Mayhem/Bin/x64/SamSM_Unrestricted.exe'}
md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64); md.detail = True
manual = json.load(open(ROOT / 'docs/re/builds/manual.json'))
for b, exe in EXES.items():
    img = Image(exe); fns = json.load(open(ROOT / f'docs/re/builds/{b}.json'))['functions']
    found = set()
    for n in ('vrGetDeviceName', 'vrGetDeviceAPI', 'vrIsEyeTracking'):
        va = int(fns[n]['va'], 16)
        o = img.off(va)
        for ins in md.disasm(img.data[o:o + 64], va):
            ops = ins.operands
            if ins.mnemonic in ('mov', 'cmp') and any(op.type == X86_OP_MEM and op.mem.base == X86_REG_RIP for op in ops):
                op = next(op for op in ops if op.type == X86_OP_MEM)
                found.add(ins.address + ins.size + op.mem.disp); break
    assert len(found) == 1, (b, [hex(x) for x in found])
    g = found.pop()
    manual.setdefault(b, {})['vr_device_global'] = hex(g)
    print(b, hex(g))
json.dump(manual, open(ROOT / 'docs/re/builds/manual.json', 'w'), indent=1)
