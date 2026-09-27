#include <stdlib.h>
#include <string.h>

#include "libsm64.h"
#include "decomp/engine/math_util.h"
#include "decomp/engine/guMtxF2L.h"
#include "gfx_adapter.h"
#include "gfx_adapter_commands.h"
#include "load_tex_data.h"
#include "load_actor_tex.h"
#include "decomp/actors/actor_textures.h"

// ------------------------------------------------------------------ the RDP state the lists set as they go
static Mat4 s_curMatrix;
static float s_curColor[3];                      // the material colour: the directional light's, or white
static int s_lighting = 1;                       // G_LIGHTING: corners carry normals, else colours
static uint16_t s_scaleS = 0xFFFF, s_scaleT = 0xFFFF, s_uls, s_ult;
static int s_textureOn;
static intptr_t s_texImage;                      // what gsDPSetTextureImage named
static int s_texId = -1;                         // the objects' texture number (-1: none)
static int s_marioTile = -1;                     // Mario's atlas tile (-1: none)
static float s_texWidth = 32.0f, s_texHeight = 32.0f;
static int s_cms, s_cmt;                         // the wrap modes of the render tile
static uint8_t s_env[4] = { 255, 255, 255, 255 };
static uint8_t s_prim[4] = { 255, 255, 255, 255 };
static intptr_t s_combine;                       // cycle 1 of the colour combiner, packed
static int s_layer = 1;
static int s_object;

// the sinks
static int s_sink = GFX_SINK_MARIO;
static struct SM64MarioGeometryBuffers *s_outBuffers;
static float *s_trianglePtr, *s_colorPtr, *s_normalPtr, *s_uvPtr;
static struct SM64ObjectGeometryBuffers *s_objBuffers;

static void mtxf_mul_vec3f_x( Mat4 mtx, Vec3f b, float w, Vec3f out )
{
    out[0] = b[0] * mtx[0][0] + b[1] * mtx[1][0] + b[2] * mtx[2][0] + w * mtx[3][0];
    out[1] = b[0] * mtx[0][1] + b[1] * mtx[1][1] + b[2] * mtx[2][1] + w * mtx[3][1];
    out[2] = b[0] * mtx[0][2] + b[1] * mtx[1][2] + b[2] * mtx[2][2] + w * mtx[3][2];
}

// Mario's textures are numbered in one atlas of eleven 64-wide tiles (load_tex_data.h)
static void convert_uv_to_atlas( float *atlas_uv_out, const short tc[] )
{
    float u = (float)((tc[0] * s_scaleS >> 16) - 8 * s_uls) / 32.0f / s_texWidth;
    float v = (float)((tc[1] * s_scaleT >> 16) - 8 * s_ult) / 32.0f / s_texHeight;

    atlas_uv_out[0] = u * s_texWidth / 64.0f / (float)NUM_USED_TEXTURES + (float)s_marioTile / (float)NUM_USED_TEXTURES;
    atlas_uv_out[1] = v * s_texHeight / 64.0f;
}

// the objects' textures are separate pictures: coordinates in the picture, wrapping past its edges
static void convert_uv( float *uv_out, const short tc[] )
{
    uv_out[0] = (float)((tc[0] * s_scaleS >> 16) - 8 * s_uls) / 32.0f / s_texWidth;
    uv_out[1] = (float)((tc[1] * s_scaleT >> 16) - 8 * s_ult) / 32.0f / s_texHeight;
}

// the colour combiner's cycle 1: which inputs feed the colour and the alpha (gbi_libsm64.h packs them)
enum { CC_COMBINED = 0, CC_TEXEL0 = 1, CC_TEXEL1 = 2, CC_PRIMITIVE = 3, CC_SHADE = 4, CC_ENVIRONMENT = 5,
       CC_TEXEL0_ALPHA = 8, CC_TEXEL1_ALPHA = 9, CC_PRIMITIVE_ALPHA = 10, CC_SHADE_ALPHA = 11, CC_ENV_ALPHA = 12 };
enum { AC_COMBINED = 0, AC_TEXEL0 = 1, AC_TEXEL1 = 2, AC_PRIMITIVE = 3, AC_SHADE = 4, AC_ENVIRONMENT = 5 };

struct CombineUse { int tex, shade, env, prim, blend; int texA, shadeA, envA, primA; };

static void combine_use( intptr_t packed, struct CombineUse *u )
{
    int c[4], a[4], i;
    memset( u, 0, sizeof( *u ));
    for( i = 0; i < 4; ++i ) c[i] = (int)((packed >> (5 * i)) & 31);
    for( i = 0; i < 4; ++i ) a[i] = (int)((packed >> (20 + 3 * i)) & 7);
    for( i = 0; i < 4; ++i )
    {
        if( c[i] == CC_TEXEL0 || c[i] == CC_TEXEL1 || c[i] == CC_TEXEL0_ALPHA || c[i] == CC_TEXEL1_ALPHA ) u->tex = 1;
        if( c[i] == CC_SHADE || c[i] == CC_SHADE_ALPHA ) u->shade = 1;
        if( c[i] == CC_ENVIRONMENT || c[i] == CC_ENV_ALPHA ) u->env = 1;
        if( c[i] == CC_PRIMITIVE || c[i] == CC_PRIMITIVE_ALPHA ) u->prim = 1;
        if( a[i] == AC_TEXEL0 || a[i] == AC_TEXEL1 ) u->texA = 1;
        if( a[i] == AC_SHADE ) u->shadeA = 1;
        if( a[i] == AC_ENVIRONMENT ) u->envA = 1;
        if( a[i] == AC_PRIMITIVE ) u->primA = 1;
    }
    // (texel - shade) * texel alpha + shade: the texture laid over the colour where it has alpha (G_CC_BLENDRGBA)
    if( c[0] == CC_TEXEL0 && c[1] == CC_SHADE && c[2] == CC_TEXEL0_ALPHA && c[3] == CC_SHADE ) u->blend = 1;
}

static void set_texture_image( intptr_t image )
{
    s_texImage = image;
    s_texId = -1;
    s_marioTile = -1;
    if( image > -1000 && image < 1000 )          // one of Mario's (load_tex_data.h numbers them from 0)
    {
        if( image >= 0 && image < NUM_USED_TEXTURES )
        {
            s_marioTile = (int)image;
            s_texWidth = (float)mario_tex_widths[s_marioTile];
            s_texHeight = (float)mario_tex_heights[s_marioTile];
        }
        return;
    }
    {
        const struct SM64TexRef *ref = (const struct SM64TexRef *)image;
        int id = ref->id;
        if( id >= 0 && id < gActorTextureCount )
        {
            s_texId = id;
            s_texWidth = (float)gActorTextures[id].w;
            s_texHeight = (float)gActorTextures[id].h;
        }
    }
}

static void emit_triangle_mario( const Vtx *vdata, intptr_t v00, intptr_t v01, intptr_t v02 )
{
    if( s_outBuffers == NULL || s_outBuffers->numTrianglesUsed >= SM64_GEO_MAX_TRIANGLES ) return;
    const Vtx *v[3] = { &vdata[v00], &vdata[v01], &vdata[v02] };
    int i;
    for( i = 0; i < 3; ++i )
    {
        Vec3f p = { v[i]->v.ob[0], v[i]->v.ob[1], v[i]->v.ob[2] };
        Vec3f n = { (float)v[i]->n.n[0] / 128.0f, (float)v[i]->n.n[1] / 128.0f, (float)v[i]->n.n[2] / 128.0f };
        mtxf_mul_vec3f_x( s_curMatrix, p, 1.0f, s_trianglePtr );
        s_trianglePtr += 3;
        // TODO normals arent correct under non-uniform scale. multiply by inverse/transpose
        mtxf_mul_vec3f_x( s_curMatrix, n, 0.0f, s_normalPtr );
        vec3f_normalize( s_normalPtr );
        s_normalPtr += 3;
        *s_colorPtr++ = s_curColor[0];
        *s_colorPtr++ = s_curColor[1];
        *s_colorPtr++ = s_curColor[2];
        if( s_textureOn && s_marioTile >= 0 )
        {
            convert_uv_to_atlas( s_uvPtr, v[i]->v.tc );
            s_uvPtr += 2;
        }
        else
        {
            *s_uvPtr++ = 1.0f;
            *s_uvPtr++ = 1.0f;
        }
    }
    s_outBuffers->numTrianglesUsed = (uint16_t)((s_trianglePtr - s_outBuffers->position) / 9);
}

static void emit_triangle_object( const Vtx *vdata, intptr_t v00, intptr_t v01, intptr_t v02 )
{
    struct SM64ObjectGeometryBuffers *b = s_objBuffers;
    if( b == NULL || b->numTrianglesUsed >= b->capacity ) return;
    uint32_t t = b->numTrianglesUsed;
    const Vtx *v[3] = { &vdata[v00], &vdata[v01], &vdata[v02] };
    struct CombineUse use;
    combine_use( s_combine, &use );
    int textured = s_textureOn && s_texId >= 0 && ( use.tex || use.texA );
    float cr = 1.0f, cg = 1.0f, cb = 1.0f, ca = 1.0f;
    if( use.env ) { cr *= s_env[0] / 255.0f; cg *= s_env[1] / 255.0f; cb *= s_env[2] / 255.0f; }
    if( use.prim ) { cr *= s_prim[0] / 255.0f; cg *= s_prim[1] / 255.0f; cb *= s_prim[2] / 255.0f; }
    if( use.envA ) ca *= s_env[3] / 255.0f;
    if( use.primA ) ca *= s_prim[3] / 255.0f;
    int i;
    for( i = 0; i < 3; ++i )
    {
        Vec3f p = { v[i]->v.ob[0], v[i]->v.ob[1], v[i]->v.ob[2] };
        float *pos = b->position + ( t * 3 + i ) * 3;
        float *nrm = b->normal + ( t * 3 + i ) * 3;
        float *col = b->color + ( t * 3 + i ) * 4;
        float *uv = b->uv + ( t * 3 + i ) * 2;
        mtxf_mul_vec3f_x( s_curMatrix, p, 1.0f, pos );
        float sr = 1.0f, sg = 1.0f, sb = 1.0f, sa = 1.0f;
        if( s_lighting )
        {
            Vec3f n = { (float)v[i]->n.n[0] / 128.0f, (float)v[i]->n.n[1] / 128.0f, (float)v[i]->n.n[2] / 128.0f };
            mtxf_mul_vec3f_x( s_curMatrix, n, 0.0f, nrm );
            vec3f_normalize( nrm );
            sr = s_curColor[0]; sg = s_curColor[1]; sb = s_curColor[2];
            sa = v[i]->n.a / 255.0f;
        }
        else
        {
            nrm[0] = nrm[1] = nrm[2] = 0.0f;
            sr = v[i]->v.cn[0] / 255.0f; sg = v[i]->v.cn[1] / 255.0f; sb = v[i]->v.cn[2] / 255.0f;
            sa = v[i]->v.cn[3] / 255.0f;
        }
        if( use.shade || use.blend ) { col[0] = cr * sr; col[1] = cg * sg; col[2] = cb * sb; }
        else { col[0] = cr; col[1] = cg; col[2] = cb; }
        col[3] = use.shadeA ? ca * sa : ca;
        if( textured ) convert_uv( uv, v[i]->v.tc );
        else { uv[0] = 0.0f; uv[1] = 0.0f; }
    }
    b->texture[t] = textured ? (uint16_t)s_texId : 0xFFFF;
    uint8_t flags = 0;
    if( textured && use.tex ) flags |= SM64_TRI_TEXTURED;
    if( textured && use.texA ) flags |= SM64_TRI_TEX_ALPHA;
    if( textured && use.blend ) flags |= SM64_TRI_BLEND;
    if( s_lighting ) flags |= SM64_TRI_LIT;
    if( s_cms & 2 ) flags |= SM64_TRI_CLAMP_S;
    if( s_cmt & 2 ) flags |= SM64_TRI_CLAMP_T;
    if( s_cms & 1 ) flags |= SM64_TRI_MIRROR_S;
    if( s_cmt & 1 ) flags |= SM64_TRI_MIRROR_T;
    b->flags[t] = flags;
    b->layer[t] = (uint8_t)s_layer;
    b->object[t] = (uint16_t)s_object;
    b->numTrianglesUsed = t + 1;
}

static void process_display_list( const Gfx *ptr )
{
    const Vtx *vdata = NULL;

    for( ;; )
    {
        const intptr_t *w = ptr->w;
        switch( w[0] )
        {
            case GFXCMD_VertexData:
                vdata = (const Vtx *)w[1];
                break;

            case GFXCMD_Triangle:
                if( vdata == NULL ) break;
                if( s_sink == GFX_SINK_MARIO ) emit_triangle_mario( vdata, w[1], w[2], w[3] );
                else emit_triangle_object( vdata, w[1], w[2], w[3] );
                break;

            case GFXCMD_Light:
                if( w[2] == 1 )
                {
                    const Light *data = (const Light *)w[1];
                    s_curColor[0] = (float)data->l.col[0] / 255.0f;
                    s_curColor[1] = (float)data->l.col[1] / 255.0f;
                    s_curColor[2] = (float)data->l.col[2] / 255.0f;
                }
                break;

            case GFXCMD_Texture:
                s_scaleS = (uint16_t)w[1];
                s_scaleT = (uint16_t)w[2];
                s_textureOn = (int)w[3];
                break;

            case GFXCMD_SetTextureImage:
                set_texture_image( w[1] );
                break;

            case GFXCMD_SetTile:
                if( w[3] == 0 )                  // the render tile: its wrap modes are what the picture repeats by
                {
                    s_cms = (int)w[4];
                    s_cmt = (int)w[5];
                }
                break;

            case GFXCMD_SetTileSize:
                s_uls = (uint16_t)w[1];
                s_ult = (uint16_t)w[2];
                break;

            case GFXCMD_SetGeometryMode:
                if( w[1] & 0x00020000 ) s_lighting = 1;    // G_LIGHTING
                break;

            case GFXCMD_ClearGeometryMode:
                if( w[1] & 0x00020000 ) s_lighting = 0;
                break;

            case GFXCMD_SetEnvColor:
                s_env[0] = (uint8_t)w[1]; s_env[1] = (uint8_t)w[2]; s_env[2] = (uint8_t)w[3]; s_env[3] = (uint8_t)w[4];
                break;

            case GFXCMD_SetPrimColor:
                s_prim[0] = (uint8_t)w[1]; s_prim[1] = (uint8_t)w[2]; s_prim[2] = (uint8_t)w[3]; s_prim[3] = (uint8_t)w[4];
                break;

            case GFXCMD_SetCombineMode:
                s_combine = w[1];
                break;

            case GFXCMD_SubDisplayList:
                process_display_list( (const Gfx *)w[1] );
                break;

            case GFXCMD_BranchList:
                ptr = (const Gfx *)w[1];
                continue;

            case GFXCMD_EndDisplayList:
                return;

            default:
                break;
        }
        ++ptr;
    }
}

void gfx_adapter_run( Mtx *transform, const void *displayList )
{
    if( transform != NULL ) guMtxL2F( s_curMatrix, transform );
    if( displayList != NULL ) process_display_list( (const Gfx *)displayList );
}

void gfx_adapter_bind_output_buffers( struct SM64MarioGeometryBuffers *outBuffers )
{
    s_outBuffers = outBuffers;
    if( outBuffers == NULL ) return;
    s_trianglePtr = s_outBuffers->position;
    s_colorPtr = s_outBuffers->color;
    s_normalPtr = s_outBuffers->normal;
    s_uvPtr = s_outBuffers->uv;
    s_outBuffers->numTrianglesUsed = 0;
}

void gfx_adapter_bind_object_buffers( struct SM64ObjectGeometryBuffers *outBuffers )
{
    s_objBuffers = outBuffers;
    if( outBuffers != NULL ) outBuffers->numTrianglesUsed = 0;
}

void gfx_adapter_select_sink( int sink ) { s_sink = sink; }
int gfx_adapter_current_sink( void ) { return s_sink; }

void gfx_adapter_frame_begin( void )
{
    s_lighting = 1;
    s_textureOn = 0;
    s_texId = -1;
    s_marioTile = -1;
    s_scaleS = s_scaleT = 0xFFFF;
    s_uls = s_ult = 0;
    s_cms = s_cmt = 0;
    s_env[0] = s_env[1] = s_env[2] = s_env[3] = 255;
    s_prim[0] = s_prim[1] = s_prim[2] = s_prim[3] = 255;
    // TEXEL0 * SHADE with the texture's alpha, the most common material, until a list says otherwise
    s_combine = (intptr_t)( CC_TEXEL0 | ( 31 << 5 ) | ( CC_SHADE << 10 ) | ( 31 << 15 ) |
                            ( AC_TEXEL0 << 20 ) | ( 7 << 23 ) | ( AC_SHADE << 26 ) | ( 7 << 29 ));
    s_curColor[0] = s_curColor[1] = s_curColor[2] = 1.0f;
    s_layer = 1;
    s_object = 0;
    mtxf_identity( s_curMatrix );
}

void gfx_adapter_set_layer( int layer ) { s_layer = layer; }
void gfx_adapter_set_object( int index ) { s_object = index; }
