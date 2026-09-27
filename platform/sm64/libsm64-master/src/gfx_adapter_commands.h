#pragma once

// The commands libsm64's display lists are made of (see decomp/include/PR/gbi_libsm64.h).  A Gfx is one record of
// GFX_WORDS words: the command, then its arguments.
#define GFX_WORDS 8

enum GFXAdapterCommands
{
    GFXCMD_None = 0,
    GFXCMD_VertexData,          // Vtx *v, n, v0
    GFXCMD_Triangle,            // v0, v1, v2
    GFXCMD_Light,               // Light *l, n (1: the directional light, 2: the ambient)
    GFXCMD_Texture,             // scale s, scale t, on
    GFXCMD_SetTileSize,         // uls, ult, lrs, lrt (10.2)
    GFXCMD_SetTextureImage,     // the image: a Mario texture number or an SM64TexRef *
    GFXCMD_SubDisplayList,      // Gfx *dl (call)
    GFXCMD_EndDisplayList,
    GFXCMD_BranchList,          // Gfx *dl (jump)
    GFXCMD_SetGeometryMode,     // word
    GFXCMD_ClearGeometryMode,   // word
    GFXCMD_SetEnvColor,         // r, g, b, a
    GFXCMD_SetPrimColor,        // r, g, b, a
    GFXCMD_SetCombineMode,      // cycle 1, cycle 2 (packed, LIBSM64_CC_PACK)
    GFXCMD_SetTile,             // fmt, siz, tile, cms, cmt
    GFXCMD_SetRenderMode,       // cycle 1, cycle 2
    GFXCMD_SetAlphaCompare,     // mode
};
