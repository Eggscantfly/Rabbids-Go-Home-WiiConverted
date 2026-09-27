"""VIF modifier (visual instances): VIF::p_Callback_LoadVIF (RGH 801E8870).

An MDF header, a word only loose files keep, then `VisualNb` entries of {visual, material, render parameters}
followed by, per entry, an instance flag word and a list of transform matrices.  RGH ships version 4.
"""
from __future__ import annotations

from . import stream as F

VIF_TYPE = 42


def vif(st, o):
    st.memsize(o)
    F.mdf_header(st, o)
    ver = o["version"]
    st.u32(o, "m1_u0", 1)
    visuals = o.setdefault("visuals", [])
    if st.writing:
        o["n_visuals"] = len(visuals)
    n = st.u32(o, "n_visuals")
    for i in range(n):
        v = visuals[i] if st.writing else {}
        if not st.writing:
            visuals.append(v)
        st.u32(v, "flags")
        st.u32(v, "visual")
        st.u32(v, "material")
        st.u8(v, "zlist")
        if ver > 2:
            st.u8(v, "render_list_order")
        st.u8(v, "light_mask")
        st.f32(v, "obj_shadow_bias"); st.f32(v, "auto_far_cull_factor")
        st.f32(v, "glow_blur"); st.f32(v, "glow_intensity")
        st.array(v, "rli_id", "<I", 2)
        st.array(v, "rli_factor", "<f", 2)
        if ver >= 2:
            st.u32(v, "u_a"); st.u32(v, "u_b")
        st.f32(v, "rli_blend_alpha")
        st.u32(v, "inst_flags")
        st.u32(v, "m1_u1", 1)
        if ver >= 4:
            st.u32(v, "far_fading")
    for i in range(n):
        v = visuals[i]
        st.u32(v, "flags2")
        mats = v.setdefault("matrices", [])
        if st.writing:
            v["n_matrices"] = len(mats)
        m = st.u32(v, "n_matrices")
        for k in range(m):
            if st.writing:
                st.trm({"m": mats[k]}, "m")
            else:
                mats.append(st.trm({}, "m"))
    return o
