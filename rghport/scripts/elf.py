"""Minimal big-endian ELF32 reader for the symbol-rich Wii executable (PowerPC, CodeWarrior), an optional input."""
from __future__ import annotations

import bisect
import collections
import struct
import sys


def be8(b, o):  return b[o]
def be16(b, o): return struct.unpack_from('>H', b, o)[0]
def be32(b, o): return struct.unpack_from('>I', b, o)[0]


STT = {0: 'NOTYPE', 1: 'OBJECT', 2: 'FUNC', 3: 'SECTION', 4: 'FILE'}


class Sym:
    __slots__ = ('name', 'value', 'size', 'info', 'shndx', 'sec')

    def __init__(self, name, value, size, info, shndx, sec):
        self.name, self.value, self.size = name, value, size
        self.info, self.shndx, self.sec = info, shndx, sec

    @property
    def type(self):
        return STT.get(self.info & 0xF, str(self.info & 0xF))

    @property
    def bind(self):
        return self.info >> 4

    @property
    def end(self):
        return self.value + self.size

    def __repr__(self):
        return '<Sym %08X+%d %s %s>' % (self.value, self.size, self.type, self.name)


class Elf:
    def __init__(self, path):
        self.path = path
        with open(path, 'rb') as f:
            self.b = f.read()
        b = self.b
        if b[:4] != b'\x7fELF' or b[5] != 2:
            raise ValueError('%s: expected a big-endian ELF' % path)
        self.entry = be32(b, 0x18)
        shoff, shentsize = be32(b, 0x20), be16(b, 0x2E)
        shnum, shstrndx = be16(b, 0x30), be16(b, 0x32)
        self.sections = []
        for i in range(shnum):
            o = shoff + i * shentsize
            self.sections.append(dict(
                _name=be32(b, o), type=be32(b, o + 4), flags=be32(b, o + 8),
                addr=be32(b, o + 12), off=be32(b, o + 16), size=be32(b, o + 20),
                link=be32(b, o + 24), info=be32(b, o + 28),
                align=be32(b, o + 32), entsize=be32(b, o + 36), idx=i))
        sh = self.sections[shstrndx]
        shstr = b[sh['off']:sh['off'] + sh['size']]
        for s in self.sections:
            s['name'] = cstr(shstr, s['_name'])
        self.byname = {s['name']: s for s in self.sections}
        # address-ordered allocated sections, for va->file mapping
        self._alloc = sorted((s for s in self.sections if s['flags'] & 2 and s['type'] != 8), key=lambda s: s['addr'])
        self._astart = [s['addr'] for s in self._alloc]
        self._symbols = None
        self._symidx = None

    # ---- sections -------------------------------------------------------
    def sec(self, name):
        return self.byname.get(name)

    def data(self, name):
        s = self.byname[name]
        if s['type'] == 8:                      # NOBITS
            return b'\0' * s['size']
        return self.b[s['off']:s['off'] + s['size']]

    def va2off(self, va):
        i = bisect.bisect_right(self._astart, va) - 1
        if i < 0:
            return None
        s = self._alloc[i]
        if va < s['addr'] + s['size']:
            return s['off'] + (va - s['addr'])
        return None

    def sec_of(self, va):
        for s in self.sections:
            if s['flags'] & 2 and s['addr'] <= va < s['addr'] + s['size']:
                return s
        return None

    def read(self, va, n):
        """Read n bytes at virtual address va (zeros for .bss)."""
        s = self.sec_of(va)
        if s is None:
            return None
        if s['type'] == 8:
            return b'\0' * n
        o = s['off'] + (va - s['addr'])
        return self.b[o:o + n]

    def u32(self, va):
        d = self.read(va, 4)
        return None if not d or len(d) < 4 else be32(d, 0)

    def cstring(self, va, limit=512):
        d = self.read(va, limit)
        if d is None:
            return None
        e = d.find(b'\0')
        return (d if e < 0 else d[:e]).decode('latin-1')

    # ---- symbols --------------------------------------------------------
    @property
    def symbols(self):
        if self._symbols is None:
            self._load_syms()
        return self._symbols

    def _load_syms(self):
        out = []
        for s in self.sections:
            if s['type'] not in (2, 11):
                continue
            st = self.sections[s['link']]
            strs = self.b[st['off']:st['off'] + st['size']]
            for i in range(s['size'] // 16):
                o = s['off'] + i * 16
                nm = cstr(strs, be32(self.b, o))
                out.append(Sym(nm, be32(self.b, o + 4), be32(self.b, o + 8), self.b[o + 12], be16(self.b, o + 14),
                               s['name']))
        self._symbols = out
        self._funcs = sorted((s for s in out if s.type == 'FUNC' and s.size), key=lambda s: s.value)
        self._fstart = [s.value for s in self._funcs]
        self._objs = sorted((s for s in out if s.type == 'OBJECT' and s.size), key=lambda s: s.value)
        self._ostart = [s.value for s in self._objs]
        self._byname = {}
        for s in out:
            if s.name:
                self._byname.setdefault(s.name, s)

    def sym(self, name):
        if self._symbols is None:
            self._load_syms()
        return self._byname.get(name)

    def func_at(self, va):
        """Function symbol containing va."""
        if self._symbols is None:
            self._load_syms()
        i = bisect.bisect_right(self._fstart, va) - 1
        if i < 0:
            return None
        f = self._funcs[i]
        return f if va < f.value + f.size else None

    def obj_at(self, va):
        if self._symbols is None:
            self._load_syms()
        i = bisect.bisect_right(self._ostart, va) - 1
        if i < 0:
            return None
        f = self._objs[i]
        return f if va < f.value + f.size else None

    def label(self, va):
        f = self.func_at(va) or self.obj_at(va)
        if f is None:
            return '%08X' % va
        d = va - f.value
        return f.name if d == 0 else '%s+0x%X' % (f.name, d)


def cstr(buf, off):
    e = buf.find(b'\0', off)
    return buf[off:e].decode('latin-1') if e >= 0 else buf[off:].decode('latin-1')


def main(argv):
    if not argv:
        print("usage: python -m rghport.scripts.elf EXECUTABLE")
        return 2
    e = Elf(argv[0])
    print('entry %08X  sections %d  symbols %d' % (e.entry, len(e.sections), len(e.symbols)))
    for s in e.sections:
        if s['size']:
            print('  %-18s addr=%08X off=%08X size=%9d' % (s['name'], s['addr'], s['off'], s['size']))
    print(collections.Counter(s.type for s in e.symbols))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
