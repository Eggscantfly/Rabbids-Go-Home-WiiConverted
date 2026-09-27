"""PowerPC disassembly for the Wii script module and the Wii executable.

`_iter` is capstone made Gekko-aware (paired-single instructions) and able to step over words capstone refuses; the
`Disassembler` adds what matters when reading CodeWarrior PPC from the executable:
  * branch targets resolved to symbol names
  * relocation-resolved operands (lis/addi pairs, bl targets, sda refs)
  * small-data-area (r2 / r13) accesses resolved to the real global
"""
from __future__ import annotations

import struct
import sys

from capstone import CS_ARCH_PPC, CS_MODE_32, CS_MODE_BIG_ENDIAN, Cs

from .elf import be32

_md = Cs(CS_ARCH_PPC, CS_MODE_32 | CS_MODE_BIG_ENDIAN)
_md.detail = False

# --------------------------------------------------------------------------
# Gekko / Broadway paired-single instructions.
#
# Capstone decodes primary opcodes 4, 56, 57, 60 and 61 as Power ISA VMX/VSX, which is wrong on this CPU -- on Gekko
# they are the paired-single unit, and the engine's vector maths is built out of them.  These are decoded here and take
# priority over capstone for those five opcodes.
# --------------------------------------------------------------------------
_PSQ = {56: 'psq_l', 57: 'psq_lu', 60: 'psq_st', 61: 'psq_stu'}
_PS_A = {10: 'ps_sum0', 11: 'ps_sum1', 12: 'ps_muls0', 13: 'ps_muls1',
         14: 'ps_madds0', 15: 'ps_madds1', 18: 'ps_div', 20: 'ps_sub',
         21: 'ps_add', 23: 'ps_sel', 24: 'ps_res', 25: 'ps_mul',
         26: 'ps_rsqrte', 28: 'ps_msub', 29: 'ps_madd', 30: 'ps_nmsub',
         31: 'ps_nmadd'}
_PS_X = {0: 'ps_cmpu0', 32: 'ps_cmpo0', 40: 'ps_neg', 64: 'ps_cmpu1',
         72: 'ps_mr', 96: 'ps_cmpo1', 136: 'ps_nabs', 264: 'ps_abs',
         528: 'ps_merge00', 560: 'ps_merge01', 592: 'ps_merge10',
         624: 'ps_merge11', 1014: 'dcbz_l'}
_PS_IDX = {6: 'psq_lx', 7: 'psq_stx', 38: 'psq_lux', 39: 'psq_stux'}


def is_gekko(w):
    return (w >> 26) in (4, 56, 57, 60, 61)


def _gekko(w):
    op = w >> 26
    d, a, b = (w >> 21) & 31, (w >> 16) & 31, (w >> 11) & 31
    if op in _PSQ:
        i, wb = (w >> 12) & 7, (w >> 15) & 1
        off = w & 0xFFF
        if off & 0x800:
            off -= 0x1000
        return '%-9s f%d, %d(r%d), %d, qr%d' % (_PSQ[op], d, off, a, wb, i)
    # primary 4
    xo5 = (w >> 1) & 0x1F
    xo6 = (w >> 1) & 0x3F
    xo10 = (w >> 1) & 0x3FF
    c = (w >> 6) & 31
    rc = '.' if w & 1 else ''
    if xo6 in _PS_IDX:
        return '%-9s f%d, r%d, r%d' % (_PS_IDX[xo6] + rc, d, a, b)
    if xo10 in _PS_X:
        nm = _PS_X[xo10]
        if nm.startswith('ps_cmp'):
            return '%-9s cr%d, f%d, f%d' % (nm, d >> 2, a, b)
        if nm == 'dcbz_l':
            return '%-9s r%d, r%d' % (nm, a, b)
        return '%-9s f%d, f%d' % (nm + rc, d, b)
    if xo5 in _PS_A:
        nm = _PS_A[xo5] + rc
        if xo5 in (18, 20, 21):                      # D, A, B
            return '%-9s f%d, f%d, f%d' % (nm, d, a, b)
        if xo5 in (24, 26):                          # D, B
            return '%-9s f%d, f%d' % (nm, d, b)
        if xo5 in (12, 13, 25):                      # D, A, C
            return '%-9s f%d, f%d, f%d' % (nm, d, a, c)
        return '%-9s f%d, f%d, f%d, f%d' % (nm, d, a, c, b)
    return '.word 0x%08X' % w


def _iter(data, va):
    """capstone, but Gekko-aware and able to step over words it refuses."""
    off = 0
    n = len(data) & ~3
    while off < n:
        w = be32(data, off)
        if is_gekko(w):
            yield (va + off, w)
            off += 4
            continue
        got = False
        for ins in _md.disasm(data[off:off + 4], va + off):
            got = True
            yield ins
        off += 4
        if not got:
            yield (va + off - 4, be32(data, off - 4))


class Disassembler:
    """Symbolised listing of the Wii executable (needs its relocation index)."""

    def __init__(self, elf, reloc=None):
        from .relocs import RelocIndex
        self.e = elf
        self.r = reloc if reloc is not None else RelocIndex(elf)
        # small data area bases: r13 -> _SDA_BASE_, r2 -> _SDA2_BASE_
        s13 = elf.sym('_SDA_BASE_')
        s2 = elf.sym('_SDA2_BASE_')
        self.sda = s13.value if s13 else None
        self.sda2 = s2.value if s2 else None

    def resolve(self, va):
        """Relocation-derived comment for the instruction at va, if any."""
        from .relocs import R_PPC
        out = []
        for off in (0, 2):
            for rl in self.r.at(va + off):
                nm = rl.sym
                if rl.addend:
                    nm += '+0x%X' % rl.addend if rl.addend > 0 else '-0x%X' % -rl.addend
                out.append('%s=%s(%08X)' % (R_PPC.get(rl.type, rl.type), nm, rl.target or 0))
        return out

    def sda_name(self, reg, disp, mnem=None):
        base = self.sda if reg == 13 else self.sda2 if reg == 2 else None
        if base is None:
            return None
        va = (base + disp) & 0xFFFFFFFF
        return self.e.label(va) + ' [%08X]' % va + self.fconst(va, mnem)

    def fconst(self, va, mnem):
        """Spell out a float/double literal so maths code can be read."""
        if mnem not in ('lfs', 'lfd'):
            return ''
        n = 4 if mnem == 'lfs' else 8
        d = self.e.read(va, n)
        if not d or len(d) < n:
            return ''
        try:
            v = struct.unpack('>f' if n == 4 else '>d', d)[0]
        except struct.error:
            return ''
        return '  = %g' % v

    def func_range(self, what):
        if isinstance(what, int):
            s = self.e.func_at(what)
            return (s.value, s.size) if s else (what, 0x200)
        s = self.e.sym(what)
        if s is None:
            cands = [x for x in self.e.symbols if x.type == 'FUNC' and x.name.split('__')[0] == what]
            if not cands:
                raise KeyError(what)
            s = cands[0]
        return s.value, s.size

    def lines(self, what, count=None):
        va, size = self.func_range(what)
        if count:
            size = count * 4
        data = self.e.read(va, size)
        out = []
        hi = {}
        for ins in _iter(data, va):
            if ins is None:
                continue
            if isinstance(ins, tuple):          # Gekko or undecodable word
                a, w = ins
                cmt = self.resolve(a)
                out.append('%08X  %08X  %-34s%s' % (a, w, _gekko(w), ('   ; ' + ' | '.join(cmt)) if cmt else ''))
                continue
            a = ins.address
            txt = '%-9s %s' % (ins.mnemonic, ins.op_str)
            cmt = []
            if ins.mnemonic.startswith('b') and '0x' in ins.op_str:
                try:
                    tgt = int(ins.op_str.rsplit('0x', 1)[1].split()[0], 16)
                    lbl = self.e.label(tgt)
                    if not lbl.startswith('8'):
                        cmt.append('-> ' + lbl)
                except ValueError:
                    pass
            cmt += self.resolve(a)
            op = ins.op_str
            if '(r13)' in op or '(r2)' in op:
                try:
                    disp = op.split(',')[-1].split('(')[0].strip()
                    reg = 13 if '(r13)' in op else 2
                    n = self.sda_name(reg, int(disp, 0) if disp else 0, ins.mnemonic)
                    if n:
                        cmt.append(n)
                except ValueError:
                    pass
            if ins.mnemonic == 'lis':
                try:
                    rd = op.split(',')[0].strip()
                    hi[rd] = int(op.split(',')[1], 0) << 16
                except ValueError:
                    pass
            elif ins.mnemonic in ('addi', 'ori', 'lwz', 'stw', 'lfs', 'lfd', 'lbz', 'lha', 'lhz', 'stb', 'sth', 'stfs'):
                try:
                    parts = op.split(',')
                    if ins.mnemonic in ('addi', 'ori'):
                        src, imm = parts[1].strip(), int(parts[2], 0)
                    else:
                        mem = parts[-1].strip()
                        imm = int(mem.split('(')[0], 0)
                        src = mem.split('(')[1].rstrip(')')
                    if src in hi:
                        full = (hi[src] + imm) & 0xFFFFFFFF
                        cmt.append('=%08X %s%s' % (full, self.e.label(full), self.fconst(full, ins.mnemonic)))
                except (ValueError, IndexError):
                    pass
            out.append('%08X  %08X  %-34s%s' % (a, be32(data, a - va), txt, ('   ; ' + ' | '.join(cmt)) if cmt else ''))
        return out


def main(argv):
    if len(argv) < 2:
        print("usage: python -m rghport.scripts.ppcdis EXECUTABLE SYMBOL|0xADDRESS [COUNT]")
        return 2
    from .elf import Elf
    d = Disassembler(Elf(argv[0]))
    what = int(argv[1], 0) if argv[1].startswith('0x') else argv[1]
    for line in d.lines(what, int(argv[2]) if len(argv) > 2 else None):
        print(line)
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
