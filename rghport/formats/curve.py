"""MTH curves (.crv): MTH_CurveLoadBuffer (RGH 8008A004).  Referenced by GOM_sinus (three curves per modifier) and by
other animated parameters.

    u32 version; f32 a, b, c, d; u32 count; [ver > 2] u32 flags;
    [ver != 0] mode-1 f32 x2; f32[count]; [ver > 3 and count and flags & 2] u32 block: count // 4 words in most
    records, one fewer in the curves 9D0042D1 / 9D0042D4 (the reader takes what the record holds)
"""
from __future__ import annotations

from .stream import FormatError


def curve(st, o):
    st.memsize(o)
    ver = st.u32(o, "version", default=4)
    if ver > 4:
        raise FormatError(f"curve version {ver}")
    st.f32(o, "f0"); st.f32(o, "f2"); st.f32(o, "f1"); st.f32(o, "f3")
    if st.writing:
        o["count"] = len(o.get("values", [])) if isinstance(o.get("values"), list) else o.get("count", 0)
    n = st.u32(o, "count")
    flags = 0
    if ver > 2:
        flags = st.u32(o, "flags")
    if ver != 0:
        st.f32(o, "m1_f0", 1); st.f32(o, "m1_f1", 1)
    if n:
        st.array(o, "values", "<f", n)
    if ver > 3 and n and (flags & 2):
        # the trailing u32 block: n // 4 words in most records, one fewer in the curves 9D0042D1 (24 values, 5 words)
        # and 9D0042D4 (34 values, 7 words), which n // 4 would read past their end: read what the record holds
        # (at most n // 4), write back what was read
        cnt = len(o.get("extra") or []) if st.writing else min(n // 4, max(0, (st.end - st.pos) // 4))
        st.array(o, "extra", "<I", cnt)
    return o


def refs_curve(o):
    return []
