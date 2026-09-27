"""Which stream reads which modifier / resource type, and what each record points at.

A Spec names the stream function and a `refs(o) -> [(key, kind), ...]` function listing the keys the record passes on.
Kinds: visual, material, texture, object, bone (an object loaded through a skin model, not listed in the world), actor
(an object an event or a sound player resolves by key once the world is loaded), var_object (an object a script
instance's object variable names), soundbank, slib:<user word> (an SLib file typed by its reference user word, see
sound.FILE_KINDS), soundfile (an SLib file of unknown type: sniffed), txg (a text group and its language files),
script_model (the model of a script instance), or a key of RESOURCES.  The walker tags a record read through a Spec
"spec:<name>", so the Spec names are the record kinds the converters dispatch on.
"""
from __future__ import annotations

import struct
from dataclasses import dataclass
from typing import Callable

from ..formats import anim as A
from ..formats import curve as CV
from ..formats import effects as X
from ..formats import modifiers as M
from ..formats import script as SC
from ..formats import sound as MS
from ..formats import text as TX
from ..formats import world as WD
from ..formats import zones as Z
from ..formats.stream import FormatError  # noqa: F401 - re-exported for the walker's error handling


@dataclass
class Spec:
    name: str
    fn: Callable
    refs: Callable = lambda o: []


MODIFIERS: dict[int, Spec] = {}      # engine type id -> Spec
RESOURCES: dict[str, Spec] = {}      # kind -> Spec


def _tag(pairs):
    return [(k, kind) for k, kind in pairs if k]


# --- curves ------------------------------------------------------------------
RESOURCES["curve"] = Spec("curve", CV.curve, CV.refs_curve)

# --- zones / groups / collision ------------------------------------------------
COB_KINDS = {"geo": "visual", "gml": "gmatlib_gml", "gms": "gmatlib_gms"}

MODIFIERS[4] = Spec("grp", Z.grp,
                    lambda o: _tag([(o.get("grprefs", 0), "grprefs")] + [(e.get("key", 0), "object") for e in o.get("objects", [])]))
MODIFIERS[19] = Spec("zde", Z.zde)
MODIFIERS[20] = Spec("cob", Z.cob, lambda o: _tag([(k, COB_KINDS[kind]) for kind, k in Z.cob_ref_kinds(o)]))
MODIFIERS[35] = Spec("occ", Z.occ)
MODIFIERS[39] = Spec("trg", Z.trg,
                     lambda o: _tag([(o.get("material", 0), "material"), (o.get("skidmarks", {}).get("curve0", 0), "curve"),
                                     (o.get("skidmarks", {}).get("curve1", 0), "curve")]))
MODIFIERS[24] = Spec("vp", Z.vp, lambda o: _tag([(o.get("viewpoint", {}).get("texture", 0), "texture")]))
RESOURCES["grprefs"] = Spec("grprefs", Z.grprefs, lambda o: _tag([(r.key, "object") for r in o.get("refs", [])]))
# TBO's group key is loaded through GRP::p_Callback_LoadGrpRefs too: the same resource
RESOURCES["grp_struct"] = RESOURCES["grprefs"]
RESOURCES["gmatlib_gml"] = Spec("gmatlib", Z.gmatlib)
RESOURCES["gmatlib_gms"] = Spec("gmatlib", Z.gmatlib)

# --- lights / dynamics / effects -------------------------------------------------
MODIFIERS[9] = Spec("lig", X.lig, lambda o: _tag([(o.get("light", {}).get("att_curve_key", 0), "curve")]))
MODIFIERS[21] = Spec("dyn", X.dyn)
MODIFIERS[33] = Spec("veg", X.veg, lambda o: _tag([(k, "visual") for k in X.refs_veg(o)]))
MODIFIERS[65] = Spec("lrp", X.lrp)
MODIFIERS[27] = Spec("lip", X.lip)
MODIFIERS[61] = Spec("cam", X.cam)
MODIFIERS[31] = Spec("lg", X.lg)                       # RGH keeps its gizmo models inline
MODIFIERS[18] = Spec("eff", X.eff,
                     lambda o: _tag([(o.get("material_key", 0), "material"), (o.get("gfx", {}).get("curve1_key", 0), "curve"),
                                     (o.get("gfx", {}).get("curve2_key", 0), "curve")]))
MODIFIERS[10] = Spec("par", X.par, lambda o: _tag([(s.get("model_key", 0), "pam") for s in o.get("subsystems", [])]))


def _pam_refs(o):
    out = [(o.get("gen_geom_key", 0), "visual"), (o.get("material_key", 0), "material"),
           (o.get("visual_key", 0), "visual"), (o.get("pam_texture_key", 0), "texture")]
    keys = set(X.refs_particle_model(o))
    for k, kind in out:
        keys.discard(k)
    out += [(k, "curve") for k in keys]
    return _tag(out)


RESOURCES["pam"] = Spec("particle_model", X.particle_model, _pam_refs)


# --- sound modifiers --------------------------------------------------------------
def _slib_kind(user):
    """The kind of an SLib file named with that reference user word."""
    return f"slib:{user}" if user in MS.FILE_KINDS else "soundfile"


def _snk_refs(o):
    typ = o.get("sound_type", 0)
    # the player's actors are resolved by key at run time: world objects, like EVE actors
    p = o.get("player") or {}
    actors = [(p.get(nm, 0), "actor") for nm in ("play_actor", "stop_actor")]
    actors += [((p.get(nm) or {}).get("obj_key", 0), "actor")
               for nm in ("play_trigger", "play_callback", "stop_trigger", "stop_callback")]
    if typ in M.SNK_FILE_TYPES:
        return _tag([(o.get("file_key", 0), _slib_kind(typ))] + actors)   # the sound type is the file's user word
    return _tag([(o.get("bank_key", 0), "soundbank")] + actors)


def _snf_refs(o):
    z = o.get("zone", {})
    banks = [(k, "soundbank") for k in o.get("bank_keys", [])]
    geo_ = [(z.get("geom_key", 0), "visual")]
    known = {k for k, _ in banks} | {z.get("geom_key", 0)}
    curves = [(k, "curve") for k in M.refs_snf(o) if k not in known]
    return _tag(banks + geo_ + curves)


def _snl_link_refs(o):
    out = []
    for e in o.get("entries", []):
        out += [(e.get("sound_key", 0), _slib_kind(e.get("sound_type", 0))), (e.get("smx_key", 0), "slib:5")]
    for t in o.get("transitions", []):
        out += [(t.get("fade_out_curve", 0), "curve"), (t.get("fade_in_curve", 0), "curve"),
                (t.get("aux_punch_curve", 0), "curve"), (t.get("xtra_sound_key", 0), "soundfile")]
    out.append((o.get("father_key", 0), "snl_link"))
    return _tag(out)


MODIFIERS[22] = Spec("snk", M.snk, _snk_refs)
MODIFIERS[23] = Spec("snf", M.snf, _snf_refs)
MODIFIERS[41] = Spec("snl", M.snl, lambda o: _tag([(o.get("base_link_key", 0), "snl_link")]))
MODIFIERS[2] = Spec("mssg", M.mssg)
MODIFIERS[7] = Spec("tbo", M.tbo, lambda o: _tag([(o.get("grp_key", 0), "grp_struct"), (o.get("shape_key", 0), "tbo_shape")]))
MODIFIERS[1] = Spec("dst", M.dst)
RESOURCES["snl_link"] = Spec("snl_link", M.snl_link, _snl_link_refs)
RESOURCES["tbo_shape"] = Spec("tbo_shape", M.tbo_shape)


# --- SLib files --------------------------------------------------------------------
def _slib_refs(kind):
    def refs(o):
        out = [(k, _slib_kind(u)) for k, u in MS.child_refs(kind, o)]
        if kind != "bank":
            out += [(k, "curve") for k in MS.curve_refs(kind, o)]
        return _tag(out)
    return refs


RESOURCES["soundbank"] = Spec("bank", MS.bank, _slib_refs("bank"))
# SLib files by sound kind (samples are tagged without reading: Walker.add_sample)
SLIB = {
    "set": Spec("set", MS.sound_set, _slib_refs("set")),
    "smp": Spec("smp", MS.smp, _slib_refs("smp")),
    "smx": Spec("smx", MS.smx, _slib_refs("smx")),
    "mic": Spec("mic", MS.mic, _slib_refs("mic")),
    "sva": Spec("sva", MS.sound_variables, _slib_refs("sva")),
}
SNIFF_ORDER = (3, 7, 5, 6, 8)        # user words tried on an SLib record that is not a RIFF sample

# --- world modifiers -------------------------------------------------------------------
for _t in (WD.NET_TYPE, WD.RDP_TYPE, WD.BVO_TYPE):
    MODIFIERS[_t] = Spec(WD.NAMES[_t], WD.STREAMS[_t], WD.REFS[_t])
# AFX: the HLSL model key of chunk 7 is not followed (no RGH loader reads that file)
MODIFIERS[WD.AFX_TYPE] = Spec("afx", WD.afx)


# --- animation -----------------------------------------------------------------------
def _files_only(refs_fn):
    """The actors of eve / trl ("object") are resolved by the engine after loading, not through LOA_MakeFileRef: kind
    "actor"."""
    return lambda o: [(k, "actor" if kind == "object" else kind) for k, kind in refs_fn(o)]


def _trl_refs(o):
    # AFX events' HLSL models are not followed
    return [(k, "actor" if kind == "object" else kind) for k, kind in A.refs_trl(o) if kind != "hlsl"]


MODIFIERS[A.SKL_TYPE] = Spec("skl", A.skl, A.refs_skl)
MODIFIERS[A.ACT_TYPE] = Spec("aci", A.aci, A.refs_aci)
MODIFIERS[A.ANI_TYPE] = Spec("ani", A.ani, A.refs_ani)
MODIFIERS[A.EVE_TYPE] = Spec("eve", A.eve, _files_only(A.refs_eve))
MODIFIERS[A.SP_TYPE] = Spec("sp", A.sp)                  # SP::mh_SKL is not a file reference
RESOURCES["skn"] = Spec("skn", A.skn, lambda o: [(k, "bone") for k, _ in A.refs_skn(o)])
RESOURCES["ack"] = Spec("ack", A.ack, A.refs_ack)
RESOURCES["act"] = Spec("act", A.act, A.refs_act)
RESOURCES["mpk"] = Spec("mpk", A.mpk, A.refs_mpk)
RESOURCES["trl"] = Spec("trl", A.trl, _trl_refs)

# --- text (the TXT modifier stream is modifiers.txt) --------------------------------------
MODIFIERS[M.TXT_TYPE] = Spec("txt", M.txt, TX.ref_kinds_txt)
RESOURCES["txd"] = Spec("txd", TX.txd, lambda o: _tag([(k, "txg") for k in TX.refs_txd(o)]))
TXG = Spec("txg", TX.txg)                                  # + one Txl per language: Walker.add_txg
TXL = Spec("txl", TX.txl, lambda o: _tag(TX.ref_kinds_txl(o)))
RESOURCES["fod"] = Spec("fod", TX.fod)

# --- script instances --------------------------------------------------------------------
SCR_OBJECT_TYPE = 0x19            # an object variable holds the object's key
SCR_CURVE_TYPE = 0x21             # a curve variable: the key of an MTH curve file
SCR_STRUCT_TYPE = 0x2F            # a struct: nested descriptor block, members in the same buffer


def _ins_var_keys(vb, init, base=0):
    """(type, key) of the object and curve variables of an instance's variable block, struct members included (their
    offsets count from the struct's own place in the instance buffer)."""
    out = []
    for v in vb.get("vars", []):
        t, low = v.get("type", 0) >> 16, v.get("type", 0) & 0xFFFF
        off = base + v.get("offset", 0)
        if low == 0 and t in (SCR_OBJECT_TYPE, SCR_CURVE_TYPE) and off + 4 <= len(init):
            out.append((t, struct.unpack_from("<I", init, off)[0]))
        elif t == SCR_STRUCT_TYPE and v.get("sub"):
            out += _ins_var_keys(v["sub"], init, off)
    return out


def _ins_refs(o):
    """The script model (kind "script_model", not followed: models are tagged from the model records), the objects the
    instance's object variables name (resolved by key at run time, so loaded with the world like track-list actors,
    kind "var_object"), and the curves its curve variables name (loaded with the instance: SCR::pt_LoadVars ->
    MTH_p_Callback_CurveLoad).  Trigger (0x31) and dataset variables are not followed."""
    out = [(o.get("model", 0), "script_model")]
    vb = o.get("vars") or {}
    for t, k in _ins_var_keys(vb, vb.get("init") or b""):
        out.append((k, "var_object" if t == SCR_OBJECT_TYPE else "curve"))
    return _tag(out)


# SCR (type 0): object instances referenced from their object, world instances from the world's modifier list
MODIFIERS[SC.SCR_TYPE] = Spec("scr", SC.instance, _ins_refs)
