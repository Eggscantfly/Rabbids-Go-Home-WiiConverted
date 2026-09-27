"""Package building: every record of a Wii package, in stored order, through the first rule that applies.

  1. an empty placeholder record stays empty;
  2. an override (a PC-format record body given for that key, see load_overrides) wins;
  3. a script record (kind "script:*") goes to the scripts phase (hooks.py: `convert(kind, key, body, ctx)`) when it
     provides one; a None result leaves the record to the rules below;
  4. a record no walk typed gets a kind from its content (small.is_mgf, walk.kinds.classify_untyped);
  5. a record still untyped whose bytes equal the PC release's record of the same key passes;
  6. `twins`: "scripts" (the Wii-exact build) takes the PC release's record for script records the PC archive holds,
     "all" for every record the PC archive holds, "none" never;
  7. kinds byte-identical on both platforms pass through;
  8. converted kinds go through their converter (`convert(kind, key, body, ctx)`);
  9. anything else is missing: an error, or (allow_missing) the Wii bytes, listed in the summary.

A script model taken from the PC release (rule 6) names its tracks and lists by key, and the loader reads them from the
package in that order: a key the Wii package does not hold is searched for past the end of the package (the engine
stops responding).  The US disc's build lacks one track the PC release and the Japanese disc have
(GST_SaveManager_State_WiiWare, 0E009029), so such records are added from the PC archive, right after the record of
the nearest earlier reference of the model that the package holds (Builder.add_twin_script_records).
"""
from __future__ import annotations

import collections
import importlib
import os
import struct

from ..formats import script as script_format
from ..formats import stream as stream_format
from ..walk.kinds import MODEL_MIN_SIZE, classify_untyped
from . import small
from .hooks import SCRIPT_KIND_PREFIX, ScriptHooks

# byte-identical on both platforms in every level the two releases share
IDENTICAL = {"atomic", "material", "vis", "vif", "bone", "spec:act", "spec:ack", "spec:skl", "spec:aci", "spec:curve",
             "spec:dst", "spec:lg", "spec:cob", "spec:zde", "spec:lig", "spec:par", "spec:particle_model", "spec:snk",
             "spec:snf", "spec:snl", "spec:snl_link", "spec:eve", "spec:tbo", "spec:tbo_shape", "spec:txd", "spec:txt",
             "spec:mpk", "spec:bank", "spec:gmatlib", "spec:lrp", "spec:dyn", "spec:grprefs", "spec:fod", "spec:afx",
             "spec:trg", "spec:net", "spec:occ", "spec:eff", "spec:bvo", "spec:mssg", "spec:rdp", "spec:lip",
             "spec:ani", "spec:cam", "spec:veg", "slib:7", "script:father", "script:init",
             "spec:vp", "empty", "jpeg", "magma_font", "spec:scr", "shader"}
# spec:scr (script instances): the only Wii/PC difference is a runtime pointer saved inside trigger/signal values
# (+0x50), which the PC loader zeroes and resolves again by key and name; variables bind by name, so Wii instance
# records pass through.  spec:veg: 310 pairs in the shared levels, all identical.

# kind -> converter module; kinds partly identical (object, texture) still go through theirs.  Script records have no
# converter here: they are the scripts phase's (rule 3, hooks.py).
CONVERTERS = {"visual": ".geometry", "texture": ".textures", "spec:trl": ".animation", "spec:skn": ".animation",
              "slib:1": ".sound", "slib:2": ".sound", "slib:10": ".sound", "slib:5": ".sound", "sample": ".sound",
              "slib:3": ".sound", "slib:None": ".sound",      # sets: sound.regroup_set
              "txg": ".language", "world": ".small", "object": ".small", "spec:grp": ".small",
              "mgf": ".small"}           # Magma descriptors (records small.is_mgf recognizes)


NEEDS_FILE = "needs.txt"       # in a layer: "<KEY> <feature>" lines, records that apply only with that feature on
FEATURES = ("options_page",)    # the features a build can leave out (cli.py --no-options-page)


def layer_needs(path: str) -> dict[int, str]:
    """{record key: feature} of a layer's needs.txt: records used only when the build has that feature on."""
    out = {}
    try:
        with open(os.path.join(path, NEEDS_FILE), encoding="utf-8") as f:
            for ln in f:
                parts = ln.split("#", 1)[0].split()
                if len(parts) == 2 and len(parts[0]) == 8:
                    try:
                        out[int(parts[0], 16)] = parts[1]
                    except ValueError:
                        pass
    except OSError:
        pass
    return out


def load_overrides(dirs, features=None) -> dict[int, bytes]:
    """{record key: PC body} from the <KEY>.bin files of each directory; later directories win.  A record a layer's
    needs.txt ties to a feature is skipped when `features` (default: all of FEATURES) lacks it, so an earlier layer's
    record of that key stays."""
    features = set(FEATURES if features is None else features)
    out = {}
    for path in dirs:
        if not os.path.isdir(path):
            continue
        needs = layer_needs(path)
        for fn in os.listdir(path):
            stem, ext = os.path.splitext(fn)
            if ext.lower() == ".bin" and len(stem) == 8:
                try:
                    key = int(stem, 16)
                except ValueError:
                    continue
                if key in needs and needs[key] not in features:
                    continue
                with open(os.path.join(path, fn), "rb") as f:
                    out[key] = f.read()
    return out


def load_entry_overrides(dirs) -> dict[int, bytes]:
    """{bigfile entry key: bytes} from the entries/<KEY>.bin files of each directory; later directories win."""
    return load_overrides([os.path.join(d, "entries") for d in dirs])


def _module(name: str):
    try:
        if name.startswith("."):
            return importlib.import_module(name, __package__)
        return importlib.import_module(name)
    except ImportError:
        return None


def _label(name: str) -> str:
    return name.lstrip(".").rsplit(".", 1)[-1]


def source_of(key: int, kind: str, pc_rec: dict, twins: str = "all", twin_kinds=()) -> str:
    """twins: "all" takes the PC archive's record of any key the PC also has; "scripts" only for script records (Wii data
    converted, PC bytecode where no override replaces it); "none".  twin_kinds: record kinds also taken from the PC
    archive when it holds the key (e.g. "spec:trl": the PC release's own animation lists instead of the Edge bakes)."""
    if key in pc_rec and (twins == "all" or (twins == "scripts" and kind.startswith("script:")) or kind in twin_kinds):
        return "pc_twin"
    if kind in IDENTICAL:
        return "identical"
    if kind in CONVERTERS:
        return "convert:" + _label(CONVERTERS[kind])
    return "unknown"


class Missing(Exception):
    pass


class Builder:
    def __init__(self, ctx, allow_missing: bool, twins: str = "all", overrides: dict[int, bytes] | None = None,
                 scripts: ScriptHooks | None = None, twin_kinds=()):
        self.ctx = ctx
        self.allow_missing = allow_missing
        self.twins = twins
        self.twin_kinds = frozenset(twin_kinds or ())
        self.missing = collections.Counter()
        self.missing_keys: list[str] = []          # "KEY kind len head" of records passed through unconverted
        self.sources = collections.Counter()
        self.errors: list[str] = []
        self.mgf_keys: list[int] = []
        self.converted: dict[int, bytes] = {}      # key -> PC body of the Magma descriptors converted
        self.overrides: dict[int, bytes] = overrides if overrides is not None else {}
        self.script_convert = scripts.convert if scripts is not None else None
        self.twin_models: list[int] = []           # script records of this package replaced (override or PC twin)
        self.added_twin_records: list[str] = []    # "PACKAGE KEY after KEY (model KEY)" of the records added
        self.model_refs: dict[int, list[int] | None] = {}   # replaced script record -> the keys its model names

    def record(self, key: int, kind: str, body: bytes) -> bytes:
        if not body:
            # an empty record: the resource was already loaded from an earlier package of the load
            self.sources["empty placeholder"] += 1
            return body
        if key in self.overrides:
            self.sources["override"] += 1
            if kind.startswith(SCRIPT_KIND_PREFIX):
                self.twin_models.append(key)
            return self.overrides[key]
        if self.script_convert is not None and kind.startswith(SCRIPT_KIND_PREFIX):
            try:
                out = self.script_convert(kind, key, body, self.ctx)
            except Exception as ex:          # noqa: BLE001
                self.errors.append("%08X %s: scripts: %s" % (key, kind, ex))
                if not self.allow_missing:
                    raise
                out = None
            if out is not None:
                self.sources["scripts"] += 1
                return out
        if kind == "?":
            try:
                if small.is_mgf(body):
                    kind = "mgf"
                    self.mgf_keys.append(key)
            except Exception:          # noqa: BLE001 - not an MGF record
                pass
        if kind == "?":
            kind = classify_untyped(body)
        if kind == "?" and key in self.ctx.pc_rec and self.ctx.pc_body(key) == body:
            # nothing types it, but the PC release ships the very same bytes under this key
            self.sources["identical to its PC twin"] += 1
            return body
        src = source_of(key, kind, self.ctx.pc_rec, self.twins, self.twin_kinds)
        if src == "pc_twin":
            pb = self.ctx.pc_body(key)
            if pb is not None:
                self.sources[src] += 1
                if kind.startswith(SCRIPT_KIND_PREFIX):
                    self.twin_models.append(key)
                return pb
            src = "identical" if kind in IDENTICAL else (
                "convert:" + _label(CONVERTERS[kind]) if kind in CONVERTERS else "unknown")
        if src == "identical":
            self.sources[src] += 1
            return body
        mod = _module(CONVERTERS[kind]) if kind in CONVERTERS else None
        fn = getattr(mod, "convert", None) if mod else None
        if fn is not None:
            try:
                out = fn(kind, key, body, self.ctx)
                self.sources[src] += 1
                if kind == "mgf":
                    self.converted[key] = out
                return out
            except Exception as ex:          # noqa: BLE001
                self.errors.append("%08X %s: %s" % (key, kind, ex))
                if not self.allow_missing:
                    raise
        elif not self.allow_missing:
            raise Missing("no converter for %08X kind %s" % (key, kind))
        self.missing[kind] += 1
        if len(self.missing_keys) < 2000:
            self.missing_keys.append("%08X %s len %d head %s" % (key, kind, len(body), body[:24].hex()))
        return body

    def refs_of_model(self, mkey: int, body: bytes) -> list[int] | None:
        """The tracks, procedure lists, rule lists and data sets a replaced script record names, in its order; None when
        it is not a model (the walk's test, walk.kinds.script_models: a record of a model's size that carries
        b"ModelFactory" and parses as a model; here it also names its own source, key - 1).  Kept per key: a replaced
        record has the same bytes in every package."""
        if mkey not in self.model_refs:
            refs = None
            if len(body) >= MODEL_MIN_SIZE and b"ModelFactory" in body:
                try:
                    every = stream_format.parse(script_format.model, body).get("refs") or []
                    if any(r.key == mkey - 1 for r in every):
                        refs = [r.key for r in every if r.user in script_format.REF_USERS]
                except Exception:          # noqa: BLE001 - not a model after all
                    pass
            self.model_refs[mkey] = refs
        return self.model_refs[mkey]

    def add_twin_script_records(self, pk: int, recs: list[tuple[int, bytes]]) -> list[tuple[int, bytes]]:
        """The tracks, procedure lists, rule lists and data sets a replaced model (an override or the PC twin) names
        that the package lacks, from the overrides or the PC archive, each right after the record of the model's
        nearest earlier reference the package holds (the loader asks for them in the model's order and reads the
        package forward)."""
        if not self.twin_models:
            return recs
        held = {k for k, _ in recs}
        bodies = None
        for mkey in self.twin_models:
            if mkey not in self.model_refs:
                if bodies is None:
                    bodies = dict(recs)
                self.refs_of_model(mkey, bodies[mkey])
            refs = self.model_refs[mkey]
            if not refs or all(k in held for k in refs):
                continue
            anchor = mkey
            for rkey in refs:
                if rkey in held:
                    anchor = rkey
                    continue
                pb = self.overrides.get(rkey)
                if pb is None and rkey in self.ctx.pc_rec:
                    pb = self.ctx.pc_body(rkey)
                if pb is None:
                    continue
                at = max(i for i, (k, _) in enumerate(recs) if k == anchor) + 1
                recs.insert(at, (rkey, pb))
                held.add(rkey)
                self.sources["pc_twin (added: the Wii package lacks it)"] += 1
                self.added_twin_records.append("%08X %08X after %08X (model %08X)" % (pk, rkey, anchor, mkey))
                anchor = rkey
        return recs

    def package(self, pk: int, kinds: dict) -> bytes:
        self.twin_models = []
        recs = []
        for key, body in self.ctx.wii.ordered(pk):
            recs.append((key, self.record(key, kinds.get(key, "?"), body)))
        out = bytearray()
        for key, new in self.add_twin_script_records(pk, recs):
            out += struct.pack("<II", key, len(new)) + new
        return bytes(out)
