"""Minimal PE helpers for Serious Engine 4 executables (pefile-based)."""
import struct
import pefile


class Image:
    def __init__(self, path):
        self.path = path
        self.data = open(path, 'rb').read()
        self.pe = pefile.PE(data=self.data, fast_load=True)
        self.base = self.pe.OPTIONAL_HEADER.ImageBase
        self.sections = {}
        for s in self.pe.sections:
            name = s.Name.rstrip(b'\0').decode()
            self.sections[name] = (self.base + s.VirtualAddress, s.Misc_VirtualSize,
                                   s.PointerToRawData, s.SizeOfRawData)

    def section_of(self, va):
        for name, (sva, vsize, _, _) in self.sections.items():
            if sva <= va < sva + vsize:
                return name
        return None

    def off(self, va):
        for sva, vsize, raw, rsize in self.sections.values():
            if sva <= va < sva + min(vsize, rsize):
                return raw + (va - sva)
        return None

    def va(self, off):
        for sva, vsize, raw, rsize in self.sections.values():
            if raw <= off < raw + rsize:
                return sva + (off - raw)
        return None

    def q(self, va):
        o = self.off(va)
        return None if o is None else struct.unpack_from('<Q', self.data, o)[0]

    def cstr(self, va, limit=512):
        if not isinstance(va, int) or va < self.base:
            return None
        o = self.off(va)
        if o is None:
            return None
        e = self.data.find(b'\0', o, o + limit)
        if e < 0:
            return None
        try:
            s = self.data[o:e].decode('utf-8')
        except UnicodeDecodeError:
            return None
        return s if (s == '' or s.isprintable()) else None

    def section_bytes(self, name):
        sva, vsize, raw, rsize = self.sections[name]
        return sva, self.data[raw:raw + min(vsize, rsize)]
