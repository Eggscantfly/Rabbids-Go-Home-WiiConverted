// surface_load.c - the collision an object carries (a Thwomp, a Whomp, a platform), placed each frame where the
// object is.  SM64 loads them into its dynamic spatial partition; libsm64 has a plain list of surfaces instead
// (load_surfaces.c), so this transforms the object's collision data the way the game does and appends the result
// to that list's dynamic part, which the object engine clears at the top of each frame.
#include <string.h>

#include "sm64.h"
#include "surface_terrains.h"
#include "engine/math_util.h"
#include "engine/surface_collision.h"
#include "engine/surface_load.h"
#include "game/object_list_processor.h"
#include "game/object_helpers.h"
#include "behavior_data.h"
#include "libsm64_objects_glue.h"
#include "../../load_surfaces.h"

s32 unused8038BE90;

void alloc_surface_pools(void) {}
u32 get_area_terrain_size(TerrainData *data) { (void)data; return 0; }
void load_area_terrain(s16 index, TerrainData *data, RoomData *surfaceRooms, s16 *macroObjects) { (void)index; (void)data; (void)surfaceRooms; (void)macroObjects; }

/**
 * If not in time stop, clear the dynamic surfaces.
 */
void clear_dynamic_surfaces(void) {
    if (!(gTimeStopState & TIME_STOP_ACTIVE)) {
        surfaces_dynamic_clear();
    }
}

/**
 * Returns whether a surface has exertion/moves Mario based on the surface type.
 */
static s32 surface_has_force(TerrainData surfaceType) {
    switch (surfaceType) {
        case SURFACE_0004:
        case SURFACE_FLOWING_WATER:
        case SURFACE_DEEP_MOVING_QUICKSAND:
        case SURFACE_SHALLOW_MOVING_QUICKSAND:
        case SURFACE_MOVING_QUICKSAND:
        case SURFACE_HORIZONTAL_WIND:
        case SURFACE_INSTANT_MOVING_QUICKSAND:
            return TRUE;
        default:
            return FALSE;
    }
}

/**
 * Applies an object's transformation to the object's vertices.  The transformed vertices are kept in 32 bits: the
 * level this library holds is not bounded the way SM64's are.
 */
static void transform_object_vertices(TerrainData **data, s32 *vertexData) {
    register TerrainData *vertices;
    register f32 vx, vy, vz;
    register s32 numVertices;

    Mat4 *objectTransform;
    Mat4 m;

    objectTransform = &gCurrentObject->transform;

    numVertices = *(*data);
    (*data)++;

    vertices = *data;

    if (gCurrentObject->header.gfx.throwMatrix == NULL) {
        gCurrentObject->header.gfx.throwMatrix = objectTransform;
        obj_build_transform_from_pos_and_angle(gCurrentObject, O_POS_INDEX, O_FACE_ANGLE_INDEX);
    }

    obj_apply_scale_to_matrix(gCurrentObject, m, *objectTransform);

    // Go through all vertices, rotating and translating them to transform the object.
    while (numVertices--) {
        vx = *(vertices++);
        vy = *(vertices++);
        vz = *(vertices++);

        //! No bounds check on vertex data
        *vertexData++ = (s32)(vx * m[0][0] + vy * m[1][0] + vz * m[2][0] + m[3][0]);
        *vertexData++ = (s32)(vx * m[0][1] + vy * m[1][1] + vz * m[2][1] + m[3][1]);
        *vertexData++ = (s32)(vx * m[0][2] + vy * m[1][2] + vz * m[2][2] + m[3][2]);
    }

    *data = vertices;
}

/**
 * Load in the surfaces for the gCurrentObject. This includes setting the flags, exertion, and room.
 */
static void load_object_surfaces(TerrainData **data, s32 *vertexData) {
    s32 surfaceType;
    s32 i;
    s32 numSurfaces;
    s16 hasForce;
    s16 room;

    surfaceType = *(*data);
    (*data)++;

    numSurfaces = *(*data);
    (*data)++;

    hasForce = surface_has_force(surfaceType);

    // The DDD warp is initially loaded at the origin and moved to the proper position in paintings.c and doesn't
    // update its room, so set it here.
    if (gCurrentObject->behavior == segmented_to_virtual(bhvDDDWarp)) {
        room = 5;
    } else {
        room = 0;
    }

    for (i = 0; i < numSurfaces; i++) {
        struct SM64Surface lib;
        s32 i1 = *(*data + 0), i2 = *(*data + 1), i3 = *(*data + 2);
        lib.type = (s16) surfaceType;
        lib.force = hasForce ? *(*data + 3) : 0;
        lib.terrain = 0;
        lib.vertices[0][0] = vertexData[3 * i1 + 0]; lib.vertices[0][1] = vertexData[3 * i1 + 1]; lib.vertices[0][2] = vertexData[3 * i1 + 2];
        lib.vertices[1][0] = vertexData[3 * i2 + 0]; lib.vertices[1][1] = vertexData[3 * i2 + 1]; lib.vertices[1][2] = vertexData[3 * i2 + 2];
        lib.vertices[2][0] = vertexData[3 * i3 + 0]; lib.vertices[2][1] = vertexData[3 * i3 + 1]; lib.vertices[2][2] = vertexData[3 * i3 + 2];
        surfaces_dynamic_add(&lib, gCurrentObject, (s8) room);

        if (hasForce) {
            *data += 4;
        } else {
            *data += 3;
        }
    }
}

/**
 * Transform an object's vertices, reload them, and render the object.
 */
void load_object_collision_model(void) {
    s32 vertexData[600];

    TerrainData *collisionData = gCurrentObject->collisionData;
    f32 marioDist = gCurrentObject->oDistanceToMario;
    f32 tangibleDist = gCurrentObject->oCollisionDistance;

    if (collisionData == NULL) {
        return;
    }

    // On an object's first frame, the distance is set to 19000.0f.
    // If the distance hasn't been updated, update it now.
    if (gCurrentObject->oDistanceToMario == 19000.0f) {
        marioDist = dist_between_objects(gCurrentObject, gMarioObject);
    }

    // If the object collision is supposed to be loaded more than the
    // drawing distance of 4000, extend the drawing range.
    if (gCurrentObject->oCollisionDistance > 4000.0f) {
        gCurrentObject->oDrawingDistance = gCurrentObject->oCollisionDistance;
    }

    // Update if no Time Stop, in range, and in the current room.
    if (!(gTimeStopState & TIME_STOP_ACTIVE) && marioDist < tangibleDist
        && !(gCurrentObject->activeFlags & ACTIVE_FLAG_IN_DIFFERENT_ROOM)) {
        collisionData++;
        transform_object_vertices(&collisionData, vertexData);

        // TERRAIN_LOAD_CONTINUE acts as an "end" to the terrain data.
        while (*collisionData != TERRAIN_LOAD_CONTINUE) {
            load_object_surfaces(&collisionData, vertexData);
        }
    }

    if (marioDist < gCurrentObject->oDrawingDistance) {
        gCurrentObject->header.gfx.node.flags |= GRAPH_RENDER_ACTIVE;
    } else {
        gCurrentObject->header.gfx.node.flags &= ~GRAPH_RENDER_ACTIVE;
    }
}
