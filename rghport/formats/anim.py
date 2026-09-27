"""Animation resources: skeletons, skins, actions, morph kits, saved positions, animations, event lists and the EVE
track lists they all end in.

    SKL   type  6   skl()   -> skin model   skn()
    ACT   type 15   aci()   -> action kit   ack()   -> action  act()
                            -> morph kit    mpk()
    SP    type 37   sp()
    ANI   type 14   ani()
    EVE   type 13   eve()
    track lists     trl()

Every `refs_<name>(o)` returns [(key, kind)]: skl -> ("skn"); skn -> bone objects ("object"); aci -> ("ack"),
("mpk"); ack -> ("act") x its non-empty slots; act -> ("trl") per item; sp -> ("skl"), ("object"); ani -> ("trl") x 6;
eve -> ("trl"), actor objects ("object"); mpk -> none; trl -> sound events ("soundfile"), animation events ("trl"),
dialog tracks ("material"), AFX events ("hlsl"), and the actors of its object tables and of the script events'
0x00190000 pushes ("object").  "object" keys of eve / sp / trl are resolved by the engine after loading, not through
LOA_MakeFileRef.

Loaders (RGH Wii executable)
----------------------------
SKL::p_Callback_LoadSKL 8015683C
SKL::p_Callback_LoadSkn 80187560 (+ pu32_LoadMorphDataSkn 801879C4)
ACT::p_Callback_LoadACI 800CF894
ACT::p_Callback_LoadAck 80119A98
ACT::p_Callback_LoadACT 80119A20, pu32_LoadACT 80119B60, pu32_LoadItemACT 80119800 (the item transitions inline)
ACT::p_Callback_LoadMpk 800CFE74 -> ACT_MorphKit::Load 802090A0, ACT_MorphChannel / Target (+ e_LoadType) / Slot /
    Vis / Expression / ExpressionNode / ExpressionTarget ::Load
SP::p_Callback_LoadSP 8011A328
ANI::p_Callback_LoadANI 800E6B04
EVE::p_Callback_LoadEVE 800FD89C
EVE::p_Callback_LoadTrl 8021CDCC
EVE::LoadTrl 8021C204, pu32_LoadEventTrl 8021BD90 (type table 8055E600), pu32_LoadBaseEventTrl 8020F5B4,
    InterpEvent_pu32_Load 802146F0, pt_LoadControlPoints 8020F008,
    Translation / Rotation / Scale / Morphing / Script / Value / Sound / Comment / Camera (+ CameraInterp, CameraOrder,
    CreateOrder, CAMOrder_OrientTo / Rumble / Shake::Load) / Action / AnimSnapDeprecated / AnimSound / AnimSnap /
    Animation / Snap / Dialog (+ DialogTrack) / AFX (-> world.hlsl_instance) EventTrl loaders,
    pu32_LoadSignalTrack 8021D0D8 (+ LoadListSignalTracks, LoadSignal, LoadCallback, LoadTrigger),
    SCR::LoadTrigger 8011ED78 (the signal inline), ViD::LoadStock 800ACE6C, MTH_CurveLoadBuffer 8008A004

What RGH ships (every world)
----------------------------
SKL 1135 records (version 3), ACT 1265 (version 1), ANI 38 (version 3), EVE 304 (version 3), SP none; through their
keys skin models 642 (119 unique, all version 9 with an Edge skeleton), kits 964 (360), actions 9998 (2288, all version
word 3), morph kits 128 (7 unique; versions 2 and 1), track lists 8433 (2290; versions 0x19, 0x18, 0x17, 0xF, 0xE,
0xB).  Every record parses and re-emits identically at level 3.  Exercised: compact T/R/S tracks (76k) and compressed
storages 0x4000000 / 0x2000000 / 0x8000000, 27930 interpolation blocks, 138 event curves, camera interpolations (v2)
and orders 1-3, 267 signal tracks (no callbacks / triggers), 77 dialog tracks with a script trigger, 278 cinematic
object tables, 4025 Edge animation blobs, action events v3, morph kits v1 / v2.  Not in the data: SP, signal callbacks
/ triggers, action versions 0 / 4, camera interpolation v1 parents.

Edge data: 4025 track lists end in an EdgeAnimAnimation 'EA03' blob (every bone's keyframes; the EVE tracks of those
lists carry only the root and events), and every skin model carries an 'ES01' skeleton, big-endian (see edge.py).
"""
from __future__ import annotations

import struct

from . import stream as F
from . import world as MW
from .stream import FormatError

SKL_TYPE, EVE_TYPE, ANI_TYPE, ACT_TYPE, SP_TYPE = 6, 13, 14, 15, 37
KIT_SLOTS = 0x200


# ----------------------------------------------------------------------------
# helpers
def _count(st, o, name, value, fn="u32", mode=2):
    """A count or presence word: derived from the content when writing."""
    if st.writing:
        o[name] = value
    return getattr(st, fn)(o, name, mode)


def _list(st, o, name, count_name, fn="u32", mode=2):
    """The list o[name] streamed with its count o[count_name]."""
    lst = o.setdefault(name, [])
    n = _count(st, o, count_name, len(lst), fn, mode)
    if not st.writing:
        lst[:] = [{} for _ in range(n)]
    return lst


def _fixed(st, o, name, n):
    """A list whose length was streamed earlier."""
    lst = o.setdefault(name, [])
    if st.writing:
        if len(lst) != n:
            raise FormatError(f"{name}: {len(lst)} entries, {n} expected")
    else:
        lst[:] = [{} for _ in range(n)]
    return lst


def _words(st, o, name, count_name):
    """u32 count, then that many u32."""
    n = _count(st, o, count_name, len(o.get(name) or []))
    return st.array(o, name, "<I", n)


def _blob(st, o, name, size_name, mode=2):
    """u32 byte count (always present), then the bytes at `mode`."""
    n = _count(st, o, size_name, len(o.get(name) or b""))
    return st.raw(o, name, n, mode)


def _put(st, fn, value):
    """A marker the writer emits in front of an extended header."""
    getattr(st, fn)({"v": value}, "v")


def _tag(pairs):
    return [(k, kind) for k, kind in pairs if k]


# ----------------------------------------------------------------------------
# MTH_CurveLoadBuffer inline (RGH 8008A004): no memsize word
def curve_inline(st, c):
    ver = st.u32(c, "version")
    st.f32(c, "minx")
    st.f32(c, "miny")
    st.f32(c, "maxx")
    st.f32(c, "maxy")
    n = _count(st, c, "n_samples", len(c.get("samples") or []))
    ctype = st.u32(c, "type") if ver > 2 else c.setdefault("type", 0)
    if ver != 0:
        st.f32(c, "m1_f0", 1)
        st.f32(c, "m1_f1", 1)
    st.array(c, "samples", "<f", n)
    if ver > 3 and n and ctype & 2:
        st.array(c, "segment_modes", "<I", n // 5)      # mulhwu 0xCCCCCCCD >> 2: one word per 5 samples
    return c


# ============================================================================
# SKL (type 6)
# ============================================================================
def skl(st, o):
    st.memsize(o)
    F.mdf_header(st, o)
    ver = o["version"]
    if ver > 3:
        raise FormatError(f"SKL version {ver}")
    st.u32(o, "skn")                          # -> skin model, 0 = none
    if ver > 1:
        st.box(o, "box")                      # MTH_pu8_LoadBoxFromBuffer
    if ver > 2:
        st.u32(o, "lod")                      # SKL::mt_LOD
        _words(st, o, "lod_values", "n_lod_values")
    return o


def refs_skl(o):
    return _tag([(o.get("skn", 0), "skn")])


# ============================================================================
# skin model: SKL::p_Callback_LoadSkn
# ============================================================================
def skn(st, o):
    bones = st.refs(o, "bones")               # LOA_p_LoadRef(b_Refs=1): bone objects, UserFlags low word = channel
    nb = len(bones)
    if not nb:
        return o                              # no second LOA_p_LoadRef without bones
    st.memsize(o)
    ver = st.u32(o, "version")
    if ver > 9:
        raise FormatError(f"skin model version {ver}")
    for i, b in enumerate(_list(st, o, "bone_data", "n_bone_data")):
        if i >= nb:                           # entries past the bone list: two words, dropped
            st.u16(b, "x0")
            st.u16(b, "x1")
            continue
        if ver >= 2:
            st.u16(b, "flags")                # SKL_tt_BoneModel::u16_Flags (forced to 1 for version <= 4)
        st.u16(b, "symmetric")                # u16_Symmetric
        if ver >= 3:
            st.u16(b, "lod")                  # u16_LOD
            if ver == 3:
                st.u16(b, "v3_u16")
        if ver >= 6:
            st.v3(b, "m1_v", 1)
        if ver >= 9:
            st.f32(b, "m1_f", 1)
    if ver >= 4:                              # SKL::pu32_LoadMorphDataSkn
        m = o.setdefault("morph", {})
        if st.writing and m.get("sets"):
            m["present"] = m.get("present") or 1
        if st.u32(m, "present"):
            for s in _list(st, m, "sets", "n_sets"):
                for e in _list(st, s, "entries", "n_entries"):
                    st.u16(e, "index")
                    fl = st.u16(e, "flags")
                    if fl & 1:
                        st.v3(e, "offset")
                    if fl & 2:
                        st.m44(e, "matrix")
                st.name64(s, "m3_name", 3)
    if ver >= 7:                              # SKL_tt_OptimPreset list (loose files only)
        for p in _list(st, o, "optim_presets", "n_optim_presets"):
            st.raw(p, "m1_name", 64, 1)
            st.u32(p, "m1_u0", 1)
            for e in _list(st, p, "entries", "n_entries"):
                st.u32(e, "m1_a", 1)
                st.u32(e, "m1_b", 1)
                st.f32(e, "m1_f", 1)
    if ver >= 8:                              # the Edge skeleton (EdgeAnimSkeleton)
        _blob(st, o, "edge_skeleton", "edge_skeleton_size")
    return o


def refs_skn(o):
    return _tag([(r.key, "object") for r in o.get("bones", [])])


# ============================================================================
# ACT (type 15): the action instance
# ============================================================================
def aci(st, o):
    st.memsize(o)
    F.mdf_header(st, o)
    ver = o["version"]
    if ver > 1:
        raise FormatError(f"ACT version {ver}")
    st.u32(o, "kit")                          # -> ACT::p_Callback_LoadAck
    if ver >= 1:
        st.u32(o, "morph_kit")                # -> ACT::p_Callback_LoadMpk (ACT_MorphKit)
    st.u32(o, "default_skl")                  # ACT::mu32_DefaultSkl
    return o


def refs_aci(o):
    return _tag([(o.get("kit", 0), "ack"), (o.get("morph_kit", 0), "mpk")])


def ack(st, o):
    """ACT::p_Callback_LoadAck: only a reference list, one slot per action id."""
    refs = st.refs(o, "actions")
    if len(refs) != KIT_SLOTS:
        raise FormatError(f"action kit with {len(refs)} slots")
    return o


def refs_ack(o):
    return _tag([(r.key, "act") for r in o.get("actions", [])])


def act(st, o):
    """ACT::p_Callback_LoadACT -> ACT::pu32_LoadACT -> pu32_LoadItemACT."""
    st.memsize(o)
    items = o.setdefault("items", [])
    if st.writing:
        o["n_items"] = len(items)
        if o.get("header", True):
            _put(st, "u8", 0xC0)
            _put(st, "u8", 0xDE)
            st.u32(o, "version_word")
        st.u8(o, "loop_item")
        st.u8(o, "n_items")
    else:
        a = st.u8(o, "loop_item")
        b = st.u8(o, "n_items")
        o["header"] = (a, b) == (0xC0, 0xDE)
        if o["header"]:
            st.u32(o, "version_word")
            st.u8(o, "loop_item")
            st.u8(o, "n_items")
        else:
            o["version_word"] = 0
    ver = o["version"] = o["version_word"] & 0xFF if o.get("header", True) else 0
    if ver > 3:
        raise FormatError(f"action version {ver}")
    if ver == 0:
        st.u16(o, "v0_u16")
    if ver >= 2:
        st.u8(o, "flags")                     # ACT_tt_Action::u8_Flags (bit 4: face layer)
    if ver >= 3:
        st.u32(o, "m1_u0", 1)
    for it in _fixed(st, o, "items", o["n_items"]):
        st.u32(it, "trl")                     # -> EVE::p_Callback_LoadTrl
        for tr in _list(st, it, "transitions", "n_transitions", "u16"):
            st.u16(tr, "dest_action")
            st.u16(tr, "trans_action")
            st.u8(tr, "blend")
            st.u8(tr, "flags")
            if ver == 0:
                st.u16(tr, "v0_u16")
        st.u16(it, "custom_flags")
        st.u8(it, "repetition")
        st.u8(it, "default_out")
        if ver == 0:
            st.u8(it, "freq_mul_u8")
            st.u8(it, "flags")
        else:
            st.u8(it, "flags")
            st.f32(it, "freq_mul")
    return o


def refs_act(o):
    return _tag([(it.get("trl", 0), "trl") for it in o.get("items", [])])


def _flag_curve(st, o, flag_name, fn):
    """A presence word (u8 / u32) and, when set, an inline MTH curve."""
    if st.writing and o.get("curve"):
        o[flag_name] = o.get(flag_name) or 1
    if getattr(st, fn)(o, flag_name):
        curve_inline(st, o.setdefault("curve", {}))


def mpk(st, o):
    """ACT::p_Callback_LoadMpk -> ACT_MorphKit::Load (the ACT's morph kit)."""
    st.memsize(o)
    ver = st.u16(o, "version")
    chans = o.setdefault("channels", [])
    n = _count(st, o, "n_channels", len(chans), "u8")
    for c in _fixed(st, o, "channels", n):                  # ACT_MorphChannel
        st.u8(c, "slot")
        _words(st, c, "targets", "n_targets")
        st.raw(c, "m3_raw", 24, 3)
    for tg in _list(st, o, "targets", "n_targets"):         # ACT_MorphTarget
        st.u32(tg, "type")                                   # e_LoadType
        st.u32(tg, "index")
        st.name64(tg, "m3_name", 3)
    for sl in _list(st, o, "slots", "n_slots"):             # ACT_MorphSlot
        st.u16(sl, "channel")
        m = _count(st, sl, "n_vis", len(sl.get("vis") or []))
        st.array(sl, "vis", "<H", m)                         # ACT_MorphVis: VIS modifier ids
    for vt in _list(st, o, "vertex_targets", "n_vertex_targets"):
        st.u32(vt, "slot")
        st.u32(vt, "vis")
        st.u32(vt, "target")
    exprs = o.setdefault("expressions", [])
    n = _count(st, o, "n_expressions", len(exprs))
    for e in _fixed(st, o, "expressions", n):
        if st.u32(e, "kind") not in (0, 1, 2):               # 0 node, 1 folder, 2 target
            raise FormatError(f"morph expression kind {e['kind']}")
    for e in exprs:                                          # virtual ::Load, then the node's id
        st.name64(e, "m3_name", 3)                           # ACT_MorphExpression::Load
        if ver >= 1:
            st.u32(e, "m1_u0", 1)
        if e["kind"] in (0, 1):                              # ACT_MorphExpressionNode::Load
            if ver <= 1:
                _flag_curve(st, e, "has_curve", "u8")
            for ch in _list(st, e, "children", "n_children"):
                st.u32(ch, "expr")
                if ver >= 2:
                    _flag_curve(st, ch, "has_curve", "u8")
        else:                                                # ACT_MorphExpressionTarget::Load
            st.u32(e, "target")
            st.u32(e, "invalid")
            if ver <= 1:
                _flag_curve(st, e, "has_curve", "u32")
        if e["kind"] == 0:
            st.u32(e, "id")                                  # < 0x200: the expression table slot
    return o


def refs_mpk(o):
    return []


# ============================================================================
# SP (type 37): saved positions
# ============================================================================
def sp(st, o):
    st.memsize(o)
    F.mdf_header(st, o)
    ver = o["version"]
    if ver == 0:
        sver = st.u32(o, "sp_version")
    else:
        sver = o["sp_version"] = ver
    if sver >= 2:
        st.u32(o, "skl")                      # SP::mh_SKL (not passed to LOA_MakeFileRef)
    _words(st, o, "objects", "n_objects")     # SP::mppo_Obj
    for sl in _list(st, o, "save_lists", "n_save_lists"):
        if sver != 0:
            st.raw(sl, "name", 64)
        for e in _list(st, sl, "entries", "n_entries"):
            st.u32(e, "u0")
            st.trm(e, "matrix")
    return o


def refs_sp(o):
    return _tag([(o.get("skl", 0), "skl")] + [(k, "object") for k in o.get("objects", [])])


# ============================================================================
# ANI (type 14)
# ============================================================================
def ani(st, o):
    st.memsize(o)
    F.mdf_header(st, o)
    ver = o["version"]
    if ver > 3:
        raise FormatError(f"ANI version {ver}")
    st.array(o, "trl", "<I", 3)               # -> EVE::p_Callback_LoadTrl each, 0 = none
    if ver >= 3:
        st.array(o, "trl2", "<I", 3)
    st.u32(o, "default_skl")                  # ANI::mu32_DefaultSkl
    if ver >= 2:
        st.f32(o, "swing")
        for k in range(1, 5):
            st.f32(o, f"dummy_f{k}")
        for k in range(1, 5):
            st.u32(o, f"dummy_u{k}")
    return o


def refs_ani(o):
    return _tag([(k, "trl") for k in list(o.get("trl", [])) + list(o.get("trl2", []))])


# ============================================================================
# EVE (type 13)
# ============================================================================
def eve(st, o):
    st.memsize(o)
    F.mdf_header(st, o)
    ver = o["version"]
    if ver > 3:
        raise FormatError(f"EVE version {ver}")
    st.u32(o, "trl")                          # -> EVE::p_Callback_LoadTrl
    for lb in _list(st, o, "labels", "n_labels"):
        st.f32(lb, "time")
        st.u32(lb, "value")
        st.raw(lb, "m1_name", 32, 1)
        st.u32(lb, "m1_u", 1)
    if ver >= 3:
        _words(st, o, "objects", "n_objects")     # actor object keys -> EVE_tt_ObjArray
    if ver > 1:
        _words(st, o, "snapshots", "n_snapshots")  # EVE::mu32_NumSnapshot words
    return o


def refs_eve(o):
    return _tag([(o.get("trl", 0), "trl")] + [(k, "object") for k in o.get("objects", [])])


# ============================================================================
# track lists: EVE::p_Callback_LoadTrl -> EVE::LoadTrl
# ============================================================================
def _loop_point(st, L, t, i):
    """The last event of a looping list (flags 0x8): the T/R/S and AnimSound loaders read its base event only (from list
    version 0xD)."""
    return bool(L["flags"] & 8) and i == t["n_events"] - 1 and L["version"] >= 0xD


def _compact(st, t):
    return bool(t["param2"] & 0x80000000)


def _base_event(st, L, t, i, e):
    """EVE::pu32_LoadBaseEventTrl: the flags (once per track when tflags & 0x4000), then the frame delta (u8 when the
    flags have 0x1000; always u8 in compact tracks, param2 & 0x80000000)."""
    if not (t["flags"] & 0x4000) or i == 0:
        fl = st.u16(e, "flags")
    else:
        fl = t["events"][0]["flags"]
        if not st.writing:
            e["flags"] = fl
    if _compact(st, t) or fl & 0x1000:
        st.u8(e, "delta")
    else:
        st.u16(e, "delta")
    return fl


def _ctrl_points(st, o):
    """EVE::pt_LoadControlPoints: u32 count, count x v3, (count - 2) / 3 time keys."""
    n = _count(st, o, "n_points", len(o.get("points") or b"") // 12)
    st.raw(o, "points", 12 * n)
    st.array(o, "time_keys", "<f", (n - 2) // 3 if n > 2 else 0)


def _interp_block(st, L, t, fl, e):
    """EVE::InterpEvent_pu32_Load after the base event (T/R/S tracks)."""
    ver = L["version"]
    if ver <= 6:
        raise FormatError(f"track list version {ver}: inline pre-7 interpolation data not handled")
    if fl & 0x80A0:
        ib = e.setdefault("interp", {})
        st.u32(ib, "u0")
        if fl & 0x20:
            if ver < 9:
                raise FormatError(f"track list version {ver}: pre-9 control points not handled")
            _ctrl_points(st, ib)
        if fl & 0x80:
            typ = t["type"]
            if ver < 8:
                st.raw(ib, "value", 16)
            elif typ == 2:
                st.v3(ib, "value")
            elif typ == 3:
                st.quat(ib, "value")
            elif typ == 4:
                st.m33(ib, "value")
        if fl & 0x8000:
            st.u32(ib, "parent")
    if fl & 0x100:
        curve_inline(st, e.setdefault("curve", {}))


def _trs_start(st, L, t, i, e):
    lp = _loop_point(st, L, t, i)
    fl = _base_event(st, L, t, i, e)
    if lp:
        return None
    if not _compact(st, t):
        _interp_block(st, L, t, fl, e)
    return fl


def _compressed(st, t):
    return bool(t["param2"] & 0x20000)


def _ev_translation(st, L, t, i, e):
    fl = _trs_start(st, L, t, i, e)
    if fl is None or fl & 0x800:
        return
    if _compressed(st, t) and t["param2"] & 0x4000000:
        st.raw(e, "value", 6)                 # 3 x i16 (MTH_Vec3Uncompress48)
    else:
        st.v3(e, "value")


def _ev_rotation(st, L, t, i, e):
    fl = _trs_start(st, L, t, i, e)
    if fl is None or fl & 0x800:
        return
    p2 = t["param2"]
    if _compressed(st, t):
        if p2 & 0x8000000:
            st.raw(e, "value", 8)             # MTH_tt_Quaternion64
        elif p2 & 0x4000000 or fl & 0x400:
            st.raw(e, "value", 6)             # 48-bit quaternion
        elif p2 & 0x2000000:
            st.raw(e, "value", 4)             # MTH_tt_Quaternion32
        else:
            st.quat(e, "value")
    elif fl & 0x400:
        if L["version"] < 14:
            raise FormatError("pre-14 unit quaternions (MTH_QuaternionUncompressUnit) not handled")
        st.raw(e, "value", 6)                 # 3 x u16 -> MTH_QuaternionUncompress48
    else:
        st.quat(e, "value")


def _ev_scale(st, L, t, i, e):
    fl = _trs_start(st, L, t, i, e)
    if fl is None or fl & 0x800:
        return
    p2 = t["param2"]
    if fl & 0x2000:
        st.raw(e, "value", 12)                # the diagonal, 3 x f32
    elif _compressed(st, t):
        if p2 & 0x2000000:
            st.raw(e, "value", 6)             # 3 x i16: 1 + i / 32767 on the diagonal
        elif p2 & 0x4000000:
            st.raw(e, "value", 12)            # 3 x f32 diagonal
        elif p2 & 0x8000000:
            st.raw(e, "value", 18)            # 9 x i16: identity + i / 32767
        else:
            st.m33(e, "value")
    else:
        st.m33(e, "value")


def _ev_morphing(st, L, t, i, e):
    _base_event(st, L, t, i, e)
    st.array(e, "targets", "<I", 6)
    st.array(e, "weights", "<f", 6)
    st.f32(e, "f0")
    st.f32(e, "f1")
    if st.u32(e, "has_curve") == 1:
        curve_inline(st, e.setdefault("curve", {}))


def _ev_script(st, L, t, i, e):
    _base_event(st, L, t, i, e)
    words = e.setdefault("words", [])
    if st.writing:
        e["n"] = len(words) + 1 if words else min(e.get("n", 0), 1)
    n = st.u32(e, "n")                        # word count + 1 (0: no call)
    st.array(e, "words", "<I", n - 1 if n else 0)


def _ev_value(st, L, t, i, e):
    fl = _base_event(st, L, t, i, e)
    st.v3(e, "value")
    if fl & 0x100:
        curve_inline(st, e.setdefault("curve", {}))


def _ev_sound(st, L, t, i, e):
    _base_event(st, L, t, i, e)
    st.u32(e, "key")                          # -> SLib::p_Callback_LoadFile
    st.u32(e, "mode")


def _ev_comment(st, L, t, i, e):
    _base_event(st, L, t, i, e)
    st.raw(e, "text", 128)
    st.u32(e, "u0")
    st.u32(e, "u1")


def _orient_to(st, c):
    ver = st.u32(c, "version")
    st.u32(c, "u0")
    if ver >= 2:
        st.u32(c, "u1")
    if st.u32(c, "has_vector") or ver < 2:
        st.v3(c, "vector")
    st.u8(c, "b0")
    if st.writing and c.get("curve"):
        c["has_curve"] = c.get("has_curve") or 1
    if st.u32(c, "has_curve"):
        curve_inline(st, c.setdefault("curve", {}))
    if ver > 2:
        st.u32(c, "u2")


def _rumble(st, c):
    ver = st.u32(c, "version")
    st.u32(c, "u0")
    st.u32(c, "u1")
    st.array(c, "f", "<f", 6)
    st.u32(c, "u2")
    st.u32(c, "u3")
    if ver > 1:
        st.u32(c, "u4")


def _shake(st, c):
    st.u32(c, "version")
    st.u32(c, "u0")
    st.v3(c, "axis")
    st.array(c, "f", "<f", 8)
    st.u32(c, "u1")


CAMERA_ORDERS = {1: _orient_to, 2: _rumble, 3: _shake}


def _camera_interp(st, L, fl, c):
    """EVE::CameraInterpEvent_pu32_Load."""
    if st.writing:
        if c.get("ext"):
            _put(st, "u32", 0xFFFFFFFF)
            st.u32(c, "iversion")
        st.quat(c, "rot")
    else:
        x = st.u32(c, "_x")
        del c["_x"]
        c["ext"] = x == 0xFFFFFFFF
        if c["ext"]:
            if st.u32(c, "iversion") > 2:
                raise FormatError(f"camera interpolation version {c['iversion']}")
            st.quat(c, "rot")
        else:
            c["iversion"] = 0
            c["rot"] = struct.pack("<I", x) + st.raw(c, "rot", 12)
    iver = c["iversion"] if c.get("ext") else 0
    st.v3(c, "pos")
    st.f32(c, "fov")
    if fl & 0x80A0:
        st.u32(c, "u0")
        if fl & 0x20:
            if L["version"] < 9:
                raise FormatError("pre-9 camera control points not handled")
            _ctrl_points(st, c.setdefault("ctrl", {}))
        if fl & 0x8000:
            if iver >= 2:
                st.u32(c, "parent")
                st.u32(c, "parent_u")
            else:
                v = st.u32(c, "parent_v") if iver == 1 else 0
                st.u32(c, "parent")
                if v:
                    st.u32(c, "parent_u")
    if fl & 0x100:
        curve_inline(st, c.setdefault("curve", {}))


def _ev_camera(st, L, t, i, e):
    fl = _base_event(st, L, t, i, e)
    if st.writing:
        if e.get("ext"):
            _put(st, "u16", 0xFFFF)
            st.u32(e, "cversion")
        kind = st.u16(e, "kind")
    else:
        kind = st.u16(e, "kind")
        e["ext"] = kind == 0xFFFF
        if e["ext"]:
            if st.u32(e, "cversion") > 1:
                raise FormatError(f"camera event version {e['cversion']}")
            kind = st.u16(e, "kind")
    st.u8(e, "view")
    st.u8(e, "cam_flags")
    if fl & 2:                                # the loop point: nothing more
        return
    if kind == 1:
        _camera_interp(st, L, fl, e.setdefault("interp", {}))
    elif kind == 2:                           # CameraOrderEvent: [0xFFFFFFFF, version] order, order->Load
        c = e.setdefault("order", {})
        if st.writing:
            if c.get("ext"):
                _put(st, "u32", 0xFFFFFFFF)
                st.u32(c, "oversion")
            order = st.u32(c, "order")
        else:
            order = st.u32(c, "order")
            c["ext"] = order == 0xFFFFFFFF
            if c["ext"]:
                if st.u32(c, "oversion") > 1:
                    raise FormatError(f"camera order version {c['oversion']}")
                order = st.u32(c, "order")
        if order in CAMERA_ORDERS:
            CAMERA_ORDERS[order](st, c.setdefault("data", {}))


def _ev_action(st, L, t, i, e):
    _base_event(st, L, t, i, e)
    if st.writing:
        if e.get("ext"):
            _put(st, "u8", 0xFF)
            st.u32(e, "aversion")
        st.u8(e, "mix_idx")
    else:
        m = st.u8(e, "mix_idx")
        e["ext"] = m == 0xFF
        if e["ext"]:
            if st.u32(e, "aversion") > 3:
                raise FormatError(f"action event version {e['aversion']}")
            st.u8(e, "mix_idx")
    av = e.get("aversion", 0) if e.get("ext") else 0
    st.u16(e, "action")                       # u16_ActionIdx: kit slot
    if av >= 2:
        st.u32(e, "aflags")                   # u32_Flags
    if av >= 3:
        st.f32(e, "play_speed")
        st.u16(e, "start_frame")
    st.u8(e, "mix_flags")
    if av >= 1:
        st.f32(e, "mix_percent")
    else:
        st.u8(e, "mix_percent_u8")
    st.u8(e, "mix_in_blend")
    st.u8(e, "mix_out_blend")
    st.array(e, "mix_mask", "<I", 8 if L["version"] >= 6 else 4)


def _ev_animsnap_old(st, L, t, i, e):
    _base_event(st, L, t, i, e)
    st.u32(e, "u0")
    if L["version"] >= 0xB:
        st.u16(e, "u1")


def _ev_animsound(st, L, t, i, e):
    lp = _loop_point(st, L, t, i)
    _base_event(st, L, t, i, e)
    if lp:
        return
    st.u16(e, "op")                           # 1 play, 2 stop
    st.u16(e, "rank")                         # SNK modifier rank
    st.u32(e, "index")                        # sound bank slot


def _ev_animsnap(st, L, t, i, e):
    _base_event(st, L, t, i, e)
    st.u32(e, "u0")
    st.u16(e, "u1")
    st.u16(e, "u2")
    if L["version"] >= 0x18:
        st.u16(e, "u3")
        st.u16(e, "u4")


def _ev_animation(st, L, t, i, e):
    _base_event(st, L, t, i, e)
    if L["version"] < 0x17:
        raise FormatError("animation events of lists below version 0x17 not handled")
    af = st.u16(e, "aflags")
    st.u8(e, "b0")
    st.u32(e, "key")                          # -> EVE::p_Callback_LoadTrl
    st.u16(e, "u0")
    st.u16(e, "u1")
    st.f32(e, "f0")
    if af & 0x10:
        st.trm(e, "matrix")


def _ev_snap(st, L, t, i, e):
    _base_event(st, L, t, i, e)
    st.u32(e, "u0")
    st.u16(e, "u1")
    st.u16(e, "u2")
    if st.u32(e, "sflags") & 1:
        st.trm(e, "matrix")


def _ev_dialog(st, L, t, i, e):
    _base_event(st, L, t, i, e)
    st.u32(e, "text_key")                     # BIG_th_Key, not a file reference
    st.u32(e, "u0")
    st.u32(e, "u1")


def _ev_afx(st, L, t, i, e):
    _base_event(st, L, t, i, e)
    st.u16(e, "u0")
    st.u8(e, "b0")
    MW.hlsl_instance(st, e.setdefault("hlsl", {}))


EVENT_LOADERS = {
    1: _ev_script, 2: _ev_translation, 3: _ev_rotation, 4: _ev_scale, 5: _ev_morphing,
    7: _ev_value, 8: _ev_value, 9: _ev_value, 10: _ev_sound, 11: _ev_comment, 12: _ev_camera,
    13: _ev_action, 14: _ev_animsnap_old, 15: _ev_animsound, 16: _ev_animsnap, 17: _ev_animation,
    19: _ev_animation, 20: _ev_snap, 21: _ev_dialog, 22: _ev_afx,
}


def scr_signal(st, g):
    """The signal SCR::LoadTrigger reads first (inline in the loader)."""
    st.u32(g, "key")                          # BIG_th_Key
    st.raw(g, "name", 64)
    st.u32(g, "key2")                         # BIG_th_Key
    st.u32(g, "u0")


def vid_stock(st, s):
    """ViD::LoadStock (RGH 800ACE6C)."""
    if st.writing:
        if s.get("ext"):
            _put(st, "u32", 0xC0DEC0DE)
            st.i32(s, "version")
        st.i32(s, "i0")
    else:
        first = st.u32(s, "i0")
        s["ext"] = first == 0xC0DEC0DE
        if s["ext"]:
            st.i32(s, "version")
            st.i32(s, "i0")
        else:
            s["version"] = 0
            s["i0"] = first - (1 << 32) if first & 0x80000000 else first
    st.array(s, "i", "<i", 4)
    st.array(s, "f", "<f", 5)
    st.array(s, "keys", "<I", 5)              # BIG_th_Key (setValue, no file reference)
    if s.get("ext") and s.get("version"):
        st.array(s, "keys2", "<I", 5)
    st.array(s, "f2", "<f", 15)


def scr_trigger(st, tr):
    """SCR::LoadTrigger: the signal, then the stock."""
    scr_signal(st, tr.setdefault("signal", {}))
    vid_stock(st, tr.setdefault("stock", {}))


def _dialog_track(st, t):
    """EVE::DialogTrack_pu32_Load."""
    d = t.setdefault("dialog", {})
    if st.u32(d, "version") == 1:
        st.u32(d, "u0")
        if st.writing:
            d["has_trigger"] = 1 if d.get("trigger") else 0
        if st.u32(d, "has_trigger"):
            scr_trigger(st, d.setdefault("trigger", {}))
        st.u32(d, "material")                 # a material key


def _track(st, L, t):
    ver = L["version"]
    if ver >= 0x11:
        st.u32(t, "m3_u0", 3)
        st.f32(t, "m3_f0", 3)
    if ver < 0x19:
        st.u32(t, "obj_idx")
    else:
        st.u16(t, "obj_idx")                  # u16_ObjIdx: the actor
    if ver >= 0x13:
        st.u16(t, "u16_a", 2 if ver < 0x19 else 3)
    evs = t.setdefault("events", [])
    n = _count(st, t, "n_events", len(evs), "u16")
    if ver < 0x10:
        typ = st.u16(t, "type")
    else:
        typ = st.u8(t, "type")
        if ver < 0x19:
            st.u8(t, "extra")                 # bit 0 -> track flag 0x80 at load time
    st.u16(t, "flags")
    st.u8(t, "channel")
    st.u8(t, "skeleton")
    st.u32(t, "modifier")
    st.u32(t, "param1")
    st.u32(t, "param2")
    _fixed(st, t, "events", n)
    if n:
        if typ not in EVENT_LOADERS:
            raise FormatError(f"events on track type {typ}")
        if typ == 1 and L["flags"] & 8 and ver >= 0xD:
            raise FormatError("script track in a looping list (the loaders destroy the list)")
        fn = EVENT_LOADERS[typ]
        for i, e in enumerate(evs):
            fn(st, L, t, i, e)
    if typ == 21:
        _dialog_track(st, t)
    st.u16(t, "m3_u1", 3)
    st.u8(t, "m3_b0", 3)
    st.u8(t, "m3_b1", 3)
    st.raw(t, "m3_raw", 24, 3)
    if ver >= 3:
        st.u32(t, "m3_u2", 3)


def _signal_track(st, s):
    """EVE::pu32_LoadSignalTrack."""
    st.u32(s, "id")
    sigs = s.setdefault("signals", [])
    if st.writing and s.get("version", 0) < 1000:
        s["version"] = len(sigs)
    ver = st.u32(s, "version")
    if ver >= 1000:
        n = _count(st, s, "n_signals", len(sigs))
    else:
        n = ver
    for g in _fixed(st, s, "signals", n):
        st.u16(g, "frame")
        st.u16(g, "signal")
        if ver >= 1000:
            st.u16(g, "length")
            st.u16(g, "flags")
        else:
            st.u32(g, "value")
    if ver >= 1000:
        for cb in _list(st, s, "callbacks", "n_callbacks"):   # EVE::pu32_LoadCallback
            st.u16(cb, "u0")
            st.u32(cb, "u1")
            vid_stock(st, cb.setdefault("stock", {}))
            st.u16(cb, "u2")
            st.u16(cb, "u3")
        for tr in _list(st, s, "triggers", "n_triggers"):      # EVE::pu32_LoadTrigger
            st.u16(tr, "u0")
            scr_trigger(st, tr.setdefault("trigger", {}))
            st.u16(tr, "u2")
            st.u16(tr, "u3")
        st.u32(s, "sflags")
    st.raw(s, "m3_raw", 24, 3)


def trl(st, o):
    st.memsize(o)
    tracks = o.setdefault("tracks", [])
    if st.writing:
        o["n_tracks"] = len(tracks)
        if o.get("version", 0) or o.get("long_header", True):
            _put(st, "u32", 0xFFFFFFFF)
            st.u32(o, "version")
        st.u32(o, "n_tracks")
    else:
        v = st.u32(o, "n_tracks")
        o["long_header"] = v == 0xFFFFFFFF
        if o["long_header"]:
            st.u32(o, "version")
            st.u32(o, "n_tracks")
        else:
            o["version"] = 0
    ver = o["version"]
    if ver > 0x19:
        raise FormatError(f"track list version {ver:#x}")
    flags = st.u32(o, "flags")
    if ver >= 4:
        st.u8(o, "frequency")
    if ver >= 0x12:
        st.u32(o, "m1_u0", 1)
    if ver >= 0xF:
        st.u32(o, "m1_u1", 1)
    if ver >= 0x11:
        st.raw(o, "m1_name", 64, 1)
        for e in _list(st, o, "m1_entries", "n_m1_entries"):
            st.u32(e, "m1_a", 1)
            st.u32(e, "m1_b", 1)
            st.f32(e, "m1_f", 1)
    if ver == 0x15:
        for e in _list(st, o, "v15_objects", "n_v15_objects"):
            st.u32(e, "key")
            st.name64(e, "m3_name", 3)
    for t in _fixed(st, o, "tracks", o["n_tracks"]):
        _track(st, o, t)
    if flags & 2:                             # EVE_tt_ListSignalTracks
        sig = o.setdefault("signals", {})
        st.u16(sig, "u0")
        for s in _list(st, sig, "tracks", "n_tracks"):
            _signal_track(st, s)
    if ver >= 0x14:
        st.u32(o, "m1_edge_u", 1)
        _blob(st, o, "edge_anim", "edge_anim_size")   # EdgeAnimAnimation 'EA03', big-endian
    if flags & 0x20 and not flags & 0x18:     # EVE_tt_SpecificCinematic
        _words(st, o, "cine_objects", "n_cine_objects")
    return o


def trl_object_keys(o):
    keys = [e.get("key", 0) for e in o.get("v15_objects", [])] + list(o.get("cine_objects", []))
    for t in o.get("tracks", []):
        if t.get("type") == 1:
            for e in t.get("events", []):
                w = e.get("words", [])
                keys += [w[j + 1] for j in range(len(w) - 1) if w[j] == 0x00190000]
    return [k for k in keys if k]


def refs_trl(o):
    out = []
    for t in o.get("tracks", []):
        typ = t.get("type")
        for e in t.get("events", []):
            if typ == 10:
                out.append((e.get("key", 0), "soundfile"))
            elif typ in (17, 19):
                out.append((e.get("key", 0), "trl"))
            elif typ == 22:
                c = MW.hlsl_chunk_of(e.get("hlsl", {}), MW.HLSL_MODEL)
                if c:
                    out.append((c.get("hlsl_key", 0), "hlsl"))
        if typ == 21:
            out.append((t.get("dialog", {}).get("material", 0), "material"))
    out += [(k, "object") for k in trl_object_keys(o)]
    seen = set()
    res = []
    for k, kind in _tag(out):
        if (k, kind) not in seen:
            seen.add((k, kind))
            res.append((k, kind))
    return res
