"""The PC game executable as a source of script tables.

The executable exports its C++ symbols (MSVC names) and keeps RTTI, so everything the scripts phase needs from it is
read from the code itself:

  registrations   every `TOOsarray::p_Add(table, key, &record)` site: script natives, keywords, fields and type
                  handlers go to the evaluator table, modifier types to the modifier table.  A record is built on the
                  stack right before the call ({handler, word, extra} for the evaluator, 11 fields for a modifier type);
                  a word of a class method is `mu16_Type + (id << 16)`, the type coming from the registrar: the virtual
                  RegisterScript (vtable slot 13) of the class an engine type constructs, or the class of an exported
                  registrar.  A RegisterScript of a class no engine type constructs never runs (inactive).
  engine types    ViD::RegisterModifiers: id, constant name, class name, name, extension, category, sizes, and the
                  constructor called before each registration (its vtable gives the class's RegisterScript)
  handlers        `u32* handler(u32* node)`: the script value / address stacks it pops and pushes give the script
                  arity, argument and return sizes; the node pointer it returns gives the node size; a call or jump to
                  an exported `<name>_C` implementation names it
  node sizes      SCR::pu32_NextNode is one compare tree over the node word

    python -m rghport.scripts.pcexe EXECUTABLE [--json OUT]
"""
from __future__ import annotations

import bisect
import collections
import json
import re
import struct
import sys

from capstone import CS_ARCH_X86, CS_MODE_32, Cs
from capstone.x86 import X86_OP_IMM, X86_OP_MEM, X86_OP_REG

P_ADD = "?p_Add@TOOsarray@@QAEPAXKPAX@Z"
NEXT_NODE = "?pu32_NextNode@SCR@@SAPAKPAK@Z"
MEMSET = "?STD_memset@@YAPAXPAXKK@Z"
REGISTER_MODIFIERS = "?RegisterModifiers@ViD@@QAEXXZ"
MODIFIER_TABLE = "?ViD_go_Modifiers@@3VTOOsarray@@A"
STACK_GLOBALS = {"?SCR_gu32_NumGlobalStack_Val@@3KA": "val", "?SCR_gu32_NumGlobalStack_Addr@@3KA": "addr"}
KEYWORD_REGISTRAR = "?RegisterKeywords@SCR@@AAEXXZ"
REGISTER_SCRIPT_SLOT = 13          # vtable slot of the virtual RegisterScript of every modifier class
MODIFIER_FIELDS = ("u32_ID", "pz_CstName", "pz_ClassName", "pz_Name", "pz_Ext", "pz_Categ", "po_Mdf", "u32_Prio",
                   "u32_Flags", "u32_Limit", "u32_Size")


class PE:
    def __init__(self, path: str):
        self.path = path
        with open(path, "rb") as f:
            self.b = b = f.read()
        if b[:2] != b"MZ":
            raise ValueError("%s: not a PE file" % path)
        pe = struct.unpack_from("<I", b, 0x3C)[0]
        if b[pe:pe + 4] != b"PE\0\0":
            raise ValueError("%s: not a PE file" % path)
        nsec = struct.unpack_from("<H", b, pe + 6)[0]
        optsz = struct.unpack_from("<H", b, pe + 20)[0]
        opt = pe + 24
        self.base = struct.unpack_from("<I", b, opt + 28)[0]
        exp_rva = struct.unpack_from("<I", b, opt + 96)[0]
        self.sections = []
        for i in range(nsec):
            name, vsize, va, rsize, rptr = struct.unpack_from("<8sIIII", b, opt + optsz + 40 * i)
            self.sections.append((name.rstrip(b"\0").decode("latin-1"), self.base + va, max(vsize, rsize), rptr, rsize))
        self.exports: dict[str, int] = {}
        self.names: dict[int, str] = {}
        if exp_rva:
            e = self.off(self.base + exp_rva)
            nfun, nnames, afun, anames, aord = struct.unpack_from("<IIIII", b, e + 20)
            for i in range(nnames):
                p = self.off(self.base + struct.unpack_from("<I", b, self.off(self.base + anames) + 4 * i)[0])
                name = b[p:b.index(b"\0", p)].decode("latin-1")
                ordi = struct.unpack_from("<H", b, self.off(self.base + aord) + 2 * i)[0]
                va = self.base + struct.unpack_from("<I", b, self.off(self.base + afun) + 4 * ordi)[0]
                self.exports[name] = va
                self.names.setdefault(va, name)
        self.md = Cs(CS_ARCH_X86, CS_MODE_32)
        self.md.detail = True
        self._vtables = None
        self._starts = None
        self._cleanup = {}

    # ---- addresses --------------------------------------------------------
    def off(self, va: int):
        for _n, sva, size, rptr, rsize in self.sections:
            if sva <= va < sva + size:
                o = va - sva
                return rptr + o if o < rsize else None
        return None

    def section(self, name: str):
        for s in self.sections:
            if s[0] == name:
                return s
        return None

    def in_text(self, va) -> bool:
        t = self.section(".text")
        return isinstance(va, int) and t is not None and t[1] <= va < t[1] + t[2]

    def read(self, va: int, n: int) -> bytes:
        o = self.off(va)
        return b"" if o is None else self.b[o:o + n]

    def u32(self, va: int):
        d = self.read(va, 4)
        return struct.unpack("<I", d)[0] if len(d) == 4 else None

    def cstring(self, va: int, limit: int = 256):
        o = self.off(va)
        if o is None:
            return None
        e = self.b.find(b"\0", o, o + limit)
        return self.b[o:e if e >= 0 else o + limit].decode("latin-1")

    def export_at(self, va: int):
        return self.names.get(va)

    def insns(self, va: int, size: int):
        return self.md.disasm(self.read(va, size), va)

    # ---- RTTI -----------------------------------------------------------
    def vtables(self) -> dict:
        """{vtable VA: class name} for every vtable with an MSVC RTTI complete object locator
        (TypeDescriptor `.?AV<name>@@` <- locator {signature 0, ..., TypeDescriptor*} <- vtable[-1])."""
        if self._vtables is not None:
            return self._vtables
        tds = {}
        for sec in self.sections:
            if sec[0] not in (".data", ".rdata"):
                continue
            blob = self.b[sec[3]:sec[3] + sec[4]]
            i = blob.find(b".?AV")
            while i != -1:
                end = blob.find(b"\0", i)
                name = blob[i:end].decode("latin-1")
                if name.endswith("@@"):
                    tds[sec[1] + i - 8] = name[4:-2]
                i = blob.find(b".?AV", i + 1)
        rdata = self.section(".rdata")
        blob = self.b[rdata[3]:rdata[3] + rdata[4]]
        cols = {}
        for td, name in tds.items():
            pat = struct.pack("<I", td)
            i = blob.find(pat)
            while i != -1:
                if i >= 12 and i % 4 == 0 and struct.unpack_from("<I", blob, i - 12)[0] == 0:
                    cols[rdata[1] + i - 12] = name
                i = blob.find(pat, i + 1)
        out = {}
        for col, name in cols.items():
            pat = struct.pack("<I", col)
            i = blob.find(pat)
            while i != -1:
                if i % 4 == 0 and self.in_text(self.u32(rdata[1] + i + 4)):
                    out[rdata[1] + i + 4] = name
                i = blob.find(pat, i + 1)
        self._vtables = out
        return out

    def vtable_functions(self, vt: int) -> list:
        out = []
        while self.in_text(self.u32(vt + 4 * len(out))):
            out.append(self.u32(vt + 4 * len(out)))
        return out

    def rtti_class_of_slot(self, func_va: int, slot: int = REGISTER_SCRIPT_SLOT):
        """Class name of the (first) vtable whose `slot` holds func_va."""
        for vt in sorted(self.vtables()):
            if self.u32(vt + 4 * slot) == func_va:
                return self._vtables[vt]
        return None

    def known_starts(self) -> list:
        """Exported code addresses and every virtual function: sorted."""
        if self._starts is None:
            s = {va for va in self.names if self.in_text(va)}
            for vt in self.vtables():
                s.update(self.vtable_functions(vt))
            self._starts = sorted(s)
        return self._starts


def is_game_executable(path: str) -> bool:
    try:
        pe = PE(path)
    except (OSError, ValueError, struct.error):
        return False
    return P_ADD in pe.exports and NEXT_NODE in pe.exports


# ============================================================================
# MSVC names
# ============================================================================
_BASIC = {"C": "signed char", "D": "char", "E": "unsigned char", "F": "short", "G": "unsigned short", "H": "int",
          "I": "unsigned int", "J": "long", "K": "unsigned long", "M": "float", "N": "double", "X": "void",
          "_N": "bool", "_J": "__int64", "_K": "unsigned __int64"}


def msvc_function(name: str):
    """`?Name@@YA<ret><params>@Z` (free function) -> (name, return type, [parameter types]) or None.  Covers the
    C-style implementation exports: basic types, pointers / references with cv, classes, structs, enums and
    back-references."""
    m = re.match(r"^\?(\w+)@@Y[AG](.*)$", name)
    if not m:
        return None
    s, i = m.group(2), 0
    seen = []

    def typ():
        nonlocal i
        if i >= len(s):
            return None
        c = s[i]
        if c.isdigit():
            i += 1
            k = int(c)
            return seen[k] if k < len(seen) else "?"
        if c == "_" and s[i:i + 2] in _BASIC:
            i += 2
            return _BASIC[s[i - 2:i]]
        if c in "PQA":
            i += 1
            cv = s[i]
            i += 1
            inner = typ()
            t = "%s%s%s" % ("const " if cv in "BD" else "", inner, "&" if c == "A" else "*")
            if len(t) > 1:
                seen.append(t)
            return t
        if c in "VU":
            j = s.index("@@", i)
            t = s[i + 1:j]
            i = j + 2
            if t not in seen:
                seen.append(t)
            return t
        if c == "W":
            j = s.index("@@", i)
            t = s[i + 2:j]
            i = j + 2
            return t
        if c in _BASIC:
            i += 1
            return _BASIC[c]
        return None

    ret = typ()
    params = []
    while i < len(s) and s[i] not in "@Z":
        if s[i] == "X" and not params:
            i += 1
            break
        t = typ()
        if t is None:
            return None
        params.append(t)
    return m.group(1), ret, params


def implementation_name(export: str):
    """`?OBJ_PosGet_C@@YA...` -> OBJ_PosGet"""
    m = re.match(r"^\?(\w+)_C@@Y", export)
    return m.group(1) if m else None


# ============================================================================
# code walking
# ============================================================================
def _is_filler(ins) -> bool:
    """Alignment filler between or inside functions: nop, int3, `lea r, [r]`, `mov r, r`."""
    ops = ins.operands
    if ins.mnemonic in ("nop", "int3"):
        return True
    if ins.mnemonic == "lea" and len(ops) == 2 and ops[0].type == X86_OP_REG and ops[1].type == X86_OP_MEM:
        m = ops[1].mem
        return m.base == ops[0].reg and not m.index and m.disp == 0
    if ins.mnemonic in ("mov", "xchg") and len(ops) == 2 and ops[0].type == X86_OP_REG and ops[1].type == X86_OP_REG:
        return ops[0].reg == ops[1].reg
    return False


def _branch_targets(pe: PE, ins, lo: int, hi: int) -> list:
    """Direct branch targets and `jmp [index*4 + table]` table entries of ins that lie in [lo, hi)."""
    ops = ins.operands
    if not (ins.mnemonic.startswith("j") and ops):
        return []
    if ops[0].type == X86_OP_IMM:
        t = ops[0].imm & 0xFFFFFFFF
        return [t] if lo <= t < hi else []
    m = ops[0].mem if ops[0].type == X86_OP_MEM else None
    out = []
    if ins.mnemonic == "jmp" and m is not None and m.index and not m.base and m.scale == 4:
        for k in range(1024):
            p = pe.u32((m.disp & 0xFFFFFFFF) + 4 * k)
            if p is None or not (lo <= p < hi):
                break
            out.append(p)
    return out


def callee_cleanup(pe: PE, target: int, depth: int = 0) -> int:
    """Argument bytes a function pops on return: N of its `ret N` (a function that only leaves through a tail jump
    inherits the jump target's)."""
    if target in pe._cleanup:
        return pe._cleanup[target]
    pe._cleanup[target] = 0
    n, tail = None, None
    targets, ended = set(), False
    for ins in pe.insns(target, 0x4000):
        if ended and not _is_filler(ins):
            if ins.address not in targets:
                break
            ended = False
        mn, ops = ins.mnemonic, ins.operands
        if mn == "ret":
            n = ops[0].imm if ops else 0
            break
        if mn.startswith("j") and ops and ops[0].type == X86_OP_IMM:
            t = ops[0].imm & 0xFFFFFFFF
            if ins.address < t < target + 0x4000:
                targets.add(t)
            elif mn == "jmp" and not (target <= t < ins.address):
                tail = t
        if mn == "jmp":
            ended = True
    if n is None:
        n = callee_cleanup(pe, tail, depth + 1) if tail is not None and depth < 4 else 0
    pe._cleanup[target] = n
    return n


def _call_sites(pe: PE, target: int):
    text = pe.section(".text")
    _n, sva, size, rptr, rsize = text
    code = pe.b[rptr:rptr + rsize]
    out = []
    i = code.find(b"\xE8")
    while i != -1 and i + 5 <= len(code):
        rel = struct.unpack_from("<i", code, i + 1)[0]
        if (sva + i + 5 + rel) & 0xFFFFFFFF == target:
            out.append(sva + i)
        i = code.find(b"\xE8", i + 1)
    return out


def _refine_start(pe: PE, start: int, va: int):
    """Walk from a known function start to va: code after a ret / jmp that no earlier branch reaches begins a new
    function.  The start of the function holding va, or None if the linear decode does not land on va."""
    targets, ended, cur = set(), False, start
    for ins in pe.insns(start, va - start + 16):
        if ended and not _is_filler(ins):
            if ins.address not in targets:
                cur = ins.address
            ended = False
        if ins.address >= va:
            return cur if ins.address == va else None
        targets.update(_branch_targets(pe, ins, start, va + 0x10000))
        if ins.mnemonic in ("ret", "jmp"):
            ended = True
    return None


def function_start(pe: PE, va: int, limit: int = 0x8000):
    """The start of the function holding va: from the nearest known start (export or virtual function) with no int3
    padding in between, else from the padding (or the ret) that precedes va; confirmed by a linear decode."""
    starts = pe.known_starts()
    i = bisect.bisect_right(starts, va) - 1
    if i >= 0:
        s = starts[i]
        if va - s < limit and b"\xCC\xCC" not in pe.read(s, va - s):
            r = _refine_start(pe, s, va)
            if r is not None:
                return r
    p = va
    lo = max(va - limit, 0)
    while p > lo:
        prev = pe.read(p - 1, 1)
        boundary = prev in (b"\xCC", b"\x90") and pe.read(p - 2, 1) in (b"\xCC", b"\x90", b"\xC3")
        boundary = boundary or prev == b"\xC3" or pe.read(p - 3, 1) == b"\xC2"
        if boundary:
            q = None
            for ins in pe.insns(p, va - p + 16):
                if ins.address >= va:
                    q = ins.address
                    break
            if q == va:
                return p
        p -= 1
    return None


# ============================================================================
# registrations
# ============================================================================
class Reg:
    __slots__ = ("site", "registrar", "registrar_name", "cls", "cls_from", "table", "key", "fields", "this_type",
                 "events")

    def __init__(self, **kw):
        for k in self.__slots__:
            setattr(self, k, kw.get(k))

    def as_dict(self):
        return {k: getattr(self, k) for k in self.__slots__}


class _Emu:
    """Just enough 32-bit x86 for registration blocks: registers, esp-relative stack slots, pushes and the calls
    between the blocks.  Values: int, ("type", imm) for mu16_Type + imm, ("sp", offset) for stack addresses."""

    def __init__(self, pe: PE):
        self.pe = pe
        self.reg = {}
        self.sp = 0
        self.stack = {}

    def val(self, ins, op):
        if op.type == X86_OP_IMM:
            return op.imm & 0xFFFFFFFF
        if op.type == X86_OP_REG:
            return self.reg.get(ins.reg_name(op.reg))
        if op.type == X86_OP_MEM:
            base = ins.reg_name(op.mem.base) if op.mem.base else None
            if base == "esp" and not op.mem.index:
                return self.stack.get(self.sp + op.mem.disp)
            return None
        return None

    def step(self, ins):
        ops = ins.operands
        mn = ins.mnemonic
        if mn == "push":
            self.sp -= 4
            self.stack[self.sp] = self.val(ins, ops[0])
        elif mn == "pop":
            if ops[0].type == X86_OP_REG:
                self.reg[ins.reg_name(ops[0].reg)] = self.stack.get(self.sp)
            self.sp += 4
        elif mn in ("add", "sub") and ops[0].type == X86_OP_REG and ins.reg_name(ops[0].reg) == "esp" \
                and ops[1].type == X86_OP_IMM:
            self.sp += ops[1].imm if mn == "add" else -ops[1].imm
        elif mn == "add" and ops[0].type == X86_OP_REG and ops[1].type == X86_OP_IMM:
            r = ins.reg_name(ops[0].reg)
            v = self.reg.get(r)
            if isinstance(v, tuple) and v[0] == "type":
                self.reg[r] = ("type", (v[1] + ops[1].imm) & 0xFFFFFFFF)
            elif isinstance(v, int):
                self.reg[r] = (v + ops[1].imm) & 0xFFFFFFFF
            else:
                self.reg[r] = None
        elif mn == "mov":
            if ops[0].type == X86_OP_REG:
                self.reg[ins.reg_name(ops[0].reg)] = self.val(ins, ops[1])
            elif ops[0].type == X86_OP_MEM:
                base = ins.reg_name(ops[0].mem.base) if ops[0].mem.base else None
                if base == "esp" and not ops[0].mem.index:
                    self.stack[self.sp + ops[0].mem.disp] = self.val(ins, ops[1])
        elif mn == "movzx" and ops[0].type == X86_OP_REG and ops[1].type == X86_OP_MEM and ops[1].mem.disp == 0xA \
                and ops[1].size == 2:
            self.reg[ins.reg_name(ops[0].reg)] = ("type", 0)
        elif mn == "lea" and ops[0].type == X86_OP_REG and ops[1].type == X86_OP_MEM:
            base = ins.reg_name(ops[1].mem.base) if ops[1].mem.base else None
            self.reg[ins.reg_name(ops[0].reg)] = ("sp", self.sp + ops[1].mem.disp) if base == "esp" else None
        elif mn == "xor" and ops[0].type == X86_OP_REG and ops[1].type == X86_OP_REG and ops[0].reg == ops[1].reg:
            self.reg[ins.reg_name(ops[0].reg)] = 0
        elif mn == "or" and ops[0].type == X86_OP_REG and ops[1].type == X86_OP_IMM and ops[1].imm & 0xFFFFFFFF == 0xFFFFFFFF:
            self.reg[ins.reg_name(ops[0].reg)] = 0xFFFFFFFF
        elif mn == "call":
            if ops and ops[0].type == X86_OP_IMM:
                self.sp += callee_cleanup(self.pe, ops[0].imm & 0xFFFFFFFF)
            for r in ("eax", "ecx", "edx"):
                self.reg[r] = None
        elif ops and ops[0].type == X86_OP_REG:
            self.reg[ins.reg_name(ops[0].reg)] = None


def scan_registrations(pe: PE):
    p_add = pe.exports[P_ADD]
    sites = sorted(_call_sites(pe, p_add))
    # group the call sites by padding (a site with no padding since the previous one continues its code run); the
    # walk over a run splits it into functions
    groups, prev = [], None
    for site in sites:
        if prev is not None and site - prev < 0x2000 and b"\xCC\xCC" not in pe.read(prev, site - prev):
            groups[-1][1].append(site)
        else:
            groups.append([function_start(pe, site), [site]])
        prev = site
    regs = []
    for gstart, fsites in groups:
        start = gstart if gstart is not None else max(fsites[0] - 0x40, 0)
        fstart = gstart
        emu = _Emu(pe)
        want = set(fsites)
        end = fsites[-1] + 5
        events, at, ended = [], {}, False
        for ins in pe.insns(start, end - start):
            a = ins.address
            if a >= end:
                break
            if ended and not _is_filler(ins):
                if a in at:
                    emu.sp = at[a]
                else:
                    fstart, emu, events, at = a, _Emu(pe), [], {}
                ended = False
            ops = ins.operands
            if a in want:
                key = emu.stack.get(emu.sp)
                rec = emu.stack.get(emu.sp + 4)
                fields = []
                if isinstance(rec, tuple) and rec[0] == "sp":
                    fields = [emu.stack.get(rec[1] + 4 * k) for k in range(len(MODIFIER_FIELDS))]
                regs.append(Reg(site=a, registrar=fstart, table=emu.reg.get("ecx"), key=key, fields=fields,
                                this_type=isinstance(key, tuple) and key[0] == "type", events=events))
                events = []
            elif ins.mnemonic == "call" and ops and ops[0].type == X86_OP_IMM:
                events.append(("call", ops[0].imm & 0xFFFFFFFF))
            elif ins.mnemonic == "mov" and len(ops) == 2 and ops[0].type == X86_OP_MEM and ops[0].mem.disp == 0 \
                    and not ops[0].mem.index and ops[1].type == X86_OP_IMM:
                events.append(("store", ops[1].imm & 0xFFFFFFFF))
            for t in _branch_targets(pe, ins, start, end + 0x10000):
                if t > a:
                    at.setdefault(t, emu.sp)
            if ins.mnemonic in ("ret", "jmp"):
                ended = True
            emu.step(ins)
    # registrar names and classes
    for r in regs:
        name = pe.export_at(r.registrar) if r.registrar else None
        r.registrar_name = name
        m = re.match(r"^\?\w+@(\w+)@@", name) if name else None
        if m:
            r.cls, r.cls_from = m.group(1), "export"
        elif r.registrar:
            r.cls = pe.rtti_class_of_slot(r.registrar)
            r.cls_from = "vtable" if r.cls else None
    return regs


# ============================================================================
# engine types
# ============================================================================
def constructor_vtable(pe: PE, ctor: int, limit: int = 0x400):
    """The RTTI vtable a constructor installs last (`mov dword ptr [reg], vtable`)."""
    vts = pe.vtables()
    found = None
    for ins in pe.insns(ctor, limit):
        if ins.mnemonic in ("ret", "int3"):
            break
        ops = ins.operands
        if ins.mnemonic == "mov" and len(ops) == 2 and ops[0].type == X86_OP_MEM and ops[0].mem.disp == 0 \
                and not ops[0].mem.index and ops[1].type == X86_OP_IMM and (ops[1].imm & 0xFFFFFFFF) in vts:
            found = ops[1].imm & 0xFFFFFFFF
    return found


def engine_types(pe: PE, regs=None):
    """Modifier types: the record fields, plus the class the registration constructs (the last vtable installed
    before the registration, by a constructor call or inline) and that class's RegisterScript."""
    table = pe.exports.get(MODIFIER_TABLE)
    regs = regs if regs is not None else scan_registrations(pe)
    vts = pe.vtables()
    out = {}
    for r in regs:
        if r.table != table or not isinstance(r.key, int):
            continue
        rec = {}
        for name, v in zip(MODIFIER_FIELDS, r.fields):
            if name.startswith("pz_"):
                rec[name] = pe.cstring(v) if isinstance(v, int) and pe.off(v) is not None else None
            else:
                rec[name] = v if isinstance(v, int) else None
        rec["u32_ID"] = r.key
        ctor = vt = None
        for kind, v in reversed(r.events or []):
            if kind == "store" and v in vts:
                vt = v
            elif kind == "call":
                vt = constructor_vtable(pe, v)
                ctor = v if vt else None
            if vt:
                break
        rec["constructor"] = ctor
        rec["vtable"] = vt
        rec["vtable_class"] = vts.get(vt)
        rec["register_script"] = pe.u32(vt + 4 * REGISTER_SCRIPT_SLOT) if vt else None
        out[r.key] = rec
    return out


# ============================================================================
# handlers
# ============================================================================
def handler_node_sizes(pe: PE, va: int, limit: int = 0x1000, depth: int = 0):
    """The node pointer a handler returns, from a walk with esp and register tracking (state carried to forward
    branch targets, callee argument cleanup from their `ret N`): a set of ("fixed", words), ("var", field, scale,
    add words), ("const", value) or ("unknown",) values of eax at its returns; a tail jump inherits its target's."""
    sp = 0
    reg = {}
    at = {}
    out = set()
    ended = False
    pending = 0                    # bytes pushed since the last call / esp adjustment (indirect call cleanup guess)
    indirect = None
    for ins in pe.insns(va, limit):
        mn, ops = ins.mnemonic, ins.operands
        a = ins.address
        if ended:
            if _is_filler(ins):
                continue
            if a not in at:
                break
            sp, reg = at[a][0], dict(at[a][1])
            ended = False
        elif a in at:
            reg = {k: v for k, v in reg.items() if at[a][1].get(k) == v}

        def rname(op):
            return ins.reg_name(op.reg) if op.type == X86_OP_REG else None
        if mn == "push":
            sp -= 4
            pending += 4
        elif mn == "pop":
            sp += 4
            pending = max(pending - 4, 0)
            if ops and ops[0].type == X86_OP_REG:
                reg.pop(rname(ops[0]), None)
        elif mn in ("add", "sub") and len(ops) == 2 and rname(ops[0]) == "esp" and ops[1].type == X86_OP_IMM:
            if not (mn == "add" and indirect is not None and ops[1].imm == indirect):
                sp += ops[1].imm if mn == "add" else -ops[1].imm
            else:
                sp -= indirect     # the indirect callee did not pop: undo the guess, then apply the caller cleanup
                sp += ops[1].imm
            pending = 0
        elif mn == "mov" and len(ops) == 2 and ops[0].type == X86_OP_REG and ops[1].type == X86_OP_MEM:
            base = ins.reg_name(ops[1].mem.base) if ops[1].mem.base else None
            src = reg.get(base) if base else None
            if base == "esp" and not ops[1].mem.index and sp + ops[1].mem.disp == 4:
                reg[rname(ops[0])] = ("node", 0)
            elif isinstance(src, tuple) and src[0] == "node" and not ops[1].mem.index:
                reg[rname(ops[0])] = ("field", (src[1] + ops[1].mem.disp) // 4)
            else:
                reg.pop(rname(ops[0]), None)
        elif mn == "mov" and len(ops) == 2 and ops[0].type == X86_OP_REG and ops[1].type == X86_OP_REG:
            v = reg.get(rname(ops[1]))
            if v is None:
                reg.pop(rname(ops[0]), None)
            else:
                reg[rname(ops[0])] = v
        elif mn == "mov" and len(ops) == 2 and ops[0].type == X86_OP_REG and ops[1].type == X86_OP_IMM:
            reg[rname(ops[0])] = ("const", ops[1].imm & 0xFFFFFFFF)
        elif mn == "xor" and len(ops) == 2 and ops[0].type == X86_OP_REG and ops[1].type == X86_OP_REG \
                and ops[0].reg == ops[1].reg:
            reg[rname(ops[0])] = ("const", 0)
        elif mn in ("add", "sub") and len(ops) == 2 and ops[0].type == X86_OP_REG and ops[1].type == X86_OP_IMM:
            v = reg.get(rname(ops[0]))
            if isinstance(v, tuple) and v[0] == "node":
                reg[rname(ops[0])] = ("node", v[1] + (ops[1].imm if mn == "add" else -ops[1].imm))
            else:
                reg.pop(rname(ops[0]), None)
        elif mn == "lea" and len(ops) == 2 and ops[1].type == X86_OP_MEM:
            m = ops[1].mem
            base = reg.get(ins.reg_name(m.base)) if m.base else None
            idx = reg.get(ins.reg_name(m.index)) if m.index else None
            if isinstance(base, tuple) and base[0] == "node" and not m.index:
                reg[rname(ops[0])] = ("node", base[1] + m.disp)
            elif isinstance(base, tuple) and base[0] == "node" and isinstance(idx, tuple) and idx[0] == "field":
                reg[rname(ops[0])] = ("var", idx[1], m.scale // 4 if m.scale >= 4 else 0, (base[1] + m.disp) // 4)
            else:
                reg.pop(rname(ops[0]), None)
        elif mn == "ret":
            v = reg.get("eax")
            if isinstance(v, tuple) and v[0] == "node":
                out.add(("fixed", v[1] // 4))
            elif isinstance(v, tuple) and v[0] in ("var", "const"):
                out.add(v)
            else:
                out.add(("unknown",))
            ended = True
        elif mn == "call":
            if ops and ops[0].type == X86_OP_IMM:
                sp += callee_cleanup(pe, ops[0].imm & 0xFFFFFFFF)
                indirect = None
            else:
                sp += pending
                indirect = pending
            pending = 0
            for r in ("eax", "ecx", "edx"):
                reg.pop(r, None)
            continue
        elif ops and ops[0].type == X86_OP_REG:
            reg.pop(rname(ops[0]), None)
        if mn != "call" and not (mn in ("add", "sub") and len(ops) == 2 and rname(ops[0]) == "esp"):
            indirect = None if mn not in ("push", "pop") else indirect
        if mn.startswith("j") and ops:
            for t in _branch_targets(pe, ins, va, va + limit):
                if t > a:
                    if t in at:
                        at[t] = (at[t][0], {k: v for k, v in at[t][1].items() if reg.get(k) == v})
                    else:
                        at[t] = (sp, dict(reg))
            if mn == "jmp":
                if ops[0].type == X86_OP_IMM:
                    t = ops[0].imm & 0xFFFFFFFF
                    if not (va <= t < va + limit):
                        if sp == 0 and depth < 3:
                            out |= handler_node_sizes(pe, t, limit, depth + 1)
                        else:
                            out.add(("unknown",))
                ended = True
    return out


def analyze_handler(pe: PE, va: int, limit: int = 600):
    """Stack effects and named callees of a native handler (linear decode up to its padding)."""
    glob = {pe.exports[n]: k for n, k in STACK_GLOBALS.items() if n in pe.exports}
    events = []            # ("val"|"addr", delta)
    callees = []
    reg_src = {}           # register -> ("val"|"addr", pending delta)
    for ins in pe.insns(va, limit):
        mn, ops = ins.mnemonic, ins.operands
        if mn == "int3":
            break
        if mn in ("add", "sub") and len(ops) == 2 and ops[0].type == X86_OP_MEM and not ops[0].mem.base \
                and ops[0].mem.disp & 0xFFFFFFFF in glob and ops[1].type == X86_OP_IMM:
            d = ops[1].imm if mn == "add" else -ops[1].imm
            events.append((glob[ops[0].mem.disp & 0xFFFFFFFF], d))
        elif mn == "mov" and len(ops) == 2 and ops[0].type == X86_OP_REG and ops[1].type == X86_OP_MEM \
                and not ops[1].mem.base and ops[1].mem.disp & 0xFFFFFFFF in glob:
            reg_src[ins.reg_name(ops[0].reg)] = [glob[ops[1].mem.disp & 0xFFFFFFFF], 0]
        elif mn in ("add", "sub") and len(ops) == 2 and ops[0].type == X86_OP_REG and ops[1].type == X86_OP_IMM \
                and ins.reg_name(ops[0].reg) in reg_src:
            reg_src[ins.reg_name(ops[0].reg)][1] += ops[1].imm if mn == "add" else -ops[1].imm
        elif mn == "mov" and len(ops) == 2 and ops[0].type == X86_OP_MEM and not ops[0].mem.base \
                and ops[0].mem.disp & 0xFFFFFFFF in glob and ops[1].type == X86_OP_REG:
            src = reg_src.get(ins.reg_name(ops[1].reg))
            if src and src[0] == glob[ops[0].mem.disp & 0xFFFFFFFF] and src[1]:
                events.append((src[0], src[1]))
                reg_src.pop(ins.reg_name(ops[1].reg), None)
        elif mn in ("call", "jmp") and ops and ops[0].type == X86_OP_IMM:
            name = pe.export_at(ops[0].imm & 0xFFFFFFFF)
            if name:
                callees.append(name)
    pops_val = [-d for k, d in events if k == "val" and d < 0]
    pops_addr = [-d for k, d in events if k == "addr" and d < 0]
    push_val = [d for k, d in events if k == "val" and d > 0]
    impl = [implementation_name(c) for c in callees if implementation_name(c)]
    return dict(events=events, argsizes=pops_val, nargs=sum(pops_addr) if pops_addr else len(pops_val),
                retsize=push_val[-1] if push_val else 0, callees=callees, implementations=impl)


# ============================================================================
# node sizes
# ============================================================================
def node_size(pe: PE, word: int, limit: int = 400):
    """Evaluate SCR::pu32_NextNode for one word: ("fixed", n words) or ("var", index, scale, add) or None."""
    va = pe.exports[NEXT_NODE]
    pc = va
    eax = None
    flags = None
    loads = {}
    for _ in range(limit):
        ins = next(iter(pe.insns(pc, 16)), None)
        if ins is None:
            return None
        mn, ops = ins.mnemonic, ins.operands
        nxt = ins.address + ins.size
        if mn == "mov" and ops[0].type == X86_OP_REG and ops[1].type == X86_OP_MEM:
            dst = ins.reg_name(ops[0].reg)
            base = ins.reg_name(ops[1].mem.base) if ops[1].mem.base else None
            if base == "esp":
                loads[dst] = "node"
            elif base == "ecx" and ops[1].mem.disp == 0 and dst == "eax":
                eax = word
            elif base == "ecx":
                loads[dst] = ("field", ops[1].mem.disp // 4)
        elif mn == "cmp" and ops[0].type == X86_OP_REG and ops[1].type == X86_OP_IMM:
            flags = (eax, ops[1].imm & 0xFFFFFFFF)
        elif mn.startswith("j") and ops[0].type == X86_OP_IMM:
            a, b = flags if flags else (None, None)
            taken = {"ja": a > b, "jae": a >= b, "jb": a < b, "jbe": a <= b, "je": a == b, "jne": a != b,
                     "jmp": True}.get(mn) if a is not None else (True if mn == "jmp" else None)
            if taken is None:
                return None
            if taken:
                nxt = ops[0].imm & 0xFFFFFFFF
        elif mn == "lea" and ops[0].type == X86_OP_REG and ops[1].type == X86_OP_MEM:
            m = ops[1].mem
            base = ins.reg_name(m.base) if m.base else None
            index = ins.reg_name(m.index) if m.index else None
            if base != "ecx":
                return None
            if index is None:
                result = ("fixed", m.disp // 4)
            else:
                src = loads.get(index)
                if not (isinstance(src, tuple) and src[0] == "field"):
                    return None
                result = ("var", src[1], m.scale // 4 if m.scale >= 4 else m.scale, m.disp // 4)
            loads[ins.reg_name(ops[0].reg)] = result
        elif mn == "ret":
            return loads.get("eax") if isinstance(loads.get("eax"), tuple) and loads["eax"][0] in ("fixed", "var") else None
        elif mn == "call":
            return None
        pc = nxt
    return None


# ============================================================================
def build(pe: PE, log=print):
    regs = scan_registrations(pe)
    types = engine_types(pe, regs)
    class_to_type = {}
    for k, v in sorted(types.items()):
        for c in (v.get("vtable_class"), v.get("pz_ClassName")):
            if c:
                class_to_type.setdefault(c, k)
    rs_count = collections.Counter(v.get("register_script") for v in types.values())
    registrar_to_type = {v["register_script"]: k for k, v in types.items()
                         if v.get("register_script") and rs_count[v["register_script"]] == 1}
    evaluator = collections.Counter(r.table for r in regs if r.table != pe.exports.get(MODIFIER_TABLE)).most_common(1)
    eval_table = evaluator[0][0] if evaluator else None
    words, problems, inactive = {}, [], collections.Counter()
    for r in regs:
        if r.table != eval_table:
            continue
        handler = r.fields[0] if r.fields else None
        if r.this_type:
            scope = registrar_to_type.get(r.registrar)
            virtual = r.cls_from == "vtable" or (r.registrar_name or "").startswith("?RegisterScript@")
            if scope is None and not virtual:
                scope = class_to_type.get(r.cls)
            if scope is None:
                if virtual:
                    inactive[(r.cls, r.registrar)] += 1
                else:
                    problems.append("no engine type for class %s (registrar %08X)" % (r.cls, r.registrar or 0))
                continue
            word = (r.key[1] + scope) & 0xFFFFFFFF
        elif isinstance(r.key, int):
            word = r.key
        else:
            problems.append("unresolved key at %08X" % r.site)
            continue
        if word in words:
            problems.append("word %08X registered twice (%08X, %08X)" % (word, words[word]["site"], r.site))
        words[word] = dict(handler=handler, registrar=r.registrar_name or ("%08X" % (r.registrar or 0)), cls=r.cls,
                           keyword=r.registrar_name == KEYWORD_REGISTRAR, site=r.site)
    inactive = [dict(cls=c, registrar=reg, words=n) for (c, reg), n in sorted(inactive.items(), key=lambda kv: kv[0][1])]
    log("registrations: %d sites, %d evaluator words, %d engine types, %d inactive registrars (%d words), %d problems" % (
        len(regs), len(words), len(types), len(inactive), sum(i["words"] for i in inactive), len(problems)))
    return dict(words=words, engine_types=types, problems=problems, inactive=inactive, eval_table=eval_table)


def main(argv):
    if not argv:
        print(__doc__)
        return 2
    pe = PE(argv[0])
    t = build(pe)
    impl = 0
    for w, d in t["words"].items():
        if d["keyword"] or not d["handler"]:
            continue
        a = analyze_handler(pe, d["handler"])
        d.update(nargs=a["nargs"], argsizes=a["argsizes"], retsize=a["retsize"], implementations=a["implementations"],
                 node=sorted(handler_node_sizes(pe, d["handler"])))
        if a["implementations"]:
            impl += 1
    print("words with a named implementation call: %d of %d" % (impl, len(t["words"])))
    print("engine types:", {k: v.get("pz_ClassName") for k, v in sorted(t["engine_types"].items())})
    print("inactive registrars:", t["inactive"])
    print("problems:", t["problems"][:10])
    if "--json" in argv:
        out = argv[argv.index("--json") + 1]
        with open(out, "w") as f:
            json.dump({"words": {"%08X" % w: d for w, d in sorted(t["words"].items())},
                       "engine_types": {str(k): v for k, v in sorted(t["engine_types"].items())},
                       "inactive": t["inactive"]}, f, indent=1)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
