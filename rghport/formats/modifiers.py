"""Modifier streams: SNK, SNF, SNL, TXT, MSSG, TBO, DST (+ the sub-resources they reference: object-table shapes,
sound-linker links).

Loaders (RGH Wii executable):

  SNK  SNK::p_Callback_LoadSNK      (RGH 800DDF98)
       SNK::p_Load_SNK00000003 (versions < 4, not shipped), SCR::LoadTrigger (4 reads + ViD::LoadStock),
       ViD::LoadStock (RGH 800ACE6C).
  SNF  SNF::p_Callback_LoadSNF      (RGH 8018CAEC)
       ZON_Zon::p_LoadFromBuffer (zone: RGH 80190E80 / area: 8018F538),
       ZON_ASlot::po_Create (only slot type 2 = ZON_ASlot_SNDFxBind exists),
       ZON_ASlot_SNDFxBind::p_LoadFromBuffer (RGH 8018FA10, chunk jump table 8055D6B0),
       ZON_ASlot_SNDFxBind::p_LoadFromBufferV5 (versions < 6),
       ZON_ASlot_SNDFxBind::OK3::pu8_LoadFromBuffer, MTH_pu8_LoadBoxFromBuffer.
  SNL  SNL::p_Callback_LoadSNL      (RGH 801D8FA0)
       SNL_Link::p_Callback_Load    (RGH 801D944C)
  TXT  TXT::p_Callback_LoadTXT      (RGH 8015BB60)
  MSSG MSSG::p_Callback_LoadMessage (RGH 8010DE00)
  TBO  TBO::p_Callback_LoadTBL      (RGH 800E4E54)
       TBO::p_Callback_LoadShape    (RGH 800E4C24)
  DST  DST::p_Callback_LoadDesignStruct (RGH 800E73D0)

Versions RGH ships: SNK 13, SNF 5 with bind version 7, SNL 1 + link 5, TXT 2, MSSG 0, TBO 0, DST 1.
ViD::LoadStock tests the high half of the marker word (0xC0DE); the marker written here is 0xC0DEC0DE.

Keys through LOA_MakeFileRef (see the refs_* helpers):
  SNK  one of bank_key (SLib::p_Callback_LoadSoundBank) / file_key (SLib::p_Callback_LoadFile), chosen by sound_type.
  SNF  bank_keys (sound banks), zone.geom_key (K3D_visual), every curve key of every bind (MTH_p_Callback_CurveLoad).
  SNL  base_link_key (SNL_Link::p_Callback_Load); the link itself references sound files, smx files, curves, an
       optional father link.
  TXT  its reference list: n_texts Txg keys (TXT::p_Callback_LoadTxg) then n_dialogs Txd keys
       (TXT::p_Callback_LoadTxd).
  TBO  grp_key (GRP::p_Callback_LoadGrp) and shape_key (TBO::p_Callback_LoadShape).
  DST / MSSG: none (DST's obj1..5 are object keys of the same world, resolved by ViD::ResolveKeyForObj in
       DST::AfterLoading, not files).
"""
from __future__ import annotations

from . import stream as F
from .stream import FormatError

MAGIC = 0xC0DEC0DE

DST_TYPE, MSSG_TYPE, TBO_TYPE, TXT_TYPE, SNK_TYPE, SNF_TYPE, SNL_TYPE = 1, 2, 7, 8, 22, 23, 41

# SNK sound types that reference a single SLib file rather than a bank
# (SNK::p_Callback_LoadSNK: type-1 <= 2 or type-5 <= 2)
SNK_FILE_TYPES = {1, 2, 3, 5, 6, 7}


# ----------------------------------------------------------------------------
# helpers
def _count(st, o, name, value, fn="u32"):
    """A count: derived from the content when writing."""
    if st.writing:
        o[name] = value
    return getattr(st, fn)(o, name)


def _items(st, o, name, n):
    """The n sub-dicts of o[name] (appended while reading, created if missing while writing)."""
    lst = o.setdefault(name, [])
    if st.writing:
        while len(lst) < n:
            lst.append({})
    for i in range(n):
        if not st.writing:
            lst.append({})
        yield lst[i]


def _mdf_version(st, o):
    """MDF::pu32_BeginLoad; loaders older than MDF versions keep their own u32 version right after the header when the
    header's is 0."""
    F.mdf_header(st, o)
    ver = o.get("version", 0)
    if ver == 0:
        ver = st.u32(o, "version_u32")
    return ver


def _v3_list(st, o, name, n):
    vecs = o.setdefault(name, [])
    for i in range(n):
        if st.writing:
            st.v3({"v": vecs[i] if i < len(vecs) else None}, "v")
        else:
            vecs.append(st.v3({}, "v"))
    return vecs


# ----------------------------------------------------------------------------
# ViD_tt_Stock_ and SCR triggers (used by SNK player data)
def stock(st, o):
    """ViD::LoadStock: [0xC0DEC0DE, version], i32 x5, f32 x5, u32 obj x5, [version != 0: u32 mdf x5], v3 x5.  Without
    the marker the first word is int1 and the version is 0."""
    if st.writing:
        ver = o.get("stock_version", 0)
        if ver:
            st.u32({"m": MAGIC}, "m")
            st.u32(o, "stock_version")
        st.i32(o, "int1")
    else:
        first = st.u32(o, "int1")
        if first == MAGIC:
            ver = st.u32(o, "stock_version")
            st.i32(o, "int1")
        else:
            ver = o["stock_version"] = 0
            o["int1"] = first - (1 << 32) if first >= (1 << 31) else first
    for i in range(2, 6):
        st.i32(o, f"int{i}")
    for i in range(1, 6):
        st.f32(o, f"float{i}")
    for i in range(1, 6):
        st.u32(o, f"obj{i}")
    if ver != 0:
        for i in range(1, 6):
            st.u32(o, f"mdf{i}")
    for i in range(1, 6):
        st.v3(o, f"vec{i}")
    return o


def trigger(st, o):
    """SCR::LoadTrigger = SCR_tt_Signal_ (function-list file key, name[64], object key, modifier rank) followed by a
    ViD_tt_Stock_."""
    st.u32(o, "fct_list_key")
    st.raw(o, "name", 64)
    st.u32(o, "obj_key")
    st.u32(o, "mdf_rank")
    stock(st, o.setdefault("stock", {}))
    return o


# ----------------------------------------------------------------------------
# SNK (type 22)
def _snk_activation(st, p, prefix, mode, ver):
    """The play / stop activation of SNK_tt_PlayerData: mode 1 = event zone key + actor key, 7 = one signed word,
    2 = a trigger, 0/3..6 = nothing; then the callback trigger (always present before version 8, flagged by a byte
    from version 8)."""
    if mode == 1:
        st.u32(p, f"{prefix}_zde")
        st.u32(p, f"{prefix}_actor")
    elif mode == 7:
        st.i32(p, f"{prefix}_zde_i32")
    elif mode == 2:
        trigger(st, p.setdefault(f"{prefix}_trigger", {}))
    elif mode not in (0, 3, 4, 5, 6):
        raise FormatError(f"SNK {prefix} activation mode {mode}")
    cb = f"{prefix}_callback"
    if ver < 8:
        trigger(st, p.setdefault(cb, {}))
    else:
        has = _count(st, p, f"has_{cb}", 1 if p.get(cb) else 0, "u8")
        if has:
            trigger(st, p.setdefault(cb, {}))


def snk(st, o):
    """SNK::p_Callback_LoadSNK (RGH ships version 13).  A bank key, a file key and the sound type (which one is
    referenced depends on the type), then the play mode and, unless its low byte is 0 (version >= 8), the
    SNK_tt_PlayerData block."""
    st.memsize(o)
    ver = _mdf_version(st, o)
    if ver < 4:
        raise FormatError(f"SNK version {ver} (SNK::p_Load_SNK00000003 layout) not supported")
    st.u32(o, "bank_key")
    st.u32(o, "file_key")
    st.u32(o, "sound_type")
    pm = st.u32(o, "play_mode")
    if (pm & 0xFF) == 0 and ver >= 8:
        if not st.writing:
            o["player"] = None
        return o
    p = o.get("player")
    if p is None:
        p = o["player"] = {}
    _snk_activation(st, p, "play", pm & 0xFF, ver)
    st.f32(p, "play_latency")
    st.u16(p, "play_limit")
    st.u16(p, "stop_limit")
    sm = st.u32(p, "stop_mode")
    _snk_activation(st, p, "stop", sm & 0xFF, ver)
    st.f32(p, "stop_latency")
    if ver >= 12:
        lm = st.u16(p, "localizer_mode")
        st.u16(p, "localizer_flag")
    else:
        # one word over both u16 fields; the Wii keeps the mode in the high half
        lm = (st.u32(p, "localizer_mode32") >> 16) & 0xFFFF
    st.u32(p, "localizer_target")
    st.u32(p, "localizer_subtarget")
    if ver >= 5:
        st.u32(p, "max_instance")
    if ver >= 6:
        locs = p.setdefault("localizers", [])
        if st.writing and lm in (3, 4, 5, 6):
            p["localizer_count"] = len(locs)
        n = st.u32(p, "localizer_count")
        if n and lm in (3, 4, 5):
            for it in _items(st, p, "localizers", n):
                st.u32(it, "obj")
                st.i32(it, "volume")
        elif n and lm == 6:
            # one tt_MultiObjByChannel per channel
            for it in _items(st, p, "localizers", n):
                st.i32(it, "volume")
                objs = it.setdefault("objs", [])
                m = _count(st, it, "count", len(objs))
                for s in _items(st, it, "objs", m):
                    st.u32(s, "obj")
                    st.i32(s, "volume")
    return o


def refs_snk(o):
    """The one sound resource the loader pulls: the file for the single-file sound types, the bank otherwise."""
    k = o.get("file_key", 0) if o.get("sound_type", 0) in SNK_FILE_TYPES else o.get("bank_key", 0)
    return [k] if k else []


def snk_trigger_keys(o):
    """Keys inside the player's triggers (script function-list files and object keys); not LOA_MakeFileRef'd by the
    loader, resolved at run time."""
    out = []
    p = o.get("player") or {}
    for nm in ("play_trigger", "play_callback", "stop_trigger", "stop_callback"):
        t = p.get(nm)
        if t:
            out += [t.get("fct_list_key", 0), t.get("obj_key", 0)]
    for nm in ("play_zde", "play_actor", "stop_zde", "stop_actor"):
        out.append(p.get(nm, 0))
    return [k for k in out if k]


# ----------------------------------------------------------------------------
# ZON zones (SNF areas) and their sound-bind slots
def box(st, o, name="box"):
    """MTH_pu8_LoadBoxFromBuffer: center, min, max + a word only loose files keep."""
    return st.box(o, name)


def ok3(st, k):
    """ZON_ASlot_SNDFxBind::OK3::pu8_LoadFromBuffer: box, u16 n + n vector indices, u16 n + n children (recursive)."""
    box(st, k)
    idx = k.setdefault("indices", [])
    n = _count(st, k, "n_indices", len(idx), "u16")
    st.array(k, "indices", "<H", n)
    kids = k.setdefault("children", [])
    m = _count(st, k, "n_children", len(kids), "u16")
    for c in _items(st, k, "children", m):
        ok3(st, c)
    return k


def _bind_chunk(st, c, tag):
    """One chunk of ZON_ASlot_SNDFxBind::p_LoadFromBuffer (bind version >= 6); the tag -> case map is the loader's jump
    table."""
    if tag == 1:
        ok3(st, c.setdefault("ok3", {}))
    elif tag == 2:
        vecs = c.setdefault("vectors", [])
        n = _count(st, c, "vect_count", len(vecs) // 2)
        _v3_list(st, c, "vectors", 2 * n)
    elif tag == 3:
        st.u32(c, "transition_type")
    elif tag == 4:
        n = _count(st, c, "n", len(c.get("radius") or []), "u8")
        st.array(c, "radius", "<f", n)
    elif tag == 5:
        n = _count(st, c, "n", len(c.get("angle") or []), "u8")
        st.array(c, "angle", "<f", n)
    elif tag == 6:
        n = _count(st, c, "n", len(c.get("curves") or []), "u8")
        st.array(c, "curves", "<I", n)
    elif tag == 7:
        box(st, c)
    elif tag == 8:
        st.u32(c, "transition_flag")
    # tag 0 and tags > 8: no payload (the loader ignores them)


def snd_bind_v5(st, s, ver):
    """ZON_ASlot_SNDFxBind::p_LoadFromBufferV5 (bind versions 0..5, not shipped by RGH): fixed layout keyed on the
    transition type."""
    vecs = s.setdefault("vectors", [])
    if ver >= 1:
        n = _count(st, s, "vect_count", len(vecs) // 2)
    else:
        n = s.setdefault("vect_count", len(vecs) // 2)
    _v3_list(st, s, "vectors", 2 * n)
    if ver < 2:
        return s
    t = st.u32(s, "transition_type")
    if t == 1:
        st.u32(s, "t1_u0")
        st.u32(s, "t1_u1")
    elif t in (2, 3):
        st.u32(s, "curve_a")
        st.u32(s, "curve_b")
        st.array(s, "radius", "<f", 4)
        st.array(s, "angle", "<f", 4)
    elif t == 4:
        st.u32(s, "curve_a")
        st.u32(s, "curve_b")
        st.array(s, "radius", "<f", 2)
    elif t == 5:
        st.u32(s, "curve_a")
        st.array(s, "radius", "<f", 1)
    if ver < 3:
        return s
    box(st, s)
    if ver < 4:
        return s
    kids = s.setdefault("ok3_list", [])
    if st.writing:
        for k in kids:
            st.u8({"t": 1}, "t")
            ok3(st, k)
        st.u8({"t": 0xFF}, "t")
    else:
        while True:
            tag = st.u8({}, "t")
            if tag == 0xFF:
                break
            if tag == 1:
                k = {}
                ok3(st, k)
                kids.append(k)
    if ver >= 5:
        st.array(s, "v5_tail", "<I", 6)
    return s


def snd_bind(st, s):
    """ZON_ASlot_SNDFxBind::p_LoadFromBuffer: a version (RGH <= 7), then tagged chunks until 0xFF (versions >= 6):
      1 OK3 tree, 2 u32 n + 2n vectors, 3 u32 transition type,
      4 u8 n + n radii, 5 u8 n + n angles, 6 u8 n + n curve keys,
      7 box, 8 u32 transition flag, 0 (and 9..254) nothing."""
    ver = st.u32(s, "bind_version", default=7)
    if ver > 7:
        raise FormatError(f"sound bind version {ver}")
    if ver < 6:
        return snd_bind_v5(st, s, ver)
    chunks = s.setdefault("chunks", [])
    if st.writing:
        for c in chunks:
            st.u8(c, "tag")
            _bind_chunk(st, c, c["tag"])
        st.u8({"t": 0xFF}, "t")
    else:
        while True:
            tag = st.u8({}, "t")
            if tag == 0xFF:
                break
            c = {"tag": tag}
            _bind_chunk(st, c, tag)
            chunks.append(c)
    return s


def bind_field(s, name, default=None):
    """The value of a named chunk field of a sound bind (chunk format)."""
    for c in s.get("chunks", []):
        if name in c:
            return c[name]
    return s.get(name, default)


def zon_area(st, a):
    """ZON_Zon::p_LoadFromBuffer(ZON_tt_Area*): uid, a word and a name only loose files keep, (area version << 16 |
    slot count), the slots (type word + slot stream), the bind uid list, and (area version >= 1) flags."""
    st.u32(a, "uid")
    st.u32(a, "m3_u32", 3)
    st.raw(a, "name", 64, 3)
    slots = a.setdefault("slots", [])
    if st.writing:
        vs = (a.get("area_version", 0) << 16) | len(slots)
        st.u32({"v": vs}, "v")
    else:
        vs = st.u32({}, "v")
        a["area_version"] = vs >> 16
    for s in _items(st, a, "slots", vs & 0xFFFF):
        t = st.u32(s, "slot_type", default=2)
        if t != 2:
            raise FormatError(f"ZON slot type {t} (only 2 = ZON_ASlot_SNDFxBind exists)")
        snd_bind(st, s)
    binds = a.setdefault("binds", [])
    n = _count(st, a, "n_binds", len(binds))
    st.array(a, "binds", "<I", n)
    if a["area_version"] >= 1:
        st.u32(a, "flags")
    return a


def zone(st, z):
    """ZON_Zon::p_LoadFromBuffer: n_ids (0 or 1 -> n_ids + 1 id blocks of {binds, areas}), the geometry key, the pivot
    (Matrix44 + position)."""
    ids = z.setdefault("ids", [])
    n = _count(st, z, "n_ids", max(len(ids) - 1, 0))
    if n > 1:
        raise FormatError(f"ZON zone with n_ids {n} (the loader reads two id blocks for any n_ids above 0)")
    for d in _items(st, z, "ids", n + 1):
        binds = d.setdefault("binds", [])
        nb = _count(st, d, "n_binds", len(binds))
        for a in _items(st, d, "binds", nb):
            zon_area(st, a)
        areas = d.setdefault("areas", [])
        na = _count(st, d, "n_areas", len(areas))
        for a in _items(st, d, "areas", na):
            zon_area(st, a)
    st.u32(z, "geom_key")
    st.m44(z, "pivot_m44")
    st.v3(z, "pivot_pos")
    return z


# ----------------------------------------------------------------------------
# SNF (type 23)
def snf(st, o):
    """SNF::p_Callback_LoadSNF (RGH ships version 5): the zone, the activator, one (version < 2) or two sound bank keys,
    z offset (>= 1), per id the sorted bind index list (>= 3, sized by that id's bind count), flags (>= 4)."""
    st.memsize(o)
    ver = _mdf_version(st, o)
    z = o.setdefault("zone", {})
    zone(st, z)
    st.u32(o, "activator")
    st.array(o, "bank_keys", "<I", 2 if ver >= 2 else 1)
    if ver >= 1:
        st.f32(o, "zoffset")
    if ver >= 3:
        ids = z.get("ids", [])
        sb = o.setdefault("sorted_binds", [])
        for i in range(2):
            n = len(ids[i].get("binds", [])) if i < len(ids) else 0
            if st.writing:
                st.array({"a": sb[i] if i < len(sb) else []}, "a", "<I", n)
            else:
                sb.append(st.array({}, "a", "<I", n))
    if ver >= 4:
        st.u32(o, "snf_flags")
    return o


def bind_curve_keys(s):
    out = []
    for c in s.get("chunks", []):
        out += c.get("curves") or []
    out += [s.get("curve_a", 0), s.get("curve_b", 0)]
    return [k for k in out if k]


def zone_binds(z):
    for d in z.get("ids", []):
        for a in d.get("binds", []) + d.get("areas", []):
            for s in a.get("slots", []):
                yield s


def refs_snf(o):
    """Sound banks, the zone geometry and every bind's curve files."""
    out = [k for k in o.get("bank_keys", []) if k]
    z = o.get("zone", {})
    if z.get("geom_key"):
        out.append(z["geom_key"])
    for s in zone_binds(z):
        out += bind_curve_keys(s)
    return out


# ----------------------------------------------------------------------------
# SNL (type 41) and its link resource
def snl(st, o):
    """SNL::p_Callback_LoadSNL: just the base link key."""
    st.memsize(o)
    _mdf_version(st, o)
    st.u32(o, "base_link_key")
    return o


def refs_snl(o):
    k = o.get("base_link_key", 0)
    return [k] if k else []


def snl_link(st, o):
    """SNL_Link::p_Callback_Load (RGH ships version 5): entries {sound type, sound key, smx key (>= 5), name (!= 0),
    u32[n_entries] transition indices}, transitions, and (version > 2) an optional father link with two inherit
    tables."""
    st.memsize(o)
    ver = st.u32(o, "version", default=5)
    if ver > 5:
        raise FormatError(f"SNL link version {ver}")
    entries = o.setdefault("entries", [])
    n = _count(st, o, "n_entries", len(entries))
    for e in _items(st, o, "entries", n):
        st.u32(e, "sound_type")          # passed to SLib::p_Callback_LoadFile
        st.u32(e, "sound_key")
        if ver >= 5:
            st.u32(e, "smx_key")         # SLib file type 5 (smx)
        if ver != 0:
            st.u32(e, "name")
        st.array(e, "transition_idx", "<I", n)
    trans = o.setdefault("transitions", [])
    m = _count(st, o, "n_transitions", len(trans))
    for t in _items(st, o, "transitions", m):
        st.u32(t, "user_count")
        st.u16(t, "mode_stop")
        st.u16(t, "trig_modulo")
        st.u32(t, "trig_marker_name")
        st.u32(t, "fade_out_curve")
        if ver >= 4:
            st.f32(t, "fade_out_scale")
        st.f32(t, "fade_out_delay")
        st.f32(t, "sync_position_delay")
        if ver >= 5:
            st.f32(t, "back_in_delay")
            st.f32(t, "back_out_delay")
        st.u32(t, "fade_in_curve")
        if ver >= 4:
            st.f32(t, "fade_in_scale")
        st.f32(t, "fade_in_delay")
        st.u32(t, "aux_punch_curve")
        if ver >= 4:
            st.f32(t, "aux_punch_scale")
        st.f32(t, "aux_punch_delay")
        st.u32(t, "xtra_sound_type")
        st.u32(t, "xtra_sound_key")
        st.f32(t, "xtra_sound_delay")
    if ver > 2:
        fk = st.u32(o, "father_key")
        if fk:
            c2f = o.get("inherit_child_to_father") or []
            k = _count(st, o, "inherit_size", len(c2f))
            st.array(o, "inherit_child_to_father", "<I", k)
            st.array(o, "inherit_father_to_child", "<I", k)
    return o


def refs_snl_link(o):
    out = []
    for e in o.get("entries", []):
        out += [e.get("sound_key", 0), e.get("smx_key", 0)]
    for t in o.get("transitions", []):
        out += [t.get("fade_out_curve", 0), t.get("fade_in_curve", 0), t.get("aux_punch_curve", 0),
                t.get("xtra_sound_key", 0)]
    out.append(o.get("father_key", 0))
    return [k for k in out if k]


# ----------------------------------------------------------------------------
# TXT (type 8)
def txt(st, o):
    """TXT::p_Callback_LoadTXT: two counts, then the reference list (LOA_p_LoadRef with b_Refs: inline in packages, the
    reference table of a loose file): refs[:n_texts] are text groups (Txg), the rest dialogs (Txd)."""
    st.memsize(o)
    _mdf_version(st, o)
    refs = o.setdefault("refs", [])
    if st.writing:
        o.setdefault("n_texts", len(refs))
        o.setdefault("n_dialogs", len(refs) - o["n_texts"])
    st.u32(o, "n_texts")
    st.u32(o, "n_dialogs")
    st.refs(o, "refs")
    return o


def refs_txt(o):
    return [r.key for r in o.get("refs", []) if r.key]


# ----------------------------------------------------------------------------
# MSSG (type 2)
def mssg(st, o):
    """MSSG::p_Callback_LoadMessage: nothing beyond the MDF header."""
    st.memsize(o)
    F.mdf_header(st, o)
    return o


def refs_mssg(o):
    return []


# ----------------------------------------------------------------------------
# TBO (type 7) and the shape model it references
def tbo(st, o):
    """TBO::p_Callback_LoadTBL: group key, shape model key, default shape, default skeleton key (mu32_DefaultSkl)."""
    st.memsize(o)
    F.mdf_header(st, o)
    st.u32(o, "grp_key")
    st.u32(o, "shape_key")
    st.u32(o, "default")
    st.u32(o, "default_skl")
    return o


def refs_tbo(o):
    return [k for k in (o.get("grp_key", 0), o.get("shape_key", 0)) if k]


def tbo_shape(st, o):
    """TBO::p_Callback_LoadShape (TBL_tt_ShapeModel): u16 count, or 0xC0DE + u16 version + u16 count; u16 flags; per
    shape a name[64] only loose files keep, a u16 mode (version != 0) and u32 entries x {channel, type, object id}."""
    st.memsize(o)
    shapes = o.setdefault("shapes", [])
    if st.writing:
        ver = o.get("version", 0)
        if ver:
            st.u16({"m": 0xC0DE}, "m")
            st.u16(o, "version")
        _count(st, o, "n_shapes", len(shapes), "u16")
    else:
        first = st.u16(o, "n_shapes")
        if first == 0xC0DE:
            ver = st.u16(o, "version")
            st.u16(o, "n_shapes")
        else:
            ver = o["version"] = 0
    st.u16(o, "flags")
    for s in _items(st, o, "shapes", o["n_shapes"]):
        st.raw(s, "name", 64, 3)
        if ver != 0:
            st.u16(s, "mode")
        ents = s.setdefault("entries", [])
        n = _count(st, s, "n_entries", len(ents))
        for e in _items(st, s, "entries", n):
            st.u32(e, "channel")
            st.u32(e, "type")
            st.u32(e, "obj_id")
    return o


# ----------------------------------------------------------------------------
# DST (type 1)
def dst(st, o):
    """DST::p_Callback_LoadDesignStruct: the first 3 (version 0) or 5 (version >= 1) slots of each ViD_tt_Stock_ family:
    i32, f32, object key, vector3."""
    st.memsize(o)
    F.mdf_header(st, o)
    n = 5 if o.get("version", 0) != 0 else 3
    for i in range(1, n + 1):
        st.i32(o, f"int{i}")
    for i in range(1, n + 1):
        st.f32(o, f"float{i}")
    for i in range(1, n + 1):
        st.u32(o, f"obj{i}")
    for i in range(1, n + 1):
        st.v3(o, f"vec{i}")
    return o


def refs_dst(o):
    """No file references; see dst_object_keys."""
    return []


def dst_object_keys(o):
    """Object keys of the same world (ViD::ResolveKeyForObj in DST::AfterLoading)."""
    return [o[f"obj{i}"] for i in range(1, 6) if o.get(f"obj{i}")]
