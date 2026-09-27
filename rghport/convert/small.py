"""Small platform records: worlds, objects, groups, Magma files, generated keys.

    small.convert("world", key, wii_body, ctx)          # view 0 flag word: 0x800 (Wii) -> 0x2000 (PC)
    small.convert("object", key, wii_body, ctx)         # unchanged (Wii keys are kept); keymap= rewrites key fields
    small.convert("spec:grp", key, wii_body, ctx)       # unchanged; keymap= rewrites the object keys
    small.convert("mgf", key, wii_body, ctx)            # Magma file: PC languages without a Wii package -> English
    small.convert("mgm", key, wii_body, ctx)            # Magma modifier: unchanged
    small.convert("fbf", key, wii_entry, ctx)           # Magma package bin (fbfXXXXX.bin): unchanged
    km = small.generated_key_map(ctx, 0xFFF00EC6)       # levels both releases share: Wii 0x0891xxxx -> PC 0x0933xxxx

Rules (addresses of the PC executable):
  world    WOR::p_Callback_LoadWorld (0054CEB0) reads 4 views with ViD::pu8_ViewLoad (00503CE0) into one temporary
           camera and copies only view 0's matrix and field of view to engine view 0 (synchronous loads); the flag word
           is stored in that temporary camera and dropped.  Every PC world has 0x2000 where the Wii has 0x800 (201 /
           201 shared worlds); the camera matrices / view-1 FOV differences are authoring camera state (the PC often
           holds the default FOV 1.2) and are kept from the Wii.
  object / spec:grp / world lists
           reference binarizer-generated records (keys from the key counter at binarization: Wii 0x089xxxxx, PC
           0x0933xxxx) in the same package.  Keys are opaque to the executable (BIG::i64_KeySearchPos is an exact
           lookup) and the PC archive has no 0x089xxxxx key, so converted content keeps the Wii keys.  The map below
           exists to compare the rest of those records with the PC release.
  mgf      MGF::LoadPackage (007F7CF0) asks MGF::GetPackageFromID(platform 1 = "WII", TXT::mu32_CurrentLang, or
           language 1 when the file is not localized) on the PC too, loads fbf(key) (BIG_ChunkDecompress mode 5) and
           hands it to MGM_Manager.  Wii MAGMA blobs load unchanged (Photo / language packages are byte-identical in
           both archives); the Wii has fr / en / ja packages, the PC fr en nl de it es.
"""
from __future__ import annotations

from ..formats import stream as F
from ..formats import world as MW
from ..formats import zones as Z
from ..formats.stream import Ref
from . import language as L

VIEW_FLAG_WII = 0x800
VIEW_FLAG_PC = 0x2000
MAGMA_PLATFORM = 1                  # the platform id the PC executable asks MGF for ("0=PC,1=WII,2=X360,3=PS3")
EN = 1
PC_LANGS = L.PC_LANGS
NO_KEY = 0xFFFFFFFF
KINDS = ("world", "object", "spec:grp", "mgf", "mgm", "fbf")
WII_GENERATED_TOPS = (0x089, 0x016)  # key >> 20 of the Wii binarizer's generated records and language bins: the
                                    # Japanese disc's build, the US disc's build
PC_GENERATED_TOP = 0x093


class KeyMap(dict):
    """key -> key; missing keys map to themselves."""

    def __call__(self, key: int) -> int:
        return self.get(key, key)


def _km(keymap, key: int) -> int:
    if keymap is None or not key or key == NO_KEY:
        return key
    return keymap(key) if callable(keymap) else keymap.get(key, key)


def _parse(fn, body):
    return F.parse(fn, body)


def _emit(fn, o):
    return F.emit(fn, o)[0]


def _refs(lst, keymap):
    return [Ref(_km(keymap, r.key), r.priv, r.user) for r in lst]


# ============================================================================ world
def world_view_flags(body: bytes) -> int:
    return _parse(F.world, body)["views"][0].get("perspective", 0)


def convert_world(body: bytes, keymap=None) -> bytes:
    o = _parse(F.world, body)
    v = o["views"][0]
    fl = v.get("perspective", 0)
    changed = False
    if fl & VIEW_FLAG_WII:
        v["perspective"] = (fl & ~VIEW_FLAG_WII) | VIEW_FLAG_PC
        changed = True
    if keymap:
        if "key" in o:
            o["key"] = _km(keymap, o["key"])
        o["mdf_key"] = _km(keymap, o.get("mdf_key", 0))
        for name in ("mod_refs", "obj_refs"):
            o[name] = _refs(o.get(name, []), keymap)
        changed = True
    return _emit(F.world, o) if changed else body


# ============================================================================ object / group
def convert_object(body: bytes, keymap=None) -> bytes:
    if not keymap:
        _parse(F.obj, body)
        return body
    o = _parse(F.obj, body)
    o["key_copy"] = _km(keymap, o.get("key_copy", 0))
    if o.get("has_father"):
        o["father"] = _km(keymap, o.get("father", 0))
    o["refs"] = _refs(o.get("refs", []), keymap)
    return _emit(F.obj, o)


def convert_grp(body: bytes, keymap=None) -> bytes:
    if not keymap:
        _parse(Z.grp, body)
        return body
    o = _parse(Z.grp, body)
    o["grprefs"] = _km(keymap, o.get("grprefs", 0))
    for e in o.get("objects", []):
        e["key"] = _km(keymap, e.get("key", 0))
    return _emit(Z.grp, o)


# ============================================================================ Magma
def is_mgf(body: bytes) -> bool:
    try:
        o = _parse(MW.mgf, body)
    except Exception:                    # noqa: BLE001
        return False
    return bytes(o.get("name", b"")).split(b"\0")[0].lower().endswith(b".mgb")


def mgf_name(o) -> str:
    return bytes(o.get("name", b"")).split(b"\0")[0].decode("latin-1")


def mgf_row(o, platform: int = MAGMA_PLATFORM) -> dict[int, int]:
    """{language id: package key} of one platform."""
    plats = [p["platform"] for p in o["platforms"]]
    langs = [l["language"] for l in o["languages"]]
    if platform not in plats:
        return {}
    pi, nl = plats.index(platform), len(langs)
    return dict(zip(langs, o["package_keys"][pi * nl:(pi + 1) * nl]))


def fbf_key(package_key: int) -> int:
    return MW.mgb_bin_key(package_key)


def convert_mgf(body: bytes, ctx=None, keymap=None, langs=None, source_lang: int = EN) -> bytes:
    """Fill the platform-1 slots of the PC languages (default: language.pc_slots of the languages the Wii archive's
    text indexes name) whose package bin the Wii archive lacks with the source-language package (needs ctx to see
    which fbf bins exist).  Name bytes after the NUL are left as they are (the PC binarizer leaves stale UTF-16 there;
    the loader reads a C string)."""
    o = _parse(MW.mgf, body)
    changed = False
    plats = [p["platform"] for p in o["platforms"]]
    lang_ids = [l["language"] for l in o["languages"]]
    if ctx is not None and MAGMA_PLATFORM in plats and source_lang in lang_ids:
        if langs is None:
            langs = L.pc_slots(L.archive_languages(ctx.wii_big))
        pi, nl = plats.index(MAGMA_PLATFORM), len(lang_ids)

        def has(k):
            return bool(k) and ctx.wii_big.find(fbf_key(k)) is not None

        src = o["package_keys"][pi * nl + lang_ids.index(source_lang)]
        if has(src):
            for lg in langs:
                if lg in lang_ids:
                    slot = pi * nl + lang_ids.index(lg)
                    if not has(o["package_keys"][slot]):
                        o["package_keys"][slot] = src
                        changed = True
    if keymap:
        o["package_keys"] = [_km(keymap, k) for k in o["package_keys"]]
        for name, _kind in MW.MGF_LISTS:
            for e in o.get(name, []):
                e["key"] = _km(keymap, e.get("key", 0))
        changed = True
    return _emit(MW.mgf, o) if changed else body


def magma_package_files(ctx, mgf_body: bytes) -> dict[int, bytes]:
    """{fbf key: Wii bytes} of the platform-1 packages a (converted) Magma file names that the Wii archive holds."""
    out = {}
    for k in set(mgf_row(_parse(MW.mgf, mgf_body)).values()):
        e = ctx.wii_big.find(fbf_key(k)) if k else None
        if e is not None:
            out[fbf_key(k)] = ctx.wii_big.read(e)
    return out


# ============================================================================ generated keys
def is_wii_generated(key: int) -> bool:
    return key >> 20 in WII_GENERATED_TOPS


def generated_key_map(ctx, package_key: int) -> KeyMap:
    """Wii generated key -> PC generated key of one package present in both archives.  Generated records sit at the same
    place in both packages: each is identified by the last record key both packages share before it (and that key's
    occurrence) plus its rank after it."""
    wo = [k for k, _ in ctx.wii.ordered(package_key)]
    po = [k for k, _ in ctx.pc.ordered(package_key)]
    common = set(wo) & set(po)

    def pc_gen(k):
        return k >> 20 == PC_GENERATED_TOP and k not in ctx.wii_rec and ctx.wii_big.find(k) is None

    def signatures(order, gen):
        out, seen, occ = [], set(), {}
        anchor, run = None, 0
        for k in order:
            if k in common:
                occ[k] = occ.get(k, 0) + 1
                anchor, run = (k, occ[k]), 0
            elif gen(k) and k not in seen:
                seen.add(k)
                out.append((k, (anchor, run)))
                run += 1
        return out

    pc_by_sig = {s: k for k, s in signatures(po, pc_gen)}
    km = KeyMap()
    for k, s in signatures(wo, is_wii_generated):
        if s in pc_by_sig:
            km[k] = pc_by_sig[s]
    return km


# ============================================================================ the converter entry point
def convert(kind: str, key: int, wii_body: bytes, ctx=None, keymap=None) -> bytes:
    if wii_body is None:
        raise ValueError("%08X: no Wii data" % key)
    if kind == "world":
        return convert_world(wii_body, keymap)
    if kind in ("object", "bone"):
        return convert_object(wii_body, keymap)
    if kind == "spec:grp":
        return convert_grp(wii_body, keymap)
    if kind == "mgf":
        return convert_mgf(wii_body, ctx, keymap)
    if kind in ("mgm", "spec:mgm"):
        _parse(MW.mgm, wii_body)
        return wii_body
    if kind == "fbf":
        return wii_body
    raise KeyError("the small-record converter does not convert %r" % kind)
