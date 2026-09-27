#include "objects.h"

#include <stdlib.h>
#include <string.h>

#include "decomp/include/sm64.h"
#include "decomp/include/behavior_data.h"
#include "decomp/include/model_ids.h"
#include "decomp/include/object_constants.h"
#include "decomp/include/object_fields.h"
#include "decomp/include/course_table.h"
#include "decomp/include/level_table.h"
#include "decomp/game/object_list_processor.h"
#include "decomp/game/spawn_object.h"
#include "decomp/game/object_helpers.h"
#include "decomp/game/interaction.h"
#include "decomp/game/mario.h"
#include "decomp/game/area.h"
#include "decomp/game/camera.h"
#include "decomp/game/game_init.h"
#include "decomp/engine/geo_layout.h"
#include "decomp/engine/graph_node.h"
#include "decomp/engine/math_util.h"
#include "decomp/engine/surface_collision.h"
#include "decomp/actors/model_table.h"
#include "decomp/actors/menu_table.h"
#include "decomp/libsm64_objects_glue.h"
#include "decomp/memory.h"
#include "debug_print.h"

// ------------------------------------------------------------------ the models
// SM64 numbers its models per level (0x56 is the Bully in one level and King Bob-omb in another), so there is a
// table per level, each the common bindings overlaid with that level's own; an object looks its models up in the
// table of the level it believes it is in, which is set before its behaviour runs.
#define LEVEL_TABLES 64
static struct AllocOnlyPool *s_model_pool;
static int s_models_ready;
static struct GraphNode *s_default_table[LIBSM64_MODEL_SLOTS];
static struct GraphNode **s_level_tables[LEVEL_TABLES];   // NULL: the default table
static struct GraphNode *s_mario_node;

// each geo layout or display list is made into a graph once, however many ids it is bound to
struct GeoNodeCache { const void *data; struct GraphNode *node; };
static struct GeoNodeCache *s_cache;
static int s_cacheCount, s_cacheCap;

static struct GraphNode *node_for_geo( const GeoLayout *geo )
{
    for( int i = 0; i < s_cacheCount; ++i ) if( s_cache[i].data == geo ) return s_cache[i].node;
    struct GraphNode *node = process_geo_layout( s_model_pool, (void *)geo );
    if( s_cacheCount == s_cacheCap ) { s_cacheCap = s_cacheCap ? s_cacheCap * 2 : 256; s_cache = realloc( s_cache, s_cacheCap * sizeof( *s_cache )); }
    s_cache[s_cacheCount].data = geo;
    s_cache[s_cacheCount].node = node;
    ++s_cacheCount;
    return node;
}

static struct GraphNode *node_for_dl( const Gfx *dl, int layer )
{
    for( int i = 0; i < s_cacheCount; ++i ) if( s_cache[i].data == dl ) return s_cache[i].node;
    struct GraphNode *node = (struct GraphNode *)init_graph_node_display_list( s_model_pool, NULL, layer, (void *)dl );
    if( s_cacheCount == s_cacheCap ) { s_cacheCap = s_cacheCap ? s_cacheCap * 2 : 256; s_cache = realloc( s_cache, s_cacheCap * sizeof( *s_cache )); }
    s_cache[s_cacheCount].data = dl;
    s_cache[s_cacheCount].node = node;
    ++s_cacheCount;
    return node;
}

static struct GraphNode *binding_node( const struct SM64ModelBinding *b )
{
    if( b->geo != NULL ) return node_for_geo( b->geo );
    if( b->dl != NULL ) return node_for_dl( b->dl, b->layer );
    return NULL;
}

struct GraphNode **libsm64_model_table_for( int32_t level )
{
    if( level > 0 && level < LEVEL_TABLES && s_level_tables[level] != NULL ) return s_level_tables[level];
    return s_default_table;
}

void objects_models_init( struct GraphNode *marioGraphNode )
{
    const struct SM64ModelBinding *b;
    s_mario_node = marioGraphNode;
    if( !s_models_ready )
    {
        s_model_pool = alloc_only_pool_init();
        memset( s_default_table, 0, sizeof( s_default_table ));
        // the common scripts' bindings, and the first binding of any id no common script binds
        for( b = gModelBindings; b->model >= 0; ++b )
            if( b->level == 0 && b->model < LIBSM64_MODEL_SLOTS ) s_default_table[b->model] = binding_node( b );
        for( b = gModelBindings; b->model >= 0; ++b )
            if( b->level != 0 && b->model < LIBSM64_MODEL_SLOTS && s_default_table[b->model] == NULL ) s_default_table[b->model] = binding_node( b );
        // then each level's own, over the common ones
        for( b = gModelBindings; b->model >= 0; ++b )
        {
            if( b->level <= 0 || b->level >= LEVEL_TABLES || b->model >= LIBSM64_MODEL_SLOTS ) continue;
            if( s_level_tables[b->level] == NULL )
            {
                s_level_tables[b->level] = malloc( sizeof( s_default_table ));
                memcpy( s_level_tables[b->level], s_default_table, sizeof( s_default_table ));
            }
            s_level_tables[b->level][b->model] = binding_node( b );
        }
        s_models_ready = 1;
    }
    s_default_table[MODEL_MARIO] = marioGraphNode;
    for( int i = 0; i < LEVEL_TABLES; ++i ) if( s_level_tables[i] != NULL ) s_level_tables[i][MODEL_MARIO] = marioGraphNode;
    gLoadedGraphNodes = s_default_table;
}

// ---------------------------------------------------------------- the level's people as prey
// The host hands over where the game's humans stand (their own reflex track reports them).  While an enemy's
// behaviour runs, Mario is moved to the nearest of them when one is nearer than he is - every behaviour reads
// "where Mario is" (oDistanceToMario, oAngleToMario, gMarioObject, gMarioState->pos), so it hunts that human
// exactly as it would hunt him - and put back the moment it is done.  Collisions with Mario are worked out before
// the behaviours run, from where he really is.  An enemy that ends its step touching its human has hit it.
#define PREY_MAX 16
static float s_prey[PREY_MAX][3];
static int32_t s_preyCount;
static u8 s_preyHit[PREY_MAX];
static struct Object *s_swapObj;
static int32_t s_swapPrey;
static Vec3f s_swapObjPos, s_swapMarioPos;

void objects_set_prey( const float *pos, int32_t n )
{
    if( n < 0 ) n = 0;
    if( n > PREY_MAX ) n = PREY_MAX;
    for( int32_t i = 0; i < n; ++i ) { s_prey[i][0] = pos[i * 3]; s_prey[i][1] = pos[i * 3 + 1]; s_prey[i][2] = pos[i * 3 + 2]; }
    s_preyCount = n;
    memset( s_preyHit, 0, sizeof( s_preyHit ));
}

int32_t objects_prey_hits( int32_t *out, int32_t max )
{
    int32_t n = 0;
    for( int32_t i = 0; i < s_preyCount && n < max; ++i )
        if( s_preyHit[i] ) { out[n++] = i; s_preyHit[i] = 0; }
    return n;
}

static int object_is_enemy( const struct Object *obj )
{
    static const BehaviorScript *lastBhv;
    static int lastIs;
    if( obj->behavior == lastBhv ) return lastIs;
    lastBhv = obj->behavior;
    lastIs = 0;
    for( int32_t e = 0; e < gMenuEntryCount; ++e )
        if( gMenuEntries[e].behavior == obj->behavior )
        {
            lastIs = gMenuEntries[e].category[0] == 'E' || gMenuEntries[e].category[0] == 'B';
            break;
        }
    return lastIs;
}

static void prey_swap_in( struct Object *obj )
{
    s_swapObj = NULL;
    if( s_preyCount == 0 || gMarioObject == NULL || obj == gMarioObject ) return;
    if( obj->oHeldState != HELD_FREE || gMarioState->heldObj == obj || gMarioState->riddenObj == obj ) return;
    if( gMarioState->action == ACT_GRABBED || ( obj->oInteractStatus & INT_STATUS_GRABBED_MARIO )) return;
    if( !object_is_enemy( obj )) return;
    float dx = obj->oPosX - gMarioObject->oPosX, dy = obj->oPosY - gMarioObject->oPosY, dz = obj->oPosZ - gMarioObject->oPosZ;
    float best = dx * dx + dy * dy + dz * dz;
    int32_t which = -1;
    for( int32_t i = 0; i < s_preyCount; ++i )
    {
        dx = obj->oPosX - s_prey[i][0]; dy = obj->oPosY - s_prey[i][1]; dz = obj->oPosZ - s_prey[i][2];
        float d = dx * dx + dy * dy + dz * dz;
        if( d < best && d < 4000.0f * 4000.0f ) { best = d; which = i; }
    }
    if( which < 0 ) return;
    s_swapObjPos[0] = gMarioObject->oPosX; s_swapObjPos[1] = gMarioObject->oPosY; s_swapObjPos[2] = gMarioObject->oPosZ;
    vec3f_copy( s_swapMarioPos, gMarioState->pos );
    gMarioObject->oPosX = s_prey[which][0]; gMarioObject->oPosY = s_prey[which][1]; gMarioObject->oPosZ = s_prey[which][2];
    gMarioState->pos[0] = s_prey[which][0]; gMarioState->pos[1] = s_prey[which][1]; gMarioState->pos[2] = s_prey[which][2];
    s_swapObj = obj;
    s_swapPrey = which;
}

void libsm64_after_object_update( struct Object *obj )
{
    if( s_swapObj == NULL || s_swapObj != obj ) return;
    const float *p = s_prey[s_swapPrey];
    float dx = obj->oPosX - p[0], dz = obj->oPosZ - p[2], dy = obj->oPosY - p[1];
    float reach = obj->hitboxRadius + 70.0f;
    if( obj->oInteractType != 0 && dx * dx + dz * dz < reach * reach && dy > -( obj->hitboxHeight + 80.0f ) && dy < 220.0f )
        s_preyHit[s_swapPrey] = 1;
    gMarioObject->oPosX = s_swapObjPos[0]; gMarioObject->oPosY = s_swapObjPos[1]; gMarioObject->oPosZ = s_swapObjPos[2];
    vec3f_copy( gMarioState->pos, s_swapMarioPos );
    s_swapObj = NULL;
}

static const char *object_name( const struct Object *obj )
{
    if( obj == gMarioObject ) return "MARIO";
    for( int32_t i = 0; i < gMenuEntryCount; ++i )
        if( gMenuEntries[i].behavior == obj->behavior ) return gMenuEntries[i].name;
    return "(a helper object)";
}

void libsm64_before_object_update( struct Object *obj )
{
    if( gLibsm64DebugFlags & LIBSM64_DEBUG_HEAP_OBJECTS )
    {
        static char s_last[64] = "nothing yet";
        char where[192];
        snprintf( where, sizeof( where ), "before object %d (%s), the last one updated being %s", (int)( obj - gObjectPool ), object_name( obj ), s_last );
        libsm64_heap_check( where );
        snprintf( s_last, sizeof( s_last ), "%d (%s)", (int)( obj - gObjectPool ), object_name( obj ));
    }
    u32 level = LIBSM64_OBJECT_LEVEL( obj );
    gCurrLevelNum = ( level != 0 ) ? (s16) level : LIBSM64_DEFAULT_LEVEL;
    gLoadedGraphNodes = libsm64_model_table_for( (int32_t) level );
    prey_swap_in( obj );
}

// ------------------------------------------------------------------ the pool
static struct SpawnInfo s_spawn_infos[OBJECT_POOL_CAPACITY];   // one per pool slot: an object's respawnInfo points into it

void objects_level_init( void )
{
    clear_objects();
    gTimeStopState = 0;
    gCurrAreaIndex = 0;
    gCurrCourseNum = COURSE_BOB;
    gCurrLevelNum = LIBSM64_DEFAULT_LEVEL;
    gLoadedGraphNodes = s_default_table;
    memset( s_spawn_infos, 0, sizeof( s_spawn_infos ));
}

struct Object *objects_spawn_mario( float x, float y, float z, struct GraphNode *marioGraphNode )
{
    struct Object *obj = create_object( bhvMario );
    if( obj == NULL ) return NULL;

    struct SpawnInfo *info = gMarioSpawnInfo;
    info->startPos[0] = x;
    info->startPos[1] = y;
    info->startPos[2] = z;
    info->startAngle[0] = 0;
    info->startAngle[1] = 0;
    info->startAngle[2] = 0;
    info->areaIndex = 0;
    info->activeAreaIndex = 0;
    info->behaviorArg = 1;                       // the level scripts' mark of the player
    info->behaviorScript = (void *)bhvMario;
    info->model = marioGraphNode;
    info->next = NULL;

    obj->oBhvParams = 1;
    obj->oBhvParams2ndByte = 0;
    obj->behavior = bhvMario;
    obj->unused1 = 0;
    obj->respawnInfoType = RESPAWN_INFO_TYPE_32;
    obj->respawnInfo = &info->behaviorArg;

    gMarioObject = obj;
    geo_make_first_child( &obj->header.gfx.node );
    geo_obj_init_spawninfo( &obj->header.gfx, info );

    obj->oPosX = x;
    obj->oPosY = y;
    obj->oPosZ = z;
    obj->oFaceAnglePitch = obj->oFaceAngleYaw = obj->oFaceAngleRoll = 0;
    obj->oMoveAnglePitch = obj->oMoveAngleYaw = obj->oMoveAngleRoll = 0;
    return obj;
}

void objects_tick( void )
{
    gPlayer1Controller = &gController;
    gPlayer2Controller = &gController;
    gPlayer3Controller = &gController;
    if( gCurrentArea != NULL ) gCamera = gCurrentArea->camera;
    gCurrLevelNum = LIBSM64_DEFAULT_LEVEL;
    gLoadedGraphNodes = s_default_table;
    libsm64_dialog_tick();
    update_objects( 0 );
    gCurrLevelNum = LIBSM64_DEFAULT_LEVEL;
    gLoadedGraphNodes = s_default_table;
    gGlobalTimer++;
}

static int32_t spawn_one( const struct SM64MenuEntry *e, float x, float y, float z, int16_t yaw )
{
    gCurrentObject = gMarioObject;               // whatever the behaviour's first commands look at is Mario's
    gCurrLevelNum = e->level ? (s16)e->level : LIBSM64_DEFAULT_LEVEL;
    gLoadedGraphNodes = libsm64_model_table_for( e->level );
    struct Object *obj = create_object( e->behavior );
    if( obj == NULL ) return -1;
    int32_t slot = (int32_t)( obj - gObjectPool );
    if( slot < 0 || slot >= OBJECT_POOL_CAPACITY ) return -1;

    struct SpawnInfo *info = &s_spawn_infos[slot];
    memset( info, 0, sizeof( *info ));
    info->startPos[0] = x;
    info->startPos[1] = y;
    info->startPos[2] = z;
    info->startAngle[0] = 0;
    info->startAngle[1] = yaw;
    info->startAngle[2] = 0;
    info->areaIndex = 0;
    info->activeAreaIndex = 0;
    info->behaviorArg = e->bhvParams;
    info->behaviorScript = (void *)e->behavior;
    if( e->geo != NULL ) info->model = node_for_geo( e->geo );
    else if( e->model > 0 && e->model < LIBSM64_MODEL_SLOTS ) info->model = gLoadedGraphNodes[e->model];   // a display list: the level's table has it
    else info->model = NULL;
    info->next = NULL;

    obj->oBhvParams = e->bhvParams;
    obj->oBhvParams2ndByte = ( e->bhvParams >> 16 ) & 0xFF;
    obj->behavior = e->behavior;
    obj->unused1 = (u32)e->level;
    obj->respawnInfoType = RESPAWN_INFO_TYPE_32;
    obj->respawnInfo = &info->behaviorArg;

    geo_obj_init_spawninfo( &obj->header.gfx, info );

    obj->oPosX = x;
    obj->oPosY = y;
    obj->oPosZ = z;
    obj->oFaceAnglePitch = 0;
    obj->oFaceAngleYaw = yaw;
    obj->oFaceAngleRoll = 0;
    obj->oMoveAnglePitch = 0;
    obj->oMoveAngleYaw = yaw;
    obj->oMoveAngleRoll = 0;
    return slot;
}

int32_t objects_spawn_entry( int32_t entry, float x, float y, float z, int16_t yaw )
{
    if( entry < 0 || entry >= gMenuEntryCount ) return -1;
    if( gMarioObject == NULL ) return -1;
    const struct SM64MenuEntry *e = &gMenuEntries[entry];
    if( e->companion >= 0 && e->companion < gMenuEntryCount && e->companion != entry )
        spawn_one( &gMenuEntries[e->companion], x, y, z, yaw );
    int32_t slot = spawn_one( e, x, y, z, yaw );
    gCurrLevelNum = LIBSM64_DEFAULT_LEVEL;
    gLoadedGraphNodes = s_default_table;
    return slot;
}

void objects_clear_spawned( void )
{
    int32_t i;
    if( gMarioObject != NULL )
    {
        struct MarioState *m = gMarioState;
        if( m->heldObj != NULL || m->riddenObj != NULL )
        {
            mario_stop_riding_and_holding( m );
            m->heldObj = NULL;
            m->riddenObj = NULL;
        }
        m->usedObj = NULL;
        m->interactObj = NULL;
        gMarioObject->platform = NULL;
    }
    for( i = 0; i < OBJECT_POOL_CAPACITY; ++i )
    {
        struct Object *obj = &gObjectPool[i];
        if( !( obj->activeFlags & ACTIVE_FLAG_ACTIVE )) continue;
        if( obj == gMarioObject ) continue;
        unload_object( obj );
    }
    // whatever held the world still (a boss's entrance, a star coming up) is gone with them
    gTimeStopState = 0;
}

static void shift_object( struct Object *obj, float dx, float dy, float dz )
{
    obj->oPosX += dx; obj->oPosY += dy; obj->oPosZ += dz;
    obj->oHomeX += dx; obj->oHomeY += dy; obj->oHomeZ += dz;
    obj->header.gfx.pos[0] += dx; obj->header.gfx.pos[1] += dy; obj->header.gfx.pos[2] += dz;
    obj->oFloorHeight += dy;
    if( obj->behavior == bhvChainChomp && obj->oChainChompSegments != NULL )
    {
        struct ChainSegment *seg = obj->oChainChompSegments;
        for( int i = 0; i < 5; ++i ) { seg[i].posX += dx; seg[i].posY += dy; seg[i].posZ += dz; }
    }
    if( obj->behavior == bhvWigglerHead && obj->oWigglerSegments != NULL )
    {
        struct ChainSegment *seg = obj->oWigglerSegments;
        for( int i = 0; i < 4; ++i ) { seg[i].posX += dx; seg[i].posY += dy; seg[i].posZ += dz; }
    }
}

void objects_shift( float dx, float dy, float dz )
{
    int32_t i;
    for( i = 0; i < OBJECT_POOL_CAPACITY; ++i )
    {
        struct Object *obj = &gObjectPool[i];
        if( !( obj->activeFlags & ACTIVE_FLAG_ACTIVE )) continue;
        if( obj == gMarioObject ) continue;
        shift_object( obj, dx, dy, dz );
        s_spawn_infos[i].startPos[0] += dx;
        s_spawn_infos[i].startPos[1] += dy;
        s_spawn_infos[i].startPos[2] += dz;
    }
}

int32_t objects_active_count( void )
{
    int32_t i, n = 0;
    for( i = 0; i < OBJECT_POOL_CAPACITY; ++i )
        if(( gObjectPool[i].activeFlags & ACTIVE_FLAG_ACTIVE ) && &gObjectPool[i] != gMarioObject ) ++n;
    return n;
}

// The enemies and bosses out there, for the host: their positions, at most `max` of them, in the pool's order.
// Anything of the menu's ENEMIES or BOSSES categories that is active and not held counts.
int32_t objects_threats( float *out, int32_t max )
{
    int32_t n = 0;
    for( int32_t i = 0; i < OBJECT_POOL_CAPACITY && n < max; ++i )
    {
        struct Object *obj = &gObjectPool[i];
        if( !( obj->activeFlags & ACTIVE_FLAG_ACTIVE ) || obj == gMarioObject ) continue;
        if( obj->oHeldState != HELD_FREE ) continue;
        const char *category = NULL;
        for( int32_t e = 0; e < gMenuEntryCount; ++e )
            if( gMenuEntries[e].behavior == obj->behavior ) { category = gMenuEntries[e].category; break; }
        if( category == NULL || ( category[0] != 'E' && category[0] != 'B' )) continue;
        out[n * 3 + 0] = obj->oPosX;
        out[n * 3 + 1] = obj->oPosY;
        out[n * 3 + 2] = obj->oPosZ;
        ++n;
    }
    return n;
}

int32_t objects_forget_platform( struct Object *platform )
{
    int32_t i, n = 0;
    for( i = 0; i < OBJECT_POOL_CAPACITY; ++i )
    {
        if( gObjectPool[i].platform == platform ) { gObjectPool[i].platform = NULL; ++n; }
    }
    return n;
}
