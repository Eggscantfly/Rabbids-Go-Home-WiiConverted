#pragma once

#include <stdint.h>

// The actors' pictures, read out of the player's ROM at the places assets.json gives (decomp/actors/actor_textures.c,
// written by import-objects.py) and decoded to RGBA, one buffer each, the way load_tex_data.c reads Mario's.
extern void load_actor_textures_from_rom( const uint8_t *rom );
extern void unload_actor_textures( void );
extern int actor_texture_count( void );
extern int actor_texture_size( int index, int *w, int *h );
extern const uint8_t *actor_texture_rgba( int index );
