"""The whole Wii archive converted into one bigfile the RGH PC executable loads, entry by entry.

Entries are written in the Wii archive's disk order (writer.BigStreamWriter, every file raw):
  FFF0xxxx  packages: every record through builder.Builder (record kinds from walk.kinds; overrides first; twins
            "scripts": the PC record only for script records no override replaces)
  FEF0xxxx  texture banks: textures.convert_bank
  FDF0xxxx  sound bins: sound.convert_bin
  FCF0xxxx  language indexes: language.convert_fcf (one slot per PC language, each on its own language's bin when
            the Wii disc has that language - en ja on the Japanese disc, fr en es on the US one -, else on English)
  language bins (the keys the FCF indexes name: 089xxxxx on the Japanese disc, 0163xxxx on the US one): every one
            through language.convert_lang_bin (samples through language.Hooks)
  FBF0xxxx  Magma blobs: as they are (the PC executable loads Wii blobs), except the in-game menu and Common blobs,
            which get the Options screen (menus.patch_blob)
  shadow shortcuts (.sns .bik): as they are; the siblings <out base>.wii.sns.bf (the sample records name ".wii.sns")
            and <out base>.$hd$.bik.bf are copies of the Wii ones (port.copy_siblings)
  other entries: an entry override wins; default.cfg gets PC_EXE_CFG and its key swaps unguarded; the scripts phase's
            convert_entry (hooks.py) may replace the others; everything else as it is (the other script libraries and
            configurations are byte-identical in both archives)
  The scripts phase's prepare(ctx) runs once before the first entry (hooks.py).
  prefixes / only / limit select bins or entries for test runs (the others are not written: not a bootable bigfile).
The summary (counts and seconds per category, builder sources, missing kinds, errors) is returned and written to
<out>.json.
"""
from __future__ import annotations

import collections
import json
import os
import re
import time

from ..archive.bigfile import FILE_HDR
from ..archive.writer import BigStreamWriter
from . import language, menus, sound, textures
from .builder import Builder
from .hooks import ScriptHooks, script_hooks
from .port import SIBLINGS, copy_siblings, find_sibling  # noqa: F401  (re-exported: the siblings belong to the port folder)

# default.cfg (0800B3F8, the key the bigfile header names): the Wii file, with the value the PC executable shipped with
# for an engine-side setting.  SND_ProjectOptions goes into the sound project settings (FUN_0087cb30): PC 13, Wii 3;
# Wii audio runs under 13.  The PC-only BIG_BinkSkipList* lines stay out: every video is in the Wii sibling.  The
# BIG_ShadowBranch* lines differ too, but binary loading never reads them.
CFG_KEY = 0x0800B3F8
PC_EXE_CFG = {"SND_ProjectOptions": "13"}
# Key swaps (LOA_go_SwapRef, applied by LOA_MakeFileRef / LOA_ResolveKey while the array holds entries): default.cfg
# lists 9 of them (Rabbids font 25101159 -> 251067AE and its atlas, IZW_TRC Arial 60001E74 -> 251067A3 and its atlas,
# TRC.mgb 0E0076C2 -> JPN_TRC CB100534, Mail.mgb 251015C4 -> JPN_Mail CB100596, B410191A -> 231001D8) inside
# "#ifdef JA", and BIG_SwapKey_Handler (00501990) defines only the current language code.  The Wii packages were
# binarized with these swaps (every Wii package holds the swapped keys where the PC ones hold the originals), so the
# requests only resolve with the swaps on: LOA_Resolve otherwise searches past the end of the package for 0E0076C2.
# The guard is dropped so the swaps apply in every language.
SWAP_GUARD = "JA"
PROGRESS_SECONDS = 30


def patch_cfg(text: bytes, values: dict) -> bytes:
    for name, value in values.items():
        pat = re.compile(rb"^([ \t]*" + re.escape(name.encode()) + rb"[ \t]*=[ \t]*)([^\r\n]*)", re.M)
        text, n = pat.subn(lambda m: m.group(1) + value.encode(), text)
        if n != 1:
            raise ValueError("default.cfg: %d lines set %s" % (n, name))
    return text


def unguard_swaps(text: bytes, guard: str = SWAP_GUARD) -> bytes:
    """Drop the "#ifdef <guard>" / "#endif" pair around the BIG_SwapRefList block (see SWAP_GUARD)."""
    lines = text.split(b"\n")
    start = next((i for i, l in enumerate(lines) if l.strip() == b"#ifdef " + guard.encode()), None)
    end = None if start is None else next((i for i in range(start + 1, len(lines)) if lines[i].strip().startswith(b"#endif")), None)
    if start is None or end is None or not any(b"BIG_SwapRefList" in l for l in lines[start:end]):
        raise ValueError("default.cfg: no #ifdef %s block holding BIG_SwapRefList" % guard)
    eol = b"\r" if lines[start].endswith(b"\r") else b""
    lines[start] = b"// RGH PC port: the Wii packages were binarized with these key swaps, so they apply in every language" + eol
    del lines[end]
    return b"\n".join(lines)


def language_bins(big) -> dict[int, int]:
    """{language bin key: language} of every bin the FCF indexes name.  The bins are known by the indexes, not by their
    keys: the binarizer's key counter differs per build (0x0891xxxx on the Japanese disc, 0x0163xxxx on the US one)."""
    bins = {}
    for e in big.entries():
        if e.ext == "bin" and e.key >> 20 == 0xFCF:
            for r in language.parse_fcf(big.read(e)):
                if r.key:
                    bins.setdefault(r.key, r.user)
    return bins


SWAP_LINE = re.compile(rb"BIG_SwapRef\[0x([0-9A-Fa-f]+)\]\s*=\s*0x([0-9A-Fa-f]+)")


def swaps_baked(ctx, cfg: bytes) -> bool:
    """True when the archive's packages were binarized with default.cfg's key swaps (the Japanese disc: they hold
    the swap targets - the Japanese fonts and pages - and none of the originals), so the swaps must apply in every
    language.  The US / European disc holds the originals: its swaps stay under their language guard."""
    pairs = [(int(a, 16), int(b, 16)) for a, b in SWAP_LINE.findall(cfg)]
    targets = sum(1 for _a, b in pairs if b in ctx.wii_rec)
    originals = sum(1 for a, _b in pairs if a in ctx.wii_rec)
    return bool(pairs) and targets > originals


def build_bigfile(ctx, kinds: dict[int, str], out: str, overrides: dict[int, bytes] | None = None,
                  entry_overrides: dict[int, bytes] | None = None, twins: str = "scripts", allow_missing: bool = True,
                  prefixes=(), only=(), limit: int = 0, log=print, scripts: ScriptHooks | None = None,
                  twin_kinds=(), options_page: bool = True) -> dict:
    """Convert every entry of ctx.wii_big into the bigfile `out`.  Returns the summary (also written to <out>.json).
    `scripts`: the scripts phase's hooks (default: those of rghport.scripts, see hooks.py).  `options_page`: add the
    OPTIONS entry and the Options page to the menus (menus.py); off, the Magma blobs stay as the Wii had them."""
    t0 = time.time()
    scripts = scripts if scripts is not None else script_hooks()
    if getattr(ctx, "kinds", None) is None:
        ctx.kinds = kinds
    if scripts.prepare is not None:
        scripts.prepare(ctx)
    b = Builder(ctx, allow_missing, twins, overrides, scripts, twin_kinds)
    hooks = language.Hooks()
    wii = ctx.wii_big
    entry_overrides = entry_overrides or {}
    entries = sorted(wii.entries(), key=lambda e: e.pos)
    lang_bins = language_bins(wii)
    languages = language.language_names(sorted(set(lang_bins.values())))
    menu_template = menus.find_template(wii) if options_page else None
    prefixes = set(prefixes)
    only = set(only)
    test_run = bool(prefixes or only or limit)
    os.makedirs(os.path.dirname(os.path.abspath(out)), exist_ok=True)
    w = BigStreamWriter(out, capacity=len(entries) + 256, tick=ctx.pc_big.tick, params=ctx.pc_big.params)
    count = collections.Counter()
    secs = collections.Counter()
    size = collections.Counter()
    errors = []
    bins_done = 0
    last_print = time.time()
    try:
        for i, e in enumerate(entries):
            top = e.key >> 20
            if only and e.key not in only:
                continue
            ln, lu, lr, fl = wii.file_header(e)
            is_bin = e.ext == "bin" and not fl & 1
            if prefixes and not (is_bin and top in prefixes):
                continue
            if limit and is_bin:
                if bins_done >= limit:
                    continue
                bins_done += 1
            refs = wii._read(e.pos + FILE_HDR + ln, lr) if lr else b""
            t1 = time.time()
            try:
                if fl & 1:                                   # shadow shortcut (.sns, .bik)
                    w.add(e.key, e.name, wii._read(e.pos + FILE_HDR, ln), refs, flags=fl, length_user=lu,
                          perm=e.perm_flags)
                    cat = "shortcut"
                    data_out = b""
                elif is_bin:
                    if top == 0xFFF:
                        data_out = b.package(e.key, kinds)
                        b.converted.clear()
                        cat = "package"
                    else:
                        data = wii.read(e)
                        if top == 0xFEF:
                            data_out, cat = textures.convert_bank(data, e.key, ctx), "texture bank"
                        elif top == 0xFDF:
                            data_out, cat = sound.convert_bin(e.key, data, ctx), "sound bin"
                        elif top == 0xFCF:
                            data_out, cat = language.convert_fcf(data), "language index"
                        elif e.key in lang_bins:
                            lg = lang_bins[e.key]
                            data_out = language.convert_lang_bin(data, ctx, hooks)
                            cat = "language bin (%s)" % language.LANG_SHORT_NAMES[lg] if lg < len(language.LANG_SHORT_NAMES) \
                                else "language bin (language %d)" % lg
                        elif top == 0xFBF:
                            new = menus.patch_blob(e.key, data, menu_template) if options_page else None
                            data_out, cat = (new, "Magma blob (Options screen added)") if new is not None \
                                else (data, "bin FBF as is")
                        else:
                            data_out, cat = data, "bin %03X as is" % top
                    w.add(e.key, e.name, data_out, refs, perm=e.perm_flags)
                else:
                    if e.key in entry_overrides:
                        data_out, cat = entry_overrides[e.key], "entry override"
                    elif e.key == CFG_KEY:
                        data_out = patch_cfg(wii.read(e), PC_EXE_CFG)
                        if swaps_baked(ctx, data_out):
                            data_out = unguard_swaps(data_out)
                            cat = "default.cfg (PC executable values, key swaps always on)"
                        else:
                            cat = "default.cfg (PC executable values)"
                    else:
                        data_out = wii.read(e)
                        new = scripts.convert_entry(e.key, e.name, data_out, ctx) if scripts.convert_entry else None
                        if new is not None:
                            data_out, cat = new, "scripts entry"
                        else:
                            cat = "entry as is (.%s)" % e.ext
                    w.add(e.key, e.name, data_out, refs, perm=e.perm_flags)
            except Exception as ex:          # noqa: BLE001
                errors.append("%08X %s: %s: %s" % (e.key, e.name, type(ex).__name__, ex))
                if not allow_missing:
                    raise
                data_out, cat = wii.read(e), "passed through after an error"
                w.add(e.key, e.name, data_out, refs, perm=e.perm_flags)
            count[cat] += 1
            secs[cat] += time.time() - t1
            size[cat] += len(data_out)
            if log and time.time() - last_print > PROGRESS_SECONDS:
                last_print = time.time()
                log("[%4d/%d] %4.0f s  %s" % (i + 1, len(entries), time.time() - t0,
                                              ", ".join("%s %d" % kv for kv in sorted(count.items()))))
    finally:
        n = w.close()
    summary = {"out": os.path.basename(out), "entries": n, "test_run": test_run, "seconds": round(time.time() - t0),
               "options_page": options_page,
               "script_hooks": scripts.present(), "languages": languages,
               "count": dict(count), "seconds_by_category": {k: round(v, 1) for k, v in secs.items()},
               "bytes_by_category": dict(size), "builder_sources": dict(b.sources),
               "builder_missing": dict(b.missing), "language_hooks_passed": dict(hooks.passed),
               "sound_sets_regrouped": dict(getattr(getattr(ctx, "_mix_groups", None), "moved", {}) or {}),
               "errors": errors[:200], "builder_errors": b.errors[:200], "builder_missing_keys": b.missing_keys,
               "pc_twin_records_added": b.added_twin_records[:400]}
    with open(out + ".json", "w") as f:
        json.dump(summary, f, indent=1)
    return summary
