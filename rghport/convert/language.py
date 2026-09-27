"""Language data: FCF language indexes, language bins, Txg language references.

    language.convert("txg", key, wii_body, ctx)                        # refs (en, ja) -> fr en nl de it es, all on the English Txl
    language.convert("txl", key, wii_body, ctx)                        # same format on both platforms: checked and returned
    language.convert("fcf", 0xFCF00EC6, ctx.wii_entry(0xFCF00EC6), ctx)  # 2 entries -> 6 entries, all on the English bin
    language.convert("langbin", 0x08915691, ctx.wii_entry(0x08915691), ctx)
    files = language.world_language_files(ctx, 0x09200EC6)             # {FCF key: bytes, English bin key: bytes}

What is where (loader addresses of the PC executable):
  fcf0XXXXX.bin (key 0xFCF00000 | world low 20 bits, BIG_ChunkDecompress mode 4): u32 n, n x {bin key, 0, language}.
      ViD::LoadTexts (004EBAA0) takes the entry whose language is TXT::mu32_NextCurrentLang; no entry -> the world's
      texts are not loaded at all (there is no fallback language).  Wii: languages (1, 12) on the Japanese disc,
      (0, 1, 7) on the US one; PC: (0, 1, 3, 5, 6, 7).
  language bin (the entry's key, mode 6): u32 size, `size` bytes of records {u32 key, u32 len, body} in LOA resolve
      order (len 0 = a key this bin already loaded), then the tail: one item {u32 n, n bytes} per Txl entry with a
      sound key, visited in record order (TXT::TXT_LoadLangData2 004FC620 -> TXL_LoadLangData2 004FB1B0 ->
      FUN_0049a860: u32 n, SLib object vtable+0x28, skip to n).  An item is non-empty exactly when the entry's sound
      is an SLib set met for the first time in this bin; it holds that set's sample bin data.
  records in a bin: Txl (version 4, identical on both platforms), SLib sets (identical), samples (RIFF: Wii 0x5050
      header + .wii.sns link; PC 0x3156 with inline data - the sound converter handles them through Hooks).
  Txg (package record): entries, then refs {Txl key, 0, language}; TXT::TXG_LoadLangData (004FC090) loads the ref whose
      language matches, so every PC language needs a ref.

Policy: the converted archive gets one slot per PC language (pc_slots: the PC release's six, plus every other language
the Wii archive carries, e.g. 12 on the Japanese disc).  A slot names the Wii Txl / bin of its own language when the
Wii archive has that language, else the source language's (English) - same keys, no copies, so a language the disc
lacks shows English texts.  Keys stay the Wii keys: the PC archive holds no key in 0x089xxxxx and BIG looks keys up
exactly (BIG::i64_KeySearchPos 006E9DB0).  The language of a slot is what /lang/<xx> selects: LANG_SHORT_NAMES.
"""
from __future__ import annotations

import collections
import struct

from ..formats import sound as S
from ..formats import stream as F
from ..formats import text as T
from ..formats.stream import FormatError, Ref

EN, JA = 1, 12
PC_LANGS = (0, 1, 3, 5, 6, 7)       # fr en nl de it es: every PC FCF list and Txg reference list, in this order
WII_LANGS = (EN, JA)
SOURCE_LANG = EN
# TXT_LangShortName of the PC executable (009C2580; the Wii one at 8055CE40 is the same list): the language index of
# each /lang/<xx> code.
LANG_SHORT_NAMES = ("fr", "en", "da", "nl", "fi", "de", "it", "es", "pt", "sv", "pl", "ru", "ja", "zh", "sq", "ar",
                    "bg", "be", "el", "ko", "no", "ro", "sr", "sk", "sl", "tr", "cs", "hu", "tw", "u0", "u1", "u2")
LANG_INDEX = {name: i for i, name in enumerate(LANG_SHORT_NAMES)}
NO_KEY = 0xFFFFFFFF
NO_REF = T.NO_REF                   # BIG ref priv bit: not a language reference (TXG_LoadLangData skips it)
KINDS = ("txg", "txl", "fcf", "langbin")
TXL, SET, SAMPLE = "txl", "set", "sample"


def _km(keymap, key: int) -> int:
    if keymap is None or not key or key == NO_KEY:
        return key
    return keymap(key) if callable(keymap) else keymap.get(key, key)


def fcf_key(key: int) -> int:
    """The language index of a world or package key (BIG_ChunkDecompress mode 4)."""
    return 0xFCF00000 | (key & 0xFFFFF)


def pc_slots(source_langs) -> tuple[int, ...]:
    """The language slots of the converted archive: the PC release's six, plus every other language the Wii data
    carries (12, Japanese, on the Japanese disc), in language-index order."""
    return tuple(sorted(set(PC_LANGS) | set(source_langs)))


def slot_keys(available: dict[int, int], langs, source_lang: int = SOURCE_LANG) -> list[tuple[int, int]]:
    """[(language, key)] for the slots `langs`: a language's own key when `available` (language -> Wii key) has it,
    else the source language's."""
    src = available.get(source_lang)
    if not src:
        raise FormatError("no language-%d text (languages %s)" % (source_lang, sorted(available)))
    return [(lg, available.get(lg) or src) for lg in langs]


def archive_languages(big) -> list[int]:
    """The languages the FCF indexes of a Wii archive name (sorted)."""
    return T.lang_data(big).languages()


def language_names(langs) -> dict[str, int]:
    """{short name: language index} of language indexes, in the given order."""
    return {LANG_SHORT_NAMES[lg]: lg for lg in langs if 0 <= lg < len(LANG_SHORT_NAMES)}


# ============================================================================ FCF language index
def parse_fcf(data: bytes) -> list[Ref]:
    return T.parse_text_list(data)


def build_fcf(refs) -> bytes:
    out = bytearray(struct.pack("<I", len(refs)))
    for r in refs:
        out += struct.pack("<III", r.key, r.priv, r.user)
    return bytes(out)


def convert_fcf(data: bytes, keymap=None, source_lang: int = SOURCE_LANG, langs=None) -> bytes:
    """A Wii index (en, ja / fr, en, es) -> the PC index: one entry per slot (default pc_slots of the index's
    languages), each on its own language's bin when the index has one, else on the source-language bin."""
    refs = parse_fcf(data)
    if not refs:
        return build_fcf([])
    available = {}
    for r in refs:
        if r.key:
            available.setdefault(r.user, r.key)
    if langs is None:
        langs = pc_slots(available)
    try:
        slots = slot_keys(available, langs, source_lang)
    except FormatError as ex:
        raise FormatError("language index has %s" % ex) from None
    return build_fcf([Ref(_km(keymap, key), 0, lg) for lg, key in slots])


# ============================================================================ Txg language references
def convert_txg(body: bytes, keymap=None, source_lang: int = SOURCE_LANG, langs=None) -> bytes:
    """Replace the Txg's language references by one per slot (default pc_slots of the Txg's languages), each naming
    its own language's Txl when the Txg has one, else the source-language Txl."""
    o = F.parse(T.txg, body)
    by_lang = T.txg_languages(o)
    if not by_lang:
        return body
    if langs is None:
        langs = pc_slots(by_lang)
    try:
        slots = slot_keys(by_lang, langs, source_lang)
    except FormatError as ex:
        raise FormatError("Txg has %s" % ex) from None
    other = [r for r in o["refs"] if not r.key or (r.priv & NO_REF)]
    o["refs"] = [Ref(_km(keymap, key), 0, lg) for lg, key in slots] + other
    out, _ = F.emit(T.txg, o)
    return out


def txg_refs(body: bytes) -> list[Ref]:
    return F.parse(T.txg, body)["refs"]


# ============================================================================ language bins
def parse_tail(tail: bytes) -> list[bytes]:
    """The bytes after the records: items {u32 n, n bytes}."""
    items, o = [], 0
    while o < len(tail):
        if o + 4 > len(tail):
            raise FormatError("language bin tail: %d stray bytes" % (len(tail) - o))
        n = struct.unpack_from("<I", tail, o)[0]
        if o + 4 + n > len(tail):
            raise FormatError("language bin tail: item of %d bytes overruns at %#x" % (n, o))
        items.append(tail[o + 4:o + 4 + n])
        o += 4 + n
    return items


def build_lang_bin(records, items) -> bytes:
    rec = bytearray()
    for k, b in records:
        rec += struct.pack("<II", k, len(b))
        rec += b
    out = bytearray(struct.pack("<I", len(rec)))
    out += rec
    for it in items:
        out += struct.pack("<I", len(it))
        out += it
    return bytes(out)


def classify(body: bytes):
    """(kind, parsed) of a non-empty bin record: a RIFF sample, a Txl, or an SLib set (parsed None when the set stream
    does not read; it is still no Txl and no sample, which is all the tail rule needs)."""
    if body[4:8] == b"RIFF":
        return SAMPLE, None
    try:
        return TXL, F.parse(T.txl, body)
    except Exception:                    # noqa: BLE001 - not a Txl
        pass
    try:
        return SET, F.parse(S.sound_set, body)
    except Exception:                    # noqa: BLE001
        return SET, None


class LangBin:
    """A parsed language bin (either platform).

    records   [(key, body)] in stored order (body b"" for a placeholder)
    items     the tail items
    body      key -> first non-empty body;  kind  key -> "txl" / "set" / "sample"
    txl       key -> parsed Txl;  sets  key -> parsed set (or None)
    plan      one entry per tail item: the set key whose bin data the item holds, or None (empty item)
    """

    def __init__(self, data: bytes):
        self.records, tail = T.parse_lang_bin(data)
        self.items = parse_tail(tail)
        self.body: dict[int, bytes] = {}
        for k, b in self.records:
            if b and k not in self.body:
                self.body[k] = b
        self.kind: dict[int, str] = {}
        self.txl: dict[int, dict] = {}
        self.sets: dict[int, dict | None] = {}
        for k, b in self.body.items():
            kd, o = classify(b)
            self.kind[k] = kd
            if kd == TXL:
                self.txl[k] = o
            elif kd == SET:
                self.sets[k] = o
        self.sample_user: dict[int, int] = {}
        for sk, o in self.sets.items():
            for r in (o or {}).get("refs", []):
                if not (r.priv & NO_REF) and self.kind.get(r.key) == SAMPLE:
                    self.sample_user.setdefault(r.key, r.user)
        self.plan = self._plan()

    def _plan(self) -> list:
        seen, plan = set(), []
        for k, _ in self.records:
            t = self.txl.get(k)
            if t is None:
                continue
            for e in t["entries"]:
                sk = e["sound_key"]
                if sk in (0, NO_KEY):
                    continue
                is_set = self.kind.get(sk) == SET
                plan.append(sk if is_set and sk not in seen else None)
                if is_set:
                    seen.add(sk)
        return plan

    def txl_order(self) -> list[int]:
        """The Txl keys in TXG_LoadLangData order (placeholders included)."""
        return [k for k, _ in self.records if k in self.txl]

    def set_refs(self, set_key: int) -> list[int]:
        """The LOA_MakeFileRef order of a set: its chunk references (model set, insert / send curves) as the chunks are
        read, then its file references (LoadReferences skips priv 0x10000000 and user word 3)."""
        o = self.sets.get(set_key)
        if o is None:
            return []
        chunk = []
        for c in o.get("chunks", []):
            t = c.get("tag")
            if t in (5, 0x1A):
                chunk.append(c.get("value", 0))
            elif t == 0x12:
                chunk += S.insert_curve_keys(c.get("insert", {}))
            elif t == 0x14:
                chunk.append(c.get("send", {}).get("curve", 0))
        for it in o.get("inserts", []):
            chunk += S.insert_curve_keys(it)
        for it in o.get("aux_sends", []):
            chunk.append(it.get("send", {}).get("curve", 0))
        files = [r.key for r in o.get("refs", []) if not (r.priv & NO_REF) and r.user != 3]
        return [k for k in chunk + files if k and k != NO_KEY]

    def expected_order(self) -> list[tuple[int, bool]]:
        """[(key, non-empty)] as LOA_Resolve produces the records: a FIFO queue seeded with the Txl-level references;
        loading a Txl queues its entries' sound / lips / expression keys, loading a set its set_refs; a key already
        loaded - or never loaded by this bin (e.g. a curve of the world package) - gives a placeholder.  Reproduces
        every language bin of both releases (266 Wii, 152 PC en/fr checked)."""
        queue = collections.deque(self.txl_order())
        loaded, out = set(), []
        while queue:
            k = queue.popleft()
            if k in loaded or k not in self.body:
                loaded.add(k)
                out.append((k, False))
                continue
            loaded.add(k)
            out.append((k, True))
            kd = self.kind.get(k)
            if kd == TXL:
                for e in self.txl[k]["entries"]:
                    for x in (e["sound_key"], e["lips_key"], e["expr_key"]):
                        if x and x != NO_KEY:
                            queue.append(x)
            elif kd == SET:
                queue.extend(self.set_refs(k))
        return out

    def order_errors(self) -> list[str]:
        real = [(k, bool(b)) for k, b in self.records]
        exp = self.expected_order()
        if real == exp:
            return []
        i = next((j for j in range(min(len(real), len(exp))) if real[j] != exp[j]), min(len(real), len(exp)))
        return ["record %d of %d/%d: stored %s, the LOA queue gives %s" % (
            i, len(real), len(exp), ["%08X%s" % (k, "" if b else "(empty)") for k, b in real[i:i + 3]],
            ["%08X%s" % (k, "" if b else "(empty)") for k, b in exp[i:i + 3]])]

    def children(self, set_key: int) -> list[Ref]:
        o = self.sets.get(set_key) or {}
        return [r for r in o.get("refs", []) if not (r.priv & NO_REF)]

    def tail_errors(self) -> list[str]:
        if len(self.plan) != len(self.items):
            return ["tail has %d items, the Txl entries with a sound give %d" % (len(self.items), len(self.plan))]
        return ["item %d: %s item for %s" % (i, "empty" if not it else "%d-byte" % len(it),
                                            "set %08X (first visit)" % p if p is not None else "a revisit / non-set")
                for i, (p, it) in enumerate(zip(self.plan, self.items)) if (p is not None) != bool(it)]

    def census(self) -> dict:
        c = collections.Counter(self.kind.values())
        return {"records": len(self.records), "placeholders": sum(1 for _, b in self.records if not b),
                "txl": c[TXL], "sets": c[SET], "samples": c[SAMPLE], "items": len(self.items),
                "items_non_empty": sum(1 for x in self.items if x),
                "items_data": sum(1 for x in self.items if len(x) > 4)}


class Hooks:
    """Records inside a language bin that the sound converter owns.

    sample(key, wii_body, user, ctx) -> PC sample record: `sound.convert("slib:<user>", key, body, ctx)`.  `user` is
        the SLib file type of the set reference (1 RAM, 2 streamed).
    bin_data(set_key, wii_item, lang_bin, ctx) -> PC tail item for a set's first visit: `sound.lang_bin_data(...)`.
    With sound=None both return the Wii bytes unchanged (counted in `passed`).
    """

    def __init__(self, sound="auto"):
        if sound == "auto":
            from . import sound
        self.sound = sound
        self.passed = collections.Counter()

    def sample(self, key, body, user, ctx):
        fn = getattr(self.sound, "convert", None) if self.sound is not None else None
        if fn is None:
            self.passed["sample"] += 1
            return body
        return fn("slib:%d" % user, key, body, ctx)

    def bin_data(self, set_key, item, lang_bin, ctx):
        fn = getattr(self.sound, "lang_bin_data", None) if self.sound is not None else None
        if fn is None:
            self.passed["bin_data"] += 1
            return item
        return fn(set_key, item, lang_bin, ctx)


def convert_lang_bin(data: bytes, ctx=None, hooks: Hooks | None = None, normalize_text: bool = False) -> bytes:
    """A Wii language bin -> the PC bin: same record order and placeholders (checked against the LOA queue order), Txl
    and sets unchanged (Txl strings through normalize_txl when asked), samples and their tail bin data through `hooks`,
    the tail rebuilt item by item."""
    lb = LangBin(data)
    errs = lb.tail_errors() or lb.order_errors()
    if errs:
        raise FormatError("language bin does not follow the loader rules: %s (%d problems)" % (errs[0], len(errs)))
    hooks = hooks or Hooks()
    cache: dict = {}
    recs = []
    for k, b in lb.records:
        if b:
            ck = (k, b)
            if ck not in cache:
                kd = lb.kind.get(k)
                if kd == SAMPLE:
                    cache[ck] = hooks.sample(k, b, lb.sample_user.get(k, 1), ctx)
                elif kd == TXL and normalize_text:
                    cache[ck] = normalize_txl(b)
                else:
                    cache[ck] = b
            b = cache[ck]
        recs.append((k, b))
    items = [hooks.bin_data(p, it, lb, ctx) if p is not None else b"" for p, it in zip(lb.plan, lb.items)]
    return build_lang_bin(recs, items)


PC_TEXT_REPLACE = {" ": " ", "…": "..."}   # code units no PC English Txl contains (Wii: 508 NBSP, 157 ellipses)


def normalize_txl(body: bytes, table=PC_TEXT_REPLACE) -> bytes:
    """Optional: rewrite a Txl's strings the way the PC release wrote them (NBSP -> space, U+2026 -> "...").  The PC
    dialog font is a Magma .mft that is in neither archive, so whether it has these glyphs is unverified.  Returns the
    body unchanged when no string needs it (the pool is rebuilt in entry order otherwise)."""
    o = F.parse(T.txl, body)
    old = T.txl_strings(o)
    new = {}
    for tid, s in old.items():
        for a, b in table.items():
            s = s.replace(a, b)
        new[tid] = s
    if new == old:
        return body
    T.set_txl_strings(o, new)
    return F.emit(T.txl, o)[0]


# ============================================================================ package level
def world_language_files(ctx, key: int, hooks: Hooks | None = None, keymap=None) -> dict[int, bytes]:
    """The converted language files of one Wii world (or package) key: {FCF key: index, bin key: bin} for every
    language bin the index names.  Empty when the Wii archive has no index for it (a world without TXT modifiers never
    reads one)."""
    fk = fcf_key(key)
    data = ctx.wii_entry(fk)
    if data is None:
        return {}
    out = {fk: convert_fcf(data, keymap)}
    for r in parse_fcf(data):
        if not r.key or _km(keymap, r.key) in out:
            continue
        raw = ctx.wii_entry(r.key)
        if raw is None:
            raise FormatError("language bin %08X of %08X is not in the Wii archive" % (r.key, fk))
        out[_km(keymap, r.key)] = convert_lang_bin(raw, ctx, hooks)
    return out


# ============================================================================ the converter entry point
def convert(kind: str, key: int, wii_body: bytes, ctx=None, hooks: Hooks | None = None, keymap=None) -> bytes:
    """kind: "txg" (package record), "txl" (bin record), "fcf" (index entry), "langbin" (bin entry)."""
    if wii_body is None:
        raise ValueError("%08X: no Wii data" % key)
    if kind == "txg":
        return convert_txg(wii_body, keymap)
    if kind == "txl":
        F.parse(T.txl, wii_body)
        return wii_body
    if kind == "fcf":
        return convert_fcf(wii_body, keymap)
    if kind == "langbin":
        return convert_lang_bin(wii_body, ctx, hooks)
    raise KeyError("the language converter does not convert %r" % kind)
