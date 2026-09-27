#pragma once

#include "decomp/include/types.h"
#include "libsm64.h"

// The object engine as the library drives it: SM64's pool of objects, each running its own behaviour script,
// with Mario one of them (object_list_processor.c, spawn_object.c, engine/behavior_script.c).  The models are
// the actors' geo layouts, bound to the model ids the level scripts use (decomp/actors/model_table.c); what the
// spawn menu offers is decomp/actors/menu_table.c.

extern void objects_models_init( struct GraphNode *marioGraphNode );   // once: the models, from their layouts
extern void objects_level_init( void );                                // an empty pool, before Mario is made
extern struct Object *objects_spawn_mario( float x, float y, float z, struct GraphNode *marioGraphNode );
extern void objects_tick( void );                                      // one frame of every object, Mario included
extern int32_t objects_spawn_entry( int32_t entry, float x, float y, float z, int16_t yaw );
extern void objects_clear_spawned( void );                             // everything but Mario
extern void objects_shift( float dx, float dy, float dz );             // the level's origin moved under them
extern int32_t objects_active_count( void );
extern int32_t objects_forget_platform( struct Object *platform );     // a host surface object is going away
extern int32_t objects_threats( float *out, int32_t max );            // where the enemies and bosses are
extern void objects_set_prey( const float *pos, int32_t n );          // where the level's humans are
extern int32_t objects_prey_hits( int32_t *out, int32_t max );        // which of them an enemy touched
