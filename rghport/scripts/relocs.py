"""Relocation index for the Wii executable (optional input).

CodeWarrior kept full .rela.* sections in the linked executable, so every symbolic reference in code and data is
recoverable exactly -- no heuristics.
"""
from __future__ import annotations

import bisect
import collections
import struct
import sys

from .elf import Elf, be32

R_PPC = {1: 'ADDR32', 2: 'ADDR24', 3: 'ADDR16', 4: 'ADDR16_LO', 5: 'ADDR16_HI',
         6: 'ADDR16_HA', 10: 'REL24', 11: 'REL14', 109: 'EMB_SDA21',
         110: 'EMB_SDA2I16', 111: 'EMB_SDA2REL', 112: 'EMB_SDA21_LO'}


class Rela:
    __slots__ = ('sec', 'off', 'va', 'type', 'sym', 'addend', 'target')

    def __init__(self, sec, off, va, type_, sym, addend, target):
        self.sec, self.off, self.va = sec, off, va
        self.type, self.sym, self.addend, self.target = type_, sym, addend, target

    def __repr__(self):
        return '<%s@%08X %s -> %s+%d = %08X>' % (self.sec, self.va, R_PPC.get(self.type, self.type), self.sym,
                                                self.addend, self.target or 0)


class RelocIndex:
    def __init__(self, elf):
        self.elf = elf
        self.by_va = collections.defaultdict(list)          # referencing address -> [Rela]
        self.to_target = collections.defaultdict(list)      # referenced address  -> [Rela]
        self._build()

    def _build(self):
        e = self.elf
        # symbol table indexed the way relocations expect
        symtab = e.sec('.symtab')
        strtab = e.sections[symtab['link']]
        strs = e.b[strtab['off']:strtab['off'] + strtab['size']]
        nsym = symtab['size'] // 16
        symval = [0] * nsym
        symname = [''] * nsym
        for i in range(nsym):
            o = symtab['off'] + i * 16
            nm = be32(e.b, o)
            ee = strs.index(b'\0', nm)
            symname[i] = strs[nm:ee].decode('latin-1')
            symval[i] = be32(e.b, o + 4)
        self.symval, self.symname = symval, symname

        for s in e.sections:
            if s['type'] != 4 or not s['name'].startswith('.rela'):
                continue
            tgt = e.byname.get(s['name'][5:])
            if tgt is None:
                continue
            # ET_EXEC (e_type==2): r_offset already is the virtual address.
            base = 0 if struct.unpack_from('>H', e.b, 16)[0] == 2 else tgt['addr']
            d = e.b[s['off']:s['off'] + s['size']]
            for i in range(0, len(d), 12):
                r_off = be32(d, i)
                r_info = be32(d, i + 4)
                r_add = struct.unpack_from('>i', d, i + 8)[0]
                sidx = r_info >> 8
                rtype = r_info & 0xFF
                va = base + r_off
                target = symval[sidx] + r_add if sidx < nsym else None
                rl = Rela(tgt['name'], r_off, va, rtype, symname[sidx] if sidx < nsym else '?', r_add, target)
                self.by_va[va].append(rl)
                if target is not None:
                    self.to_target[target].append(rl)
        self._targets = sorted(self.to_target)

    def refs_to(self, va, span=0):
        """Relocations pointing at [va, va+span]."""
        out = []
        i = bisect.bisect_left(self._targets, va)
        while i < len(self._targets) and self._targets[i] <= va + span:
            out.extend(self.to_target[self._targets[i]])
            i += 1
        return out

    def at(self, va):
        return self.by_va.get(va, [])


def main(argv):
    if not argv:
        print("usage: python -m rghport.scripts.relocs EXECUTABLE [SYMBOL]")
        return 2
    e = Elf(argv[0])
    ri = RelocIndex(e)
    print('%d relocation sites' % len(ri.by_va))
    c = collections.Counter(r.type for v in ri.by_va.values() for r in v)
    for k, n in c.most_common():
        print('  %-14s %d' % (R_PPC.get(k, k), n))
    if len(argv) > 1:
        s = e.sym(argv[1])
        print('\nrefs to', s)
        for r in ri.refs_to(s.value):
            print('  ', r, '  in', e.label(r.va))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
