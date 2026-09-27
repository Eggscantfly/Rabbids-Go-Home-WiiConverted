"""Named disassembly of the Wii script module (the SCR2CPP-compiled scripts).

Every call and data reference is named through the module's own relocations:
    external REL24 sites  -> the imported engine symbol
    internal relocations  -> the export (or literal pool) they point at

    python -m rghport.scripts.rsodump MODULE <regex> [--max N]      exports whose name matches
    python -m rghport.scripts.rsodump MODULE --addr 811234A0 [--size 400]
"""
from __future__ import annotations

import re
import sys

from . import demangle, ppcdis
from .rso import RSO, be32

_R = None


def rso() -> RSO:
    """The configured Wii script module, with an address -> export name map."""
    global _R
    if _R is None:
        from .workspace import ws
        use(ws().rso())
    return _R


def use(r: RSO) -> RSO:
    """Make `r` the module every helper here reads (the workspace does this; tools can pass a file)."""
    global _R
    _R = r
    if not hasattr(r, "addr_to_export"):
        r.addr_to_export = {}
        for x in r.exports:
            va = r.va(x.sec, x.off)
            if va:
                r.addr_to_export.setdefault(va, x.name)
    return r


class Insn:
    __slots__ = ("va", "word", "mn", "ops")

    def __init__(self, va, word, mn, ops):
        self.va, self.word, self.mn, self.ops = va, word, mn, ops

    def __repr__(self):
        return f"{self.va:08X} {self.mn} {self.ops}"


def insns(va: int, size: int):
    r = rso()
    data = r.read(va, size)
    if data is None:
        return
    for ins in ppcdis._iter(data, va):
        if isinstance(ins, tuple):
            a, w = ins
            if ppcdis.is_gekko(w):
                text = ppcdis._gekko(w)
                mn, _, ops = text.partition(" ")
                yield Insn(a, w, mn.strip(), ops.strip())
            elif (w >> 26) == 63 and ((w >> 1) & 0x3FF) in (0, 32):
                mn = "fcmpu" if ((w >> 1) & 0x3FF) == 0 else "fcmpo"
                yield Insn(a, w, mn, "cr%d, f%d, f%d" % ((w >> 23) & 7, (w >> 16) & 31, (w >> 11) & 31))
            else:
                yield Insn(a, w, ".word", f"0x{w:08X}")
        else:
            w = be32(data, ins.address - va)
            yield Insn(ins.address, w, ins.mnemonic, ins.op_str)


def cstring(va: int, limit: int = 1024) -> str:
    b = rso().read(va, limit) or bytes(1)
    return b.split(bytes(1))[0].decode("latin-1")


def note(i: Insn) -> str:
    r = rso()
    e = r.ext_by_site.get(i.va)
    if e:
        return "-> " + e[0]
    t = r.int_by_site.get(i.va) or r.int_by_site.get(i.va + 2)   # ADDR16_* patch the low half-word
    if t:
        return f"~{t[0]} " + r.label(t[1])
    if i.mn in ("bl", "b") or i.mn.startswith("b"):
        m = re.search(r"0x([0-9a-fA-F]+)$", i.ops)
        if m:
            tgt = int(m.group(1), 16)
            nm = r.addr_to_export.get(tgt)
            if nm:
                return "=> " + nm
    return ""


def function_range(name: str):
    r = rso()
    va = r.sym(name)
    return va, r.size_of(name)


def dump(name: str, out=sys.stdout):
    va, size = function_range(name)
    print(f"== {demangle.demangle(name)}   [{name}]  {va:08X} +{size}", file=out)
    for i in insns(va, size):
        n = note(i)
        print(f"   {i.va:08X}  {i.mn:8} {i.ops:28} {n}", file=out)


def main(argv):
    if len(argv) < 2:
        print(__doc__)
        return 2
    use(RSO(argv[0]))
    argv = argv[1:]
    if argv[:1] == ["--addr"]:
        va = int(argv[1], 16)
        size = int(argv[argv.index("--size") + 1]) if "--size" in argv else 256
        for i in insns(va, size):
            print(f"   {i.va:08X}  {i.mn:8} {i.ops:28} {note(i)}")
        return 0
    pat = re.compile(argv[0])
    mx = int(argv[argv.index("--max") + 1]) if "--max" in argv else 5
    r = rso()
    hits = [x.name for x in r.exports if pat.search(x.name) and not x.name.startswith("@")]
    print(f"{len(hits)} matches")
    for nm in hits[:mx]:
        dump(nm)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
