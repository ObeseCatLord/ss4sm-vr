#!/usr/bin/env python3
"""List or extract files from SE4 .gro archives (they are plain zip files). Read-only on the game.
Later archives override earlier ones (pass them in mount order, e.g. 00_All.gro then 50_<build>_00_All.gro).
usage: gro_extract.py list <regex> <gro>...           list matching names (size, archive)
       gro_extract.py extract <outdir> <regex> <gro>...  extract matches (last archive wins) into outdir
Never commit what this extracts (AGENTS.md): use an outdir under lab/.
"""
import os
import re
import sys
import zipfile


def main():
    mode = sys.argv[1]
    if mode == 'list':
        rx, gros, out = re.compile(sys.argv[2], re.I), sys.argv[3:], None
    else:
        out, rx, gros = sys.argv[2], re.compile(sys.argv[3], re.I), sys.argv[4:]
    pick = {}
    for g in gros:
        z = zipfile.ZipFile(g)
        for i in z.infolist():
            if rx.search(i.filename):
                pick[i.filename] = (g, i.file_size)
    for n, (g, sz) in sorted(pick.items()):
        print(f'{sz:10d} {os.path.basename(g)} {n}')
        if out:
            dst = os.path.join(out, n)
            if not os.path.realpath(dst).startswith(os.path.realpath(out)):
                continue
            os.makedirs(os.path.dirname(dst), exist_ok=True)
            with zipfile.ZipFile(g) as z, open(dst, 'wb') as f:
                f.write(z.read(n))


if __name__ == '__main__':
    main()
