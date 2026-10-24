#!/usr/bin/env python3
"""Find SE4 binary-meta files (CTSEMETA) inside .gro archives that *serialize* an object of a given class,
i.e. whose DTTY (type definition) table describes the class itself (not only Ptr<Class>). The EXTY table
lists every type the schema references, even through null pointers, so it is not evidence of an object.
Read-only on the game; nothing is extracted.
usage: meta_types.py <Class>[,<Class>...] <gro>...     (prints archive, file, matched classes)
       meta_types.py --file <path> ...                  (prints the DTTY table of loose files)
"""
import struct
import sys
import zipfile


def dtty(d):
    i = d.find(b'INTY'); j = d.find(b'EXOB'); k = i; out = []
    if i < 0:
        return out
    while True:
        k = d.find(b'DTTY', k + 1)
        if k < 0 or (j >= 0 and k > j):
            break
        idx, ln = struct.unpack_from('<II', d, k + 4)
        out.append(d[k + 12:k + 12 + ln].decode('latin1'))
    return out


def main():
    if sys.argv[1] == '--file':
        for p in sys.argv[2:]:
            print(p, dtty(open(p, 'rb').read()))
        return
    want = set(sys.argv[1].split(','))
    for g in sys.argv[2:]:
        z = zipfile.ZipFile(g)
        for i in z.infolist():
            if i.file_size > 64_000_000:
                continue
            with z.open(i) as f:
                if f.read(8) != b'CTSEMETA':
                    continue
            d = z.read(i)
            hit = sorted(want.intersection(dtty(d)))
            if hit:
                print(g.rsplit('/', 1)[-1], i.filename, hit, flush=True)


if __name__ == '__main__':
    main()
