"""Sound library (SLib) resources: the sound banks SNK / SNF modifiers point at and the SLib files a bank references.

Loaders (RGH Wii executable) and the jump tables of their chunk switches:

  bank  SLib::p_Callback_LoadSoundBank (RGH 80267FE4), SLib_tt_SoundBank::u_LoadReferences 8026830C.
  file  SLib::p_Callback_LoadFile (RGH 80281E64) -> SLib_xxx::b_Create by the reference's user word:
          1 SLib_file_S (RAM sample)  2 SLib_file_S (streamed sample)  3 SLib_set
          5 SLib_smx  6 SLib_mic  7 SLib_smp  8 SLib_sound_variable_file
  set   SLib_set::b_Create (RGH 80274274) -> pu8_LoadFromBuffer (RGH 80274C14, tags 0..0x18, table 8055FBC0),
        pu8_LoadFromBufferV1 for versions <= 1 (RGH 802755E4), then LoadReferences (RGH 80275A9C).
        SLib_insert::new_insert (RGH 80252D84, table 8055F1D8):
          1 pitch 2 doppler 3 fadein 4 fadeout 5 pan 6 3dVolume 7 gain
          8..0x17 user 0x18 filter 0x19 lfe 0x1A 3dCone 0x1B dirmic
        SLib_insert_<x>::Load(cursor, version):
          lfe 80256B90  pan 80256EFC  gain 80256894  user 80258B74  pitch 80258798  3dCone 8026E664
          dirmic 80277494  fadein 802554B0  filter 802560F8  doppler 80254B84  fadeout 80255A5C  3dVolume 802533E8
        SLib_aux_send::pu8_LoadFromBuffer (RGH 8024FE04)
  smp   SLib_smp::b_Create (RGH 80272438) -> pu8_LoadFromBuffer (RGH 80272580).
  smx   SLib_smx::b_Create (RGH 80266674) -> pu8_LoadFromBuffer (RGH 802666F8, table 8055F758).
        SLib_group::LoadFromBuffer (RGH 8026BB60), SLib_ginsert::new_insert (RGH 8026BE78: 0 pan 1 filter
        2 3dvolume), SLib_ginsert_pan/filter/3dvolume (RGH 8026BFE8/8026C24C/80274104).
        SLib_aux::new_aux (RGH 8024C6C4: 0 echo 1 reverb 2 equalizer 3 nofx 4 agccompressor 5 autowah 6 chorus
        7 distortion 8 flanger 9 freqshifter 10 vocalmorpher 11 pitchshifter 12 ringmodulator), SLib_aux::pu8_Load
        (RGH 8024CA50), SLib_aux_user::pu8_Load (RGH 80275D80), SLib_aux_echo::pu8_Load (RGH 8024CFBC, table
        8055EF20), SLib_aux_reverb::pu8_Load (RGH 8024ED70, table 8055EFE8), SLib_aux_type_supported
        <reverb_eax/x360/ps3, echo_ps3> (RGH 8024DFB4 / 8024E4D8 / 8024E974 / 8024CD90).
  mic   SLib_mic::b_Create (RGH 8025CF60) -> pu8_LoadFromBuffer (RGH 8025CFE4).
  sva   SLib_sound_variable_file::b_Create (RGH 80282BF8) -> pu8_LoadFromBuffer (RGH 80282C7C);
        SLib_sound_variable::pu_LoadFromBuffer (RGH 8028269C).
  wave  SLib_file::b_Create (RGH 80251E5C) -> SLib_wave::b_Create (RGH 80268B64): a RIFF chunk walk whose chunk
        contents the Wii does not need are mode 1; SLib_file::b_LoadBinData (RGH 8025218C) reads the sample bytes from
        the package's sound bin.

Record layouts in a package (level 3, binary): every data block is preceded by its size -- the size of the loose-file
(level 0) data, e.g. 4 + 128 n for a bank, the .sns file size for a sample, 0x19000 for a streamed sample -- and a
size of 0 means the block is absent.
  bank  u32 n + n x {key, priv, user} (entry i = reference i), memsize, [u32 version (0), mode-1 char[128] name per
        entry]
  set   memsize, data, then (only when mu_FileCount != 0) the reference list
  smp   memsize, data, then (only when the file count != 0) the reference list
  smx / mic / sva / wave   memsize, data
In a loose file the reference lists are the file's reference table.

What RGH ships (every record reached from the SNK / SNF / SNL modifiers and the Txl sound keys): banks; sets version 1
(V1 layout), 3, 4, 5, 6, 7; smp 0..5; smx 0x1001 / 0x1004 with echo, reverb, nofx and chorus aux effects; inserts
pitch 0, fadein / fadeout 1, doppler 2, filter 1 / 2, 3dCone 2, 3dVolume 1 / 2 / 3, pan 0x1003 / 0x1004, user
0x1001 / 0x1003; samples in format 0x5050 with fmt, fact [, cue, LIST adtl (ltxt, labl)], data.  No mic or
variable-file record is in any package; the archive keeps Mic_config_Bunnies.mic (version 0x1005) and
Mix_config_Bunnies.smx as loose files.  Reference tables: entries whose priv word has 0x10000000 are not files (a set
lists its curves that way, with arbitrary user words); the bank, set and smp loaders skip them and
SLib_set::LoadReferences also skips user word 3.  A bank entry with key 0 (user word 0) is an empty slot.  Sample records
of streamed samples (user word 2) end with a 40-byte link {u32 3, u32 .sns size, char[32] ".wii.sns"...} the BIG layer
resolves the stream through (not a Priv_Read); it only exists in packages.

Keys through LOA_MakeFileRef (the refs_* helpers):
  bank  every reference (SLib::p_Callback_LoadFile, typed by the user word)
  set   its reference list (children), chunk 5 mpo_Model (user 3, another set), insert and aux-send curve keys
        (MTH_p_Callback_CurveLoad)
  smp   its reference list (children, SLib::p_Callback_LoadFile)
  smx   chunk 4/5/7 keys, aux sends, inserts, aux users: MTH curves
  mic   in/out curve keys and per-micro curve keys: MTH curves

Localized sounds: the Txl entries' sound keys name sets (and one sample) that live in the language bins (089XXXXX.bin,
text.LangData), not in the world packages; their samples are there too, with their bytes after the records
(TXG_LoadLangData2) rather than in the package sound bins.

Every record parses consuming its length and re-emits identically at level 3: bank 893, set 7810 (2198 in language
bins), smp 485, smx 271, sample 9258 (2196 in language bins; 6469 in RAM with user word 1, 2789 streamed with user word
2).
"""
from __future__ import annotations

import struct

from .stream import FormatError


def _cc(s: bytes) -> int:
    return struct.unpack("<I", s)[0]


RIFF, WAVE, FMT, FACT, CUE, LIST, ADTL, LABL, LTXT, DATA = (
    _cc(b"RIFF"), _cc(b"WAVE"), _cc(b"fmt "), _cc(b"fact"), _cc(b"cue "), _cc(b"LIST"),
    _cc(b"adtl"), _cc(b"labl"), _cc(b"ltxt"), _cc(b"data"))

MAX_HEADER = 0x19000              # SLib_wave::b_Create reads at most this many header bytes
SET_LANGUAGE = 0                  # SLib::u_GetCurrentLanguage when a set chunk 0x17 is binarised


# ----------------------------------------------------------------------------
# helpers
def _count(st, o, name, value, fn="u32"):
    """A count: derived from the content when writing."""
    if st.writing:
        o[name] = value
    return getattr(st, fn)(o, name)


def _items(st, o, name, n):
    lst = o.setdefault(name, [])
    if st.writing:
        while len(lst) < n:
            lst.append({})
    for i in range(n):
        if not st.writing:
            lst.append({})
        yield lst[i]


def _seq(st, o, spec, prefix, mode=2):
    """Fields by a type string: i i32, u u32, f f32, b u8, h u16, s i16."""
    fn = {"i": st.i32, "u": st.u32, "f": st.f32, "b": st.u8, "h": st.u16, "s": st.i16}
    for i, c in enumerate(spec):
        fn[c](o, f"{prefix}{i}", mode)


def _block(st, o):
    """LOA_p_LoadRef(b_Refs=0): a package data block starts with its size and a size of 0 means no data; an empty
    loose file has none either."""
    if st.binary:
        has = st.memsize(o) != 0
    elif st.writing:
        has = bool(o.get("has_data", True))
    else:
        has = st.remaining() > 0
    o["has_data"] = has
    return has


def _refs(st, o, present=True):
    """A reference list: inline in packages only when the loader reads it, always the reference table in loose files."""
    if st.binary and not present:
        o.setdefault("refs", [])
        return o["refs"]
    return st.refs(o, "refs")


def _tagword(st, c, lo, hi):
    """A u32 whose low half is a type / index and high half a version."""
    if st.writing and "tag_word" not in c:
        c["tag_word"] = (c.get(lo, 0) & 0xFFFF) | ((c.get(hi, 0) & 0xFFFF) << 16)
    w = st.u32(c, "tag_word")
    c[lo], c[hi] = w & 0xFFFF, w >> 16
    return w


def _next(st, lst, i):
    if st.writing:
        return lst[i]
    d = {}
    lst.append(d)
    return d


# ----------------------------------------------------------------------------
# bank
def bank(st, o):
    """SLib::p_Callback_LoadSoundBank: the reference list (bank entry i = reference i, an SLib file typed by its user
    word), then the data block: u32 version (must be 0) and one 128-byte name per entry (loose files only)."""
    refs = st.refs(o, "refs")
    if not _block(st, o):
        return o
    if st.u32(o, "version") != 0:
        raise FormatError(f"sound bank version {o['version']}")
    for it in _items(st, o, "names", len(refs)):
        st.raw(it, "name", 128, 1)
    return o


def refs_bank(o):
    return [r.key for r in o.get("refs", []) if r.key]


# ----------------------------------------------------------------------------
# inserts (set chunk 0x12, V1 insert list, smx chunk 2)
def _ins_pitch(st, o, ver):
    _seq(st, o, "fff", "f")


def _ins_doppler(st, o, ver):
    st.f32(o, "factor"); st.f32(o, "f14")
    if ver >= 1:
        st.f32(o, "f10"); st.u32(o, "curve0"); st.u32(o, "curve1")


def _ins_fade(st, o, ver):
    st.u32(o, "curve")
    if ver >= 1:
        st.f32(o, "duration")


def _ins_pan(st, o, ver):
    if ver < 0x1000:
        st.u32(o, "u0")
        for it in _items(st, o, "sides", 2):
            _seq(st, it, "ffff", "f")
        st.f32(o, "f70"); st.f32(o, "f74")
        return
    st.u32(o, "u0")
    if ver < 0x1004:
        n = _count(st, o, "n_channels", o.get("n_channels", 8))
    else:
        n = 8
    for it in _items(st, o, "sides", 2):
        vals = it.setdefault("channel", [])
        if st.writing:
            while len(vals) < n:
                vals.append(0.0)
        for i in range(n):
            v = st.f32({"v": vals[i] if st.writing else 0.0}, "v")
            if not st.writing:
                vals.append(v)
        _seq(st, it, "ffff", "f")
    st.f32(o, "f70"); st.f32(o, "f74")
    if ver >= 0x1001:
        if ver < 0x1004:
            st.u32(o, "n_channels2")
        st.array(o, "channel_map", "<I", n)
        if ver == 0x1002:
            for it in _items(st, o, "v1002", n):
                st.array(it, "a7", "<i", 7)
                st.array(it, "a5", "<i", 5)
        if ver >= 0x1003:
            for it in _items(st, o, "levels", n):
                st.array(it, "a5", "<i", 5)


def _ins_3dvolume(st, o, ver):
    st.u32(o, "flags")
    st.array(o, "f10", "<f", 9)
    st.array(o, "f40", "<f", 10)
    if ver >= 1:
        if ver < 2:
            st.u16(o, "v1_a"); st.u16(o, "v1_b")
        else:
            st.u32(o, "u8")
        if ver >= 3:
            st.array(o, "curves", "<I", 3)


def _ins_gain(st, o, ver):
    st.u32(o, "gain")


def _ins_user(st, o, ver):
    st.u32(o, "flags")
    st.u32(o, "curve")
    if ver >= 0x1000:
        st.f32(o, "f14"); st.f32(o, "f18")
    if ver >= 0x1001:
        st.f32(o, "m1_f", 1)
        st.raw(o, "m1_name", 512, 1)
    if ver >= 0x1003:
        st.u32(o, "u8")


def _ins_filter(st, o, ver):
    st.u32(o, "gain")
    st.u32(o, "curve")
    if ver >= 2:
        st.u32(o, "u8"); st.u32(o, "u10")


def _ins_lfe(st, o, ver):
    st.u32(o, "level")
    if ver >= 1:
        st.u32(o, "u10"); st.u32(o, "curve")


def _ins_3dcone(st, o, ver):
    st.f32(o, "m1_f0", 1); st.f32(o, "m1_f1", 1)
    st.f32(o, "f10"); st.f32(o, "f18")
    st.u32(o, "curve0"); st.u32(o, "curve1")
    st.v3(o, "axis")
    if ver < 2:
        st.u16(o, "v1_a"); st.u16(o, "v1_b")
    else:
        st.u32(o, "u8")


def _ins_dirmic(st, o, ver):
    pass


INSERT_TYPES = {1: ("pitch", _ins_pitch), 2: ("doppler", _ins_doppler), 3: ("fadein", _ins_fade),
                4: ("fadeout", _ins_fade), 5: ("pan", _ins_pan), 6: ("3dVolume", _ins_3dvolume),
                7: ("gain", _ins_gain), 0x18: ("filter", _ins_filter), 0x19: ("lfe", _ins_lfe),
                0x1A: ("3dCone", _ins_3dcone), 0x1B: ("dirmic", _ins_dirmic)}
INSERT_TYPES.update({t: ("user", _ins_user) for t in range(8, 0x18)})


def _insert_payload(st, ins, typ, ver):
    """SLib_insert::new_insert(type)->Load(cursor, ver); unknown types have no object and no payload."""
    ent = INSERT_TYPES.get(typ)
    if ent is None:
        return
    ins["kind"] = ent[0]
    ent[1](st, ins, ver)


def insert_curve_keys(ins):
    out = [ins.get(k, 0) for k in ("curve", "curve0", "curve1")]
    out += list(ins.get("curves", []))
    return [k for k in out if k]


# ----------------------------------------------------------------------------
# aux sends, aux effects, groups
def aux_send(st, o):
    """SLib_aux_send::pu8_LoadFromBuffer."""
    ver = st.u32(o, "version")
    st.u32(o, "flags")
    st.i32(o, "level")
    if ver < 2:
        st.i32(o, "v1_level2")
    st.u32(o, "curve")
    if ver < 2:
        st.u32(o, "v1_u")
    return o


def aux_user(st, o):
    """SLib_aux_user::pu8_Load."""
    ver = st.u32(o, "version")
    if ver > 2:
        raise FormatError(f"aux user version {ver}")
    st.u32(o, "u4")
    _seq(st, o, "fff", "f")
    st.u32(o, "u0")
    st.u32(o, "curve")
    st.raw(o, "m1_name", 256, 1)
    if ver >= 2:
        st.u32(o, "u18")
    return o


def _aux_base(st, o):
    """SLib_aux::pu8_Load: u32 count + aux users."""
    users = o.setdefault("users", [])
    n = _count(st, o, "n_users", len(users))
    for u in _items(st, o, "users", n):
        aux_user(st, u)


def _flagged(st, o, head_mode, spec, tail, mode):
    fl = st.u32(o, "flag", head_mode)
    _seq(st, o, spec, "f", mode)
    if fl:
        _seq(st, o, tail, "t", mode)


ECHO_CASES = {
    0: lambda st, c: _seq(st, c, "iufiuu" + "f" * 6, "v"),
    1: lambda st, c: _seq(st, c, "i" + "f" * 5, "v"),
    2: _aux_base,
    3: lambda st, c: _flagged(st, c, 2, "f" * 5, "i", 1),                      # eax
    4: lambda st, c: _flagged(st, c, 2, "ffff", "i", 2),                       # wii
    5: lambda st, c: st.i32(c, "v"),
    6: lambda st, c: (st.u32(c, "version"), _seq(st, c, "ifff" * 2 + "iffffff" * 4 + "ffi", "m1_", 1)),  # ps3
    7: lambda st, c: (st.u32(c, "version"), _seq(st, c, "iff", "m1_", 1)),     # x360
}


def _reverb_x360(st, c):
    v = st.u32(c, "version")
    if v < 2:
        _seq(st, c, "u" + "b" * 12 + "f" * 8 + ("i" if v else ""), "m1_", 1)
    else:
        _seq(st, c, "i" + "f" * 11 + "b" * 7 + "f", "m1_", 1)


def _reverb_ps3(st, c):
    v = st.u32(c, "version")
    if v >= 2:
        _seq(st, c, "i" + "f" * 11 + "ifii" + "fff", "m1_", 1)
    else:
        _seq(st, c, "f" * 11 + ("i" if v >= 1 else ""), "m1_", 1)


REVERB_CASES = {
    0: lambda st, c: _seq(st, c, "iufuffiiifffiffffi" + "f" * 12 + "u", "v"),
    1: lambda st, c: _seq(st, c, "iuffiiifffiffffi" + "f" * 12 + "u", "v"),
    2: _aux_base,
    3: lambda st, c: _flagged(st, c, 2, "uffiiifffiffffi" + "f" * 12 + "u", "i", 1),   # eax
    4: lambda st, c: _flagged(st, c, 2, "uffu" + "f" * 6, "i", 2),                     # wii
    5: _reverb_x360,
    6: _reverb_ps3,
    7: lambda st, c: st.i32(c, "v"),
}
CHORUS_CASES = {   # SLib_aux_chorus::pu8_Load (RGH 8026CC8C, a compare chain)
    0: lambda st, c: _seq(st, c, "iiufuiffff", "v"),
    1: lambda st, c: _seq(st, c, "iuiffff", "v"),
    2: _aux_base,
    3: lambda st, c: _flagged(st, c, 2, "uiffff", "i", 1),                    # eax
    4: lambda st, c: _flagged(st, c, 2, "ffff", "i", 2),                      # wii
    5: lambda st, c: st.i32(c, "v"),
}
AUX_TYPES = {0: ("echo", ECHO_CASES), 1: ("reverb", REVERB_CASES), 3: ("nofx", None), 6: ("chorus", CHORUS_CASES)}
AUX_NAMES = ["echo", "reverb", "equalizer", "nofx", "agccompressor", "autowah", "chorus", "distortion",
             "flanger", "freqshifter", "vocalmorpher", "pitchshifter", "ringmodulator"]


def aux_effect(st, o, typ):
    """SLib_aux::new_aux(type)->pu8_Load: u32 tags 0xEA50..0xEA57 (platform parameter blocks) until 0xFFFFFFFF."""
    if typ not in AUX_TYPES:
        if 0 <= typ < len(AUX_NAMES):
            raise FormatError(f"aux effect {AUX_NAMES[typ]} not handled (not shipped by RGH)")
        return
    name, cases = AUX_TYPES[typ]
    o["kind"] = name
    if cases is None:
        return
    lst = o.setdefault("blocks", [])
    i = 0
    while True:
        if st.writing and i >= len(lst):
            st.u32({"t": 0xFFFFFFFF}, "t")
            break
        c = _next(st, lst, i)
        i += 1
        t = st.u32(c, "tag")
        if t == 0xFFFFFFFF:
            if not st.writing:
                lst.pop()
            break
        fn = cases.get((t - 0xEA50) & 0xFFFFFFFF)
        if fn:
            fn(st, c)


def aux_curve_keys(o):
    out = []
    for c in o.get("blocks", []):
        out += [u.get("curve", 0) for u in c.get("users", [])]
    return [k for k in out if k]


def _ginsert(st, o, typ):
    """SLib_ginsert::new_insert (0 pan, 1 filter, 2 3dvolume)->pu_LoadFromBuffer."""
    if typ == 0:
        if st.u32(o, "version") >= 1:
            _seq(st, o, "f" * 6, "f")
    elif typ == 1:
        st.u32(o, "version"); st.u32(o, "gain")
    elif typ == 2:
        if st.u32(o, "version") >= 1:
            _seq(st, o, "ffff", "f")


def group(st, o):
    """SLib_group::LoadFromBuffer: version, three words and the group-insert count, the group inserts, four optional
    aux sends, [v1] a word."""
    ver = st.u32(o, "version")
    _seq(st, o, "iii", "i")
    ins = o.setdefault("inserts", [])
    n = _count(st, o, "n_inserts", len(ins), "i32")
    for it in _items(st, o, "inserts", max(n, 0)):
        _ginsert(st, it, st.u32(it, "type"))
    for it in _items(st, o, "sends", 4):
        if st.writing:
            it["present"] = 1 if it.get("send") else 0
        if st.u32(it, "present"):
            aux_send(st, it.setdefault("send", {}))
    if ver >= 1:
        st.u32(o, "u10")
    return o


# ----------------------------------------------------------------------------
# set
SET_SCALARS = {
    2: ("i32", "volume"), 4: ("u32", "group"), 5: ("u32", "model_key"), 6: ("u32", "file_count"),
    7: ("u32", "play_mode"), 8: ("u32", "file_loop_begin"), 9: ("u32", "file_loop_end"),
    0xA: ("u32", "file_loop_count"), 0xB: ("u32", "mode_flags"), 0xE: ("f32", "priority_scale"),
    0xF: ("i16", "priority"), 0x10: ("i16", "duplicate_limit"), 0x11: ("u32", "duplicate_mode"),
    0x13: ("u32", "insert_absent"), 0x15: ("u32", "aux_send_absent"), 0x16: ("u16", "file_flags"),
    0x18: ("u32", "category_id"),
}
SET_NAMES = {t: n for t, (_, n) in SET_SCALARS.items()}
SET_NAMES.update({1: "file_desc", 0xC: "vol_random_max", 0xD: "vol_random_min", 0x12: "insert",
                  0x14: "aux_send", 0x17: "language_file_desc"})


def _set_chunk(st, c, t, ver):
    if t > 0x18:
        return
    spec = SET_SCALARS.get(t)
    if spec:
        getattr(st, spec[0])(c, "value")
    elif t in (0xC, 0xD):
        (st.i32 if ver > 5 else st.f32)(c, "value")
    elif t == 1:
        _tagword(st, c, "index", "version")
        _seq(st, c, "fffffu", "d")
    elif t == 0x17:
        _tagword(st, c, "language", "version")
        mode = 2 if c["language"] == getattr(st, "language", SET_LANGUAGE) else 1
        _seq(st, c, "fffffu", "d", mode)
    elif t == 0x12:
        ins = c.setdefault("insert", {})
        _tagword(st, ins, "type", "version")
        _insert_payload(st, ins, ins["type"], ins["version"])
    elif t == 0x14:
        _tagword(st, c, "slot", "version")
        aux_send(st, c.setdefault("send", {}))


def _set_v1(st, o):
    """SLib_set::pu8_LoadFromBufferV1 (versions 0 and 1)."""
    for nm in ("file_count", "play_mode", "file_loop_begin", "file_loop_end", "file_loop_count", "mode_flags"):
        st.u32(o, nm)
    ins = o.setdefault("inserts", [])
    i = 0
    while True:                                          # until a 0 word
        if st.writing and i >= len(ins):
            st.u32({"w": 0}, "w")
            break
        it = _next(st, ins, i)
        i += 1
        w = _tagword(st, it, "type", "version")
        if w == 0:
            if not st.writing:
                ins.pop()
            break
        _insert_payload(st, it, it["type"], it["version"])
    sends = o.setdefault("aux_sends", [])
    i = 0
    while True:                                          # until 0xFFFFFFFF
        if st.writing and i >= len(sends):
            st.u32({"w": 0xFFFFFFFF}, "w")
            break
        it = _next(st, sends, i)
        i += 1
        w = _tagword(st, it, "slot", "version")
        if w == 0xFFFFFFFF:
            if not st.writing:
                sends.pop()
            break
        if it["slot"] <= 3:
            if it["version"] < 1:
                st.i32(it, "level"); st.u32(it, "flags"); st.f32(it, "f")
            else:
                aux_send(st, it.setdefault("send", {}))
    ch = o.setdefault("v1_chunks", [])
    i = 0
    while True:                                          # until a word whose low half is 0
        if st.writing and i >= len(ch):
            st.u32({"w": 0}, "w")
            break
        it = _next(st, ch, i)
        i += 1
        _tagword(st, it, "kind", "version")
        k = it["kind"]
        if k == 0:
            if not st.writing:
                ch.pop()
            break
        if k == 1:
            st.u32(it, "index"); _seq(st, it, "ffffu", "d")
            if it["version"] == 1:
                st.f32(it, "d10")
        elif k == 2:
            st.u32(it, "volume"); st.f32(it, "vol_random_max"); st.f32(it, "vol_random_min")
        elif k == 3:
            st.f32(it, "priority_scale"); st.u16(it, "priority"); st.u16(it, "duplicate_limit")
            st.u32(it, "duplicate_mode")
        elif k == 4:
            st.u32(it, "u0"); st.u32(it, "u1")


def sound_set(st, o):
    """SLib_set::b_Create: the data block (SLib_set::pu8_LoadFromBuffer: u32 version, then u16-tagged chunks until tag
    0; versions <= 1 use the V1 layout; from version 4 two words only loose files keep), then SLib_set::LoadReferences
    (the children; in a package only when mu_FileCount != 0)."""
    if not _block(st, o):
        _refs(st, o, False)
        return o
    ver = st.u32(o, "version")
    if ver > 7:
        raise FormatError(f"set version {ver}")
    count = 0
    if ver <= 1:
        _set_v1(st, o)
        count = o.get("file_count", 0)
    else:
        chunks = o.setdefault("chunks", [])
        i = 0
        while True:
            if st.writing and i >= len(chunks):
                st.u16({"t": 0}, "t")
                break
            c = _next(st, chunks, i)
            i += 1
            t = st.u16(c, "tag")
            if t == 0:
                if not st.writing:
                    chunks.pop()
                break
            _set_chunk(st, c, t, ver)
            if t == 6:
                count = c["value"]
    if ver >= 4:
        st.u32(o, "m1_u0", 1); st.u32(o, "m1_u1", 1)
    o["file_count"] = count
    _refs(st, o, count != 0)
    return o


def set_field(o, name, default=None):
    """The value of a scalar set chunk by name (the last one wins)."""
    v = default
    for c in o.get("chunks", []):
        if SET_NAMES.get(c.get("tag")) == name and "value" in c:
            v = c["value"]
    return o.get(name, v) if not o.get("chunks") and name in o else v


def refs_set(o):
    out = [r.key for r in o.get("refs", [])]
    for c in o.get("chunks", []):
        t = c.get("tag")
        if t == 5:
            out.append(c.get("value", 0))
        elif t == 0x12:
            out += insert_curve_keys(c.get("insert", {}))
        elif t == 0x14:
            out.append(c.get("send", {}).get("curve", 0))
    for it in o.get("inserts", []):
        out += insert_curve_keys(it)
    for it in o.get("aux_sends", []):
        out.append(it.get("send", {}).get("curve", 0))
    return [k for k in out if k]


# ----------------------------------------------------------------------------
# smp
def smp(st, o):
    """SLib_smp::b_Create: the data block (u32 version <= 5, u32 file count, [v1] u16, [v2] u32 mode: 1 random -> u16
    chance per file [v3 u8], 2 switch -> u16 variable + u8 value per file), then the children (in a package only when
    the file count is not 0)."""
    if not _block(st, o):
        _refs(st, o, False)
        return o
    ver = st.u32(o, "version")
    if ver > 5:
        raise FormatError(f"smp version {ver}")
    n = st.u32(o, "file_count")
    if ver >= 1:
        st.u16(o, "u16")
    if ver >= 2:
        mode = st.u32(o, "mode")
        if mode == 1:
            st.array(o, "chances", "<H", n)
            if ver >= 3:
                st.u8(o, "random_remember")
        elif mode == 2:
            st.u16(o, "variable")
            st.array(o, "values", "<B", n)
    _refs(st, o, n != 0)
    return o


def refs_smp(o):
    return [r.key for r in o.get("refs", []) if r.key]


# ----------------------------------------------------------------------------
# smx
def smx(st, o):
    """SLib_smx::pu8_LoadFromBuffer: u16, u16 version (<= 0x1004), then u32 tags until 0xFFFFFFFF: 0 old group, 1 group
    aux send, 2 group insert, 3 aux effect, 4/5/7 curve keys (+f32 scales from 0x1002), 6 u32, 8 group, 9 f32, 10 group
    names, 11 group colours (the names and colours themselves only in loose files), 12 u16."""
    if not _block(st, o):
        return o
    st.u16(o, "u0")
    ver = st.u16(o, "version")
    if ver > 0x1004:
        raise FormatError(f"smx version {ver:#x}")
    lst = o.setdefault("chunks", [])
    i = 0
    while True:
        if st.writing and i >= len(lst):
            st.u32({"t": 0xFFFFFFFF}, "t")
            break
        c = _next(st, lst, i)
        i += 1
        t = st.u32(c, "tag")
        if t == 0xFFFFFFFF:
            if not st.writing:
                lst.pop()
            break
        if t == 0:
            st.u32(c, "index"); _seq(st, c, "iiii", "i")
        elif t == 1:
            st.u32(c, "index"); st.u32(c, "slot")
            if ver > 0x1000:
                aux_send(st, c.setdefault("send", {}))
            else:
                st.u32(c, "flags"); st.i32(c, "level")
        elif t == 2:
            st.u32(c, "index"); st.u32(c, "slot")
            ins = c.setdefault("insert", {})
            _tagword(st, ins, "type", "version")
            _insert_payload(st, ins, ins["type"], ins["tag_word"] & 0xFFFF0000)   # the raw high half
        elif t == 3:
            st.u32(c, "slot")
            aux_effect(st, c.setdefault("aux", {}), st.u32(c, "type"))
        elif t in (4, 5):
            st.u32(c, "curve")
            if ver >= 0x1002:
                st.f32(c, "scale")
        elif t == 6:
            st.u32(c, "value")
        elif t == 7:
            st.u32(c, "curve")
        elif t == 8:
            st.u32(c, "index")
            group(st, c.setdefault("group", {}))
        elif t == 9:
            st.f32(c, "value")
        elif t == 10:
            names = c.setdefault("names", [])
            m = _count(st, c, "count", len(names))
            ln = st.u32(c, "length")
            for it in _items(st, c, "names", m):
                st.raw(it, "name", ln, 1)
        elif t == 11:
            cols = c.setdefault("colors", [])
            m = _count(st, c, "count", len(cols))
            for it in _items(st, c, "colors", m):
                st.u32(it, "color", 1)
        elif t == 12:
            st.u16(c, "value")
    return o


def refs_smx(o):
    out = []
    for c in o.get("chunks", []):
        out += [c.get("curve", 0), c.get("send", {}).get("curve", 0)]
        out += insert_curve_keys(c.get("insert", {}))
        out += aux_curve_keys(c.get("aux", {}))
        g = c.get("group", {})
        out += [s.get("send", {}).get("curve", 0) for s in g.get("sends", [])]
    return [k for k in out if k]


# ----------------------------------------------------------------------------
# mic
def mic(st, o):
    """SLib_mic::pu8_LoadFromBuffer: u32 version << 16 | micro count, then u32 tags until 0x10005: 0x10000 micro
    definition, 0x10002 / 0x10003 in / out curve keys, 0x10004 priority, 0x10006 micro names, 0x10007 micro colours
    (names and colours only in loose files), 0x10008 file flags.  From version 0x1004 a micro definition carries a
    flag word whose bit 0 announces 4 curve keys."""
    if not _block(st, o):
        return o
    head = st.u32(o, "head")
    ver = head & 0xFFFF0000
    if ver > 0x10050000:
        raise FormatError(f"mic version {ver >> 16:#x}")
    lst = o.setdefault("chunks", [])
    i = 0
    while True:
        if st.writing and i >= len(lst):
            st.u32({"t": 0x10005}, "t")
            break
        c = _next(st, lst, i)
        i += 1
        t = st.u32(c, "tag")
        if t == 0x10005:
            if not st.writing:
                lst.pop()
            break
        if t == 0x10000:
            st.u32(c, "index"); st.i32(c, "i0")
            if ver == 0x10010000:
                st.i32(c, "old0"); st.i32(c, "i4"); st.u32(c, "old1")
            elif ver < 0x10030000:
                st.i32(c, "old0"); st.i32(c, "i4")
            else:
                st.i32(c, "i4"); _seq(st, c, "ffff", "f")
                if ver >= 0x10040000 and st.u32(c, "flags") & 1:
                    st.array(c, "curves", "<I", 4)
        elif t in (0x10002, 0x10003, 0x10004):
            st.u32(c, "value")
        elif t == 0x10006:
            names = c.setdefault("names", [])
            m = _count(st, c, "count", len(names))
            ln = st.u32(c, "length")
            for it in _items(st, c, "names", m):
                st.raw(it, "name", ln, 1)
        elif t == 0x10007:
            cols = c.setdefault("colors", [])
            m = _count(st, c, "count", len(cols))
            for it in _items(st, c, "colors", m):
                st.u32(it, "color", 1)
        elif t == 0x10008:
            st.u16(c, "file_flags")
    return o


def refs_mic(o):
    out = []
    for c in o.get("chunks", []):
        if c.get("tag") in (0x10002, 0x10003):
            out.append(c.get("value", 0))
        out += list(c.get("curves", []))
    return [k for k in out if k]


# ----------------------------------------------------------------------------
# sound variables
def sound_variable(st, o):
    """SLib_sound_variable::pu_LoadFromBuffer."""
    if st.u32(o, "version") > 1:
        raise FormatError(f"sound variable version {o['version']}")
    st.u16(o, "id")
    st.u32(o, "key")
    vals = o.setdefault("values", [])
    n = _count(st, o, "n_values", len(vals), "u8")
    st.raw(o, "m1_name", 64, 1)
    st.u32(o, "m1_u", 1)
    for it in _items(st, o, "values", n):
        st.u8(it, "value")
        st.raw(it, "m1_name", 64, 1)
    return o


def sound_variables(st, o):
    """SLib_sound_variable_file::pu8_LoadFromBuffer: u32 version <= 1, u16 count, the variables, a word only loose files
    keep."""
    if not _block(st, o):
        return o
    if st.u32(o, "version") > 1:
        raise FormatError(f"sound variable file version {o['version']}")
    vs = o.setdefault("variables", [])
    n = _count(st, o, "count", len(vs), "u16")
    for it in _items(st, o, "variables", n):
        sound_variable(st, it)
    st.u32(o, "m1_u", 1)
    return o


def refs_sva(o):
    return []


# ----------------------------------------------------------------------------
# samples (SLib_file_S -> SLib_wave)
def sample(st, o):
    """SLib_wave::b_Create: 'RIFF', size, 'WAVE', chunks up to 'fmt ' (contents only in loose files), fmt (format,
    channels, rate; the rest only in loose files), then fact (sample count, a word), cue (point ids and positions),
    LIST 'adtl' whose ltxt (cue id, length) and labl (cue id, NUL-terminated name) sub-chunks follow as chunks, and
    'data'.  Chunks start on even loose-file positions (the pad byte only in loose files).  After 'data': in a package
    the stream link of a streamed sample; in a loose file the sample bytes."""
    if not _block(st, o):
        return o
    if st.u32(o, "riff", default=RIFF) != RIFF:
        raise FormatError("not a RIFF sample")
    st.u32(o, "riff_size")
    if st.u32(o, "wave", default=WAVE) != WAVE:
        raise FormatError("not a WAVE sample")
    chunks = o.setdefault("chunks", [])
    i = 0
    pos = 12
    while True:                                          # chunks before 'fmt '
        c = _next(st, chunks, i)
        i += 1
        cid = st.u32(c, "id")
        size = st.u32(c, "size")
        pos += 8
        if cid == FMT:
            break
        st.raw(c, "content", size, 1)
        pos += size
        if pos > MAX_HEADER:
            raise FormatError("no fmt chunk")
    if size < 14:
        raise FormatError(f"fmt chunk of {size} bytes")
    tag = st.u16(c, "format")
    st.u16(c, "channels")
    st.u32(c, "rate")
    st.u32(c, "m1_bytes_per_sec", 1)
    st.u16(c, "m1_block_align", 1)
    if tag in (0x5050, 0x5051, 1, 0x3156, 0x166):
        rest = size - 14
    elif tag == 0xFFFE:
        st.u16(c, "m1_bits", 1); st.u16(c, "m1_cbsize", 1); st.u16(c, "m1_valid_bits", 1)
        st.u32(c, "channel_mask"); st.u32(c, "guid0"); st.u16(c, "guid1"); st.u16(c, "guid2")
        st.raw(c, "guid3", 8)
        rest = size - 14 - 26
    else:
        raise FormatError(f"sample format {tag:#x}")
    if rest > 0:
        st.raw(c, "m1_rest", rest, 1)
    pos += size
    while True:
        if st.writing and i >= len(chunks):
            raise FormatError("sample without a data chunk")
        c = _next(st, chunks, i)
        i += 1
        if pos & 1:
            st.u8(c, "m1_pad", 1)
            pos += 1
        if pos > MAX_HEADER:
            raise FormatError("sample header too long")
        cid = st.u32(c, "id")
        size = st.u32(c, "size")
        pos += 8
        if cid == FACT:
            if size >= 8:
                st.u32(c, "samples"); st.u32(c, "u4")
                st.raw(c, "m1_rest", size - 8, 1)
                pos += size
        elif cid == CUE:
            pts = c.setdefault("points", [])
            n = _count(st, c, "count", len(pts))
            for p in _items(st, c, "points", n):
                st.u32(p, "id"); st.u32(p, "position")
                _seq(st, p, "uuuu", "m1_", 1)
            rest = size - 4 - 24 * n
            if rest > 0:
                st.raw(c, "m1_rest", rest, 1)
            pos += size
        elif cid == LIST:
            lt = st.u32(c, "type")
            pos += 4
            if size - 4 and lt != ADTL:
                st.raw(c, "m1_rest", size - 4, 1)
                pos += size - 4
        elif cid == LABL:
            st.u32(c, "cue_id")
            if st.writing:
                txt = bytes(c.get("text", b"")).split(b"\0")[0] + b"\0"
                st.raw({"t": txt}, "t", len(txt))
            else:
                start = st.pos
                while st.u8({}, "b"):
                    pass
                txt = bytes(st.d[start:st.pos])
                c["text"] = txt[:-1]
            rest = size - 4 - len(txt)
            if rest > 0:
                st.raw(c, "m1_rest", rest, 1)
            pos += size
        elif cid == LTXT:
            st.u32(c, "cue_id"); st.u32(c, "length")
            if size > 8:
                st.raw(c, "m1_rest", size - 8, 1)
            pos += size
        elif cid == DATA:
            break
        else:
            raise FormatError(f"sample chunk {struct.pack('<I', cid)!r} not read by SLib_wave")
    o["data_size"] = size
    if st.binary:
        if st.writing:
            link = o.get("stream")
        else:
            link = {} if st.remaining() >= 40 else None
        if link is not None:
            o["stream"] = link
            st.u32(link, "kind"); st.u32(link, "file_size"); st.raw(link, "name", 32)
    else:
        if st.writing:
            d = bytes(o.get("data") or b"")
            st.raw({"d": d}, "d", len(d))
        else:
            o["data"] = st.raw({}, "d", st.remaining())
    return o


def refs_sample(o):
    return []


def sample_markers(o):
    """[(cue id, position, length, name)] of a sample's cue / ltxt / labl chunks."""
    pts = {}
    for c in o.get("chunks", []):
        if c.get("id") == CUE:
            for p in c.get("points", []):
                pts[p["id"]] = [p["id"], p["position"], 0, b""]
    for c in o.get("chunks", []):
        if c.get("id") == LTXT and c.get("cue_id") in pts:
            pts[c["cue_id"]][2] = c.get("length", 0)
        elif c.get("id") == LABL and c.get("cue_id") in pts:
            pts[c["cue_id"]][3] = c.get("text", b"")
    return [tuple(v) for v in pts.values()]


# ----------------------------------------------------------------------------
# dispatch
FILE_KINDS = {1: ("sample", sample), 2: ("sample", sample), 3: ("set", sound_set), 5: ("smx", smx),
              6: ("mic", mic), 7: ("smp", smp), 8: ("sva", sound_variables), 10: ("sample", sample)}
REFS = {"bank": refs_bank, "set": refs_set, "smp": refs_smp, "smx": refs_smx, "mic": refs_mic,
        "sva": refs_sva, "sample": refs_sample}

NON_FILE_REF = 0x10000000          # priv bit of reference-table entries the SLib loaders skip (curves)


def child_refs(kind, o):
    """[(key, user word)] of the SLib files a bank / set / smp loads, with the loaders' rules: entries whose priv word
    has 0x10000000 are not files (the curves a set's table lists, with arbitrary user words), key 0 is an empty bank
    slot, and SLib_set::LoadReferences skips user word 3 (the model set, loaded through chunk 5)."""
    out = []
    if kind in ("bank", "set", "smp"):
        for r in o.get("refs", []):
            if not r.key or r.priv & NON_FILE_REF or (kind == "set" and r.user == 3):
                continue
            out.append((r.key, r.user))
    if kind == "set":
        out += [(c.get("value", 0), 3) for c in o.get("chunks", []) if c.get("tag") == 5 and c.get("value")]
    return out


def curve_refs(kind, o):
    children = {k for k, _ in child_refs(kind, o)}
    return [k for k in REFS[kind](o) if k not in children]
