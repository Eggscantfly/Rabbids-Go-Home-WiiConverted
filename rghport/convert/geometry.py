"""K3D visual records: Wii package format -> RGH PC package format.

    body = geometry.convert("visual", key, wii_body, ctx)      # ctx is not needed: a pure function

Both platforms serialise a visual through the same stream (formats.geometry.visual parses and re-emits Wii and PC
records byte-exactly); what differs is the content the binarizer put there:

  * Wii: geometry lives in a GX vertex buffer (K3D_geometric flags 0x80, positions f32, UVs S16.10, skinned normals
    S8.6) indexed per attribute by several u8/u16 index streams (one GX_TRIANGLESTRIP list per element substrip);
    lighting uses a 256-entry normal palette stream (mask 0x20).
  * PC:  points, normals (recomputed, angle weighted), light points and UVs inline; one D3D vertex buffer of unified
    vertices (pos f32, UV f16, normal UBYTE4N) with the indirection tables that say which point / normal / UV each
    vertex came from; one u16 index buffer holding the same strips, remapped to the unified vertices and stitched with
    degenerate indices per element.

Conversion recipe (every step checked on the 10304 Wii/PC pairs of the levels both releases share):
  1. map every element triangle to its strip triangle (positions / UVs welded on the Wii side are matched through the
     nearest index present in the strips); a PC vertex is one distinct tuple of Wii index-stream values, numbered in
     order of first appearance over the element triangles (the PC builder's order);
  2. normals: the PC binarizer's normal computation (angle/pi weighted normalised face normals) with every x87
     operation rounded to 32 bits (the binarizer ran with the FPU in single precision);
  3. UVs: GX S16.10 values were truncated from the PC floats, so the floats are estimated (lossy);
  4. vertex buffer packed like FillVertexBuffer_D3D (D3DXFloat32To16Array for UVs, trunc((n*.5+.5)*255));
  5. index buffer: Wii strips remapped and stitched (2 or 3 degenerate indices so each strip starts even).
"""
from __future__ import annotations

import collections
import copy
import struct

import numpy as np

from ..formats import geometry as geo
from ..formats import stream as F

NONE32 = 0xFFFFFFFF
F32 = np.float32
PI32 = F32(3.14159274)

# K3D_geometric flag bits the PC records never carry: 0x80 data in the (Wii) mesh vertex buffer, 0x40 Wii normal
# palette, 0x800 / 0x1000 / 0x2000 compressed normal / UV / UV2 arrays, 0x100 (set on about a third of the PC records
# by state the Wii record does not keep)
WII_ONLY_FLAGS = 0x0080 | 0x0040 | 0x0800 | 0x1000 | 0x2000

DEFAULTS = {
    "uv": "hybrid",          # "trunc": s/1024, "center": middle of the truncation interval, "hybrid": exact
                             # for multiples of 1/64 (authored values), middle otherwise
    "normal_pack": "f32",    # arithmetic of trunc((n*0.5+0.5)*255): "f32" (24-bit FPU) or "f64"
    "tri_normals": "cross",  # "cross" normalize(cross(p1-p0,p2-p0)) f32, "wii" copy, "engine" ComputeFaceNormals
    "flag_100": "clear",     # "clear" or "keep" the Wii 0x100 bit
}


class ConvertError(Exception):
    pass


def _gom_empty(st, g):
    """GOM_vgizmo (tag 9) and GOM_recnor (tag 4) keep nothing in the stream (their po_CreateFromBuffer read no field;
    every RGH record carrying them has the next tag right after)"""
    return g


class _ExtraGoms:
    """formats.geometry.gom_chain knows skin / morph / sinus only; 13 RGH meshes also chain vgizmo or recnor.
    Registered for the duration of one conversion and removed afterwards, so the format module's own behaviour is
    unchanged."""

    EXTRA = {"vgizmo": _gom_empty, "recnor": _gom_empty}

    def __enter__(self):
        self.added = [k for k in self.EXTRA if k not in geo.GOM_FMT]
        for k in self.added:
            geo.GOM_FMT[k] = self.EXTRA[k]
        return self

    def __exit__(self, *exc):
        for k in self.added:
            geo.GOM_FMT.pop(k, None)
        return False


# ----------------------------------------------------------------------------------------------------------
# entry points
def parse_visual(body, level=3, binary=True, strict=True):
    """formats.geometry.parse_visual that also accepts the vgizmo / recnor GOMs"""
    with _ExtraGoms():
        return geo.parse_visual(body, level=level, binary=binary, strict=strict)


def convert(kind, key, wii_body, ctx=None, options=None) -> bytes:
    """PC record body for one Wii package record of kind 'visual' (u32 in-memory size first)."""
    if kind != "visual":
        raise ValueError("the geometry converter converts kind 'visual', not %r" % (kind,))
    return convert_visual(wii_body, options)[0]


def convert_visual(wii_body, options=None):
    """(PC body, info dict).  Records that are not K3D_geometric visuals (K3D_lod, unknown visual types, truncated
    records) have the same bytes on both platforms and are returned unchanged."""
    opts = dict(DEFAULTS)
    opts.update(options or {})
    info = {"kind": "copy"}
    with _ExtraGoms():
        try:
            o = geo.parse_visual(wii_body, level=3, binary=True, strict=True)
        except F.FormatError as exc:
            info["reason"] = "not parsed: %s" % exc
            return bytes(wii_body), info
        if o["type"] != 1:
            info["reason"] = "K3D_lod"
            return bytes(wii_body), info
        if not (o["flags"] & 0x80) or not o.get("vb") or not o.get("ib"):
            info["reason"] = "geometry not in a Wii vertex buffer"
            return bytes(wii_body), info
        p, info = pc_geometric(o, opts)
        body, _ = F.emit(geo.visual, p, level=3, binary=True)
    # the in-memory size LOA_p_LoadRef puts first grows by exactly what the record grows
    body = struct.pack("<I", len(body) + (o["memsize"] - len(wii_body))) + body[4:]
    return body, info


# ----------------------------------------------------------------------------------------------------------
# Wii side
class WiiMesh:
    """Decoded Wii vertex buffer and index streams of one K3D_geometric record."""

    def __init__(self, o):
        self.o = o
        vb = o["vb"]
        d = vb["data"]
        self.npts = o["n_points"]
        if self.npts * 12 > len(d):
            raise ConvertError("vertex buffer shorter than the points")
        self.points = np.frombuffer(d, "<f4", self.npts * 3, 0).reshape(self.npts, 3).astype(F32)
        self.uv16 = self._s16(d, vb["off_uv1"], o["n_uv"])
        self.uv216 = self._s16(d, vb["off_uv2"], o["n_uv2"])
        self.vb_normals = vb["off_nrm"] not in (NONE32, 0) and o["n_normals"] > 0
        ib = o["ib"]
        if ib.get("sorted"):
            raise ConvertError("sorted index buffer set")
        self.masks = [b["attribute_mask"] for b in ib["buffers"]]
        self.streams = []
        for b in ib["buffers"]:
            dt = {1: np.uint8, 2: "<u2", 4: "<u4"}[b["index_size"]]
            self.streams.append(np.frombuffer(b["data"], dt, b["index_number"]).astype(np.int64))
        self.nstrip = len(self.streams[0]) if self.streams else 0
        if any(len(s) != self.nstrip for s in self.streams):
            raise ConvertError("index streams of different lengths")
        self.tris = [np.frombuffer(e["tris"], "<u2").reshape(-1, 6).astype(np.int64) for e in o["elements"]]

    @staticmethod
    def _s16(d, off, n):
        if not n or off == NONE32 or off + n * 4 > len(d):
            return None
        return np.frombuffer(d, "<i2", n * 2, off).reshape(n, 2).astype(np.int64)

    def stream(self, bit):
        """index (into self.streams) of the stream carrying an attribute bit, or None"""
        for i, m in enumerate(self.masks):
            if m != NONE32 and m & bit:
                return i
        return None

    def strip_triangles(self):
        """(element, [a, b, c]) strip positions of every non-degenerate triangle, GX winding.  Index buffer sets of
        topology 0 hold plain triangle lists (element material = index count, no substrips)."""
        pos = self.streams[self.stream(1)]
        is_list = self.o["ib"]["topology"] == 0
        for ie, e in enumerate(self.o["ib"]["elements"]):
            if is_list:
                start = e["index_start"]
                for j in range(e["material"] // 3):
                    a, b, c = start + 3 * j, start + 3 * j + 1, start + 3 * j + 2
                    if pos[a] == pos[b] or pos[b] == pos[c] or pos[a] == pos[c]:
                        continue
                    yield ie, (a, b, c)
                continue
            for ss in e.get("substrips", []):
                start = e["index_start"] + ss["start"]
                for j in range(ss["count"] - 2):
                    a, b, c = start + j, start + j + 1, start + j + 2
                    if j & 1:
                        a, b = b, a
                    if pos[a] == pos[b] or pos[b] == pos[c] or pos[a] == pos[c]:
                        continue
                    yield ie, (a, b, c)


def _rot_min(cs):
    r = min(range(3), key=lambda r: (cs[r], cs[(r + 1) % 3], cs[(r + 2) % 3]))
    return (cs[r], cs[(r + 1) % 3], cs[(r + 2) % 3]), r


def _nearest_rep(values, present_idx):
    """rep[i] = i when index i occurs in the strips, else the strip index with the nearest value (the Wii binarizer
    welded positions within 1e-4 and UVs within 1e-3 before writing the streams)"""
    n = len(values)
    rep = np.arange(n, dtype=np.int64)
    pres = np.unique(present_idx[(present_idx >= 0) & (present_idx < n)])
    if not len(pres) or len(pres) == n:
        return rep
    mask = np.ones(n, bool)
    mask[pres] = False
    V = values.astype(np.float64)
    S = V[pres]
    for i in np.nonzero(mask)[0]:
        rep[i] = pres[int(np.argmin(np.abs(S - V[i]).max(1)))]
    return rep


def map_corners(m: WiiMesh):
    """cpos[k][t, c] = strip position holding corner c of triangle t of element k (-1 when none)"""
    pos_s = m.stream(1)
    if pos_s is None:
        raise ConvertError("no position index stream")
    pos = m.streams[pos_s]
    uv_s = m.stream(2) if (m.o["n_uv"] and m.uv16 is not None) else None
    prep = _nearest_rep(m.points, pos)
    urep = _nearest_rep(m.uv16, m.streams[uv_s]) if uv_s is not None else None
    npts = m.npts
    look = collections.defaultdict(list)
    for k, t in enumerate(m.tris):
        pc = np.clip(t[:, :3], 0, max(npts - 1, 0))
        P = prep[pc]
        if urep is not None:
            U = urep[np.clip(t[:, 3:], 0, len(urep) - 1)]
        for i in range(len(t)):
            key, r = _rot_min((int(P[i, 0]), int(P[i, 1]), int(P[i, 2])))
            uk = (int(U[i, r]), int(U[i, (r + 1) % 3]), int(U[i, (r + 2) % 3])) if urep is not None else ()
            look[key].append([k, i, r, uk, False])
    cpos = [np.full((len(t), 3), -1, np.int64) for t in m.tris]
    miss = 0
    uvs = m.streams[uv_s] if uv_s is not None else None
    for ie, idx in m.strip_triangles():
        Pv = (int(pos[idx[0]]), int(pos[idx[1]]), int(pos[idx[2]]))
        key, rs = _rot_min(Pv)
        cands = look.get(key)
        if not cands:
            miss += 1
            continue
        uk = (int(uvs[idx[rs]]), int(uvs[idx[(rs + 1) % 3]]), int(uvs[idx[(rs + 2) % 3]])) if uvs is not None else ()
        best = None
        for c in cands:
            if c[4]:
                continue
            if c[3] == uk:
                best = c
                break
            if best is None:
                best = c
        if best is None:
            miss += 1
            continue
        best[4] = True
        k, ti, rg = best[0], best[1], best[2]
        for q in range(3):
            cpos[k][ti, (rg + q) % 3] = idx[(rs + q) % 3]
    # corners of triangles the strips do not hold (degenerate after welding, duplicates): any strip corner with the
    # same welded point and UV
    first = {}
    for x in range(m.nstrip):
        first.setdefault((int(pos[x]), int(uvs[x]) if uvs is not None else 0), x)
    unassigned = 0
    for k, t in enumerate(m.tris):
        for i in np.nonzero((cpos[k] < 0).any(1))[0]:
            unassigned += 1
            for q in range(3):
                if cpos[k][i, q] >= 0:
                    continue
                p = int(prep[min(t[i, q], npts - 1)]) if npts else 0
                u = int(urep[min(t[i, 3 + q], len(urep) - 1)]) if urep is not None else 0
                cpos[k][i, q] = first.get((p, u), -1)
    return cpos, {"strip_miss": miss, "unassigned_triangles": unassigned}


def unify(m: WiiMesh, cpos):
    """PC unified vertices: one per distinct tuple of index-stream values, numbered by first appearance over the element
    triangles.  Returns (vertex id per strip position, first strip position and first corner of every vertex, extra
    count)."""
    S = np.stack(m.streams, 1) if m.streams else np.zeros((0, 1), np.int64)
    rows, ident = np.unique(S, axis=0, return_inverse=True)
    ident = ident.reshape(-1)
    flat = np.concatenate([c.reshape(-1) for c in cpos]) if cpos else np.zeros(0, np.int64)
    elem_of = np.concatenate([np.full(c.size, k, np.int64) for k, c in enumerate(cpos)]) if cpos else np.zeros(0, np.int64)
    valid = flat >= 0
    cid = np.full(len(flat), -1, np.int64)
    cid[valid] = ident[flat[valid]]
    vid_of_ident = np.full(len(rows), -1, np.int64)
    vi = np.nonzero(valid)[0]
    firsts = []
    if len(vi):
        _, fi = np.unique(cid[vi], return_index=True)
        fi = np.sort(fi)
        firsts = vi[fi]
        vid_of_ident[cid[firsts]] = np.arange(len(firsts))
    # strip positions whose tuple no triangle corner reached: keep them drawable, numbered after
    extra = np.nonzero(vid_of_ident < 0)[0]
    n_main = len(firsts)
    vid_of_ident[extra] = n_main + np.arange(len(extra))
    vid = vid_of_ident[ident] if len(ident) else np.zeros(0, np.int64)
    first_strip = np.empty(n_main + len(extra), np.int64)
    first_corner = np.full((n_main + len(extra), 3), -1, np.int64)       # element, triangle, corner
    if n_main:
        first_strip[:n_main] = flat[firsts]
        k = elem_of[firsts]
        offs = np.cumsum([0] + [c.size for c in cpos])
        local = firsts - offs[k]
        first_corner[:n_main, 0] = k
        first_corner[:n_main, 1] = local // 3
        first_corner[:n_main, 2] = local % 3
    if len(extra):
        # first strip position of each extra tuple
        pos_of = np.full(len(rows), -1, np.int64)
        order = np.argsort(ident, kind="stable")
        fpos = order[np.r_[0, np.nonzero(np.diff(ident[order]))[0] + 1]]
        pos_of[ident[fpos]] = fpos
        first_strip[n_main:] = pos_of[extra]
    return vid, first_strip, first_corner, len(extra)


# ----------------------------------------------------------------------------------------------------------
# normals (the binarizer's computation, FPU in single precision)
def _f(x):
    return np.asarray(x).astype(F32)


def _sqrnorm(v):
    s = _f(v[:, 0] * v[:, 0])
    s = _f(s + _f(v[:, 1] * v[:, 1]))
    return _f(s + _f(v[:, 2] * v[:, 2]))


def _dot(a, b):
    s = _f(a[:, 0] * b[:, 0])
    s = _f(s + _f(a[:, 1] * b[:, 1]))
    return _f(s + _f(a[:, 2] * b[:, 2]))


def normalize32(v):
    """MTH_Vec3NormalizeEqual: v * (1/sqrtf(|v|^2)), zero stays zero"""
    n = np.sqrt(_sqrnorm(v)).astype(F32)
    out = np.zeros_like(v, dtype=F32)
    nz = n != 0
    inv = _f(F32(1) / n[nz])
    out[nz] = _f(v[nz] * inv[:, None])
    return out


def _cross(a, b):
    c = np.empty_like(a, dtype=F32)
    c[:, 0] = _f(_f(a[:, 1] * b[:, 2]) - _f(a[:, 2] * b[:, 1]))
    c[:, 1] = _f(_f(a[:, 2] * b[:, 0]) - _f(a[:, 0] * b[:, 2]))
    c[:, 2] = _f(_f(a[:, 0] * b[:, 1]) - _f(a[:, 1] * b[:, 0]))
    return c


def _acos32(c):
    with np.errstate(invalid="ignore"):
        a = _f(np.arccos(np.clip(c.astype(np.float64), -1.0, 1.0)))
    a = np.where(c < -1, PI32, a)
    a = np.where(c > 1, F32(0), a)
    return _f(a)


def face_normals(P, T, mode="cross"):
    p0, p1, p2 = P[T[:, 0]], P[T[:, 1]], P[T[:, 2]]
    if mode == "engine":             # the PC executable's K3D_geometric::ComputeFaceNormals 0044B1C0
        a, b = _f(p0 - p1), _f(p0 - p2)
        c = np.empty_like(a)
        c[:, 0] = _f(_f(a[:, 1] * b[:, 2]) - _f(a[:, 2] * b[:, 1]))
        c[:, 1] = _f(_f(b[:, 0] * a[:, 2]) - _f(a[:, 0] * b[:, 2]))
        c[:, 2] = _f(_f(b[:, 1] * a[:, 0]) - _f(a[:, 1] * b[:, 0]))
        return normalize32(c)
    return normalize32(_cross(_f(p1 - p0), _f(p2 - p0)))


def vertex_normals(P, T, NT, n_out):
    """angle/pi weighted sum of normalised face normals per normal index, accumulated in triangle order"""
    out = np.zeros((n_out, 3), F32)
    if not len(T) or not n_out:
        return out
    p0, p1, p2 = P[T[:, 0]], P[T[:, 1]], P[T[:, 2]]
    e01, e02, e12 = _f(p1 - p0), _f(p2 - p0), _f(p2 - p1)
    l01 = np.sqrt(_sqrnorm(e01)).astype(F32)
    l02 = np.sqrt(_sqrnorm(e02)).astype(F32)
    l12 = np.sqrt(_sqrnorm(e12)).astype(F32)
    with np.errstate(divide="ignore", invalid="ignore"):
        c0 = _f(_dot(e01, e02) / _f(l01 * l02))
        c1 = _f(-_dot(e01, e12) / _f(l01 * l12))
    a0, a1 = _acos32(c0), _acos32(c1)
    a2 = _f(_f(PI32 - a0) - a1)
    w = np.stack([_f(a0 / PI32), _f(a1 / PI32), _f(a2 / PI32)], 1)
    w[(l01 == 0) | (l02 == 0) | (l12 == 0)] = F32(1)
    fn = normalize32(_cross(e01, e02))
    contrib = _f(fn[:, None, :] * w[:, :, None]).reshape(-1, 3)
    tgt = NT.reshape(-1)
    ok = (tgt >= 0) & (tgt < n_out)
    contrib, tgt = contrib[ok], tgt[ok]
    # sequential accumulation: the r-th contribution of every normal is added in round r
    order = np.argsort(tgt, kind="stable")
    st = tgt[order]
    grp_start = np.r_[0, np.nonzero(np.diff(st))[0] + 1] if len(st) else np.zeros(0, np.int64)
    rank = np.arange(len(st)) - np.repeat(grp_start, np.diff(np.r_[grp_start, len(st)]))
    for r in range(int(rank.max()) + 1 if len(rank) else 0):
        sel = order[rank == r]
        out[tgt[sel]] = _f(out[tgt[sel]] + contrib[sel])
    return normalize32(out)


# ----------------------------------------------------------------------------------------------------------
# packing
def f16_d3dx(x):
    """D3DXFloat32To16Array (round to nearest even; exponent 31 is written with its mantissa; beyond saturates)"""
    x = np.asarray(x, np.float32)
    shape = x.shape
    x = x.reshape(-1)
    out = np.zeros(len(x), np.uint32)
    sign = np.signbit(x).astype(np.uint32) << 15
    a = np.abs(x.astype(np.float64))
    fin = np.isfinite(a) & (a != 0)
    out[~np.isfinite(a)] = 0x7FFF
    if fin.any():
        af = a[fin]
        m, e = np.frexp(af)                    # af = m * 2**e, m in [0.5, 1)
        exp = e - 1 + 15
        tmp = np.ldexp(m, 11)                  # in [1024, 2048)
        mant = np.floor(tmp)
        frac = tmp - mant
        up = (frac > 0.5) | ((frac == 0.5) & (mant.astype(np.int64) % 2 == 1))
        mant = mant + up
        carry = mant == 2048
        mant[carry] = 1024
        exp = exp + carry
        res = np.zeros(len(af), np.uint32)
        big = exp > 31
        res[big] = 0x7FFF
        norm = (exp >= 1) & ~big
        res[norm] = (exp[norm].astype(np.uint32) << 10) | (mant[norm].astype(np.uint32) & 0x3FF)
        den = exp <= 0
        if den.any():
            sh = 1 - exp[den]
            v = np.floor(np.ldexp(mant[den], -sh.astype(np.int64)) + 0.5)
            res[den] = v.astype(np.uint32) & 0x7FFF
        out[fin] = res
    return ((out | sign) & 0xFFFF).astype("<u2").reshape(shape)


def pack_unit(v, mode="f32"):
    """UBYTE4N component of a unit vector: trunc((v*0.5+0.5)*255)"""
    if mode == "f64":
        x = (v.astype(np.float64) * 0.5 + 0.5) * 255.0
    else:
        x = _f(_f(_f(_f(v) * F32(0.5)) + F32(0.5)) * F32(255.0))
    return np.trunc(x).astype(np.int64) & 0xFF


# ----------------------------------------------------------------------------------------------------------
# PC record
def uv_floats(s16, mode):
    if s16 is None:
        return None
    w = s16.astype(np.float64)
    if mode == "trunc":
        v = w / 1024.0
    else:
        c = (w + 0.5 * np.sign(w)) / 1024.0
        if mode == "hybrid":
            c = np.where(np.mod(w, 64) == 0, w / 1024.0, c)
        v = c
    return v.astype(F32)


def pc_geometric(o, opts=None):
    opts = dict(DEFAULTS, **(opts or {}))
    m = WiiMesh(o)
    info = {"kind": "geometric"}
    cpos, st = map_corners(m)
    info.update(st)
    vid, first_strip, first_corner, n_extra = unify(m, cpos)
    info["extra_vertices"] = n_extra
    nv = len(first_strip)
    npts, nnrm, nuv, nuv2 = o["n_points"], o["n_normals"], o["n_uv"], o["n_uv2"]
    s_uv2 = m.stream(4) if nuv2 else None
    s20 = m.stream(0x20)
    T_all = np.concatenate([t[:, :3] for t in m.tris]) if m.tris else np.zeros((0, 3), np.int64)
    C_all = np.concatenate([c for c in cpos]) if cpos else np.zeros((0, 3), np.int64)

    # ---- normal index of every corner
    if nnrm == 0:
        NT = None
        info["normals"] = "none"
    elif nnrm == npts:
        NT = T_all
        info["normals"] = "per point"
    elif m.vb_normals and s20 is not None:
        vals = m.streams[s20]
        NT = np.where(C_all >= 0, vals[np.maximum(C_all, 0)], -1)
        info["normals"] = "skinned (Wii normal index stream)"
    else:
        NT, ngroups = _group_split_normals(m, T_all, C_all, s20, nnrm)
        info["normals"] = "split (palette groups %d/%d)" % (ngroups, nnrm)
        info["normal_groups"] = ngroups
    P = m.points
    normals = vertex_normals(P, T_all, NT, nnrm) if nnrm else np.zeros((0, 3), F32)

    # ---- light points: one per normal (owning point, normal)
    if nnrm:
        if o["n_lightpoints"] == nnrm and o.get("lightpoints"):
            lp = np.frombuffer(o["lightpoints"], "<u2").reshape(-1, 2).astype(np.int64)
        else:
            owner = np.arange(nnrm, dtype=np.int64) if nnrm == npts else np.zeros(nnrm, np.int64)
            if NT is not None and nnrm != npts:
                flatn, flatp = NT.reshape(-1), T_all.reshape(-1)
                okn = (flatn >= 0) & (flatn < nnrm)
                fo = np.full(nnrm, -1, np.int64)
                rev = np.nonzero(okn)[0][::-1]
                fo[flatn[rev]] = flatp[rev]
                owner = np.where(fo >= 0, fo, 0)
            lp = np.stack([owner, np.arange(nnrm)], 1)
    else:
        lp = np.zeros((0, 2), np.int64)

    # ---- per vertex source indices
    ind_point = np.zeros(nv, np.int64)
    ind_uv1 = np.zeros(nv, np.int64)
    ind_uv2 = np.zeros(nv, np.int64)
    ind_nrm = np.zeros(nv, np.int64)
    n_main = nv - n_extra
    pos_s = m.stream(1)
    uv_s = m.stream(2) if nuv else None
    tri_off = np.cumsum([0] + [len(tt) for tt in m.tris])
    for v in range(nv):
        x = first_strip[v]
        if v < n_main:
            k, t, c = first_corner[v]
            ind_point[v] = m.tris[k][t, c]
            ind_uv1[v] = m.tris[k][t, 3 + c] if nuv else 0
            if NT is not None:
                ind_nrm[v] = max(int(NT[tri_off[k] + t, c]), 0)
        else:
            ind_point[v] = m.streams[pos_s][x]
            ind_uv1[v] = m.streams[uv_s][x] if uv_s is not None else 0
            if nnrm == npts:
                ind_nrm[v] = ind_point[v]
            elif s20 is not None and m.vb_normals:
                ind_nrm[v] = m.streams[s20][x]
        if s_uv2 is not None and x >= 0:
            ind_uv2[v] = m.streams[s_uv2][x]
    if not nnrm:
        ind_nrm[:] = 0

    # ---- UVs
    uv = uv_floats(m.uv16, opts["uv"])
    uv2 = uv_floats(m.uv216, opts["uv"])
    if nuv and uv is None:
        uv = np.zeros((nuv, 2), F32)
    if nuv2 and uv2 is None:
        uv2 = np.zeros((nuv2, 2), F32)

    # ---- vertex buffer
    fmt = o["vfmt_key"]
    cols = []
    if fmt & 0x1:
        cols.append(P[np.clip(ind_point, 0, npts - 1)].astype("<f4").view(np.uint8).reshape(nv, 12))
    comp = bool(fmt & 0x800)
    for bit, arr, idx in ((0x2, uv, ind_uv1), (0x4, uv2, ind_uv2)):
        if fmt & bit:
            vals = arr[np.clip(idx, 0, len(arr) - 1)] if arr is not None and len(arr) else np.zeros((nv, 2), F32)
            if comp:
                cols.append(f16_d3dx(vals).view(np.uint8).reshape(nv, 4))
            else:
                cols.append(vals.astype("<f4").view(np.uint8).reshape(nv, 8))
    if fmt & 0x8:
        cols.append(np.zeros((nv, 4), np.uint8))
    if fmt & 0x10:
        nn = normals[np.clip(ind_nrm, 0, len(normals) - 1)] if len(normals) else np.zeros((nv, 3), F32)
        if comp:
            b = np.zeros((nv, 4), np.uint8)
            b[:, :3] = pack_unit(nn, opts["normal_pack"])
            cols.append(b)
        else:
            cols.append(nn.astype("<f4").view(np.uint8).reshape(nv, 12))
    if fmt & 0x20:
        cols.append(np.zeros((nv, 4), np.uint8))
    vb_data = np.concatenate(cols, 1).tobytes() if cols and nv else b""

    # ---- index buffer: the Wii strips on the unified vertices, stitched per element
    ib_out = []
    p_elems = []
    is_list = o["ib"]["topology"] == 0
    for e in o["ib"]["elements"]:
        eo, subs = [], []
        if is_list:
            eo = vid[e["index_start"]:e["index_start"] + e["material"]].tolist()
        for ss in ([] if is_list else e.get("substrips", [])):
            s0 = e["index_start"] + ss["start"]
            s = vid[s0:s0 + ss["count"]].tolist()
            if eo and s:
                eo += [eo[-1], s[0]]
                if len(eo) & 1:
                    eo.append(s[0])
            subs.append({"start": len(eo), "count": ss["count"]})
            eo += s
        pe = dict(e)
        pe["material"] = len(eo)               # stitched (strips) or plain (lists) index count, as the Wii record stores it
        pe["index_start"] = len(ib_out)
        pe["substrips"] = subs
        pe["n_substrips"] = len(subs)
        p_elems.append(pe)
        ib_out += eo
    if len(ib_out) and max(ib_out) > 0xFFFF:
        raise ConvertError("more than 65536 unified vertices")
    ib = {"header": o["ib"]["header"], "sorted": False, "topology": o["ib"]["topology"], "elements": p_elems,
          "buffers": [{"magic": 0xC0DE0001, "index_size": 2, "attribute_mask": NONE32, "vertex_number": nv,
                       "data": np.asarray(ib_out, "<u2").tobytes()}]}

    # ---- the record
    p = copy.copy(o)
    flags = o["flags"] & ~WII_ONLY_FLAGS
    if opts["flag_100"] == "clear":
        flags &= ~0x100
    p["flags"] = flags
    p["n_lightpoints"] = len(lp)
    p["points"] = P.astype("<f4").tobytes()
    p["normals"] = normals.astype("<f4").tobytes()
    p["lightpoints"] = lp.astype("<u2").tobytes()
    p["uv"] = uv.astype("<f4").tobytes() if nuv else b""
    p["uv2"] = uv2.astype("<f4").tobytes() if nuv2 else b""
    elems = []
    off = 0
    for k, e in enumerate(o["elements"]):
        pe = dict(e)
        nt = e["n_triangles"]
        if nuv2:
            # every element of a mesh with UV2 carries its per-triangle UV2 indices (flag 0x20); the Wii dropped them
            pe["flags"] = e["flags"] | 0x20
            if s_uv2 is not None:
                c = cpos[k]
                t2 = np.where(c >= 0, m.streams[s_uv2][np.maximum(c, 0)], 0)
            else:
                t2 = np.zeros((nt, 3), np.int64)
            pe["tri_uv2"] = t2.astype("<u2").tobytes()
        if (o["flags"] & 0x4000) and opts["tri_normals"] != "wii" and nt:
            pe["tri_normals"] = face_normals(P, m.tris[k][:, :3], opts["tri_normals"]).astype("<f4").tobytes()
        elems.append(pe)
        off += nt
    p["elements"] = elems
    p["mesh_format"] = 1
    p["vb"] = {"magic": 0xC0DE0002, "off_uv1": 0, "off_uv2": 0, "off_nrm": 0, "off_col": 0, "data": vb_data}
    p["ib"] = ib
    p["ind_point"] = ind_point.astype("<u2").tobytes()
    p["ind_color"] = ind_nrm.astype("<u2").tobytes()
    p["ind_normal"] = ind_nrm.astype("<u2").tobytes()
    p["ind_uv1"] = ind_uv1.astype("<u2").tobytes()
    p["ind_uv2"] = ind_uv2.astype("<u2").tobytes()
    info["vertices"] = nv
    return p, info


def _group_split_normals(m, T_all, C_all, s20, n_target=None):
    """Static meshes with more normals than points keep no normal indices on the Wii.  Corners of a point that received
    different palette normals (mask 0x20 stream: a 256-entry direction palette) had different normals; normals the
    palette cannot tell apart (same point, same palette entry: 1 split mesh in 3) are recovered by splitting the groups
    with the widest spread of face normals (2-means) until the Wii normal count is reached.  Normals are numbered by
    point, then by first appearance over the element triangles."""
    npts = m.npts
    if s20 is None:
        return T_all.copy(), npts
    pal = np.where(C_all >= 0, m.streams[s20][np.maximum(C_all, 0)], -1)
    flatp, flatq = T_all.reshape(-1), pal.reshape(-1)
    members = collections.OrderedDict()           # (point, palette, sub) -> corner flat indices, first-appearance order
    for i, (p, q) in enumerate(zip(flatp.tolist(), flatq.tolist())):
        if q < 0:
            continue
        members.setdefault((p, q, 0), []).append(i)
    if n_target and len(members) < n_target:
        fn = face_normals(m.points, T_all, "cross").astype(np.float64)
        split_id = collections.Counter()

        def spread(cs):
            if len(cs) < 2:
                return -1.0
            v = fn[np.asarray(cs) // 3]
            return float(1.0 - (v @ v.T).min())

        import heapq
        heap = [(-spread(cs), k) for k, cs in members.items()]
        heapq.heapify(heap)
        while len(members) < n_target and heap:
            s, k = heapq.heappop(heap)
            if -s <= 1e-6 or k not in members:
                break
            cs = members[k]
            v = fn[np.asarray(cs) // 3]
            d = v @ v.T
            a, b = np.unravel_index(np.argmin(d), d.shape)
            ca, cb = v[a].copy(), v[b].copy()
            for _ in range(8):
                lab = (v @ cb) > (v @ ca)
                if lab.all() or not lab.any():
                    break
                ca = v[~lab].sum(0)
                cb = v[lab].sum(0)
            if lab.all() or not lab.any():
                continue
            ga = [c for c, l in zip(cs, lab) if not l]
            gb = [c for c, l in zip(cs, lab) if l]
            split_id[(k[0], k[1])] += 1
            kb = (k[0], k[1], split_id[(k[0], k[1])])
            members[k] = ga
            members[kb] = gb
            heapq.heappush(heap, (-spread(ga), k))
            heapq.heappush(heap, (-spread(gb), kb))
    order = sorted(members.items(), key=lambda kv: (kv[0][0], kv[1][0]))
    NT = np.full(len(flatp), -1, np.int64)
    first_of_point = {}
    for n, (k, cs) in enumerate(order):
        NT[cs] = n
        first_of_point.setdefault(k[0], n)
    miss = np.nonzero(NT < 0)[0]
    for i in miss:
        NT[i] = first_of_point.get(int(flatp[i]), -1)
    return NT.reshape(T_all.shape), len(order)
