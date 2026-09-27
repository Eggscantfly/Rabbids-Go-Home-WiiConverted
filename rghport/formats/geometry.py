"""K3D visuals: the geometric mesh stream, its GOM chain, LODs.

Field order and modes follow the RGH Wii executable's loaders for version 0x25, the version every RGH mesh carries.
A package record (level 3) keeps the vertex data in a Wii vertex buffer (flags & 0x80) and drops every mode-1 field.
"""
from __future__ import annotations

import numpy as np

from . import stream as F
from .stream import FormatError

MAGIC = 0xC0DEC0DE
NONE32 = 0xFFFFFFFF

# GOM::RegisterAll order (RGH 8030CB3C): the stream tag is the index.
GOM_NAMES = {0: "sinus", 1: "empty2", 2: "skin", 3: "morph", 4: "recnor", 5: "spring",
             6: "ffd", 7: "empty1", 8: "vertices", 9: "vgizmo", 10: "fur", 11: "empty3"}


# ----------------------------------------------------------------------------
# helpers
def _count(st, o, name, value, fn="u32"):
    """A count or presence flag: derived from the content when writing."""
    if st.writing:
        o[name] = value
    return getattr(st, fn)(o, name)


def _blob(st, o, name, nbytes, mode=2):
    """An opaque byte run (numpy-backed arrays are stored as bytes)."""
    if st.writing:
        v = o.get(name, b"")
        if isinstance(v, np.ndarray):
            v = v.tobytes()
        if st.present(mode):
            if len(v) != nbytes:
                raise FormatError(f"{name}: {len(v)} bytes, expected {nbytes}")
            st.buf += v
        return v
    return st.raw(o, name, nbytes, mode)


# ----------------------------------------------------------------------------
# GOMs
def gom_skin(st, g):
    """GOM_skin::po_CreateFromBuffer (RGH 80312CC8)."""
    ver = st.u32(g, "version", default=10)
    st.u32(g, "flags")
    st.u8(g, "m1_u8", 1)
    st.u8(g, "max_pond")
    lists = g.setdefault("lists", [])
    n = _count(st, g, "n_lists", len(lists), "u16")
    for i in range(n):
        L = lists[i] if st.writing else {}
        if not st.writing:
            lists.append(L)
        st.u16(L, "id")
        nb = _count(st, L, "nb", len(L.get("weights", b"")) // 6, "u16")
        st.m44(L, "matrix")
        # nb x (f32 weight, u16 vertex): the 6-byte stride is kept as one blob
        _blob(st, L, "weights", nb * 6)
    if ver <= 7:
        if ver >= 6:
            st.f32(g, "pond_precision")
        if ver >= 7:
            st.f32(g, "pond_precision7"); st.f32(g, "threshold"); st.f32(g, "reassort")
    else:
        if ver >= 9:
            st.i8(g, "shadow_lod0"); st.i8(g, "shadow_lod1"); st.i8(g, "shadow_lod2")
        lods = g.setdefault("lods", [])
        nlod = _count(st, g, "n_lod", len(lods), "u8")
        if ver < 10:
            nlod = 4
        for i in range(nlod):
            L = lods[i] if st.writing else {}
            if not st.writing:
                lods.append(L)
            st.f32(L, "pond_precision"); st.f32(L, "threshold"); st.f32(L, "reassort")
            st.u32(L, "flags"); st.f32(L, "validity_distance")
    st.u32(g, "m1_u32", 1)
    # the bind matrices (mode 1): n_matrices of them, read over two loops with one counter (the first 256 into the GOM,
    # the rest skipped); the loaders fill identities up to 256
    n = _count(st, g, "n_matrices", len(g.get("bind") or []) if st.writing else 0)
    if not st.writing:
        g["bind"] = [st.m44({}, "m", 1) for _ in range(n)]
    else:
        for m in g.get("bind") or [None] * n:
            st.m44({"m": m} if m is not None else {}, "m", 1)
    return g


def gom_morph(st, g):
    """GOM_morph::po_CreateFromBuffer (RGH 8030DCC0)."""
    ver = st.u32(g, "version", default=3)
    st.u32(g, "flags")
    st.u32(g, "nb_points")
    targets = g.setdefault("targets", [])
    nt = _count(st, g, "nb_targets", len(targets))
    nseq = 0
    if ver <= 1:
        nseq = _count(st, g, "nb_seq", len(g.get("seqs", [])))
    for i in range(nt):
        t = targets[i] if st.writing else {}
        if not st.writing:
            targets.append(t)
        if ver >= 3:
            st.u32(t, "m3_u32", 3)
        kind = st.u32(t, "kind") if ver >= 1 else 0
        if kind == 0:
            cnt = _count(st, t, "count", len(t.get("idx", b"")) // 4)
            st.raw(t, "name", 64, 3)
            if cnt:
                _blob(st, t, "idx", cnt * 4)
                _blob(st, t, "offsets", cnt * 12)
        else:
            _blob(st, t, "u32x6", 24)
            _blob(st, t, "f32x6", 24)
            st.raw(t, "name", 64, 3)
    seqs = g.setdefault("seqs", [])
    for i in range(nseq):
        s = seqs[i] if st.writing else {}
        if not st.writing:
            seqs.append(s)
        st.f32(s, "f")
        st.raw(s, "name", 64, 3)
        keys = s.setdefault("keys", [])
        for k in range(4):
            K = keys[k] if st.writing else {}
            if not st.writing:
                keys.append(K)
            _blob(st, K, "u32x6", 24)
            _blob(st, K, "f32x6", 24)
            st.f32(K, "f")
            st.f32(K, "m1_f", 1)
    return g


def gom_sinus(st, g):
    """GOM_sinus::po_CreateFromBuffer (RGH 8031064C)."""
    ver = st.u32(g, "version", default=4)
    st.u32(g, "flags")
    n = 3 if ver == 0 else 12
    vals = g.setdefault("vals", [0.0] * n)
    for i in range(n):
        if st.writing:
            st.f32({"v": vals[i]}, "v")
        else:
            vals[i] = st.f32({}, "v")
    curves = g.setdefault("curves", [])
    if ver > 1:
        for i in range(3):
            c = curves[i] if st.writing else []
            if not st.writing:
                curves.append(c)
            nk = 4 if ver > 3 else 3
            for k in range(nk):
                if st.writing:
                    st.u32({"k": c[k] if k < len(c) else 0}, "k")
                else:
                    c.append(st.u32({}, "k"))
    if ver > 2:
        st.u8(g, "rli_list")
    return g


GOM_FMT = {"skin": gom_skin, "morph": gom_morph, "sinus": gom_sinus}


def gom_chain(st, o):
    goms = o.setdefault("goms", [])
    if st.writing:
        for g in goms:
            tag = next(k for k, v in GOM_NAMES.items() if v == g["type"])
            st.u32({"t": tag}, "t")
            GOM_FMT[g["type"]](st, g)
        st.u32({"t": NONE32}, "t")
        return
    while True:
        tag = st.u32({}, "t")
        if tag == NONE32:
            break
        name = GOM_NAMES.get(tag)
        fn = GOM_FMT.get(name)
        if fn is None:
            raise FormatError(f"GOM {name or tag} not supported")
        g = {"type": name}
        fn(st, g)
        goms.append(g)


# ----------------------------------------------------------------------------
# vertex / index buffers (package records only)
def static_vb(st, vb):
    """K3D_VertexBufferManager_Base::LoadStaticFromBuffer."""
    magic = st.u32(vb, "magic", default=0xC0DE0002)
    size = _count(st, vb, "size", len(vb.get("data", b"")))
    if magic >= 0xC0DE0001:
        st.u32(vb, "off_uv1", default=NONE32); st.u32(vb, "off_uv2", default=NONE32); st.u32(vb, "off_nrm", default=NONE32)
    else:
        vb.setdefault("off_uv1", NONE32); vb.setdefault("off_uv2", NONE32); vb.setdefault("off_nrm", NONE32)
    if magic >= 0xC0DE0002:
        st.u32(vb, "off_col", default=NONE32)
    else:
        vb.setdefault("off_col", NONE32)
    _blob(st, vb, "data", size)
    return vb


def index_buffer(st, ib):
    """K3D_VertexBufferManager_Base::LoadIndexBufferFromBuffer."""
    magic = st.u32(ib, "magic", default=0xC0DE0001)
    n = _count(st, ib, "index_number", len(ib.get("data", b"")) // max(ib.get("index_size", 2), 1))
    isz = st.u32(ib, "index_size", default=2)
    if magic >= 0xC0DE0001:
        st.u32(ib, "attribute_mask")
    st.u32(ib, "vertex_number")
    _blob(st, ib, "data", n * isz)
    return ib


def ib_set(st, s):
    """K3D_VertexBufferManager_Base::LoadIndexBufferSetFromBuffer."""
    hdr = st.u32(s, "header", default=0xC0DE0004)
    ver = hdr & 0xFFF
    sorted_set = (hdr & 0xF000) == 0x1000
    s["sorted"] = sorted_set
    elems = s.setdefault("elements", [])
    if sorted_set:
        ne = _count(st, s, "n_elements", len(elems))
        nsrc = _count(st, s, "n_sources", len(s.get("sources", b"")) // 2)
        if ver >= 5:
            st.u32(s, "v5_u32")
        for i in range(ne):
            e = elems[i] if st.writing else {}
            if not st.writing:
                elems.append(e)
            st.u32(e, "a"); st.u32(e, "b"); st.u32(e, "c"); st.u32(e, "d")
            if ver >= 3:
                _substrips(st, e, ver)
        _blob(st, s, "sources", nsrc * 2)
        return s
    ne = _count(st, s, "n_elements", len(elems))
    ibs = s.setdefault("buffers", [])
    nib = _count(st, s, "n_buffers", len(ibs))
    st.u32(s, "topology")
    for i in range(ne):
        e = elems[i] if st.writing else {}
        if not st.writing:
            elems.append(e)
        st.u32(e, "material"); st.u32(e, "index_start")
        if ver >= 1:
            st.u32(e, "index_number")
        if ver >= 2:
            st.u32(e, "v2_u32")
        if ver >= 3:
            _substrips(st, e, ver)
    for i in range(nib):
        ib = ibs[i] if st.writing else {}
        if not st.writing:
            ibs.append(ib)
        index_buffer(st, ib)
    return s


def _substrips(st, e, ver):
    subs = e.setdefault("substrips", [])
    n = _count(st, e, "n_substrips", len(subs))
    for j in range(n):
        ss = subs[j] if st.writing else {}
        if not st.writing:
            subs.append(ss)
        if ver < 4:
            st.u32(ss, "start"); st.u32(ss, "count")
        else:
            st.u16(ss, "start"); st.u16(ss, "count")


# ----------------------------------------------------------------------------
# the mesh
def octree(st, o, nnodes):
    """K3D_geometric_Octree::LoadFromBuffer."""
    t = o.setdefault("octree", {})
    st.v3(t, "min"); st.v3(t, "max")
    nodes = t.setdefault("nodes", [])
    for i in range(nnodes):
        nd = nodes[i] if st.writing else {}
        if not st.writing:
            nodes.append(nd)
        st.raw(nd, "rel", 6)
        x = st.u16(nd, "index_son_nb_elem")
        n = (x >> 2) if (x & 3) == 0 else 0      # leaf: element count; else the sons' index
        els = nd.setdefault("elements", [])
        for j in range(n):
            el = els[j] if st.writing else {}
            if not st.writing:
                els.append(el)
            st.u16(el, "element")
            nt = _count(st, el, "n_triangles", len(el.get("triangles", b"")) // 2, "u16")
            _blob(st, el, "triangles", nt * 2)
    return t


def geometric(st, o):
    """K3D_geometric::pu32_LoadFromBuffer."""
    first = st.u32(o, "magic", default=MAGIC)
    if first == MAGIC:
        ver = st.u32(o, "version", default=0x25)
        npts = st.u32(o, "n_points")
    else:
        ver = 0
        npts = first
        o["version"] = 0
        o["n_points"] = npts
    if ver < 0x1B:
        raise FormatError(f"geometric version {ver:#x} too old")
    nnrm = st.u32(o, "n_normals")
    ntan = nbitan = 0
    if 0x1F <= ver <= 0x21:
        ntan = st.u32(o, "n_tan"); nbitan = st.u32(o, "n_bitan")
    nlp = st.u32(o, "n_lightpoints")
    nuv = st.u32(o, "n_uv")
    nuv2 = st.u32(o, "n_uv2")
    nel = _count(st, o, "n_elements", len(o.get("elements", [])))
    ncol = _count(st, o, "n_colors", len(o.get("colors", [])))
    flags = st.u32(o, "flags")
    if ver > 0x24:
        st.i32(o, "visual_version")
    in_vb = bool(flags & 0x80)
    if not in_vb:
        _blob(st, o, "points", npts * 12)
        _blob(st, o, "normals", nnrm * 12)
    if ntan:
        _blob(st, o, "tan_m1", ntan * 16, 1)
    if nbitan:
        _blob(st, o, "bitan_m1", nbitan * 12, 1)
    _blob(st, o, "lightpoints", nlp * 4)
    if not in_vb:
        _blob(st, o, "uv", nuv * 8)
        _blob(st, o, "uv2", nuv2 * 8)

    # ---- elements
    elems = o.setdefault("elements", [])
    extra = st.present(1)                   # 24 more bytes per triangle in loose files
    tri_extra = 24
    for i in range(nel):
        e = elems[i] if st.writing else {}
        if not st.writing:
            elems.append(e)
        st.u16(e, "material"); st.u16(e, "material2")
        ef = st.u16(e, "flags")
        n = _count(st, e, "n_triangles", len(e.get("tris", b"")) // 12)
        if n * 12 > 64 * 1024 * 1024:
            raise FormatError(f"element triangle count {n} implausible")
        stride = 12 + (tri_extra if extra else 0)
        if st.writing:
            tris = e.get("tris", b"")
            if isinstance(tris, np.ndarray):
                tris = tris.tobytes()
            if len(tris) != n * 12:
                raise FormatError(f"element {i}: {len(tris)} bytes of triangles for {n}")
            if extra:
                ex = e.get("tris_extra")
                if isinstance(ex, np.ndarray):
                    ex = ex.tobytes()
                if ex is None or len(ex) != n * tri_extra:
                    raise FormatError(f"element {i}: triangle extras missing or of the wrong size")
                blk = np.empty((n, stride), np.uint8)
                blk[:, :12] = np.frombuffer(tris, np.uint8).reshape(n, 12)
                blk[:, 12:] = np.frombuffer(ex, np.uint8).reshape(n, tri_extra)
                st.buf += blk.tobytes()
            else:
                st.buf += tris
        else:
            p = st._take(n * stride)
            blk = np.frombuffer(st.d, np.uint8, n * stride, p).reshape(n, stride)
            e["tris"] = blk[:, :12].tobytes()
            if extra:
                e["tris_extra"] = blk[:, 12:].tobytes()
        if (ver < 0x23 and (ef & 0x8)) or (flags & 0x4000):
            _blob(st, e, "tri_normals", n * 12)
        if ef & 0x80:
            _blob(st, e, "tri_m1_u8", n, 1)
        if ef & 0x20:
            _blob(st, e, "tri_uv2", n * 6)

    # ---- colour lists
    cols = o.setdefault("colors", [])
    for i in range(ncol):
        c = cols[i] if st.writing else {}
        if not st.writing:
            cols.append(c)
        st.raw(c, "name", 32, 3)
        st.u32(c, "id")
        nb = npts
        if ver > 0x23:
            st.u32(c, "rli_per_point_normal")
            nb = _count(st, c, "nb", len(c.get("colors", b"")) // 4)
        _blob(st, c, "colors", nb * 4)
        st.u32(c, "m1_u32a", 1); st.u32(c, "m1_u32b", 1)

    # ---- geometry modifiers
    gom_chain(st, o)

    if ver < 0x22:
        has = _count(st, o, "has_sorting_plane", 1 if o.get("sorting_plane") else 0)
        if has:
            sp = o.setdefault("sorting_plane", {})
            st.f32(sp, "f"); st.v3(sp, "m1_v3", 1); _blob(st, sp, "m1_v3x32", 32 * 12, 1)
    st.u32(o, "mesh_format")
    st.f32(o, "m1_f32", 1, default=1.0)
    gm = o.setdefault("gmat", [])
    has = _count(st, o, "has_gmat", 1 if gm else 0)
    if has:
        for i in range(nel):
            g = gm[i] if st.writing else {}
            if not st.writing:
                gm.append(g)
            n = _count(st, g, "n", len(g.get("pairs", b"")) // 4)
            _blob(st, g, "pairs", n * 4)
    has = _count(st, o, "has_vb", 1 if o.get("vb") else 0)
    if has:
        static_vb(st, o.setdefault("vb", {}))
    has = _count(st, o, "has_ib", 1 if o.get("ib") else 0)
    if has:
        ib_set(st, o.setdefault("ib", {}))
    st.u32(o, "vfmt_key")
    nind = _count(st, o, "indirection_size", len(o.get("ind_point", b"")) // 2)
    if nind:
        _blob(st, o, "ind_point", nind * 2); _blob(st, o, "ind_color", nind * 2); _blob(st, o, "ind_normal", nind * 2)
        if ver >= 0x1C:
            _blob(st, o, "ind_uv1", nind * 2); _blob(st, o, "ind_uv2", nind * 2)
    st.u32(o, "vertex_components", default=NONE32)
    kind = st.u32(o, "bv_kind", default=1)
    bv = o.setdefault("bv", {})
    if kind == 1:
        st.u32(bv, "type", default=1); st.u32(bv, "flags"); st.u32(bv, "x", default=1)
        st.v3(bv, "center"); st.v3(bv, "half")
    elif kind == 0:
        st.u32(bv, "type"); st.u32(bv, "flags"); st.u32(bv, "x", default=1)
        st.v3(bv, "center"); st.f32(bv, "radius")
    if ver < 0x20:
        raise FormatError("OK3 (version < 0x20) geometry not supported")
    nn = _count(st, o, "n_octree_nodes", len((o.get("octree") or {}).get("nodes", [])), "u16")
    if nn:
        octree(st, o, nn)
    elif ver == 0x20:
        st.v3(o, "v20_a"); st.v3(o, "v20_b")
    if ver >= 0x1D:
        nk = _count(st, o, "n_batch_keys", len(o.get("batch_keys", b"")) // 4)
        _blob(st, o, "batch_keys", nk * 4)
        st.u8(o, "batch_flags")
    else:
        st.u32(o, "batch_u32"); st.u8(o, "batch_flags")
    st.u8(o, "batch_head"); st.u8(o, "batch_count"); st.u8(o, "batch_id")
    # K3D_geometric::LoadGrpSel
    grps = o.setdefault("groups", [])
    ng = _count(st, o, "n_groups", len(grps))
    for i in range(ng):
        g = grps[i] if st.writing else {}
        if not st.writing:
            grps.append(g)
        st.u32(g, "m1_u32", 1)
        n = _count(st, g, "count", len(g.get("m1_items", b"")) // 4)
        st.raw(g, "name", 64, 1)
        _blob(st, g, "m1_items", n * 4, 1)
    for i in range(5):
        st.u32(o, f"m1_tail{i}", 1)
    return o


def lod(st, o):
    """K3D_lod::pu32_LoadFromBuffer: version byte then six {distance, geometric key}."""
    st.u8(o, "lod_version", default=1)
    lods = o.setdefault("lods", [])
    for i in range(6):
        L = lods[i] if st.writing else {}
        if not st.writing:
            lods.append(L)
        st.f32(L, "distance"); st.u32(L, "geometric")
    return o


def visual(st, o):
    """K3D_visual::p_Callback_LoadVisual: the container of a geometric mesh or a LOD set."""
    st.memsize(o)
    t = st.u32(o, "type", default=1)
    st.u16(o, "vis_flags")
    st.box(o, "box")
    if t == 1:
        geometric(st, o)
    elif t == 2:
        lod(st, o)
    else:
        raise FormatError(f"visual type {t} not supported")
    return o


def parse_visual(data, level=3, binary=True, strict=True):
    return F.parse(visual, data, level=level, binary=binary, strict=strict)
