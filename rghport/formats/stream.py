"""Resource streams, read and written through one description per type.

The engine serialises every resource through `Priv_Read*(cursor, mode, name)`; the same code is its writer, and
`mode` says which bin-load level a field belongs to: mode 1 fields are absent above level 1, mode 3 fields are absent at
level 3, mode 2 fields are always there.  Package records are level 3 (binary: inline reference lists, data blocks
prefixed by their in-memory size); the loose files of a bigfile (a universe model, sound configurations) are level 0
with a separate reference table.

So each format below is one function `xxx(st, o)` that walks the fields in loader order against a `Reader` or a
`Writer`; the dict `o` collects (or supplies) the values.  `parse` reads a record into a dict, `emit` writes a dict
back; re-emitting a parsed package record at level 3 gives the same bytes.

Field order, modes and version gates follow the loaders of the RGH Wii executable (addresses where given).
"""
from __future__ import annotations

import struct
from dataclasses import dataclass

MAGIC = 0xC0DEC0DE


class FormatError(Exception):
    pass


@dataclass
class Ref:
    key: int
    priv: int = 0
    user: int = 0

    def pack(self):
        return struct.pack("<III", self.key, self.priv, self.user)

    @property
    def is_modifier(self):        # PrivFlags bit 0x10000000 marks a non-modifier link
        return not (self.priv & 0x10000000)

    @property
    def type(self):
        return self.user & 0xFFFF


class Stream:
    """`level` is BIG_gu32_CurBinLoad; `binary` is BIG_b_IsBinaryData() (a package record: inline reference lists,
    data blocks prefixed by their in-memory size)."""

    def __init__(self, level: int, binary: bool):
        self.level = level
        self.binary = binary

    def present(self, mode: int) -> bool:
        if mode == 1:
            return self.level <= 1
        if mode == 3:
            return self.level != 3
        return True


class Reader(Stream):
    def __init__(self, data: bytes, level: int, binary: bool, refs: list[Ref] | None = None, pos: int = 0,
                 end: int | None = None):
        super().__init__(level, binary)
        self.d = data
        self.pos = pos
        self.end = len(data) if end is None else end
        self.table = list(refs or [])       # the reference table of a loose file
        self.writing = False

    # ---- primitives ------------------------------------------------------
    def _take(self, n: int) -> int:
        if self.pos + n > self.end:
            raise FormatError(f"read past end at {self.pos:#x}+{n} (end {self.end:#x})")
        p = self.pos
        self.pos += n
        return p

    def _scalar(self, o, name, fmt, size, mode, default):
        if not self.present(mode):
            o.setdefault(name, default)
            return o[name]
        v = struct.unpack_from(fmt, self.d, self._take(size))[0]
        o[name] = v
        return v

    def u8(self, o, name, mode=2, default=0):  return self._scalar(o, name, "<B", 1, mode, default)
    def i8(self, o, name, mode=2, default=0):  return self._scalar(o, name, "<b", 1, mode, default)
    def u16(self, o, name, mode=2, default=0): return self._scalar(o, name, "<H", 2, mode, default)
    def i16(self, o, name, mode=2, default=0): return self._scalar(o, name, "<h", 2, mode, default)
    def u32(self, o, name, mode=2, default=0): return self._scalar(o, name, "<I", 4, mode, default)
    def i32(self, o, name, mode=2, default=0): return self._scalar(o, name, "<i", 4, mode, default)
    def f32(self, o, name, mode=2, default=0.0): return self._scalar(o, name, "<f", 4, mode, default)

    def raw(self, o, name, n, mode=2, default=None):
        """n opaque bytes (vectors, matrices, names, blobs)."""
        if not self.present(mode):
            o.setdefault(name, default if default is not None else b"\0" * n)
            return o[name]
        v = bytes(self.d[self._take(n):self.pos])
        o[name] = v
        return v

    def v3(self, o, name, mode=2): return self.raw(o, name, 12, mode)
    def v4(self, o, name, mode=2): return self.raw(o, name, 16, mode)
    def quat(self, o, name, mode=2): return self.raw(o, name, 16, mode)
    def m33(self, o, name, mode=2): return self.raw(o, name, 36, mode)
    def m44(self, o, name, mode=2): return self.raw(o, name, 64, mode)
    def trm(self, o, name, mode=2): return self.raw(o, name, 144, mode)

    def box(self, o, name):
        """MTH_pu8_LoadBoxFromBuffer: min, max, center, then a word only loose files keep."""
        v = self.raw(o, name, 36, 2)
        self.u32(o, name + "_m1", 1)
        return v

    def sphere(self, o, name):
        """MTH_pu8_LoadSphereFromBuffer: radius, center, then a word only loose files keep."""
        v = self.raw(o, name, 16, 2)
        self.u32(o, name + "_m1", 1)
        return v

    def name64(self, o, name, mode=3):
        return self.raw(o, name, 64, mode)

    def array(self, o, name, fmt, count, mode=2):
        """count elements of struct fmt (e.g. '<f')."""
        size = struct.calcsize(fmt)
        if not self.present(mode):
            o.setdefault(name, [])
            return o[name]
        p = self._take(size * count)
        v = list(struct.unpack_from("<" + fmt.lstrip("<") * count, self.d, p)) if count else []
        o[name] = v
        return v

    def refs(self, o, name):
        """A LOA_p_LoadRef(b_Refs=1) reference list."""
        if self.binary:
            n = struct.unpack_from("<I", self.d, self._take(4))[0]
            out = []
            for _ in range(n):
                k, p, u = struct.unpack_from("<III", self.d, self._take(12))
                out.append(Ref(k, p, u))
            o[name] = out
        else:
            o[name] = list(self.table)
        return o[name]

    def ref(self, o, name, mode=2):
        """Priv_Readref: one inline BIG_tt_Ref_."""
        if not self.present(mode):
            o.setdefault(name, Ref(0, 0, 0))
            return o[name]
        k, p, u = struct.unpack_from("<III", self.d, self._take(12))
        o[name] = Ref(k, p, u)
        return o[name]

    def memsize(self, o, name="memsize"):
        """The in-memory size LOA_p_LoadRef puts in front of a package data block."""
        if self.binary:
            return self.u32(o, name)
        o.setdefault(name, 0)
        return o[name]

    def remaining(self) -> int:
        return self.end - self.pos


class Writer(Stream):
    def __init__(self, level: int, binary: bool):
        super().__init__(level, binary)
        self.buf = bytearray()
        self.table: list[Ref] = []
        self.writing = True

    def _scalar(self, o, name, fmt, size, mode, default):
        v = o.get(name, default)
        if v is None:
            v = default
        if self.present(mode):
            try:
                self.buf += struct.pack(fmt, v)
            except struct.error as e:
                raise FormatError(f"{name}={v!r}: {e}")
        return v

    def u8(self, o, name, mode=2, default=0):  return self._scalar(o, name, "<B", 1, mode, default)
    def i8(self, o, name, mode=2, default=0):  return self._scalar(o, name, "<b", 1, mode, default)
    def u16(self, o, name, mode=2, default=0): return self._scalar(o, name, "<H", 2, mode, default)
    def i16(self, o, name, mode=2, default=0): return self._scalar(o, name, "<h", 2, mode, default)
    def u32(self, o, name, mode=2, default=0):
        return self._scalar(o, name, "<I", 4, mode, default & 0xFFFFFFFF if isinstance(default, int) else default)
    def i32(self, o, name, mode=2, default=0): return self._scalar(o, name, "<i", 4, mode, default)
    def f32(self, o, name, mode=2, default=0.0): return self._scalar(o, name, "<f", 4, mode, float(default))

    def raw(self, o, name, n, mode=2, default=None):
        v = o.get(name)
        if v is None:
            v = default if default is not None else b"\0" * n
        v = bytes(v)
        if len(v) != n:
            if len(v) < n:
                v = v + b"\0" * (n - len(v))
            else:
                v = v[:n]
        if self.present(mode):
            self.buf += v
        return v

    def v3(self, o, name, mode=2): return self.raw(o, name, 12, mode)
    def v4(self, o, name, mode=2): return self.raw(o, name, 16, mode)
    def quat(self, o, name, mode=2): return self.raw(o, name, 16, mode)
    def m33(self, o, name, mode=2): return self.raw(o, name, 36, mode, IDENTITY33)
    def m44(self, o, name, mode=2): return self.raw(o, name, 64, mode, IDENTITY44)
    def trm(self, o, name, mode=2): return self.raw(o, name, 144, mode, IDENTITY_TRM)

    def box(self, o, name):
        v = self.raw(o, name, 36, 2)
        self.u32(o, name + "_m1", 1)
        return v

    def sphere(self, o, name):
        v = self.raw(o, name, 16, 2)
        self.u32(o, name + "_m1", 1)
        return v

    def name64(self, o, name, mode=3): return self.raw(o, name, 64, mode)

    def array(self, o, name, fmt, count, mode=2):
        v = list(o.get(name) or [])
        if len(v) < count:
            v = v + [0] * (count - len(v))
        v = v[:count]
        if self.present(mode) and count:
            self.buf += struct.pack("<" + fmt.lstrip("<") * count, *v)
        return v

    def refs(self, o, name):
        lst = list(o.get(name) or [])
        if self.binary:
            self.buf += struct.pack("<I", len(lst))
            for r in lst:
                self.buf += r.pack()
        else:
            self.table.extend(lst)
        return lst

    def ref(self, o, name, mode=2):
        r = o.get(name) or Ref(0, 0, 0)
        if self.present(mode):
            self.buf += r.pack()
        return r

    def memsize(self, o, name="memsize"):
        if self.binary:
            return self.u32(o, name)
        return o.get(name, 0)

    def data(self) -> bytes:
        return bytes(self.buf)


IDENTITY44 = struct.pack("<16f", 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1)
IDENTITY33 = struct.pack("<9f", 1, 0, 0, 0, 1, 0, 0, 0, 1)
IDENTITY_TRM = IDENTITY44 + IDENTITY44 + struct.pack("<3fI", 0, 0, 0, 0)


# ============================================================================
# running a format
# ============================================================================
def parse(fn, data: bytes, *, level: int = 3, binary: bool = True, refs=None, strict: bool = True) -> dict:
    """Read one record (a package record by default); strict: the stream must end exactly on the last byte."""
    st = Reader(data, level, binary, refs)
    o = {}
    fn(st, o)
    o["_consumed"] = st.pos
    if strict and st.pos != len(data):
        raise FormatError(f"{fn.__name__}: consumed {st.pos} of {len(data)} bytes")
    return o


def emit(fn, o: dict, *, level: int = 3, binary: bool = True):
    """Write one record; returns (bytes, reference table)."""
    st = Writer(level, binary)
    fn(st, o)
    return st.data(), st.table


# ============================================================================
# shared building blocks
# ============================================================================
def mdf_header(st, o):
    """MDF::pu32_BeginLoad: flags, version, type (0xFFFF -> extended)."""
    st.u32(o, "mdf_flags")
    st.u16(o, "version")
    if st.writing:
        ext = o.get("mdf_ext", True)
        t = 0xFFFF if ext else o["type"]
        o["_t"] = t
        st.u16(o, "_t")
    else:
        t = st.u16(o, "_t")
        ext = t == 0xFFFF
        o["mdf_ext"] = ext
    if ext:
        st.u16(o, "type")
        st.u16(o, "mdf_id")
        mv = st.u16(o, "mdf_version", default=2)
        if mv >= 1:
            st.u16(o, "mdf_ext1", 1, default=0xFFFF)
            st.u32(o, "mdf_ext2", 1)
    else:
        o["type"] = t
        o.setdefault("mdf_id", 0)
        o.setdefault("mdf_version", 0)


# ============================================================================
# ViD::pu8_ViewLoad  (RGH accepts version <= 5)
# ============================================================================
def view(st, o):
    ver = st.u32(o, "version", default=5)
    if ver < 5:
        st.trm(o, "trm", 1)
    st.m44(o, "camera")                 # K3D_BasicCamera::SetMatrix
    st.f32(o, "iso_zoom", default=1.0)
    st.f32(o, "fov", default=1.2)
    if ver == 1:
        for i in range(4):
            st.f32(o, f"v1_f{i}")
    st.u32(o, "perspective")
    st.u32(o, "draw_global_mask", default=0xCC2F5)
    if ver < 5:
        st.m44(o, "m13", 1)
        st.m44(o, "m14", 1)
    if ver == 1:
        for i in range(3):
            st.m44(o, f"v1_m{i}", 1)
        for i in range(4):
            st.u32(o, f"v1_u{i}", 1)


# ============================================================================
# ViD::pu32_LoadRenderParam and its sub-blocks
# ============================================================================
def rp_culling(st, o):
    ver = st.u32(o, "version", default=5)
    st.f32(o, "f0")
    if ver >= 3:
        st.f32(o, "f1")
    st.f32(o, "f2"); st.f32(o, "f3")
    st.u8(o, "b0"); st.u8(o, "b1"); st.u8(o, "b2"); st.u8(o, "b3")
    st.f32(o, "f4"); st.f32(o, "f5")
    if ver >= 2:
        st.f32(o, "f6"); st.f32(o, "f7")
    if ver == 4:
        st.f32(o, "v4_f0"); st.f32(o, "v4_f1")


def rp_fog(st, o):
    ver = st.u32(o, "version", default=1)
    st.u32(o, "u0"); st.u32(o, "color")
    for i in range(7):
        st.f32(o, f"f{i}")
    for i in range(4):
        st.u32(o, f"u{i+1}")
    st.f32(o, "f7"); st.f32(o, "f8")


def rp_wiiglow(st, o):
    ver = st.u32(o, "version", default=2)
    st.u32(o, "u0")
    if ver >= 2:
        st.u32(o, "u1")
    st.f32(o, "f0"); st.f32(o, "f1")


def rp_cartoon(st, o):
    ver = st.u32(o, "version", default=2)
    for i in range(7):
        st.u32(o, f"u{i}")
    for i in range(5):
        st.f32(o, f"f{i}")
    if ver > 1:
        st.f32(o, "f5")


def rp_wii(st, o):
    st.u32(o, "version", default=1)
    st.u32(o, "u0")


def rp_ao_dummy(st, o):
    st.u32(o, "version", default=3); st.u32(o, "u0"); st.f32(o, "f0"); st.u32(o, "u1")


def rp_hdr_dummy(st, o):
    ver = st.u32(o, "version", default=6)
    st.u32(o, "u0")
    for i in range(5):
        st.f32(o, f"f{i}")
    if ver >= 6:
        st.f32(o, "f5"); st.f32(o, "f6"); st.f32(o, "f7")
    st.f32(o, "f8")
    if not st.binary:
        st.v4(o, "v0"); st.v4(o, "v1"); st.v4(o, "v2")
    st.u32(o, "u1")
    for i in range(6):
        st.f32(o, f"g{i}")
    if not st.binary:
        st.v4(o, "v3")


def rp_aa_dummy(st, o):
    ver = st.u32(o, "version", default=2)
    st.u32(o, "u0"); st.u32(o, "u1")
    if ver > 1:
        for i in range(6):
            st.f32(o, f"f{i}")


def rp_deferred_dummy(st, o):
    st.u32(o, "version", default=1); st.u32(o, "u0"); st.u32(o, "u1"); st.f32(o, "f0")


def rp_postprocess_dummy(st, o):
    ver = st.u32(o, "version", default=6)
    st.u32(o, "u0")
    if ver >= 5:
        st.u32(o, "u1")
    st.u32(o, "u2"); st.u32(o, "u3")
    for i in range(4):
        st.f32(o, f"a{i}")
    for i in range(4):
        st.f32(o, f"b{i}")
    st.f32(o, "f0")
    if ver >= 1:
        st.f32(o, "c0"); st.f32(o, "c1")
    if ver >= 4:
        st.u32(o, "u4"); st.u32(o, "u5"); st.u32(o, "u6")
        for i in range(5):
            st.f32(o, f"d{i}")
    if ver >= 2:
        st.u32(o, "u7")
        for i in range(8):
            st.f32(o, f"e{i}")
    if ver >= 3:
        st.u32(o, "u8")
        for i in range(4):
            st.u32(o, f"g{i}")
    if ver >= 6:
        st.f32(o, "f9")


def render_params(st, o):
    """ViD::pu32_LoadRenderParam (RGH accepts version <= 9)."""
    sub = o.setdefault
    ver = st.u32(o, "version", default=9)
    if ver < 1:
        raise FormatError("render params version 0 (old format) not supported")
    st.u8(o, "flags"); st.u8(o, "priority"); st.u8(o, "viewports"); st.u8(o, "viewpoints")
    st.u32(o, "color0"); st.u32(o, "color1"); st.u32(o, "color2")
    # the (version == 6 and !flag) special cases are dead for the versions handled here
    if ver > 5 and ver != 7:
        st.u32(o, "color3")
    if ver >= 5:
        st.u8(o, "lightmask_nand"); st.u8(o, "lightmask_or")
    if ver >= 9:
        st.array(o, "ligcoef", "<f", 8)
    if ver < 5:
        rp_ao_dummy(st, sub("ao_dummy", {}))
    rp_culling(st, sub("culling", {}))
    rp_fog(st, sub("fog", {}))
    if ver < 5:
        rp_hdr_dummy(st, sub("hdr_dummy", {}))
        rp_aa_dummy(st, sub("aa_dummy", {}))
        rp_deferred_dummy(st, sub("deferred_dummy", {}))
    rp_wiiglow(st, sub("wiiglow", {}))
    if ver > 1:
        rp_cartoon(st, sub("cartoon", {}))
    if ver > 2:
        rp_wii(st, sub("wii", {}))
    if ver == 4:
        rp_postprocess_dummy(st, sub("postprocess_dummy", {}))


# ============================================================================
# WOR::LoadSectoFromBuffer
# ============================================================================
def secto(st, o):
    ver = st.u32(o, "version", default=13)
    if ver == 0 or ver > 13:
        raise FormatError(f"secto version {ver}")
    sectors = o.setdefault("sectors", [])
    if ver < 12:
        if ver == 3:
            st.u32(o, "v3_u")
        c = st.u32(o, "count_or_magic", default=len(sectors))
        if c < 0x100:
            n = st.u32(o, "count", default=len(sectors))
        else:
            n = o.setdefault("count", c)
        if st.writing:
            n = len(sectors)
        for i in range(n):
            s = sectors[i] if i < len(sectors) else {}
            if i >= len(sectors):
                sectors.append(s)
            st.name64(s, "name", 2)
            st.u32(s, "u0")
            if ver >= 7:
                st.u8(s, "b0"); st.u8(s, "b1")
            if ver >= 8:
                st.u8(s, "shape")
            if ver >= 11:
                st.f32(s, "f0")
            if ver >= 9 and s.get("b1", 0) == 1:
                st.f32(s, "f1"); st.f32(s, "f2"); st.u32(s, "u1")
            if ver >= 10 and s.get("b1", 0) == 2:
                st.f32(s, "f3")
            n1 = st.u32(s, "n_a", default=len(s.get("a", [])))
            st.array(s, "a", "<I", n1)
            n2 = st.u32(s, "n_b", default=len(s.get("b", [])))
            st.array(s, "b", "<I", n2)
            if ver > 1:
                st.u32(s, "u2")
            st.u32(s, "u3")
            if ver > 4:
                if ver >= 8:
                    shp = s.get("shape", 0)
                    if shp == 0:
                        st.box(s, "box")
                    elif shp == 1:
                        for k in range(8):
                            st.v3(s, f"pt{k}")
                    elif shp == 2:
                        st.box(s, "box"); st.m44(s, "m44")
                else:
                    st.box(s, "box")
            if ver > 5:
                _portals(st, s, ver)
    else:
        n = st.u32(o, "count", default=len(sectors))
        if st.writing:
            n = len(sectors)
        for i in range(n):
            s = sectors[i] if i < len(sectors) else {}
            if i >= len(sectors):
                sectors.append(s)
            st.u16(s, "idx")
            st.name64(s, "name", 2)
            st.u32(s, "u0")
            st.u8(s, "b0"); st.u8(s, "b1"); st.u8(s, "shape")
            st.f32(s, "f0")
            if s.get("b1", 0) == 1:
                st.f32(s, "f1"); st.f32(s, "f2"); st.u32(s, "u1")
            if s.get("b1", 0) == 2:
                st.f32(s, "f3")
            n1 = st.u32(s, "n_a", default=len(s.get("a", [])))
            st.array(s, "a", "<I", n1)
            n2 = st.u32(s, "n_b", default=len(s.get("b", [])))
            st.array(s, "b", "<I", n2)
            shp = s.get("shape", 0)
            if shp == 0:
                st.box(s, "box")
            elif shp == 1:
                for k in range(8):
                    st.v3(s, f"pt{k}")
            elif shp == 2:
                st.box(s, "box"); st.m44(s, "m44")
        _portals(st, o, ver)


def _portals(st, s, ver):
    portals = s.setdefault("portals", [])
    n = st.u32(s, "n_portals", default=len(portals))
    if st.writing:
        n = len(portals)
    for i in range(n):
        p = portals[i] if i < len(portals) else {}
        if i >= len(portals):
            portals.append(p)
        st.name64(p, "name", 2)
        if ver > 12:
            st.u32(p, "u0")
        st.u32(p, "u1"); st.u32(p, "u2"); st.u32(p, "u3")
        for k in range(4):
            st.v3(p, f"pt{k}")


# ============================================================================
# WOR::p_Callback_LoadWorld
# ============================================================================
def world(st, o):
    """RGH: version <= 2.  Package records start with the in-memory size; loose files start at the magic."""
    st.memsize(o)
    magic = st.u32(o, "magic", default=MAGIC)
    if magic == MAGIC:
        ver = st.u32(o, "version", default=2)
        st.u32(o, "key")
    else:
        ver = o["version"] = 0
    views = o.setdefault("views", [{} for _ in range(4)])
    for i in range(4):
        view(st, views[i])
    rp = o.setdefault("render_params", {})
    render_params(st, rp)
    if rp.get("version", 0) != 0:
        secto(st, o.setdefault("secto", {}))
    if ver != 0:
        lst = o.setdefault("bounds", [])
        n = st.u32(o, "n_bounds", default=len(lst))
        if st.writing:
            n = len(lst)
        for i in range(n):
            b = lst[i] if i < len(lst) else {}
            if i >= len(lst):
                lst.append(b)
            st.v3(b, "min", 1); st.v3(b, "max", 1)
            st.u32(b, "u0", 1)
            if ver > 1:
                st.u32(b, "u1", 1)
    # the key of the world's modifier holder, its modifier list, then the object list
    mk = st.u32(o, "mdf_key")
    if st.binary or st.writing:
        if mk:
            st.refs(o, "mod_refs")
        st.refs(o, "obj_refs")
    else:
        # a loose file has one reference table: world modifiers carry their engine type in UserFlags, objects (and
        # the modifier holder's link) do not
        o["mod_refs"] = [r for r in st.table if r.is_modifier and (r.user & 0xFFFF)]
        o["obj_refs"] = [r for r in st.table if not (r.is_modifier and (r.user & 0xFFFF))]


# ============================================================================
# OBJ::p_Callback_LoadObject
# ============================================================================
def obj(st, o):
    st.memsize(o)
    ver = st.u32(o, "version", default=9)
    if ver > 9:
        raise FormatError(f"object version {ver}")
    st.u32(o, "key_copy")                 # read and dropped by the loader (the object's key)
    st.u32(o, "control_flags")
    st.u32(o, "custom_bits")
    st.u32(o, "doe_u32_3", 3)
    st.u8(o, "doe_u8_3", 3)
    if ver < 6:
        st.u32(o, "dynamic_state_old")
    else:
        st.u8(o, "dynamic_state")
    if ver >= 7:
        st.u8(o, "secto_mask", default=0xFF)
    if ver >= 8:
        exc = o.setdefault("secto_exceptions", [])
        n = st.u8(o, "n_secto_exc", default=len(exc))
        if st.writing:
            n = len(exc)
        st.array(o, "secto_exceptions", "<h", n)
    if ver >= 9:
        st.u16(o, "list_flag_init")
    st.u32(o, "m1_u0", 1); st.u32(o, "m1_u1", 1)
    if ver >= 1:
        st.u32(o, "m1_u2", 1)
    st.u8(o, "apply_prio"); st.u8(o, "apply_always_prio")
    st.u8(o, "nox_mode", default=0x20); st.u8(o, "anim_tuning", default=0x20)
    if ver > 2:
        st.u32(o, "m1_u3", 1, default=0xFFFFFFFF); st.u32(o, "m1_u4", 1, default=0x32)
    if ver > 3:
        st.f32(o, "m1_f0", 1)
    if ver > 1:
        st.array(o, "secto", "<H", 8)
    st.name64(o, "name", 3)
    st.trm(o, "trm")
    # OBJ::pu32_HieLoad
    has = st.u32(o, "has_father", default=1 if o.get("father") else 0)
    if has:
        st.u32(o, "father"); st.u32(o, "hie_flags")
    if st.writing:
        o["n_mods"] = len([r for r in o.get("refs", []) if r.is_modifier])
    st.u32(o, "n_mods")
    st.refs(o, "refs")


# ============================================================================
# VIS::p_Callback_LoadVIS and its sub-blocks
# ============================================================================
def sharp_shadow(st, o):
    """K3D_SharpShadowInstanceData::Load (RGH <= 5)."""
    ver = st.u32(o, "version", default=5)
    if ver < 2:
        st.u32(o, "u0")
        return
    st.u32(o, "u1"); st.u32(o, "u2"); st.u32(o, "u3")
    if ver > 3:
        st.u32(o, "u4"); st.u32(o, "u5")
    if ver > 4:
        st.u32(o, "u6")


def symmetry(st, o):
    st.u32(o, "u0"); st.f32(o, "f0")


def shadow_instance(st, o):
    """K3D::SDW_pu32_LoadFromBuffer (RGH <= 10)."""
    ver = st.u32(o, "version", default=10)
    if ver < 1:
        return
    for i in range(5):
        st.f32(o, f"f{i}")
    if ver >= 5:
        st.f32(o, "f5")
    st.u32(o, "key")                  # -> LOA_MakeFileRef when set
    st.v3(o, "v")
    if ver < 2:
        st.u16(o, "old_u16")
    st.u32(o, "u0")
    if ver == 3:
        st.u32(o, "v3_u")
    st.u32(o, "u1"); st.u32(o, "color"); st.u8(o, "b0")


def vertex_color(st, o, vis_version):
    """The per-visual vertex colour block, read inline by LoadVIS: name[32] (mode 3), u32 id, [VIS version >= 0x18:
    u32 rli], u32 count, u32[count], two mode-1 u32."""
    st.raw(o, "name", 32, 3)
    st.u32(o, "id")
    if vis_version >= 0x18:
        st.u32(o, "rli")
    n = st.u32(o, "count", default=len(o.get("colors", [])))
    if st.writing:
        n = len(o.get("colors", []))
    st.array(o, "colors", "<I", n)
    st.u32(o, "m1_a", 1); st.u32(o, "m1_b", 1)


def vis(st, o):
    """VIS::p_Callback_LoadVIS.  The version is the MDF header's (RGH ships up to 0x1E)."""
    st.memsize(o)
    mdf_header(st, o)
    ver = o["version"]
    if ver < 0x13:
        raise FormatError(f"VIS version {ver:#x} too old")
    st.u32(o, "flags")
    st.u32(o, "visual")
    st.u32(o, "material")
    st.u8(o, "zlist")
    if ver > 0x1D:
        st.u8(o, "render_list_order")
    st.u8(o, "light_mask")
    st.u8(o, "post_render_mask")
    st.u32(o, "m1_u0", 1)
    st.f32(o, "f0"); st.f32(o, "f1")
    if ver > 0x1C:
        st.u32(o, "u1")
    st.f32(o, "f2"); st.f32(o, "f3")
    has = st.u32(o, "has_key2", default=1 if o.get("key2") else 0)
    if has:
        st.u32(o, "key2")
    st.f32(o, "f4"); st.f32(o, "m1_f5", 1); st.u32(o, "m1_u6", 1)
    sp = st.u32(o, "has_sorting_plane", default=1 if o.get("sorting_plane") else 0)
    if sp:
        p = o.setdefault("sorting_plane", {})
        st.v3(p, "a"); st.v3(p, "b"); st.u8(p, "c")
    st.array(o, "u_pair", "<I", 2)
    st.array(o, "f_pair", "<f", 2)
    st.f32(o, "f7")
    ns = st.u32(o, "has_shadow", default=1 if o.get("shadow") else 0)
    if ns:
        shadow_instance(st, o.setdefault("shadow", {}))
        st.u32(o, "exclusive_light")
    st.u32(o, "u28")
    st.u32(o, "m1_u29", 1)
    if ver >= 0x15:
        sharp_shadow(st, o.setdefault("sharp_shadow", {}))
    if ver >= 0x16:
        symmetry(st, o.setdefault("symmetry", {}))
    if ver >= 0x17:
        vc = st.i32(o, "vertex_color_id", default=-1)
        if vc != -1:
            vertex_color(st, o.setdefault("vertex_color", {}), ver)
    flag = o.get("_k3d_flag", 0)      # a global bit the loader tests for 0x1A/0x1B
    if ver == 0x19 or (ver == 0x1A and not flag) or (ver == 0x1B and flag):
        for i in range(4):
            st.u32(o, f"lod_u{i}")
    if ver >= 0x1A:
        if ver == 0x1A and flag:
            st.u32(o, "u44")
        elif ver == 0x1B and not flag:
            pass
        else:
            if ver < 0x1F:
                st.array(o, "lod8", "<I", 8)
            else:
                st.u8(o, "lod_b")
            if ver >= 0x1B:
                st.u32(o, "u54")


# ============================================================================
# materials: K3D_material -> atomics -> templates (Wii*) -> level descs -> UV
# ============================================================================
def _cname(b):
    return bytes(b).split(b"\0")[0]


def uv_modifier(st, m):
    """K3D_UVModifier::u32_Load (u32, u32) then the subclass's own fields."""
    cls = _cname(m.get("class", b""))
    st.u32(m, "base_u0"); st.u32(m, "base_u1")
    if cls == b"K3D_UVModifier_Scale":
        v = st.u32(m, "version", default=2); st.f32(m, "f0"); st.f32(m, "f1")
        if v >= 2:
            st.f32(m, "f2"); st.f32(m, "f3")
    elif cls == b"K3D_UVModifier_Translation":
        st.u32(m, "version", default=1); st.f32(m, "f0"); st.f32(m, "f1"); st.u32(m, "u0")
    elif cls == b"K3D_UVModifier_Rotation":
        v = st.u32(m, "version", default=2); st.f32(m, "f0")
        if v >= 2:
            st.f32(m, "f1"); st.f32(m, "f2")
        st.u32(m, "u0")
    elif cls == b"K3D_UVModifier_TileAnimation":
        st.u32(m, "version", default=1); st.u16(m, "u0"); st.u16(m, "u1"); st.f32(m, "f0")
    elif cls == b"K3D_UVModifier_TileAnimationFixedDT":
        st.u32(m, "version", default=1); st.u16(m, "u0"); st.u16(m, "u1"); st.u32(m, "u2")
    elif cls == b"K3D_UVModifier_SinusScale":
        st.u32(m, "version", default=1)
        for i in range(6):
            st.f32(m, f"f{i}")
    elif cls in (b"K3D_UVModifier_SinusRotation", b"K3D_UVModifier_SinusTranslation"):
        st.u32(m, "version", default=1)
        for i in range(4):
            st.f32(m, f"f{i}")
    else:
        raise FormatError(f"unknown UV modifier class {cls!r}")


def uv_params(st, u):
    """K3D_UVParams_Load (version >= 6)."""
    ver = st.u32(u, "version", default=9)
    if ver < 6:
        raise FormatError(f"UV params version {ver}")
    st.u8(u, "source"); st.u8(u, "planar"); st.u8(u, "gizmo")
    mods = u.setdefault("modifiers", [])
    n = st.u8(u, "n_modifiers", default=len(mods))
    if st.writing:
        n = len(mods)
    for i in range(n):
        if i >= len(mods):
            mods.append({})
        m = mods[i]
        st.raw(m, "class", 64)
        uv_modifier(st, m)
    if ver >= 7:
        st.u32(u, "indirect_key")
        st.u32(u, "u8_")
        if ver >= 9:
            st.u32(u, "u9_")
        st.f32(u, "f0"); st.f32(u, "f1")
        if ver >= 8:
            st.u32(u, "flags")


def wii_level_desc(st, L):
    """u32_LoadWiiBasicLevelDesc."""
    st.u32(L, "flags")                # the engine bit-reverses this word after reading
    st.u32(L, "tex0")
    st.u32(L, "tex1")
    st.u32(L, "color")
    uv = L.setdefault("uv", {})
    uv_params(st, uv)
    # the blend bytes follow only for UV sources up to 4
    if uv.get("source", 0) <= 4:
        st.u8(L, "blend"); st.u8(L, "alpha_func"); st.u8(L, "alpha_threshold")


def wii_multitex(st, t):
    """K3D_material_WiiMultiTex::u32_Load (RGH 6..10)."""
    ver = st.u32(t, "version", default=10)
    if ver < 6:
        raise FormatError(f"WiiMultiTex version {ver}")
    levels = t.setdefault("levels", [])
    n = st.u32(t, "n_levels", default=len(levels))
    if st.writing:
        n = len(levels)
    st.u32(t, "flags"); st.u32(t, "flags2"); st.f32(t, "zbias")
    for i in range(n):
        if i >= len(levels):
            levels.append({})
        wii_level_desc(st, levels[i])


def wii_glow(st, t):
    """K3D_material_WiiGlow::u32_Load: version <= 4, one word, then the multitex stream (versions >= 3)."""
    ver = st.u32(t, "version", default=4)
    if ver > 4:
        raise FormatError(f"WiiGlow version {ver}")
    st.u32(t, "glow_u0")
    if ver >= 3:
        wii_multitex(st, t.setdefault("multitex", {}))
    else:
        st.f32(t, "old_f0")
        if ver < 2:
            st.f32(t, "old_f1")
            if ver == 1:
                st.f32(t, "old_f2")


def wii_water(st, t):
    """K3D_material_WiiWater::u32_Load (versions 3..5): two textures with their UV params, wave parameters, then one
    basic level."""
    ver = st.u32(t, "version", default=5)
    if ver < 3 or ver > 5:
        raise FormatError(f"WiiWater version {ver}")
    st.u32(t, "flags")
    st.u32(t, "texA")
    uv_params(st, t.setdefault("uvA", {}))
    st.u32(t, "texB")
    uv_params(st, t.setdefault("uvB", {}))
    for i in range(4):
        st.f32(t, f"w{i}")
    if ver < 4:
        for i in range(4):
            st.f32(t, f"old_f{i}")
    else:
        st.f32(t, "w4")
    if ver >= 5:
        st.u32(t, "u5")
    wii_level_desc(st, t.setdefault("level", {}))
    for i in range(4):
        st.f32(t, f"tail_f{i}")


def material_template(st, t):
    """The stream a K3D_material_atomic's template reads, by class name."""
    cls = _cname(t.get("class", b""))
    if cls in (b"K3D_material_WiiSimpleTex", b"K3D_material_WiiChrome"):
        ver = st.u32(t, "version", default=3)
        if ver > 3:
            raise FormatError(f"{cls} version {ver}")
        wii_multitex(st, t.setdefault("multitex", {}))
    elif cls == b"K3D_material_WiiMultiTex":
        wii_multitex(st, t.setdefault("multitex", {}))
    elif cls == b"K3D_material_WiiGlow":
        wii_glow(st, t)
    elif cls == b"K3D_material_WiiWater":
        wii_water(st, t)
    else:
        raise FormatError(f"material template {cls!r} not supported")


def template_textures(t) -> list:
    """Every texture key a template references."""
    out = []
    for L in t.get("multitex", {}).get("levels", []):
        out += [L.get("tex0", 0), L.get("tex1", 0)]
    if "level" in t:
        out += [t["level"].get("tex0", 0), t["level"].get("tex1", 0)]
    out += [t.get("texA", 0), t.get("texB", 0)]
    for u in (t.get("uvA"), t.get("uvB")):
        if u:
            out.append(u.get("indirect_key", 0))
    for L in t.get("multitex", {}).get("levels", []):
        out.append(L.get("uv", {}).get("indirect_key", 0))
    return [k for k in out if k]


def material_atomic(st, o):
    """K3D_material_atomic::pc_LoadFromBuffer (RGH <= 4).  The callback fetches the data block with LOA_p_LoadRef, so a
    package record carries the in-memory size prefix."""
    st.memsize(o)
    ver = st.u32(o, "version", default=4)
    st.u16(o, "flags")
    st.u32(o, "color")
    st.f32(o, "shadow_bias")
    st.u32(o, "render_mask")
    if ver > 1:
        st.u32(o, "material_type"); st.u32(o, "sound_type")
    tpls = o.setdefault("templates", [])
    n = st.u16(o, "n_templates", default=len(tpls))
    if st.writing:
        n = len(tpls)
    for i in range(n):
        if i >= len(tpls):
            tpls.append({})
        t = tpls[i]
        st.raw(t, "class", 64)
        st.u32(t, "tpl_flags")
        material_template(st, t)


def material(st, o):
    """K3D_material::pc_LoadFromBuffer: u16 flags; u16 0xFFFF; u16 version; u16 n; version >= 3: n x u32 atomic keys
    (older: inline atomics)."""
    st.memsize(o)
    st.u16(o, "flags")
    if st.writing:
        o["_ffff"] = 0xFFFF
        st.u16(o, "_ffff")
        ver = st.u16(o, "version", default=4)
        o["n_sub"] = len(o.get("atomics", []))
        st.u16(o, "n_sub")
    else:
        t = st.u16(o, "_ffff")
        if t == 0xFFFF:
            ver = st.u16(o, "version")
            st.u16(o, "n_sub")
        else:
            ver = o["version"] = 0
            o["n_sub"] = t
    if ver > 4 or ver < 3:
        raise FormatError(f"material version {ver} (inline atomics not supported)")
    st.array(o, "atomics", "<I", o["n_sub"])


# ============================================================================
# textures: the descriptor (K3D_texture::pu32_LoadFromBuffer + LoadDesc); the pixel data is a separate resource (a
# texture bank entry)
# ============================================================================
def texture_desc(st, t, ver, full):
    """K3D_texture::LoadDesc(cursor, version, full)."""
    if not full:
        st.u16(t, "d_u16a"); st.u16(t, "d_u16b"); st.u8(t, "d_u8a"); st.u8(t, "d_u8b"); st.u16(t, "d_u16c")
        if ver >= 8:
            st.u32(t, "d_u32a")
        st.u32(t, "d_font")
        if ver >= 2:
            if ver < 5:
                n = st.u32(t, "d_n"); st.array(t, "d_arr", "<I", n)
            if ver >= 5:
                st.u32(t, "d_mip_color")
        return
    st.u16(t, "width"); st.u16(t, "height")
    if ver >= 6:
        st.u8(t, "format")
    else:
        st.u8(t, "format_old")
    st.u8(t, "quality")
    if ver < 5:
        st.u16(t, "old_u16")
    elif ver >= 6:
        st.u8(t, "m1_b", 1)
        if ver < 9:
            st.u8(t, "divide_ld")
    else:
        st.u16(t, "old_u16b")
    if ver >= 8:
        st.u8(t, "wrap_u"); st.u8(t, "wrap_v")
        if ver < 9:
            st.u8(t, "old_b1"); st.u8(t, "old_b2")
    st.u32(t, "font")
    if ver >= 2:
        if ver < 5:
            n = st.u32(t, "n_old"); st.array(t, "old_arr", "<I", n)
        if ver >= 5:
            st.u32(t, "mip_color")
        if ver >= 9:
            st.u16(t, "mip_level_max")
        if ver >= 10:
            st.u8(t, "mip_color_mode")
        if ver >= 11:
            st.f32(t, "bias")


def texture(st, o):
    """K3D_texture::pu32_LoadFromBuffer (RGH <= 13): magic, version, flags, the descriptor, optional blocks by flag
    bits, then the texdata key."""
    st.memsize(o)
    if st.writing:
        o["_magic"] = MAGIC
        st.u32(o, "_magic")
        ver = st.u32(o, "version", default=13)
        flags = st.u32(o, "flags")
    else:
        first = st.u32(o, "_magic")
        if first == MAGIC:
            ver = st.u32(o, "version")
            flags = st.u32(o, "flags")
        else:
            ver = o["version"] = 0
            flags = o["flags"] = first
    if ver > 13:
        raise FormatError(f"texture version {ver}")
    if ver >= 5:
        texture_desc(st, o, ver, True)
    if ver >= 6:
        for i in range(4):
            st.f32(o, f"m1_f{i}", 1)
    if flags & 1:
        if ver < 5:
            texture_desc(st, o, ver, True)
        st.u32(o, "m1_u8", 1)
        if ver < 5:
            st.u32(o, "old_u9")
        if 5 <= ver <= 6:
            st.u32(o, "texdata_v56")
        if ver != 0 and ver < 5:
            st.u32(o, "old_u11")
        if ver >= 6:
            st.u16(o, "m1_u16a", 1); st.u16(o, "m1_u16b", 1); st.u8(o, "m1_u8c", 1)
    if ver < 5:
        for bit, nm in ((2, "desc2"), (4, "desc4")):
            if flags & bit:
                d = o.setdefault(nm, {}); texture_desc(st, d, ver, False)
                st.u32(d, "a"); st.u32(d, "b")
                if ver != 0:
                    st.u32(d, "c")
    if flags & 8:
        if ver < 5:
            d = o.setdefault("desc8", {}); texture_desc(st, d, ver, False)
        st.u32(o, "texpro_key")
    if flags & 0x1000000 and ver >= 4:
        st.f32(o, "m1_fa", 1); st.f32(o, "m1_fb", 1)
    if ver >= 7:
        st.u32(o, "texdata")
