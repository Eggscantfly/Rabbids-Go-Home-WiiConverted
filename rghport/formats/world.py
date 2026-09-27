"""World-side modifier streams: NET, AFX, RDP, MGM, BVO (the modifiers a world lists in its modifier reference list),
plus MGF, the Magma file descriptor an MGM record loads.  Script instances live in script.py.

Every stream follows its loader in the RGH Wii executable; field names are the DWARF members (NET, NET_tt_Node_,
NET_tt_Link_, AFX, K3D_HLSL_Instance, K3D_HLSL_tt_StoreVar, BVO, K3D_texpro_bink, MGM, MGF, MGB, IK3, LOO).
`refs_<name>(o)` returns [(key, kind), ...]: object, video, hlsl, mgf, mgb, mgb_font, texture.

Loaders                                       RGH
NET::p_Callback_LoadNetwork                   8024751C
  NET::AfterLoading                           80247778
AFX::p_Callback_LoadAFX                       800E5E84
  K3D_HLSL_Instance::p_CreateFromBuffer       8034AC70
  K3D_HLSL_Instance::pc_LoadFromBuffer        8034AD2C
RDP::p_Callback_LoadRDP                       801A5F0C
MGM::p_Callback_LoadMGM                       801FFC90
  MGF::p_Callback_LoadMGF                     80456D24
  MGF::LoadPackage / MGB::p_Callback_LoadMGB  80457640 / 8045670C
BVO::p_Callback_LoadBVO                       801F5C28
  K3D_texpro_bink::pu32_LoadFromBuffer        803322EC
IK3::p_Callback_LoadIK3                       80106F0C
LOO::p_Callback_LoadLOO                       8010D678

Census (MDF header scan of all 386 Wii packages; every record parses at level 3 and re-emits byte-exact): NET 354,
AFX 53, RDP 42, MGM 11, BVO 1, MGF 13 distinct (24 copies), IK3 0, LOO 0.

NET (5) - MDF version 2 in all records.
  u32 node count; per node: u32 obj (NET_tt_Node_::po_Obj, an object key), u32 link count, per link {u32 next
  (u32_Next, a node index), u32 capacities_init (u32_CapacitiesInit, copied into u32_Capacities)}; u32 merge_id
  (mu32_MergeID); u32 u7 (read and dropped); version >= 1: u32 design_init (mu32_DesignInit); version >= 2: mode-1 u32
  m1_u9 (absent in packages).
  Keys: node objects -> "object".  The loader makes no file reference: po_Obj holds the key until NET::AfterLoading
  resolves it (ViD::ResolveKeyForObj), so the objects must be loaded by the world.  551 of the 3184 node objects are
  OBJ records of the package that are neither in their world's object list nor hierarchy fathers.

AFX (29) - MDF version 3 in all records.
  version >= 2: u16 mode (mu16_Mode); version >= 3: u8 viewports, u8 dummy; version 2 instead one u16 read and dropped;
  then the HLSL instance: u32 0xC0DE0005 (K3D_HLSL_Instance::p_CreateFromBuffer; anything else = no instance), then
  pc_LoadFromBuffer loops over tag words (full 32-bit compares) until 0xC0DE0006, skipping any word that is no tag.
  The word right after the magic is such a word (`legacy_size`: in all 53 records the byte count of the chunks before
  the FX-type chunk, not counting the count word of the var lists; re-emitted by that rule).  Chunks:
    0xC0DE0007 model: u32 size (8), u32 hlsl_key, u32 technique (low byte -> mu8_Technique), u32 0xC0DE0008
    0xC0DE0010 engine shader (not in RGH data): u32 size, u32 name length, the name, u32 technique, u32 0xC0DE0011
    0xC0DE0009 var buffer: u32 size, that many bytes (mpc_Var), 0xC0DE000A
    0xC0DE000B save vars / 0xC0DE0012 model vars: u32 size (bytes after the count through the list end tag), u32
      count (mu32_SaveVarNumber / mu32_SaveModelVarNumber), entries 0xC0DE000D / 0xC0DE0014 {u32 size (bytes after it
      through the entry end tag), u8 type, u8 flags, u16 data size, u32 name length, data, name with its NUL,
      0xC0DE000E / 0xC0DE0015} up to 0xC0DE000C / 0xC0DE0013.
    0xC0DE0016 FX type: u32 fx_type (mu32_FXType, 0 -> 10); no end tag.
  After the end tag the loader calls LOA_MakeFileRef(0, 0, p_Callback_ResolveInstance, this, 1): no reads.
  Data: chunks 7, 9, 0xB, 0x12, 0x16 (52; one without 0xB); FX types 10 (39), 4 (6), 3 (5), 2 (2), 1 (1); var types 3,
  4, 5, 6, 11, 255 - parameters only, no var word is a record key.
  Keys: the chunk-7 model key -> "hlsl" (read and ignored by RGH; 6 distinct keys 0x47xxxxxx, records in the packages).

RDP (40) - MDF version 1, render params version 9 in all records: the MDF header then ViD::pu32_LoadRenderParam
  (stream.render_params), nothing else.  The params are copied after EndLoad when MDF flag 0x10000 is set.

MGM (48) - MDF version 1 in all records.  version >= 1: u32 count, count x {u32 key (stored in mo_PackageKeyList and
  passed to LOA_MakeFileRef(MGF::p_Callback_LoadMGF) into mo_MGFList), u32 u1 (read and dropped)}.  Worlds:
  _basic_Photo&Mail, _basic_InGame, _basic_global, _Map_Language, IZW, IZW_TRC, _basic_tuto, CREDITS, plus the
  packages of main_title (Common + main_title), Hub and Hud_endlevel_02.
  Keys: MGF records of the same package -> "mgf"; 0x251015C4 (_basic_Photo&Mail) and 0x0E0076C2 (_basic_global) are
  not in the Wii archive.

MGF (Magma file descriptor) - version 4 in all records.
  u16 version (mu16_CurrVersion), u32 n_platforms, u32 n_languages, version > 1: char[64] name (sz_Name, e.g.
  "Photo.mgb"); n_platforms x {i32 index, u32 platform} (m_supportedPlatForms); n_languages x {i32 index, u32
  language} (m_supportedLanguages); n_platforms x n_languages u32 package_keys (tab_PackageKeys[platform][language]);
  then lists {u32 count, count x {u32 key, char[64] name}}: textures (m_DependenceTextures ->
  K3D_texture::p_Callback_LoadTexture), fonts (m_DependenceFont -> MGB::p_Callback_LoadMGB, user 2), packages
  (m_DependencePackages -> MGF::p_Callback_LoadMGF), version > 2: textures_alpha (m_DependenceTexturesAlpha, as
  textures, key 0 = none); version > 3: u32 localized (mb_Localized).
  - package_keys: the UI binaries ("MAGMA" blobs: pages, areas, widgets, strings), one per platform x language (2 x 10;
    language ids 0, 1, 2, 3, 5, 6, 7, 9, 20, 12).  MGF::LoadPackage picks one (GetPackageFromID), BIG_BinInit,
    LOA_MakeFileRef(MGB::p_Callback_LoadMGB, user 1): one raw buffer handed to MGM_Manager::LoadPackage.  In the
    archive they are the loose bin files mgb_bin_key(key) = 0xFBF00000 | (key & 0xFFFFF) (`fbfXXXXX.bin`, 41 files:
    u32 original key, u32 size - 8, u32 size - 12, the "MAGMA" blob).  Only platform index 1 (Wii) at language indexes
    0, 1 and 9 exists; the other 17 keys of each MGF are absent.
  - textures / textures_alpha: ordinary K3D textures of the packages (colour + alpha).
  - fonts: .mft binaries (Common.mgb, InZeWiimoteTRC.mgb), not in the archive.
  - packages: other MGF records; every UI file depends on Common.mgb, InZeWiimote on Tutorial_2, InZeWiimoteTRC on
    Tutorial_FullScreen.
  UI files: Common, main_title, Photo, InGame, language, InZeWiimote, InZeWiimoteTRC, Tutorial, Tutorial_2,
  Tutorial_FullScreen, Hub, Hud_endlevel_02, Credits.

BVO (46) - one record (_basic_AsyncLoading.wol, MDF version 4).
  Right after the MDF header the loader calls mpo_Bink->pu32_LoadFromBuffer through the vtable: K3D_texpro_bink: u32
  version, u8 flags (mu8_Flags), u8 read_mode (mu8_ReadMode), u8 dummy0 and u8 dummy (both into mu8_Dummy), version != 0:
  u8 count + count u32 keys (mah_BinkList), version 0: exactly one key; mh_BinkFile = the first key.  Then f32
  offset_x, offset_y (mt_Offset), f32 scale_x, scale_y (mt_Scale), u8 size_mode, version < 1: u8 dropped, u8 viewports,
  version >= 1: u32 obj_file (mh_ObjFile), u32 eve_rank; version >= 2: u8 play_on_foreground; version >= 3: u32 count
  + keys (mo_BinkKeyList; version < 3 takes the texpro's first key instead); version >= 4: u8 run_video_at_init.  RGH
  stops at version 4.  No key goes through LOA_MakeFileRef: the videos are streamed from the bigfile by key.
  Keys: modifier bink keys -> "video" (65, every one a named .bik loose file: intro_RGH, outro_RGH, PAL_UbiLogo,
  Bink_Mission_*, Bink_XL_*, CineGAG_*, loading_bink3s, ...; the texpro's own single key is 0), obj_file -> "object"
  (0 in RGH).

IK3 (12), LOO (16) - no record anywhere in the archive (extended and plain MDF headers of every package record), so
  nothing is implemented.  The reads, no version gates:
  IK3: u32 mpo_ObjUp, u32 mpo_ObjDown (object keys), u32 mu32_Flags, f32 mf32_DistScale.  LOO: u8 mu8_LookWhat, u8
  mu8_Type, u8 mu8_Orient, u8 mu8_Flags, u32 mpo_LookTo (object key), u32 mu32_ApplyAlwaysAble, u8
  mu8_ToleranceOrient, f32 mf32_HieMaxAngleLimit, f32 mf32_HieMaxAngleTolerance, f32 mf32_MaxAngleDeadZone, f32
  mf32_BlendRotateRate, v3 mv_TargetOffset.
"""
from __future__ import annotations

from . import stream as F
from .stream import FormatError

NET_TYPE = 5
IK3_TYPE = 12
LOO_TYPE = 16
AFX_TYPE = 29
RDP_TYPE = 40
BVO_TYPE = 46
MGM_TYPE = 48

INVALID_KEY = 0xFFFFFFFF


# ----------------------------------------------------------------------------
# helpers
def _list(st, o, name, count_name, fn="u32"):
    """A counted list: the count comes from the content when writing."""
    lst = o.setdefault(name, [])
    if st.writing:
        o[count_name] = len(lst)
    n = getattr(st, fn)(o, count_name)
    return lst, n


def _item(st, lst, i):
    if st.writing:
        return lst[i]
    it = {}
    lst.append(it)
    return it


def _const(st, o, name, value):
    """A word that must hold `value` (a tag); written from the constant."""
    if st.writing:
        o[name] = value
    v = st.u32(o, name)
    if v != value:
        raise FormatError(f"{name}: {v:#x}, expected {value:#x}")
    return v


def _tag(pairs):
    """(key, kind) pairs without null / invalid keys and duplicates."""
    out, seen = [], set()
    for k, kind in pairs:
        if k and k != INVALID_KEY and (k, kind) not in seen:
            seen.add((k, kind))
            out.append((k, kind))
    return out


# ============================================================================
# NET::p_Callback_LoadNetwork
# ============================================================================
def net(st, o):
    """RGH <= 2."""
    st.memsize(o)
    F.mdf_header(st, o)
    ver = o["version"]
    if ver > 2:
        raise FormatError(f"NET version {ver}")
    nodes, n = _list(st, o, "nodes", "n_nodes")
    for i in range(n):
        nd = _item(st, nodes, i)
        st.u32(nd, "obj")                       # NET_tt_Node_::po_Obj (object key)
        links, m = _list(st, nd, "links", "n_links")
        for k in range(m):
            lk = _item(st, links, k)
            st.u32(lk, "next")                  # NET_tt_Link_::u32_Next (node index)
            st.u32(lk, "capacities_init")       # u32_CapacitiesInit (-> u32_Capacities too)
    st.u32(o, "merge_id")                       # mu32_MergeID
    st.u32(o, "u7")                             # read and dropped
    if ver >= 1:
        st.u32(o, "design_init")                # mu32_DesignInit
    if ver >= 2:
        st.u32(o, "m1_u9", 1)                   # absent in packages
    return o


def refs_net(o) -> list[tuple[int, str]]:
    """The node objects (resolved by NET::AfterLoading, not LOA_MakeFileRef: they must be objects the world loads)."""
    return _tag([(nd.get("obj", 0), "object") for nd in o.get("nodes", [])])


# ============================================================================
# K3D_HLSL_Instance (the AFX payload)
# ============================================================================
HLSL_MAGIC = 0xC0DE0005
HLSL_END = 0xC0DE0006
HLSL_MODEL = 0xC0DE0007          # ends with 0xC0DE0008
HLSL_VARDATA = 0xC0DE0009        # ends with 0xC0DE000A
HLSL_SAVEVARS = 0xC0DE000B       # entries 0xC0DE000D..0xC0DE000E, ends with 0xC0DE000C
HLSL_ENGINESHADER = 0xC0DE0010   # ends with 0xC0DE0011
HLSL_MODELVARS = 0xC0DE0012      # entries 0xC0DE0014..0xC0DE0015, ends with 0xC0DE0013
HLSL_FXTYPE = 0xC0DE0016         # no end tag


def hlsl_var(st, v, end_tag):
    """One K3D_HLSL_tt_StoreVar after its entry tag: size of what follows, u8_Type, u8_Flags, u16_Size, name length, the
    data, the name (with its NUL), the entry end tag."""
    data = v.get("data", b"")
    name = v.get("name", b"")
    if st.writing:
        v["data_size"] = len(data)
        v["name_len"] = len(name)
        v["size"] = 8 + len(data) + len(name) + 4
    st.u32(v, "size")
    st.u8(v, "type")                            # u8_Type
    st.u8(v, "flags")                           # u8_Flags
    ds = st.u16(v, "data_size")                 # u16_Size
    nl = st.u32(v, "name_len")
    st.raw(v, "data", ds)                       # pc_Data
    st.raw(v, "name", nl)                       # sz_Name
    _const(st, v, "end", end_tag)


def _hlsl_varlist(st, c, entry_tag, entry_end, end_tag):
    vars_ = c.setdefault("vars", [])
    if st.writing:
        c["count"] = len(vars_)
        c["size"] = sum(8 + 8 + len(v.get("data", b"")) + len(v.get("name", b"")) + 4
                        for v in vars_) + 4
    st.u32(c, "size")                           # bytes after the count through the end tag
    n = st.u32(c, "count")                      # mu32_SaveVarNumber / mu32_SaveModelVarNumber
    if st.writing:
        for v in vars_:
            _const(st, v, "tag", entry_tag)
            hlsl_var(st, v, entry_end)
        _const(st, c, "end", end_tag)
        return
    i = 0
    while True:
        v = {}
        tag = st.u32(v, "tag")
        if tag == end_tag:
            c["end"] = tag
            break
        if tag != entry_tag:
            raise FormatError(f"HLSL var list: unexpected word {tag:#x}")
        if i >= n:
            raise FormatError("HLSL var list: more entries than its count")
        hlsl_var(st, v, entry_end)
        vars_.append(v)
        i += 1


def hlsl_chunk(st, c):
    tag = c["tag"]
    if tag == HLSL_MODEL:
        if st.writing:
            c["size"] = 8
        st.u32(c, "size")
        st.u32(c, "hlsl_key")                   # read and ignored by the loader
        st.u32(c, "technique")                  # low byte -> mu8_Technique
        _const(st, c, "end", HLSL_MODEL + 1)
    elif tag == HLSL_ENGINESHADER:
        name = c.get("name", b"")
        if st.writing:
            c["name_len"] = len(name)
            c.setdefault("size", 4 + len(name) + 4)
        st.u32(c, "size")
        nl = st.u32(c, "name_len")
        st.raw(c, "name", nl)
        st.u32(c, "technique")
        _const(st, c, "end", HLSL_ENGINESHADER + 1)
    elif tag == HLSL_VARDATA:
        data = c.get("data", b"")
        if st.writing:
            c["size"] = len(data)
        n = st.u32(c, "size")                   # mpc_Var buffer size
        st.raw(c, "data", n)
        _const(st, c, "end", HLSL_VARDATA + 1)
    elif tag == HLSL_SAVEVARS:
        _hlsl_varlist(st, c, 0xC0DE000D, 0xC0DE000E, 0xC0DE000C)
    elif tag == HLSL_MODELVARS:
        _hlsl_varlist(st, c, 0xC0DE0014, 0xC0DE0015, 0xC0DE0013)
    elif tag == HLSL_FXTYPE:
        st.u32(c, "fx_type")                    # mu32_FXType (0 -> 10 in the loaders)
    else:
        raise FormatError(f"HLSL instance: unknown chunk {tag:#x}")


def _chunk_bytes(st, c):
    w = F.Writer(st.level, st.binary)
    w.u32(c, "tag")
    hlsl_chunk(w, c)
    return len(w.buf)


def hlsl_instance(st, h):
    """K3D_HLSL_Instance::p_CreateFromBuffer + pc_LoadFromBuffer: the 0xC0DE0005 magic, the word the loader skips, then
    tagged chunks up to 0xC0DE0006."""
    _const(st, h, "magic", HLSL_MAGIC)
    chunks = h.setdefault("chunks", [])
    if st.writing:
        # the word every RGH record carries here: the bytes of the chunks before the FX-type chunk, not counting the
        # count word of the var list chunks.  No loader uses it (not a tag: skipped).
        n = 0
        for c in chunks:
            if c["tag"] == HLSL_FXTYPE:
                break
            n += _chunk_bytes(st, c) - (4 if c["tag"] in (HLSL_SAVEVARS, HLSL_MODELVARS) else 0)
        h["legacy_size"] = n
    st.u32(h, "legacy_size")
    if st.writing:
        for c in chunks:
            st.u32(c, "tag")
            hlsl_chunk(st, c)
        _const(st, h, "end", HLSL_END)
        return
    while True:
        c = {}
        tag = st.u32(c, "tag")
        if tag == HLSL_END:
            h["end"] = tag
            break
        hlsl_chunk(st, c)
        chunks.append(c)


def hlsl_chunk_of(h, tag):
    for c in h.get("chunks", []):
        if c.get("tag") == tag:
            return c
    return None


# ============================================================================
# AFX::p_Callback_LoadAFX
# ============================================================================
def afx(st, o):
    st.memsize(o)
    F.mdf_header(st, o)
    ver = o["version"]
    if ver >= 2:
        st.u16(o, "mode")                       # mu16_Mode
        if ver >= 3:
            st.u8(o, "viewports")               # mu8_Viewports
            st.u8(o, "dummy")                   # mu8_Dummy
        else:
            st.u16(o, "v2_u16")                 # read and dropped
    hlsl_instance(st, o.setdefault("hlsl", {}))
    return o


def refs_afx(o) -> list[tuple[int, str]]:
    """The HLSL model key of chunk 7 (read and ignored by the loader)."""
    c = hlsl_chunk_of(o.get("hlsl", {}), HLSL_MODEL)
    return _tag([(c.get("hlsl_key", 0), "hlsl")]) if c else []


# ============================================================================
# RDP::p_Callback_LoadRDP
# ============================================================================
def rdp(st, o):
    st.memsize(o)
    F.mdf_header(st, o)
    F.render_params(st, o.setdefault("render_params", {}))
    return o


def refs_rdp(o) -> list[tuple[int, str]]:
    return []


# ============================================================================
# MGM::p_Callback_LoadMGM
# ============================================================================
def mgm(st, o):
    st.memsize(o)
    F.mdf_header(st, o)
    if o["version"] >= 1:
        files, n = _list(st, o, "mgf", "n_mgf")
        for i in range(n):
            f = _item(st, files, i)
            st.u32(f, "key")                    # mo_PackageKeyList; LOA_MakeFileRef(MGF::p_Callback_LoadMGF)
            st.u32(f, "u1")                     # read and dropped
    return o


def refs_mgm(o) -> list[tuple[int, str]]:
    return _tag([(f.get("key", 0), "mgf") for f in o.get("mgf", [])])


# ============================================================================
# MGF::p_Callback_LoadMGF  (a resource MGM loads, no MDF header)
# ============================================================================
MGF_LISTS = (("textures", "texture"),           # m_DependenceTextures -> K3D_texture::p_Callback_LoadTexture
             ("fonts", "mgb_font"),             # m_DependenceFont -> MGB::p_Callback_LoadMGB (user 2)
             ("packages", "mgf"),               # m_DependencePackages -> MGF::p_Callback_LoadMGF
             ("textures_alpha", "texture"))     # m_DependenceTexturesAlpha (version > 2)


def _mgf_named(st, o, name):
    lst, n = _list(st, o, name, "n_" + name)
    for i in range(n):
        e = _item(st, lst, i)
        st.u32(e, "key")
        st.raw(e, "name", 64)


def mgf(st, o):
    """RGH ships version 4 (the loader has no upper bound)."""
    st.memsize(o)
    ver = st.u16(o, "version", default=4)       # mu16_CurrVersion
    if ver > 4:
        raise FormatError(f"MGF version {ver}")
    plats = o.setdefault("platforms", [])
    langs = o.setdefault("languages", [])
    if st.writing:
        o["n_platforms"] = len(plats)
        o["n_languages"] = len(langs)
    npl = st.u32(o, "n_platforms")
    nla = st.u32(o, "n_languages")
    if ver > 1:
        st.raw(o, "name", 64)                   # sz_Name ("Photo.mgb")
    for i in range(npl):
        e = _item(st, plats, i)
        st.i32(e, "index")
        st.u32(e, "platform")                   # m_supportedPlatForms
    for i in range(nla):
        e = _item(st, langs, i)
        st.i32(e, "index")
        st.u32(e, "language")                   # m_supportedLanguages
    st.array(o, "package_keys", "<I", npl * nla)    # tab_PackageKeys[platform][language]
    _mgf_named(st, o, "textures")
    _mgf_named(st, o, "fonts")
    _mgf_named(st, o, "packages")
    if ver > 2:
        _mgf_named(st, o, "textures_alpha")
    if ver > 3:
        st.u32(o, "localized")                  # mb_Localized
    return o


def mgb_bin_key(key: int) -> int:
    """Where the archive keeps a Magma package loaded through BIG_BinInit."""
    return 0xFBF00000 | (key & 0xFFFFF)


def mgf_package_key(o, platform_index: int, language_index: int) -> int:
    return o["package_keys"][platform_index * len(o["languages"]) + language_index]


def refs_mgf(o) -> list[tuple[int, str]]:
    """Package binaries ("mgb", in the archive at mgb_bin_key), textures, fonts and the MGF files it depends on."""
    out = [(k, "mgb") for k in o.get("package_keys", [])]
    for name, kind in MGF_LISTS:
        out += [(e.get("key", 0), kind) for e in o.get(name, [])]
    return _tag(out)


# ============================================================================
# BVO::p_Callback_LoadBVO
# ============================================================================
def texpro_bink(st, t):
    """K3D_texpro_bink::pu32_LoadFromBuffer, called through the texpro's vtable right after the MDF header.  A zero
    first word means exactly one key and no count byte."""
    keys = t.setdefault("binks", [])
    if st.writing:
        t.setdefault("version", 1)
        if t["version"] == 0 and len(keys) != 1:
            t["version"] = 1
    ver = st.u32(t, "version")
    st.u8(t, "flags")                                   # mu8_Flags
    st.u8(t, "read_mode")                               # mu8_ReadMode
    st.u8(t, "dummy0")                                  # -> mu8_Dummy, overwritten by the next
    st.u8(t, "dummy")                                   # mu8_Dummy
    if ver != 0:
        if st.writing:
            t["n_binks"] = len(keys)
        n = st.u8(t, "n_binks")
    else:
        n = 1
    st.array(t, "binks", "<I", n)                       # mah_BinkList; [0] -> mh_BinkFile
    return t


def bvo(st, o):
    """RGH <= 4."""
    st.memsize(o)
    F.mdf_header(st, o)
    ver = o["version"]
    if ver > 4:
        raise FormatError(f"BVO version {ver}")
    texpro_bink(st, o.setdefault("texpro", {}))         # mpo_Bink->pu32_LoadFromBuffer
    st.f32(o, "offset_x"); st.f32(o, "offset_y")        # mt_Offset
    st.f32(o, "scale_x"); st.f32(o, "scale_y")          # mt_Scale
    st.u8(o, "size_mode")                               # mu8_SizeMode
    if ver < 1:
        st.u8(o, "v0_u8")                               # read and dropped
    st.u8(o, "viewports")                               # mu8_Viewports
    if ver >= 1:
        st.u32(o, "obj_file")                           # mh_ObjFile (an object key)
        st.u32(o, "eve_rank")                           # mu32_EveRank
    if ver >= 2:
        st.u8(o, "play_on_foreground")                  # mu8_PlayOnForeground
    if ver >= 3:
        binks = o.setdefault("binks", [])
        if st.writing:
            o["n_binks"] = len(binks)
        n = st.u32(o, "n_binks")
        st.array(o, "binks", "<I", n)                   # mo_BinkKeyList (.bik keys)
    if ver >= 4:
        st.u8(o, "run_video_at_init")                   # mu8_RunVideoAtInit
    return o


def refs_bvo(o) -> list[tuple[int, str]]:
    """Bink keys of the texpro and of the modifier ("video": loose .bik files streamed by key, no LOA_MakeFileRef), the
    object."""
    out = [(k, "video") for k in o.get("texpro", {}).get("binks", [])]
    out += [(k, "video") for k in o.get("binks", [])]
    out.append((o.get("obj_file", 0), "object"))
    return _tag(out)


# ----------------------------------------------------------------------------
STREAMS = {NET_TYPE: net, AFX_TYPE: afx, RDP_TYPE: rdp, MGM_TYPE: mgm, BVO_TYPE: bvo}
REFS = {NET_TYPE: refs_net, AFX_TYPE: refs_afx, RDP_TYPE: refs_rdp, MGM_TYPE: refs_mgm,
        BVO_TYPE: refs_bvo}
NAMES = {NET_TYPE: "net", AFX_TYPE: "afx", RDP_TYPE: "rdp", MGM_TYPE: "mgm", BVO_TYPE: "bvo"}
RESOURCES = {"mgf": (mgf, refs_mgf)}                    # kind -> (stream, refs)
