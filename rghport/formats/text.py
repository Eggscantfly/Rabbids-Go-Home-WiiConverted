"""Text resources: text groups (Txg), language files (Txl), dialogs (Txd), fonts (Fod), and where Rabbids Go Home keeps
the localized strings.

Loaders (RGH Wii executable):

  Txg  TXT::p_Callback_LoadTxg   (RGH 8015B824)
  Txl  TXT::p_Callback_LoadTxl   (RGH 8015B4A8)
  Txd  TXT::p_Callback_LoadTxd   (RGH 8015BDA4)
       SCR::LoadTrigger (RGH 8011ED78; modifiers.trigger)
  TXT  TXT::p_Callback_LoadTXT   (RGH 8015BB60; stream in modifiers.txt)
  language data: ViD::LoadTexts (RGH 800B558C), TXT::TXT_LoadLangData (8015C72C) -> TXG_LoadLangData (8015C9F4,
       LOA_MakeFileRef of the Txl whose ref user word == language), TXT_LoadLangData2 (8015C820) -> TXG_LoadLangData2
       (8015CAC0, SLib_file::b_LoadBinData of the text sounds), TXT::GetConsoleDefaultLanguage (8015AEEC),
       TXT_LangShortName (8055CE40).
  Fod  K3D_texture::p_Callback_LoadFont (RGH 8032CB3C).  Referenced only by K3D_texture::LoadDesc (a texture
       descriptor's font key, stream.texture "font"/"d_font"), never by TXT/Txg/Txl/Txd; RGH's packages hold 5 fonts
       (four version 1, one version 3 with 1137 glyphs) named by 41 texture descriptors.

What a TXT resource is:
  TXT modifier  n_texts Txg keys + n_dialogs Txd keys (modifiers.txt).
  Txg  TXT_tt_TextLang_: the text ids of a group with owner/target keys and priority/facial/anim bytes, annotations only
       loose files keep (mode 3: a 64-char name, four words, 20 length-prefixed columns, two words and a second name;
       mode 1: two words), then a reference list whose user word is the language index -> one Txl per language.
  Txl  TXT_tt_Text_: per id {offset, delay, type, sound key (SLib localized file), lips key, expression key (EVE track
       lists)} and the string pool (UTF-16 from version 4, bytes before).  The strings live here (mode 2); only two
       words per entry are mode 3.
  Txd  TXT_tt_DialogDescriptor_: dialog items {id, attribute, category, enabled/disabled/linked/variant item lists,
       period, 4 text entries {Txg key, text id}, script function ids, triggers, user words}, then the Txg keys the
       dialog loads.

Where RGH's strings are:
  * The Txg/Txd records sit in the world packages (0xFFF.....); a Txg ends with two refs {Txl key, 0, 1} and
    {Txl key, 0, 12}.
  * The Txl records are not in the packages.  Every world package has a text list fcfXXXXX.bin (0xFCF | package low
    20 bits; 133 with two entries, 26 empty): u32 n + n x {bin key, 0, language}.  ViD::LoadTexts picks the entry whose
    language is the console's, LOA_MakeFileRef's the Txl keys of the world's TXT groups, then reads that language bin
    089XXXXX.bin (266 of them): u32 size, `size` bytes of package-style records {u32 key, u32 len, memsize + data}
    (the Txl records; the SLib sound records their entries name; 0-byte placeholders for records another bin already
    holds), then the sample data TXG_LoadLangData2 reads.  RGH's Txl lips/expression keys are all 0.
  * Languages on the Wii disc: 1 (en) and 12 (ja) only.  The index is TXT_LangShortName's (0 fr, 1 en, 2 da, 3 nl,
    4 fi, 5 de, 6 it, 7 es, 8 pt, 9 sv, 10 pl, 11 ru, 12 ja, 13 zh, ..., 28 tw); the Wii console language maps JP->12,
    EN->1, DE->5, FR->0, ES->7, IT->6, NL->3, zh->13, KO->19 (default 1).
  `LangData` indexes the lists and bins.

Versions RGH ships: Txg 5, Txl 4, Txd 6/7/8, Fod 1 and 3.  Txg version < 2 with priority 0 -> priority 3 (no read).
Fod below version 2: the glyph table has 256 slots and no counts; a font without the 0xC0DE word is version 0 (glyphs
only).

Keys through LOA_MakeFileRef (refs_* helpers):
  Txg  its reference list (Txl per language; TXG_LoadLangData)
  Txl  sound_key (SLib::p_Callback_LoadLocalizedFile; 0xFFFFFFFF = none), lips_key and expr_key (EVE::p_Callback_LoadTrl)
  Txd  txg_keys (TXT::p_Callback_LoadTxg); the items' text entries name Txg keys too but are resolved against that
       list, not loaded.
"""
from __future__ import annotations

import struct

from . import modifiers as M
from . import stream as F
from .stream import FormatError

TXG_MAX = 5
TXL_MAX = 4
TXD_MAX = 8
FOD_MAX = 3

LANG_SHORT = ("fr", "en", "da", "nl", "fi", "de", "it", "es", "pt", "sv", "pl", "ru", "ja", "zh", "sq", "ar",
              "bg", "be", "el", "ko", "no", "ro", "sr", "sk", "sl", "tr", "cs", "hu", "tw", "u0", "u1", "u2")
# SCGetLanguage() -> language index (TXT::GetConsoleDefaultLanguage; anything else -> 1)
CONSOLE_LANG = {0: 12, 1: 1, 2: 5, 3: 0, 4: 7, 5: 6, 6: 3, 7: 13, 8: 13, 9: 19}

NO_REF = 0x10000000          # BIG_tt_Ref priv bit TXG_LoadLangData skips
NO_KEY = 0xFFFFFFFF


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


# ----------------------------------------------------------------------------
# Txg
def txg(st, o):
    """TXT::p_Callback_LoadTxg: version, entry count, u16 flags, u16 group, the entries (TXT_tt_TextID_ + annotations
    only loose files keep), then the reference list {Txl key, priv, language}."""
    st.memsize(o)
    ver = st.u32(o, "version", default=5)
    if ver > TXG_MAX:
        raise FormatError(f"Txg version {ver}")
    ents = o.setdefault("entries", [])
    n = _count(st, o, "n_entries", len(ents))
    st.u16(o, "flags")                           # u16_Flags
    st.u16(o, "group")                           # u16_Group
    for e in _items(st, o, "entries", n):
        st.u32(e, "id")                          # u32_ID
        st.u32(e, "owner")                       # h_Owner (a key, not loaded)
        st.u32(e, "target")                      # h_Target
        st.u8(e, "priority"); st.u8(e, "facial"); st.u8(e, "anim"); st.u8(e, "dummy")
        st.raw(e, "m3_name", 64, 3)
        for i in range(4):
            st.u32(e, f"m3_u{i}", 3)
        cols = e.get("m3_columns") if st.writing else None
        out = []
        for i in range(20):                      # u32 length, then that many bytes
            c = bytes(cols[i]) if cols and i < len(cols) and cols[i] else b""
            tmp = {"n": len(c), "c": c}
            ln = st.u32(tmp, "n", 3)
            if ln and st.present(3):
                c = st.raw(tmp, "c", ln, 3)
            out.append(bytes(c) if ln else b"")
        if not st.writing:
            e["m3_columns"] = out
        if ver >= 3:
            st.u32(e, "m3_u4", 3, default=NO_KEY)
            st.u32(e, "m3_u5", 3, default=NO_KEY)
            st.raw(e, "m3_name2", 64, 3)
        if ver >= 4:
            st.u32(e, "m1_u6", 1)
        if ver >= 5:
            st.u32(e, "m1_variants", 1)
    st.refs(o, "refs")
    return o


def refs_txg(o):
    """The Txl keys (one per language)."""
    return [r.key for r in o.get("refs", []) if r.key and not (r.priv & NO_REF)]


def txg_languages(o) -> dict[int, int]:
    """{language index: Txl key}."""
    return {r.user: r.key for r in o.get("refs", []) if r.key and not (r.priv & NO_REF)}


# ----------------------------------------------------------------------------
# Txl
def txl(st, o):
    """TXT::p_Callback_LoadTxl: version, entry count, TXT_tt_TextIDSpec_ entries, the pool size (characters) and the
    pool (u16 from version 4)."""
    st.memsize(o)
    ver = st.u32(o, "version", default=4)
    if ver > TXL_MAX:
        raise FormatError(f"Txl version {ver}")
    ents = o.setdefault("entries", [])
    n = _count(st, o, "n_entries", len(ents))
    for e in _items(st, o, "entries", n):
        st.u32(e, "id")                          # u32_ID
        st.u32(e, "offset")                      # u32_Offset (characters)
        if ver > 1:
            st.f32(e, "delay")                   # f32_Delay
        if ver > 2:
            st.u32(e, "type", default=10)        # u32_Type
        elif not st.writing:
            e.setdefault("type", 10)
        st.u32(e, "sound_key")                   # SLib::p_Callback_LoadLocalizedFile unless 0xFFFFFFFF
        st.u32(e, "lips_key")                    # EVE::p_Callback_LoadTrl
        st.u32(e, "expr_key")                    # EVE::p_Callback_LoadTrl
        st.u32(e, "m3_u0", 3)
        st.u32(e, "m3_u1", 3)
    unit = 2 if ver > 3 else 1
    if st.writing:
        o["size"] = len(o.get("text") or b"") // unit
    size = st.u32(o, "size")                     # u32_SizeBuf
    if size:
        st.raw(o, "text", size * unit)
    elif not st.writing:
        o["text"] = b""
    return o


def refs_txl(o):
    out = []
    for e in o.get("entries", []):
        for k in (e.get("sound_key", 0), e.get("lips_key", 0), e.get("expr_key", 0)):
            if k and k != NO_KEY:
                out.append(k)
    return out


def ref_kinds_txl(o):
    out = []
    for e in o.get("entries", []):
        for k, kind in ((e.get("sound_key", 0), "soundfile"), (e.get("lips_key", 0), "trl"), (e.get("expr_key", 0), "trl")):
            if k and k != NO_KEY:
                out.append((k, kind))
    return out


def txl_strings(o) -> dict[int, str]:
    """{text id: string} of a parsed Txl (the pool is NUL-separated; "/n" is the game's line break and is kept)."""
    text = o.get("text") or b""
    wide = o.get("version", 4) > 3
    out = {}
    for e in o.get("entries", []):
        if wide:
            a = 2 * e.get("offset", 0)
            b = a
            while b + 1 < len(text) and text[b:b + 2] != b"\0\0":
                b += 2
            out[e["id"]] = text[a:b].decode("utf-16-le", "replace")
        else:
            a = e.get("offset", 0)
            b = text.find(b"\0", a)
            out[e["id"]] = text[a:b if b >= 0 else len(text)].decode("latin-1")
    return out


def set_txl_strings(o, strings: dict[int, str]):
    """Rebuild a Txl's pool from {text id: string} (entries keep their order; ids missing from `strings` keep their
    current text).  Writes version 4."""
    old = txl_strings(o)
    pool = bytearray()
    for e in o.get("entries", []):
        s = strings.get(e["id"], old.get(e["id"], ""))
        e["offset"] = len(pool) // 2
        pool += s.encode("utf-16-le") + b"\0\0"
    o["version"] = max(4, o.get("version", 4))
    o["text"] = bytes(pool)
    o["size"] = len(pool) // 2
    return o


# ----------------------------------------------------------------------------
# Txd
def txd(st, o):
    """TXT::p_Callback_LoadTxd: version, item count, Txg count, the dialog items, then the Txg keys."""
    st.memsize(o)
    ver = st.u32(o, "version", default=8)
    if ver < 4 or ver > TXD_MAX:
        raise FormatError(f"Txd version {ver}")
    n = _count(st, o, "n_items", len(o.get("items") or []))
    m = _count(st, o, "n_txgs", len(o.get("txg_keys") or []))
    for it in _items(st, o, "items", n):
        st.u32(it, "id")                         # u_Id
        attr = st.u32(it, "attribute")           # u_Attribute
        st.u8(it, "category")                    # u_Category
        ne = _count(st, it, "n_enabled", len(it.get("enabled") or []), "u8")
        nd = _count(st, it, "n_disabled", len(it.get("disabled") or []), "u8")
        nl = _count(st, it, "n_linked", len(it.get("linked") or []), "u8")
        nv = _count(st, it, "n_variants", len(it.get("variants") or []), "u8") if ver >= 6 else 0
        st.f32(it, "period")                     # f_Period
        for t in _items(st, it, "texts", 4):     # at_TextEntrie[4]
            st.u32(t, "txg_key")
            st.u32(t, "text_id")
        st.array(it, "enabled", "<I", ne)
        st.array(it, "disabled", "<I", nd)
        st.array(it, "linked", "<I", nl)
        if ver >= 6:
            st.array(it, "variants", "<I", nv)
            if attr & 0x2000:
                st.u32(it, "variant_container")  # pt_VariantContainer
        st.raw(it, "m3_name", 64, 3)
        if ver >= 6:
            st.u32(it, "m3_u0", 3)
            st.u32(it, "m3_u1", 3)
        st.u32(it, "test_sf0"); st.u32(it, "test_sf1")
        k = 1 if ver <= 4 else 8
        st.array(it, "exec_sf0", "<I", k)
        st.array(it, "exec_sf1", "<I", k)
        st.u32(it, "gao_ref")                    # u_GaoRef
        if ver > 7:
            fl = (it.get("trigger_flags", 0) & ~3) | (1 if it.get("test_trigger") else 0) | (2 if it.get("exec_trigger") else 0)
            fl = _count(st, it, "trigger_flags", fl, "u8")
            if fl & 1:
                M.trigger(st, it.setdefault("test_trigger", {}))    # pt_Test
            if fl & 2:
                M.trigger(st, it.setdefault("exec_trigger", {}))    # pt_Exec
        else:
            M.trigger(st, it.setdefault("test_trigger", {}))
            M.trigger(st, it.setdefault("exec_trigger", {}))
        if ver > 4:
            st.u16(it, "user0"); st.u16(it, "user1")
    st.array(o, "txg_keys", "<I", m)
    return o


def refs_txd(o):
    return [k for k in o.get("txg_keys", []) if k]


def txd_text_txg_keys(o):
    """Txg keys named by the items' text entries (resolved against txg_keys)."""
    out = []
    for it in o.get("items", []):
        for t in it.get("texts", []):
            k = t.get("txg_key", 0)
            if k and k != NO_KEY and k not in out:
                out.append(k)
    return out


def ref_kinds_txt(o):
    """A TXT modifier (modifiers.txt) -> [(key, "txg"|"txd")]."""
    nt = o.get("n_texts", 0)
    return [(r.key, "txg" if i < nt else "txd") for i, r in enumerate(o.get("refs", [])) if r.key]


# ----------------------------------------------------------------------------
# Fod: a font's glyph table
FOD_MAGIC = 0xC0DE


def fod(st, o):
    """K3D_texture::p_Callback_LoadFont: u16 0xC0DE, u16 version, two floats, [version >= 2: u16 glyph count, u16 map
    count, map count x {u16 char, u16 glyph index} (a char -> glyph hash)], then glyphs {u16 char, f32 x4,
    [version >= 3: f32 x4]} while the count (256 slots below version 2) and the remaining bytes allow.  Without the
    0xC0DE word the font is version 0: glyphs only."""
    st.memsize(o)
    glyphs = o.setdefault("glyphs", [])
    if st.writing:
        ver = o.get("version", 3)
        if ver:
            st.u16({"m": FOD_MAGIC}, "m")
            st.u16(o, "version")
    else:
        first = st.u16({}, "m")
        if first == FOD_MAGIC:
            ver = st.u16(o, "version")
        else:                                    # version 0: that word was the first glyph's char
            ver = o["version"] = 0
            g = {"char": first}
            st.array(g, "rect", "<f", 4)
            glyphs.append(g)
    if ver > FOD_MAX:
        raise FormatError(f"font version {ver}")
    if ver:
        st.f32(o, "f0")
        st.f32(o, "f1")
    if ver >= 2:
        n_slots = _count(st, o, "n_glyphs", max(o.get("n_glyphs", 0), len(glyphs)), "u16")
        n_map = _count(st, o, "n_map", len(o.get("char_map") or []), "u16")
        for m in _items(st, o, "char_map", n_map):
            st.u16(m, "char")
            st.u16(m, "glyph")
    else:
        n_slots = 256
    gsize = 34 if ver >= 3 else 18
    if st.writing:
        first_i, n = 0, len(glyphs)
    else:
        first_i = len(glyphs)
        n = max(first_i, min(n_slots, first_i + st.remaining() // gsize))
    for i in range(first_i, n):
        g = glyphs[i] if st.writing else {}
        if not st.writing:
            glyphs.append(g)
        st.u16(g, "char")
        st.array(g, "rect", "<f", 4)
        if ver >= 3:
            st.array(g, "rect2", "<f", 4)
    return o


def refs_fod(o):
    return []


# ----------------------------------------------------------------------------
# the archive's language data
def package_of_text_list(key: int) -> int:
    return 0xFFF00000 | (key & 0xFFFFF)


def text_list_key(package_key: int) -> int:
    return 0xFCF00000 | (package_key & 0xFFFFF)


def parse_text_list(data: bytes) -> list:
    """fcfXXXXX.bin: u32 n + n x {language bin key, priv, language}."""
    n = struct.unpack_from("<I", data, 0)[0]
    if len(data) != 4 + 12 * n:
        raise FormatError(f"text list: {len(data)} bytes for {n} refs")
    return [F.Ref(*struct.unpack_from("<III", data, 4 + 12 * i)) for i in range(n)]


def parse_lang_bin(data: bytes):
    """089XXXXX.bin -> ([(key, body)], tail).  body = memsize + stream (empty for placeholders); tail = the bytes after
    the records (sample data)."""
    size = struct.unpack_from("<I", data, 0)[0]
    end = 4 + size
    if end > len(data):
        raise FormatError(f"language bin: size {size} > {len(data) - 4}")
    out = []
    o = 4
    while o + 8 <= end:
        k, ln = struct.unpack_from("<II", data, o)
        if o + 8 + ln > end:
            raise FormatError(f"language bin: record {k:08X} overruns at {o:#x}")
        out.append((k, data[o + 8:o + 8 + ln]))
        o += 8 + ln
    if o != end:
        raise FormatError(f"language bin: records end at {o:#x}, size says {end:#x}")
    return out, data[end:]


class LangData:
    """The text lists (per world package) and language bins of an archive (bigfile.Big), with a key -> record index over
    the bins."""

    def __init__(self, big):
        self.big = big
        self.lists: dict[int, list[tuple[int, int]]] = {}     # package key -> [(language, bin key)]
        self.bin_lang: dict[int, int] = {}
        for e in big.entries():
            if e.key >> 20 == 0xFCF:
                refs = parse_text_list(big.read(e))
                self.lists[package_of_text_list(e.key)] = [(r.user, r.key) for r in refs]
                for r in refs:
                    self.bin_lang[r.key] = r.user
        self._index = None

    def index(self) -> dict:
        """record key -> (bin key, language, body), first non-empty copy."""
        if self._index is None:
            idx = {}
            for bk in sorted(self.bin_lang):
                e = self.big.find(bk)
                if e is None:
                    continue
                recs, _tail = parse_lang_bin(self.big.read(e))
                for k, body in recs:
                    if body and k not in idx:
                        idx[k] = (bk, self.bin_lang[bk], body)
            self._index = idx
        return self._index

    def record(self, key: int):
        r = self.index().get(key)
        return r[2] if r else None

    def language_of(self, key: int):
        r = self.index().get(key)
        return r[1] if r else None

    def txl(self, key: int):
        b = self.record(key)
        return F.parse(txl, b) if b else None

    def languages(self) -> list:
        return sorted(set(self.bin_lang.values()))


_LANG_CACHE: dict = {}


def lang_data(big) -> LangData:
    ld = _LANG_CACHE.get(id(big))
    if ld is None:
        ld = _LANG_CACHE[id(big)] = LangData(big)
    return ld
