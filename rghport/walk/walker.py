"""The walk from world lists and world groups down to every package record they load, tagging each with a kind.

    w = Walker(Archive(big), record_index)
    w.add_list("B4")                     # every world the list B4.wol names
    w.add_group("MapMonde.wog", walked)  # the lists of a world group not walked yet, then the worlds it names itself
    w.kinds                              # key -> kind ("visual", "material", "spec:trl", "slib:1", "object", ...)

A world record leads to its objects and its modifiers; objects to their modifiers (the SKL skin model first) and their
hierarchy father; every modifier and resource stream to what it points at (registry.py).  Every entry point tags the
key it is handed before reading it: the first kind a key gets stays, except that a "res:<kind>" placeholder (a
reference of a kind no stream reads, e.g. a video key) gives way to a real kind.  A record is walked once; a record
whose stream did not read is not tried again through a Spec.

Record bodies come from the package of the world being walked, then from every package loaded so far (in load order),
then from the package the record index names, then from the language bins.  An empty placeholder record counts as a
body there and fails to read.  Before walking the worlds of a list, the packages of all its worlds are loaded in list
order, which fixes the order of that fallback.
"""
from __future__ import annotations

import collections
import re

from ..archive.packages import Archive
from ..formats import anim as A
from ..formats import geometry as G
from ..formats import sound as MS
from ..formats import stream as F
from ..formats import text as TX
from ..formats import vif as V
from . import registry

NO_KEY = 0xFFFFFFFF
VIS_TYPE = 3
# references to records another walk step handles: a script instance's model (script records are tagged from the
# model records, see kinds.script_tags)
EXTERNAL_KINDS = ("script_model",)


def resolve(big, name: str) -> dict:
    """What a world list (<name>.wol) or world group (<name>.wog) loads: {"entry", "lists": [(list name, key)],
    "worlds": [world keys in load order], "direct": [worlds a group names without a .wol], "containers": [the .wol /
    .wog entries]}.  For a .wol the containers are the list and the groups that load it.  A name ending in ".wog" names
    the group even when a list of the same name exists (_Map_Language, _basic_AsyncLoading, CREDITS)."""
    e = None if name.lower().endswith(".wog") else big.find_name(name + ".wol")
    if e is not None:
        wogs = [g for g in big.entries() if g.ext == "wog" and any(r.key == e.key for r in big.refs(g))]
        return {"entry": e, "lists": [(name, e.key)], "worlds": [r.key for r in big.refs(e)], "direct": [],
                "containers": [e] + wogs}
    g = big.find_name(name if name.lower().endswith(".wog") else name + ".wog")
    if g is None:
        raise KeyError("no world list or world group named %s" % name)
    out = {"entry": g, "lists": [], "worlds": [], "direct": [], "containers": []}
    seen = set()

    def expand(entry):
        if entry.key in seen:
            return
        seen.add(entry.key)
        out["containers"].append(entry)
        for r in big.refs(entry):
            t = big.find(r.key)
            if t is not None and t.ext == "wog":
                expand(t)
            elif t is not None and t.ext == "wol":
                if t.key not in seen:
                    seen.add(t.key)
                    out["containers"].append(t)
                    out["lists"].append((t.name.rsplit(".", 1)[0], t.key))
                    out["worlds"] += [x.key for x in big.refs(t) if x.key not in out["worlds"]]
            elif big.find(0xFFF00000 | (r.key & 0xFFFFF)) is not None and r.key not in out["worlds"]:
                out["worlds"].append(r.key)
                out["direct"].append(r.key)

    expand(g)
    return out


class Walker:
    def __init__(self, arc: Archive, rec_index: dict[int, int]):
        self.arc = arc
        self.rec_index = rec_index
        self.kinds: dict[int, str] = {}
        self.done: set[int] = set()              # keys walked
        self.busy: set[int] = set()              # keys being walked further up the stack
        self.failed: set[int] = set()            # keys whose stream did not read (noted once)
        self.pkg = None                          # the package of the world being walked
        self._lang_index = None
        self.stats = collections.Counter()
        self.problems: list[str] = []

    # ------------------------------------------------------------ helpers
    def tag(self, key: int, kind: str):
        if not key or key == NO_KEY:
            return
        old = self.kinds.get(key)
        if old is None or (old.startswith("res:") and not kind.startswith("res:")):
            self.kinds[key] = kind

    def note(self, what: str):
        """A record that could not be walked: counted by message (keys replaced by '*'), the first 400 kept."""
        self.stats[re.sub(r"\b[0-9A-F]{8}\b", "*", what)] += 1
        if len(self.problems) < 400:
            self.problems.append(what)

    def fail(self, key: int, what: str) -> bool:
        self.failed.add(key)
        self.note(what)
        return False

    def parse(self, fn, data):
        return F.parse(fn, data)

    def lang_index(self) -> dict:
        """record key -> (language bin, language, body) over the archive's language bins."""
        if self._lang_index is None:
            self._lang_index = TX.lang_data(self.arc.big).index()
        return self._lang_index

    def body(self, key: int):
        b = self.pkg.body(key) if self.pkg is not None else None
        if b is None:
            # a shared resource may live in another world's package
            for p in self.arc.loaded():
                b = p.body(key)
                if b is not None:
                    return b
            pk = self.rec_index.get(key)
            if pk is not None:
                p = self.arc.package(pk)
                if p is not None:
                    b = p.body(key)
        if b is None:
            # Txl records and the localized sounds they name are in the language bins
            r = self.lang_index().get(key)
            if r is not None:
                b = r[2]
        return b

    def skl_skin(self, skl_key: int) -> int:
        """The skin model key of an SKL modifier (0 when unreadable)."""
        b = self.body(skl_key)
        if b is None:
            return 0
        try:
            return self.parse(A.skl, b).get("skn", 0) or 0
        except Exception:          # noqa: BLE001
            return 0

    # ------------------------------------------------------------ resources
    def add_texture(self, key: int):
        self.tag(key, "texture")
        if not key or key in self.done:
            return
        b = self.body(key)
        if b is None:
            self.note(f"texture {key:08X} missing")
            return
        try:
            t = self.parse(F.texture, b)
        except Exception as ex:          # noqa: BLE001
            self.note(f"texture {key:08X}: {ex}")
            return
        fk = t.get("font", 0)
        if fk:
            # a font texture's glyph table (K3D_texture::LoadDesc -> p_Callback_LoadFont)
            self.add_spec(fk, registry.RESOURCES["fod"])
        self.done.add(key)

    def add_atomic(self, key: int):
        self.tag(key, "atomic")
        if not key or key in self.done:
            return
        b = self.body(key)
        if b is None:
            self.note(f"atomic {key:08X} missing")
            return
        try:
            a = self.parse(F.material_atomic, b)
        except Exception as ex:          # noqa: BLE001
            self.note(f"atomic {key:08X}: {ex}")
            return
        for t in a.get("templates", []):
            for tk in F.template_textures(t):
                self.add_texture(tk)
        self.done.add(key)

    def add_material(self, key: int):
        self.tag(key, "material")
        if not key or key in self.done:
            return
        b = self.body(key)
        if b is None:
            self.note(f"material {key:08X} missing")
            return
        try:
            m = self.parse(F.material, b)
        except Exception as ex:          # noqa: BLE001
            self.note(f"material {key:08X}: {ex}")
            return
        for ak in m.get("atomics", []):
            self.add_atomic(ak)
        self.done.add(key)

    def add_visual(self, key: int):
        self.tag(key, "visual")
        if not key or key in self.done:
            return
        b = self.body(key)
        if b is None:
            self.note(f"visual {key:08X} missing")
            return
        try:
            v = self.parse(G.visual, b)
        except Exception as ex:          # noqa: BLE001
            self.note(f"visual {key:08X}: {ex}")
            return
        if v["type"] == 2:
            for L in v["lods"]:
                if L["geometric"]:
                    self.add_visual(L["geometric"])
        else:
            for g in v.get("goms", []):
                if g["type"] == "sinus":
                    for c in g.get("curves", []):
                        for ck in c:
                            if ck:
                                self.add_resource(ck, "curve")
        self.done.add(key)

    def add_vis(self, key: int):
        self.tag(key, "vis")
        if not key or key in self.done:
            return
        b = self.body(key)
        if b is None:
            self.note(f"vis {key:08X} missing")
            return
        try:
            v = self.parse(F.vis, b)
        except Exception as ex:          # noqa: BLE001
            self.note(f"vis {key:08X}: {ex}")
            return
        self.add_visual(v.get("visual", 0))
        self.add_material(v.get("material", 0))
        if v.get("key2"):
            self.add_texture(v["key2"])
        self.done.add(key)

    def add_vif(self, key: int):
        self.tag(key, "vif")
        if not key or key in self.done:
            return
        b = self.body(key)
        if b is None:
            self.note(f"vif {key:08X} missing")
            return
        try:
            v = self.parse(V.vif, b)
        except Exception as ex:          # noqa: BLE001
            self.note(f"vif {key:08X}: {ex}")
            return
        for vi in v["visuals"]:
            self.add_visual(vi.get("visual", 0))
            self.add_material(vi.get("material", 0))
        self.done.add(key)

    def add_spec(self, key: int, spec, body: bytes | None = None, o: dict | None = None) -> bool:
        """A record through a registry Spec: read it (unless `o` is already parsed) and walk what it references.
        Returns True when the record was walked (or is being walked up the stack)."""
        self.tag(key, "spec:" + spec.name)
        if key in self.done or key in self.busy:
            return True
        if key in self.failed:
            return False
        if o is None:
            b = body if body is not None else self.body(key)
            if b is None:
                return self.fail(key, f"{spec.name} {key:08X} missing")
            try:
                o = self.parse(spec.fn, b)
            except Exception as ex:          # noqa: BLE001
                return self.fail(key, f"{spec.name} {key:08X}: {ex}")
        self.busy.add(key)
        try:
            for rkey, kind in spec.refs(o):
                if rkey and rkey != NO_KEY:
                    self.add_resource(rkey, kind)
        finally:
            self.busy.discard(key)
        self.done.add(key)
        return True

    def add_generic(self, r) -> bool:
        """A modifier through registry.MODIFIERS; False when no stream reads its type."""
        spec = registry.MODIFIERS.get(r.type)
        if spec is None:
            return False
        return self.add_spec(r.key, spec)

    def add_resource(self, key: int, kind: str) -> bool:
        """A non-modifier resource another stream points at, by kind."""
        self.tag(key, "res:" + kind)
        if key in self.done:
            return True
        if kind == "visual":
            self.add_visual(key)
        elif kind == "material":
            self.add_material(key)
        elif kind == "texture":
            self.add_texture(key)
        elif kind == "object":
            self.add_object(key)
        elif kind == "bone":
            self.add_object(key, in_world=False)
        elif kind == "actor":
            # an event / sound player actor: resolved by key once the world is loaded
            if key not in self.done:
                self.add_object(key)
            return False
        elif kind == "var_object":
            # an object a script instance's object variable names: loaded with the world like an actor
            if key not in self.done:
                self.add_object(key)
            return key in self.done
        elif kind in EXTERNAL_KINDS:
            return True
        elif kind == "soundfile" or kind.startswith("slib:"):
            return self.add_slib(key, int(kind[5:]) if kind.startswith("slib:") else None)
        elif kind == "txg":
            return self.add_txg(key)
        else:
            spec = registry.RESOURCES.get(kind)
            if spec is None:
                self.stats[f"resource kind {kind} not followed"] += 1
                return False
            return self.add_spec(key, spec)
        return key in self.done

    # ------------------------------------------------------------ sound
    def sniff_slib(self, b: bytes):
        """The SLib user word of a record whose referrer does not type it (track-list sound events, Txl entries, SNL extra
        sounds)."""
        if b[4:8] == b"RIFF":
            return 1
        for user in registry.SNIFF_ORDER:
            try:
                self.parse(MS.FILE_KINDS[user][1], b)
                return user
            except Exception:           # noqa: BLE001
                continue
        return None

    def add_slib(self, key: int, user) -> bool:
        """An SLib file named by a bank / set / smp entry, an SNK or SNL link (typed by the reference user word) or sniffed
        by content."""
        self.tag(key, "slib:%s" % user)
        if key in self.done or key in self.busy:
            return True
        if key == NO_KEY or key in self.failed:
            return False
        b = self.body(key)
        if b is None:
            return self.fail(key, f"slib file {key:08X} missing")
        if user is None:
            user = self.sniff_slib(b)
            if user is None:
                return self.fail(key, f"slib file {key:08X}: no SLib stream reads it")
        if user not in MS.FILE_KINDS:
            return self.fail(key, f"slib file {key:08X}: user word {user}")
        kind = MS.FILE_KINDS[user][0]
        if kind == "sample":
            return self.add_sample(key)
        return self.add_spec(key, registry.SLIB[kind], body=b)

    def add_sample(self, key: int) -> bool:
        """A sample record: tagged, not read."""
        self.tag(key, "sample")
        self.done.add(key)
        return True

    # ------------------------------------------------------------ text
    def add_txg(self, key: int) -> bool:
        """A text group and the language file of each of its languages (the Txl records come from the language bins)."""
        self.tag(key, "txg")
        if key in self.done or key in self.busy:
            return True
        b = self.body(key)
        if b is None:
            self.note(f"txg {key:08X} missing")
            return False
        try:
            g = self.parse(registry.TXG.fn, b)
        except Exception as ex:          # noqa: BLE001
            self.note(f"txg {key:08X}: {ex}")
            return False
        for lang, tk in sorted(TX.txg_languages(g).items()):
            tb = self.body(tk)
            if tb is None:
                self.note(f"txl {tk:08X} (language {lang}) missing")
                continue
            try:
                t = self.parse(registry.TXL.fn, tb)
            except Exception as ex:      # noqa: BLE001
                self.note(f"txl {tk:08X}: {ex}")
                continue
            self.add_spec(tk, registry.TXL, o=t)
        return self.add_spec(key, registry.TXG, o=g)

    # ------------------------------------------------------------ objects / worlds / lists
    def add_object(self, key: int, in_world: bool = True):
        """An object and its modifiers.  `in_world` False: an object the engine loads through another file (a skin
        model's bones), tagged "bone", whose hierarchy father is walked the same way."""
        self.tag(key, "object" if in_world else "bone")
        if key in self.done:
            return
        b = self.body(key)
        if b is None:
            self.note(f"object {key:08X} missing")
            return
        try:
            o = self.parse(F.obj, b)
        except Exception as ex:          # noqa: BLE001
            self.note(f"object {key:08X}: {ex}")
            return
        # the object's skin model first (its Edge animations drive that skin's bones)
        for r in o["refs"]:
            if r.is_modifier and r.type == A.SKL_TYPE:
                sk = self.skl_skin(r.key)
                if sk:
                    self.add_resource(sk, "skn")
                break
        for r in o["refs"]:
            if not r.is_modifier:
                continue                  # hierarchy link
            if r.type == VIS_TYPE:
                self.add_vis(r.key)
                continue
            if r.type == V.VIF_TYPE:
                self.add_vif(r.key)
                continue
            if not self.add_generic(r):
                self.stats[f"modifier type {r.type} not followed"] += 1
        self.done.add(key)
        # hierarchy fathers are loaded through their key
        father = o.get("father", 0)
        if father and father not in self.done:
            self.add_object(father, in_world)

    def add_world(self, wkey: int):
        self.tag(wkey, "world")
        self.pkg = self.arc.package(wkey)
        if self.pkg is None or self.pkg.body(wkey) is None:
            raise KeyError(f"world {wkey:08X} has no package")
        if wkey not in self.arc.worlds:
            raise KeyError(f"world {wkey:08X} is in no list or group")
        wo = self.parse(F.world, self.pkg.body(wkey))
        for r in wo["obj_refs"]:
            if r.is_modifier:
                self.add_object(r.key)
        for r in wo["mod_refs"]:
            if not self.add_generic(r):
                self.stats[f"world modifier type {r.type} not followed"] += 1
        # the world's modifier holder and the world itself count as walked
        self.done.add(wo.get("mdf_key") or (0xEE000000 | (wkey & 0xFFFFF)))
        self.done.add(wkey)

    def load_world_packages(self, refs):
        """Load the packages of the worlds among `refs`, in order (see the module docstring)."""
        for r in refs:
            if r.key in self.arc.worlds:
                self.arc.package(r.key)

    def add_list(self, name: str):
        """Every world a .wol names; the list and the groups that load it count as walked."""
        big = self.arc.big
        wols = [e for e in big.entries() if e.ext == "wol" and e.name.lower() == name.lower() + ".wol"]
        if not wols:
            raise KeyError(f"no world list {name}.wol")
        wol = wols[0]
        refs = big.refs(wol)
        self.load_world_packages(refs)
        for r in refs:
            if r.key in self.arc.worlds:
                try:
                    self.add_world(r.key)
                except Exception as ex:          # noqa: BLE001
                    self.note(f"world {r.key:08X}: {ex}")
            else:
                self.note(f"list {name}: world {r.key:08X} unknown")
        self.done.add(wol.key)
        for e in big.entries():
            if e.ext == "wog" and any(r.key == wol.key for r in big.refs(e)):
                self.done.add(e.key)

    def add_group(self, name: str, walked: set) -> list[int]:
        """A world group (NAME.wog): the lists it loads that were not walked yet, then the worlds it names without a list.
        Returns those direct world keys."""
        g = resolve(self.arc.big, name)
        for lname, _key in g["lists"]:
            if lname not in walked:
                self.add_list(lname)
                walked.add(lname)
        for w in g["direct"]:
            self.arc.worlds.setdefault(w, name)
            self.load_world_packages([F.Ref(w)])
            try:
                self.add_world(w)
            except Exception as ex:          # noqa: BLE001
                self.note(f"world {w:08X}: {ex}")
        return g["direct"]
