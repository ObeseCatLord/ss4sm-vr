#!/usr/bin/env python3
"""Port docs/re/builds/signatures.json entries from Sam4.exe to all builds into manual.json (unique only)."""
import json, pathlib, sys
from se4pe import Image
from sigscan import signature, find

ROOT = pathlib.Path(__file__).resolve().parents[1]
G = '/home/obesecatlord/FasterGames/SteamLibrary/steamapps/common/'
EXES = {'Sam4': G + 'Serious Sam 4/Bin/x64/Sam4.exe', 'Sam4_Unrestricted': G + 'Serious Sam 4/Bin/x64/Sam4_Unrestricted.exe',
        'SamSM': G + 'Siberian Mayhem/Bin/x64/SamSM.exe', 'SamSM_Unrestricted': G + 'Siberian Mayhem/Bin/x64/SamSM_Unrestricted.exe'}
sigs = {k: v for k, v in json.load(open(ROOT / 'docs/re/builds/signatures.json')).items() if not k.startswith('_')}
manual = json.load(open(ROOT / 'docs/re/builds/manual.json'))
src = Image(EXES['Sam4'])
imgs = {b: Image(p) for b, p in EXES.items()}
ok = True
import capstone
md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)


def harvested(build, name):
    """VA of a registered script function (docs/re/builds/<build>.json), e.g. 'CPlayerVRData::GetHeadPlacement'."""
    f = json.load(open(ROOT / f'docs/re/builds/{build}.json'))['functions'].get(name)
    return (f['va'] if isinstance(f, dict) else f) if f else None


def nth_call(img, va, index, limit=0x200):
    o = img.off(va)
    calls = [i.operands_str if False else int(i.op_str, 16) for i in md.disasm(img.data[o:o + limit], va)
             if i.mnemonic == 'call' and i.op_str.startswith('0x')]
    return calls[index] if index < len(calls) else None


for name, s in [(k, v) for k, v in sigs.items() if 'call_of' not in v] + [(k, v) for k, v in sigs.items() if 'call_of' in v]:
    if 'call_of' in s:
        for b, img in imgs.items():
            base = manual.get(b, {}).get(s['call_of']) or harvested(b, s['call_of'])
            t = nth_call(img, int(base, 16), s['index'], s.get('limit', 0x200)) if base else None
            if t: manual.setdefault(b, {})[name] = hex(t)
            else:
                ok = False
                print('FAIL', name, b, file=sys.stderr)
        continue
    code, mask = signature(src, int(s['src'], 16), s['len'])
    # 'alt': {build, src, len} - a second signature taken from another build, for builds whose code differs
    # from Sam4's (e.g. Siberian Mayhem); its source address must be established in that build's gate notes.
    alt = s.get('alt')
    alt_sig = signature(imgs[alt['build']], int(alt['src'], 16), alt['len']) if alt else None
    for b, img in imgs.items():
        hits = find(img, code, mask)
        if len(hits) != 1 and alt_sig:
            hits = find(img, *alt_sig)
        if len(hits) == 1:
            manual.setdefault(b, {})[name] = hex(hits[0])
        else:
            ok = False
            print('FAIL', name, b, [hex(h) for h in hits[:4]], file=sys.stderr)
json.dump(manual, open(ROOT / 'docs/re/builds/manual.json', 'w'), indent=1)
print('ported', len(sigs), 'functions' if ok else 'with failures')
sys.exit(0 if ok else 1)
