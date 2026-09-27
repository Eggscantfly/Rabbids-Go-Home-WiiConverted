"""Saved universe variables of Rabbids Go Home.

A save slot's data is a list of entries {u32 key, u32 id, u32 len, len bytes}: key 0 is the universe, id is the
offset of the variable in the universe variable buffer.  To move a save between the Wii (big-endian) and the PC
(little-endian) the bytes of every entry are swapped according to the type of its variable, which comes from the
universe script model (bigfile entry 72002B9C, identical in the Wii bigfile and in a converted one).

Entry layout (SCR::SAVValidateVar, PC 004F7530 / Wii 8012218C; SCR::SAVComputeSlotSizeVar 004F73B0 / 80121F30;
SCR::SAVUnValidateVar 004F78F0 / 80122870): every variable with descriptor flag 0x0400 is saved into the structure
given by descriptor byte +0x23.  A struct (type 0x2F) is expanded member by member (an array of structs element by
element), a fixed buffer (type 0x6A) gives one entry per element holding the buffer contents, a buffer (type 0x64)
is not saved, any other type is one entry of the variable's whole size.

Model layout (SCR::p_Callback_LoadModel, SCR::pt_LoadVars): u32 father, then a variable block:
    u32 0xC0DEC0DF, u32, u32 count, u32 version   (or 0xC0DEC0DE, u32, u32 count: version 0; or a bare count)
    per variable: u32 index, u32 type (type id << 16), [version >= 1: u32], [version >= 2: u32],
                  u32 offset, u32 size, u32 array flag, u32, u32, u32 name length, 3 x u32 string lengths,
                  u16 flags, u8, u8 structure, [version >= 3: u8], u32, name, 3 strings,
                  [type 0x2F, or 0x4D with version >= 2: u32 0xABCD6666 (no members) or a nested variable block]
    u32 buffer size, u32 initial value bytes flag, [buffer size rounded down to 4 bytes of initial values]
"""
from __future__ import annotations

import collections
import struct

from . import _bigfile

UNIVERSE_MODEL_KEY = 0x72002B9C

_BLOCK_C0DE = 0xC0DEC0DE
_BLOCK_C0DF = 0xC0DEC0DF
_NO_MEMBERS = 0xABCD6666

T_STRUCT = 0x2F
T_BUFFER = 0x64
T_FIXED_BUFFER = 0x6A

# byte-swap unit per script type id; fixed buffers are copied as they are
SWAP_UNIT = {0x0A: 4, 0x0B: 4, 0x14: 4, 0x15: 4, 0x16: 4, 0x1C: 4, 0x1E: 4, 0x3D: 4, 0x3E: 4, 0x46: 4,
             0x5A: 4, 0x5B: 4, 0x60: 2, 0x61: 8}
TYPE_NAMES = {0x0A: "enum", 0x0B: "bitfield", 0x14: "int", 0x15: "float", 0x16: "vector", 0x1C: "color",
              0x1E: "key", 0x3D: "specflag", 0x3E: "matrix44", 0x46: "vector4", 0x5A: "objref", 0x5B: "mdfref",
              0x60: "widechar", 0x61: "longint", T_FIXED_BUFFER: "fixedbuffer"}

# kind: a swap unit (2, 4, 8), "raw" (fixed buffer contents) or "untyped" (no swap rule known)
Leaf = collections.namedtuple("Leaf", "id kind type size name top")


class ModelError(ValueError):
    pass


class _Reader:
    def __init__(self, data: bytes):
        self.data = data
        self.pos = 0

    def take(self, n: int) -> int:
        p = self.pos
        if p + n > len(self.data):
            raise ModelError("universe model truncated at +%#x" % p)
        self.pos = p + n
        return p

    def u8(self) -> int:
        return self.data[self.take(1)]

    def u16(self) -> int:
        return struct.unpack_from("<H", self.data, self.take(2))[0]

    def u32(self) -> int:
        return struct.unpack_from("<I", self.data, self.take(4))[0]

    def raw(self, n: int) -> bytes:
        p = self.take(n)
        return self.data[p:p + n]


def _variable_block(rd: _Reader, first: int | None = None) -> dict:
    first = rd.u32() if first is None else first
    if first == _BLOCK_C0DE:
        rd.u32()
        count = rd.u32()
        version = 0
    elif first == _BLOCK_C0DF:
        rd.u32()
        count = rd.u32()
        version = rd.u32()
    else:
        count = first
        version = 0
    variables = []
    for _ in range(count):
        v = {"index": rd.u32(), "type": rd.u32()}
        if version != 0:
            rd.u32()
        if version > 1:
            rd.u32()
        v["offset"] = rd.u32()
        v["size"] = rd.u32()
        v["array"] = rd.u32()
        rd.u32()
        rd.u32()
        lengths = [rd.u32() for _ in range(4)]
        v["flags"] = rd.u16()
        rd.u8()
        v["structure"] = rd.u8()
        if version > 2:
            rd.u8()
        rd.u32()
        v["name"] = rd.raw(lengths[0]).split(b"\0")[0].decode("latin-1") if lengths[0] else ""
        for n in lengths[1:]:
            if n:
                rd.raw(n)
        t = v["type"] & 0xFFFF0000
        v["members"] = None
        if t == T_STRUCT << 16 or (version > 1 and t == 0x004D0000):
            w = rd.u32()
            if w != _NO_MEMBERS:
                v["members"] = _variable_block(rd, w)
        variables.append(v)
    size_buffer = rd.u32()
    if rd.u32():
        rd.raw((size_buffer // 4) * 4)
    return {"variables": variables, "size_buffer": size_buffer, "version": version}


def saved_variables(model: bytes) -> dict:
    """{structure: {id: Leaf}} for every saved universe variable of a universe model entry."""
    rd = _Reader(model)
    rd.u32()                                        # father model
    block = _variable_block(rd)
    out: dict = {}

    def add(structure, leaf):
        table = out.setdefault(structure, collections.OrderedDict())
        if leaf.id in table:
            raise ModelError("two saved leaves at id %#x (%s, %s)" % (leaf.id, table[leaf.id].name, leaf.name))
        table[leaf.id] = leaf

    def walk(v, base, structure, path, top):
        t = v["type"] >> 16
        name = path + v["name"]
        if t == T_STRUCT:
            members = v["members"]
            if not members:
                raise ModelError("struct %s without a member block" % name)
            step = members["size_buffer"]
            if v["array"] == 0:
                for m in members["variables"]:
                    walk(m, base + v["offset"], structure, name + ".", top)
            else:
                for i in range(v["size"] // step):
                    for m in members["variables"]:
                        walk(m, base + v["offset"] + step * i, structure, "%s[%d]." % (name, i), top)
            return
        if t == T_BUFFER:
            return
        if t == T_FIXED_BUFFER:
            if v["array"] == 0:
                add(structure, Leaf(base + v["offset"], "raw", t, None, name, top))
            else:
                for i in range(v["size"] // 12):
                    add(structure, Leaf(base + v["offset"] + 12 * i, "raw", t, None, "%s[%d]" % (name, i), top))
            return
        add(structure, Leaf(base + v["offset"], SWAP_UNIT.get(t) or "untyped", t, v["size"], name, top))

    for v in block["variables"]:
        if v["flags"] & 0x400:
            walk(v, 0, v["structure"], "", v["name"])
    return out


def load_from_bigfile(path: str) -> dict:
    """Saved variables of the universe model stored in a Wii or converted bigfile."""
    return saved_variables(_bigfile.read_entry(path, UNIVERSE_MODEL_KEY))
