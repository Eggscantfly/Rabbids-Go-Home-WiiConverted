"""The record kinds of a whole Wii archive, generated from the archive and kept in the cache folder.

    kinds = load_kinds(big, rec_index, path, log)          # key -> kind

build_kinds walks every world list of the archive (each .wol, in name order), then every world group that names world
packages itself (the groups loading worlds no list holds: startup, map, language, global and nand groups), in bigfile
order, and then tags the script records (script_tags).  classify_untyped types, when a package is built, the records
no walk reached.
"""
from __future__ import annotations

import json
import os
import struct

from ..archive.index import fingerprint
from ..archive.packages import Archive, Package
from ..formats import geometry as G
from ..formats import script as SC
from ..formats import stream as F
from ..formats import vif as V
from .walker import Walker, resolve

FORMAT = 1
MODEL_MIN_SIZE = 280


# ============================================================================ the walk of the whole archive
def global_groups(big) -> list[str]:
    """The world groups (.wog) that name world packages themselves (not only through their lists and nested groups),
    in bigfile order."""
    out = []
    for g in big.entries():
        if g.ext != "wog":
            continue
        for r in big.refs(g):
            t = big.find(r.key)
            if t is not None and t.ext in ("wog", "wol"):
                continue
            if big.find(0xFFF00000 | (r.key & 0xFFFFF)) is not None:
                out.append(g.name)
                break
    return out


def build_kinds(big, rec_index: dict[int, int], log=None) -> dict:
    """{"kinds": {key: kind}, "lists": {list or group name: world keys}, "problems": {message: count}}."""
    arc = Archive(big)
    w = Walker(arc, rec_index)
    lists = {}
    walked = set()
    names = sorted({e.name[:-4] for e in big.entries() if e.ext == "wol"})
    for name in names:
        try:
            wol = big.find_name(name + ".wol")
            lists[name] = ["%08X" % r.key for r in big.refs(wol)] if wol else []
            w.add_list(name)
            walked.add(name)
        except Exception as ex:          # noqa: BLE001
            if log:
                log("world list %s: %s" % (name, ex))
    for name in global_groups(big):
        try:
            lists[name] = ["%08X" % k for k in w.add_group(name, walked)]
        except Exception as ex:          # noqa: BLE001
            if log:
                log("world group %s: %s" % (name, ex))
    tagged = script_tags(big, arc, rec_index, w)
    if log:
        log("kinds: %d keys from %d lists and groups, %d script records" % (len(w.kinds), len(lists), tagged))
    return {"kinds": w.kinds, "lists": lists, "problems": dict(w.stats.most_common(200))}


def load_kinds(big, rec_index: dict[int, int], path: str, log=None) -> dict[int, str]:
    """The kinds of `big` from the JSON file at `path`; built and written there when missing or built from another
    bigfile."""
    fp = fingerprint(big)
    if os.path.exists(path):
        try:
            with open(path) as f:
                doc = json.load(f)
            if doc.get("format") == FORMAT and doc.get("source") == fp:
                return {int(k, 16): v for k, v in doc["kinds"].items()}
        except (OSError, ValueError, KeyError):
            pass
    if log:
        log("walking the world lists of %s" % os.path.basename(big.path))
    res = build_kinds(big, rec_index, log)
    os.makedirs(os.path.dirname(os.path.abspath(path)), exist_ok=True)
    tmp = path + ".tmp"
    with open(tmp, "w") as f:
        json.dump({"format": FORMAT, "source": fp, "kinds": {"%08X" % k: v for k, v in res["kinds"].items()},
                   "lists": res["lists"], "problems": res["problems"]}, f)
    os.replace(tmp, path)
    return res["kinds"]


# ============================================================================ script records
def script_models(big, rec_index: dict[int, int], w: Walker) -> dict[int, dict]:
    """{model key: parsed model} of every script model record: the package records of at least MODEL_MIN_SIZE bytes that
    carry b"ModelFactory" and parse exactly as a model, the models the walked script instances name that parse as models,
    and the loose model files (.mdl, file level) of the archive."""
    found: dict[int, dict] = {}
    for e in big.entries():
        if e.key >> 20 != 0xFFF:
            continue
        p = Package(big.read(e), e.key)
        for o, k, ln in p.records:
            if k in found or ln < MODEL_MIN_SIZE:
                continue
            body = p.d[o + 8:o + 8 + ln]
            if b"ModelFactory" not in body:
                continue
            try:
                found[k] = F.parse(SC.model, body)
            except Exception:          # noqa: BLE001
                continue
    for k, kind in list(w.kinds.items()):
        if kind != "res:script_model" or k in found:
            continue
        b = w.body(k)
        if b is None:
            continue
        try:
            found[k] = F.parse(SC.model, b)
        except Exception:              # noqa: BLE001
            continue
    for e in big.entries():
        if e.ext == "mdl" and e.key not in found and e.key >> 20 != 0xFFF:
            try:
                refs = [F.Ref(r.key, r.priv, r.user) for r in big.refs(e)]
                found[e.key] = F.parse(SC.model, big.read(e), level=0, binary=False, refs=refs)
            except Exception:          # noqa: BLE001
                continue
    return found


def script_tags(big, arc: Archive, rec_index: dict[int, int], w: Walker) -> int:
    """Tag the script records of every script model: the model ("script:key"), its source (model key - 1, when the model
    references it: "script:source_key"), its init tracks, then per reference kind (tracks, procedure lists, rule lists,
    data sets) each record ("script:key") and its source (key - 1, "script:source_key"), then the model's other
    references (not the sources of the records above, not the father).  Keys the walk already typed keep their kind.
    Returns the number of keys tagged here.

    Hook for the script phase: the script records tagged here are the ones the package builder takes from the PC
    release (when the PC archive holds the key) or hands to rghport.scripts."""
    before = sum(1 for v in w.kinds.values() if v.startswith("script:"))
    models = script_models(big, rec_index, w)
    order: list[tuple[int, str]] = []
    for k in sorted(models):
        o = models[k]
        refs = o.get("refs") or []
        order.append((k, "key"))
        if any(rf.key == k - 1 and rf.user == 0 for rf in refs):
            order.append((k - 1, "source_key"))
        for tk in o.get("init") or []:
            if tk:
                order.append((tk, "key"))
        for user in SC.REF_USERS:
            for rf in refs:
                if rf.user == user:
                    order.append((rf.key, "key"))
                    order.append((rf.key - 1, "source_key"))
        known = {rf.key for rf in refs if rf.user in SC.REF_USERS}
        for rf in refs:
            if rf.user in SC.REF_USERS:
                continue
            if rf.user == 0 and (rf.key + 1 in known or rf.key == k - 1):
                continue
            if rf.key == o.get("father"):
                continue
            order.append((rf.key, "key"))
    for key, field in order:
        w.tag(key & 0xFFFFFFFF, "script:" + field)
    return sum(1 for v in w.kinds.values() if v.startswith("script:")) - before


# ============================================================================ records no walk reaches
def classify_untyped(body: bytes) -> str:
    """A kind for a record no walk reaches: empty placeholders, texture header records (C0DEC0DE, versions 8..13), JPEG
    data, font glyph tables (the spec:fod family), Magma fonts and Wii material template records are
    platform-independent and pass through; post-effect shaders named *_Wii pass through ("shader"); records whose stream
    parses exactly as a visual, material, VIS, VIF or object get that kind; anything else stays '?'."""
    if len(body) == 0:
        return "empty"
    if body[4:8] == b"\xde\xc0\xde\xc0" and len(body) >= 12 and 8 <= struct.unpack_from("<I", body, 8)[0] <= 13:
        return "texture"
    if body[4:6] == b"\xff\xd8":
        return "jpeg"
    if (len(body) >= 16 and body[4:6] == b"\xde\xc0" and 1 <= struct.unpack_from("<H", body, 6)[0] <= 3
            and struct.unpack_from("<I", body, 0)[0] == len(body) - 4):
        # u32 size, u16 C0DE, u16 version 1..3, f32 x 2: the spec:fod family (v1 records are byte-identical in both
        # archives, e.g. A60013F3); the Magma font atlases' texdata are v3 in both (Wii 251067AF, PC DB00E3F9), written
        # little-endian on the Wii too
        return "spec:fod"
    if body[4:14] == b"Magma Font":
        return "magma_font"
    if b"K3D_material_Wii" in body[:64]:
        return "atomic"
    if b"CTAB" in body[:128] and b"_Wii" in body[:64]:
        # post-effect shaders named *_Wii (D3D9 HLSL bytecode vs_1_1 / ps_2_0, referenced by spec:afx records): the PC
        # archive ships ColorCorrection_Wii 47004D1D, BigBlur_Wii 470051BE and Remanence_Wii 470051DA byte for byte, so
        # DepthBlur_Wii 470051C1 (no PC twin) is loadable as it is
        return "shader"
    # records no walk reached whose stream parses exactly: a world may carry a skinned character's mesh and materials
    # untyped, and Wii mesh bytes passed through desynchronise the PC binary loader
    for kind, fn in _probes():
        try:
            F.parse(fn, body, strict=True)
            return kind
        except Exception:          # noqa: BLE001 - not this stream
            continue
    return "?"


def _probes():
    return (("visual", G.visual), ("material", F.material), ("atomic", F.material_atomic),
            ("vis", F.vis), ("vif", V.vif), ("object", F.obj))


__all__ = ["build_kinds", "load_kinds", "global_groups", "script_tags", "classify_untyped", "resolve"]
