#include "load_actor_tex.h"

#include <stdlib.h>
#include <string.h>

#include "decomp/tools/libmio0.h"
#include "decomp/tools/n64graphics.h"
#include "decomp/actors/actor_textures.h"
#include "debug_print.h"

static uint8_t **s_pixels;                       // RGBA, w * h * 4 each, or NULL when the ROM had none
static int s_count;

// the MIO0 blocks of the ROM the pictures lie in, each decoded once
struct Block { uint32_t start; uint8_t *data; uint32_t size; };
static struct Block s_blocks[64];
static int s_blockCount;

static struct Block *block_for( const uint8_t *rom, uint32_t start )
{
    int i;
    for( i = 0; i < s_blockCount; ++i )
        if( s_blocks[i].start == start ) return &s_blocks[i];
    if( s_blockCount == 64 ) return NULL;
    if( memcmp( rom + start, "MIO0", 4 ) != 0 )
    {
        DEBUG_PRINT( "no MIO0 block at ROM offset %u: its pictures are left blank", start );
        return NULL;
    }
    mio0_header_t head;
    mio0_decode_header( rom + start, &head );
    struct Block *b = &s_blocks[s_blockCount++];
    b->start = start;
    b->size = head.dest_size;
    b->data = malloc( head.dest_size );
    mio0_decode( rom + start, b->data, NULL );
    return b;
}

void load_actor_textures_from_rom( const uint8_t *rom )
{
    int i;
    unload_actor_textures();
    s_count = gActorTextureCount;
    s_pixels = calloc( (size_t)s_count, sizeof( uint8_t * ));
    for( i = 0; i < s_count; ++i )
    {
        const struct SM64ActorTexture *t = &gActorTextures[i];
        struct Block *b = block_for( rom, t->mio0 );
        if( b == NULL ) continue;
        uint32_t bytes = (uint32_t)t->w * t->h * t->bits / 8;
        if( t->offset + bytes > b->size ) continue;
        const uint8_t *raw = b->data + t->offset;
        uint8_t *out = malloc( (size_t)t->w * t->h * 4 );
        int n = t->w * t->h, p;
        if( t->fmt == 0 )                        // RGBA
        {
            rgba *img = raw2rgba( raw, t->w, t->h, t->bits );
            for( p = 0; p < n; ++p ) { out[4*p] = img[p].red; out[4*p+1] = img[p].green; out[4*p+2] = img[p].blue; out[4*p+3] = img[p].alpha; }
            free( img );
        }
        else if( t->fmt == 1 )                   // IA: a grey with its own alpha
        {
            ia *img = raw2ia( raw, t->w, t->h, t->bits );
            for( p = 0; p < n; ++p ) { out[4*p] = out[4*p+1] = out[4*p+2] = img[p].intensity; out[4*p+3] = img[p].alpha; }
            free( img );
        }
        else if( t->fmt == 2 )                   // I: the grey is the alpha as well
        {
            ia *img = raw2i( raw, t->w, t->h, t->bits );
            for( p = 0; p < n; ++p ) { out[4*p] = out[4*p+1] = out[4*p+2] = img[p].intensity; out[4*p+3] = img[p].alpha; }
            free( img );
        }
        else
        {
            free( out );
            continue;
        }
        s_pixels[i] = out;
    }
    for( i = 0; i < s_blockCount; ++i ) free( s_blocks[i].data );
    s_blockCount = 0;
}

void unload_actor_textures( void )
{
    int i;
    if( s_pixels != NULL )
    {
        for( i = 0; i < s_count; ++i ) free( s_pixels[i] );
        free( s_pixels );
    }
    s_pixels = NULL;
    s_count = 0;
}

int actor_texture_count( void ) { return gActorTextureCount; }

int actor_texture_size( int index, int *w, int *h )
{
    if( index < 0 || index >= gActorTextureCount ) return 0;
    if( w ) *w = gActorTextures[index].w;
    if( h ) *h = gActorTextures[index].h;
    return 1;
}

const uint8_t *actor_texture_rgba( int index )
{
    if( s_pixels == NULL || index < 0 || index >= s_count ) return NULL;
    return s_pixels[index];
}
