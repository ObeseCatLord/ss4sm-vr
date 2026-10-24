#!/usr/bin/env python3
"""Resolve the Class::member names in tools/members.txt to byte offsets in every build, from the engine's
reflection member records (layout: tools/meta_members.py). Writes docs/re/builds/members.json. A member
must resolve to exactly one record owned by the named class, or the build gets no value for it."""
import json, pathlib, sys
from se4pe import Image
from strrefs import find_strings, abs_ptrs
from meta_members import owner

ROOT = pathlib.Path(__file__).resolve().parents[1]
G = '/home/obesecatlord/FasterGames/SteamLibrary/steamapps/common/'
EXES = {'Sam4': G + 'Serious Sam 4/Bin/x64/Sam4.exe', 'Sam4_Unrestricted': G + 'Serious Sam 4/Bin/x64/Sam4_Unrestricted.exe',
        'SamSM': G + 'Siberian Mayhem/Bin/x64/SamSM.exe', 'SamSM_Unrestricted': G + 'Siberian Mayhem/Bin/x64/SamSM_Unrestricted.exe'}


def main():
    wanted = [l.split('#')[0].strip() for l in (ROOT / 'tools/members.txt').read_text().splitlines()]
    wanted = [w for w in wanted if w]
    out, ok = {}, True
    for b, path in EXES.items():
        img = Image(path)
        res = {}
        for w in wanted:
            cls, mem = w.split('::')
            vas = find_strings(img, mem)
            ap = abs_ptrs(img, vas) if vas else {}
            hits = set()
            for v in vas:
                for p in ap[v]:
                    rec = p - 0x20
                    _, names = owner(img, rec)
                    if len(names) > 1 and names[1] == cls:
                        hits.add(img.q(rec + 0x38))
            if len(hits) == 1:
                res[w] = hex(hits.pop())
            else:
                ok = False
                print('FAIL', b, w, sorted(hex(h) for h in hits), file=sys.stderr)
        out[b] = res
    json.dump(out, open(ROOT / 'docs/re/builds/members.json', 'w'), indent=1)
    sys.exit(0 if ok else 1)


if __name__ == '__main__':
    main()
