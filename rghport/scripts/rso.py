"""Reader for Nintendo Revolution RSO modules (the Wii script module).

RSO is the Wii relocatable-module format.  The script module is SCR2CPP output: the game's scripts compiled to C++ and
shipped as a side module, with a full export table -- about 20k named symbols -- which is the ground truth for what
every script function, track, rule and trigger is called.
"""
from __future__ import annotations

import bisect
import struct
import sys


def be32(b, o): return struct.unpack_from('>I', b, o)[0]
def be16(b, o): return struct.unpack_from('>H', b, o)[0]


R_PPC = {0: 'NONE', 1: 'ADDR32', 2: 'ADDR24', 3: 'ADDR16', 4: 'ADDR16_LO',
         5: 'ADDR16_HI', 6: 'ADDR16_HA', 10: 'REL24', 11: 'REL14',
         201: 'RVL_NONE', 202: 'RVL_SECT', 203: 'RVL_STOP'}


class Export:
    __slots__ = ('name', 'off', 'sec', 'hash')

    def __init__(self, name, off, sec, h):
        self.name, self.off, self.sec, self.hash = name, off, sec, h

    def __repr__(self):
        return '<%s sec%d+0x%X>' % (self.name, self.sec, self.off)


class RSO:
    def __init__(self, path, load_base=0x81000000):
        self.path = path
        with open(path, 'rb') as f:
            self.b = b = f.read()
        h = struct.unpack_from('>22I', b, 0)
        (self.next, self.prev, self.numSections, self.sectionInfoOffset,
         self.nameOffset, self.nameSize, self.version, self.bssSize) = h[:8]
        self.prologSection = b[0x20]
        self.epilogSection = b[0x21]
        self.unresolvedSection = b[0x22]
        self.bssSection = b[0x23]
        (self.prolog, self.epilog, self.unresolved,
         self.internalRelOffset, self.internalRelSize,
         self.externalRelOffset, self.externalRelSize,
         self.exportsOffset, self.exportsSize, self.exportsNameOffset,
         self.importsOffset, self.importsSize,
         self.importsNameOffset) = struct.unpack_from('>13I', b, 0x24)

        self.sections = []
        for i in range(self.numSections):
            o = self.sectionInfoOffset + i * 8
            off, size = be32(b, o), be32(b, o + 4)
            self.sections.append(dict(idx=i, off=off & ~1, exec=bool(off & 1), size=size))
        self.name = self._cstr(self.nameOffset)

        # exports: {nameOff, offset, sectionIdx, hash} x 16 bytes
        self.exports = []
        for o in range(self.exportsOffset, self.exportsOffset + self.exportsSize, 16):
            nm = self._cstr(self.exportsNameOffset + be32(b, o))
            self.exports.append(Export(nm, be32(b, o + 4), be32(b, o + 8), be32(b, o + 12)))
        self.by_name = {}
        for x in self.exports:
            self.by_name.setdefault(x.name, x)

        # imports: {nameOff, offset(head of reloc chain), sectionIdx} x 12
        self.imports = []
        for o in range(self.importsOffset, self.importsOffset + self.importsSize, 12):
            nm = self._cstr(self.importsNameOffset + be32(b, o))
            self.imports.append((nm, be32(b, o + 4), be32(b, o + 8)))

        # Address space: VA = load_base + file offset.  The module is laid out so that this mapping keeps every
        # internal PC-relative branch correct, and internal relocations (whose r_offset is a file offset) map to an
        # address directly.
        self.load_base = load_base
        self.base = {}
        for s in self.sections:
            if s['size'] and s['off']:
                self.base[s['idx']] = load_base + s['off']
            elif s['size']:                       # .bss, placed after the file
                self.base[s['idx']] = load_base + ((len(b) + 0x1F) & ~0x1F)
            else:
                self.base[s['idx']] = 0
        self._exsorted = sorted(((self.base[x.sec] + x.off, x) for x in self.exports if self.base.get(x.sec)),
                                key=lambda t: t[0])
        self._exaddr = [a for a, _ in self._exsorted]

        # --- relocations, indexed by the address they patch --------------
        self.ext_by_site = {}          # site VA -> (import name, reloc type)
        self.int_by_site = {}          # site VA -> (reloc type, target VA)
        self._index_ext()
        self._index_int()

    def _index_ext(self):
        """External relocations are grouped by import; sym_idx is the import index (all entries agree)."""
        b = self.b
        for k in range(self.externalRelSize // 12):
            o = self.externalRelOffset + k * 12
            r_off = be32(b, o)
            info = be32(b, o + 4)
            idx = info >> 8
            if idx < len(self.imports):
                self.ext_by_site[self.load_base + r_off] = (self.imports[idx][0], info & 0xFF)

    def _index_int(self):
        b = self.b
        for k in range(self.internalRelSize // 12):
            o = self.internalRelOffset + k * 12
            r_off = be32(b, o)
            info = be32(b, o + 4)
            add = be32(b, o + 8)
            sec = info >> 8
            base = self.base.get(sec, 0)
            if base:
                self.int_by_site[self.load_base + r_off] = (info & 0xFF, base + add)

    def _cstr(self, off, limit=1024):
        e = self.b.find(b'\0', off)
        return self.b[off:e].decode('latin-1')

    # --- flat address helpers -------------------------------------------
    def va(self, sec, off):
        return self.base.get(sec, 0) + off

    def read(self, va, n):
        for s in self.sections:
            b = self.base.get(s['idx'], 0)
            if b and b <= va < b + s['size']:
                if s['off'] == 0:                 # .bss
                    return b'\0' * n
                o = s['off'] + (va - b)
                return self.b[o:o + n]
        return None

    def sec_of(self, va):
        for s in self.sections:
            b = self.base.get(s['idx'], 0)
            if b and b <= va < b + s['size']:
                return s
        return None

    def label(self, va):
        i = bisect.bisect_right(self._exaddr, va) - 1
        if i < 0:
            return '%08X' % va
        a, x = self._exsorted[i]
        s = self.sec_of(va)
        if s and self.base[s['idx']] > a:
            return '%08X' % va
        d = va - a
        return x.name if d == 0 else '%s+0x%X' % (x.name, d)

    def sym(self, name):
        x = self.by_name.get(name)
        return None if x is None else self.va(x.sec, x.off)

    def size_of(self, name):
        """Distance to the next export in the same section -- best available."""
        x = self.by_name.get(name)
        if x is None:
            return None
        a = self.va(x.sec, x.off)
        i = bisect.bisect_right(self._exaddr, a)
        s = self.sec_of(a)
        end = self.base[s['idx']] + s['size'] if s else a
        if i < len(self._exaddr) and self._exaddr[i] < end:
            end = self._exaddr[i]
        return end - a

    # --- relocations -----------------------------------------------------
    def relocations(self, internal=True):
        """Yield (r_offset, type, symbol index, addend)."""
        b = self.b
        off = self.internalRelOffset if internal else self.externalRelOffset
        size = self.internalRelSize if internal else self.externalRelSize
        for o in range(off, off + size, 12):
            r_off = be32(b, o)
            r_info = be32(b, o + 4)
            r_add = be32(b, o + 8)
            yield r_off, r_info & 0xFF, r_info >> 8, r_add


def main(argv):
    if not argv:
        print("usage: python -m rghport.scripts.rso MODULE")
        return 2
    r = RSO(argv[0])
    print('module %r  version %d  bss %d' % (r.name, r.version, r.bssSize))
    print('sections:')
    for s in r.sections:
        print('  [%2d] off=%08X size=%8d %s  base=%08X' % (s['idx'], s['off'], s['size'], 'X' if s['exec'] else ' ',
                                                         r.base.get(s['idx'], 0)))
    print('exports: %d   imports: %d' % (len(r.exports), len(r.imports)))
    print('prolog sec%d+%X  epilog sec%d+%X' % (r.prologSection, r.prolog, r.epilogSection, r.epilog))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
