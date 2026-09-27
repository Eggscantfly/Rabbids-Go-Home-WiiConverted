/*
 * gbi_libsm64.h - libsm64's display lists.
 *
 * SM64's models are display lists for the N64's RSP: arrays of Gfx words made by the gs* macros of gbi.h, read by
 * the microcode.  libsm64 has no RSP; it walks them itself (gfx_adapter.c) and turns them into triangles for the
 * program that embeds it.  So every macro a model or a behaviour uses is redefined here to write the adapter's own
 * commands instead: a Gfx is one fixed-size record of GFX_WORDS machine words, the first naming the command
 * (gfx_adapter_commands.h) and the rest its arguments, so that a static list initialised from a flat sequence of
 * the gs* macros (each exactly one or two records) and a list a behaviour builds at run time with the g* macros
 * (alloc_display_list(n * sizeof(Gfx)), one call per record) come out the same.  Only what SM64's own data and
 * code use is defined.
 *
 * Included at the end of gbi.h, which keeps the rest of the graphics binary interface (the vertex and light types,
 * the combiner and geometry mode words) as it is.
 */
#pragma once

#include <stdint.h>
#include "../../../gfx_adapter_commands.h"

typedef struct { intptr_t w[GFX_WORDS]; } Gfx;

#define LIBSM64_GFX_PAD4 0, 0, 0, 0
#define LIBSM64_GFX_PAD5 0, 0, 0, 0, 0
#define LIBSM64_GFX_PAD6 0, 0, 0, 0, 0, 0
#define LIBSM64_GFX_PAD7 0, 0, 0, 0, 0, 0, 0
#define LIBSM64_GFX_NOP  GFXCMD_None, LIBSM64_GFX_PAD7

/* The colour combiner's inputs, packed into one word: four 5-bit RGB sources then four 3-bit alpha sources, in
 * the (a - b) * c + d order of the hardware.  gbi.h's G_CC_* names expand to the eight source names, which paste
 * onto G_CCMUX_ / G_ACMUX_ the way gbi.h's own combiner macros paste them. */
#define LIBSM64_CC_PACK(a, b, c, d, Aa, Ab, Ac, Ad) \
    ((intptr_t)(((G_CCMUX_##a) & 31) | (((G_CCMUX_##b) & 31) << 5) | (((G_CCMUX_##c) & 31) << 10) | \
                (((G_CCMUX_##d) & 31) << 15) | (((G_ACMUX_##Aa) & 7) << 20) | (((G_ACMUX_##Ab) & 7) << 23) | \
                (((G_ACMUX_##Ac) & 7) << 26) | (((G_ACMUX_##Ad) & 7) << 29)))
#define LIBSM64_CC(...) LIBSM64_CC_PACK(__VA_ARGS__)

/* ------------------------------------------------------------------ the static macros (model data) */
#undef gsSPVertex
#undef gsSP1Triangle
#undef gsSP2Triangles
#undef gsSPEndDisplayList
#undef gsSPDisplayList
#undef gsSPBranchList
#undef gsSPLight
#undef gsSPNumLights
#undef gsSPSetLights1
#undef gsSPLightColor
#undef gsSPTexture
#undef gsDPSetTextureImage
#undef gsDPSetTile
#undef gsDPSetTileSize
#undef gsDPLoadTextureBlock
#undef gsDPLoadBlock
#undef gsDPLoadSync
#undef gsDPTileSync
#undef gsDPPipeSync
#undef gsDPFullSync
#undef gsSPSetGeometryMode
#undef gsSPClearGeometryMode
#undef gsSPGeometryMode
#undef gsDPSetEnvColor
#undef gsDPSetPrimColor
#undef gsDPSetFogColor
#undef gsDPSetBlendColor
#undef gsDPSetCombineMode
#undef gsDPSetCombineLERP
#undef gsDPSetRenderMode
#undef gsDPSetAlphaCompare
#undef gsDPSetCycleType
#undef gsDPSetTextureFilter
#undef gsDPSetTexturePersp
#undef gsDPSetTextureLUT
#undef gsDPSetTextureLOD
#undef gsDPSetTextureDetail
#undef gsDPSetTextureConvert
#undef gsDPSetAlphaDither
#undef gsDPSetColorDither
#undef gsDPSetDepthSource
#undef gsSPFogPosition
#undef gsSPPerspNormalize
#undef gsSPClipRatio
#undef gsSPViewport
#undef gsSPMatrix
#undef gsSPPopMatrix
#undef gsSPCullDisplayList
#undef gsSPSegment

#define gsSPVertex(v, n, v0)              GFXCMD_VertexData, (intptr_t)(v), (intptr_t)(n), (intptr_t)(v0), LIBSM64_GFX_PAD4
#define gsSP1Triangle(v0, v1, v2, flag)   GFXCMD_Triangle, (intptr_t)(v0), (intptr_t)(v1), (intptr_t)(v2), LIBSM64_GFX_PAD4
#define gsSP2Triangles(v00, v01, v02, flag0, v10, v11, v12, flag1) \
    gsSP1Triangle(v00, v01, v02, flag0), gsSP1Triangle(v10, v11, v12, flag1)
#define gsSPEndDisplayList()              GFXCMD_EndDisplayList, LIBSM64_GFX_PAD7
#define gsSPDisplayList(dl)               GFXCMD_SubDisplayList, (intptr_t)(dl), LIBSM64_GFX_PAD6
#define gsSPBranchList(dl)                GFXCMD_BranchList, (intptr_t)(dl), LIBSM64_GFX_PAD6
#define gsSPLight(l, n)                   GFXCMD_Light, (intptr_t)(l), (intptr_t)(n), LIBSM64_GFX_PAD5
#define gsSPNumLights(n)                  LIBSM64_GFX_NOP
#define gsSPSetLights1(name)              gsSPLight(&(name).l[0], 1), gsSPLight(&(name).a, 2)
#define gsSPLightColor(n, col)            LIBSM64_GFX_NOP
#define gsSPTexture(s, t, level, tile, on) GFXCMD_Texture, (intptr_t)(s), (intptr_t)(t), (intptr_t)(on), LIBSM64_GFX_PAD4
#define gsDPSetTextureImage(f, s, w, i)   GFXCMD_SetTextureImage, (intptr_t)(i), LIBSM64_GFX_PAD6
#define gsDPSetTile(fmt, siz, line, tmem, tile, palette, cmt, maskt, shiftt, cms, masks, shifts) \
    GFXCMD_SetTile, (intptr_t)(fmt), (intptr_t)(siz), (intptr_t)(tile), (intptr_t)(cms), (intptr_t)(cmt), 0, 0
#define gsDPSetTileSize(t, uls, ult, lrs, lrt) \
    GFXCMD_SetTileSize, (intptr_t)(uls), (intptr_t)(ult), (intptr_t)(lrs), (intptr_t)(lrt), 0, 0, 0
#define gsDPLoadTextureBlock(timg, fmt, siz, width, height, pal, cms, cmt, masks, maskt, shifts, shiftt) \
    gsDPSetTextureImage(fmt, siz, width, timg), \
    gsDPSetTile(fmt, siz, 0, 0, 0, pal, cmt, maskt, shiftt, cms, masks, shifts), \
    gsDPSetTileSize(0, 0, 0, ((width) - 1) << 2, ((height) - 1) << 2)
#define gsDPLoadBlock(tile, uls, ult, lrs, dxt) LIBSM64_GFX_NOP
#define gsDPLoadSync()                    LIBSM64_GFX_NOP
#define gsDPTileSync()                    LIBSM64_GFX_NOP
#define gsDPPipeSync()                    LIBSM64_GFX_NOP
#define gsDPFullSync()                    LIBSM64_GFX_NOP
#define gsSPSetGeometryMode(word)         GFXCMD_SetGeometryMode, (intptr_t)(word), LIBSM64_GFX_PAD6
#define gsSPClearGeometryMode(word)       GFXCMD_ClearGeometryMode, (intptr_t)(word), LIBSM64_GFX_PAD6
#define gsSPGeometryMode(c, s)            gsSPClearGeometryMode(c), gsSPSetGeometryMode(s)
#define gsDPSetEnvColor(r, g, b, a)       GFXCMD_SetEnvColor, (intptr_t)(r), (intptr_t)(g), (intptr_t)(b), (intptr_t)(a), 0, 0, 0
#define gsDPSetPrimColor(m, l, r, g, b, a) GFXCMD_SetPrimColor, (intptr_t)(r), (intptr_t)(g), (intptr_t)(b), (intptr_t)(a), 0, 0, 0
#define gsDPSetFogColor(r, g, b, a)       LIBSM64_GFX_NOP
#define gsDPSetBlendColor(r, g, b, a)     LIBSM64_GFX_NOP
#define gsDPSetCombineMode(a, b)          GFXCMD_SetCombineMode, LIBSM64_CC(a), LIBSM64_CC(b), LIBSM64_GFX_PAD5
#define gsDPSetCombineLERP(a0, b0, c0, d0, Aa0, Ab0, Ac0, Ad0, a1, b1, c1, d1, Aa1, Ab1, Ac1, Ad1) \
    GFXCMD_SetCombineMode, LIBSM64_CC_PACK(a0, b0, c0, d0, Aa0, Ab0, Ac0, Ad0), \
    LIBSM64_CC_PACK(a1, b1, c1, d1, Aa1, Ab1, Ac1, Ad1), LIBSM64_GFX_PAD5
#define gsDPSetRenderMode(c0, c1)         GFXCMD_SetRenderMode, (intptr_t)(c0), (intptr_t)(c1), LIBSM64_GFX_PAD5
#define gsDPSetAlphaCompare(type)         GFXCMD_SetAlphaCompare, (intptr_t)(type), LIBSM64_GFX_PAD6
#define gsDPSetCycleType(type)            LIBSM64_GFX_NOP
#define gsDPSetTextureFilter(type)        LIBSM64_GFX_NOP
#define gsDPSetTexturePersp(type)         LIBSM64_GFX_NOP
#define gsDPSetTextureLUT(type)           LIBSM64_GFX_NOP
#define gsDPSetTextureLOD(type)           LIBSM64_GFX_NOP
#define gsDPSetTextureDetail(type)        LIBSM64_GFX_NOP
#define gsDPSetTextureConvert(type)       LIBSM64_GFX_NOP
#define gsDPSetAlphaDither(type)          LIBSM64_GFX_NOP
#define gsDPSetColorDither(type)          LIBSM64_GFX_NOP
#define gsDPSetDepthSource(type)          LIBSM64_GFX_NOP
#define gsSPFogPosition(min, max)         LIBSM64_GFX_NOP
#define gsSPPerspNormalize(s)             LIBSM64_GFX_NOP
#define gsSPClipRatio(r)                  LIBSM64_GFX_NOP
#define gsSPViewport(v)                   LIBSM64_GFX_NOP
#define gsSPMatrix(m, p)                  LIBSM64_GFX_NOP
#define gsSPPopMatrix(n)                  LIBSM64_GFX_NOP
#define gsSPCullDisplayList(vstart, vend) LIBSM64_GFX_NOP
#define gsSPSegment(segment, base)        LIBSM64_GFX_NOP

/* ------------------------------------------------------------------ the run-time macros (behaviour code) */
static inline void libsm64_gfx_put(void *pkt, intptr_t a, intptr_t b, intptr_t c, intptr_t d, intptr_t e) {
    Gfx *g = (Gfx *)pkt;
    g->w[0] = a; g->w[1] = b; g->w[2] = c; g->w[3] = d; g->w[4] = e;
    g->w[5] = 0; g->w[6] = 0; g->w[7] = 0;
}

#undef gSPVertex
#undef gSP1Triangle
#undef gSP2Triangles
#undef gSPEndDisplayList
#undef gSPDisplayList
#undef gSPBranchList
#undef gSPLight
#undef gSPNumLights
#undef gSPSetLights1
#undef gSPLightColor
#undef gSPTexture
#undef gDPSetTextureImage
#undef gDPSetTile
#undef gDPSetTileSize
#undef gDPLoadTextureBlock
#undef gDPLoadBlock
#undef gDPLoadSync
#undef gDPTileSync
#undef gDPPipeSync
#undef gDPFullSync
#undef gSPSetGeometryMode
#undef gSPClearGeometryMode
#undef gSPGeometryMode
#undef gDPSetEnvColor
#undef gDPSetPrimColor
#undef gDPSetFogColor
#undef gDPSetBlendColor
#undef gDPSetCombineMode
#undef gDPSetRenderMode
#undef gDPSetAlphaCompare
#undef gDPSetCycleType
#undef gDPSetTextureFilter
#undef gDPSetTexturePersp
#undef gDPSetTextureLUT
#undef gDPSetDepthSource
#undef gSPFogPosition
#undef gSPPerspNormalize
#undef gSPClipRatio
#undef gSPViewport
#undef gSPMatrix
#undef gSPPopMatrix
#undef gDPFillRectangle
#undef gSPCullDisplayList
#undef gSPSegment
#undef gDPSetScissor
#undef gDPSetFillColor
#undef gSPLookAt

#define gSPVertex(pkt, v, n, v0)          libsm64_gfx_put((pkt), GFXCMD_VertexData, (intptr_t)(v), (intptr_t)(n), (intptr_t)(v0), 0)
#define gSP1Triangle(pkt, v0, v1, v2, f)  libsm64_gfx_put((pkt), GFXCMD_Triangle, (intptr_t)(v0), (intptr_t)(v1), (intptr_t)(v2), 0)
#define gSPEndDisplayList(pkt)            libsm64_gfx_put((pkt), GFXCMD_EndDisplayList, 0, 0, 0, 0)
#define gSPDisplayList(pkt, dl)           libsm64_gfx_put((pkt), GFXCMD_SubDisplayList, (intptr_t)(dl), 0, 0, 0)
#define gSPBranchList(pkt, dl)            libsm64_gfx_put((pkt), GFXCMD_BranchList, (intptr_t)(dl), 0, 0, 0)
#define gSPLight(pkt, l, n)               libsm64_gfx_put((pkt), GFXCMD_Light, (intptr_t)(l), (intptr_t)(n), 0, 0)
#define gSPNumLights(pkt, n)              libsm64_gfx_put((pkt), GFXCMD_None, 0, 0, 0, 0)
#define gSPLightColor(pkt, n, col)        libsm64_gfx_put((pkt), GFXCMD_None, 0, 0, 0, 0)
#define gSPTexture(pkt, s, t, level, tile, on) libsm64_gfx_put((pkt), GFXCMD_Texture, (intptr_t)(s), (intptr_t)(t), (intptr_t)(on), 0)
#define gDPSetTextureImage(pkt, f, s, w, i) libsm64_gfx_put((pkt), GFXCMD_SetTextureImage, (intptr_t)(i), 0, 0, 0)
#define gDPSetTile(pkt, fmt, siz, line, tmem, tile, palette, cmt, maskt, shiftt, cms, masks, shifts) \
    libsm64_gfx_put((pkt), GFXCMD_SetTile, (intptr_t)(fmt), (intptr_t)(siz), (intptr_t)(tile), (intptr_t)(cms))
#define gDPSetTileSize(pkt, t, uls, ult, lrs, lrt) \
    libsm64_gfx_put((pkt), GFXCMD_SetTileSize, (intptr_t)(uls), (intptr_t)(ult), (intptr_t)(lrs), (intptr_t)(lrt))
#define gDPLoadBlock(pkt, tile, uls, ult, lrs, dxt) libsm64_gfx_put((pkt), GFXCMD_None, 0, 0, 0, 0)
#define gDPLoadSync(pkt)                  libsm64_gfx_put((pkt), GFXCMD_None, 0, 0, 0, 0)
#define gDPTileSync(pkt)                  libsm64_gfx_put((pkt), GFXCMD_None, 0, 0, 0, 0)
#define gDPPipeSync(pkt)                  libsm64_gfx_put((pkt), GFXCMD_None, 0, 0, 0, 0)
#define gDPFullSync(pkt)                  libsm64_gfx_put((pkt), GFXCMD_None, 0, 0, 0, 0)
#define gSPSetGeometryMode(pkt, word)     libsm64_gfx_put((pkt), GFXCMD_SetGeometryMode, (intptr_t)(word), 0, 0, 0)
#define gSPClearGeometryMode(pkt, word)   libsm64_gfx_put((pkt), GFXCMD_ClearGeometryMode, (intptr_t)(word), 0, 0, 0)
#define gDPSetEnvColor(pkt, r, g, b, a)   libsm64_gfx_put((pkt), GFXCMD_SetEnvColor, (intptr_t)(r), (intptr_t)(g), (intptr_t)(b), (intptr_t)(a))
#define gDPSetPrimColor(pkt, m, l, r, g, b, a) libsm64_gfx_put((pkt), GFXCMD_SetPrimColor, (intptr_t)(r), (intptr_t)(g), (intptr_t)(b), (intptr_t)(a))
#define gDPSetFogColor(pkt, r, g, b, a)   libsm64_gfx_put((pkt), GFXCMD_None, 0, 0, 0, 0)
#define gDPSetBlendColor(pkt, r, g, b, a) libsm64_gfx_put((pkt), GFXCMD_None, 0, 0, 0, 0)
#define gDPSetCombineMode(pkt, a, b)      libsm64_gfx_put((pkt), GFXCMD_SetCombineMode, LIBSM64_CC(a), LIBSM64_CC(b), 0, 0)
#define gDPSetRenderMode(pkt, c0, c1)     libsm64_gfx_put((pkt), GFXCMD_SetRenderMode, (intptr_t)(c0), (intptr_t)(c1), 0, 0)
#define gDPSetAlphaCompare(pkt, type)     libsm64_gfx_put((pkt), GFXCMD_SetAlphaCompare, (intptr_t)(type), 0, 0, 0)
#define gDPSetCycleType(pkt, type)        libsm64_gfx_put((pkt), GFXCMD_None, 0, 0, 0, 0)
#define gDPSetTextureFilter(pkt, type)    libsm64_gfx_put((pkt), GFXCMD_None, 0, 0, 0, 0)
#define gDPSetTexturePersp(pkt, type)     libsm64_gfx_put((pkt), GFXCMD_None, 0, 0, 0, 0)
#define gDPSetTextureLUT(pkt, type)       libsm64_gfx_put((pkt), GFXCMD_None, 0, 0, 0, 0)
#define gDPSetDepthSource(pkt, type)      libsm64_gfx_put((pkt), GFXCMD_None, 0, 0, 0, 0)
#define gSPFogPosition(pkt, min, max)     libsm64_gfx_put((pkt), GFXCMD_None, 0, 0, 0, 0)
#define gSPPerspNormalize(pkt, s)         libsm64_gfx_put((pkt), GFXCMD_None, 0, 0, 0, 0)
#define gSPClipRatio(pkt, r)              libsm64_gfx_put((pkt), GFXCMD_None, 0, 0, 0, 0)
#define gSPViewport(pkt, v)               ((void)0)
#define gSPMatrix(pkt, m, p)              ((void)0)
#define gSPPopMatrix(pkt, n)              ((void)0)
#define gDPFillRectangle(pkt, ulx, uly, lrx, lry) ((void)0)
#define gSPCullDisplayList(pkt, vstart, vend) ((void)0)
#define gSPSegment(pkt, segment, base)    ((void)0)
#define gDPSetScissor(pkt, mode, ulx, uly, lrx, lry) ((void)0)
#define gDPSetFillColor(pkt, d)           ((void)0)
#define gSPLookAt(pkt, l)                 ((void)0)
