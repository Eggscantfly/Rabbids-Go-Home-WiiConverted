#ifndef LIB_SM64_H
#define LIB_SM64_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#if defined(_WIN32)
    #ifdef SM64_LIB_EXPORT
        #define SM64_LIB_FN __declspec(dllexport)
    #else
        #define SM64_LIB_FN __declspec(dllimport)
    #endif
#elif defined(__GNUC__) && __GNUC__ >= 4
    #ifdef SM64_LIB_EXPORT
        #define SM64_LIB_FN __attribute__ ((visibility("default")))
    #else
        #define SM64_LIB_FN
    #endif
#else
    #define SM64_LIB_FN
#endif

#ifdef __cplusplus
extern "C" {
#endif

struct SM64Surface
{
    int16_t type;
    int16_t force;
    uint16_t terrain;
    int32_t vertices[3][3];
};

struct SM64MarioInputs
{
    float camLookX, camLookZ;
    float stickX, stickY;
    uint8_t buttonA, buttonB, buttonZ;
};

struct SM64ObjectTransform
{
    float position[3];
    float eulerRotation[3];
};

struct SM64SurfaceObject
{
    struct SM64ObjectTransform transform;
    uint32_t surfaceCount;
    struct SM64Surface *surfaces;
};

struct SM64MarioState
{
    float position[3];
    float velocity[3];
    float faceAngle;
    float forwardVelocity;
    int16_t health;
    uint32_t action;
    int32_t animID;
    int16_t animFrame;
    uint32_t flags;
    uint32_t particleFlags;
    int16_t invincTimer;
};

struct SM64MarioGeometryBuffers
{
    float *position;
    float *normal;
    float *color;
    float *uv;
    uint16_t numTrianglesUsed;
};

struct SM64WallCollisionData
{
    /*0x00*/ float x, y, z;
    /*0x0C*/ float offsetY;
    /*0x10*/ float radius;
    /*0x14*/ int16_t unk14;
    /*0x16*/ int16_t numWalls;
    /*0x18*/ struct SM64SurfaceCollisionData *walls[4];
};

struct SM64FloorCollisionData
{
    float unused[4]; // possibly position data?
    float normalX;
    float normalY;
    float normalZ;
    float originOffset;
};

struct SM64SurfaceObjectTransform
{
    float aPosX, aPosY, aPosZ;
    float aVelX, aVelY, aVelZ;

    int16_t aFaceAnglePitch;
    int16_t aFaceAngleYaw;
    int16_t aFaceAngleRoll;

    int16_t aAngleVelPitch;
    int16_t aAngleVelYaw;
    int16_t aAngleVelRoll;
};

struct SM64SurfaceCollisionData
{
    int16_t type;
    int16_t force;
    int8_t flags;
    int8_t room;
    int32_t lowerY; // libsm64: 32 bit
    int32_t upperY; // libsm64: 32 bit
    int32_t vertex1[3]; // libsm64: 32 bit
    int32_t vertex2[3]; // libsm64: 32 bit
    int32_t vertex3[3]; // libsm64: 32 bit
    struct {
        float x;
        float y;
        float z;
    } normal;
    float originOffset;

    uint8_t isValid; // libsm64: added field
    struct SM64SurfaceObjectTransform *transform; // libsm64: added field
    uint16_t terrain; // libsm64: added field
    struct Object *object; // libsm64: the object this is the collision of, or a stand-in for a surface object
};

// The objects' triangles: everything in the level but Mario himself, from one call of sm64_mario_tick.
// Positions, normals (zero where a corner has none: an unlit, vertex-coloured triangle), a colour with alpha per
// corner, texture coordinates (repeating past 0..1 unless the flags say the picture clamps), and per triangle the
// picture (sm64_texture_*, 0xFFFF for none), flags, the drawing layer (0..7: 5 and above are see-through and drawn
// last) and the pool slot of the object the triangle belongs to.
struct SM64ObjectGeometryBuffers
{
    float *position;        // 9 per triangle
    float *normal;          // 9 per triangle
    float *color;           // 12 per triangle: r g b a per corner
    float *uv;              // 6 per triangle
    uint16_t *texture;      // 1 per triangle
    uint8_t *flags;         // 1 per triangle (SM64_TRI_*)
    uint8_t *layer;         // 1 per triangle
    uint16_t *object;       // 1 per triangle
    uint32_t capacity;      // in triangles
    uint32_t numTrianglesUsed;
};

enum
{
    SM64_TRI_TEXTURED = 1 << 0,     // the colour is the picture's, times the corner colour
    SM64_TRI_TEX_ALPHA = 1 << 1,    // the alpha is the picture's, times the corner alpha
    SM64_TRI_BLEND = 1 << 2,        // the picture is laid over the corner colour by its alpha (Mario's face)
    SM64_TRI_LIT = 1 << 3,          // the corners carry normals: light it
    SM64_TRI_CLAMP_S = 1 << 4,
    SM64_TRI_CLAMP_T = 1 << 5,
    SM64_TRI_MIRROR_S = 1 << 6,
    SM64_TRI_MIRROR_T = 1 << 7,
};

enum
{
    SM64_TEXTURE_WIDTH = 64 * 11,
    SM64_TEXTURE_HEIGHT = 64,
    SM64_GEO_MAX_TRIANGLES = 1024,
};


typedef void (*SM64DebugPrintFunctionPtr)( const char * );
extern SM64_LIB_FN void sm64_register_debug_print_function( SM64DebugPrintFunctionPtr debugPrintFunction );

typedef void (*SM64PlaySoundFunctionPtr)( uint32_t soundBits, float *pos );
extern SM64_LIB_FN void sm64_register_play_sound_function( SM64PlaySoundFunctionPtr playSoundFunction );

extern SM64_LIB_FN void sm64_global_init( const uint8_t *rom, uint8_t *outTexture );
extern SM64_LIB_FN void sm64_global_terminate( void );

extern SM64_LIB_FN void sm64_audio_init( const uint8_t *rom );
extern SM64_LIB_FN uint32_t sm64_audio_tick( uint32_t numQueuedSamples, uint32_t numDesiredSamples, int16_t *audio_buffer );

extern SM64_LIB_FN void sm64_static_surfaces_load( const struct SM64Surface *surfaceArray, uint32_t numSurfaces );

extern SM64_LIB_FN int32_t sm64_mario_create( float x, float y, float z );
extern SM64_LIB_FN void sm64_mario_tick( int32_t marioId, const struct SM64MarioInputs *inputs, struct SM64MarioState *outState, struct SM64MarioGeometryBuffers *outBuffers );
extern SM64_LIB_FN void sm64_mario_delete( int32_t marioId );

extern SM64_LIB_FN void sm64_set_mario_action(int32_t marioId, uint32_t action);
extern SM64_LIB_FN void sm64_set_mario_action_arg(int32_t marioId, uint32_t action, uint32_t actionArg);
extern SM64_LIB_FN void sm64_set_mario_animation(int32_t marioId, int32_t animID);
extern SM64_LIB_FN void sm64_set_mario_anim_frame(int32_t marioId, int16_t animFrame);
extern SM64_LIB_FN void sm64_set_mario_state(int32_t marioId, uint32_t flags);
extern SM64_LIB_FN void sm64_set_mario_position(int32_t marioId, float x, float y, float z);
extern SM64_LIB_FN void sm64_set_mario_angle(int32_t marioId, float x, float y, float z);
extern SM64_LIB_FN void sm64_set_mario_faceangle(int32_t marioId, float y);
extern SM64_LIB_FN void sm64_set_mario_velocity(int32_t marioId, float x, float y, float z);
extern SM64_LIB_FN void sm64_set_mario_forward_velocity(int32_t marioId, float vel);
extern SM64_LIB_FN void sm64_set_mario_invincibility(int32_t marioId, int16_t timer);
extern SM64_LIB_FN void sm64_set_mario_water_level(int32_t marioId, signed int level);
extern SM64_LIB_FN void sm64_set_mario_gas_level(int32_t marioId, signed int level);
extern SM64_LIB_FN void sm64_set_mario_health(int32_t marioId, uint16_t health);
extern SM64_LIB_FN void sm64_mario_take_damage(int32_t marioId, uint32_t damage, uint32_t subtype, float x, float y, float z);
extern SM64_LIB_FN void sm64_mario_heal(int32_t marioId, uint8_t healCounter);
extern SM64_LIB_FN void sm64_mario_kill(int32_t marioId);
extern SM64_LIB_FN void sm64_mario_interact_cap(int32_t marioId, uint32_t capFlag, uint16_t capTime, uint8_t playMusic);
extern SM64_LIB_FN void sm64_mario_extend_cap(int32_t marioId, uint16_t capTime);
extern SM64_LIB_FN bool sm64_mario_attack(int32_t marioId, float x, float y, float z, float hitboxHeight);

extern SM64_LIB_FN uint32_t sm64_surface_object_create( const struct SM64SurfaceObject *surfaceObject );
extern SM64_LIB_FN void sm64_surface_object_move( uint32_t objectId, const struct SM64ObjectTransform *transform );
extern SM64_LIB_FN void sm64_surface_object_delete( uint32_t objectId );

extern SM64_LIB_FN int32_t sm64_surface_find_wall_collision( float *xPtr, float *yPtr, float *zPtr, float offsetY, float radius );
extern SM64_LIB_FN int32_t sm64_surface_find_wall_collisions( struct SM64WallCollisionData *colData );
extern SM64_LIB_FN float sm64_surface_find_ceil( float posX, float posY, float posZ, struct SM64SurfaceCollisionData **pceil );
extern SM64_LIB_FN float sm64_surface_find_floor_height_and_data( float xPos, float yPos, float zPos, struct SM64FloorCollisionData **floorGeo );
extern SM64_LIB_FN float sm64_surface_find_floor_height( float x, float y, float z );
extern SM64_LIB_FN float sm64_surface_find_floor( float xPos, float yPos, float zPos, struct SM64SurfaceCollisionData **pfloor );
extern SM64_LIB_FN float sm64_surface_find_water_level( float x, float z );
extern SM64_LIB_FN float sm64_surface_find_poison_gas_level( float x, float z );

extern SM64_LIB_FN void sm64_seq_player_play_sequence(uint8_t player, uint8_t seqId, uint16_t arg2);
extern SM64_LIB_FN void sm64_play_music(uint8_t player, uint16_t seqArgs, uint16_t fadeTimer);
extern SM64_LIB_FN void sm64_stop_background_music(uint16_t seqId);
extern SM64_LIB_FN void sm64_fadeout_background_music(uint16_t arg0, uint16_t fadeOut);
extern SM64_LIB_FN uint16_t sm64_get_current_background_music();
extern SM64_LIB_FN void sm64_play_sound(int32_t soundBits, float *pos);
extern SM64_LIB_FN void sm64_play_sound_global(int32_t soundBits);
extern SM64_LIB_FN void sm64_set_sound_volume(float vol);
extern SM64_LIB_FN void sm64_seq_channel_fade(uint8_t player, uint8_t channel, uint8_t volScale, uint16_t fadeDuration);
extern SM64_LIB_FN void sm64_seq_channel_mute(uint8_t player, uint16_t mask);

// ---- the object engine (objects.c): SM64's enemies, bosses, items and the rest, driven by their own behaviours
// The camera, in SM64 units: where it is and which way it looks, before each tick.  Billboards face it and the
// sounds are placed by it.
extern SM64_LIB_FN void sm64_set_camera( float x, float y, float z, float lookX, float lookY, float lookZ, float upX, float upY, float upZ );
// Where the objects' triangles of the next tick go (may be NULL: then only Mario is drawn).
extern SM64_LIB_FN void sm64_set_object_geometry( struct SM64ObjectGeometryBuffers *outBuffers );
// The spawn menu's entries.
extern SM64_LIB_FN int32_t sm64_menu_count( void );
extern SM64_LIB_FN const char *sm64_menu_name( int32_t entry );
extern SM64_LIB_FN const char *sm64_menu_category( int32_t entry );
// One of them, at a place (SM64 units) facing a way (radians, SM64's yaw); the pool slot, or -1.
extern SM64_LIB_FN int32_t sm64_object_spawn( int32_t entry, float x, float y, float z, float yaw );
extern SM64_LIB_FN void sm64_objects_clear( void );
extern SM64_LIB_FN int32_t sm64_objects_count( void );
// The level's origin moved under everything by this much (the host recentred its window): move the objects with it.
extern SM64_LIB_FN void sm64_objects_shift( float dx, float dy, float dz );
// The actors' pictures, decoded from the ROM at sm64_global_init: how many, each one's size and RGBA pixels.
extern SM64_LIB_FN int32_t sm64_texture_count( void );
extern SM64_LIB_FN int32_t sm64_texture_size( int32_t index, int32_t *w, int32_t *h );
extern SM64_LIB_FN const uint8_t *sm64_texture_rgba( int32_t index );
// One object's place and whether it is still there, by pool slot.
extern SM64_LIB_FN int32_t sm64_object_info( int32_t slot, float *pos, int32_t *entryOrMinusOne );

// the diagnostic switches (memory.h has them): 1 guard pages after every pool block, 2 the heaps validated at
// each phase of a tick, 4 and before every object's behaviour.  Set before sm64_global_init for the pages.
extern SM64_LIB_FN void sm64_set_debug( uint32_t flags );

// The text box an object asked for (an NPC talking, a boss's opening line, a sign): how many frames it has left
// (0: none) and which SM64 dialog it was.  The host draws it with words of its own, and can close it early.
extern SM64_LIB_FN int32_t sm64_dialog_state( int32_t *dialogID );
extern SM64_LIB_FN void sm64_dialog_close( void );

// what Mario has collected: his coins (SM64's count) and the stars caught this session
extern SM64_LIB_FN void sm64_hud_counts( int32_t *coins, int32_t *stars );
// where the enemies and bosses are (x y z triples, SM64's units, at most max of them): how many were written
extern SM64_LIB_FN int32_t sm64_threats( float *out, int32_t max );
// the level's people, as the enemies' prey: where they stand before a step (SM64 units, at most 16), and after
// it which of them (indices into that list) an enemy touched
extern SM64_LIB_FN void sm64_prey_set( const float *pos, int32_t n );
extern SM64_LIB_FN int32_t sm64_prey_hits( int32_t *out, int32_t max );

#ifdef __cplusplus
}
#endif

#endif//LIB_SM64_H
