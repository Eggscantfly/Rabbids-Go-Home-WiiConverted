"""Wii lighting and glow for the port folder's copy of the PC release's shaders.

    report = patch_shaders(shaders_dir)      {"applied": [...], "already": [...], "differs": [...]}
    state = shader_state(shaders_dir)        the same lists, without writing

The PC release draws the Wii materials with HLSL versions of the Wii's hardware lighting (include/WIICommon.fxh and
core/Wii_MaterialsTemplates.fx: the lights are packed like GXInitLightAttn, GXInitLightPos and GXInitLightDir) and of
its glow (core/Basic_Glow.fx).  Where that HLSL computes something the Wii does not, the port folder's copy is changed
to the Wii's arithmetic:

  spot cosine        GXInitLightDir stores the negated direction and the lighting takes max(0, dot(L, -direction)),
                     L the unit vector to the light.  The HLSL took abs(dot(L, direction)), so a spot light also lit
                     the mirrored cone behind it.
  angle attenuation  max(0, a0 + a1 cos + a2 cos^2); the HLSL also capped it at 4.
  distance attenuation  every light starts from 1; the HLSL kept the previous light's attenuation for a light without
                     attenuation terms.
  normals            the Wii loads the inverse transpose of the model-view matrix as the normal matrix
                     (PSMTXInvXpose, GXLoadNrmMtxImm) and lights unit normals.  The HLSL multiplied normals by the
                     world matrix, so a scaled object was lit brighter or darker than on the Wii and a stretched one
                     with bent normals.  The cofactors of the world matrix give the inverse transpose's direction
                     (the view matrix only rotates and moves).
  lightmap UV        the vertex shader wrote the lit colour into the lightmap coordinates of every material; only
                     cartoon materials look their ramp up that way.
  glow               the Wii adds the blurred glow image, scaled by the glow factor, to the frame
                     (AFX_S_Glow::Apply); the HLSL added the brighter of the blurred and the unblurred glow image.

Each change replaces one exact piece of the release's text (in the file's own line endings).  A file whose text
differs from the release is left as it is and reported under "differs".
"""
from __future__ import annotations

import os
from typing import NamedTuple


class Patch(NamedTuple):
    file: str      # relative to the shaders folder
    name: str
    old: str       # "\n" line endings; converted to the file's
    new: str


_LIGHT_LOOP_FILES = ("include/WIICommon.fxh", "core/Wii_MaterialsTemplates.fx")

PATCHES: tuple[Patch, ...] = tuple(
    p for f in _LIGHT_LOOP_FILES for p in (
        Patch(f, "spot cosine",
              "                AAtt = abs(clamp(AAtt,-1.f,1.f));\n",
              "                AAtt = max(-AAtt, 0.f);  // Wii: the stored direction is negated, clamped at 0\n"),
        Patch(f, "angle attenuation",
              "            ap = clamp(ap,0,4.f);\n",
              "            ap = max(ap, 0.f);  // Wii: not capped\n"),
        Patch(f, "distance attenuation",
              "        if(vs_LigSets[i].v3.w > 0) // use Atten\n",
              "        Atten = 1.f;  // Wii: per light\n"
              "        if(vs_LigSets[i].v3.w > 0) // use Atten\n"),
    )
) + (
    Patch("core/Wii_MaterialsTemplates.fx", "normal matrix",
          "VS_MULTI_OUTPUT\nVS_MultiTex(",
          "// Wii: lights unit normals transformed by the normal matrix, the inverse transpose of the model-view\n"
          "// matrix. The cofactors of the world matrix give its direction; the sign follows the determinant.\n"
          "float3 WiiLightNormal(in float3 _n)\n"
          "{\n"
          "  float3 r0 = mat_W[0].xyz;\n"
          "  float3 r1 = mat_W[1].xyz;\n"
          "  float3 r2 = mat_W[2].xyz;\n"
          "  float3 c0 = cross(r1, r2);\n"
          "  float3 n = float3(dot(_n, c0), dot(_n, cross(r2, r0)), dot(_n, cross(r0, r1)));\n"
          "  if (dot(r0, c0) < 0.f)\n"
          "    n = -n;\n"
          "  return n * rsqrt(max(dot(n, n), 1e-20f));\n"
          "}\n"
          "\n"
          "VS_MULTI_OUTPUT\nVS_MultiTex("),
    Patch("core/Wii_MaterialsTemplates.fx", "normal matrix (cartoon lighting)",
          "                        Pos.xyz,\n"
          "                        hlsl_mul(Normal.xyz, mat_W));\n",
          "                        Pos.xyz,\n"
          "                        WiiLightNormal(Normal.xyz));\n"),
    Patch("core/Wii_MaterialsTemplates.fx", "normal matrix (lighting)",
          "                       Pos.xyz,\n"
          "                       hlsl_mul(Normal.xyz, mat_W));\n",
          "                       Pos.xyz,\n"
          "                       WiiLightNormal(Normal.xyz));\n"),
    Patch("core/Wii_MaterialsTemplates.fx", "lightmap UV",
          "  Output.UV_ZisDivisor[NB_Levels] = float3(Diffuse.xy, 1.f);\n",
          "  if (b_VS_UseCartoon.x)  // the cartoon ramp lookup; lightmaps keep UV2\n"
          "    Output.UV_ZisDivisor[NB_Levels] = float3(Diffuse.xy, 1.f);\n"),
    Patch("core/Basic_Glow.fx", "glow",
          "  \treturn max(tex2D( samp_Texture, _f2_TexCoord ),\n"
          "             tex2D( samp_GlowColor, _f2_TexCoord ));\n",
          "  \treturn tex2D( samp_Texture, _f2_TexCoord );  // Wii: the blurred glow only\n"),
)

# The release's shader text is ASCII apart from a few Latin-1 comment characters; latin-1 round-trips every byte.
_ENCODING = "latin-1"


def _run(shaders_dir: str, write: bool) -> dict:
    rep = {"applied": [], "already": [], "differs": []}
    for fn in sorted({p.file for p in PATCHES}):
        path = os.path.join(shaders_dir, *fn.split("/"))
        try:
            with open(path, "rb") as f:
                text = f.read().decode(_ENCODING)
        except OSError:
            rep["differs"].extend("%s: %s (file missing)" % (fn, p.name) for p in PATCHES if p.file == fn)
            continue
        nl = "\r\n" if "\r\n" in text else "\n"
        changed = text
        for p in (p for p in PATCHES if p.file == fn):
            old, new = p.old.replace("\n", nl), p.new.replace("\n", nl)
            label = "%s: %s" % (fn, p.name)
            if new in changed:
                rep["already"].append(label)
            elif changed.count(old) == 1:
                changed = changed.replace(old, new)
                rep["applied"].append(label)
            else:
                rep["differs"].append(label)
        if write and changed != text:
            with open(path, "wb") as f:
                f.write(changed.encode(_ENCODING))
    return rep


def patch_shaders(shaders_dir: str) -> dict:
    """Apply the Wii lighting and glow changes to a shaders folder (the port folder's copy)."""
    return _run(shaders_dir, write=True)


def shader_state(shaders_dir: str) -> dict:
    """Which changes a shaders folder has ("already"), lacks ("applied" would apply them) or cannot take ("differs")."""
    return _run(shaders_dir, write=False)
