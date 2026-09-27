"""Zone / collision / group modifier streams:

    GRP  type  4   grp()      + grprefs()  (the reference list a GRP may point at)
    ZDE  type 19   zde()
    COB  type 20   cob()      + gmatlib()  (the game / sound material libraries it points at)
    VP   type 24   vp()
    OCC  type 35   occ()
    TRG  type 39   trg()

Every function walks the fields in loader order against a stream Reader or Writer; `refs_<name>(o)` lists the keys
the record passes to LOA_MakeFileRef.

Loaders (RGH Wii executable):
  GRP::p_Callback_LoadGRP, GRP::p_Callback_LoadGrpRefs, GRP::p_Callback_LoadGrp
  ZDE::p_Callback_LoadZDE, MTH_pu8_Load{Box,Sphere}FromBuffer
  COB::p_Callback_LoadCOB, COB::NavDataLoad, COB::Q3LoadFromBuffer, Q3LoadNodes,
      COB::NavLayerLoad, COB::ElemDataLoad, COB::NavWallLoad, COB::NavNodeLoad,
      COB::NavLinkLoad, COB::NavZoneListLoad, COB::NavRoutingLoad,
      COB::p_Callback_LoadGMATLib, COB::LoadGMAT
  OCC::p_Callback_LoadOCC
  TRG::p_Callback_LoadTRG, TRG::SetTRGType,
      TRG::{Rectangles,Skidmarks,Profiler,XMen}_LoadFromBuffer
  VP::p_Callback_LoadVP, VP::p_LoadViewpoint

What RGH ships (every world): GRP v2, ZDE v4, COB v27 (0x1B), OCC v2, TRG v12 (types 1 and 2 only), VP v8.
  GRP   the key goes to GRP::p_Callback_LoadGrpRefs (801030E4), the member keys to OBJ::p_Callback_LoadObject;
        GRP::p_Callback_LoadGrp is not reached from a GRP.
  ZDE   shapes 0 (sphere) and 1 / 2 (box).
  COB   the mode-1 word after the capsule (version >= 0x1A) is read and dropped.  0 is the "no key" value.
  TRG   for version < 3 the loaders add 1 to the stream type before choosing a sub-loader.
  VP    the viewpoint version is the MDF version unless that is 0 (then it is a leading u32); nothing is read for
        viewpoint versions above 8.

Non-MDF resources reached through LOA_MakeFileRef:
  grprefs()  LOA_p_LoadRef(b_Refs=1) and no data at all: an inline reference list in a package (u32 count, 12-byte
             refs; no memsize word).  RGH holds one (AD00188D in FFF0557F, empty).
  gmatlib()  memsize, u32 count, u32 version, count x LoadGMAT (84 bytes each in a package).

Every GRP (4777), ZDE (4626), COB (2682), OCC (36), TRG (18) and VP (2) record parses, and so do the 127 material
libraries the COB keys name (three carry version 0xAAAAAAAA, which the loaders treat as 0) and the GrpRefs record.
Not in the data: ZDE shape 3, COB nav layers (the Q3 / layer / wall / node / link streams follow the loaders alone),
COB capsules, TRG profiler and xmen sub-streams, VP object lists.
"""
from __future__ import annotations

from . import stream as F
from .stream import FormatError

GRP_TYPE, ZDE_TYPE, COB_TYPE, VP_TYPE, OCC_TYPE, TRG_TYPE = 4, 19, 20, 24, 35, 39
NONE32 = 0xFFFFFFFF


# ----------------------------------------------------------------------------
# helpers
def _count(st, o, name, value, fn="u32"):
    """A count or presence word: derived from the content when writing."""
    if st.writing:
        o[name] = value
    return getattr(st, fn)(o, name)


def _list(st, o, name, count_name, fn="u32"):
    """The list o[name]; its length is streamed as o[count_name] (read: the list is filled with that many empty dicts
    for the caller to fill)."""
    lst = o.setdefault(name, [])
    n = _count(st, o, count_name, len(lst), fn)
    if not st.writing:
        lst.extend({} for _ in range(n))
    return lst


def _fixed(st, o, name, n):
    """A list whose length n is known from elsewhere in the stream."""
    lst = o.setdefault(name, [])
    if st.writing:
        if len(lst) != n:
            raise FormatError(f"{name}: {len(lst)} entries, expected {n}")
    else:
        lst.extend({} for _ in range(n))
    return lst


# ============================================================================
# GRP::p_Callback_LoadGRP  (RGH 80100660)
# ============================================================================
def grp(st, o):
    st.memsize(o)
    F.mdf_header(st, o)
    ver = o["version"]
    if ver > 3:
        raise FormatError(f"GRP version {ver}")
    st.u32(o, "grprefs")                  # -> GRP::p_Callback_LoadGrpRefs; 0 = none
    if ver >= 1:
        for e in _list(st, o, "objects", "n_objects"):
            st.u32(e, "key")              # -> OBJ::p_Callback_LoadObject
            st.u32(e, "u1")               # read and dropped (always 0 in RGH's data)
        st.name64(o, "m3_name", 3)
        if ver >= 2:
            st.u32(o, "m3_u0", 3)


def refs_grp(o) -> list[int]:
    keys = [o.get("grprefs", 0)] + [e.get("key", 0) for e in o.get("objects", [])]
    return [k for k in keys if k]


def grprefs(st, o):
    """GRP::p_Callback_LoadGrpRefs (RGH 801030E4): LOA_p_LoadRef(b_Refs=1) and no Priv_Read at all - every reference is
    passed to LOA_MakeFileRef and its UserFlags low word kept as the member's channel."""
    st.refs(o, "refs")


def refs_grprefs(o) -> list[int]:
    return [r.key for r in o.get("refs", []) if r.key]


# ============================================================================
# ZDE::p_Callback_LoadZDE  (RGH 8016CF4C)
# ============================================================================
def zde(st, o):
    st.memsize(o)
    F.mdf_header(st, o)
    ver = o["version"]
    if ver > 5:
        raise FormatError(f"ZDE version {ver}")
    if ver >= 1:
        st.u32(o, "snp")                  # lowest set bit of the low byte -> mu8_SnP
    t = st.u8(o, "shape_type")            # 0 sphere, 1 / 2 box
    if ver == 1 and t in (2, 3):          # the loader renumbers version-1 types
        t = 1
    st.u8(o, "flags")                     # mu8_Flags
    if ver > 3:
        st.u8(o, "flags_init")
    st.u32(o, "design")                   # mu32_Design
    if ver > 2:
        st.u32(o, "m1_u0", 1)
    if t in (1, 2):
        st.box(o, "box")
    elif t == 0:
        st.sphere(o, "sphere")
    st.u32(o, "merge_id")


def refs_zde(o) -> list[int]:
    return []


# ============================================================================
# COB::p_Callback_LoadCOB  (RGH 801A92E0)
# ============================================================================
def cob(st, o):
    st.memsize(o)
    F.mdf_header(st, o)
    ver = o["version"]
    if ver > 0x1D:
        raise FormatError(f"COB version {ver:#x}")
    if ver >= 1:
        st.u32(o, "u0")                   # read and dropped
    st.u32(o, "geometry")                 # -> K3D_visual::p_Callback_LoadVisual
    if ver >= 0x19:
        if _count(st, o, "has_capsule", 1 if o.get("capsule") else 0):
            c = o.setdefault("capsule", {})
            st.f32(c, "radius"); st.f32(c, "height"); st.v3(c, "offset")
            st.i32(c, "gmat_id"); st.i32(c, "smat_id")
    if ver >= 0x1A:
        st.u32(o, "m1_u1", 1)             # read and dropped
    st.u32(o, "gmat_lib")                 # -> COB::p_Callback_LoadGMATLib (game materials)
    if ver >= 3:
        st.u32(o, "smat_lib")             # -> COB::p_Callback_LoadGMATLib (sound materials)
    if ver >= 2:
        st.u16(o, "cob_type"); st.u16(o, "dummy")
    if ver >= 4:
        cob_nav(st, o.setdefault("nav", {}), ver)


def refs_cob(o) -> list[int]:
    keys = [o.get("geometry", 0), o.get("gmat_lib", 0), o.get("smat_lib", 0)]
    return [k for k in keys if k]


def cob_ref_kinds(o) -> list[tuple[str, int]]:
    """The same keys with what they are: ('geo', visual), ('gml', game material library), ('gms', sound material
    library)."""
    out = []
    for kind, name in (("geo", "geometry"), ("gml", "gmat_lib"), ("gms", "smat_lib")):
        if o.get(name):
            out.append((kind, o[name]))
    return out


def cob_nav(st, o, ver):
    """COB::NavDataLoad (RGH 801BA3B8)."""
    if ver >= 0x10:
        st.u32(o, "navmesh_merge_id", default=NONE32)
    layers = _list(st, o, "layers", "n_layers", "u16")
    if layers:
        cob_q3(st, o.setdefault("q3", {}), ver)
        for L in layers:
            cob_nav_layer(st, L, ver)
    if ver >= 0x12:
        st.f32(o, "m1_f0", 1)
    if ver >= 0xE:
        # COB::NavZoneListLoad
        for z in _list(st, o, "zones", "n_zones", "u16"):
            st.u16(z, "id")
            n = _count(st, z, "n_items", len(z.get("items", [])), "u16")
            st.v3(z, "v3_a"); st.v3(z, "v3_b")
            if ver <= 0xE:
                st.u32(z, "old_u")
            st.array(z, "items", "<H", n)
    if ver >= 0x18:
        # COB::NavRoutingLoad: n nodes, each (n >> 4) + 1 words (2 bits per node)
        n = st.u16(o, "n_routing")
        st.array(o, "routing", "<I", n * ((n >> 4) + 1))


def cob_q3(st, q, ver):
    """COB::Q3LoadFromBuffer: boxes with their triangle lists, then the node tree (no RGH COB has nav layers)."""
    boxes = _list(st, q, "boxes", "n_boxes")
    if not boxes:
        return
    for b in boxes:
        tris = _list(st, b, "tris", "n_tris", "u16")
        st.v3(b, "v3_c")                  # stored at box+0xC
        st.v3(b, "v3_0")                  # stored at box+0
        for t in tris:
            st.u16(t, "a"); st.u16(t, "b")
            if ver < 0x14:
                n1 = _count(st, t, "n_old1", len(t.get("old1", [])), "u16")
                st.array(t, "old1", "<H", n1)
                n2 = _count(st, t, "n_old2", len(t.get("old2", [])), "u16")
                st.array(t, "old2", "<H", n2)
    _q3_nodes(st, q)


def _q3_nodes(st, q):
    """Q3LoadNodes flattened.  Per node: u32 box index, u32 tag; tag 1 = a son subtree follows and then another tag,
    tag 2 = a next subtree follows and then another tag, anything else ends the node.  Kept as the flat word list in
    stream order (q['nodes'])."""
    if st.writing:
        st.array(q, "nodes", "<I", len(q.get("nodes", [])))
        return
    out = []

    def word():
        v = st.u32({}, "w")
        out.append(v)
        return v

    stack = []
    state = "node"
    while True:
        if state == "node":
            word()
            t = word()
            if t == 1:
                stack.append("after_son")
                continue
            if t == 2:
                stack.append("after_next")
                continue
        elif state == "after_son":
            if word() == 2:
                stack.append("after_next")
                state = "node"
                continue
        elif state == "after_next":
            word()
        if not stack:
            break
        state = stack.pop()
    q["nodes"] = out


def cob_nav_layer(st, L, ver):
    """COB::NavLayerLoad and its four sub-loaders (no RGH COB has nav layers)."""
    st.u16(L, "layer")
    st.array(L, "forbidden_gmat", "<I", 8)
    if ver > 4:
        st.array(L, "crossable_gmat", "<I", 8)
    st.f32(L, "angle_slope"); st.f32(L, "actor_size")
    # COB::ElemDataLoad
    for ed in _list(st, L, "elem_data", "n_elem_data"):
        for sub in _list(st, ed, "subs", "n_subs"):
            if _count(st, sub, "present", 1 if "a" in sub else 0, "u8"):
                na = _count(st, sub, "n_a", len(sub.get("a", [])) // 2, "u16")
                st.array(sub, "a", "<H", 2 * na)
                if ver > 4:
                    nb = _count(st, sub, "n_b", len(sub.get("b", [])) // 2, "u16")
                    st.array(sub, "b", "<H", 2 * nb)
                nc = _count(st, sub, "n_c", len(sub.get("c", [])), "u16")
                st.array(sub, "c", "<H", nc)
    # COB::NavWallLoad
    for w in _list(st, L, "walls", "n_walls", "u16"):
        if ver > 4:
            st.u32(w, "u0")
        if ver > 5:
            st.u16(w, "u1")
        n = _count(st, w, "n_points", len(w.get("idx", [])), "u16")
        if n:
            st.array(w, "idx", "<H", n)
            st.array(w, "f", "<f", n)
            if ver < 9:
                st.raw(w, "old_v3", n * 12)
            else:
                for p in _fixed(st, w, "planes", n):
                    st.i32(p, "a"); st.i32(p, "b"); st.f32(p, "f")
        if ver < 0x13:
            n1 = _count(st, w, "n_old16", len(w.get("old16", [])), "u16")
            st.array(w, "old16", "<H", n1)
        if ver < 0x16:
            n2 = _count(st, w, "n_old32", len(w.get("old32", [])), "u16")
            st.array(w, "old32", "<I", n2)
    # COB::NavNodeLoad
    for nd in _list(st, L, "nodes", "n_nodes", "u16"):
        st.u16(nd, "u0"); st.u16(nd, "u1"); st.u32(nd, "u2")
        if ver < 8:
            n1 = _count(st, nd, "n_old1", len(nd.get("old1", b"")) // 12, "u16")
            st.raw(nd, "old1", n1 * 12)
            n2 = _count(st, nd, "n_old2", len(nd.get("old2", b"")) // 12, "u16")
            st.raw(nd, "old2", n2 * 12)
        elif ver != 0x15:
            for k in (0, 1):
                for it in _list(st, nd, f"list{k}", f"n_list{k}", "u16"):
                    st.u16(it, "a"); st.u16(it, "b"); st.f32(it, "f")
    # COB::NavLinkLoad
    for lk in _list(st, L, "links", "n_links", "u16"):
        st.u16(lk, "u0"); st.u16(lk, "u1"); st.f32(lk, "f0"); st.u32(lk, "u2")
        if ver < 7:
            st.array(lk, "old8", "<I", 8)
        n = _count(st, lk, "n_pts", len(lk.get("pts", [])), "u16")
        st.array(lk, "pts", "<I", n)
        st.raw(lk, "segs", max(n - 1, 0) * 12)     # n - 1 vectors
        if ver > 4:
            m = _count(st, lk, "n_ext", len(lk.get("ext", [])), "u16")
            st.array(lk, "ext", "<H", m)


# ----------------------------------------------------------------------------
# COB::p_Callback_LoadGMATLib  (RGH 801A6268)
# ----------------------------------------------------------------------------
def gmatlib(st, o):
    st.memsize(o)
    gm = _list(st, o, "gmats", "n_gmats")
    st.u32(o, "version", default=1)       # > 1000 is treated as 0 by the loaders
    for g in gm:
        gmat(st, g)


def gmat(st, g):
    """COB::LoadGMAT (RGH 801A6064): field names from COB_tt_GameMaterial_."""
    st.u32(g, "id")
    st.f32(g, "slide"); st.f32(g, "rebound")
    st.f32(g, "f3"); st.f32(g, "f4")       # read and dropped
    st.u32(g, "sound"); st.u32(g, "param1"); st.u32(g, "param2"); st.u32(g, "crossable")
    st.u32(g, "backface_skip")            # read and dropped
    st.f32(g, "dynamic_friction"); st.f32(g, "static_friction"); st.f32(g, "restitution")
    st.f32(g, "dynamic_friction_v"); st.f32(g, "static_friction_v")
    st.v3(g, "dir_of_anisotropy")
    st.u32(g, "flags"); st.u32(g, "friction_combine"); st.u32(g, "restitution_combine")
    st.raw(g, "m1_name", 64, 1)
    st.u32(g, "m1_u0", 1); st.u32(g, "m1_u1", 1)


def refs_gmatlib(o) -> list[int]:
    return []


# ============================================================================
# OCC::p_Callback_LoadOCC  (RGH 80112D4C)
# ============================================================================
def occ(st, o):
    st.memsize(o)
    F.mdf_header(st, o)
    ver = o["version"]
    if ver > 2:
        raise FormatError(f"OCC version {ver}")
    if ver < 2:
        st.box(o, "box")
    else:
        n = _count(st, o, "n_vertices", len(o.get("vertices", b"")) // 12, "u8")
        st.raw(o, "vertices", n * 12)


def refs_occ(o) -> list[int]:
    return []


# ============================================================================
# TRG::p_Callback_LoadTRG  (RGH 80199088)
# ============================================================================
def trg(st, o):
    st.memsize(o)
    F.mdf_header(st, o)
    ver = o["version"]
    if ver > 12:
        raise FormatError(f"TRG version {ver}")
    t = st.u32(o, "trg_type")             # 1 rectangles, 2 skidmarks, 3 profiler, 4 xmen
    if ver < 3:
        t += 1
    if ver > 1:
        st.u16(o, "mat_id"); st.u8(o, "zlist")
    if ver > 0xB:
        st.u8(o, "render_list_order")
    if ver >= 7:
        st.u32(o, "draw_mask")
    if t == 1:
        _trg_rectangles(st, o.setdefault("rectangles", {}), ver)
    elif t == 2:
        _trg_skidmarks(st, o.setdefault("skidmarks", {}), ver)
    elif t == 3:
        _trg_profiler(st, o.setdefault("profiler", {}), ver)
    elif t == 4:
        _trg_xmen(st, o.setdefault("xmen", {}), ver)
    st.box(o, "box")
    st.u32(o, "material")                 # -> K3D_material::p_Callback_LoadMaterial
    if ver < 3:
        st.u32(o, "old_u")


def refs_trg(o) -> list[int]:
    s = o.get("skidmarks", {})
    keys = [o.get("material", 0), s.get("curve0", 0), s.get("curve1", 0)]
    return [k for k in keys if k]


def _trg_rectangles(st, r, ver):
    st.u32(r, "u0"); st.f32(r, "f1"); st.f32(r, "f2")
    if ver > 2:
        st.u32(r, "u3")
    if ver > 5:
        st.f32(r, "f4"); st.f32(r, "f5"); st.u32(r, "color")


def _trg_skidmarks(st, s, ver):
    st.u32(s, "n_marks"); st.f32(s, "f0"); st.f32(s, "f1"); st.f32(s, "f2")
    if ver > 2:
        st.u32(s, "u3")
    if ver > 3:
        st.u32(s, "u4")
    if ver > 4:
        st.u32(s, "curve0")               # -> MTH_p_Callback_CurveLoad
    if ver > 5:
        st.u32(s, "color")
    if ver >= 0xB:
        st.u32(s, "curve1")               # -> MTH_p_Callback_CurveLoad


def _trg_profiler(st, p, ver):
    """TRG::Profiler_LoadFromBuffer (names from TRG_tt_Profiler_; no RGH TRG is a profiler)."""
    if ver < 0xA:
        return
    if st.u32(p, "profiler_version", default=1) < 1:
        return
    st.u32(p, "preset")
    st.v3(p, "friction_xyz"); st.f32(p, "gravity"); st.f32(p, "segment_size_min")
    st.v3(p, "normal"); st.v4(p, "tangent")
    st.v3(p, "gx"); st.v3(p, "gy"); st.v3(p, "gz"); st.v3(p, "gt")
    st.u16(p, "u_tiler")
    st.u32(p, "relatif"); st.u32(p, "compute_normal"); st.u32(p, "one_uv_per_point")
    st.u32(p, "model_u2")
    links = _list(st, p, "model_links", "n_model_links")
    points = _list(st, p, "model_points", "n_model_points")
    for pt in points:
        st.v3(pt, "v0"); st.v3(pt, "v1"); st.v3(pt, "v2"); st.v3(pt, "v3"); st.v3(pt, "v4")
        st.f32(pt, "f"); st.u32(pt, "color_a"); st.u32(pt, "color_b")
    for ln in links:
        st.u32(ln, "a"); st.u32(ln, "b"); st.f32(ln, "f0"); st.f32(ln, "f1")


def _trg_xmen(st, x, ver):
    """TRG::XMen_LoadFromBuffer (names from TRG_tt_XMen_; no RGH TRG is an xmen trail)."""
    if ver < 8:
        return
    st.u32(x, "flags"); st.f32(x, "dt_min"); st.u32(x, "segs_po2")
    trails = _list(st, x, "trails", "n_trails")
    st.u32(x, "projection"); st.u32(x, "color")
    for tr in trails:
        st.u32(tr, "u0"); st.f32(tr, "f0")
        if ver >= 9:
            st.v3(tr, "v0"); st.v3(tr, "v1")


# ============================================================================
# VP::p_Callback_LoadVP  (RGH 80161E44)
# ============================================================================
def vp(st, o):
    st.memsize(o)
    F.mdf_header(st, o)
    ver = o["version"]
    vp_viewpoint(st, o.setdefault("viewpoint", {}), ver)
    n = _count(st, o, "n_objects", len(o.get("objects", [])))
    st.array(o, "objects", "<I", n)       # object keys, resolved after loading (no file ref)
    st.u32(o, "back_color")               # ABGR in the file


def vp_viewpoint(st, v, mdf_ver):
    """VP::p_LoadViewpoint (RGH 8016140C): field names from K3D_tt_ViewPoint."""
    if mdf_ver == 0:
        vv = st.u32(v, "vp_version")
    else:
        vv = v["vp_version"] = mdf_ver
    if vv <= 6:
        st.u32(v, "width"); st.u32(v, "height")
        if vv >= 4:
            st.u32(v, "old_a"); st.u32(v, "old_b")      # read and dropped
        st.f32(v, "fov"); st.u32(v, "id"); st.u32(v, "flags")
        st.u32(v, "texture")              # -> K3D_texture::p_Callback_LoadTexture
        if vv in (1, 5):
            st.u32(v, "old_c")            # read and dropped
        st.u32(v, "type")
        if vv >= 3:
            st.u32(v, "subtype")
        st.array(v, "params", "<I", 4)
    elif vv <= 8:
        st.f32(v, "fov"); st.u32(v, "id"); st.u32(v, "flags")
        st.u32(v, "texture")
        st.u32(v, "type"); st.u32(v, "subtype")
        st.array(v, "params", "<I", 4)
        st.u16(v, "m1_u16a", 1); st.u16(v, "m1_u16b", 1)
    else:
        raise FormatError(f"viewpoint version {vv} (RGH reads nothing above 8)")


def refs_vp(o) -> list[int]:
    k = o.get("viewpoint", {}).get("texture", 0)
    return [k] if k else []


def vp_object_keys(o) -> list[int]:
    """The objects a viewpoint renders: keys of objects of the same world, not file references."""
    return [k for k in o.get("objects", []) if k]


STREAMS = {GRP_TYPE: grp, ZDE_TYPE: zde, COB_TYPE: cob, VP_TYPE: vp, OCC_TYPE: occ, TRG_TYPE: trg}
REFS = {GRP_TYPE: refs_grp, ZDE_TYPE: refs_zde, COB_TYPE: refs_cob, VP_TYPE: refs_vp,
        OCC_TYPE: refs_occ, TRG_TYPE: refs_trg}
