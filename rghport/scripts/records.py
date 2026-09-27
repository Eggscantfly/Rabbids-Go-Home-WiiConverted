"""Script code records as the game ships them: tracks (functions), procedure lists and rule lists, with the node
streams inside them.  Variable blocks are rghport.formats.script.vars_block.

Layouts follow the RGH loaders instruction for instruction (Wii executable addresses):
    SCR::pt_LoadVars              8012A020
    SCR::p_Callback_LoadFunction  8012A598   (.efc, package function records)
    SCR::p_Callback_LoadProcList  8012A7FC   (.efg global libraries, .efl model lists)
    SCR::p_Callback_LoadRuleList  8012AD30   (.eru model rule lists)
Loose files of a bigfile (.efg/.efc) carry the mode-1 fields (a source line per node word); package records are level 3
with the in-memory size in front.

Node words are `(id << 16) | scope`: scope 0 is a VM keyword, a type handler or a library builtin, 0xFFFF a global
engine function, anything else the engine type of the modifier whose method it is (see tables.py).
"""
from __future__ import annotations

import struct

from ..formats.script import C0DE, _bytes, _cnt, vars_block  # noqa: F401  (vars_block re-exported)


def var_name(v) -> str:
    return (v.get("name") or b"").split(b"\0")[0].decode("latin-1")


# ----------------------------------------------------------------------------
# nodes
def _words(st, o, name, n, mode=2):
    if st.writing:
        w = o.get(name)
        if st.present(mode):
            if w is None:
                w = [0] * n
            st.buf += struct.pack(f"<{n}I", *w)
        return w
    if not st.present(mode):
        o[name] = None
        return None
    p = st._take(4 * n)
    o[name] = list(struct.unpack_from(f"<{n}I", st.d, p))
    return o[name]


# ----------------------------------------------------------------------------
# SCR::p_Callback_LoadFunction  (a track: .efc / package record)
def function(st, o):
    st.memsize(o)
    if st.writing:
        o.setdefault("form", "C0DE")
    if st.writing:
        if o["form"] == "C0DE":
            st.u32({"m": C0DE}, "m")
            st.u32(o, "version", default=1); st.u32(o, "fflags")
            nm = o.get("name") or b""
            o["len_name"] = len(nm)
            st.u32(o, "len_name")
            _bytes(st, o, "name", len(nm))
        else:
            st.u32(o, "first")
    else:
        first = st.u32({}, "first")
        if first == C0DE:
            o["form"] = "C0DE"
            st.u32(o, "version"); st.u32(o, "fflags")
            n = st.u32(o, "len_name")
            _bytes(st, o, "name", n)
        else:
            o["form"] = "old"; o["first"] = first
    st.u32(o, "w_a"); st.u32(o, "w_b")
    vars_block(st, o.setdefault("locals", {}))
    nodes = o.get("nodes") or []
    n = _cnt(st, o, "n_words", len(nodes))
    if n:
        _words(st, o, "nodes", n, 2)
    else:
        o["nodes"] = []
    st.u32(o, "m3_w", 3)
    if n:
        _words(st, o, "lines", n, 1)
    return o


# ----------------------------------------------------------------------------
# SCR::p_Callback_LoadProcList  (.efg global libraries, .efl model lists)
def proclist(st, o):
    st.memsize(o)
    vars_block(st, o.setdefault("globals", {}))
    procs = o.setdefault("procs", [])
    n = _cnt(st, o, "n_procs", len(procs), "u16")
    for i in range(n):
        p = procs[i] if st.writing else {}
        if not st.writing:
            procs.append(p)
        vars_block(st, p.setdefault("locals", {}))
        nm = p.get("name") or b""
        ln = _cnt(st, p, "len_name", len(nm))
        if ln:
            _bytes(st, p, "name", ln)
        st.u16(p, "pflags")
        st.u32(p, "m1_w", 1)
        nodes = p.get("nodes") or []
        nw = _cnt(st, p, "n_words", len(nodes))
        if nw:
            _words(st, p, "nodes", nw, 2)
            _words(st, p, "lines", nw, 1)
        else:
            p["nodes"] = []
    st.u32(o, "tail")
    return o


def proc_name(p) -> str:
    return (p.get("name") or b"").split(b"\0")[0].decode("latin-1")


# ----------------------------------------------------------------------------
# SCR::p_Callback_LoadRuleList  (.eru model rule lists)
#   u16 first; C0DE -> u16 version, u16 count  (else count = first, version 0)
#   per rule: [ver>=3: u16 a, b, c] vars_block(locals) u32 len + name
#             [ver<3: u16 a, [ver>=1: u16 b, c]]
#             3x (u32 n, u32[n] nodes mode2, u32[n] lines mode1)   in / out / exec
#             [ver>=2: u16 d, e]
#   [ver>=1: u32 n_sectors, u32[n_sectors]]  u32 tail
def rulelist(st, o):
    st.memsize(o)
    rules = o.setdefault("rules", [])
    if st.writing:
        if o.get("form", "C0DE") == "C0DE":
            st.u16({"m": 0xC0DE}, "m")
            st.u16(o, "version", default=3)
            _cnt(st, o, "count", len(rules), "u16")
        else:
            _cnt(st, o, "count", len(rules), "u16")
            o["version"] = 0
    else:
        first = st.u16({}, "first")
        if first == 0xC0DE:
            o["form"] = "C0DE"
            st.u16(o, "version")
            st.u16(o, "count")
        else:
            o["form"] = "old"
            o["count"] = first
            o["version"] = 0
    ver = o["version"]
    for i in range(o["count"]):
        r = rules[i] if st.writing else {}
        if not st.writing:
            rules.append(r)
        if ver >= 3:
            st.u16(r, "a"); st.u16(r, "b"); st.u16(r, "c")
        vars_block(st, r.setdefault("locals", {}))
        nm = r.get("name") or b""
        ln = _cnt(st, r, "len_name", len(nm))
        if ln:
            _bytes(st, r, "name", ln)
        if ver < 3:
            st.u16(r, "a")
            if ver >= 1:
                st.u16(r, "b"); st.u16(r, "c")
        for part in ("in", "out", "exec"):
            nodes = r.get(part) or []
            n = _cnt(st, r, "n_" + part, len(nodes))
            if n:
                _words(st, r, part, n, 2)
                _words(st, r, part + "_lines", n, 1)
            else:
                r[part] = []
        if ver >= 2:
            st.u16(r, "d"); st.u16(r, "e")
    if ver >= 1:
        secs = o.get("sectors") or []
        n = _cnt(st, o, "n_sectors", len(secs))
        if n:
            _words(st, o, "sectors", n, 2)
        else:
            o["sectors"] = []
    st.u32(o, "tail")
    return o
