#ifndef SM64_LIB_EXPORT
    #define SM64_LIB_EXPORT
#endif

#include "libsm64.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <math.h>

#include "decomp/audio/external.h"
#include "decomp/audio/load.h"
#include "decomp/audio/internal.h"
#include "decomp/include/PR/os_cont.h"
#include "decomp/engine/math_util.h"
#include "decomp/include/sm64.h"
#include "decomp/include/seq_ids.h"
#include "decomp/include/types.h"
#include "decomp/shim.h"
#include "decomp/memory.h"
#include "decomp/global_state.h"
#include "decomp/game/mario.h"
#include "decomp/game/object_list_processor.h"
#include "decomp/game/object_helpers.h"
#include "decomp/game/spawn_object.h"
#include "objects.h"
#include "load_actor_tex.h"
#include "decomp/libsm64_objects_glue.h"
#include "decomp/actors/menu_table.h"
#include "decomp/engine/surface_collision.h"
#include "decomp/engine/graph_node.h"
#include "decomp/engine/geo_layout.h"
#include "decomp/game/rendering_graph_node.h"
#include "decomp/mario/geo.inc.h"
#include "decomp/game/platform_displacement.h"
#include "decomp/game/sound_init.h"

#include "debug_print.h"
#include "load_surfaces.h"
#include "gfx_adapter.h"
#include "load_anim_data.h"
#include "load_audio_data.h"
#include "load_tex_data.h"
#include "obj_pool.h"
#include "fake_interaction.h"

static struct AllocOnlyPool *s_mario_geo_pool = NULL;
static struct GraphNode *s_mario_graph_node = NULL;

static bool s_init_global = false;
static bool s_init_one_mario = false;

struct MarioInstance
{
    struct GlobalState *globalState;
};
struct ObjPool s_mario_instance_pool = { 0, 0 };
static struct SM64ObjectGeometryBuffers *s_objectBuffers = NULL;   // where the objects' triangles go each tick

static void update_button( bool on, u16 button )
{
    gController.buttonPressed &= ~button;

    if( on )
    {
        if(( gController.buttonDown & button ) == 0 )
            gController.buttonPressed |= button;

        gController.buttonDown |= button;
    }
    else
    {
        gController.buttonDown &= ~button;
    }
}

static struct Area *allocate_area( void )
{
    struct Area *result = malloc( sizeof( struct Area ));
    memset( result, 0, sizeof( struct Area ));

    result->flags = 1;
    result->camera = malloc( sizeof( struct Camera ));
    memset( result->camera, 0, sizeof( struct Camera ));

    return result;
}

static void free_area( struct Area *area )
{
    free( area->camera );
    free( area );
}

typedef void (*SM64DebugPrintFunctionPtr)( const char * );
SM64_LIB_FN void sm64_register_debug_print_function( SM64DebugPrintFunctionPtr debugPrintFunction )
{
    g_debug_print_func = debugPrintFunction;
}

typedef void (*SM64PlaySoundFunctionPtr)( uint32_t soundBits, float *pos );
SM64_LIB_FN void sm64_register_play_sound_function( SM64PlaySoundFunctionPtr playSoundFunction )
{
    g_play_sound_func = playSoundFunction;
}


SM64_LIB_FN void sm64_global_init( const uint8_t *rom, uint8_t *outTexture )
{
    if( s_init_global )
        sm64_global_terminate();

    s_init_global = true;

    load_mario_textures_from_rom( rom, outTexture );
    load_mario_anims_from_rom( rom );
    load_actor_textures_from_rom( rom );

    memory_init();
}

SM64_LIB_FN void sm64_global_terminate( void )
{
    if( !s_init_global ) return;

    global_state_bind( NULL );

    if( s_init_one_mario )
    {
        for( int i = 0; i < s_mario_instance_pool.size; ++i )
            if( s_mario_instance_pool.objects[i] != NULL )
                sm64_mario_delete( i );

        obj_pool_free_all( &s_mario_instance_pool );
    }

    s_init_global = false;
    s_init_one_mario = false;

    if( s_mario_geo_pool )
    {
        alloc_only_pool_free( s_mario_geo_pool );
        s_mario_geo_pool = NULL;
    }

    surfaces_unload_all();
    unload_mario_anims();
    unload_actor_textures();
    memory_terminate();
}

SM64_LIB_FN void sm64_audio_init( const uint8_t *rom ) {
    load_audio_banks( rom );
}

#define SAMPLES_HIGH 544
#define SAMPLES_LOW 528

// libsm64 addition: channels held silent whatever the sequence does with them.  A fade cannot do this - it is
// dropped while the sequence player has not made the channel yet, and by the time it exists its first notes are
// already in the buffer - so the mute is applied here, every tick, immediately before the samples are made.
static uint16_t s_mute_mask[SEQUENCE_PLAYERS];

SM64_LIB_FN void sm64_seq_channel_mute( uint8_t player, uint16_t mask )
{
    if( player < SEQUENCE_PLAYERS )
        s_mute_mask[player] = mask;
}

static void apply_channel_mutes( void )
{
    for( int p = 0; p < SEQUENCE_PLAYERS; ++p )
    {
        if( !s_mute_mask[p] )
            continue;
        for( int c = 0; c < CHANNELS_MAX; ++c )
        {
            if( !( s_mute_mask[p] & ( 1 << c ) ) )
                continue;
            struct SequenceChannel *ch = gSequencePlayers[p].channels[c];
            if( IS_SEQUENCE_CHANNEL_VALID( ch ) )
                ch->volumeScale = 0.0f;
        }
    }
}

SM64_LIB_FN uint32_t sm64_audio_tick( uint32_t numQueuedSamples, uint32_t numDesiredSamples, int16_t *audio_buffer ) {
    if ( !g_is_audio_initialized ) {
        DEBUG_PRINT("Attempted to tick audio, but sm64_audio_init() has not been called yet.");
        return 0;
    }

    update_game_sound();
    apply_channel_mutes();

    u32 num_audio_samples = numQueuedSamples < numDesiredSamples ? SAMPLES_HIGH : SAMPLES_LOW;
    for (int i = 0; i < 2; i++)
    {
        create_next_audio_buffer( audio_buffer + i * ( 2 * num_audio_samples ), num_audio_samples );
    }

    return num_audio_samples;
}

SM64_LIB_FN void sm64_static_surfaces_load( const struct SM64Surface *surfaceArray, uint32_t numSurfaces )
{
    surfaces_load_static( surfaceArray, numSurfaces );
}

SM64_LIB_FN int32_t sm64_mario_create( float x, float y, float z )
{
    int32_t marioIndex = obj_pool_alloc_index( &s_mario_instance_pool, sizeof( struct MarioInstance ));
    struct MarioInstance *newInstance = s_mario_instance_pool.objects[marioIndex];

    newInstance->globalState = global_state_create();
    global_state_bind( newInstance->globalState );

    if( !s_init_one_mario )
    {
        s_init_one_mario = true;
        s_mario_geo_pool = alloc_only_pool_init();
        s_mario_graph_node = process_geo_layout( s_mario_geo_pool, mario_geo_ptr );
    }

    gCurrSaveFileNum = 1;
    gCurrentArea = allocate_area();

    // the object engine: the models once, an empty pool for this level, and Mario as the first object in it
    objects_models_init( s_mario_graph_node );
    objects_level_init();
    gMarioObject = objects_spawn_mario( x, y, z, s_mario_graph_node );
    if( gMarioObject == NULL )
    {
        free_area( gCurrentArea );
        global_state_delete( newInstance->globalState );
        obj_pool_free_index( &s_mario_instance_pool, marioIndex );
        return -1;
    }
    gCurrentObject = gMarioObject;

    init_mario_from_save_file();

    if( init_mario() < 0 )
    {
        sm64_mario_delete( marioIndex );
        return -1;
    }

    set_mario_action( gMarioState, ACT_SPAWN_SPIN_AIRBORNE, 0);
    find_floor( x, y, z, &gMarioState->floor );

    return marioIndex;
}

SM64_LIB_FN void sm64_mario_tick( int32_t marioId, const struct SM64MarioInputs *inputs, struct SM64MarioState *outState, struct SM64MarioGeometryBuffers *outBuffers )
{
    if( marioId >= s_mario_instance_pool.size || s_mario_instance_pool.objects[marioId] == NULL )
    {
        DEBUG_PRINT("Tried to tick non-existant Mario with ID: %u", marioId);
        return;
    }

    global_state_bind( ((struct MarioInstance *)s_mario_instance_pool.objects[ marioId ])->globalState );

    update_button( inputs->buttonA, A_BUTTON );
    update_button( inputs->buttonB, B_BUTTON );
    update_button( inputs->buttonZ, Z_TRIG );

    gMarioState->marioObj->header.gfx.cameraToObject[0] = 0;
    gMarioState->marioObj->header.gfx.cameraToObject[1] = 0;
    gMarioState->marioObj->header.gfx.cameraToObject[2] = 0;

    gMarioState->area->camera->yaw = atan2s( inputs->camLookZ, inputs->camLookX );

    gController.stickX = -64.0f * inputs->stickX;
    gController.stickY = 64.0f * inputs->stickY;
    gController.stickMag = sqrtf( gController.stickX*gController.stickX + gController.stickY*gController.stickY );

    // every object in the pool, Mario among them: the platforms first, then the collisions between objects, then
    // each one's behaviour (Mario's is his action), then what was deactivated goes, then the platform he stands on
    libsm64_heap_check( "at the start of the tick" );
    display_list_pool_reset();                   // the frame's pool: what the behaviours build now (a slope's matrix) is drawn, then let go
    objects_tick();
    libsm64_heap_check( "after the object updates" );

    gfx_adapter_bind_output_buffers( outBuffers );
    gfx_adapter_bind_object_buffers( s_objectBuffers );

    geo_process_root_objects();
    libsm64_heap_check( "after the render" );

    gAreaUpdateCounter++;

    outState->health = gMarioState->health;
    vec3f_copy( outState->position, gMarioState->pos );
    vec3f_copy( outState->velocity, gMarioState->vel );
    outState->faceAngle = (float)gMarioState->faceAngle[1] / 32768.0f * 3.14159f;
    outState->forwardVelocity = gMarioState->forwardVel;
    outState->action = gMarioState->action;

    struct AnimInfo animInfo = gMarioState->marioObj->header.gfx.animInfo;
    outState->animID = animInfo.animID;
    outState->animFrame = animInfo.animFrame;

    outState->flags = gMarioState->flags;
    outState->particleFlags = gMarioState->particleFlags;
    outState->invincTimer = gMarioState->invincTimer;
}

SM64_LIB_FN void sm64_mario_delete( int32_t marioId )
{
    if( marioId >= s_mario_instance_pool.size || s_mario_instance_pool.objects[marioId] == NULL )
    {
        DEBUG_PRINT("Tried to delete non-existant Mario with ID: %u", marioId);
        return;
    }

    struct GlobalState *globalState = ((struct MarioInstance *)s_mario_instance_pool.objects[ marioId ])->globalState;
    global_state_bind( globalState );

    if ( g_is_audio_initialized ) {
        stop_sound(SOUND_MARIO_SNORING3, gMarioState->marioObj->header.gfx.cameraToObject);
    }

    // everything he spawned goes with him, and his own slot in the pool is given back
    objects_clear_spawned();
    if( gMarioObject != NULL )
    {
        stop_sounds_from_source( gMarioObject->header.gfx.cameraToObject );
        unload_object( gMarioObject );
        gMarioObject = NULL;
    }
    free_area( gCurrentArea );

    global_state_delete( globalState );
    obj_pool_free_index( &s_mario_instance_pool, marioId );
}

SM64_LIB_FN void sm64_set_mario_action(int32_t marioId, uint32_t action)
{
    if( marioId >= s_mario_instance_pool.size || s_mario_instance_pool.objects[marioId] == NULL )
    {
        DEBUG_PRINT("Tried to use non-existant Mario with ID: %d", marioId);
        return;
    }

    struct GlobalState *globalState = ((struct MarioInstance *)s_mario_instance_pool.objects[ marioId ])->globalState;
    global_state_bind( globalState );

    set_mario_action(gMarioState, action, 0);
}

SM64_LIB_FN void sm64_set_mario_action_arg(int32_t marioId, uint32_t action, uint32_t actionArg)
{
    if( marioId >= s_mario_instance_pool.size || s_mario_instance_pool.objects[marioId] == NULL )
    {
        DEBUG_PRINT("Tried to use non-existant Mario with ID: %d", marioId);
        return;
    }

    struct GlobalState *globalState = ((struct MarioInstance *)s_mario_instance_pool.objects[ marioId ])->globalState;
    global_state_bind( globalState );

    set_mario_action(gMarioState, action, actionArg);
}

SM64_LIB_FN void sm64_set_mario_animation(int32_t marioId, int32_t animID)
{
    if( marioId >= s_mario_instance_pool.size || s_mario_instance_pool.objects[marioId] == NULL )
    {
        DEBUG_PRINT("Tried to use non-existant Mario with ID: %d", marioId);
        return;
    }

    struct GlobalState *globalState = ((struct MarioInstance *)s_mario_instance_pool.objects[ marioId ])->globalState;
    global_state_bind( globalState );

    set_mario_animation(gMarioState, animID);
}

SM64_LIB_FN void sm64_set_mario_anim_frame(int32_t marioId, int16_t animFrame)
{
    if( marioId >= s_mario_instance_pool.size || s_mario_instance_pool.objects[marioId] == NULL )
    {
        DEBUG_PRINT("Tried to use non-existant Mario with ID: %d", marioId);
        return;
    }

    struct GlobalState *globalState = ((struct MarioInstance *)s_mario_instance_pool.objects[ marioId ])->globalState;
    global_state_bind( globalState );

    gMarioState->marioObj->header.gfx.animInfo.animFrame = animFrame;
}

SM64_LIB_FN void sm64_set_mario_state(int32_t marioId, uint32_t flags)
{
    if( marioId >= s_mario_instance_pool.size || s_mario_instance_pool.objects[marioId] == NULL )
    {
        DEBUG_PRINT("Tried to use non-existant Mario with ID: %d", marioId);
        return;
    }

    struct GlobalState *globalState = ((struct MarioInstance *)s_mario_instance_pool.objects[ marioId ])->globalState;
    global_state_bind( globalState );

    gMarioState->flags = flags;
}

SM64_LIB_FN void sm64_set_mario_position(int32_t marioId, float x, float y, float z)
{
    if( marioId >= s_mario_instance_pool.size || s_mario_instance_pool.objects[marioId] == NULL )
    {
        DEBUG_PRINT("Tried to use non-existant Mario with ID: %d", marioId);
        return;
    }

    struct GlobalState *globalState = ((struct MarioInstance *)s_mario_instance_pool.objects[ marioId ])->globalState;
    global_state_bind( globalState );

    gMarioState->pos[0] = x;
    gMarioState->pos[1] = y;
    gMarioState->pos[2] = z;
    vec3f_copy(gMarioState->marioObj->header.gfx.pos, gMarioState->pos);
}

SM64_LIB_FN void sm64_set_mario_angle(int32_t marioId, float x, float y, float z)
{
    if( marioId >= s_mario_instance_pool.size || s_mario_instance_pool.objects[marioId] == NULL )
    {
        DEBUG_PRINT("Tried to use non-existant Mario with ID: %d", marioId);
        return;
    }

    struct GlobalState *globalState = ((struct MarioInstance *)s_mario_instance_pool.objects[ marioId ])->globalState;
    global_state_bind( globalState );

    vec3s_set(gMarioState->faceAngle, (int16_t)(x / 3.14159f * 32768.f), (int16_t)(y / 3.14159f * 32768.f), (int16_t)(z / 3.14159f * 32768.f));
    vec3s_copy(gMarioState->marioObj->header.gfx.angle, gMarioState->faceAngle);
}

SM64_LIB_FN void sm64_set_mario_faceangle(int32_t marioId, float y)
{
    if( marioId >= s_mario_instance_pool.size || s_mario_instance_pool.objects[marioId] == NULL )
    {
        DEBUG_PRINT("Tried to use non-existant Mario with ID: %d", marioId);
        return;
    }

    struct GlobalState *globalState = ((struct MarioInstance *)s_mario_instance_pool.objects[ marioId ])->globalState;
    global_state_bind( globalState );

    gMarioState->faceAngle[1] = (int16_t)(y / 3.14159f * 32768.f);
    vec3s_set(gMarioState->marioObj->header.gfx.angle, 0, gMarioState->faceAngle[1], 0);
}

SM64_LIB_FN void sm64_set_mario_velocity(int32_t marioId, float x, float y, float z)
{
    if( marioId >= s_mario_instance_pool.size || s_mario_instance_pool.objects[marioId] == NULL )
    {
        DEBUG_PRINT("Tried to use non-existant Mario with ID: %d", marioId);
        return;
    }

    struct GlobalState *globalState = ((struct MarioInstance *)s_mario_instance_pool.objects[ marioId ])->globalState;
    global_state_bind( globalState );

    gMarioState->vel[0] = x;
    gMarioState->vel[1] = y;
    gMarioState->vel[2] = z;
}

SM64_LIB_FN void sm64_set_mario_forward_velocity(int32_t marioId, float vel)
{
    if( marioId >= s_mario_instance_pool.size || s_mario_instance_pool.objects[marioId] == NULL )
    {
        DEBUG_PRINT("Tried to use non-existant Mario with ID: %d", marioId);
        return;
    }

    struct GlobalState *globalState = ((struct MarioInstance *)s_mario_instance_pool.objects[ marioId ])->globalState;
    global_state_bind( globalState );

    gMarioState->forwardVel = vel;
}

SM64_LIB_FN void sm64_set_mario_invincibility(int32_t marioId, int16_t timer)
{
    if( marioId >= s_mario_instance_pool.size || s_mario_instance_pool.objects[marioId] == NULL )
    {
        DEBUG_PRINT("Tried to use non-existant Mario with ID: %d", marioId);
        return;
    }

    struct GlobalState *globalState = ((struct MarioInstance *)s_mario_instance_pool.objects[ marioId ])->globalState;
    global_state_bind( globalState );

    gMarioState->invincTimer = timer;
}

SM64_LIB_FN void sm64_set_mario_water_level(int32_t marioId, signed int level)
{
    if( marioId >= s_mario_instance_pool.size || s_mario_instance_pool.objects[marioId] == NULL )
    {
        DEBUG_PRINT("Tried to use non-existant Mario with ID: %d", marioId);
        return;
    }

    struct GlobalState *globalState = ((struct MarioInstance *)s_mario_instance_pool.objects[ marioId ])->globalState;
    global_state_bind( globalState );

    gMarioState->waterLevel = level;
}

SM64_LIB_FN void sm64_set_mario_gas_level(int32_t marioId, signed int level)
{
    if( marioId >= s_mario_instance_pool.size || s_mario_instance_pool.objects[marioId] == NULL )
    {
        DEBUG_PRINT("Tried to use non-existant Mario with ID: %d", marioId);
        return;
    }

    struct GlobalState *globalState = ((struct MarioInstance *)s_mario_instance_pool.objects[ marioId ])->globalState;
    global_state_bind( globalState );

    gMarioState->gasLevel = level;
}

SM64_LIB_FN void sm64_set_mario_health(int32_t marioId, uint16_t health)
{
    if( marioId >= s_mario_instance_pool.size || s_mario_instance_pool.objects[marioId] == NULL )
    {
        DEBUG_PRINT("Tried to use non-existant Mario with ID: %d", marioId);
        return;
    }

    struct GlobalState *globalState = ((struct MarioInstance *)s_mario_instance_pool.objects[ marioId ])->globalState;
    global_state_bind( globalState );

    gMarioState->health = health;
    gMarioState->hurtCounter = 0;
    gMarioState->healCounter = 0;
}

SM64_LIB_FN void sm64_mario_take_damage(int32_t marioId, uint32_t damage, uint32_t subtype, float x, float y, float z)
{
    if( marioId >= s_mario_instance_pool.size || s_mario_instance_pool.objects[marioId] == NULL )
    {
        DEBUG_PRINT("Tried to use non-existant Mario with ID: %d", marioId);
        return;
    }

    struct GlobalState *globalState = ((struct MarioInstance *)s_mario_instance_pool.objects[ marioId ])->globalState;
    global_state_bind( globalState );

    fake_damage_knock_back(gMarioState, damage, subtype, x, y, z);
}

SM64_LIB_FN void sm64_mario_heal(int32_t marioId, uint8_t healCounter)
{
    if( marioId >= s_mario_instance_pool.size || s_mario_instance_pool.objects[marioId] == NULL )
    {
        DEBUG_PRINT("Tried to use non-existant Mario with ID: %d", marioId);
        return;
    }

    struct GlobalState *globalState = ((struct MarioInstance *)s_mario_instance_pool.objects[ marioId ])->globalState;
    global_state_bind( globalState );

    gMarioState->healCounter += healCounter;
}

SM64_LIB_FN void sm64_mario_kill(int32_t marioId)
{
    if( marioId >= s_mario_instance_pool.size || s_mario_instance_pool.objects[marioId] == NULL )
    {
        DEBUG_PRINT("Tried to use non-existant Mario with ID: %d", marioId);
        return;
    }

    struct GlobalState *globalState = ((struct MarioInstance *)s_mario_instance_pool.objects[ marioId ])->globalState;
    global_state_bind( globalState );

    gMarioState->health = 0xff;
}

SM64_LIB_FN void sm64_mario_interact_cap(int32_t marioId, uint32_t capFlag, uint16_t capTime, uint8_t playMusic)
{
    if( marioId >= s_mario_instance_pool.size || s_mario_instance_pool.objects[marioId] == NULL )
    {
        DEBUG_PRINT("Tried to use non-existant Mario with ID: %d", marioId);
        return;
    }

    struct GlobalState *globalState = ((struct MarioInstance *)s_mario_instance_pool.objects[ marioId ])->globalState;
    global_state_bind( globalState );

    uint16_t capMusic = 0;
    if(gMarioState->action != ACT_GETTING_BLOWN && capFlag != 0)
    {
        gMarioState->flags &= ~MARIO_CAP_ON_HEAD & ~MARIO_CAP_IN_HAND;
        gMarioState->flags |= capFlag;

        switch(capFlag)
        {
            case MARIO_VANISH_CAP:
                if(capTime == 0) capTime = 600;
                capMusic = SEQUENCE_ARGS(4, SEQ_EVENT_POWERUP);
                break;
            case MARIO_METAL_CAP:
                if(capTime == 0) capTime = 600;
                capMusic = SEQUENCE_ARGS(4, SEQ_EVENT_METAL_CAP);
                break;
            case MARIO_WING_CAP:
                if(capTime == 0) capTime = 1800;
                capMusic = SEQUENCE_ARGS(4, SEQ_EVENT_POWERUP);
                break;
        }

        if (capTime > gMarioState->capTimer) {
            gMarioState->capTimer = capTime;
        }

        if ((gMarioState->action & ACT_FLAG_IDLE) || gMarioState->action == ACT_WALKING) {
            gMarioState->flags |= MARIO_CAP_IN_HAND;
            set_mario_action(gMarioState, ACT_PUTTING_ON_CAP, 0);
        } else {
            gMarioState->flags |= MARIO_CAP_ON_HEAD;
        }

        play_sound(SOUND_MENU_STAR_SOUND, gMarioState->marioObj->header.gfx.cameraToObject);
        play_sound(SOUND_MARIO_HERE_WE_GO, gMarioState->marioObj->header.gfx.cameraToObject);

        if (playMusic != 0 && capMusic != 0) {
            play_cap_music(capMusic);
        }
    }
}

SM64_LIB_FN void sm64_mario_extend_cap(int32_t marioId, uint16_t capTime)
{
    if( marioId >= s_mario_instance_pool.size || s_mario_instance_pool.objects[marioId] == NULL )
    {
        DEBUG_PRINT("Tried to use non-existant Mario with ID: %d", marioId);
        return;
    }

    struct GlobalState *globalState = ((struct MarioInstance *)s_mario_instance_pool.objects[ marioId ])->globalState;
    global_state_bind( globalState );

    gMarioState->capTimer += capTime;
}

SM64_LIB_FN bool sm64_mario_attack(int32_t marioId, float x, float y, float z, float hitboxHeight)
{
    if( marioId >= s_mario_instance_pool.size || s_mario_instance_pool.objects[marioId] == NULL )
    {
        DEBUG_PRINT("Tried to use non-existant Mario with ID: %d", marioId);
        return false;
    }

    struct GlobalState *globalState = ((struct MarioInstance *)s_mario_instance_pool.objects[ marioId ])->globalState;
    global_state_bind( globalState );

    return fake_interact_bounce_top(gMarioState, x, y, z, hitboxHeight);
}

SM64_LIB_FN uint32_t sm64_surface_object_create( const struct SM64SurfaceObject *surfaceObject )
{
    uint32_t id = surfaces_load_object( surfaceObject );
    return id;
}

SM64_LIB_FN void sm64_surface_object_move( uint32_t objectId, const struct SM64ObjectTransform *transform )
{
    surface_object_update_transform( objectId, transform );
}

SM64_LIB_FN void sm64_surface_object_delete( uint32_t objectId )
{
    // A mario standing on the platform that is being destroyed will have a pointer to freed memory if we don't clear it.
    for( int i = 0; i < s_mario_instance_pool.size; ++i )
    {
        if( s_mario_instance_pool.objects[i] == NULL )
            continue;

        struct GlobalState *state = ((struct MarioInstance *)s_mario_instance_pool.objects[ i ])->globalState;
        if( state->mgMarioObject != NULL && state->mgMarioObject->platform == surfaces_object_get_object( objectId ))
            state->mgMarioObject->platform = NULL;
    }
    // and any object standing on it (Bowser tracks his platform)
    if( surfaces_object_get_object( objectId ) != NULL )
        objects_forget_platform( surfaces_object_get_object( objectId ));

    surfaces_unload_object( objectId );
}

// ---------------------------------------------------------------- the object engine
static struct SM64ObjectGeometryBuffers *s_objectBuffers_unused;

SM64_LIB_FN void sm64_set_camera( float x, float y, float z, float lookX, float lookY, float lookZ, float upX, float upY, float upZ )
{
    float pos[3] = { x, y, z };
    float look[3] = { lookX, lookY, lookZ };
    float up[3] = { upX, upY, upZ };
    libsm64_camera_set( pos, look, up );
}

SM64_LIB_FN void sm64_set_object_geometry( struct SM64ObjectGeometryBuffers *outBuffers )
{
    s_objectBuffers = outBuffers;
}

SM64_LIB_FN int32_t sm64_menu_count( void )
{
    return gMenuEntryCount;
}

SM64_LIB_FN const char *sm64_menu_name( int32_t entry )
{
    if( entry < 0 || entry >= gMenuEntryCount ) return "";
    return gMenuEntries[entry].name;
}

SM64_LIB_FN const char *sm64_menu_category( int32_t entry )
{
    if( entry < 0 || entry >= gMenuEntryCount ) return "";
    return gMenuEntries[entry].category;
}

// the instance that owns the objects: the first Mario made (this library holds one pool)
static struct GlobalState *owner_state( void )
{
    for( int i = 0; i < s_mario_instance_pool.size; ++i )
        if( s_mario_instance_pool.objects[i] != NULL )
            return ((struct MarioInstance *)s_mario_instance_pool.objects[ i ])->globalState;
    return NULL;
}

SM64_LIB_FN int32_t sm64_object_spawn( int32_t entry, float x, float y, float z, float yaw )
{
    struct GlobalState *state = owner_state();
    if( state == NULL ) return -1;
    global_state_bind( state );
    return objects_spawn_entry( entry, x, y, z, (int16_t)( yaw / 3.14159f * 32768.0f ));
}

SM64_LIB_FN void sm64_objects_clear( void )
{
    struct GlobalState *state = owner_state();
    if( state == NULL ) return;
    global_state_bind( state );
    objects_clear_spawned();
}

SM64_LIB_FN int32_t sm64_objects_count( void )
{
    struct GlobalState *state = owner_state();
    if( state == NULL ) return 0;
    global_state_bind( state );
    return objects_active_count();
}

SM64_LIB_FN void sm64_objects_shift( float dx, float dy, float dz )
{
    struct GlobalState *state = owner_state();
    if( state == NULL ) return;
    global_state_bind( state );
    objects_shift( dx, dy, dz );
}

SM64_LIB_FN int32_t sm64_texture_count( void )
{
    return actor_texture_count();
}

SM64_LIB_FN int32_t sm64_texture_size( int32_t index, int32_t *w, int32_t *h )
{
    int iw = 0, ih = 0;
    int ok = actor_texture_size( index, &iw, &ih );
    if( w ) *w = iw;
    if( h ) *h = ih;
    return ok;
}

SM64_LIB_FN const uint8_t *sm64_texture_rgba( int32_t index )
{
    return actor_texture_rgba( index );
}

extern int32_t libsm64_dialog_frames( void );
extern void libsm64_dialog_close( void );
extern s16 gDialogID;

SM64_LIB_FN int32_t sm64_dialog_state( int32_t *dialogID )
{
    if( dialogID != NULL ) *dialogID = gDialogID;
    return libsm64_dialog_frames();
}

SM64_LIB_FN void sm64_dialog_close( void )
{
    libsm64_dialog_close();
}

SM64_LIB_FN void sm64_hud_counts( int32_t *coins, int32_t *stars )
{
    struct GlobalState *state = owner_state();
    if( coins != NULL ) *coins = ( state != NULL ) ? state->mgMarioStateVal.numCoins : 0;
    if( stars != NULL ) *stars = libsm64_stars_collected();
}

SM64_LIB_FN int32_t sm64_threats( float *out, int32_t max )
{
    struct GlobalState *state = owner_state();
    if( state == NULL || out == NULL || max <= 0 ) return 0;
    global_state_bind( state );
    return objects_threats( out, max );
}

SM64_LIB_FN void sm64_prey_set( const float *pos, int32_t n )
{
    objects_set_prey( pos, pos != NULL ? n : 0 );
}

SM64_LIB_FN int32_t sm64_prey_hits( int32_t *out, int32_t max )
{
    if( out == NULL || max <= 0 ) return 0;
    return objects_prey_hits( out, max );
}

SM64_LIB_FN void sm64_set_debug( uint32_t flags )
{
    gLibsm64DebugFlags = flags;
}

SM64_LIB_FN int32_t sm64_object_info( int32_t slot, float *pos, int32_t *entryOrMinusOne )
{
    if( slot < 0 || slot >= OBJECT_POOL_CAPACITY ) return 0;
    struct Object *obj = &gObjectPool[slot];
    if( !( obj->activeFlags & ACTIVE_FLAG_ACTIVE )) return 0;
    if( pos ) { pos[0] = obj->oPosX; pos[1] = obj->oPosY; pos[2] = obj->oPosZ; }
    if( entryOrMinusOne )
    {
        *entryOrMinusOne = -1;
        for( int i = 0; i < gMenuEntryCount; ++i )
            if( gMenuEntries[i].behavior == obj->behavior ) { *entryOrMinusOne = i; break; }
    }
    return 1;
}


SM64_LIB_FN int32_t sm64_surface_find_wall_collision( float *xPtr, float *yPtr, float *zPtr, float offsetY, float radius )
{
    return f32_find_wall_collision( xPtr, yPtr, zPtr, offsetY, radius );
}

SM64_LIB_FN int32_t sm64_surface_find_wall_collisions( struct SM64WallCollisionData *colData )
{
    return find_wall_collisions( colData );
}

SM64_LIB_FN float sm64_surface_find_ceil( float posX, float posY, float posZ, struct SM64SurfaceCollisionData **pceil )
{
    return find_ceil( posX, posY, posZ, pceil );
}

SM64_LIB_FN float sm64_surface_find_floor_height_and_data( float xPos, float yPos, float zPos, struct SM64FloorCollisionData **floorGeo )
{
    return find_floor_height_and_data( xPos, yPos, zPos, floorGeo );
}

SM64_LIB_FN float sm64_surface_find_floor_height( float x, float y, float z )
{
    return find_floor_height( x, y, z );
}

SM64_LIB_FN float sm64_surface_find_floor( float xPos, float yPos, float zPos, struct SM64SurfaceCollisionData **pfloor )
{
    return find_floor( xPos, yPos, zPos, pfloor );
}

SM64_LIB_FN float sm64_surface_find_water_level( float x, float z )
{
    return find_water_level( x, z );
}

SM64_LIB_FN float sm64_surface_find_poison_gas_level( float x, float z )
{
    return find_poison_gas_level( x, z );
}

SM64_LIB_FN void sm64_seq_player_play_sequence(uint8_t player, uint8_t seqId, uint16_t arg2)
{
    seq_player_play_sequence(player,seqId,arg2);
}

SM64_LIB_FN void sm64_play_music(uint8_t player, uint16_t seqArgs, uint16_t fadeTimer)
{
    play_music(player,seqArgs,fadeTimer);
}

SM64_LIB_FN void sm64_stop_background_music(uint16_t seqId)
{
    stop_background_music(seqId);
}

SM64_LIB_FN void sm64_fadeout_background_music(uint16_t arg0, uint16_t fadeOut)
{
    fadeout_background_music(arg0,fadeOut);
}

SM64_LIB_FN uint16_t sm64_get_current_background_music()
{
    return get_current_background_music();
}

SM64_LIB_FN void sm64_play_sound(int32_t soundBits, float *pos)
{
    play_sound(soundBits,pos);
}

SM64_LIB_FN void sm64_play_sound_global(int32_t soundBits)
{
    play_sound(soundBits,gGlobalSoundSource);
}

SM64_LIB_FN void sm64_set_sound_volume(float vol)
{
    gAudioVolume = vol;
}

SM64_LIB_FN void sm64_seq_channel_fade(uint8_t player, uint8_t channel, uint8_t volScale, uint16_t fadeDuration)
{
    audio_channel_fade(player, channel, volScale, fadeDuration);
}
