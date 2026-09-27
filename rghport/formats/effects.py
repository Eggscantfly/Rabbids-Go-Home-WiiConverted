"""Modifier streams for the gfx / physics / camera modifier types.

One stream function per type (plus the sub-streams they call) and a `refs_<name>(o)` helper listing the keys that go
through LOA_MakeFileRef.  Per type (loaders of the RGH Wii executable):

LIG (9) - LIG::p_Callback_LoadLIG -> K3D_light::pu32_LoadFromBuffer.  The MDF header (RGH ships version 0) is followed
    by the light's own stream: a word 'V'<<24|version then a u8 type (or a bare type word = version 0), the common
    params, then a jump table on the type (types 0..13; types 1 and 8 are remapped to 0 before the switch).  RGH
    accepts versions <= 0x11.
DYN (21) - DYN::p_Callback_LoadDYN.  RGH ships version 3: a shape list of {u8 zdm type (0 sphere, 1 box, 2 ellipse),
    u8 flags, shape}.
VEG (33) - VEG::p_Callback_LoadVEG -> K3D_veg::LoadVegFromBuffer (+ vgReadGenerator).  RGH ships MDF version 0x10.
LRP (65) - LRP::p_Callback_LoadLRP.  Own version word (RGH ships 0), blend distance, 4 words, a byte, [version < 1:
    two bytes], then ViD::pu32_LoadRenderParam.
LIP (27) - LIP::p_Callback_LoadLIP: nothing after the MDF header.
CAM (61) - CAM::p_Callback_LoadCAM: flags, fov, iso zoom, flags.
LG (31) - LG::p_Callback_LoadLG (+ LG_model::pc_LoadDataFromBuffer, LG_model::p_Callback_LoadLGModel).  RGH ships MDF
    version 4: a reference-object key, then a model key (an external LG_model file) or, when it is 0, the model data
    inline.
EFF (18) - EFF::p_Callback_LoadEFF + Line/Lightning/Lasso/Flare_Load.  RGH ships version 2.
PAR (10) - PAR::p_Callback_LoadPAR + PAR_subsystem::pu32_LoadFromBuffer.  RGH ships MDF version 0x38, the "new" format
    (>= 0x33): a subsystem list, each carrying its particle model as a key
    (K3D_PArticleModel::p_Callback_LoadParticleModel, a separate file), then a short tail.  The old inline format
    (< 0x33) is not handled.
particle model (the file a PAR subsystem's model key points at) - K3D_PArticleModel::p_Callback_LoadParticleModel ->
    pc_LoadFromBuffer.  Every RGH model is version 0x10 (the RGH maximum).  The spawn shape block depends on
    patch_ConvertOldSpawnShapeType (old u8 -> point / box / sphere / cylinder / none), reproduced in
    SPAWN_SHAPE_CONVERT.

Branches the RGH data exercises (every record parses): LIG 1691 (light stream 0x11; types omni, 2, direct_att, spot,
depth_dark, modifier; no curve keys), DYN 884 (sphere shapes only), VEG 896 (units v9, generators v2), LRP 131
(stream version 1, render params 9), LIP 102, CAM 18, LG 18 (models all inline, no LG_model files), EFF 29 (lightning,
flare, lasso), PAR 421 (one v3 subsystem each), particle models 251 (v0x10, point/box/sphere/cylinder/none).
Not in the data (written from the loaders only): DYN box/ellipse shapes and versions < 3, light types
box_dir/box_env/sphere_env/sun/multi_*, light versions < 0x11, EFF line, LG versions 1..2 and the standalone LG_model
file, LRP stream version 0, particle models < 0x10.
"""
from __future__ import annotations

from . import stream as F
from .stream import FormatError

LIG_TYPE, DYN_TYPE, VEG_TYPE, LRP_TYPE, LIP_TYPE = 9, 21, 33, 65, 27
CAM_TYPE, LG_TYPE, EFF_TYPE, PAR_TYPE = 61, 31, 18, 10


# ----------------------------------------------------------------------------
# helpers
def _count(st, o, name, value, fn="u32"):
    """A count: derived from the content when writing."""
    if st.writing:
        o[name] = value
    return getattr(st, fn)(o, name)


def _items(st, o, name, count_name, fn="u32"):
    """The list `o[name]` sized by the count field `count_name`."""
    lst = o.setdefault(name, [])
    n = _count(st, o, count_name, len(lst), fn)
    if not st.writing:
        lst[:] = [{} for _ in range(n)]
    return lst


def ellipse(st, o, name):
    """MTH_pu8_LoadEllipseFromBuffer: centre, radii, then a word only loose files keep."""
    v = st.raw(o, name, 24)
    st.u32(o, name + "_m1", 1)
    return v


def _keys(*ks):
    return [k for k in ks if k]


# ============================================================================
# LIG: K3D_light::pu32_LoadFromBuffer (RGH 8032AE88)
# ============================================================================
LIGHT_TYPES = {0: "omni", 3: "direct_att", 4: "spot", 5: "box_dir", 6: "box_env",
               7: "sphere_env", 9: "sun", 10: "depth_dark", 11: "modifier",
               12: "multi_direct_att", 13: "multi_direct"}


def _light_spec(st, p, eff, ver):
    """The type-specific block (the jump table after the common params)."""
    if eff in (0, 3):                                   # pt_GetOmni / pt_GetDirectAttenuate
        st.f32(p, "near_radius"); st.f32(p, "far_radius")
    elif eff == 12:                                     # pt_GetMultiDirectAttenuate
        st.f32(p, "near_radius"); st.f32(p, "far_radius")
        st.array(p, "u32x6", "<I", 6)
    elif eff == 13:                                     # pt_GetMultiDirect
        st.array(p, "u32x6", "<I", 6)
    elif eff == 4:                                      # pt_GetSpot
        st.f32(p, "near_radius"); st.f32(p, "far_radius")
        st.f32(p, "little_alpha"); st.f32(p, "big_alpha")
        if ver >= 9:
            st.f32(p, "v9_f0"); st.f32(p, "v9_f1"); st.f32(p, "v9_f2")
    elif eff == 5:                                      # pt_GetBoxDir
        st.v3(p, "v0"); st.v3(p, "v1")
        if ver >= 0xA:
            st.f32(p, "va_f0"); st.f32(p, "va_f1"); st.f32(p, "va_f2")
    elif eff == 6:                                      # pt_GetBoxEnv
        st.v3(p, "near"); st.v3(p, "far"); st.f32(p, "twist")
        st.v3(p, "offset_rendering"); st.f32(p, "dist_rendering")
        st.u32(p, "background_color")
        if ver >= 6:
            st.f32(p, "diffuse_blur"); st.f32(p, "contrast")
            st.f32(p, "contrast_pivot"); st.f32(p, "saturation")
    elif eff == 7:                                      # pt_GetSphereEnv
        st.f32(p, "near_radius"); st.f32(p, "far_radius"); st.f32(p, "twist")
        st.v3(p, "offset_rendering"); st.f32(p, "dist_rendering")
        st.u32(p, "background_color")
        if ver >= 6:
            st.f32(p, "diffuse_blur"); st.f32(p, "contrast")
            st.f32(p, "contrast_pivot"); st.f32(p, "saturation")
    elif eff == 9:                                      # pt_GetSun
        st.f32(p, "max_shadow_dist"); st.f32(p, "cascade_factor")
        if ver >= 4:
            st.u8(p, "far_shadow_occlusion"); st.u8(p, "nbr_shadow_viewport")
        if ver >= 5:
            st.v3(p, "volum_min"); st.v3(p, "volum_max")
        if ver >= 0xC:
            st.f32(p, "threshold_far_culling")
        if ver >= 0xF:
            for nm in ("shw_bias_base", "shw_bias_slope", "shw_bias_dist", "cascade", "size"):
                st.array(p, nm + "_vp", "<f", 6)
    elif eff == 10:                                     # pt_GetDepthDark
        if ver >= 0xE:
            bt = st.u32(p, "bounding_type")
            if bt:
                st.v3(p, "box_min"); st.v3(p, "box_max")
            else:
                st.f32(p, "sphere_radius")
        else:
            st.f32(p, "sphere_radius")
        st.f32(p, "ground_lev"); st.f32(p, "depth_lev")
    elif eff == 11:                                     # pt_GetModifier
        mv = st.u32(p, "mod_version", default=1)
        bt = st.u32(p, "bounding_type")
        if mv < 1:
            if bt:
                st.v3(p, "box_far_min"); st.v3(p, "box_far_max")
            else:
                st.f32(p, "sphere_far_radius")
            st.u8(p, "modifier_light_mask")
        else:
            if bt:
                st.v3(p, "box_far_min"); st.v3(p, "box_far_max")
                st.v3(p, "box_near_min"); st.v3(p, "box_near_max")
            else:
                st.f32(p, "sphere_far_radius"); st.f32(p, "sphere_near_radius")
            st.u8(p, "modifier_light_mask")
            st.u8(p, "mod_flags")
    # any other type (1, 2, 8 after the remap cannot happen): nothing


def light(st, o):
    """K3D_light::pu32_LoadFromBuffer."""
    if st.writing:
        ver = o.get("version", 0)
        if o.get("versioned", ver != 0):
            o["_hdr"] = 0x56000000 | (ver & 0xFFFFFF)
            st.u32(o, "_hdr")
            st.u8(o, "type")
        else:
            o.setdefault("type_word", o.get("type", 0))
            st.u32(o, "type_word")
    else:
        hdr = st.u32(o, "_hdr")
        if hdr >> 24 == 0x56:
            ver = o["version"] = hdr & 0xFFFFFF
            o["versioned"] = True
            st.u8(o, "type")
        else:
            ver = o["version"] = 0
            o["versioned"] = False
            o["type_word"] = hdr
            o["type"] = hdr & 0xFF
    if ver > 0x11:
        raise FormatError(f"light version {ver:#x}")
    typ = o["type"]
    eff = 0 if typ in (1, 8) else typ         # the loader rewrites 1 and 8 to 0
    o["type_name"] = LIGHT_TYPES.get(eff, f"type{eff}")
    st.u8(o, "categ")
    st.u8(o, "light_mask")
    st.u8(o, "list_mask")
    st.i8(o, "global_index")
    st.u32(o, "flags")
    st.u32(o, "color")
    st.f32(o, "mul_factor")
    if ver >= 0x11:
        st.u32(o, "att_curve_key")        # -> LOA_MakeFileRef when set
    p = o.setdefault("spec", {})
    if ver == 0:
        if eff in (0, 3):
            st.f32(p, "near_radius"); st.f32(p, "far_radius")
    elif ver in (1, 2):
        if ver == 2:
            st.u16(o, "v2_u16a"); st.u16(o, "v2_u16b")
        if eff in (0, 3):
            st.f32(p, "near_radius"); st.f32(p, "far_radius")
        elif eff == 4:
            st.f32(p, "near_radius"); st.f32(p, "far_radius")
            st.f32(p, "little_alpha"); st.f32(p, "big_alpha")
    else:
        if ver <= 7:
            st.u16(o, "old_flags16")      # bits 1/2 are or'ed into flags (0x40/0x100)
        for i in range(4 if ver < 0x10 else 3):
            st.u8(o, f"pad_b{i}")         # read and dropped
        if ver >= 0xB:
            st.u32(o, "editable_render_mask", default=0x8000)
        if ver >= 9:
            st.u32(o, "v9_u0"); st.u32(o, "v9_u1")
        if eff <= 0xD:
            _light_spec(st, p, eff, ver)
    return o


def lig(st, o):
    """LIG::p_Callback_LoadLIG (RGH 80109EC4)."""
    st.memsize(o)
    F.mdf_header(st, o)
    light(st, o.setdefault("light", {}))
    return o


def refs_lig(o) -> list[int]:
    return _keys(o.get("light", {}).get("att_curve_key", 0))


# ============================================================================
# DYN: DYN::p_Callback_LoadDYN (RGH 800F5960)
# ============================================================================
def dyn_shape(st, s):
    """One DYN_tt_Shape_: u8 ZDM type -> DYN::ShapeAdd, u8 flags, then the shape by type (0 sphere, 1 box, 2 ellipse)."""
    kind = st.u8(s, "zdm_type")
    st.u8(s, "zdm_flags")
    if kind == 0:
        st.sphere(s, "sphere")
    elif kind == 1:
        st.box(s, "box")
    elif kind == 2:
        ellipse(st, s, "ellipse")
    else:
        raise FormatError(f"DYN shape type {kind}")


def dyn(st, o):
    st.memsize(o)
    F.mdf_header(st, o)
    ver = o["version"]
    if ver > 3:
        raise FormatError(f"DYN version {ver}")
    if ver >= 1:
        st.u32(o, "u0")
    st.u8(o, "rec_count")
    st.f32(o, "f0"); st.f32(o, "f1"); st.f32(o, "f2"); st.f32(o, "f3")
    st.f32(o, "wall_cos_angle")
    if ver >= 2:
        st.f32(o, "max_step")
    st.u32(o, "u1"); st.u32(o, "u2")
    if ver < 3:
        dyn_shape(st, o.setdefault("shape", {}))
    else:
        for s in _items(st, o, "shapes", "n_shapes"):
            dyn_shape(st, s)
    return o


def refs_dyn(o) -> list[int]:
    return []


# ============================================================================
# VEG: K3D_veg::LoadVegFromBuffer (RGH 802FB914) + vgReadGenerator (RGH 802FE030)
# ============================================================================
def vg_generator(st, g):
    """vgReadGenerator: the K3D_vg_generator of a unit."""
    ver = st.u32(g, "version", default=2)
    st.u32(g, "mode"); st.u32(g, "flags")
    st.u8(g, "rlia"); st.u8(g, "rlir"); st.u8(g, "rlig"); st.u8(g, "rlib")
    st.u32(g, "use_rli"); st.f32(g, "density")
    if ver != 0:
        st.u32(g, "paint_rli0")
    if ver > 1:
        st.u32(g, "paint_rli1")


def veg_unit(st, u):
    """One K3D_VegUnit (the loop body of LoadVegFromBuffer)."""
    ver = st.u32(u, "version", default=9)
    if ver > 9:
        raise FormatError(f"veg unit version {ver}")
    st.u32(u, "visual_key")               # -> LOA_MakeFileRef (the unit's clone visual)
    st.u32(u, "flags")
    st.f32(u, "rand_size_factor")
    if ver > 4:
        st.f32(u, "rand_noise_r_factor"); st.f32(u, "rand_noise_z_factor")
    st.f32(u, "spg_gsize"); st.f32(u, "spg_extraction")
    if ver < 8:
        st.f32(u, "old_f8")
    if ver < 2:
        st.f32(u, "old_f0"); st.f32(u, "old_f1"); st.v3(u, "old_v")
    st.u16(u, "elem0"); st.u16(u, "elem1"); st.u16(u, "elem2"); st.u16(u, "elem3")
    st.u32(u, "spg_color")                # ABGR in the file (ABGR_ToRGBA after)
    if ver != 0:
        st.f32(u, "spg_xovery_x"); st.f32(u, "spg_xovery_y")
        st.f32(u, "spg_xovery_z"); st.f32(u, "spg_xovery_s")
    if ver > 2:
        st.u32(u, "paint_rli_id")
    if ver > 3:
        st.f32(u, "extract_bottom_cross_x"); st.f32(u, "extract_bottom_cross_y")
        st.f32(u, "extract_bottom_z"); st.f32(u, "extract_bottom_s")
        st.f32(u, "extract_top_cross_x"); st.f32(u, "extract_top_cross_y")
        st.f32(u, "extract_left_cross_x"); st.f32(u, "extract_left_cross_y")
        st.f32(u, "extract_right_cross_x"); st.f32(u, "extract_right_cross_y")
    if ver > 5:
        st.f32(u, "extract_bottom_noise_z")
    if ver > 6:
        st.f32(u, "rand_noise_angle_factor_s"); st.f32(u, "rand_noise_extract_factor_s")


def k3d_veg(st, o, ver):
    """K3D_veg::LoadVegFromBuffer(cursor, version): `ver` is the MDF version."""
    units = _items(st, o, "units", "n_units")
    st.u32(o, "root_visual_key")          # -> LOA_MakeFileRef
    for u in units:
        veg_unit(st, u)
    st.u8(o, "b_f9")
    if ver > 0xF:
        st.u8(o, "b_fe")
    st.u32(o, "u40")
    st.u8(o, "light_mask")
    st.f32(o, "spg_wind_freq"); st.f32(o, "spg_wind_force")
    st.v3(o, "spg_wind_dir_init")
    st.f32(o, "spgg_wind_freq"); st.f32(o, "spgg_wind_force")
    st.u32(o, "gzone_channel"); st.u32(o, "gzone_mask_on"); st.u32(o, "gzone_mask_off")
    st.u32(o, "mdf_vis_key")              # -> LOA_MakeFileRef
    for u in units:                       # vgReadGenerator per unit
        vg_generator(st, u.setdefault("generator", {}))
    st.f32(o, "freeze_distance")
    st.f32(o, "far_culling_dist")
    st.f32(o, "disappear_length"); st.f32(o, "disappear_height")
    st.f32(o, "lod0to1_distance"); st.f32(o, "lod1to2_distance")
    st.u32(o, "mdf_vis_rank")


def veg(st, o):
    """VEG::p_Callback_LoadVEG (RGH 80160568)."""
    st.memsize(o)
    F.mdf_header(st, o)
    ver = o["version"]
    if ver < 0xF:
        raise FormatError(f"VEG version {ver} (the loader skips it)")
    k3d_veg(st, o.setdefault("veg", {}), ver)
    return o


def refs_veg(o) -> list[int]:
    v = o.get("veg", {})
    out = [v.get("root_visual_key", 0), v.get("mdf_vis_key", 0)]
    out += [u.get("visual_key", 0) for u in v.get("units", [])]
    return _keys(*out)


# ============================================================================
# LRP: LRP::p_Callback_LoadLRP (RGH 802412F4)
# ============================================================================
def lrp(st, o):
    st.memsize(o)
    F.mdf_header(st, o)
    ver = st.u32(o, "lrp_version", default=0)
    st.f32(o, "blend_dist")
    st.array(o, "dummy", "<I", 4)
    st.u8(o, "activation_zde")
    if ver < 1:
        st.u8(o, "rp_b0"); st.u8(o, "rp_b1")      # stored into the render params
    F.render_params(st, o.setdefault("render_params", {}))
    return o


def refs_lrp(o) -> list[int]:
    return []


# ============================================================================
# LIP: LIP::p_Callback_LoadLIP (RGH 8010A850)
# ============================================================================
def lip(st, o):
    st.memsize(o)
    F.mdf_header(st, o)
    return o


def refs_lip(o) -> list[int]:
    return []


# ============================================================================
# CAM: CAM::p_Callback_LoadCAM (RGH 8023F164)
# ============================================================================
def cam(st, o):
    st.memsize(o)
    F.mdf_header(st, o)
    st.u32(o, "camera_flags")             # K3D_BasicCamera flags
    st.f32(o, "fov")                      # SetFieldOfVision
    st.f32(o, "iso_zoom")                 # SetIsoZoom
    st.u32(o, "flags")
    return o


def refs_cam(o) -> list[int]:
    return []


# ============================================================================
# LG: LG::p_Callback_LoadLG (RGH 8010B89C)
#     LG_model::pc_LoadDataFromBuffer (RGH 8010B68C)
#     LG_model::p_Callback_LoadLGModel (RGH 8010B7F0)
# ============================================================================
def _lg_gizmo(st, g, fmt):
    """One LG_tt_Gizmo_ {u32 obj key, u32 num}; `fmt` is the LG version (1, 2) or 3 for the LG_model data layout
    (= version 2's)."""
    st.u32(g, "num")
    st.u32(g, "obj_key")                  # an OBJ key (resolved, not a file ref)
    if fmt == 1:
        st.u32(g, "m1_u0", 1)
    elif fmt >= 2:
        st.u32(g, "m1_u0", 1); st.f32(g, "m1_f1", 1)
        st.u32(g, "m1_u2", 1); st.f32(g, "m1_f3", 1); st.f32(g, "m1_f4", 1)


def lg_model_data(st, m):
    """LG_model::pc_LoadDataFromBuffer."""
    st.u32(m, "u0")                       # read and dropped
    st.u32(m, "m1_u1", 1)
    for g in _items(st, m, "gizmos", "n_gizmos"):
        _lg_gizmo(st, g, 3)


def lg_model(st, o):
    """LG_model::p_Callback_LoadLGModel: the standalone model resource."""
    st.memsize(o)
    lg_model_data(st, o)
    return o


def lg(st, o):
    st.memsize(o)
    F.mdf_header(st, o)
    ver = o["version"]
    if ver < 1 or ver > 4:
        raise FormatError(f"LG version {ver}")
    st.u32(o, "ref_obj_key")              # LG::mpo_RefObj (an OBJ key)
    if ver in (1, 2):
        m = o.setdefault("model", {})
        for g in _items(st, m, "gizmos", "n_gizmos"):
            _lg_gizmo(st, g, ver)
    else:
        if st.writing and o.get("model") is not None:
            o["model_key"] = 0
        key = st.u32(o, "model_key")      # -> LOA_MakeFileRef (an LG_model file)
        if not key:
            lg_model_data(st, o.setdefault("model", {}))
        elif not st.writing:
            o["model"] = None
    return o


def refs_lg(o) -> list[int]:
    return _keys(o.get("model_key", 0))


# ============================================================================
# EFF: EFF::p_Callback_LoadEFF (RGH 800FCB94)
# ============================================================================
def eff_line(st, e):
    st.v3(e, "a"); st.v3(e, "b"); st.v3(e, "z")
    st.u32(e, "color"); st.f32(e, "size")


def eff_lightning(st, e):
    st.u32(e, "num_patterns"); st.u32(e, "color"); st.u32(e, "flags")
    st.f32(e, "radius"); st.f32(e, "size")
    st.v3(e, "a"); st.v3(e, "b"); st.v3(e, "z")


def eff_lasso(st, e):
    st.u32(e, "num_patterns"); st.u32(e, "color"); st.u32(e, "flags")
    st.f32(e, "radius_c1"); st.f32(e, "radius_c2"); st.f32(e, "size"); st.f32(e, "tiler")
    st.u16(e, "build_mode")
    st.v3(e, "a"); st.v3(e, "b"); st.v3(e, "curve_normal1"); st.v3(e, "curve_normal2")
    st.u32(e, "curve1_key"); st.u32(e, "curve2_key")      # -> LOA_MakeFileRef


def eff_flare(st, e):
    st.v3(e, "pos"); st.v3(e, "sight")
    st.f32(e, "size"); st.f32(e, "limit"); st.f32(e, "offset")
    st.f32(e, "in_wall_factor"); st.f32(e, "out_wall_factor"); st.f32(e, "angle")
    st.u8(e, "flags"); st.u8(e, "type"); st.u8(e, "number"); st.u8(e, "dummy")
    st.u32(e, "color"); st.u32(e, "death_color")


def eff(st, o):
    st.memsize(o)
    F.mdf_header(st, o)
    ver = o["version"]
    st.u32(o, "material_key")             # -> LOA_MakeFileRef
    gt = st.u16(o, "gfx_type")            # EFF::SetGfxType
    st.u16(o, "mat_id")
    st.u8(o, "b0")
    if ver >= 2:
        st.u8(o, "b1")
    if ver >= 1:
        st.u32(o, "u80")
    st.u16(o, "eff_flags")
    e = o.setdefault("gfx", {})
    if gt == 2:
        eff_line(st, e)
    elif gt == 3:
        eff_lightning(st, e)
    elif gt == 6:
        eff_lasso(st, e)
    elif gt == 4:
        eff_flare(st, e)
    st.box(o, "bv")
    return o


def refs_eff(o) -> list[int]:
    e = o.get("gfx", {})
    return _keys(o.get("material_key", 0), e.get("curve1_key", 0), e.get("curve2_key", 0))


# ============================================================================
# PAR: PAR::p_Callback_LoadPAR (RGH 80114D00) for versions >= 0x33
#      PAR_subsystem::pu32_LoadFromBuffer (RGH 801148A8)
# ============================================================================
def par_subsystem(st, s):
    ver = st.u32(s, "version", default=3)
    st.u32(s, "flags")
    if ver >= 2:
        st.u8(s, "id")
    if ver >= 1:
        st.trm(s, "matrix")
    if ver < 3:
        st.u8(s, "old_b0"); st.u8(s, "old_b1"); st.u32(s, "old_u")   # stored into the PAR
    st.u32(s, "raycast_obj_key")          # PAR_subsystem::mpo_RayCastObj (an OBJ key)
    st.u32(s, "model_key")                # -> LOA_MakeFileRef (a K3D_PArticleModel file)


def par(st, o):
    st.memsize(o)
    F.mdf_header(st, o)
    ver = o["version"]
    if ver < 0x2A:
        raise FormatError(f"PAR version {ver:#x} (the loader skips it)")
    if ver < 0x33:
        raise FormatError(f"PAR version {ver:#x}: old inline particle-model format not supported")
    for s in _items(st, o, "subsystems", "n_subsystems"):
        if st.writing:
            s["present"] = 1 if s.get("present", 1) else 0
        if st.u32(s, "present", default=1):
            par_subsystem(st, s)
    if ver >= 0x34:
        st.u8(o, "b155"); st.u32(o, "u9c")
    if ver >= 0x35:
        st.u32(o, "global_color"); st.f32(o, "gsize")
    if ver >= 0x36:
        st.u32(o, "model_mode")           # PAR::mu32_model
    if ver >= 0x37:
        st.u32(o, "pause_when_invisible")
    if ver >= 0x38:
        st.u8(o, "b15a")
    return o


def refs_par(o) -> list[int]:
    return _keys(*[s.get("model_key", 0) for s in o.get("subsystems", [])])


# ============================================================================
# The particle model a PAR subsystem points at (not a modifier):
#   K3D_PArticleModel::p_Callback_LoadParticleModel (RGH 8031BD30)
#   -> K3D_PArticleModel::pc_LoadFromBuffer (RGH 8031A4C4)
# RGH accepts versions <= 0x10 (every RGH model is 0x10).
# ============================================================================
def cylinder(st, o, name):
    """MTH_pu8_LoadCylinderFromBuffer: radius, height, centre, then a word only loose files keep."""
    v = st.raw(o, name, 20)
    st.u32(o, name + "_m1", 1)
    return v


# patch_ConvertOldSpawnShapeType: old u8 -> (GenShapeType, ShapeOrient)
SPAWN_SHAPE_CONVERT = {0: (0, 0), 1: (1, 1), 2: (1, 2), 3: (1, 3), 4: (1, 0), 5: (2, 1),
                       6: (2, 2), 7: (2, 3), 8: (3, 0), 9: (5, 0), 10: (6, 0), 11: (4, 0)}

MODEL_CURVES = ("speed_axis_add_x", "speed_axis_add_y", "speed_axis_add_z", "nb_per_sec",
                "speed", "speed_up", "speed_angle", "angle", "freq_emitter_speed",
                "time_size_mul_x", "time_size_mul_y", "time_alpha_mul")
PHASE_CURVES_A = ("phase_blend_color", "phase_blend_alpha")
PHASE_CURVES_B = ("blend_color", "blend_alpha", "blend_size", "size_x", "size_y")
PHASE_CURVES_C = ("friction", "add_speed_x", "add_speed_y", "add_speed_z", "friction_speed_up",
                  "add_speed_up_x", "add_speed_up_y", "add_speed_up_z", "rotate_friction",
                  "add_rotate", "add_pos_x", "add_pos_y", "add_pos_z", "time_prob")


def par_phase(st, ph, ver):
    """One PAR_tt_Phase (the NumPhase loop of pc_LoadFromBuffer)."""
    st.u16(ph, "sub_material"); st.u16(ph, "flags")
    st.f32(ph, "time_min"); st.f32(ph, "time_max")
    for i in range(8):                    # interleaved min/max, ABGR in the file
        st.u32(ph, f"color_min{i}"); st.u32(ph, f"color_max{i}")
    for nm in PHASE_CURVES_A:
        st.u32(ph, "curve_" + nm)         # -> LOA_MakeFileRef
    st.f32(ph, "size_min_x"); st.f32(ph, "size_min_y")
    st.f32(ph, "size_max_x"); st.f32(ph, "size_max_y")
    if ver >= 4:
        st.f32(ph, "size_min_z"); st.f32(ph, "size_max_z")
    for nm in PHASE_CURVES_B:
        st.u32(ph, "curve_" + nm)
    if ver >= 5:
        st.u32(ph, "curve_size_z")
    for nm in PHASE_CURVES_C:
        st.u32(ph, "curve_" + nm)
    st.array(ph, "curve_transfert", "<I", 4)
    for nm in ("uv_mode", "uv_div_x", "uv_div_y", "uv_first_x", "uv_first_y",
               "uv_last_x", "uv_last_y", "uv_flags", "speed_rotate_axis", "speed_up_rotate_axis"):
        st.u16(ph, nm)
    if ver >= 0xE:
        st.u16(ph, "add_rotate_axis")


def particle_model_data(st, o):
    """K3D_PArticleModel::pc_LoadFromBuffer."""
    ver = st.u32(o, "version", default=0x10)
    if ver > 0x10:
        raise FormatError(f"particle model version {ver:#x}")
    st.u32(o, "par_flags")
    st.box(o, "bv")
    st.u32(o, "max_part")
    st.f32(o, "nb_per_sec_init"); st.f32(o, "sim_time_init")
    st.u32(o, "gen_once_value"); st.f32(o, "auto_stop_time")
    if ver >= 2:
        st.f32(o, "auto_stop_loop")
    else:
        st.u32(o, "old_u9")
    if ver >= 3:
        st.f32(o, "delay_before_generation")
    if ver >= 0xC:
        st.u32(o, "propagate_head_color"); st.u32(o, "propagate_head_alpha")
        st.u32(o, "head_color")           # ABGR in the file
    if ver >= 4:
        st.u32(o, "uv_mode_global"); st.f32(o, "uv_anim_global_freq")
    for nm in ("speed_min", "speed_max", "speed_angle1", "speed_angle2", "angle_min", "angle_max"):
        st.f32(o, nm)
    if ver >= 4:
        for nm in ("angle_min_y", "angle_max_y", "angle_min_z", "angle_max_z"):
            st.f32(o, nm)
    st.f32(o, "angle_init_min"); st.f32(o, "angle_init_max")
    if ver >= 9:
        for nm in ("axial_speed", "delta_axial_speed", "radial_speed", "delta_radial_speed",
                   "tangential_speed", "delta_tangential_speed"):
            st.f32(o, nm)
        st.v3(o, "axial_acc"); st.v3(o, "delta_axial_acc")
        for nm in ("radial_acc", "delta_radial_acc", "gravity", "delta_gravity", "trail_enlarge_ratio"):
            st.f32(o, nm)
        st.u32(o, "nb_spawn_points")
    st.f32(o, "f42")                      # read; destination not identified
    st.v3(o, "acc_min"); st.v3(o, "acc_max")
    st.f32(o, "mul_size_x"); st.f32(o, "mul_size_y")
    old = st.u8(o, "old_gen_shape_type")  # -> patch_ConvertOldSpawnShapeType
    if old not in SPAWN_SHAPE_CONVERT:
        raise FormatError(f"particle model spawn shape {old}")
    shape, o["shape_orient"] = SPAWN_SHAPE_CONVERT[old]
    o["gen_shape_type"] = shape
    st.u8(o, "speed_type")
    st.u8(o, "orientation_type")          # patch_ConvertOldOrientationType below version 5
    if ver >= 5:
        st.u8(o, "collision_type"); st.u32(o, "draw_mask")
    if ver >= 2:
        st.u8(o, "trail_flags")
    st.f32(o, "rotate_offset")
    if ver >= 2:
        st.v3(o, "local_pivot")
    else:
        st.f32(o, "old_pivot_x"); st.f32(o, "old_pivot_y")
    if ver >= 0xD:
        st.f32(o, "depth_offset")
    st.f32(o, "objt_factor")
    if ver >= 6:
        st.f32(o, "lod_distance_min"); st.f32(o, "lod_distance_max"); st.u32(o, "lod_percent")
    st.u32(o, "meta_def"); st.f32(o, "meta_level"); st.f32(o, "meta_persp")
    st.u32(o, "trail_mode"); st.u32(o, "trail_num_olds"); st.f32(o, "trail_old_timer")
    st.u16(o, "trail_u16a"); st.u16(o, "trail_color_mode")
    if ver >= 0xC:
        st.u16(o, "trail_alpha_mode")
    st.f32(o, "trail_fixed_factor"); st.f32(o, "trail_tgt_fixed"); st.f32(o, "trail_tgt_beg")
    if ver >= 2:
        st.f32(o, "trail_tile_length"); st.u32(o, "trail_color")
    if ver >= 0x10:
        st.f32(o, "trail_max_length"); st.u32(o, "is_trail_length_limit")
    st.u32(o, "gen_geom_key")             # -> LOA_MakeFileRef (a visual)
    st.u32(o, "gen_geom_type")
    st.u32(o, "material_key")             # -> LOA_MakeFileRef
    st.u32(o, "visual_key")               # -> LOA_MakeFileRef
    for nm in MODEL_CURVES:
        st.u32(o, "curve_" + nm)          # -> LOA_MakeFileRef
    st.array(o, "transfert_mode", "<I", 4)
    st.u32(o, "in_transfert")
    st.array(o, "transfert_mdf_id", "<I", 4)
    if ver >= 1:
        st.array(o, "transfert_subsystem_id", "<B", 4)
    if shape == 0:
        st.v3(o, "point")
    elif shape == 1:
        st.box(o, "shape_box")
    elif shape in (2, 3, 4):
        st.sphere(o, "shape_sphere")
    elif shape == 5:
        cylinder(st, o, "shape_cylinder")
    cons = o.setdefault("constraints", [])
    if not st.writing:
        cons[:] = [{} for _ in range(8)]
    for c in cons[:8]:
        st.u32(c, "type"); st.array(c, "params", "<f", 16)
    if ver >= 0xA:
        st.u32(o, "activation_mode"); st.f32(o, "emitter_life_time")
        st.f32(o, "interval_between_two_phasis"); st.u32(o, "random_seed")
        st.u8(o, "b105")
        st.v3(o, "contact_limit_speed"); st.f32(o, "contact_limit_rotate")
        st.v3(o, "speed_friction_on_contact"); st.f32(o, "rotate_friction_on_contact")
        st.f32(o, "random_contact_spread")
    if ver >= 7:
        st.u32(o, "pam_texture_key")      # -> LOA_MakeFileRef
        st.i32(o, "blend_mode"); st.f32(o, "dist_fade_tresh"); st.u32(o, "depth_trans")
    if ver >= 0xB:
        st.u32(o, "final_color")          # ABGR in the file
        st.f32(o, "final_size_x"); st.f32(o, "final_size_y"); st.f32(o, "final_size_z")
    if ver >= 8:
        st.u32(o, "uv_use_animation"); st.u32(o, "uv_anim_start_random")
        st.u8(o, "uv_rotate_option"); st.u8(o, "uv_anim_option")
        st.u16(o, "uv_number_u"); st.u16(o, "uv_number_v")
    for ph in _items(st, o, "phases", "num_phase"):
        par_phase(st, ph, ver)
    if ver > 0xE:
        st.u8(o, "light_mask")


def particle_model(st, o):
    """K3D_PArticleModel::p_Callback_LoadParticleModel: the model resource."""
    st.memsize(o)
    particle_model_data(st, o)
    return o


def refs_particle_model(o) -> list[int]:
    out = [o.get("gen_geom_key", 0), o.get("material_key", 0), o.get("visual_key", 0),
           o.get("pam_texture_key", 0)]
    out += [o.get("curve_" + nm, 0) for nm in MODEL_CURVES]
    for ph in o.get("phases", []):
        out += [ph.get("curve_" + nm, 0) for nm in PHASE_CURVES_A + PHASE_CURVES_B + PHASE_CURVES_C]
        out.append(ph.get("curve_size_z", 0))
        out += list(ph.get("curve_transfert", []))
    return _keys(*out)
