"""Script records as streams: script instances (the SCR modifier) and script model records, with the variable blocks
both carry.

  instance  SCR::p_Callback_LoadInstance   (RGH 8012BA00)
            MDF header, u32 model key (LOA_MakeFileRef), SCR::pt_LoadVars, [MDF version >= 1: u32 n, u8[n] mode 3]
  model     SCR::p_Callback_LoadModel      (RGH 8012B40C)
            u32 father model key, SCR::pt_LoadVars, u32 n_init, u32[n] init tracks, u8[128] mode 2 x 2, u8[128] mode 1
            x 2, then the reference list: user word FFFFFFFF track, FFFFFFFE procedure list, FFFFFFFD rule list,
            FFFFFFFC data set; each of those follows a user-0 reference to its source (key - 1), and the model's own
            source is the model key - 1.  priv 0x10000001 (a non-modifier link) is the father (user 0) or a resource
            link (user != 0).
  variables SCR::pt_LoadVars (RGH 8012A020): a C0DE / C0DF block header or a bare count, the descriptors, the buffer
            size and the initial values.
"""
from __future__ import annotations

from . import stream as F
from .stream import FormatError

SCR_TYPE = 0
USER_TRACK, USER_PROCLIST, USER_RULELIST, USER_DATASET = 0xFFFFFFFF, 0xFFFFFFFE, 0xFFFFFFFD, 0xFFFFFFFC
REF_USERS = (USER_TRACK, USER_PROCLIST, USER_RULELIST, USER_DATASET)

C0DE = 0xC0DEC0DE
C0DF = 0xC0DEC0DF
NOSUB = 0xABCD6666


def _cnt(st, o, name, value, fn="u32"):
    if st.writing:
        o[name] = value
    return getattr(st, fn)(o, name)


def _bytes(st, o, name, n, mode=2):
    if st.writing:
        v = o.get(name) or b""
        if st.present(mode):
            if len(v) != n:
                raise FormatError(f"{name}: {len(v)} bytes, expected {n}")
            st.buf += v
        return v
    return st.raw(o, name, n, mode)


def vars_block(st, o, preread=None):
    """One variable descriptor block.  `preread` is the first word when the caller already consumed it (the nested
    descriptor after a struct type)."""
    if st.writing:
        form = o.get("form", "C0DF")
        if form == "C0DF":
            st.u32({"m": C0DF}, "m"); st.u32(o, "w20", default=0)
            v = o.setdefault("vars", [])
            _cnt(st, o, "count", len(v)); st.u32(o, "version", default=3)
        elif form == "C0DE":
            st.u32({"m": C0DE}, "m"); st.u32(o, "w20", default=0)
            _cnt(st, o, "count", len(o.setdefault("vars", [])))
            o["version"] = 0
        else:
            _cnt(st, o, "count", len(o.setdefault("vars", [])))
            o["version"] = 0
    else:
        first = preread if preread is not None else st.u32({}, "first")
        if first == C0DE:
            o["form"] = "C0DE"; st.u32(o, "w20"); st.u32(o, "count"); o["version"] = 0
        elif first == C0DF:
            o["form"] = "C0DF"; st.u32(o, "w20"); st.u32(o, "count"); st.u32(o, "version")
        else:
            o["form"] = "count"; o["count"] = first; o["w20"] = 0xFFFFFFFF; o["version"] = 0
    ver = o["version"]
    lst = o.setdefault("vars", [])
    for i in range(o["count"]):
        v = lst[i] if st.writing else {}
        if not st.writing:
            lst.append(v)
        st.u32(v, "idx")
        st.u32(v, "type")                    # SCR_tt_VarInfo_ +0x00: type << 16 (and flags)
        if ver != 0:
            st.u32(v, "w04", default=0x10000)
        else:
            v.setdefault("w04", 0x10000)
        if ver > 1:
            st.u32(v, "m1_w", 1)
        st.u32(v, "offset"); st.u32(v, "size"); st.u32(v, "w10"); st.u32(v, "w14"); st.u32(v, "w18")
        if st.writing:
            for nm, ln in (("name", "len_name"), ("s2", "len_s2"), ("s3", "len_s3"), ("s4", "len_s4")):
                v[ln] = len(v.get(nm) or b"")
        n1 = st.u32(v, "len_name"); n2 = st.u32(v, "len_s2"); n3 = st.u32(v, "len_s3"); n4 = st.u32(v, "len_s4")
        st.u16(v, "flags"); st.u8(v, "b22"); st.u8(v, "b23")
        if ver > 2:
            st.u8(v, "b24")
        st.u32(v, "w28")
        if n1:
            _bytes(st, v, "name", n1, 2)
        if n2:
            _bytes(st, v, "s2", n2, 1)
        if n3:
            _bytes(st, v, "s3", n3, 1)
        if n4:
            _bytes(st, v, "s4", n4, 1)
        t = v["type"] & 0xFFFF0000
        if t == 0x002F0000 or (ver > 1 and t == 0x004D0000):
            if st.writing:
                sub = v.get("sub")
                if sub is None:
                    st.u32({"m": NOSUB}, "m")
                else:
                    vars_block(st, sub)
            else:
                w = st.u32({}, "w")
                if w == NOSUB:
                    v["sub"] = None
                else:
                    v["sub"] = {}
                    vars_block(st, v["sub"], preread=w)
    st.u32(o, "size_buffer")
    init_size = st.u32(o, "size_buffer_init")
    if init_size:
        n = int(o["size_buffer"] / 4) if o["size_buffer"] >= 0 else 0
        _bytes(st, o, "init", n * 4)
    return o


def instance(st, o):
    st.memsize(o)
    F.mdf_header(st, o)
    st.u32(o, "model")
    vars_block(st, o.setdefault("vars", {}))
    if o["version"] >= 1:
        blob = o.get("m3") or b""
        n = _cnt(st, o, "len_m3", len(blob))
        if n:
            _bytes(st, o, "m3", n, 3)
    return o


def refs_instance(o):
    return [(o.get("model", 0), "script_model")] if o.get("model") else []


def model(st, o):
    st.memsize(o)
    st.u32(o, "father")
    vars_block(st, o.setdefault("vars", {}))
    init = o.get("init") or []
    n = _cnt(st, o, "n_init", len(init))
    if st.writing:
        for i in range(n):
            st.u32({"v": init[i]}, "v")
    else:
        o["init"] = [st.u32({}, "v") for _ in range(n)]
    _bytes(st, o, "s1", 128, 2)
    _bytes(st, o, "s2", 128, 2)
    _bytes(st, o, "s3", 128, 1)
    _bytes(st, o, "s4", 128, 1)
    st.refs(o, "refs")
    return o
