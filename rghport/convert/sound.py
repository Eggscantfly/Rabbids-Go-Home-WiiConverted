"""Sound: Wii SLib samples, sound bins (FDF0W) and smx records -> the data the RGH PC executable loads.

Wii audio is kept: the PC executable plays the Wii's 0x5050 DSP-ADPCM data.  convert() returns Wii sample records
unchanged, convert_bin() / lang_bin_data() return the Wii bytes, and only smx records are rewritten (convert_smx: the PC
aux loaders read EAX fields).  Streamed Wii sample records name ".wii.sns": in binary loading the executable opens
"<main bigfile path minus extension>.wii.sns.bf" (BIG_S_P4::b_OpenDynAccess 006BDA80), so the port ships the Wii
RGH.wii.sns.BF under that name (see build.py).

PC executable facts this relies on:
  * SLib_wave create 004A5C60 walks the RIFF (fmt/fact/cue/LIST/labl/ltxt/data) and builds the codec with the factory
    004B9710: fmt tag 1 SLib_codec_pcm_S (004C6F80), 0x3156 SLib_codec_oggvorbis1_S (004C7770), 0x5050
    SLib_codec_ngcadpcm_S (004C7C90, the Wii's DSP-ADPCM with the same fixed 8-pair coefficient table at 008A70D8).
  * In binary loading every byte a codec reads is taken from the load stream in order (004A6790 -> 004A5B00): a Wii
    record holds only the header because the ngcadpcm constructor reads nothing; its bin entry holds the whole data
    because decode-all reads the frames in order.  The FDF bins have the same container and recursive block layout on
    both platforms, and ngcadpcm decode-all takes exactly each RAM block.
  * LOA_Resolve (006D8500) reads {key, len} and hands the record to its callback without seeking to the record end,
    so every callback must consume exactly `len` bytes.  A streamed sample record (user word 2) is consumed by
    SLib_wave create: the header walk, then 004A58E0 looks the file key up in the main bigfile (i64_KeySearchPos) and
    004A5920 calls BIG vfunc +0x40 (b_OpenDynAccess, which reads the 40-byte stream link from the load stream) only
    when the key was found; the Wii archive's .sns shortcut entries (copied as they are) provide those keys.
"""
from __future__ import annotations

import struct

from ..formats import sound as MS
from ..formats import stream as F


class SndError(Exception):
    pass


class _PosReader(F.Reader):
    """A stream Reader noting where every present scalar is read (the stream functions stay unchanged)."""

    def __init__(self, *a, **k):
        super().__init__(*a, **k)
        self.at: list[tuple[str, int, object]] = []

    def _scalar(self, o, name, fmt, size, mode, default):
        p = self.pos
        v = super()._scalar(o, name, fmt, size, mode, default)
        if self.present(mode):
            self.at.append((name, p, v))
        return v


# =====================================================================================================================
# smx: platform aux effect blocks (Wii 0xEA54 carries data, PC 0xEA53 = EAX carries data)
EAX_FIELDS = {1: "uffiiifffiffffiffffffffffffu", 0: "fffff", 6: "uiffff"}     # reverb, echo, chorus
WII_FIELDS = {1: "uffuffffff", 0: "ffff", 6: "ffff"}


def _words(fmt: str, vals) -> bytes:
    return b"".join(struct.pack("<" + {"u": "I", "i": "i", "f": "f"}[c], v) for c, v in zip(fmt, vals))


def eax_from_wii(typ: int, wii: list, tail: int):
    """Parametric Wii -> EAX parameters (fields EAX_FIELDS[typ], tail).  Constants are the values every PC record of the
    levels both releases share uses; the varying ones follow the Wii parameters.  The PC executable does not apply EAX
    through DirectSound property sets (no EAX GUID in the binary)."""
    half_tail = int(round(tail / 200.0)) * 100
    if typ == 1:
        early_mode, pre_max, pre, fused_mode, time_, color, damp, xtalk, early_g, fused_g = wii
        decay = min(max(time_ + 0.5, 0.1), 20.0)
        vals = [26, 7.5, 1.0, 0, -100, 0, decay, 0.6, 1.0, -200, min(max(pre * 1.3, 0.0), 0.3), 0.0, 0.0, 0.0,
                200, 0.01, 0.0, 0.0, 0.0, 0.075, 0.0, 0.25, 0.0, 0.0, 1000.0, 250.0, 0.0, 0]
        return vals, half_tail
    if typ == 0:
        delay, delay2, fb, out = wii
        vals = [min(max(delay / 1000.0, 0.0), 0.207), 0.0, min(max(1.0 - fb, 0.0), 0.99), min(max(out + 0.1, 0.0), 1.0),
                0.0]
        return vals, half_tail
    if typ == 6:
        base, variation, period, fb = wii
        vals = [1, 90, min(max(1000.0 / period if period else 1.1, 0.0), 10.0), min(max(variation, 0.0), 1.0),
                min(max(fb, -1.0), 1.0), min(max(base / 1000.0, 0.0), 0.016)]
        return vals, half_tail
    raise SndError("aux type %d" % typ)


def convert_smx(wii_body: bytes, eax=eax_from_wii) -> bytes:
    """Wii smx record -> PC: every aux effect's 0xEA53 block gets its EAX fields (+ tail word when the flag is set) and
    the 0xEA54 block keeps only its flag word; all other bytes (memsize included) are unchanged."""
    st = _PosReader(wii_body, 3, True)
    MS.smx(st, {})
    if st.pos != len(wii_body):
        raise SndError("smx: consumed %d of %d" % (st.pos, len(wii_body)))
    at = st.at
    edits = []                                                  # (start, end, replacement)
    i = 0
    while i < len(at):
        name, p, v = at[i]
        if name == "tag" and v == 3 and i + 2 < len(at) and at[i + 1][0] == "slot" and at[i + 2][0] == "type":
            typ = at[i + 2][2]
            j = i + 3
            blocks = []
            while j < len(at):
                if at[j][0] == "tag":
                    blocks.append((at[j][2], at[j][1]))
                    if at[j][2] == 0xFFFFFFFF:
                        break
                j += 1
            ranges = {blocks[k][0]: (blocks[k][1], blocks[k + 1][1]) for k in range(len(blocks) - 1)}
            if 0xEA53 in ranges and 0xEA54 in ranges and typ in EAX_FIELDS:
                s53, e53 = ranges[0xEA53]
                s54, e54 = ranges[0xEA54]
                flag53 = struct.unpack_from("<I", wii_body, s53 + 4)[0]
                flag54 = struct.unpack_from("<I", wii_body, s54 + 4)[0]
                n = len(WII_FIELDS[typ])
                if e53 - s53 != 8 or e54 - s54 != 8 + 4 * n + (4 if flag54 else 0):
                    raise SndError("smx aux %d: unexpected Wii block sizes %d / %d" % (typ, e53 - s53, e54 - s54))
                raw = wii_body[s54 + 8:s54 + 8 + 4 * n]
                wii_vals = [struct.unpack_from("<" + {"u": "I", "f": "f"}[c], raw, 4 * q)[0]
                            for q, c in enumerate(WII_FIELDS[typ])]
                tail = struct.unpack_from("<i", wii_body, s54 + 8 + 4 * n)[0] if flag54 else 0
                vals, ptail = eax(typ, wii_vals, tail)
                rep53 = struct.pack("<II", 0xEA53, flag53) + _words(EAX_FIELDS[typ], vals) + \
                    (struct.pack("<i", ptail) if flag53 else b"")
                rep54 = struct.pack("<II", 0xEA54, flag54)
                edits.append((s53, e53, rep53))
                edits.append((s54, e54, rep54))
            i = j + 1
        else:
            i += 1
    out = bytearray()
    cur = 0
    for s, e, rep in sorted(edits):
        out += wii_body[cur:s] + rep
        cur = e
    out += wii_body[cur:]
    return bytes(out)


# =====================================================================================================================
# sound groups: the Options screen's music and effects volumes follow the mix groups (platform/wm_options.cpp: music =
# USER_MUSIC 2 and below, effects = SFX 10, AMB 11, HUD 14 and below, voices = DIALOG 13).  The mix config
# (Mix_config_Bunnies.smx, loose file) defines 19 groups, none with an insert or an aux send: a group only adds its base
# volume to its parent's.  Most sets sit in the group of their kind, but some do not: jingles (Mus_Jingle_*) in MASTER
# (0), a band tune in Characters (24), radio songs in SFX (10) and the ambience buffers (31-33), interface sounds in
# MASTER.  Those sets move to MUSIC (12) or SFX (10) when the move keeps their volume (same summed base volume): they
# sound exactly as before and follow the right slider.  A set's kind comes from the names of the streamed samples it
# plays (the .sns shortcut entries of the archive): Mus_*, *Music*, Radio_*, Custo_zik* are music, language-prefixed
# ones (EN_ ...) voices; a set without named samples counts as sound effects.
GROUP_MASTER, GROUP_SFX, GROUP_USER_MUSIC, GROUP_MUSIC = 0, 10, 2, 12
MUSIC_PREFIXES = ("mus_", "radio_", "custo_zik")
VOICE_PREFIXES = ("en_", "ja_", "fr_", "de_", "es_", "it_", "nl_")


class MixGroups:
    """The mix config's groups and the archive's streamed sample names, read once per conversion."""

    def __init__(self, big):
        self.parent: dict[int, int] = {}
        self.base: dict[int, int] = {}
        self.names: dict[int, str] = {}
        self.moved = {"music": 0, "effects": 0, "kept (volume would change)": 0}
        smx = None
        for e in big.entries():
            if e.ext == "sns":
                self.names[e.key] = e.name
            elif e.ext == "smx" and smx is None:
                smx = big.read(e)
        if smx:
            o = F.parse(MS.smx, smx, level=0, binary=False, strict=False)
            for c in o.get("chunks", []):
                g = c.get("group") if c.get("tag") == 8 else None
                if g and "i0" in g:
                    self.parent[g["i0"]] = g["i1"]
                    self.base[g["i0"]] = g["i2"]

    def level(self, g: int) -> int | None:
        """Summed base volume from the group to the root (None for a group the mix does not define)."""
        total, seen = 0, set()
        while g in self.base and g not in seen:
            seen.add(g)
            total += self.base[g]
            p = self.parent[g]
            if p == g or p not in self.base:
                return total
            g = p
        return None if not seen else total

    def below(self, g: int, ancestor: int) -> bool:
        seen = set()
        while g in self.parent and g not in seen:
            if g == ancestor:
                return True
            seen.add(g)
            g = self.parent[g]
        return g == ancestor

    def kind(self, child_keys) -> str:
        found = set()
        for k in child_keys:
            n = self.names.get(k, "").lower()
            if not n:
                continue
            if n.startswith(MUSIC_PREFIXES) or "music" in n:
                found.add("music")
            elif n.startswith(VOICE_PREFIXES):
                found.add("voice")
            else:
                found.add("effects")
        if "music" in found:
            return "music"
        return "voice" if found == {"voice"} else "effects"


def mix_groups(ctx) -> MixGroups | None:
    if ctx is None or getattr(ctx, "wii_big", None) is None:
        return None
    mg = getattr(ctx, "_mix_groups", None)
    if mg is None:
        mg = ctx._mix_groups = MixGroups(ctx.wii_big)
    return mg if mg.base else None


def regroup_set(body: bytes, mg: MixGroups) -> bytes:
    """A sound set record with its group chunk (tag 4) moved to the group of its kind when that keeps its volume."""
    st = _PosReader(body, 3, True)
    o: dict = {}
    try:
        MS.sound_set(st, o)
    except Exception:           # noqa: BLE001 - not a set record: unchanged
        return body
    at = st.at
    pos = [at[i + 1][1] for i in range(len(at) - 1) if at[i][0] == "tag" and at[i][2] == 4 and at[i + 1][0] == "value"]
    if len(pos) != 1:
        return body             # no own group (the model's or the default) or several: unchanged
    group = struct.unpack_from("<I", body, pos[0])[0]
    kind = mg.kind(r.key for r in o.get("refs", []))
    target = None
    if kind == "music" and not mg.below(group, GROUP_USER_MUSIC):
        target, label = GROUP_MUSIC, "music"
    elif kind == "effects" and group == GROUP_MASTER:
        target, label = GROUP_SFX, "effects"
    if target is None:
        return body
    if mg.level(group) is None or mg.level(group) != mg.level(target):
        mg.moved["kept (volume would change)"] += 1
        return body
    mg.moved[label] += 1
    out = bytearray(body)
    struct.pack_into("<I", out, pos[0], target)
    return bytes(out)


# =====================================================================================================================
def convert(kind: str, key: int, wii_body: bytes, ctx=None) -> bytes:
    """PC package record for a Wii record of the sound kinds:
         slib:1 / slib:2 / slib:10 / sample   sample record: unchanged
         slib:5                               smx record: aux platform blocks rewritten (convert_smx)
         slib:3 / None                        sets: the group moves of regroup_set, otherwise unchanged
         slib:7 / 6 / 8                       unchanged (identical on both platforms)
    """
    if kind in ("slib:1", "slib:2", "slib:10", "sample"):
        return wii_body
    if kind == "slib:5":
        return convert_smx(wii_body)
    if kind in ("slib:3", "slib:None"):
        mg = mix_groups(ctx)
        return regroup_set(wii_body, mg) if mg is not None else wii_body
    if kind in ("slib:7", "slib:6", "slib:8"):
        return wii_body                     # smp, mic, sva: platform-identical
    raise SndError("sound converter: kind %r" % kind)


def convert_bin(key: int, data: bytes, ctx=None) -> bytes:
    """FDF0W sound bin: the Wii bin unchanged (same container and block layout as the PC release's bins)."""
    return data


def lang_bin_data(set_key: int, wii_item: bytes, lang_bin, ctx) -> bytes:
    """Tail item of a set in a language bin (language.Hooks.bin_data): unchanged."""
    return wii_item
