#!/usr/bin/env python3
"""Fingerprint a Serious Engine 4 executable: sha256, PE timestamp, image size, .text CRC, build string."""
import hashlib, json, re, sys, zlib
from se4pe import Image


def fingerprint(path):
    img = Image(path)
    sva, text = img.section_bytes('.text')
    m = re.search(rb'\$Version: (Sam4_dev; [A-Za-z0-9-]+; (\d+) [0-9: -]+ @\w+; [A-Za-z0-9-]+)\$', img.data)
    return {
        'file': path.rsplit('/', 1)[-1],
        'sha256': hashlib.sha256(img.data).hexdigest(),
        'time_date_stamp': img.pe.FILE_HEADER.TimeDateStamp,
        'size_of_image': img.pe.OPTIONAL_HEADER.SizeOfImage,
        'text_crc32': zlib.crc32(text) & 0xffffffff,
        'build_string': m.group(1).decode() if m else None,
        'build': int(m.group(2)) if m else None,
    }


if __name__ == '__main__':
    print(json.dumps([fingerprint(p) for p in sys.argv[1:]], indent=2))
