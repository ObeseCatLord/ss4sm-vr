#!/usr/bin/env python3
"""Confirm a port found by other means (vtable slot, call order, constant shift): compare the wildcarded
signature of a source function (sigscan.signature: rel32 call/jump targets and RIP displacements masked)
with the bytes at a candidate VA in the target build. Prints matched/total masked bytes for the first N
bytes and the first differing offset, so a port can be accepted (full match) or rejected. Read-only.
usage: sig_at.py <src.exe> <target.exe> [--len N] <src_va>=<target_va>[=name] [...]
"""
import sys
from se4pe import Image
from sigscan import signature


def main():
    args = sys.argv[1:]
    src, dst = Image(args[0]), Image(args[1])
    args = args[2:]
    n = 64
    if args and args[0] == '--len':
        n = int(args[1], 0)
        args = args[2:]
    for a in args:
        parts = a.split('=')
        sva, tva = int(parts[0], 16), int(parts[1], 16)
        name = parts[2] if len(parts) > 2 else ''
        code, mask = signature(src, sva, n)
        o = dst.off(tva)
        tgt = dst.data[o:o + len(code)]
        tot = sum(mask)
        ok = sum(1 for c, t, m in zip(code, tgt, mask) if m and c == t)
        first = next((i for i, (c, t, m) in enumerate(zip(code, tgt, mask)) if m and c != t), None)
        verdict = 'MATCH' if ok == tot else 'DIFF at +0x%x' % first
        print(f'0x{sva:x} -> 0x{tva:x}  {name:28s} {ok}/{tot} masked bytes of {n}: {verdict}')


if __name__ == '__main__':
    main()
