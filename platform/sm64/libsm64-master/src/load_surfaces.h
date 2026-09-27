#pragma once

#include "decomp/include/types.h"
#include "libsm64.h"

// The surfaces Mario and the objects collide with, in three groups the collision code walks (surface_collision.c):
//  0            the level, given whole by the host (sm64_static_surfaces_load);
//  1 .. n       the host's moving objects (sm64_surface_object_create), each with a stand-in Object so that what
//               stands on one is carried the way SM64 carries what stands on a platform;
//  n + 1        the collision SM64's own objects carry (a Thwomp, a Whomp), rebuilt every frame by the object
//               engine (engine/surface_load.c).

extern uint32_t loaded_surface_iter_group_count( void );
extern uint32_t loaded_surface_iter_group_size( uint32_t groupIndex );
extern struct SM64SurfaceCollisionData *loaded_surface_iter_get_at_index( uint32_t groupIndex, uint32_t surfaceIndex );

extern void surfaces_load_static( const struct SM64Surface *surfaceArray, uint32_t numSurfaces );
extern uint32_t surfaces_load_object( const struct SM64SurfaceObject *surfaceObject );
extern void surface_object_update_transform( uint32_t objId, const struct SM64ObjectTransform *newTransform );
extern struct SM64SurfaceObjectTransform *surfaces_object_get_transform_ptr( uint32_t objId );
extern struct Object *surfaces_object_get_object( uint32_t objId );
extern void surfaces_unload_object( uint32_t objId );
extern void surfaces_unload_all( void );

extern void surfaces_dynamic_clear( void );
extern void surfaces_dynamic_add( const struct SM64Surface *libSurf, struct Object *owner, int8_t room );
