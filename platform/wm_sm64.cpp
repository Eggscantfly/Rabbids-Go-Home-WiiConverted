// wm_sm64.cpp - Super Mario 64's Mario in the game, through libsm64 ([sm64] in wiimote.ini).
//
// What: libsm64 (https://github.com/libsm64/libsm64, the SM64 decompilation's Mario code as a library) runs Mario's
// movement, collision and animation on a set of collision surfaces and hands back his position and the triangles of
// his model every 30 Hz tick.  This module loads it (sm64.dll next to the executable, else in the mod's own folder;
// built by platform\sm64\build.bat),
// gives it the player's own SM64 ROM (the library reads Mario's texture and animations from it: the ROM is never
// shipped), the collision of the loaded mod level (mods\<mod>\sm64\surfaces.bin, written by the level builder in SM64
// units with the transform to the game's units) and draws Mario into the 3D image right after the engine's own
// geometry (before the after effects, so he is graded and depth-sorted like the level).
//
// Scripts drive him: the script natives SM64_* (registered in the engine's native table at attach) create Mario, tick
// him with the game's input and camera, and read his place back, so a level's own script (a model compiled for the
// engine's script VM) is what moves Mario and the game's actor around him.  Without a script, nothing ticks.
//
// Units: SM64 is Y-up; the game Z-up.  game = (x * scale + at.x, -z * scale + at.y, y * scale + at.z) as the level
// builder places the geometry; the inverse takes the game's points to SM64.
#include "wiimote.h"

#include <d3d9.h>
#include <mmsystem.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include <algorithm>
#include <malloc.h>
#include <string>
#include <vector>

using namespace wmpatch;

// ------------------------------------------------------------------------------------------------ libsm64's API
struct SM64Surface { int16_t type; int16_t force; uint16_t terrain; int32_t vertices[3][3]; };
struct SM64MarioInputs { float camLookX, camLookZ; float stickX, stickY; uint8_t buttonA, buttonB, buttonZ; };
struct SM64MarioState {
    float position[3]; float velocity[3]; float faceAngle; float forwardVelocity; int16_t health; uint32_t action;
    int32_t animID; int16_t animFrame; uint32_t flags; uint32_t particleFlags; int16_t invincTimer;
};
struct SM64MarioGeometryBuffers { float* position; float* normal; float* color; float* uv; uint16_t numTrianglesUsed; };
enum { SM64_TEXTURE_WIDTH = 64 * 11, SM64_TEXTURE_HEIGHT = 64, SM64_GEO_MAX_TRIANGLES = 1024 };

typedef void (*sm64_global_init_t)(const uint8_t* rom, uint8_t* outTexture);
typedef void (*sm64_global_terminate_t)();
typedef void (*sm64_static_surfaces_load_t)(const SM64Surface* surfaces, uint32_t n);
typedef int32_t (*sm64_mario_create_t)(float x, float y, float z);
typedef void (*sm64_mario_tick_t)(int32_t id, const SM64MarioInputs* in, SM64MarioState* out, SM64MarioGeometryBuffers* buf);
typedef void (*sm64_mario_delete_t)(int32_t id);
typedef void (*sm64_set_mario_position_t)(int32_t id, float x, float y, float z);
typedef void (*sm64_set_mario_faceangle_t)(int32_t id, float y);
typedef void (*sm64_set_mario_velocity_t)(int32_t id, float x, float y, float z);
typedef void (*sm64_set_mario_action_t)(int32_t id, uint32_t action);
typedef void (*sm64_mario_take_damage_t)(int32_t id, uint32_t damage, uint32_t subtype, float x, float y, float z);
typedef void (*sm64_set_mario_water_level_t)(int32_t id, signed int level);
typedef void (*sm64_register_debug_print_function_t)(void (*fn)(const char*));
typedef void (*sm64_audio_init_t)(const uint8_t* rom);
typedef uint32_t (*sm64_audio_tick_t)(uint32_t queued, uint32_t desired, int16_t* out);
typedef void (*sm64_play_music_t)(uint8_t player, uint16_t seqArgs, uint16_t fadeTimer);
typedef void (*sm64_stop_background_music_t)(uint16_t seqId);
typedef void (*sm64_set_sound_volume_t)(float vol);
typedef void (*sm64_play_sound_global_t)(int32_t soundBits);
typedef void (*sm64_seq_channel_fade_t)(uint8_t player, uint8_t channel, uint8_t volScale, uint16_t fade);
typedef void (*sm64_seq_channel_mute_t)(uint8_t player, uint16_t mask);
typedef void (*sm64_mario_interact_cap_t)(int32_t id, uint32_t capFlag, uint16_t capTime, uint8_t playMusic);
typedef void (*sm64_set_mario_state_t)(int32_t id, uint32_t flags);
typedef void (*sm64_set_mario_health_t)(int32_t id, uint16_t health);
typedef void (*sm64_set_mario_forward_velocity_t)(int32_t id, float vel);
typedef void (*sm64_set_mario_anim_frame_t)(int32_t id, int16_t frame);
// libsm64's own moving surfaces: an object's triangles in its own space, placed by a transform of a position and
// three angles.  The transform is left at nothing here and the triangles handed over already placed - the engine
// gives an object's axes as a matrix, and the angles libsm64 wants out of one are its own order and sign to get
// wrong - and the object is simply made again whenever it has moved.
struct SM64ObjectTransform { float position[3]; float eulerRotation[3]; };
struct SM64SurfaceObject { SM64ObjectTransform transform; uint32_t surfaceCount; SM64Surface* surfaces; };
typedef uint32_t (*sm64_surface_object_create_t)(const SM64SurfaceObject* object);
typedef void (*sm64_surface_object_delete_t)(uint32_t id);
// libsm64's object engine (this build's own addition, platform\sm64\import-objects.py): SM64's enemies, bosses and
// items, each run by its own behaviour script inside Mario's tick, drawn from a second set of triangles with the
// actors' own pictures out of the ROM.  Every triangle names its picture, its drawing layer (5 and above are
// see-through), which object it belongs to, and how it is put together (the SM64_TRI_* bits).
struct SM64ObjectGeometryBuffers {
    float* position; float* normal; float* color; float* uv;    // 9, 9, 12 (r g b a per corner), 6 per triangle
    uint16_t* texture; uint8_t* flags; uint8_t* layer; uint16_t* object;   // 1 per triangle
    uint32_t capacity; uint32_t numTrianglesUsed;
};
enum { SM64_TRI_TEXTURED = 1, SM64_TRI_TEX_ALPHA = 2, SM64_TRI_BLEND = 4, SM64_TRI_LIT = 8,
       SM64_TRI_CLAMP_S = 16, SM64_TRI_CLAMP_T = 32, SM64_TRI_MIRROR_S = 64, SM64_TRI_MIRROR_T = 128 };
typedef void (*sm64_set_camera_t)(float, float, float, float, float, float, float, float, float);
typedef void (*sm64_set_object_geometry_t)(SM64ObjectGeometryBuffers* out);
typedef int32_t (*sm64_menu_count_t)();
typedef const char* (*sm64_menu_name_t)(int32_t entry);
typedef const char* (*sm64_menu_category_t)(int32_t entry);
typedef int32_t (*sm64_object_spawn_t)(int32_t entry, float x, float y, float z, float yaw);
typedef void (*sm64_objects_clear_t)();
typedef int32_t (*sm64_objects_count_t)();
typedef void (*sm64_objects_shift_t)(float dx, float dy, float dz);
typedef void (*sm64_set_debug_t)(uint32_t flags);
typedef int32_t (*sm64_dialog_state_t)(int32_t* dialogID);
typedef void (*sm64_dialog_close_t)();
typedef void (*sm64_hud_counts_t)(int32_t* coins, int32_t* stars);
typedef int32_t (*sm64_threats_t)(float* out, int32_t max);
typedef void (*sm64_prey_set_t)(const float* pos, int32_t n);
typedef int32_t (*sm64_prey_hits_t)(int32_t* out, int32_t max);
typedef int32_t (*sm64_texture_count_t)();
typedef int32_t (*sm64_texture_size_t)(int32_t index, int32_t* w, int32_t* h);
typedef const uint8_t* (*sm64_texture_rgba_t)(int32_t index);

// SM64's own action words and the one animation this module has to know (sm64.h, mario_animation_ids.h)
const uint32_t ACT_TRIPLE_JUMP = 0x01000882u;    // the front flip, which lands as ACT_TRIPLE_JUMP_LAND by itself
const uint32_t ACT_FREEFALL = 0x0100088Cu;
const int32_t MARIO_ANIM_TRIPLE_JUMP = 0xC1;

namespace {

const char* MOD_NAME = "It's a me!";     // the mod whose folder holds Mario's settings and his ROM

bool s_enabled;
bool s_log;
uint32_t s_debugFlags;                           // debug= in config.ini: 1 guard pages, 2 heap checks, 4 per object
bool s_inAttach;
std::string s_romPath;
std::vector<std::string> s_pending;
HMODULE s_lib;
sm64_global_init_t p_global_init;
sm64_global_terminate_t p_global_terminate;
sm64_static_surfaces_load_t p_surfaces_load;
sm64_mario_create_t p_mario_create;
sm64_mario_tick_t p_mario_tick;
sm64_mario_delete_t p_mario_delete;
sm64_set_mario_position_t p_set_position;
sm64_set_mario_faceangle_t p_set_faceangle;
sm64_set_mario_velocity_t p_set_velocity;
sm64_set_mario_action_t p_set_action;
sm64_mario_take_damage_t p_take_damage;
sm64_set_mario_water_level_t p_set_water_level;
sm64_register_debug_print_function_t p_register_debug_print;
sm64_audio_init_t p_audio_init;
sm64_audio_tick_t p_audio_tick;
sm64_play_music_t p_play_music;
sm64_stop_background_music_t p_stop_music;
sm64_set_sound_volume_t p_set_sound_volume;
sm64_play_sound_global_t p_play_sound_global;
sm64_seq_channel_fade_t p_channel_fade;
sm64_seq_channel_mute_t p_channel_mute;
sm64_mario_interact_cap_t p_interact_cap;
sm64_set_mario_state_t p_set_mario_state;
sm64_set_mario_health_t p_set_mario_health;
sm64_set_mario_forward_velocity_t p_set_forward_velocity;
sm64_set_mario_anim_frame_t p_set_anim_frame;
sm64_surface_object_create_t p_surface_object_create;
sm64_surface_object_delete_t p_surface_object_delete;
sm64_set_camera_t p_set_camera;
sm64_set_object_geometry_t p_set_object_geometry;
sm64_menu_count_t p_menu_count;
sm64_menu_name_t p_menu_name;
sm64_menu_category_t p_menu_category;
sm64_object_spawn_t p_object_spawn;
sm64_objects_clear_t p_objects_clear;
sm64_objects_count_t p_objects_count;
sm64_objects_shift_t p_objects_shift;
sm64_set_debug_t p_set_debug;
sm64_dialog_state_t p_dialog_state;
sm64_dialog_close_t p_dialog_close;
sm64_hud_counts_t p_hud_counts;
sm64_threats_t p_threats;
sm64_prey_set_t p_prey_set;
sm64_prey_hits_t p_prey_hits;
sm64_texture_count_t p_texture_count;
sm64_texture_size_t p_texture_size;
sm64_texture_rgba_t p_texture_rgba;
bool ObjectsAvailable() {                        // this sm64.dll carries the object engine
    return p_set_camera && p_set_object_geometry && p_menu_count && p_menu_name && p_menu_category && p_object_spawn &&
           p_objects_clear && p_objects_count && p_objects_shift && p_texture_count && p_texture_size && p_texture_rgba;
}
bool s_libReady;                                 // sm64.dll loaded and initialised with the ROM
uint8_t* s_texture;                              // Mario's texture atlas (RGBA, 704 x 64) from the ROM

void SmLog(const char* fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf(buf, sizeof(buf) - 1, fmt, ap);
    va_end(ap);
    buf[sizeof(buf) - 1] = 0;
    if (s_inAttach) s_pending.push_back(buf);
    else Log("SM64: %s", buf);
}

void DebugPrint(const char* s) { if (s_log || (s[0] == '!' && s[1] == '!')) SmLog("lib: %s", s); }   // "!!": always worth a line

// debug=2: the process heap validated at the two places the platform is done with the library for the moment
void HostHeapCheck(const char* where) {
    static bool reported;
    if (!(s_debugFlags & 2) || reported) return;
    if (HeapValidate(GetProcessHeap(), 0, NULL) && _heapchk() == _HEAPOK) return;
    reported = true;
    SmLog("!! the heap is corrupt: first seen by the platform %s", where);
}

// ------------------------------------------------------------------------------------------------ the level
struct Level {
    bool loaded;
    std::string file;
    float scale;
    float at[3];
    float spawn[3];                              // Mario's start, SM64 units
    std::vector<SM64Surface> surfaces;
};
Level s_level;

void ToGame(const float* sm, float* g) {         // SM64 (Y up) -> game (Z up)
    g[0] = sm[0] * s_level.scale + s_level.at[0];
    g[1] = -sm[2] * s_level.scale + s_level.at[1];
    g[2] = sm[1] * s_level.scale + s_level.at[2];
}
void ToSm64(const float* g, float* sm) {
    sm[0] = (g[0] - s_level.at[0]) / s_level.scale;
    sm[1] = (g[2] - s_level.at[2]) / s_level.scale;
    sm[2] = -(g[1] - s_level.at[1]) / s_level.scale;
}

uint32_t Be32(const uint8_t* p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }

bool ReadFileBytes(const std::string& path, std::vector<uint8_t>& out) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n <= 0) { fclose(f); return false; }
    out.resize((size_t)n);
    size_t got = fread(&out[0], 1, (size_t)n, f);
    fclose(f);
    return got == (size_t)n;
}

// mods\<mod>\sm64\surfaces.bin: "SM64", u32 version 1, f32 scale, f32 at[3], f32 spawn[3], u32 count, count x
// { i16 type, i16 force, u16 terrain, u16 pad, i32 v[3][3] }
bool LoadSurfaces(const std::string& path) {
    std::vector<uint8_t> d;
    if (!ReadFileBytes(path, d) || d.size() < 40 || memcmp(&d[0], "SM64", 4) != 0) return false;
    uint32_t ver, count;
    memcpy(&ver, &d[4], 4);
    if (ver != 1) return false;
    memcpy(&s_level.scale, &d[8], 4);
    memcpy(s_level.at, &d[12], 12);
    memcpy(s_level.spawn, &d[24], 12);
    memcpy(&count, &d[36], 4);
    if (d.size() < 40 + (size_t)count * 44) return false;
    s_level.surfaces.resize(count);
    for (uint32_t i = 0; i < count; ++i) {
        const uint8_t* p = &d[40 + i * 44];
        SM64Surface& s = s_level.surfaces[i];
        memcpy(&s.type, p, 2);
        memcpy(&s.force, p + 2, 2);
        memcpy(&s.terrain, p + 4, 2);
        memcpy(s.vertices, p + 8, 36);
    }
    s_level.file = path;
    s_level.loaded = true;
    return true;
}

bool FindLevelSurfaces() {                       // the first enabled mod with an sm64\surfaces.bin
    const std::vector<std::string>& mods = ModsEnabled();
    for (size_t i = 0; i < mods.size(); ++i) {
        std::string path = g_dllDir + "mods\\" + mods[i] + "\\sm64\\surfaces.bin";
        DWORD a = GetFileAttributesA(path.c_str());
        if (a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY)) {
            if (LoadSurfaces(path)) {
                SmLog("level: %s (%u surfaces, scale %.4f, at %.1f %.1f %.1f, Mario's start %.0f %.0f %.0f)",
                      path.c_str(), (unsigned)s_level.surfaces.size(), s_level.scale, s_level.at[0], s_level.at[1],
                      s_level.at[2], s_level.spawn[0], s_level.spawn[1], s_level.spawn[2]);
                return true;
            }
            SmLog("level: %s cannot be read", path.c_str());
        }
    }
    return false;
}

// ------------------------------------------------------------------------------------------------ Mario
struct Mario {
    int32_t id;
    SM64MarioState state;
    std::vector<float> position, normal, color, uv;
    uint16_t triangles;
    bool valid;                                  // ticked at least once (there is geometry to draw)
    SM64MarioInputs inputs;                      // what the next tick gets (set by the script natives)
    // He steps at 30 Hz and the game draws at whatever the window runs at, so what is drawn is the step before
    // blended into the step just taken - the same thing SM64 Coop DX does to its graph nodes (prev * (1 - delta)
    // + cur * delta, with the blend thrown away when the two frames are not continuous).  Without it he moves in
    // 30 Hz jumps while the world around him moves smoothly, and since the camera is hung off him the whole
    // picture lurches forward and settles once a step - worse the faster he goes.
    // The kept copy is in the game's units, not SM64's: the window his coordinates are measured from is rebuilt
    // as he walks, and a copy in SM64 units would be measured from the old middle.
    std::vector<float> prevGame;                 // last step's vertices, game units
    float prevRoot[3];                           // and where he stood, game units
    uint16_t prevTriangles;
    bool interp;                                 // the two are one step apart: the blend means something
};
Mario s_mario;
bool s_surfacesGiven;                            // libsm64 has the level's surfaces
bool s_hudOn;                                    // SM64_HudSet: Mario has the level, so his HUD is the one drawn
// E: the long jump cheat.  In SM64 a backwards long jump only builds speed where the ground lets him land again
// at once - a staircase.  With this on he is thrown back down the moment a long jump is going backwards, so any
// flat floor will do, and the star counter becomes what is worth watching while it is on: his speed.
// Q: the jump pressed again for you as fast as he can take it while it is held, so a long jump can be chained
// without hammering the spacebar.  It presses nothing on its own - the key still has to be down.
bool s_bljOn, s_rapidOn, s_eDown, s_qDown;
bool s_jumpHeld;                                 // the jump button as it really is, before Q takes it apart
uint32_t s_rapidPhase = 1;
double s_lastTick;                               // when the script last ticked him
double s_deathAt;                                // when his health reached zero (0: he is not dying)
bool s_modeOn;                                   // the level is Mario's: kept here so a world reinit cannot lose it
// Mario belongs to the level's script, and the script only runs while a level is being played: it stops when the
// game pauses for a Magma menu, when the level is left for a menu world of its own, and it is told to stop while a
// video is on the screen.  So a Mario nobody has ticked for a moment is a Mario nobody is playing, and neither he
// nor his HUD is drawn - before this he stayed over menus and over the mission videos.
bool Driven() { return s_mario.id >= 0 && (NowSeconds() - s_lastTick) < 0.35; }
double s_hudMoved;                               // when it last started sliding in or out
const double HUD_SLIDE = 0.35;                   // seconds it takes
int s_lives = 4;                                 // what the HUD shows: a death costs one
int s_coins;                                     // the rabbids riding in the cart
int s_stars;                                     // the XL items it has collected
int32_t s_sm64Coins, s_sm64Stars;                // and SM64's own: the coins he has picked up, the stars he has caught
const int THREAT_MAX = 8;                        // the enemies and bosses the game's creatures are told about
float s_threats[THREAT_MAX][3];
int s_threatCount;
// The level's humans, as the enemies' prey: each reports itself every frame (SM64_PreyReport, from its own
// recompiled reflex track); before a step the nearest go to libsm64, after it the ones an enemy touched are
// flagged, and the human's next report answers 1 - its track then takes the boosted cart's hit.
struct PreyReport { uint32_t obj; float g[3]; };
std::vector<PreyReport> s_preyReports;
uint32_t s_preyTick[16];
int s_preyTickN;
std::vector<uint32_t> s_preyHitPending;
std::vector<std::pair<uint32_t, double> > s_preyLastHit;
int s_deadTicks;                                 // ticks since his health ran out
double s_accum;                                  // seconds owed to the 30 Hz tick
// what has been moving him: his own step, or the script putting him somewhere.  Reported once a second, because
// the two are told apart by nothing else - a speed of nothing while the level goes past him is the script's doing
int s_moveN;
float s_moveSum, s_moveMax;
double s_moveAt;
double s_lastFrame;
uint32_t s_ticks;
// A flip the script asked for (SM64_MarioLaunch / SM64_MarioFlip) is kept turning until he lands: SM64's triple
// jump animation plays once and then holds its last frame, which is fine for a jump that lasts as long as the
// animation and wrong for a fall off a jump panel.  So while this is set the animation is wound back whenever it
// has stopped advancing, and it clears itself the moment the action is no longer the flip.
bool s_flipLoop;
int16_t s_flipFrame = -1;                        // the animation frame seen at the last step
// A launch (SM64_MarioLaunch) with speed along the ground keeps that speed for the whole flight: SM64 bleeds
// air speed above 32 units a step away, and a throw that is to land where the game's own throw lands cannot
// have that.  Put back every step while he is in the air, and let go the moment he is not.
bool s_launchHold;
float s_launchFwd;
// The wind of a fan (SM64_MarioWind), an acceleration in the game's units a second each second, applied at the
// next step and forgotten: the script hands it over every frame the fan is blowing.  The game's own gravity
// is 9.81 of those units and SM64's about 32, so the wind carries the difference with it: what floats the cart
// floats him.  While it blows he is in SM64's own wind action, the spread-eagle rise of the wing cap levels.
float s_wind[3];
bool s_windOn;

// ------------------------------------------------------------------------------------------------ the objects
// SM64's enemies, bosses and items, spawned from the T menu.  libsm64 runs their behaviours inside Mario's tick
// and hands their triangles back the way it hands back his, so they are kept and blended between steps like him
// (per object: an object whose triangle count changed between the two steps is drawn where it is now).
const uint32_t OBJ_TRI_CAP = 40000;
struct ObjGeo {
    std::vector<float> position, normal, color, uv;
    std::vector<uint16_t> texture, object;
    std::vector<uint8_t> flags, layer;
    SM64ObjectGeometryBuffers buf;
    uint32_t triangles;
    std::vector<float> prevGame;                 // last step's corners, game units
    std::vector<uint16_t> prevObject;            // and whose each triangle was
    uint32_t prevTriangles;
    bool interp;
};
ObjGeo s_obj;
bool s_objReady;                                 // the buffers exist and libsm64 writes into them
uint32_t s_lastView;                             // the view the scene was last drawn for
D3DMATRIX s_camView;                             // the view matrix Mario was last drawn with: the camera the objects get
bool s_camViewValid;
uint32_t s_camLogged;

void ObjectsAlloc() {
    if (!ObjectsAvailable()) return;
    if (!s_objReady) {
        s_obj.position.assign(OBJ_TRI_CAP * 9, 0.0f);
        s_obj.normal.assign(OBJ_TRI_CAP * 9, 0.0f);
        s_obj.color.assign(OBJ_TRI_CAP * 12, 0.0f);
        s_obj.uv.assign(OBJ_TRI_CAP * 6, 0.0f);
        s_obj.texture.assign(OBJ_TRI_CAP, 0xFFFF);
        s_obj.object.assign(OBJ_TRI_CAP, 0);
        s_obj.flags.assign(OBJ_TRI_CAP, 0);
        s_obj.layer.assign(OBJ_TRI_CAP, 0);
    }
    s_obj.buf.position = &s_obj.position[0];
    s_obj.buf.normal = &s_obj.normal[0];
    s_obj.buf.color = &s_obj.color[0];
    s_obj.buf.uv = &s_obj.uv[0];
    s_obj.buf.texture = &s_obj.texture[0];
    s_obj.buf.flags = &s_obj.flags[0];
    s_obj.buf.layer = &s_obj.layer[0];
    s_obj.buf.object = &s_obj.object[0];
    s_obj.buf.capacity = OBJ_TRI_CAP;
    s_obj.buf.numTrianglesUsed = 0;
    s_obj.triangles = 0;
    s_obj.prevTriangles = 0;
    s_obj.interp = false;
    p_set_object_geometry(&s_obj.buf);
    s_objReady = true;
}

void ObjectsForget() {                           // Mario is gone, and libsm64 took the objects with him
    s_threatCount = 0;
    s_sm64Coins = 0;
    s_preyReports.clear();
    s_preyHitPending.clear();
    s_preyTickN = 0;
    s_obj.triangles = 0;
    s_obj.prevTriangles = 0;
    s_obj.interp = false;
}

// The enemies and bosses out there, read after each step in the game's units: the SM64_Mario script keeps the
// mod's threat objects on them, so the game's own creatures, whose scripts fear the cart, fear them too.
void ThreatsRead() {
    s_threatCount = 0;
    if (!s_objReady || !p_threats || !Driven()) return;
    float sm[THREAT_MAX * 3];
    int n = (int)p_threats(sm, THREAT_MAX);
    if (n < 0) n = 0;
    if (n > THREAT_MAX) n = THREAT_MAX;
    for (int i = 0; i < n; ++i) ToGame(&sm[i * 3], s_threats[i]);
    s_threatCount = n;
}

// the step just gone, kept so the next one can be drawn out of the two of them (as Mario's is)
void ObjectsCapturePrev() {
    if (!s_objReady) return;
    s_obj.prevGame.resize((size_t)s_obj.triangles * 9);
    for (uint32_t i = 0; i < s_obj.triangles * 3; ++i) ToGame(&s_obj.position[i * 3], &s_obj.prevGame[i * 3]);
    s_obj.prevObject.assign(s_obj.object.begin(), s_obj.object.begin() + s_obj.triangles);
    s_obj.prevTriangles = s_obj.triangles;
    s_obj.interp = s_obj.triangles > 0;
}

// The engine's camera, given to libsm64 before each step: billboards (the coins, the flames, the Bob-ombs'
// bodies) turn to it, and the objects' sounds are placed by it.  It is the very view matrix Mario was last drawn
// with (SceneDraw keeps it), so the two can never disagree: its columns are the camera's axes in the world, so
// where the camera stands is the translation row taken back through them, and the picture space is Y down and Z
// into the picture.
void ObjectsCamera() {
    if (!s_objReady || !s_camViewValid) return;
    const D3DMATRIX& v = s_camView;
    float R[3][3] = { { v._11, v._12, v._13 }, { v._21, v._22, v._23 }, { v._31, v._32, v._33 } };
    float t[3] = { v._41, v._42, v._43 };
    float g[3], fwd[3], up[3];
    for (int i = 0; i < 3; ++i) {
        g[i] = -(t[0] * R[i][0] + t[1] * R[i][1] + t[2] * R[i][2]);
        fwd[i] = R[i][2];
        up[i] = -R[i][1];
    }
    float sm[3];
    ToSm64(g, sm);
    // a direction goes over to SM64's axes the way a point does, without the middle: x, z up, -y
    p_set_camera(sm[0], sm[1], sm[2], fwd[0], fwd[2], -fwd[1], up[0], up[2], -up[1]);
    if (s_log && (s_camLogged++ % 300) == 0) {
        float mg[3];
        ToGame(s_mario.state.position, mg);
        SmLog("camera for the objects: at %.1f %.1f %.1f looking %.2f %.2f %.2f up %.2f %.2f %.2f (Mario at %.1f %.1f %.1f)",
              g[0], g[1], g[2], fwd[0], fwd[1], fwd[2], up[0], up[1], up[2], mg[0], mg[1], mg[2]);
    }
}

bool EnsureSurfaces() {
    if (s_surfacesGiven) return true;
    if (!s_libReady) return false;
    if (!s_level.loaded && !FindLevelSurfaces()) return false;
    p_surfaces_load(s_level.surfaces.empty() ? NULL : &s_level.surfaces[0], (uint32_t)s_level.surfaces.size());
    s_surfacesGiven = true;
    SmLog("surfaces given to libsm64: %u", (unsigned)s_level.surfaces.size());
    return true;
}

// ------------------------------------------------------------------------------- the game's own levels' collision
// A level mod can carry the collision of one of the game's levels as mods\<mod>\sm64\<world>.rghc, written by
// level_collision.py out of the level's COB geometries: "RGHC", version, count, then three corners per triangle in
// the game's own units.  SM64 holds collision in 16-bit coordinates, so the level cannot be handed over whole:
// libsm64 is given the triangles within WINDOW of a centre, and that centre is where the two coordinate systems
// meet (s_level.at).  When Mario walks far enough from it the window is built again around him and he is moved to
// the same place in the new coordinates.
const float RGHC_SCALE = 0.009f;                 // the game's units per SM64 unit, as the castle level uses
const float WINDOW = 260.0f;                     // game units either way: 260 / 0.009 = 28889, inside SM64's 32767
const float RECENTRE = 30.0f;                    // how far he may walk from the centre before it is rebuilt
                                                 // (well inside BED_RADIUS, so a swimmer keeps his bed)

struct CollisionSet {
    std::string name;
    uint32_t world;                              // the world its file is named after, 0 if the name is not a key
    std::vector<float> tris;                     // 9 floats a triangle, the game's units
    float lo[3], hi[3];
};
std::vector<CollisionSet> s_sets;
bool s_setsScanned;
std::vector<int> s_curSets;                      // the sets the window is being built from
float s_centre[3];                               // the window's centre, game units
bool s_windowed;                                 // libsm64 is holding a window of a level rather than a whole one
uint32_t s_windowGen;                            // counts the windows built, so what was placed in an old one knows

// ------------------------------------------------------------------- what moves in a level: <world>.rghd
// A barrier its script lowers, a door that swings, a lift on its way: their collision is not the level's ground,
// it is the object's, and the object is where the engine has it.  level_collision.py writes those objects'
// triangles in the object's own space, with the object's key and which of its collision modifiers they are, and
// every frame this finds the object as the engine holds it (the lookup the scripts' SCR_ObjByKeyGet makes), asks
// it for its axes and position the way the script natives do, checks that both the object and that collision
// are switched on, and gives libsm64 the triangles placed there - made again only when the object has moved.
struct DynObj {
    uint32_t world, key, rank;
    std::vector<float> tris;                     // 9 floats a triangle, the object's own space, game units
    float lo[3], hi[3];                          // their bounds in that space
    std::vector<SM64Surface> surf;               // as last handed to libsm64 (it reads them again on its own)
    uint32_t libId;                              // libsm64's number for it, or none
    uint32_t gen;                                // the window it was placed in
    float placed[12];                            // the axes and position it was placed with
    bool faulted;                                // the engine faulted reading it: left alone until it is gone
    bool odd;                                    // its axes came back as nonsense once (said once)
};
const uint32_t NO_OBJECT = 0xFFFFFFFFu;
std::vector<DynObj> s_dyn;
std::vector<uint32_t> s_dynWorlds;               // the worlds that came with live collision
int s_dynPlaced;                                 // how many are with libsm64 right now
bool s_engineObjOk;                              // the engine's object routines are where this expects them
bool DynWorld(uint32_t world) {                  // this world's collision is placed live, not from the bake
    if (!s_engineObjOk) return false;
    for (size_t i = 0; i < s_dynWorlds.size(); ++i)
        if (s_dynWorlds[i] == world) return true;
    return false;
}
// the engine's own routines, verified by checksum before they are believed: an object by key, and an object's
// x axis, y axis, z axis and position (these validate the hierarchy first, so a child of a moving thing is right)
const uint32_t LOA_KEY_SEARCH = 0x006D81A0;      // LOA_pt_KeySearchRealAddress(key, 0)
const uint32_t OBJ_HORIZON_GET = 0x0053C500;     // OBJ_HorizonGet_C(obj, out)
const uint32_t OBJ_SIGHT_GET = 0x0053C5E0;       // OBJ_SightGet_C
const uint32_t OBJ_BANKING_GET = 0x0053C6C0;     // OBJ_BankingGet_C
const uint32_t OBJ_POS_GET = 0x0053BB80;         // OBJ_PosGet_C
typedef void* (__cdecl* KeySearchFn)(uint32_t key, uint32_t zero);
typedef void (__cdecl* ObjVecFn)(void* obj, float* out);

bool ReadRghd(const std::string& path, uint32_t world) {
    std::vector<uint8_t> d;
    if (!ReadFileBytes(path, d) || d.size() < 12 || memcmp(&d[0], "RGHD", 4) != 0) return false;
    uint32_t ver, n;
    memcpy(&ver, &d[4], 4);
    memcpy(&n, &d[8], 4);
    if (ver != 1) return false;
    size_t at = 12;
    int made = 0;
    for (uint32_t i = 0; i < n; ++i) {
        if (at + 12 > d.size()) return false;
        DynObj o;
        uint32_t tris;
        memcpy(&o.key, &d[at], 4);
        memcpy(&o.rank, &d[at + 4], 4);
        memcpy(&tris, &d[at + 8], 4);
        at += 12;
        if (at + (size_t)tris * 36 > d.size()) return false;
        o.world = world;
        o.tris.resize((size_t)tris * 9);
        if (tris) memcpy(&o.tris[0], &d[at], (size_t)tris * 36);
        at += (size_t)tris * 36;
        o.lo[0] = o.lo[1] = o.lo[2] = 1e30f;
        o.hi[0] = o.hi[1] = o.hi[2] = -1e30f;
        for (size_t k = 0; k < o.tris.size(); k += 3)
            for (int c = 0; c < 3; ++c) {
                if (o.tris[k + c] < o.lo[c]) o.lo[c] = o.tris[k + c];
                if (o.tris[k + c] > o.hi[c]) o.hi[c] = o.tris[k + c];
            }
        o.libId = NO_OBJECT;
        o.gen = 0;
        memset(o.placed, 0, sizeof(o.placed));
        o.faulted = o.odd = false;
        if (tris) { s_dyn.push_back(o); ++made; }
    }
    if (made) s_dynWorlds.push_back(world);
    return made > 0;
}

void ScanDynamicDir(const std::string& dir) {
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA((dir + "*.rghd").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        uint32_t world = (uint32_t)strtoul(fd.cFileName, NULL, 16);
        bool already = false;                    // a world given its movables twice keeps the first
        for (size_t k = 0; k < s_dyn.size() && !already; ++k)
            if (s_dyn[k].world == world) already = true;
        if (already) continue;
        size_t was = s_dyn.size();
        if (!ReadRghd(dir + fd.cFileName, world)) { SmLog("%s cannot be read", fd.cFileName); continue; }
        size_t tris = 0;
        for (size_t k = was; k < s_dyn.size(); ++k) tris += s_dyn[k].tris.size() / 9;
        SmLog("movable collision %s: %u objects, %u triangles", fd.cFileName, (unsigned)(s_dyn.size() - was), (unsigned)tris);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
}

void DynDrop(DynObj& o) {
    if (o.libId != NO_OBJECT && p_surface_object_delete) { p_surface_object_delete(o.libId); --s_dynPlaced; }
    o.libId = NO_OBJECT;
}

void DynClear() {
    for (size_t i = 0; i < s_dyn.size(); ++i) DynDrop(s_dyn[i]);
}

// the collision modifier of that rank on a live object, and whether the object and it are switched on
bool DynObjectOn(void* obj, uint32_t rank) {
    const uint8_t* o = (const uint8_t*)obj;
    if (!(*(const uint32_t*)(o + 0x14) & 8)) return false;          // the object is not applied
    uint32_t data = *(const uint32_t*)(o + 0x2C), stride = *(const uint32_t*)(o + 0x30), n = *(const uint32_t*)(o + 0x38);
    if (!data || stride < 4 || stride > 64 || n > 256) return false;
    uint32_t seen = 0;
    for (uint32_t i = 0; i < n; ++i) {
        const uint8_t* mdf = *(const uint8_t* const*)(uintptr_t)(data + stride * i);
        if (!mdf) continue;
        if (*(const uint16_t*)(mdf + 0x0A) != 20) continue;         // not a COB
        if (seen++ == rank) return (*(const uint32_t*)(mdf + 4) & 2) != 0;   // MDF_ControlApplySet's bit
    }
    return false;
}

// One object as the engine has it now: whether it is there at all, whether it and its collision are switched on,
// and if so (and asked) its axes and position.  The engine's routines run under a fault guard: the axis getters
// validate the object's hierarchy first, and an object the engine is part-way through building or tearing down
// - its hierarchy link or its matrix block not a pointer yet - faults inside that validation.  Such an object
// costs itself its collision, not the game its life.  False on a fault; the object is then to be left alone
// until the engine no longer has it.  (No C++ objects in here: __try does not allow them.)
bool DynProbe(uint32_t key, uint32_t rank, bool axes, void** objOut, bool* onOut, float m[12]) {
    *objOut = NULL;
    *onOut = false;
    __try {
        void* obj = ((KeySearchFn)(uintptr_t)LOA_KEY_SEARCH)(key, 0);
        *objOut = obj;
        if (!obj || !DynObjectOn(obj, rank)) return true;
        *onOut = true;
        if (!axes) return true;
        ((ObjVecFn)(uintptr_t)OBJ_HORIZON_GET)(obj, m);
        ((ObjVecFn)(uintptr_t)OBJ_SIGHT_GET)(obj, m + 3);
        ((ObjVecFn)(uintptr_t)OBJ_BANKING_GET)(obj, m + 6);
        ((ObjVecFn)(uintptr_t)OBJ_POS_GET)(obj, m + 9);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// axes and a position that could be an object's: finite, the axes of some length, the position in the level
bool DynSane(const float m[12]) {
    for (int k = 0; k < 12; ++k)
        if (!(m[k] == m[k]) || fabsf(m[k]) > 1.0e6f) return false;
    for (int a = 0; a < 3; ++a) {
        float l = m[a * 3] * m[a * 3] + m[a * 3 + 1] * m[a * 3 + 1] + m[a * 3 + 2] * m[a * 3 + 2];
        if (l < 1.0e-6f || l > 1.0e6f) return false;
    }
    return true;
}

// The world's live collision, placed where the engine has it now.  Run every frame, and at once whenever the
// window is built again - the objects are measured from its middle, and a step taken against the old ones
// would find the level thirty units from where it is.
void DynPlace() {
    if (!s_libReady || !s_engineObjOk || !p_surface_object_create || !p_surface_object_delete || !s_windowed) return;
    int seen = 0, found = 0, off = 0, beyond = 0;
    for (size_t i = 0; i < s_dyn.size(); ++i) {
        DynObj& d = s_dyn[i];
        bool inWorld = ModsWorldSeen(d.world);
        if (inWorld) ++seen;
        void* obj = NULL;
        bool on = false;
        float m[12];
        if (inWorld && !DynProbe(d.key, d.rank, !d.faulted, &obj, &on, m)) {
            if (!d.faulted)
                SmLog("live %08X of %08X (collision %u): the engine faulted reading it (an object being built or torn down): "
                      "left out until it is gone and back", d.key, d.world, d.rank);
            d.faulted = true;
            obj = NULL;
            on = false;
        }
        if (!obj) d.faulted = false;             // gone: the next one under this key is another object
        if (d.faulted) on = false;               // there, but not to be read
        if (on && !DynSane(m)) {
            if (!d.odd) SmLog("live %08X of %08X (collision %u): its axes are nonsense (%.3g %.3g %.3g / %.3g %.3g %.3g / %.3g %.3g %.3g at %.3g %.3g %.3g): not placed",
                              d.key, d.world, d.rank, m[0], m[1], m[2], m[3], m[4], m[5], m[6], m[7], m[8], m[9], m[10], m[11]);
            d.odd = true;
            on = false;
        }
        if (obj) ++found;
        if (obj && !on) ++off;
        if (on) {
            // beyond the window: not where he can get to before it moves.  By the object's extent, not its
            // place - a level's floor is one object whose origin can be anywhere
            float lo[3] = { 1e30f, 1e30f, 1e30f }, hi[3] = { -1e30f, -1e30f, -1e30f };
            for (int corner = 0; corner < 8; ++corner) {
                float p[3] = { (corner & 1) ? d.hi[0] : d.lo[0], (corner & 2) ? d.hi[1] : d.lo[1], (corner & 4) ? d.hi[2] : d.lo[2] };
                for (int c = 0; c < 3; ++c) {
                    float g = p[0] * m[c] + p[1] * m[3 + c] + p[2] * m[6 + c] + m[9 + c];
                    if (g < lo[c]) lo[c] = g;
                    if (g > hi[c]) hi[c] = g;
                }
            }
            if (lo[0] > s_centre[0] + WINDOW || hi[0] < s_centre[0] - WINDOW || lo[1] > s_centre[1] + WINDOW ||
                hi[1] < s_centre[1] - WINDOW || lo[2] > s_centre[2] + WINDOW || hi[2] < s_centre[2] - WINDOW) {
                on = false;
                ++beyond;
            }
        }
        if (!on) { DynDrop(d); continue; }
        bool same = d.libId != NO_OBJECT && d.gen == s_windowGen;
        for (int k = 0; k < 12 && same; ++k)
            if (fabsf(m[k] - d.placed[k]) > 0.0005f) same = false;
        if (same) continue;
        std::vector<SM64Surface> surf;
        surf.reserve(d.tris.size() / 9);
        for (size_t t = 0; t + 8 < d.tris.size(); t += 9) {
            SM64Surface s;
            memset(&s, 0, sizeof(s));
            bool ok = true;
            for (int v = 0; v < 3 && ok; ++v) {
                const float* p = &d.tris[t + v * 3];
                float g[3], sm[3];
                for (int c = 0; c < 3; ++c) g[c] = p[0] * m[c] + p[1] * m[3 + c] + p[2] * m[6 + c] + m[9 + c];
                ToSm64(g, sm);
                for (int c = 0; c < 3; ++c) {
                    if (sm[c] < -32000.0f || sm[c] > 32000.0f) ok = false;
                    s.vertices[v][c] = (int32_t)sm[c];
                }
            }
            if (ok) surf.push_back(s);
        }
        bool first = d.libId == NO_OBJECT;
        DynDrop(d);
        d.surf.swap(surf);
        if (!d.surf.empty()) {
            SM64SurfaceObject so;
            memset(&so, 0, sizeof(so));
            so.surfaceCount = (uint32_t)d.surf.size();
            so.surfaces = &d.surf[0];
            d.libId = p_surface_object_create(&so);
            ++s_dynPlaced;
            if (first && s_log)
                SmLog("live %08X of %08X (collision %u): %u triangles placed at %.1f %.1f %.1f", d.key, d.world,
                      d.rank, (unsigned)d.surf.size(), m[9], m[10], m[11]);
        }
        d.gen = s_windowGen;
        memcpy(d.placed, m, sizeof(m));
    }
    // what the live collision is doing, once a second and only when it changes
    static double next;
    static int wasSeen = -1, wasFound, wasOff, wasFar, wasPlaced;
    double now = NowSeconds();
    if (now >= next && (seen != wasSeen || found != wasFound || off != wasOff || beyond != wasFar || s_dynPlaced != wasPlaced)) {
        next = now + 1.0;
        wasSeen = seen; wasFound = found; wasOff = off; wasFar = beyond; wasPlaced = s_dynPlaced;
        SmLog("live collision: %d objects of this level's worlds, %d found, %d switched off, %d beyond the window, %d placed",
              seen, found, off, beyond, s_dynPlaced);
    }
}

void DynFrame() {
    if (s_mario.id < 0 || !s_windowed) { if (s_dynPlaced) DynClear(); return; }
    DynPlace();
}

bool ReadRghc(const std::string& path, CollisionSet& out) {
    std::vector<uint8_t> d;
    if (!ReadFileBytes(path, d) || d.size() < 12 || memcmp(&d[0], "RGHC", 4) != 0) return false;
    uint32_t ver, n;
    memcpy(&ver, &d[4], 4);
    memcpy(&n, &d[8], 4);
    if (ver != 1 || d.size() < 12 + (size_t)n * 36) return false;
    out.tris.resize((size_t)n * 9);
    if (n) memcpy(&out.tris[0], &d[12], (size_t)n * 36);
    out.lo[0] = out.lo[1] = out.lo[2] = 1e30f;
    out.hi[0] = out.hi[1] = out.hi[2] = -1e30f;
    for (size_t i = 0; i < out.tris.size(); i += 3)
        for (int c = 0; c < 3; ++c) {
            float v = out.tris[i + c];
            if (v < out.lo[c]) out.lo[c] = v;
            if (v > out.hi[c]) out.hi[c] = v;
        }
    return n > 0;
}

// The collision of a world belongs to the world, not to Mario: any mod that makes one carries its own, in its
// `collision` folder, named after the world key (level_collision.py --mod writes them).  So a custom map ships its
// ground with it and whatever is standing on it - Mario today, anything later - finds it without being rebuilt.
// `sm64` is the old name for the same thing and is still read, because this mod's own 92 files live there.
void ScanCollisionDir(const std::string& dir) {
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA((dir + "*.rghc").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        CollisionSet set;
        set.name = fd.cFileName;
        set.world = (uint32_t)strtoul(set.name.c_str(), NULL, 16);
        bool already = false;                    // a world given collision twice keeps the first
        for (size_t k = 0; k < s_sets.size() && !already; ++k)
            if (s_sets[k].world && s_sets[k].world == set.world) already = true;
        if (already) continue;
        if (!ReadRghc(dir + fd.cFileName, set)) { SmLog("%s cannot be read", fd.cFileName); continue; }
        SmLog("level collision %s: %u triangles, x %.0f..%.0f y %.0f..%.0f z %.0f..%.0f", set.name.c_str(),
              (unsigned)(set.tris.size() / 9), set.lo[0], set.hi[0], set.lo[1], set.hi[1], set.lo[2], set.hi[2]);
        s_sets.push_back(set);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
}

void ScanCollisionSets() {                       // every enabled mod's collision, once
    if (s_setsScanned) return;
    s_setsScanned = true;
    const std::vector<std::string>& mods = ModsEnabled();
    for (size_t i = 0; i < mods.size(); ++i) {
        ScanCollisionDir(g_dllDir + "mods\\" + mods[i] + "\\collision\\");
        ScanCollisionDir(g_dllDir + "mods\\" + mods[i] + "\\sm64\\");
        ScanDynamicDir(g_dllDir + "mods\\" + mods[i] + "\\collision\\");
        ScanDynamicDir(g_dllDir + "mods\\" + mods[i] + "\\sm64\\");
    }
}

// The collision of the level being played: its file is named after the world it came from, and the loader knows
// which worlds the engine has asked for.  A level is several worlds and its collision is split between them - the
// ground he stands on and the walls around him are rarely the same one - so every world of the level that reaches
// this place contributes, not just the first.  Two of the game's levels can stand at the same coordinates, so the
// world is what tells them apart; where Mario is only settles it if nothing else does.
void SetsForPoint(const float* g, std::vector<int>& out) {
    ScanCollisionSets();
    out.clear();
    std::vector<int> loose;
    for (size_t i = 0; i < s_sets.size(); ++i) {
        const CollisionSet& s = s_sets[i];
        bool inside = g[0] >= s.lo[0] - WINDOW && g[0] <= s.hi[0] + WINDOW && g[1] >= s.lo[1] - WINDOW &&
                      g[1] <= s.hi[1] + WINDOW && g[2] >= s.lo[2] - WINDOW && g[2] <= s.hi[2] + WINDOW;
        if (!inside) continue;
        if (s.world && ModsWorldSeen(s.world)) out.push_back((int)i);
        else if (!s.world) loose.push_back((int)i);
    }
    if (out.empty()) out = loose;
}

// A level's water has no bottom in the collision the game ships: the cart floats on it by script, so in D1 the
// point where it sits has walls beside it and nothing at all under it.  SM64 makes no Mario without a floor, so
// when there is none the window gets a flat bed this far below him - deep enough to swim over, and only ever
// where the game itself has nothing.
const float BED_DEPTH = 25.0f;                   // game units below the point he is created at
const float BED_RADIUS = 60.0f;                  // and this far around him: a square the size of the window
                                                 // overflows the 32-bit arithmetic of libsm64's point-in-triangle
                                                 // test (its edges are tens of thousands of SM64 units long) and
                                                 // then he falls straight through it
std::vector<SM64Surface> s_windowSurf;           // the window as it was last handed over, so a bed can be added
bool s_bedOn;                                    // that bed is in the window, and stays in it while he swims
float s_bedZ;                                    // at this height of the game's world, wherever the window moves

// a flat square across the window at s_bedZ, wound so SM64 reads it as floor rather than ceiling
void AppendBed(std::vector<SM64Surface>& v, const float* centre) {
    int32_t y = (int32_t)((s_bedZ - centre[2]) / s_level.scale);
    int32_t r = (int32_t)(BED_RADIUS / s_level.scale);
    const int32_t corner[4][2] = { { -r, -r }, { r, -r }, { r, r }, { -r, r } };
    const int idx[2][3] = { { 0, 2, 1 }, { 0, 3, 2 } };
    for (int t = 0; t < 2; ++t) {
        SM64Surface s;
        memset(&s, 0, sizeof(s));
        for (int i = 0; i < 3; ++i) {
            s.vertices[i][0] = corner[idx[t][i]][0];
            s.vertices[i][1] = y;
            s.vertices[i][2] = corner[idx[t][i]][1];
        }
        v.push_back(s);
    }
}

// the triangles within WINDOW of `centre`, in SM64's units around it, given to libsm64
bool BuildWindow(const std::vector<int>& sets, const float* centre) {
    if (sets.empty()) return false;
    s_level.scale = RGHC_SCALE;
    s_level.at[0] = centre[0];
    s_level.at[1] = centre[1];
    s_level.at[2] = centre[2];
    std::vector<SM64Surface> surf;
    surf.reserve(2048);
    std::string from;
    bool live = false;
    for (size_t si = 0; si < sets.size(); ++si) {
        if (sets[si] < 0 || sets[si] >= (int)s_sets.size()) continue;
        const CollisionSet& set = s_sets[sets[si]];
        if (set.world && DynWorld(set.world)) { // this world's collision is placed object by object, live
            live = true;
            from += (from.empty() ? "" : ", ") + set.name + " live";
            continue;
        }
        size_t was = surf.size();
        size_t n = set.tris.size() / 9;
        for (size_t t = 0; t < n; ++t) {
            const float* p = &set.tris[t * 9];
            bool close = false;
            for (int v = 0; v < 3 && !close; ++v)
                if (fabsf(p[v * 3 + 0] - centre[0]) <= WINDOW && fabsf(p[v * 3 + 1] - centre[1]) <= WINDOW &&
                    fabsf(p[v * 3 + 2] - centre[2]) <= WINDOW)
                    close = true;
            if (!close) continue;
            SM64Surface s;
            s.type = 0;                          // SURFACE_DEFAULT: SM64 makes floors, walls and ceilings of them
            s.force = 0;                         // by the normal it works out for itself
            s.terrain = 0;
            bool ok = true;
            for (int v = 0; v < 3; ++v) {
                float g[3] = { p[v * 3 + 0], p[v * 3 + 1], p[v * 3 + 2] };
                float sm[3];
                ToSm64(g, sm);
                for (int c = 0; c < 3; ++c) {
                    if (sm[c] < -32000.0f || sm[c] > 32000.0f) ok = false;
                    s.vertices[v][c] = (int32_t)sm[c];
                }
            }
            if (ok) surf.push_back(s);
        }
        if (surf.size() > was) {
            char line[64];
            _snprintf(line, sizeof(line) - 1, "%s%s %u", from.empty() ? "" : ", ", set.name.c_str(),
                      (unsigned)(surf.size() - was));
            line[sizeof(line) - 1] = 0;
            from += line;
        }
    }
    if (surf.empty() && !live) {
        SmLog("no collision within %.0f units of %.0f %.0f %.0f", WINDOW, centre[0], centre[1], centre[2]);
        return false;
    }
    if (s_bedOn) AppendBed(surf, centre);        // he is swimming: the bed travels with the window
    ++s_windowGen;                               // the live things are measured from the middle: placed again
    int floors = 0, walls = 0, ceilings = 0;
    for (size_t i = 0; i < surf.size(); ++i) {
        const int32_t* v = &surf[i].vertices[0][0];
        float ax = (float)(v[3] - v[0]), ay = (float)(v[4] - v[1]), az = (float)(v[5] - v[2]);
        float bx = (float)(v[6] - v[0]), by = (float)(v[7] - v[1]), bz = (float)(v[8] - v[2]);
        float ny = az * bx - ax * bz;                // the y of the cross product: SM64 sorts them by it
        float len = sqrtf((ay * bz - az * by) * (ay * bz - az * by) + ny * ny + (ax * by - ay * bx) * (ax * by - ay * bx));
        float n = len > 0 ? ny / len : 0;
        if (n > 0.01f) ++floors; else if (n < -0.01f) ++ceilings; else ++walls;
    }
    double t0 = NowSeconds();
    p_surfaces_load(surf.empty() ? NULL : &surf[0], (uint32_t)surf.size());
    s_windowSurf = surf;
    s_curSets = sets;
    s_centre[0] = centre[0];
    s_centre[1] = centre[1];
    s_centre[2] = centre[2];
    s_windowed = true;
    s_surfacesGiven = true;
    DynPlace();                                  // and the live objects, before any step is taken in it
    SmLog("window at %.0f %.0f %.0f: %u baked triangles (%d floor, %d wall, %d ceiling) and %d live objects of %s, %.0f ms",
          centre[0], centre[1], centre[2], (unsigned)surf.size(), floors, walls, ceilings, s_dynPlaced, from.c_str(),
          (NowSeconds() - t0) * 1000.0);
    return true;
}

// he has walked away from the middle of the window: build it again around him, and put him at the same place in
// the coordinates it now has
void RecentreIfNeeded() {
    if (!s_windowed || s_mario.id < 0 || !s_mario.valid) return;
    float g[3];
    ToGame(s_mario.state.position, g);
    if (fabsf(g[0] - s_centre[0]) < RECENTRE && fabsf(g[1] - s_centre[1]) < RECENTRE &&
        fabsf(g[2] - s_centre[2]) < RECENTRE)
        return;
    float was[3] = { s_level.at[0], s_level.at[1], s_level.at[2] };
    if (!BuildWindow(s_curSets, g)) return;
    float sm[3];
    ToSm64(g, sm);
    p_set_position(s_mario.id, sm[0], sm[1], sm[2]);
    s_mario.state.position[0] = sm[0];
    s_mario.state.position[1] = sm[1];
    s_mario.state.position[2] = sm[2];
    // His triangles were built by the step just taken, measured from the old middle, and nothing rebuilds them
    // before the next step.  Drawn against the new middle they stood up to RECENTRE units away from him for a
    // step or two - a flicker of him far from the camera every time the window moved.  They are carried over
    // to the new middle here, so they stay exactly where they were in the game's world.
    float d[3] = { was[0] - s_level.at[0], was[1] - s_level.at[1], was[2] - s_level.at[2] };
    float dsm[3] = { d[0] / s_level.scale, d[2] / s_level.scale, -d[1] / s_level.scale };
    for (int i = 0; i < s_mario.triangles * 3; ++i) {
        s_mario.position[i * 3 + 0] += dsm[0];
        s_mario.position[i * 3 + 1] += dsm[1];
        s_mario.position[i * 3 + 2] += dsm[2];
    }
    // and the objects, measured from the same middle: their triangles of this step, and themselves in libsm64
    if (s_objReady) {
        for (uint32_t i = 0; i < s_obj.triangles * 3; ++i) {
            s_obj.position[i * 3 + 0] += dsm[0];
            s_obj.position[i * 3 + 1] += dsm[1];
            s_obj.position[i * 3 + 2] += dsm[2];
        }
        p_objects_shift(dsm[0], dsm[1], dsm[2]);
    }
}

int MarioCreate(const float* gamePos) {
    std::vector<int> sets;
    SetsForPoint(gamePos, sets);                 // the worlds of the level he is standing in
    if (!sets.empty()) {
        if (!BuildWindow(sets, gamePos)) return -1;
    } else if (!EnsureSurfaces()) {
        return -1;
    }
    if (s_mario.id >= 0) { p_mario_delete(s_mario.id); s_mario.id = -1; }
    float sm[3];
    ToSm64(gamePos, sm);
    // SM64 wants a floor under the point or it makes no Mario at all, and the cart's own point sits on the ground
    // it stands on - a hair inside it is enough to find nothing.  So try from a little higher, a step at a time.
    const float kLift[] = { 0.0f, 0.3f, 1.0f, 3.0f, 8.0f };   // game units above the point asked for
    int id = -1;
    float lift = 0.0f;
    for (int i = 0; i < (int)(sizeof(kLift) / sizeof(kLift[0])) && id < 0; ++i) {
        lift = kLift[i];
        id = p_mario_create(sm[0], sm[1] + lift / s_level.scale, sm[2]);
    }
    if (id < 0 && s_windowed) {                  // nothing under him: the water the cart floats on has no bottom
        s_bedOn = true;
        s_bedZ = gamePos[2] - BED_DEPTH;
        std::vector<SM64Surface> with = s_windowSurf;
        AppendBed(with, gamePos);
        p_surfaces_load(&with[0], (uint32_t)with.size());
        id = p_mario_create(sm[0], sm[1], sm[2]);
        if (id >= 0) {
            s_windowSurf = with;
            lift = 0.0f;
            // nothing under the cart means it is floating, so the height it floats at is the water's surface.
            // The script sets the level itself when the cart says it is on the water; this is for when it does not.
            if (p_set_water_level) p_set_water_level(id, (signed int)sm[1]);
            SmLog("nothing to stand on there: the cart is floating, so a bed %.0f units down and the water at his "
                  "own height let him swim", BED_DEPTH);
        } else {
            s_bedOn = false;
            p_surfaces_load(s_windowSurf.empty() ? NULL : &s_windowSurf[0], (uint32_t)s_windowSurf.size());
        }
    }
    if (id < 0) {
        SmLog("Mario could not be created at %.1f %.1f %.1f (SM64 %.0f %.0f %.0f: no floor under it, or up to %.0f "
              "units above)", gamePos[0], gamePos[1], gamePos[2], sm[0], sm[1], sm[2], kLift[4]);
        return -1;
    }
    if (lift > 0.0f) {
        sm[1] += lift / s_level.scale;
        SmLog("Mario created %.1f units above the point asked for: the floor is there, not at it", lift);
    }
    s_deathAt = 0;
    s_mario.id = id;
    s_mario.valid = false;
    s_mario.triangles = 0;
    memset(&s_mario.inputs, 0, sizeof(s_mario.inputs));
    memset(&s_mario.state, 0, sizeof(s_mario.state));
    s_mario.state.position[0] = sm[0]; s_mario.state.position[1] = sm[1]; s_mario.state.position[2] = sm[2];
    s_mario.position.assign(SM64_GEO_MAX_TRIANGLES * 9, 0.0f);
    s_mario.normal.assign(SM64_GEO_MAX_TRIANGLES * 9, 0.0f);
    s_mario.color.assign(SM64_GEO_MAX_TRIANGLES * 9, 0.0f);
    s_mario.uv.assign(SM64_GEO_MAX_TRIANGLES * 6, 0.0f);
    s_accum = 0;
    ObjectsAlloc();
    SmLog("Mario %d created at %.1f %.1f %.1f (SM64 %.0f %.0f %.0f)", id, gamePos[0], gamePos[1], gamePos[2], sm[0], sm[1], sm[2]);
    return id;
}

void MarioRespawn() {                            // at the level's Mario start, keeping the script's handle valid
    if (s_mario.id >= 0) { p_mario_delete(s_mario.id); s_mario.id = -1; }
    int id = p_mario_create(s_level.spawn[0], s_level.spawn[1], s_level.spawn[2]);
    if (id < 0) { SmLog("Mario could not start again at the level's Mario start"); return; }
    s_deathAt = 0;
    s_mario.id = id;
    s_mario.valid = false;
    s_mario.triangles = 0;
    memset(&s_mario.inputs, 0, sizeof(s_mario.inputs));
    s_accum = 0;
}

void DeathSound();                               // Bowser's laugh over the death transition (the audio is below)

bool s_snapNext;                                 // he was put somewhere: the step across it is not his own motion

// the step just gone, kept so the next one can be drawn out of the two of them
void InterpCapture() {
    if (!s_mario.valid) { s_mario.interp = false; s_snapNext = false; return; }
    s_mario.prevGame.resize((size_t)s_mario.triangles * 9);
    for (int i = 0; i < s_mario.triangles * 3; ++i)
        ToGame(&s_mario.position[i * 3], &s_mario.prevGame[i * 3]);
    ToGame(s_mario.state.position, s_mario.prevRoot);
    s_mario.prevTriangles = s_mario.triangles;
    s_mario.interp = !s_snapNext;
    s_snapNext = false;
}

// A teleport, so nothing is blended through it: not this frame, where he is still drawn where he was, and not the
// step after it either - that pair straddles the jump, and blending it would drag him across the level in view.
void InterpBreak() { s_mario.interp = false; s_snapNext = true; }

// how far into the step the picture is: 0 at the step just taken, 1 at the next one
float InterpDelta() {
    float d = (float)(s_accum * 30.0);
    return d < 0.0f ? 0.0f : d > 1.0f ? 1.0f : d;
}

bool InterpOn() { return s_mario.interp && s_mario.prevTriangles == s_mario.triangles; }

void MarioTick() {                               // one 30 Hz step with the inputs the script gave
    InterpCapture();
    ObjectsCapturePrev();
    ObjectsCamera();
    // Q: the jump pressed again for you while it is held.  A jump only starts on the frame the button goes down,
    // so chaining one needs a release in between: a step on, a step off, which is as fast as 30 Hz can press it.
    // It belongs here and not where the keys are read - the keys are read once a frame, and a frame is worth half
    // a step at 60 Hz, so alternating there would hand the same value to every step and he would hold or nothing.
    if (s_rapidOn && s_jumpHeld) s_mario.inputs.buttonA = (uint8_t)(s_rapidPhase++ & 1);
    // the humans nearest him, for the enemies to hunt
    s_preyTickN = 0;
    if (p_prey_set) {
        float mg[3];
        ToGame(s_mario.state.position, mg);
        std::vector<std::pair<float, size_t> > order;
        for (size_t i = 0; i < s_preyReports.size(); ++i) {
            const float* g = s_preyReports[i].g;
            float dx = g[0] - mg[0], dy = g[1] - mg[1], dz = g[2] - mg[2];
            order.push_back(std::make_pair(dx * dx + dy * dy + dz * dz, i));
        }
        std::sort(order.begin(), order.end());
        float sm[16 * 3];
        for (size_t k = 0; k < order.size() && s_preyTickN < 16; ++k) {
            const PreyReport& r = s_preyReports[order[k].second];
            ToSm64(r.g, &sm[s_preyTickN * 3]);
            s_preyTick[s_preyTickN++] = r.obj;
        }
        p_prey_set(sm, s_preyTickN);
        s_preyReports.clear();
    }
    SM64MarioGeometryBuffers buf;
    buf.position = &s_mario.position[0];
    buf.normal = &s_mario.normal[0];
    buf.color = &s_mario.color[0];
    buf.uv = &s_mario.uv[0];
    buf.numTrianglesUsed = 0;
    p_mario_tick(s_mario.id, &s_mario.inputs, &s_mario.state, &buf);
    s_mario.triangles = buf.numTrianglesUsed;
    s_mario.valid = true;
    if (s_objReady) s_obj.triangles = s_obj.buf.numTrianglesUsed;
    if (p_hud_counts) p_hud_counts(&s_sm64Coins, &s_sm64Stars);
    ThreatsRead();
    if (p_prey_hits && s_preyTickN > 0) {        // who an enemy touched: a second between two hits on the same one
        int32_t idx[16];
        int32_t n = p_prey_hits(idx, 16);
        double now = NowSeconds();
        for (int32_t i = 0; i < n; ++i) {
            if (idx[i] < 0 || idx[i] >= s_preyTickN) continue;
            uint32_t obj = s_preyTick[idx[i]];
            bool recent = false;
            for (size_t j = 0; j < s_preyLastHit.size(); ++j)
                if (s_preyLastHit[j].first == obj) { recent = now - s_preyLastHit[j].second < 1.0; if (!recent) s_preyLastHit[j].second = now; break; }
            if (recent) continue;
            bool known = false;
            for (size_t j = 0; j < s_preyLastHit.size(); ++j) if (s_preyLastHit[j].first == obj) known = true;
            if (!known) s_preyLastHit.push_back(std::make_pair(obj, now));
            s_preyHitPending.push_back(obj);
        }
        if (s_preyLastHit.size() > 256) s_preyLastHit.erase(s_preyLastHit.begin(), s_preyLastHit.begin() + 128);
    }
    ++s_ticks;
    // ACT_LONG_JUMP and going backwards: throw him at the floor so the landing comes at once and the speed he has
    // built is still his.  His own horizontal speed is left alone - that is the whole point of the trick.
    // Only while the jump is held: letting go is how the trick is stopped.  Held for ever he can never land out
    // of the long jump, so the speed he has built has nothing to bleed it away and he keeps it for ever.
    if (s_bljOn && s_jumpHeld && p_set_velocity && s_mario.state.action == 0x03000888u && s_mario.state.forwardVelocity < 0.0f)
        p_set_velocity(s_mario.id, s_mario.state.velocity[0], -120.0f, s_mario.state.velocity[2]);
    // the wind: the fan's push, less the game's gravity, plus SM64's, once a step
    if (s_windOn) {
        s_windOn = false;
        const uint32_t ACT_VERTICAL_WIND = 0x1008089Cu;
        float k = 1.0f / (s_level.scale != 0.0f ? s_level.scale : RGHC_SCALE) / 900.0f;   // game/s^2 -> SM64/step^2
        // The cart in a stream has the dynamics' friction on every axis (the cart sets 8 to 10 a second there), so
        // it floats up at the speed the push and the drag agree on rather than climbing ever faster.  The same
        // drag here, a fifth of the speed a step, and a ceiling on the climb - a float, not a launch.
        float up = (s_wind[2] - 9.81f + 32.4f) * k;
        float vy = s_mario.state.velocity[1] * 0.8f + up;
        if (vy > 30.0f) vy = 30.0f;
        // sideways, through the speed and facing SM64's air steps are built on
        float sx = s_wind[0] * k, sz = -s_wind[1] * k;
        float fwd = s_mario.state.forwardVelocity;
        float a = s_mario.state.faceAngle;
        float vx = fwd * sinf(a) + sx, vz = fwd * cosf(a) + sz;
        if (s_mario.state.action != ACT_VERTICAL_WIND && !(s_mario.state.action & 0x00002000u) && p_set_action) {
            p_set_action(s_mario.id, ACT_VERTICAL_WIND);   // ACT_FLAG_SWIMMING excepted: the water keeps him
            s_mario.state.action = ACT_VERTICAL_WIND;
            s_flipLoop = false;
            s_launchHold = false;
        }
        if (p_set_velocity) p_set_velocity(s_mario.id, vx, vy, vz);
        if (fabsf(sx) + fabsf(sz) > 0.001f) {
            if (p_set_forward_velocity) p_set_forward_velocity(s_mario.id, sqrtf(vx * vx + vz * vz));
            if (p_set_faceangle && (vx * vx + vz * vz) > 0.01f) p_set_faceangle(s_mario.id, atan2f(vx, vz));
        }
    }
    // a throw keeps its speed along the ground until he lands
    if (s_launchHold) {
        if (s_mario.state.action & 0x00000800u) {           // ACT_FLAG_AIR
            if (p_set_forward_velocity) p_set_forward_velocity(s_mario.id, s_launchFwd);
        } else {
            s_launchHold = false;
        }
    }
    // a flip that is to last until he lands: wound back once its animation has run out
    if (s_flipLoop) {
        if (s_mario.state.action != ACT_TRIPLE_JUMP) {
            s_flipLoop = false;
        } else if (s_mario.state.animID == MARIO_ANIM_TRIPLE_JUMP && s_mario.state.animFrame == s_flipFrame &&
                   p_set_anim_frame) {
            p_set_anim_frame(s_mario.id, 0);
            s_mario.state.animFrame = 0;
        }
        s_flipFrame = s_mario.state.animFrame;
    }
    RecentreIfNeeded();
    double when = NowSeconds();                  // once a second: his own step against the script's moving of him
    if (when - s_moveAt >= 1.0) {
        if (s_moveAt) {
            float g[3];
            ToGame(s_mario.state.position, g);
            SmLog("SM64: at %.0f %.0f %.0f, speed %.0f, action %08X; the script moved him %d times, %.1f units "
                  "in all, biggest %.2f", g[0], g[1], g[2], s_mario.state.forwardVelocity, s_mario.state.action,
                  s_moveN, s_moveSum, s_moveMax);
        }
        s_moveAt = when;
        s_moveN = 0;
        s_moveSum = 0;
        s_moveMax = 0;
    }
    if (s_mario.state.health <= 0) {             // his health ran out: it costs a life.  Where he comes back is
        if (s_deadTicks++ == 0 && s_lives > 0) --s_lives;   // the level script's to say - it knows where the
    } else {                                     // rabbids respawn - so nothing is done here
        s_deadTicks = 0;
    }
    if (s_mario.state.health == 0 && !s_deathAt) {
        s_deathAt = NowSeconds();                // SM64's death transition, and what goes over it
        DeathSound();
    }

}

// ------------------------------------------------------------------------------------------------ drawing
const uint32_t G_VID = 0x00A70BA0;               // ViD
const uint32_t VID_VIEWS = 0x57F0;               // K3D_BasicCamera[8], 0xA4 each: world matrix, view matrix (+0x40), fov (+0x84)
const uint32_t CAM_SIZE = 0xA4;

// The detail on Mario - his eyes, his sideburns, the buttons on his overalls - is the textured part of the model,
// and the rest of him is vertex colour; the two are blended by the texture's alpha.  So the light cannot go on the
// vertex colour alone, or everything textured comes out unlit and glows in a dark level.  It rides in a second
// colour instead, and the blend of the two is modulated by it.
struct Vtx { float x, y, z; float nx, ny, nz; DWORD colour; DWORD light; float u, v; };
const DWORD VTX_FVF = D3DFVF_XYZ | D3DFVF_NORMAL | D3DFVF_DIFFUSE | D3DFVF_SPECULAR | D3DFVF_TEX1;

// The engine hands its own shader two lights in vertex shader constants c56..c63: a colour, a position, and the
// direction the light travels.  Mario is drawn with the fixed function pipeline, so the same terms are worked out
// here per vertex - without them he had ambient only, which left him darker than everything around him.
struct EngineLight { float colour[3]; float dir[3]; };
EngineLight s_lights[2];
bool s_lightsRead;

void ReadEngineLights(IDirect3DDevice9* dev) {
    // reading shader constants back asks the driver to sync with the GPU, which costs more than it looks: doing it
    // every frame pushed the frame over its 16.7 ms and vsync halved the game to 30 Hz.  The level's lights barely
    // change, so a few times a second is plenty.
    static double next;
    double now = NowSeconds();
    if (s_lightsRead && now < next) return;
    next = now + 0.25;
    float c[32];
    s_lightsRead = SUCCEEDED(dev->GetVertexShaderConstantF(56, c, 8));
    if (!s_lightsRead) return;
    for (int L = 0; L < 2; ++L) {
        for (int i = 0; i < 3; ++i) {
            s_lights[L].colour[i] = c[L * 16 + i];
            s_lights[L].dir[i] = c[L * 16 + 12 + i];
        }
    }
}

// the world's ambient terms, as the script reads them from the engine (WOR_AmbiantGet / WOR_Ambiant2Get).  The
// engine writes a colour as one word, blue in the high byte: 0x00BBGGRR.
float s_ambTop[3] = { 1.0f, 1.0f, 1.0f };        // what an upward face gets
float s_ambBottom[3] = { 1.0f, 1.0f, 1.0f };     // what a downward one gets

void SetAmbient(float* out, uint32_t word) {
    out[0] = (word & 0xFF) / 255.0f;             // R
    out[1] = ((word >> 8) & 0xFF) / 255.0f;      // G
    out[2] = ((word >> 16) & 0xFF) / 255.0f;     // B
}

IDirect3DStateBlock9* s_sceneState;              // captured round Mario's draw (made once, not once a frame)
IDirect3DTexture9* s_tex;
IDirect3DDevice9* s_texDevice;
std::vector<Vtx> s_vtx;
std::vector<Vtx> s_plain, s_textured, s_decal;   // his own colours, the sprites, and the decals over his face
bool s_probed;
bool s_useEngineTransforms;                      // the device's fixed-function transforms carry the scene's matrices
D3DMATRIX s_proj;

bool EnsureTexture(IDirect3DDevice9* dev) {
    if (s_tex && s_texDevice == dev) return true;
    if (s_tex) { s_tex->Release(); s_tex = NULL; }
    if (!s_texture) return false;
    if (FAILED(dev->CreateTexture(SM64_TEXTURE_WIDTH, SM64_TEXTURE_HEIGHT, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &s_tex, NULL)))
        return false;
    D3DLOCKED_RECT lr;
    if (FAILED(s_tex->LockRect(0, &lr, NULL, 0))) { s_tex->Release(); s_tex = NULL; return false; }
    for (int y = 0; y < SM64_TEXTURE_HEIGHT; ++y) {
        uint8_t* dst = (uint8_t*)lr.pBits + y * lr.Pitch;
        const uint8_t* src = s_texture + y * SM64_TEXTURE_WIDTH * 4;
        for (int x = 0; x < SM64_TEXTURE_WIDTH; ++x) {          // RGBA -> BGRA
            dst[x * 4 + 0] = src[x * 4 + 2];
            dst[x * 4 + 1] = src[x * 4 + 1];
            dst[x * 4 + 2] = src[x * 4 + 0];
            dst[x * 4 + 3] = src[x * 4 + 3];
        }
    }
    s_tex->UnlockRect(0);
    s_texDevice = dev;
    return true;
}

void LogMatrix(const char* name, const D3DMATRIX& m) {
    SmLog("%s: [%.4f %.4f %.4f %.4f] [%.4f %.4f %.4f %.4f] [%.4f %.4f %.4f %.4f] [%.4f %.4f %.4f %.4f]", name,
          m._11, m._12, m._13, m._14, m._21, m._22, m._23, m._24, m._31, m._32, m._33, m._34, m._41, m._42, m._43, m._44);
}

bool CameraView(uint32_t view, D3DMATRIX& out, float* fov) {
    uint8_t* vid = *(uint8_t**)(uintptr_t)G_VID;
    if (!vid || view > 7) return false;
    const uint8_t* cam = vid + VID_VIEWS + view * CAM_SIZE;
    memcpy(&out, cam + 0x40, 64);
    if (fov) memcpy(fov, cam + 0x84, 4);
    return true;
}

void Probe(IDirect3DDevice9* dev, uint32_t view) {
    if (s_probed) return;
    s_probed = true;
    D3DMATRIX v, p, w;
    dev->GetTransform(D3DTS_VIEW, &v);
    dev->GetTransform(D3DTS_PROJECTION, &p);
    dev->GetTransform(D3DTS_WORLD, &w);
    LogMatrix("device view", v);
    LogMatrix("device projection", p);
    LogMatrix("device world", w);
    D3DMATRIX cv;
    float fov = 0;
    if (CameraView(view, cv, &fov)) { LogMatrix("engine camera view", cv); SmLog("engine camera fov %.4f (view %u)", fov, view); }
    D3DVIEWPORT9 vp;
    if (SUCCEEDED(dev->GetViewport(&vp))) SmLog("viewport %u %u %ux%u z %.3f..%.3f", vp.X, vp.Y, vp.Width, vp.Height, vp.MinZ, vp.MaxZ);
    float c[16 * 4];
    if (SUCCEEDED(dev->GetVertexShaderConstantF(0, c, 16))) {
        for (int r = 0; r < 16; r += 4)
            SmLog("vs c%d..c%d: [%.4f %.4f %.4f %.4f] [%.4f %.4f %.4f %.4f] [%.4f %.4f %.4f %.4f] [%.4f %.4f %.4f %.4f]", r, r + 3,
                  c[r * 4 + 0], c[r * 4 + 1], c[r * 4 + 2], c[r * 4 + 3], c[r * 4 + 4], c[r * 4 + 5], c[r * 4 + 6], c[r * 4 + 7],
                  c[r * 4 + 8], c[r * 4 + 9], c[r * 4 + 10], c[r * 4 + 11], c[r * 4 + 12], c[r * 4 + 13], c[r * 4 + 14], c[r * 4 + 15]);
    }
    IDirect3DVertexShader9* vs = NULL;
    dev->GetVertexShader(&vs);
    SmLog("vertex shader bound: %s", vs ? "yes" : "no (fixed function)");
    {                                            // the engine's lights, as its own shader gets them
        float c[32];
        if (SUCCEEDED(dev->GetVertexShaderConstantF(56, c, 8)))
            for (int L = 0; L < 2; ++L)
                SmLog("engine light %d: colour %.3f %.3f %.3f  at %.1f %.1f %.1f  direction %.3f %.3f %.3f", L,
                      c[L * 16 + 0], c[L * 16 + 1], c[L * 16 + 2], c[L * 16 + 4], c[L * 16 + 5], c[L * 16 + 6],
                      c[L * 16 + 12], c[L * 16 + 13], c[L * 16 + 14]);
    }
    for (int base = 0; base < 16; base += 4) {
        float c[16];
        if (FAILED(dev->GetPixelShaderConstantF(base, c, 4))) break;
        SmLog("ps c%02d..c%02d: [%.3f %.3f %.3f %.3f] [%.3f %.3f %.3f %.3f] [%.3f %.3f %.3f %.3f] [%.3f %.3f %.3f %.3f]",
              base, base + 3, c[0], c[1], c[2], c[3], c[4], c[5], c[6], c[7], c[8], c[9], c[10], c[11],
              c[12], c[13], c[14], c[15]);
    }
    if (vs) vs->Release();
    // a perspective projection has _34 = +-1 and _44 = 0
    s_useEngineTransforms = (fabsf(p._34) > 0.5f && fabsf(p._44) < 0.01f);
    SmLog("Mario is drawn with %s", s_useEngineTransforms ? "the device's transforms" : "the engine camera + a built projection");
}

// The depth Mario is written at has to be the depth the level is written at, or he reads as a picture stuck over
// the game rather than something standing in it: a projection of our own, with near and far planes we invented,
// puts him in front of whatever he walks up to.  The engine keeps its own projection in its ViD block, and only
// its third and fourth rows are taken from it - the depth mapping, which is the same for every view - while the
// shape of the picture keeps coming from the camera this draw is for.
const uint32_t VID_PROJ = 0x9CC0;                // K3D projection matrix (checked before it is believed)
bool s_projLogged;

bool EngineDepthRows(D3DMATRIX& out) {
    uint8_t* vid = *(uint8_t**)(uintptr_t)G_VID;
    if (!vid) return false;
    D3DMATRIX m;
    memcpy(&m, vid + VID_PROJ, sizeof(m));
    if (fabsf(fabsf(m._34) - 1.0f) > 0.01f || fabsf(m._44) > 1e-5f) return false;
    if (!(m._11 > 0.05f && m._11 < 20.0f) || !(m._33 > 0.0f && m._33 < 1.5f) || !(m._43 < 0.0f)) return false;
    float zn = -m._43 / m._33;
    if (!(zn > 0.0001f && zn < 100.0f)) return false;
    out._33 = m._33;
    out._34 = m._34;
    out._43 = m._43;
    out._44 = 0.0f;
    if (!s_projLogged) {
        s_projLogged = true;
        float zf = m._33 > 1.0f ? zn * m._33 / (m._33 - 1.0f) : 0.0f;
        SmLog("depth from the engine's own projection: near %.4f far %.0f", zn, zf);
    }
    return true;
}

// ------------------------------------------------------------------------------------------------ the objects' draw
// The actors' pictures, one texture each, made from libsm64's RGBA the first time a triangle wants one.
IDirect3DDevice9* s_objTexDevice;
std::vector<IDirect3DTexture9*> s_objTex;

void ObjTexturesLost() {
    for (size_t i = 0; i < s_objTex.size(); ++i) if (s_objTex[i]) s_objTex[i]->Release();
    s_objTex.clear();
    s_objTexDevice = NULL;
}

IDirect3DTexture9* ObjTexture(IDirect3DDevice9* dev, int id) {
    if (s_objTexDevice != dev) { ObjTexturesLost(); s_objTexDevice = dev; }
    if (s_objTex.empty()) s_objTex.assign((size_t)p_texture_count(), (IDirect3DTexture9*)NULL);
    if (id < 0 || id >= (int)s_objTex.size()) return NULL;
    if (s_objTex[id]) return s_objTex[id];
    int32_t w = 0, h = 0;
    if (!p_texture_size(id, &w, &h) || w <= 0 || h <= 0) return NULL;
    const uint8_t* rgba = p_texture_rgba(id);
    if (!rgba) return NULL;
    IDirect3DTexture9* t = NULL;
    if (FAILED(dev->CreateTexture(w, h, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &t, NULL))) return NULL;
    D3DLOCKED_RECT lr;
    if (FAILED(t->LockRect(0, &lr, NULL, 0))) { t->Release(); return NULL; }
    for (int y = 0; y < h; ++y) {
        uint8_t* dst = (uint8_t*)lr.pBits + y * lr.Pitch;
        const uint8_t* src = rgba + (size_t)y * w * 4;
        for (int x = 0; x < w; ++x) {          // RGBA -> BGRA
            dst[x * 4 + 0] = src[x * 4 + 2];
            dst[x * 4 + 1] = src[x * 4 + 1];
            dst[x * 4 + 2] = src[x * 4 + 0];
            dst[x * 4 + 3] = src[x * 4 + 3];
        }
    }
    t->UnlockRect(0);
    s_objTex[id] = t;
    return t;
}

std::vector<Vtx> s_objVtx;
std::vector<uint32_t> s_objOrder;
std::vector<int> s_objPrevOffset, s_objPrevCount, s_objCurCount, s_objCurK, s_objPrevIdx;

// which way a triangle is drawn: its layer's bucket (opaque, cut out, or see-through in order), its picture and
// how it is put together, so that the ones alike are drawn together
inline uint32_t ObjKey(uint32_t t) {
    uint8_t layer = s_obj.layer[t];
    uint32_t bucket = layer < 4 ? 0 : layer == 4 ? 1 : 2 + (layer - 5);
    return (bucket << 28) | ((uint32_t)s_obj.texture[t] << 8) | s_obj.flags[t];
}

// The objects, after Mario, with the same camera and the same lighting: the lit ones take the world's ambient and
// its lights on their normals the way he does, the unlit ones (flames, coins, smoke: the corner colours are the
// whole of their look) are drawn as they are.  Opaque first, then the cut-outs, then the see-through layers
// blended without writing depth.
void ObjectsDraw(IDirect3DDevice9* dev) {
    if (!s_objReady || s_obj.triangles == 0) return;
    uint32_t n = s_obj.triangles;
    // per object, the k-th triangle of this step blends with the k-th of the last if it has as many
    bool blend = s_obj.interp && s_obj.prevTriangles > 0;
    float k = InterpDelta();
    if (blend) {
        s_objPrevOffset.assign(257, 0);
        s_objPrevCount.assign(257, 0);
        s_objCurCount.assign(257, 0);
        for (uint32_t t = 0; t < s_obj.prevTriangles; ++t) ++s_objPrevCount[s_obj.prevObject[t] & 0xFF];
        for (uint32_t t = 0; t < n; ++t) ++s_objCurCount[s_obj.object[t] & 0xFF];
        int off = 0;
        for (int o = 0; o < 257; ++o) { s_objPrevOffset[o] = off; off += s_objPrevCount[o]; }
        s_objPrevIdx.assign(off, 0);
        std::vector<int> fill(s_objPrevOffset);
        for (uint32_t t = 0; t < s_obj.prevTriangles; ++t) s_objPrevIdx[fill[s_obj.prevObject[t] & 0xFF]++] = (int)t;
        s_objCurK.assign(n, 0);
        std::vector<int> seen(257, 0);
        for (uint32_t t = 0; t < n; ++t) s_objCurK[t] = seen[s_obj.object[t] & 0xFF]++;
    }
    s_objOrder.resize(n);
    for (uint32_t t = 0; t < n; ++t) s_objOrder[t] = t;
    struct ByKey { bool operator()(uint32_t a, uint32_t b) const { uint32_t ka = ObjKey(a), kb = ObjKey(b); return ka != kb ? ka < kb : a < b; } };
    std::stable_sort(s_objOrder.begin(), s_objOrder.end(), ByKey());
    s_objVtx.resize((size_t)n * 3);
    for (uint32_t i = 0; i < n; ++i) {
        uint32_t t = s_objOrder[i];
        int o = s_obj.object[t] & 0xFF;
        bool lit = (s_obj.flags[t] & SM64_TRI_LIT) != 0;
        int prevTri = -1;
        if (blend && s_objPrevCount[o] == s_objCurCount[o] && s_objCurK[t] < s_objPrevCount[o])
            prevTri = s_objPrevIdx[s_objPrevOffset[o] + s_objCurK[t]];
        for (int c = 0; c < 3; ++c) {
            uint32_t corner = t * 3 + c;
            const float* p = &s_obj.position[corner * 3];
            const float* nn = &s_obj.normal[corner * 3];
            const float* col = &s_obj.color[corner * 4];
            Vtx& v = s_objVtx[i * 3 + c];
            float g[3];
            ToGame(p, g);
            if (prevTri >= 0) {
                const float* q = &s_obj.prevGame[((size_t)prevTri * 3 + c) * 3];
                g[0] = q[0] + (g[0] - q[0]) * k;
                g[1] = q[1] + (g[1] - q[1]) * k;
                g[2] = q[2] + (g[2] - q[2]) * k;
            }
            v.x = g[0]; v.y = g[1]; v.z = g[2];
            v.nx = nn[0]; v.ny = -nn[2]; v.nz = nn[1];
            float lr = 1.0f, lg = 1.0f, lb = 1.0f;
            if (lit) {
                float tt = v.nz * 0.5f + 0.5f;
                lr = s_ambBottom[0] + (s_ambTop[0] - s_ambBottom[0]) * tt;
                lg = s_ambBottom[1] + (s_ambTop[1] - s_ambBottom[1]) * tt;
                lb = s_ambBottom[2] + (s_ambTop[2] - s_ambBottom[2]) * tt;
                for (int L = 0; L < 2 && s_lightsRead; ++L) {
                    const EngineLight& lt = s_lights[L];
                    float ndl = -(v.nx * lt.dir[0] + v.ny * lt.dir[1] + v.nz * lt.dir[2]);
                    if (ndl <= 0.0f) continue;
                    lr += lt.colour[0] * ndl;
                    lg += lt.colour[1] * ndl;
                    lb += lt.colour[2] * ndl;
                }
                if (lr > 1.0f) lr = 1.0f;
                if (lg > 1.0f) lg = 1.0f;
                if (lb > 1.0f) lb = 1.0f;
            }
            int r = (int)(col[0] * 255.0f + 0.5f), gg = (int)(col[1] * 255.0f + 0.5f), b = (int)(col[2] * 255.0f + 0.5f), a = (int)(col[3] * 255.0f + 0.5f);
            r = r < 0 ? 0 : r > 255 ? 255 : r; gg = gg < 0 ? 0 : gg > 255 ? 255 : gg; b = b < 0 ? 0 : b > 255 ? 255 : b; a = a < 0 ? 0 : a > 255 ? 255 : a;
            v.colour = D3DCOLOR_ARGB(a, r, gg, b);
            v.light = D3DCOLOR_ARGB(255, (int)(lr * 255.0f + 0.5f), (int)(lg * 255.0f + 0.5f), (int)(lb * 255.0f + 0.5f));
            v.u = s_obj.uv[corner * 2]; v.v = s_obj.uv[corner * 2 + 1];
        }
    }
    // the runs of triangles drawn alike
    dev->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
    dev->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
    uint32_t start = 0;
    while (start < n) {
        uint32_t key = ObjKey(s_objOrder[start]);
        uint32_t end = start + 1;
        while (end < n && ObjKey(s_objOrder[end]) == key) ++end;
        uint32_t bucket = key >> 28;
        uint16_t tex = (uint16_t)((key >> 8) & 0xFFFF);
        uint8_t flags = (uint8_t)(key & 0xFF);
        IDirect3DTexture9* texture = tex != 0xFFFF ? ObjTexture(dev, tex) : NULL;
        bool textured = texture != NULL && (flags & (SM64_TRI_TEXTURED | SM64_TRI_TEX_ALPHA | SM64_TRI_BLEND));
        dev->SetTexture(0, textured ? texture : NULL);
        if (textured && (flags & SM64_TRI_BLEND)) {          // the picture laid over the colour by its alpha
            dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_BLENDTEXTUREALPHA);
            dev->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
            dev->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
        } else if (textured && (flags & SM64_TRI_TEXTURED)) {
            dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_MODULATE);
            dev->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
            dev->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
        } else {
            dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG2);
            dev->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
        }
        if (textured && (flags & SM64_TRI_TEX_ALPHA)) {
            dev->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_MODULATE);
            dev->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
            dev->SetTextureStageState(0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE);
        } else {
            dev->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG2);
            dev->SetTextureStageState(0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE);
        }
        dev->SetSamplerState(0, D3DSAMP_ADDRESSU, (flags & SM64_TRI_CLAMP_S) ? D3DTADDRESS_CLAMP : (flags & SM64_TRI_MIRROR_S) ? D3DTADDRESS_MIRROR : D3DTADDRESS_WRAP);
        dev->SetSamplerState(0, D3DSAMP_ADDRESSV, (flags & SM64_TRI_CLAMP_T) ? D3DTADDRESS_CLAMP : (flags & SM64_TRI_MIRROR_T) ? D3DTADDRESS_MIRROR : D3DTADDRESS_WRAP);
        if (bucket >= 2) {                       // see-through: blended, over what is already there
            dev->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
            dev->SetRenderState(D3DRS_ALPHATESTENABLE, TRUE);
            dev->SetRenderState(D3DRS_ALPHAREF, 2);
            dev->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
        } else {                                 // solid, with the clear texels of a cut-out thrown away
            dev->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
            dev->SetRenderState(D3DRS_ALPHATESTENABLE, TRUE);
            dev->SetRenderState(D3DRS_ALPHAREF, 8);
            dev->SetRenderState(D3DRS_ZWRITEENABLE, TRUE);
        }
        dev->DrawPrimitiveUP(D3DPT_TRIANGLELIST, (UINT)(end - start), &s_objVtx[(size_t)start * 3], sizeof(Vtx));
        start = end;
    }
    dev->SetRenderState(D3DRS_ZWRITEENABLE, TRUE);
    dev->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
    dev->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
    dev->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
}

void SceneDraw(IDirect3DDevice9* dev, uint32_t view) {
    s_lastView = view;
    if (!s_enabled || !s_libReady || s_mario.id < 0 || !s_mario.valid || s_mario.triangles == 0) return;
    if (!Driven()) return;                       // nobody is playing him: a menu, a video, or no level at all
    Probe(dev, view);
    if (!EnsureTexture(dev)) return;
    D3DMATRIX viewM, projM, ident;
    memset(&ident, 0, sizeof(ident));
    ident._11 = ident._22 = ident._33 = ident._44 = 1.0f;
    if (s_useEngineTransforms) {
        dev->GetTransform(D3DTS_VIEW, &viewM);
        dev->GetTransform(D3DTS_PROJECTION, &projM);
    } else {
        float fov = 0.948f;
        if (!CameraView(view, viewM, &fov)) return;
        D3DVIEWPORT9 vp;
        dev->GetViewport(&vp);
        float aspect = vp.Height ? (float)vp.Width / (float)vp.Height : 16.0f / 9.0f;
        float zn = 0.1f, zf = 5000.0f;
        float ys = 1.0f / tanf(fov * 0.5f), xs = ys / aspect;
        memset(&projM, 0, sizeof(projM));
        projM._11 = xs; projM._22 = -ys; projM._33 = zf / (zf - zn); projM._34 = 1.0f; projM._43 = -zn * zf / (zf - zn);   // the engine's camera space is Y down
        EngineDepthRows(projM);                   // and the depth mapping as the engine writes it
    }
    s_camView = viewM;                           // the objects' camera is this one, whichever way it was found
    s_camViewValid = true;
    ReadEngineLights(dev);
    bool vanish = (s_mario.state.flags & 0x00000002u) != 0;   // MARIO_VANISH_CAP: he is drawn half there
    // MARIO_METAL_CAP.  SM64 draws metal Mario with an environment-mapped material, and the library hands back no
    // material at all - only position, normal, colour and UV - so this is the nearest honest thing: his own colours
    // taken to silver, brighter where a face points up, which is where a reflection would catch.
    bool metal = (s_mario.state.flags & 0x00000004u) != 0;
    // the triangles in the game's units and colours, drawn between the last two steps.  Only the places move:
    // the colours and the atlas coordinates are this step's, so a blink or a cap change lands on one frame
    // instead of being smeared across the blend.
    bool blend = InterpOn();
    float k = InterpDelta();
    s_vtx.resize(s_mario.triangles * 3);
    for (int i = 0; i < s_mario.triangles * 3; ++i) {
        const float* p = &s_mario.position[i * 3];
        const float* n = &s_mario.normal[i * 3];
        const float* c = &s_mario.color[i * 3];
        Vtx& v = s_vtx[i];
        float g[3];
        ToGame(p, g);
        if (blend) {
            const float* q = &s_mario.prevGame[i * 3];
            g[0] = q[0] + (g[0] - q[0]) * k;
            g[1] = q[1] + (g[1] - q[1]) * k;
            g[2] = q[2] + (g[2] - q[2]) * k;
        }
        v.x = g[0]; v.y = g[1]; v.z = g[2];
        v.nx = n[0]; v.ny = -n[2]; v.nz = n[1];
        // the world's ambient, between its two terms by which way the face points (z is up in the game) ...
        float t = v.nz * 0.5f + 0.5f;
        float lr = s_ambBottom[0] + (s_ambTop[0] - s_ambBottom[0]) * t;
        float lg = s_ambBottom[1] + (s_ambTop[1] - s_ambBottom[1]) * t;
        float lb = s_ambBottom[2] + (s_ambTop[2] - s_ambBottom[2]) * t;
        // ... and the level's own lights on top, the way the engine's shader takes them
        for (int L = 0; L < 2 && s_lightsRead; ++L) {
            const EngineLight& lt = s_lights[L];
            float ndl = -(v.nx * lt.dir[0] + v.ny * lt.dir[1] + v.nz * lt.dir[2]);
            if (ndl <= 0.0f) continue;
            lr += lt.colour[0] * ndl;
            lg += lt.colour[1] * ndl;
            lb += lt.colour[2] * ndl;
        }
        if (lr > 1.0f) lr = 1.0f;
        if (lg > 1.0f) lg = 1.0f;
        if (lb > 1.0f) lb = 1.0f;
        float cr = c[0], cg = c[1], cb = c[2];
        if (metal) {
            float grey = cr * 0.30f + cg * 0.59f + cb * 0.11f;
            float sheen = 0.45f + 0.55f * (v.nz * 0.5f + 0.5f);
            cr = (grey * 0.55f + 0.45f) * sheen;
            cg = (grey * 0.55f + 0.47f) * sheen;
            cb = (grey * 0.55f + 0.52f) * sheen;   // a touch of blue, the way polished metal reads
            if (cr > 1.0f) cr = 1.0f;
            if (cg > 1.0f) cg = 1.0f;
            if (cb > 1.0f) cb = 1.0f;
        }
        int r = (int)(cr * 255.0f + 0.5f), gg = (int)(cg * 255.0f + 0.5f), b = (int)(cb * 255.0f + 0.5f);
        r = r < 0 ? 0 : r > 255 ? 255 : r; gg = gg < 0 ? 0 : gg > 255 ? 255 : gg; b = b < 0 ? 0 : b > 255 ? 255 : b;
        v.colour = D3DCOLOR_ARGB(vanish ? 128 : 255, r, gg, b);
        v.light = D3DCOLOR_ARGB(255, (int)(lr * 255.0f + 0.5f), (int)(lg * 255.0f + 0.5f), (int)(lb * 255.0f + 0.5f));
        v.u = s_mario.uv[i * 2]; v.v = s_mario.uv[i * 2 + 1];
    }
    if (!s_sceneState && FAILED(dev->CreateStateBlock(D3DSBT_ALL, &s_sceneState))) s_sceneState = NULL;
    if (s_sceneState) s_sceneState->Capture();
    dev->SetVertexShader(NULL);
    dev->SetPixelShader(NULL);
    dev->SetTransform(D3DTS_WORLD, &ident);
    dev->SetTransform(D3DTS_VIEW, &viewM);
    dev->SetTransform(D3DTS_PROJECTION, &projM);
    dev->SetRenderState(D3DRS_LIGHTING, FALSE);
    dev->SetRenderState(D3DRS_ZENABLE, D3DZB_TRUE);
    dev->SetRenderState(D3DRS_ZWRITEENABLE, TRUE);
    dev->SetRenderState(D3DRS_ZFUNC, D3DCMP_LESSEQUAL);
    dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    dev->SetRenderState(D3DRS_ALPHABLENDENABLE, vanish ? TRUE : FALSE);
    dev->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
    dev->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
    dev->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
    dev->SetRenderState(D3DRS_FOGENABLE, FALSE);
    dev->SetRenderState(D3DRS_STENCILENABLE, FALSE);
    dev->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
    dev->SetRenderState(D3DRS_COLORWRITEENABLE, 0xF);
    dev->SetRenderState(D3DRS_FILLMODE, D3DFILL_SOLID);
    dev->SetTexture(0, s_tex);
    dev->SetTexture(1, NULL);
    dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_BLENDTEXTUREALPHA);   // tex where its alpha is, else the colour
    dev->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
    dev->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
    dev->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG2);
    dev->SetTextureStageState(0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE);
    dev->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_MODULATE);      // the light, on the texture as well
    dev->SetTextureStageState(1, D3DTSS_COLORARG1, D3DTA_CURRENT);
    dev->SetTextureStageState(1, D3DTSS_COLORARG2, D3DTA_SPECULAR);
    dev->SetTextureStageState(1, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
    dev->SetTextureStageState(1, D3DTSS_ALPHAARG1, D3DTA_CURRENT);
    dev->SetTextureStageState(2, D3DTSS_COLOROP, D3DTOP_DISABLE);
    dev->SetTextureStageState(2, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
    dev->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
    dev->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
    dev->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
    dev->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
    dev->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
    dev->SetFVF(VTX_FVF);
    // The library marks a triangle with no texture by giving all three corners the uv (1, 1); textured ones carry
    // real atlas coordinates.  They must be drawn apart: a textured triangle takes its colour and its alpha from
    // the atlas, so the clear ground around a sprite - the wings - is thrown away by the alpha test instead of
    // falling back to the vertex colour and showing up as white.
    // Which of the atlas's eleven tiles a triangle samples says how it has to be drawn.  Tiles 1..8 - the
    // overall buttons, the cap's M, the moustache, the sideburns and the four pairs of eyes - are decals laid on
    // Mario himself, and their texture is clear everywhere except the feature itself: cutting those clear texels
    // away left holes that the level showed through, because what is behind them is the very same polygons.  They
    // are blended over the vertex colour instead, which is exactly what they are drawn on - the colour under each
    // of them is his own (skin 254 193 121 under the eyes and the moustache, overall blue under the buttons, cap
    // red under the M).  The wings (9, 10) and the metal shine (0) are sprites with nothing behind them at all,
    // so those keep the alpha test that cuts their clear ground away.
    s_plain.clear();
    s_textured.clear();
    s_decal.clear();
    for (int t = 0; t < s_mario.triangles; ++t) {
        const Vtx* tv = &s_vtx[t * 3];
        bool plain = tv[0].u == 1.0f && tv[0].v == 1.0f && tv[1].u == 1.0f && tv[1].v == 1.0f &&
                     tv[2].u == 1.0f && tv[2].v == 1.0f;
        int tile = (int)((tv[0].u + tv[1].u + tv[2].u) * (11.0f / 3.0f));
        if (tile < 0) tile = 0;
        if (tile > 10) tile = 10;
        std::vector<Vtx>& into = plain ? s_plain : (tile >= 1 && tile <= 8 ? s_decal : s_textured);
        into.push_back(tv[0]);
        into.push_back(tv[1]);
        into.push_back(tv[2]);
    }
    if (s_log) {                                 // what each tile is being used for, and with what colour under it
        static bool once;
        if (!once && s_mario.triangles) {
            once = true;
            for (int tile = 0; tile < 11; ++tile) {
                int n = 0;
                double r = 0, g = 0, b = 0;
                for (int t = 0; t < s_mario.triangles; ++t) {
                    const Vtx& v = s_vtx[t * 3];
                    if (v.u == 1.0f && v.v == 1.0f) continue;
                    int ti = (int)((v.u + s_vtx[t * 3 + 1].u + s_vtx[t * 3 + 2].u) * (11.0f / 3.0f));
                    if (ti != tile) continue;
                    ++n;
                    r += (v.colour >> 16) & 0xFF; g += (v.colour >> 8) & 0xFF; b += v.colour & 0xFF;
                }
                if (n) SmLog("atlas tile %d: %d triangles, mean colour under them %.0f %.0f %.0f", tile, n,
                             r / n, g / n, b / n);
            }
        }
    }
    dev->SetRenderState(D3DRS_ALPHATESTENABLE, TRUE);      // the atlas alpha is 1-bit: 0 or 255
    dev->SetRenderState(D3DRS_ALPHAREF, 8);
    dev->SetRenderState(D3DRS_ALPHAFUNC, D3DCMP_GREATEREQUAL);
    if (!s_plain.empty()) {
        dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG2);
        dev->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
        dev->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG2);
        dev->SetTextureStageState(0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE);
        dev->DrawPrimitiveUP(D3DPT_TRIANGLELIST, (UINT)(s_plain.size() / 3), &s_plain[0], sizeof(Vtx));
    }
    if (!s_textured.empty()) {
        dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
        dev->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
        dev->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_MODULATE);
        dev->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
        dev->SetTextureStageState(0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE);
        dev->DrawPrimitiveUP(D3DPT_TRIANGLELIST, (UINT)(s_textured.size() / 3), &s_textured[0], sizeof(Vtx));
    }
    if (!s_decal.empty()) {                      // the face: the feature where the texture has one, the face where not
        dev->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
        dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_BLENDTEXTUREALPHA);
        dev->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
        dev->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
        dev->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG2);
        dev->SetTextureStageState(0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE);
        dev->DrawPrimitiveUP(D3DPT_TRIANGLELIST, (UINT)(s_decal.size() / 3), &s_decal[0], sizeof(Vtx));
    }
    ObjectsDraw(dev);                            // the enemies and the rest, after him
    HostHeapCheck("after drawing the objects");
    if (s_sceneState) s_sceneState->Apply();
}

void SceneLost() {
    if (s_sceneState) { s_sceneState->Release(); s_sceneState = NULL; }
    if (s_tex) { s_tex->Release(); s_tex = NULL; s_texDevice = NULL; }
    ObjTexturesLost();
}

// ------------------------------------------------------------------------------------------------ SM64's HUD
// The HUD's own pictures come out of the ROM: two MIO0 blocks hold them, the glyphs (16x16) in segment 2 and the
// power meter's wedges (32x32) with the level's textures.  They are decoded once at attach into BGRA and uploaded
// when the device first draws them.  Offsets are the US ROM's.
const uint32_t HUD_SEG2_BLOCK = 1083968;         // MIO0: the HUD's glyphs
const uint32_t HUD_PWR_BLOCK = 2102288;          // MIO0: the power meter
enum { HUD_DIGIT = 0, HUD_MULT = 10, HUD_COIN = 11, HUD_HEAD = 12, HUD_STAR = 13, HUD_PWR = 14,
       HUD_PWR_LEFT = 22, HUD_PWR_RIGHT = 23, HUD_TRANS = 24, HUD_S = 25, HUD_P = 26, HUD_D = 27,
       HUD_M = 28, HUD_LETTER = 29, HUD_DASH = 55, HUD_DOT = 56, HUD_BANG = 57, HUD_COUNT = 58 };

// The US ROM's HUD font has no J, Q, V, X or Z (and no dash or full stop): those are drawn here in its manner,
// white with a dark edge, from these pictures.
const char* const kOwnGlyphs[8][16] = {
    { "................", "................", "......#######...", "......#######...", ".........###....", ".........###....",
      ".........###....", ".........###....", ".........###....", ".........###....", "..###....###....", "..###....###....",
      "..###....###....", "...#########....", "....#######.....", "................" },
    { "................", ".....######.....", "....########....", "...###....###...", "..###......###..", "..###......###..",
      "..###......###..", "..###......###..", "..###......###..", "..###...##.###..", "..###....#####..", "...###....###...",
      "....#########...", ".....#######....", "...........###..", "................" },
    { "................", "..###......###..", "..###......###..", "..###......###..", "..###......###..", "...###....###...",
      "...###....###...", "...###....###...", "....###..###....", "....###..###....", "....###..###....", ".....######.....",
      ".....######.....", "......####......", ".......##.......", "................" },
    { "................", "..###......###..", "..###......###..", "...###....###...", "....###..###....", ".....######.....",
      "......####......", ".......##.......", "......####......", ".....######.....", "....###..###....", "...###....###...",
      "..###......###..", "..###......###..", "................", "................" },
    { "................", "..############..", "..############..", "..############..", "..........###...", ".........###....",
      "........###.....", ".......###......", "......###.......", ".....###........", "....###.........", "...###..........",
      "..############..", "..############..", "..############..", "................" },
    { "................", "................", "................", "................", "................", "................",
      "...##########...", "...##########...", "...##########...", "................", "................", "................",
      "................", "................", "................", "................" },
    { "................", "................", "................", "................", "................", "................",
      "................", "................", "................", "................", "................", "......###.......",
      "......###.......", "......###.......", "................", "................" },
    { "................", "......###.......", "......###.......", "......###.......", "......###.......", "......###.......",
      "......###.......", "......###.......", "......###.......", "......###.......", "................", "................",
      "......###.......", "......###.......", "......###.......", "................" },
};

struct HudTex {
    int w, h;
    std::vector<uint8_t> bgra;
    IDirect3DTexture9* tex;
};
HudTex s_hudTex[HUD_COUNT];
IDirect3DDevice9* s_hudDevice;
bool s_hudReady;                                 // the pictures are decoded

// MIO0: a bit per output run (1 = one literal byte, 0 = a back reference {length 3-18, distance 1-4096}), the two
// streams named by the header
bool Mio0(const uint8_t* rom, size_t size, uint32_t off, std::vector<uint8_t>& out) {
    if ((size_t)off + 16 > size || memcmp(rom + off, "MIO0", 4) != 0) return false;
    uint32_t total = Be32(rom + off + 4), compOff = Be32(rom + off + 8), rawOff = Be32(rom + off + 12);
    if (total > 16u * 1024 * 1024) return false;
    out.clear();
    out.reserve(total);
    size_t bits = off + 16, comp = off + compOff, raw = off + rawOff;
    uint32_t word = 0;
    int have = 0;
    while (out.size() < total) {
        if (!have) {
            if (bits + 4 > size) return false;
            word = Be32(rom + bits);
            bits += 4;
            have = 32;
        }
        if (word & 0x80000000u) {
            if (raw >= size) return false;
            out.push_back(rom[raw++]);
        } else {
            if (comp + 2 > size) return false;
            uint32_t code = (uint32_t)rom[comp] << 8 | rom[comp + 1];
            comp += 2;
            uint32_t len = (code >> 12) + 3, dist = (code & 0xFFF) + 1;
            if (dist > out.size()) return false;
            for (uint32_t i = 0; i < len; ++i) out.push_back(out[out.size() - dist]);
        }
        word <<= 1;
        --have;
    }
    out.resize(total);
    return true;
}

void HudTexFrom(HudTex& t, const std::vector<uint8_t>& seg, uint32_t inner, int w, int h) {
    t.w = w;
    t.h = h;
    t.tex = NULL;
    t.bgra.assign((size_t)w * h * 4, 0);
    if ((size_t)inner + (size_t)w * h * 2 > seg.size()) return;
    for (int i = 0; i < w * h; ++i) {            // RGBA16 (5-5-5-1), big endian
        uint32_t p = (uint32_t)seg[inner + i * 2] << 8 | seg[inner + i * 2 + 1];
        t.bgra[i * 4 + 0] = (uint8_t)(((p >> 1) & 31) * 255 / 31);
        t.bgra[i * 4 + 1] = (uint8_t)(((p >> 6) & 31) * 255 / 31);
        t.bgra[i * 4 + 2] = (uint8_t)(((p >> 11) & 31) * 255 / 31);
        t.bgra[i * 4 + 3] = (uint8_t)((p & 1) ? 255 : 0);
    }
}

// IA8 (4 bits intensity, 4 bits alpha): the transition masks are shapes cut out of a solid field, so what is
// wanted is black paint with the mask's own alpha - opaque outside the shape, clear inside it.
void HudMaskFrom(HudTex& t, const std::vector<uint8_t>& seg, uint32_t inner, int w, int h) {
    t.w = w;
    t.h = h;
    t.tex = NULL;
    t.bgra.assign((size_t)w * h * 4, 0);
    if ((size_t)inner + (size_t)w * h > seg.size()) return;
    for (int i = 0; i < w * h; ++i) t.bgra[i * 4 + 3] = (uint8_t)((seg[inner + i] & 0xF) * 17);
}

// a glyph of this module's own, from a picture above: white where painted, a dark edge next to it, clear elsewhere
void HudOwnGlyph(HudTex& t, const char* const rows[16]) {
    t.w = t.h = 16;
    t.tex = NULL;
    t.bgra.assign(16 * 16 * 4, 0);
    for (int y = 0; y < 16; ++y)
        for (int x = 0; x < 16; ++x) {
            bool on = rows[y][x] == '#';
            bool edge = false;
            for (int dy = -1; dy <= 1 && !on && !edge; ++dy)
                for (int dx = -1; dx <= 1 && !edge; ++dx) {
                    int yy = y + dy, xx = x + dx;
                    if (yy >= 0 && yy < 16 && xx >= 0 && xx < 16 && rows[yy][xx] == '#') edge = true;
                }
            uint8_t* px = &t.bgra[(y * 16 + x) * 4];
            if (on) { px[0] = px[1] = px[2] = 255; px[3] = 255; }
            else if (edge) { px[0] = px[1] = px[2] = 40; px[3] = 255; }
        }
}

void HudDecode(const std::vector<uint8_t>& rom) {
    std::vector<uint8_t> seg;
    if (!Mio0(&rom[0], rom.size(), HUD_SEG2_BLOCK, seg)) { SmLog("the HUD's glyphs are not where expected in the ROM"); return; }
    for (int d = 0; d < 10; ++d) HudTexFrom(s_hudTex[HUD_DIGIT + d], seg, d * 512, 16, 16);
    // the letters: the US ROM carries A-I, K-P, R-U, W and Y after the digits, one after another
    {
        const char* have = "ABCDEFGHIKLMNOPRSTUWY";
        for (int i = 0; have[i]; ++i) HudTexFrom(s_hudTex[HUD_LETTER + (have[i] - 'A')], seg, (10 + i) * 512, 16, 16);
        HudOwnGlyph(s_hudTex[HUD_LETTER + ('J' - 'A')], kOwnGlyphs[0]);
        HudOwnGlyph(s_hudTex[HUD_LETTER + ('Q' - 'A')], kOwnGlyphs[1]);
        HudOwnGlyph(s_hudTex[HUD_LETTER + ('V' - 'A')], kOwnGlyphs[2]);
        HudOwnGlyph(s_hudTex[HUD_LETTER + ('X' - 'A')], kOwnGlyphs[3]);
        HudOwnGlyph(s_hudTex[HUD_LETTER + ('Z' - 'A')], kOwnGlyphs[4]);
        HudOwnGlyph(s_hudTex[HUD_DASH], kOwnGlyphs[5]);
        HudOwnGlyph(s_hudTex[HUD_DOT], kOwnGlyphs[6]);
        HudOwnGlyph(s_hudTex[HUD_BANG], kOwnGlyphs[7]);
    }
    HudTexFrom(s_hudTex[HUD_MULT], seg, 16896, 16, 16);
    HudTexFrom(s_hudTex[HUD_COIN], seg, 17408, 16, 16);
    HudTexFrom(s_hudTex[HUD_HEAD], seg, 17920, 16, 16);
    HudTexFrom(s_hudTex[HUD_STAR], seg, 18432, 16, 16);
    // the block carries a whole alphabet after the digits - 0-9, then A B C D E F G H I K L M N O P R S T U W Y,
    // the two quotes, the X above, the coin, his head and the star.  The speedometer wants three of the letters.
    HudTexFrom(s_hudTex[HUD_S], seg, 26 * 512, 16, 16);
    HudTexFrom(s_hudTex[HUD_P], seg, 24 * 512, 16, 16);
    HudTexFrom(s_hudTex[HUD_D], seg, 13 * 512, 16, 16);
    HudTexFrom(s_hudTex[HUD_M], seg, 21 * 512, 16, 16);   // the font has no minus: M stands for it
    HudMaskFrom(s_hudTex[HUD_TRANS], seg, 82392, 32, 64);        // half of Bowser's head: the death transition
    if (!Mio0(&rom[0], rom.size(), HUD_PWR_BLOCK, seg)) { SmLog("the power meter is not where expected in the ROM"); return; }
    for (int i = 0; i < 8; ++i)                  // full (8 wedges), seven, six, ... one: 32x32, one after another
        HudTexFrom(s_hudTex[HUD_PWR + i], seg, 152544 + i * 2048, 32, 32);
    HudTexFrom(s_hudTex[HUD_PWR_LEFT], seg, 144352, 32, 64);     // the POWER label and the ring around the wedges
    HudTexFrom(s_hudTex[HUD_PWR_RIGHT], seg, 148448, 32, 64);
    s_hudReady = true;
}

void HudLost() {
    for (int i = 0; i < HUD_COUNT; ++i)
        if (s_hudTex[i].tex) { s_hudTex[i].tex->Release(); s_hudTex[i].tex = NULL; }
    s_hudDevice = NULL;
}

bool HudUpload(IDirect3DDevice9* dev, HudTex& t) {
    if (t.tex) return true;
    if (t.bgra.empty()) return false;
    if (FAILED(dev->CreateTexture(t.w, t.h, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &t.tex, NULL))) return false;
    D3DLOCKED_RECT lr;
    if (FAILED(t.tex->LockRect(0, &lr, NULL, 0))) { t.tex->Release(); t.tex = NULL; return false; }
    for (int y = 0; y < t.h; ++y)
        memcpy((uint8_t*)lr.pBits + y * lr.Pitch, &t.bgra[(size_t)y * t.w * 4], (size_t)t.w * 4);
    t.tex->UnlockRect(0);
    return true;
}

struct HudVtx { float x, y, z, rhw; DWORD colour; float u, v; };
const DWORD HUD_FVF = D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1;

void HudSpriteC(IDirect3DDevice9* dev, int which, float x, float y, float s, DWORD colour) {
    if (which < 0 || which >= HUD_COUNT) return;
    HudTex& t = s_hudTex[which];
    if (!HudUpload(dev, t)) return;
    float w = t.w * s, h = t.h * s;
    HudVtx v[4] = {
        { x - 0.5f,     y - 0.5f,     0, 1, colour, 0, 0 },
        { x + w - 0.5f, y - 0.5f,     0, 1, colour, 1, 0 },
        { x - 0.5f,     y + h - 0.5f, 0, 1, colour, 0, 1 },
        { x + w - 0.5f, y + h - 0.5f, 0, 1, colour, 1, 1 },
    };
    dev->SetTexture(0, t.tex);
    dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, v, sizeof(HudVtx));
}

void HudSprite(IDirect3DDevice9* dev, int which, float x, float y, float s) { HudSpriteC(dev, which, x, y, s, 0xFFFFFFFF); }

// a line of the HUD font (capitals, digits, dash, full stop, space), 12 glyph units to the letter; returns how
// wide it came out.  The texture stage is expected to modulate the glyph by the colour (HudElements sets it so).
const float HUD_ADVANCE = 12.0f;
float HudTextWidth(const char* s, float scale) { return (float)strlen(s) * HUD_ADVANCE * scale; }
float HudText(IDirect3DDevice9* dev, const char* s, float x, float y, float scale, DWORD colour) {
    float at = x;
    for (; *s; ++s) {
        char c = *s;
        int which = -1;
        if (c >= 'A' && c <= 'Z') which = HUD_LETTER + (c - 'A');
        else if (c >= 'a' && c <= 'z') which = HUD_LETTER + (c - 'a');
        else if (c >= '0' && c <= '9') which = HUD_DIGIT + (c - '0');
        else if (c == '-') which = HUD_DASH;
        else if (c == '.') which = HUD_DOT;
        else if (c == '!') which = HUD_BANG;
        if (which >= 0) HudSpriteC(dev, which, at, y, scale, colour);
        at += HUD_ADVANCE * scale;
    }
    return at - x;
}

// a number, right to left, at most `digits` of them (SM64 draws its counters with leading zeros suppressed)
int HudDigits(int value, int digits) {           // how many the number will actually take
    if (value < 0) value = 0;
    int n = 0;
    do { ++n; value /= 10; } while (value && n < digits);
    return n;
}

float HudNumber(IDirect3DDevice9* dev, int value, int digits, float x, float y, float s) {
    if (value < 0) value = 0;
    int d[8], n = 0;
    do { d[n++] = value % 10; value /= 10; } while (value && n < digits);
    for (int i = 0; i < n; ++i) HudSprite(dev, HUD_DIGIT + d[n - 1 - i], x + i * 16 * s, y, s);
    return x + n * 16 * s;
}



// SM64's own death transition: the picture closes into Bowser's head and goes black, which is what that game
// does when a life is lost.  The mask is half of the head, mirrored - opaque outside it, clear inside - so the
// shape is a hole that shrinks to nothing, with the rest of the screen painted black around it.  It runs on its
// own clock from the moment his health reaches zero; the script gives the level back and restarts it at 2.5 s,
// and the black lifts just after that.
// The hold is long on purpose: the screen has to stay black until the level has actually been put back (the
// script calls WOR_Reinit a little over two seconds in), or the rabbids are on screen again for a moment before
// the level reloads under them.
const double DIE_CLOSE = 1.6, DIE_HOLD = 3.9, DIE_OPEN = 0.4;

void HudQuad(IDirect3DDevice9* dev, float x0, float y0, float x1, float y1, float u0, float u1, DWORD colour) {
    HudVtx v[4] = {
        { x0 - 0.5f, y0 - 0.5f, 0, 1, colour, u0, 0 },
        { x1 - 0.5f, y0 - 0.5f, 0, 1, colour, u1, 0 },
        { x0 - 0.5f, y1 - 0.5f, 0, 1, colour, u0, 1 },
        { x1 - 0.5f, y1 - 0.5f, 0, 1, colour, u1, 1 },
    };
    dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, v, sizeof(HudVtx));
}

void TransitionDraw(IDirect3DDevice9* dev, const RECT& picture) {
    if (!s_deathAt || !s_hudReady) return;
    double t = NowSeconds() - s_deathAt;
    if (t > DIE_CLOSE + DIE_HOLD + DIE_OPEN) { s_deathAt = 0; return; }
    float left = (float)picture.left, top = (float)picture.top;
    float w = (float)(picture.right - picture.left), h = (float)(picture.bottom - picture.top);
    if (w < 16 || h < 16) return;
    float r0 = sqrtf(w * w + h * h) * 0.5f;      // the hole starts big enough to leave the whole picture showing
    float r = 0.0f, fade = 1.0f;
    if (t < DIE_CLOSE) {
        float k = (float)(t / DIE_CLOSE);
        r = r0 * (1.0f - k * k);                 // slow at first, like SM64's own
    } else if (t > DIE_CLOSE + DIE_HOLD) {
        fade = 1.0f - (float)((t - DIE_CLOSE - DIE_HOLD) / DIE_OPEN);
    }
    DWORD black = D3DCOLOR_ARGB((int)(fade * 255.0f + 0.5f), 255, 255, 255);   // the mask is black already
    float mx = left + w * 0.5f, my = top + h * 0.5f;
    float x0 = mx - r, x1 = mx + r, y0 = my - r, y1 = my + r;
    if (s_hudDevice != dev) { HudLost(); s_hudDevice = dev; }
    dev->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
    dev->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
    dev->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
    dev->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
    dev->SetRenderState(D3DRS_ZENABLE, FALSE);
    dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    dev->SetRenderState(D3DRS_LIGHTING, FALSE);
    dev->SetRenderState(D3DRS_FOGENABLE, FALSE);
    dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
    dev->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
    dev->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_MODULATE);
    dev->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
    dev->SetTextureStageState(0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE);
    dev->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
    dev->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
    dev->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
    dev->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
    dev->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
    dev->SetFVF(HUD_FVF);
    HudTex& mask = s_hudTex[HUD_TRANS];
    if (r > 0.5f && HudUpload(dev, mask)) {      // the head: its left half is the right half mirrored
        dev->SetTexture(0, mask.tex);
        HudQuad(dev, x0, y0, mx, y1, 1, 0, black);
        HudQuad(dev, mx, y0, x1, y1, 0, 1, black);
    }
    dev->SetTexture(0, NULL);                    // and black everywhere outside it
    dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG2);
    dev->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
    dev->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG2);
    DWORD fill = D3DCOLOR_ARGB((int)(fade * 255.0f + 0.5f), 0, 0, 0);
    float sx0 = x0 < left ? left : x0, sx1 = x1 > left + w ? left + w : x1;
    HudQuad(dev, left, top, sx0, top + h, 0, 1, fill);
    HudQuad(dev, sx1, top, left + w, top + h, 0, 1, fill);
    HudQuad(dev, sx0, top, sx1, y0 < top ? top : y0, 0, 1, fill);
    HudQuad(dev, sx0, y1 > top + h ? top + h : y1, sx1, top + h, 0, 1, fill);
}

void HudElements(IDirect3DDevice9* dev, const RECT& picture) {
    if (!s_hudReady) return;
    // it slides down into place when Mario takes the level and back up when he gives it away, so it is still drawn
    // for a moment after he has gone
    bool on = s_hudOn && Driven();
    double since = NowSeconds() - s_hudMoved;
    if (!on && since >= HUD_SLIDE) return;
    float k = (float)(since / HUD_SLIDE);
    if (k > 1.0f) k = 1.0f;
    float eased = 1.0f - (1.0f - k) * (1.0f - k) * (1.0f - k);
    float shown = on ? eased : 1.0f - eased;
    if (s_hudDevice != dev) { HudLost(); s_hudDevice = dev; }
    dev->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
    dev->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
    dev->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
    dev->SetRenderState(D3DRS_ALPHATESTENABLE, TRUE);
    dev->SetRenderState(D3DRS_ALPHAREF, 8);
    dev->SetRenderState(D3DRS_ALPHAFUNC, D3DCMP_GREATEREQUAL);
    dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    dev->SetRenderState(D3DRS_LIGHTING, FALSE);
    dev->SetRenderState(D3DRS_ZENABLE, FALSE);
    dev->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
    dev->SetRenderState(D3DRS_FOGENABLE, FALSE);
    dev->SetRenderState(D3DRS_STENCILENABLE, FALSE);
    dev->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
    dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
    dev->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
    dev->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
    dev->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
    dev->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
    dev->SetTextureStageState(1, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
    dev->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
    dev->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_POINT);
    dev->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
    dev->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
    dev->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
    dev->SetFVF(HUD_FVF);
    dev->SetPixelShader(NULL);
    dev->SetVertexShader(NULL);

    // SM64 lays its HUD out on a 240-high screen: one glyph is 16 of those, and everything sits on one line at the
    // top with the power meter hanging below its label
    // SM64 lays its HUD out on a 320x240 screen, on a 4:3 line centred in the picture.  The surface drawn into is
    // what to measure that against, not the engine's picture rectangle: the widescreen fix stretches the engine's
    // idea of the picture (K3D_ScreenVSize 720 -> 10000), and laying the line out on that put the star count off
    // the side of the window.
    D3DVIEWPORT9 vp;
    if (FAILED(dev->GetViewport(&vp))) return;
    static bool told;
    if (!told) {
        told = true;
        IDirect3DSurface9* rt = NULL;
        D3DSURFACE_DESC d;
        memset(&d, 0, sizeof(d));
        if (SUCCEEDED(dev->GetRenderTarget(0, &rt)) && rt) { rt->GetDesc(&d); rt->Release(); }
        SmLog("HUD: picture %d,%d..%d,%d (%dx%d); viewport %u,%u %ux%u; target %ux%u", picture.left, picture.top,
              picture.right, picture.bottom, picture.right - picture.left, picture.bottom - picture.top,
              vp.X, vp.Y, vp.Width, vp.Height, d.Width, d.Height);
    }
    // the presented rectangle, not the whole target: the engine draws a 1008x729 picture into a 1920x1080 buffer
    // and only that part reaches the window
    float left = (float)picture.left, top = (float)picture.top;
    float w = (float)(picture.right - picture.left), h = (float)(picture.bottom - picture.top);
    if (w < 16 || h < 16) { left = (float)vp.X; top = (float)vp.Y; w = (float)vp.Width; h = (float)vp.Height; }
    float s = h / 240.0f;
    float mid = left + w * 0.5f;
    float y = top + 15.0f * s - (1.0f - shown) * 70.0f * s;    // above the picture until it has slid in
    float unit = 16.0f * s;

    // The counters sit in the corners rather than on SM64's own 320-wide line: this picture is far wider than the
    // one that line was drawn for, and in the middle of it they are in the way of the game.  The power meter is
    // what stays in the middle - it is the one worth looking at while he is being hit.
    float margin = 14.0f * s;

    // Mario's lives, in the left corner
    float x = left + margin;
    HudSprite(dev, HUD_HEAD, x, y, s);
    HudSprite(dev, HUD_MULT, x + unit, y, s);
    HudNumber(dev, s_lives, 2, x + unit * 2.0f, y, s);

    // the power meter: its two halves carry the POWER label and the ring, and the wedge picture for what is left
    // of his health goes in the ring
    int wedges = s_mario.state.health >> 8;
    if (wedges > 8) wedges = 8;
    if (wedges < 1) wedges = 1;
    if (s_mario.valid && wedges < 8) {        // SM64 only shows it once he has lost some, and hides it when full
        float px = mid - 32.0f * s;              // its two halves are 64 wide together, so this is the middle
        HudSprite(dev, HUD_PWR_LEFT, px, y, s);
        HudSprite(dev, HUD_PWR_RIGHT, px + 32.0f * s, y, s);
        // the wedge goes on the ring's hole, whose middle is at (32.1, 34.0) of the two side pieces together,
        // and the wedge's own picture is centred at (16.5, 16.8) of its 32x32
        HudSprite(dev, HUD_PWR + (8 - wedges), px + 15.6f * s, y + 17.2f * s, s);
    }

    // the coins (what the level counts as collected, the number the rabbids' own HUD shows beside the cart; SM64
    // gives it three digits because its own coins stop at 999, and a pile runs past that) and the stars (the XL
    // items), together in the right corner, ending at the edge however long the numbers grow
    // While the long jump cheat is on his speed takes the star counter's place, and SPD with five digits is a good
    // deal wider than a star and two - so whichever is being drawn is measured first and the whole group is hung
    // off the right edge, or the number runs off the side of the window.
    float sign = unit;                           // the minus: the font has no sign, so its M stands in for one
    float speed = s_mario.valid ? s_mario.state.forwardVelocity : 0.0f;
    bool back = speed < 0.0f;
    if (back) speed = -speed;
    int spd = (int)(speed + 0.5f);
    float starW = s_bljOn ? unit * (3.0f + (float)HudDigits(spd, 5)) + (back ? sign : 0.0f)
                          : unit * (2.0f + (float)HudDigits(s_stars + s_sm64Stars, 2));
    float coinW = unit * (2.0f + (float)HudDigits(s_coins + s_sm64Coins, 4));
    float sx = left + w - margin - starW;
    float cx = sx - unit - coinW;
    HudSprite(dev, HUD_COIN, cx, y, s);
    HudSprite(dev, HUD_MULT, cx + unit, y, s);
    HudNumber(dev, s_coins + s_sm64Coins, 4, cx + unit * 2.0f, y, s);
    if (s_bljOn) {
        float at = sx;
        HudSprite(dev, HUD_S, at, y, s);
        HudSprite(dev, HUD_P, at + unit, y, s);
        HudSprite(dev, HUD_D, at + unit * 2.0f, y, s);
        at += unit * 3.0f;
        if (back) {
            HudSprite(dev, HUD_M, at, y, s);
            at += sign;
        }
        HudNumber(dev, spd, 5, at, y, s);
    } else {
        HudSprite(dev, HUD_STAR, sx, y, s);
        HudSprite(dev, HUD_MULT, sx + unit, y, s);
        HudNumber(dev, s_stars + s_sm64Stars, 2, sx + unit * 2.0f, y, s);
    }
    dev->SetTexture(0, NULL);
}

// ------------------------------------------------------------------------------------------------ the T menu
// Every enemy, boss and item libsm64's object engine carries, by category, spawned in front of Mario.  Open and
// shut with T while he has the level; the arrows (or WASD) move through it, left and right change the category,
// Enter or Space spawns, Backspace clears everything spawned.  While it is open his controls are his own no more.
bool s_menuOpen;
std::vector<std::string> s_menuCats;             // the categories, in the order the library lists them
std::vector<std::vector<int> > s_menuEntries;    // the entries of each
int s_menuCat;
std::vector<int> s_menuCursor, s_menuScroll;     // per category
std::string s_menuMessage;                       // what was last done
double s_menuMessageAt;
const int MENU_ROWS = 13;

void MenuBuild() {
    s_menuCats.clear();
    s_menuEntries.clear();
    if (!ObjectsAvailable()) return;
    int n = p_menu_count();
    for (int i = 0; i < n; ++i) {
        std::string cat = p_menu_category(i);
        size_t c = 0;
        for (; c < s_menuCats.size(); ++c) if (s_menuCats[c] == cat) break;
        if (c == s_menuCats.size()) { s_menuCats.push_back(cat); s_menuEntries.push_back(std::vector<int>()); }
        s_menuEntries[c].push_back(i);
    }
    s_menuCursor.assign(s_menuCats.size(), 0);
    s_menuScroll.assign(s_menuCats.size(), 0);
    s_menuCat = 0;
}

void MenuSay(const char* fmt, ...) {
    char buf[128];
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf(buf, sizeof(buf) - 1, fmt, ap);
    va_end(ap);
    buf[sizeof(buf) - 1] = 0;
    s_menuMessage = buf;
    s_menuMessageAt = NowSeconds();
}

// one of the menu's things, about three units in front of him, turned to face him
void MenuSpawn(int entry) {
    if (!s_objReady || s_mario.id < 0 || !s_mario.valid) { MenuSay("MARIO IS NOT HERE"); return; }
    float a = s_mario.state.faceAngle;           // SM64's yaw: he faces along (sin a, cos a) in its x, z
    const float AHEAD = 3.0f / (s_level.scale != 0.0f ? s_level.scale : RGHC_SCALE);   // game units -> SM64
    float x = s_mario.state.position[0] + sinf(a) * AHEAD;
    float y = s_mario.state.position[1] + 10.0f;
    float z = s_mario.state.position[2] + cosf(a) * AHEAD;
    int32_t slot = p_object_spawn(entry, x, y, z, a + 3.14159265f);
    float g[3], sm[3] = { x, y, z };
    ToGame(sm, g);
    if (slot < 0) { MenuSay("NO ROOM FOR %s", p_menu_name(entry)); SmLog("could not spawn %s: the pool is full", p_menu_name(entry)); return; }
    MenuSay("%s", p_menu_name(entry));
    SmLog("spawned %s (slot %d) at %.1f %.1f %.1f, %d objects now", p_menu_name(entry), slot, g[0], g[1], g[2], p_objects_count());
}

void MenuClear() {
    if (!s_objReady) return;
    int had = p_objects_count();
    p_objects_clear();
    MenuSay("CLEARED %d", had);
    SmLog("the menu cleared %d objects", had);
}

bool Focused();                                  // (defined with the controls, below)
// a key of the menu's: pressed once when it goes down, and again while it is held, after a moment
struct MenuKey { int vk; bool down; double since; double next; };
MenuKey s_menuKeys[16];
int s_menuKeyN;

bool MenuKeyPressed(int vk, bool repeats) {
    MenuKey* k = NULL;
    for (int i = 0; i < s_menuKeyN; ++i) if (s_menuKeys[i].vk == vk) { k = &s_menuKeys[i]; break; }
    if (!k) {
        if (s_menuKeyN == 16) return false;
        k = &s_menuKeys[s_menuKeyN++];
        k->vk = vk; k->down = false; k->since = 0; k->next = 0;
    }
    bool down = Focused() && (GetAsyncKeyState(vk) & 0x8000) != 0;
    double now = NowSeconds();
    bool pressed = false;
    if (down && !k->down) { pressed = true; k->since = now; k->next = now + 0.4; }
    else if (down && repeats && now >= k->next) { pressed = true; k->next = now + 0.07; }
    k->down = down;
    return pressed;
}

void MenuFrame() {                               // once a frame, from ToggleFrame
    bool live = s_hudOn && Driven() && s_objReady && !s_menuCats.empty();
    if (MenuKeyPressed('T', false)) {
        if (!live) { if (s_menuOpen) s_menuOpen = false; }
        else {
            s_menuOpen = !s_menuOpen;
            if (s_menuOpen) MenuSay("%d OBJECTS", p_objects_count());
        }
    }
    if (!s_menuOpen) return;
    if (!live) { s_menuOpen = false; return; }
    int ncat = (int)s_menuCats.size();
    std::vector<int>& list = s_menuEntries[s_menuCat];
    int& cursor = s_menuCursor[s_menuCat];
    int& scroll = s_menuScroll[s_menuCat];
    int n = (int)list.size();
    if (MenuKeyPressed(VK_UP, true) || MenuKeyPressed('W', true)) cursor = (cursor + n - 1) % n;
    if (MenuKeyPressed(VK_DOWN, true) || MenuKeyPressed('S', true)) cursor = (cursor + 1) % n;
    if (MenuKeyPressed(VK_PRIOR, true)) cursor = cursor > 0 ? (cursor - MENU_ROWS < 0 ? 0 : cursor - MENU_ROWS) : n - 1;
    if (MenuKeyPressed(VK_NEXT, true)) cursor = cursor < n - 1 ? (cursor + MENU_ROWS >= n ? n - 1 : cursor + MENU_ROWS) : 0;
    if (MenuKeyPressed(VK_LEFT, true) || MenuKeyPressed('A', true)) s_menuCat = (s_menuCat + ncat - 1) % ncat;
    if (MenuKeyPressed(VK_RIGHT, true) || MenuKeyPressed('D', true)) s_menuCat = (s_menuCat + 1) % ncat;
    if (MenuKeyPressed(VK_RETURN, true) || MenuKeyPressed(VK_SPACE, true)) MenuSpawn(list[cursor]);
    if (MenuKeyPressed(VK_BACK, false) || MenuKeyPressed(VK_DELETE, false)) MenuClear();
    if (cursor < scroll) scroll = cursor;
    if (cursor >= scroll + MENU_ROWS) scroll = cursor - MENU_ROWS + 1;
}

// a plain quad of one colour (the panel and the cursor bar), with the texture stage taken off for it
void MenuBox(IDirect3DDevice9* dev, float x0, float y0, float x1, float y1, DWORD colour) {
    dev->SetTexture(0, NULL);
    dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG2);
    dev->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
    dev->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG2);
    dev->SetTextureStageState(0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE);
    HudQuad(dev, x0, y0, x1, y1, 0, 1, colour);
    dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_MODULATE);   // the glyphs: their picture times the colour
    dev->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
    dev->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
    dev->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_MODULATE);
    dev->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
    dev->SetTextureStageState(0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE);
}

void MenuDraw(IDirect3DDevice9* dev, const RECT& picture) {
    if (!s_menuOpen || !s_hudReady || s_menuCats.empty()) return;
    D3DVIEWPORT9 vp;
    if (FAILED(dev->GetViewport(&vp))) return;
    float left = (float)picture.left, top = (float)picture.top;
    float w = (float)(picture.right - picture.left), h = (float)(picture.bottom - picture.top);
    if (w < 16 || h < 16) { left = (float)vp.X; top = (float)vp.Y; w = (float)vp.Width; h = (float)vp.Height; }
    float s = h / 240.0f;                        // the HUD's own unit: the picture is 240 of them tall
    if (s_hudDevice != dev) { HudLost(); s_hudDevice = dev; }
    dev->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
    dev->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
    dev->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
    dev->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
    dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    dev->SetRenderState(D3DRS_LIGHTING, FALSE);
    dev->SetRenderState(D3DRS_ZENABLE, FALSE);
    dev->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
    dev->SetRenderState(D3DRS_FOGENABLE, FALSE);
    dev->SetRenderState(D3DRS_STENCILENABLE, FALSE);
    dev->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
    dev->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
    dev->SetTextureStageState(1, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
    dev->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
    dev->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
    dev->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
    dev->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
    dev->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
    dev->SetFVF(HUD_FVF);
    dev->SetPixelShader(NULL);
    dev->SetVertexShader(NULL);

    const float PX = 12.0f, PY = 26.0f, PW = 216.0f, PH = 196.0f;   // the panel, in HUD units
    float x0 = left + PX * s, y0 = top + PY * s, x1 = x0 + PW * s, y1 = y0 + PH * s;
    MenuBox(dev, x0, y0, x1, y1, D3DCOLOR_ARGB(190, 0, 0, 0));
    const DWORD WHITE = 0xFFFFFFFF, GREY = D3DCOLOR_ARGB(255, 200, 200, 200), YELLOW = D3DCOLOR_ARGB(255, 255, 230, 70);
    float title = 0.62f * s, row = 0.5f * s, tiny = 0.38f * s;   // ("small" is a word of the Windows headers)
    // the title and the count
    HudText(dev, "SPAWN", x0 + 8 * s, y0 + 5 * s, title, WHITE);
    char count[32];
    _snprintf(count, sizeof(count) - 1, "%d OBJECTS", s_objReady ? p_objects_count() : 0);
    count[sizeof(count) - 1] = 0;
    HudText(dev, count, x1 - 8 * s - HudTextWidth(count, row), y0 + 7 * s, row, GREY);
    // the category, between its arrows
    const std::string& cat = s_menuCats[s_menuCat];
    float cw = HudTextWidth(cat.c_str(), row);
    float cx = x0 + (PW * s - cw) * 0.5f;
    HudText(dev, cat.c_str(), cx, y0 + 22 * s, row, YELLOW);
    HudText(dev, "-", cx - 14 * s, y0 + 22 * s, row, GREY);
    HudText(dev, "-", cx + cw + 4 * s, y0 + 22 * s, row, GREY);
    // the list
    const std::vector<int>& list = s_menuEntries[s_menuCat];
    int cursor = s_menuCursor[s_menuCat], scroll = s_menuScroll[s_menuCat];
    float ly = y0 + 38 * s;
    for (int i = 0; i < MENU_ROWS && scroll + i < (int)list.size(); ++i) {
        int entry = list[scroll + i];
        float ry = ly + i * 11.0f * s;
        if (scroll + i == cursor) MenuBox(dev, x0 + 4 * s, ry - 1.5f * s, x1 - 4 * s, ry + 9.5f * s, D3DCOLOR_ARGB(150, 200, 30, 30));
        HudText(dev, p_menu_name(entry), x0 + 10 * s, ry, row, scroll + i == cursor ? WHITE : GREY);
    }
    if (scroll > 0) HudText(dev, "-", x1 - 12 * s, ly - 8 * s, tiny, GREY);
    if (scroll + MENU_ROWS < (int)list.size()) HudText(dev, "-", x1 - 12 * s, ly + MENU_ROWS * 11.0f * s - 2 * s, tiny, GREY);
    // what was last done, and how it is worked
    float fy = y1 - 14 * s;
    if (!s_menuMessage.empty() && NowSeconds() - s_menuMessageAt < 3.0)
        HudText(dev, s_menuMessage.c_str(), x0 + 8 * s, fy - 9 * s, tiny, YELLOW);
    HudText(dev, "ENTER SPAWN  BKSP CLEAR  T CLOSE", x0 + 8 * s, fy, tiny, GREY);
    dev->SetTexture(0, NULL);
}


// ------------------------------------------------------------------------------------------------ the text box
// SM64's objects ask for one (an NPC talking, a boss's opening line, a sign) and the library counts it down; these
// are the words it shows here.  Enter or Space closes it early.
const char* const kDialogText = "OH MY FREAKING GOD IM IN RABBIDS GO HOME! THIS IS SO FREAKING EPIC";

void DialogFrame() {                             // once a frame, from ToggleFrame
    if (s_menuOpen || !p_dialog_state || !p_dialog_close || !Driven()) return;
    int32_t id = -1;
    if (p_dialog_state(&id) <= 0) return;
    if (MenuKeyPressed(VK_RETURN, false) || MenuKeyPressed(VK_SPACE, false)) p_dialog_close();
}

// the words broken into lines of at most `width` letters, at the spaces
void WrapText(const char* text, size_t width, std::vector<std::string>& lines) {
    std::string line, word;
    for (const char* p = text;; ++p) {
        if (*p == ' ' || *p == 0) {
            if (!word.empty()) {
                if (!line.empty() && line.size() + 1 + word.size() > width) { lines.push_back(line); line.clear(); }
                if (!line.empty()) line += ' ';
                line += word;
                word.clear();
            }
            if (*p == 0) break;
        } else word += *p;
    }
    if (!line.empty()) lines.push_back(line);
}

void DialogDraw(IDirect3DDevice9* dev, const RECT& picture) {
    if (!s_hudReady || !p_dialog_state || !Driven()) return;
    int32_t id = -1;
    if (p_dialog_state(&id) <= 0) return;
    D3DVIEWPORT9 vp;
    if (FAILED(dev->GetViewport(&vp))) return;
    float left = (float)picture.left, top = (float)picture.top;
    float w = (float)(picture.right - picture.left), h = (float)(picture.bottom - picture.top);
    if (w < 16 || h < 16) { left = (float)vp.X; top = (float)vp.Y; w = (float)vp.Width; h = (float)vp.Height; }
    float s = h / 240.0f;                        // the HUD's own unit: the picture is 240 of them tall
    if (s_hudDevice != dev) { HudLost(); s_hudDevice = dev; }
    dev->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
    dev->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
    dev->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
    dev->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
    dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    dev->SetRenderState(D3DRS_LIGHTING, FALSE);
    dev->SetRenderState(D3DRS_ZENABLE, FALSE);
    dev->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
    dev->SetRenderState(D3DRS_FOGENABLE, FALSE);
    dev->SetRenderState(D3DRS_STENCILENABLE, FALSE);
    dev->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
    dev->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
    dev->SetTextureStageState(1, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
    dev->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
    dev->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
    dev->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
    dev->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
    dev->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
    dev->SetFVF(HUD_FVF);
    dev->SetPixelShader(NULL);
    dev->SetVertexShader(NULL);

    std::vector<std::string> lines;
    WrapText(kDialogText, 26, lines);
    const float BW = 216.0f, LINE = 11.0f;       // the box, in HUD units: as wide as the menu, a line per row
    float row = 0.5f * s, tiny = 0.38f * s;
    float bh = 16.0f + LINE * (float)lines.size();
    float x0 = left + (w - BW * s) * 0.5f, y0 = top + (222.0f - bh) * s;   // low on the picture, as SM64's sits
    float x1 = x0 + BW * s, y1 = y0 + bh * s;
    MenuBox(dev, x0, y0, x1, y1, D3DCOLOR_ARGB(205, 6, 6, 40));
    const DWORD WHITE = 0xFFFFFFFF, GREY = D3DCOLOR_ARGB(255, 190, 190, 190);
    for (size_t i = 0; i < lines.size(); ++i)
        HudText(dev, lines[i].c_str(), x0 + 12 * s, y0 + (7.0f + LINE * (float)i) * s, row, WHITE);
    HudText(dev, "ENTER", x1 - 10 * s - HudTextWidth("ENTER", tiny), y1 - 7 * s, tiny, GREY);
}

void HudDraw(IDirect3DDevice9* dev, const RECT& picture) {
    HudElements(dev, picture);
    MenuDraw(dev, picture);
    DialogDraw(dev, picture);                    // the text box an object asked for
    TransitionDraw(dev, picture);                // his death closes over everything, his own HUD included
}

// ------------------------------------------------------------------------------------------------ SM64's audio
// libsm64 holds the game's audio engine: its sequence player (the music, with the loop points the sequences carry
// themselves) and its sound effects, which Mario's own code asks for as he moves.  One tick a frame fills a block
// and hands it to the sound card; the engine's own music is turned down while this plays.
enum { AUD_RATE = 32000, AUD_BLOCKS = 4, AUD_BLOCK_I16 = 2 * 544 * 2 };   // a tick fills two sub-buffers, stereo
HWAVEOUT s_wave;
WAVEHDR s_hdr[AUD_BLOCKS];
int16_t s_audio[AUD_BLOCKS][AUD_BLOCK_I16];
bool s_audioReady;                               // the ROM's banks are loaded
bool s_musicOn;                                  // a sequence is playing
double s_levelSeen;                              // when the level's script last ran
int s_musicSeq = 3;                              // [sm64] music: SEQ_LEVEL_GRASS by default
// the level sequences of SM64, in the order F5 and F6 step through them
const struct { int seq; const char* name; } kTracks[] = {
    { 0x03, "Bob-omb Battlefield" }, { 0x04, "inside the castle" }, { 0x05, "Dire Dire Docks" },
    { 0x06, "Lethal Lava Land" },    { 0x08, "Cool Cool Mountain" }, { 0x09, "the slide" },
    { 0x0A, "Big Boo's Haunt" },     { 0x0C, "Hazy Maze Cave" },     { 0x11, "Koopa's Road" },
    { 0x13, "the merry-go-round" },  { 0x07, "Bowser" },             { 0x19, "the final Bowser" },
    { 0x16, "a boss" },              { 0x02, "the title" },
};
const int kTrackCount = (int)(sizeof(kTracks) / sizeof(kTracks[0]));
int s_track;                                     // where in that list the music is
bool s_f5Down, s_f6Down, s_nDown;

// N steps through Mario's cap states.  The flags are SM64's own (MARIO_VANISH_CAP 0x02, MARIO_METAL_CAP 0x04,
// MARIO_WING_CAP 0x08); the library puts one on the way picking one up does, and taking them all off is just the
// flags again without them.
const struct { uint32_t flag; const char* name; } kCaps[] = {
    { 0x00000008, "the wing cap" }, { 0x00000004, "the metal cap" }, { 0x00000002, "the vanish cap" },
    { 0x00000000, "no cap" },
};
const int kCapCount = (int)(sizeof(kCaps) / sizeof(kCaps[0]));
int s_cap = kCapCount - 1;                       // he starts with none

void CapNext() {
    if (s_mario.id < 0) return;
    s_cap = (s_cap + 1) % kCapCount;
    uint32_t flag = kCaps[s_cap].flag;
    if (flag) {
        if (p_interact_cap) p_interact_cap(s_mario.id, flag, 0xFFFF, 1);   // long enough not to run out, with
                                                                           // the cap's own music
    } else if (p_set_mario_state) {
        p_set_mario_state(s_mario.id, s_mario.state.flags & ~0x0000000Eu);
    }
    SmLog("cap: %s", kCaps[s_cap].name);
}
int s_musicVolume = 100;
// The rabbids' own noises while Mario is the one being played.  Not the dialogue group - that is the game's
// speech, and silencing it took the level's sounds with it - but the mix's Characters group (24), which sits
// under effects and holds what the characters themselves make.  [sm64] silence_groups changes the list.
int s_quietGroups[8] = { 24 };
int s_quietGroupN = 1;
bool s_quietApplied;
const int WATER_SEQ = 5;                         // SEQ_LEVEL_WATER: Dire Dire Docks
uint16_t s_waterChannels = 0x0003;               // the two string layers: they are the water
uint16_t s_muteChannels = 0x0E40;                // the drums (9, 10, 11) and the low part on 6: never
int s_waterDyn = -1;
double s_waterQuietFrom;                         // when the strings may be held down (after their fade out)
uint16_t s_maskApplied = 0xFFFF;                 // the mute mask the library is holding
FILE* s_audioDump;                               // [sm64] audio_dump: the ticked samples, raw 32 kHz stereo s16,
std::string s_audioDumpPath;                     // so a capture can be given the sound it cannot record otherwise

bool AudioOpen() {
    if (s_wave) return true;
    WAVEFORMATEX f;
    memset(&f, 0, sizeof(f));
    f.wFormatTag = WAVE_FORMAT_PCM;
    f.nChannels = 2;
    f.nSamplesPerSec = AUD_RATE;
    f.wBitsPerSample = 16;
    f.nBlockAlign = 4;
    f.nAvgBytesPerSec = AUD_RATE * 4;
    if (waveOutOpen(&s_wave, WAVE_MAPPER, &f, 0, 0, CALLBACK_NULL) != MMSYSERR_NOERROR) {
        s_wave = NULL;
        SmLog("no sound card would take 32 kHz stereo: SM64's music and effects are off");
        return false;
    }
    memset(s_hdr, 0, sizeof(s_hdr));
    return true;
}

void AudioClose() {
    if (!s_wave) return;
    waveOutReset(s_wave);
    for (int i = 0; i < AUD_BLOCKS; ++i)
        if (s_hdr[i].lpData) waveOutUnprepareHeader(s_wave, &s_hdr[i], sizeof(WAVEHDR));
    waveOutClose(s_wave);
    s_wave = NULL;
    memset(s_hdr, 0, sizeof(s_hdr));
}

void AudioFrame() {                              // keep the card about two blocks ahead
    if (!s_audioReady || !s_wave || !p_audio_tick) return;
    uint32_t queued = 0;
    for (int i = 0; i < AUD_BLOCKS; ++i)
        if (s_hdr[i].lpData && !(s_hdr[i].dwFlags & WHDR_DONE)) queued += s_hdr[i].dwBufferLength / 4;
    // at most one block a frame: a tick synthesises 2 x 544 samples, which is 34 ms of sound and a real piece of
    // work, and filling every free block in one frame did that four times over and paced the game to 30 Hz
    for (int i = 0; i < AUD_BLOCKS && queued < 2 * 544 * 2; ++i) {
        if (s_hdr[i].lpData && !(s_hdr[i].dwFlags & WHDR_DONE)) continue;
        if (s_hdr[i].lpData) waveOutUnprepareHeader(s_wave, &s_hdr[i], sizeof(WAVEHDR));
        uint32_t n = p_audio_tick(queued, 2 * 544, s_audio[i]);
        if (!n || n * 4 > AUD_BLOCK_I16) break;
        memset(&s_hdr[i], 0, sizeof(WAVEHDR));
        s_hdr[i].lpData = (LPSTR)s_audio[i];
        s_hdr[i].dwBufferLength = n * 8;         // two sub-buffers x two channels x two bytes
        if (!s_audioDumpPath.empty()) {
            if (!s_audioDump) {
                s_audioDump = fopen(s_audioDumpPath.c_str(), "wb");
                SmLog("audio dump %s started at %.3f", s_audioDumpPath.c_str(), NowSeconds());
            }
            if (s_audioDump) fwrite(s_audio[i], 1, n * 8, s_audioDump);
        }
        if (waveOutPrepareHeader(s_wave, &s_hdr[i], sizeof(WAVEHDR)) != MMSYSERR_NOERROR) { s_hdr[i].lpData = NULL; break; }
        if (waveOutWrite(s_wave, &s_hdr[i], sizeof(WAVEHDR)) != MMSYSERR_NOERROR) { s_hdr[i].lpData = NULL; break; }
        queued += n * 2;
        break;                                   // one tick a frame is plenty: each is 34 ms of sound
    }
}

void MusicPick(int step) {                       // F5 / F6 while Mario has the level
    if (!s_audioReady) return;
    s_track = (s_track + step + kTrackCount) % kTrackCount;
    int was = s_musicSeq;
    s_musicSeq = kTracks[s_track].seq;
    if (s_musicOn) {
        if (p_stop_music) p_stop_music((uint16_t)(was & 0xFF));
        if (p_play_music) p_play_music(0, (uint16_t)((4 << 8) | (s_musicSeq & 0xFF)), 0);
    }
    s_waterDyn = -1;                             // the new sequence starts with every channel up: mix it again
    s_waterQuietFrom = 0;
    SmLog("music: %s (sequence %d)", kTracks[s_track].name, s_musicSeq);
}

void MusicStart() {
    if (!s_audioReady || s_musicOn) return;
    if (!AudioOpen()) return;
    if (p_set_sound_volume) p_set_sound_volume(s_musicVolume / 100.0f);
    if (p_play_music) p_play_music(0, (uint16_t)((4 << 8) | (s_musicSeq & 0xFF)), 0);
    OptionsSilenceMusic(true);                   // only the music makes way for SM64's
    s_musicOn = true;
    s_waterDyn = -1;
    s_waterQuietFrom = 0;
    SmLog("SM64 sequence %d playing; the game's own music is down", s_musicSeq);
}

// SM64 answers a death with Bowser's laugh, and the level's music stops under it.  The sound card is opened here
// if the music never did it, so the laugh is heard even with [sm64] music=0.
void DeathSound() {
    if (!s_audioReady || !AudioOpen()) return;
    if (s_musicOn && p_stop_music) p_stop_music((uint16_t)(s_musicSeq & 0xFF));
    if (p_set_sound_volume) p_set_sound_volume(s_musicVolume / 100.0f);
    if (p_play_sound_global) p_play_sound_global(0x70188081);   // SOUND_MENU_BOWSER_LAUGH
    SmLog("he is dead: Bowser's laugh over the transition");
}

// SM64's water music is layered: the sequence carries its instruments on separate channels and the game fades
// whole groups of them with where Mario is (sMusicDynamics, the SEQ_LEVEL_WATER rows - its group is 0x0E43, and
// it brings them back over 200 frames).  The water theme's own channels say which is which: 0 and 1 are the two
// string layers (sustained, 84 notes each), 9, 10 and 11 the drums (kick and snare, hi-hat, ride) and 6 a low
// part.  So the strings are what the water brings in, and the drums stay out of it altogether.  [sm64]
// water_channels and music_mute change both groups without a rebuild.

void WaterMusicDynamic(bool swimming) {
    if (!p_channel_mute) return;
    uint16_t mask = 0;
    if (s_musicOn && s_musicSeq == WATER_SEQ) {
        int want = swimming ? 1 : 0;
        if (want != s_waterDyn) {
            bool fresh = s_waterDyn < 0;         // the sequence just started: nothing to fade out, only to hold
            s_waterDyn = want;
            if (!fresh)
                for (int i = 0; i < 16; ++i)
                    if ((s_waterChannels & (1 << i)) && !(s_muteChannels & (1 << i)) && p_channel_fade)
                        p_channel_fade(0, (uint8_t)i, want ? 127 : 0, want ? 200 : 100);
            s_waterQuietFrom = (want || fresh) ? 0.0 : NowSeconds() + 100.0 / 30.0;
            if (!fresh) SmLog("water music: %s", want ? "he is in it, the strings fade in"
                                                     : "out of it, the strings fade out");
        }
        mask = s_muteChannels;
        if (!want && NowSeconds() >= s_waterQuietFrom) mask |= s_waterChannels;
    } else {
        s_waterDyn = -1;
    }
    if (mask != s_maskApplied) {
        p_channel_mute(0, mask);                 // held inside the library's own tick, so a sequence starting
        s_maskApplied = mask;                    // again cannot get a note out before it takes
    }
}

void MusicStop() {
    if (!s_musicOn) return;
    if (p_stop_music) {
        p_stop_music((uint16_t)(s_musicSeq & 0xFF));
        p_stop_music(0x0E);                      // a cap's own music outlives the track it interrupted, so the
        p_stop_music(0x0F);                      // powerup and metal cap sequences go too
    }
    OptionsSilenceMusic(false);
    s_musicOn = false;
    s_cap = kCapCount - 1;                       // he comes back capless, so the next N starts the cycle over
}

// ------------------------------------------------------------------------------------------------ controls
// Mario's controls, read here so they do not depend on the cart's binding page: keyboard (arrows / WASD, Space A,
// left mouse or F = B, Ctrl or right mouse = Z) and an XInput pad (left stick, A, X = B, B or a trigger = Z, right
// stick = look).  Mouse look: while a script keeps asking for it (SM64_MouseLookGet every frame), the cursor is
// hidden and kept at the window's centre; the deltas accumulate between calls.
struct XState { DWORD packet; WORD buttons; BYTE lt, rt; SHORT lx, ly, rx, ry; };
typedef DWORD (WINAPI* XGetStateFn)(DWORD, XState*);
XGetStateFn s_xGet;
bool s_xTried;
double s_lookWantedUntil;                        // NowSeconds() until which mouse look is wanted
bool s_cursorHidden;
float s_lookDx, s_lookDy;
bool s_lookCentred;

bool Focused() {
    HWND w = GetForegroundWindow();
    if (!w) return false;
    DWORD pid = 0;
    GetWindowThreadProcessId(w, &pid);
    return pid == GetCurrentProcessId();
}

bool PadState(XState& st) {
    if (!s_xTried) {
        s_xTried = true;
        static const char* const dlls[] = { "xinput1_4.dll", "xinput1_3.dll", "xinput9_1_0.dll" };
        for (int i = 0; i < 3 && !s_xGet; ++i) {
            HMODULE m = LoadLibraryA(dlls[i]);
            if (m) s_xGet = (XGetStateFn)GetProcAddress(m, "XInputGetState");
        }
    }
    if (!s_xGet) return false;
    static double nextScan;                      // an empty port is slow to query: once a second
    static int idx = -1;
    double now = NowSeconds();
    if (idx >= 0) {
        if (s_xGet((DWORD)idx, &st) == ERROR_SUCCESS) return true;
        idx = -1;
    }
    if (now < nextScan) return false;
    nextScan = now + 1.0;
    for (int i = 0; i < 4; ++i)
        if (s_xGet((DWORD)i, &st) == ERROR_SUCCESS) { idx = i; return true; }
    return false;
}

bool s_togglePending;                            // M was pressed this frame: the script's next SM64_ToggleGet takes it
bool s_mDown;
bool s_kDown;

// K: what is around him, for the log - where he stands, what libsm64 has under him, and every movable thing
// within twenty units with its state.  For finding out why a thing that should stop him does not.
void DynReport() {
    if (s_mario.id < 0) {
        SmLog("K: Mario is not in the level (press M first): nothing to report; %u live objects known of %u worlds, engine routines %s",
              (unsigned)s_dyn.size(), (unsigned)s_dynWorlds.size(), s_engineObjOk ? "verified" : "NOT verified");
        return;
    }
    float g[3];
    ToGame(s_mario.state.position, g);
    SmLog("K: Mario at %.1f %.1f %.1f (SM64 %.0f %.0f %.0f), action %08X, window centre %.0f %.0f %.0f gen %u, %d movables placed",
          g[0], g[1], g[2], s_mario.state.position[0], s_mario.state.position[1], s_mario.state.position[2],
          s_mario.state.action, s_centre[0], s_centre[1], s_centre[2], s_windowGen, s_dynPlaced);
    for (size_t i = 0; i < s_dyn.size(); ++i) {
        DynObj& d = s_dyn[i];
        if (!ModsWorldSeen(d.world)) continue;
        void* obj = NULL;
        bool on = false;
        float m[12] = { 0 };
        bool okRead = s_engineObjOk && DynProbe(d.key, d.rank, !d.faulted, &obj, &on, m);
        if (s_engineObjOk && !okRead) { d.faulted = true; obj = NULL; on = false; }
        float p[3] = { m[9], m[10], m[11] };
        float dist = (obj && on) ? sqrtf((p[0] - g[0]) * (p[0] - g[0]) + (p[1] - g[1]) * (p[1] - g[1]) + (p[2] - g[2]) * (p[2] - g[2])) : 1e9f;
        if (d.faulted) dist = 0.0f;              // always worth a line
        if (dist > 20.0f) continue;
        float lo[3] = { 1e9f, 1e9f, 1e9f }, hi[3] = { -1e9f, -1e9f, -1e9f };
        for (size_t t = 0; t < d.surf.size(); ++t)
            for (int v = 0; v < 3; ++v)
                for (int c = 0; c < 3; ++c) {
                    float sm = (float)d.surf[t].vertices[v][c];
                    if (sm < lo[c]) lo[c] = sm;
                    if (sm > hi[c]) hi[c] = sm;
                }
        SmLog("K: movable %08X (collision %u, %u tris): %s, %s, at %.1f %.1f %.1f, %.1f away; libsm64 id %u gen %u, placed SM64 x %.0f..%.0f y %.0f..%.0f z %.0f..%.0f",
              d.key, d.rank, (unsigned)(d.tris.size() / 9), d.faulted ? "FAULTED (left alone)" : obj ? "live" : "NOT FOUND",
              on ? "on" : "off (or its place unread)", p[0], p[1], p[2], dist, d.libId, d.gen, lo[0], hi[0], lo[1], hi[1], lo[2], hi[2]);
    }
}

void ToggleFrame() {                             // once per frame: a held key counts once
    bool focused = Focused();
    bool down = focused && (GetAsyncKeyState('M') & 0x8000) != 0;
    // the press waits for the script to take it rather than lasting one frame: on the frame its track changes the
    // script does not poll, and a press landing there used to be dropped and feel like a cooldown
    bool live = (NowSeconds() - s_levelSeen) < 0.5;
    if (down && !s_mDown && live) s_togglePending = true;
    if (!live) s_togglePending = false;          // nothing is running to take it
    s_mDown = down;
    bool n = focused && (GetAsyncKeyState('N') & 0x8000) != 0;
    if (s_hudOn && n && !s_nDown) CapNext();
    s_nDown = n;
    bool kk = focused && (GetAsyncKeyState('K') & 0x8000) != 0;
    if (kk && !s_kDown) DynReport();             // with or without Mario: a press always leaves a line
    s_kDown = kk;
    MenuFrame();                                 // T, and the menu's own keys while it is open
    DialogFrame();                               // Enter or Space closes a text box
    bool e = focused && (GetAsyncKeyState('E') & 0x8000) != 0;
    if (s_hudOn && e && !s_eDown) {
        s_bljOn = !s_bljOn;
        SmLog("the long jump cheat is %s", s_bljOn ? "on: a backwards long jump is slammed back down" : "off");
    }
    s_eDown = e;
    bool q = focused && (GetAsyncKeyState('Q') & 0x8000) != 0;
    if (s_hudOn && q && !s_qDown) {
        s_rapidOn = !s_rapidOn;
        SmLog("the jump is %s", s_rapidOn ? "being pressed for you" : "yours again");
    }
    s_qDown = q;
    bool f5 = focused && (GetAsyncKeyState(VK_F5) & 0x8000) != 0;
    bool f6 = focused && (GetAsyncKeyState(VK_F6) & 0x8000) != 0;
    if (s_hudOn) {                               // stepping through the music is Mario's, like the music itself
        if (f5 && !s_f5Down) MusicPick(-1);
        if (f6 && !s_f6Down) MusicPick(+1);
    }
    s_f5Down = f5;
    s_f6Down = f6;
}

void MouseLookFrame() {                          // once per frame: capture the cursor while wanted
    bool want = NowSeconds() < s_lookWantedUntil && Focused();
    HWND w = GfxWindow();
    if (!want || !w) {
        if (s_cursorHidden) { ShowCursor(TRUE); s_cursorHidden = false; }
        s_lookCentred = false;
        return;
    }
    RECT r;
    if (!GetClientRect(w, &r)) return;
    POINT c = { (r.right - r.left) / 2, (r.bottom - r.top) / 2 };
    ClientToScreen(w, &c);
    POINT p;
    if (s_lookCentred && GetCursorPos(&p)) {
        s_lookDx += (float)(p.x - c.x);
        s_lookDy += (float)(p.y - c.y);
    }
    SetCursorPos(c.x, c.y);
    s_lookCentred = true;
    if (!s_cursorHidden) { ShowCursor(FALSE); s_cursorHidden = true; }
}

// ------------------------------------------------------------------------------------------------ script natives
// Handlers follow the VM's convention (void* h(void* ip), arguments popped last first, the result pushed).  Words:
// scope 0xFFFF (global engine functions), ids 0x7D00.. (unused by the game).
inline uint32_t VmIndex() { return *(uint32_t*)0x00A7E188; }
inline uint32_t VmOffset() { return *(uint32_t*)0x00A7E18C; }
inline uint8_t* VmData() { return *(uint8_t**)0x00A7E198; }
inline uint32_t* VmPtrs() { return *(uint32_t**)0x00A7E19C; }
uint32_t Pop() {
    uint32_t idx = VmIndex() - 1;
    *(uint32_t*)0x00A7E188 = idx;
    *(uint32_t*)0x00A7E18C -= 4;
    return *(uint32_t*)(uintptr_t)VmPtrs()[idx];
}
float PopF() { uint32_t v = Pop(); float f; memcpy(&f, &v, 4); return f; }
void PopVec(float* out) {                        // a 12-byte entry
    uint32_t idx = VmIndex() - 1;
    *(uint32_t*)0x00A7E188 = idx;
    *(uint32_t*)0x00A7E18C -= 12;
    memcpy(out, (const void*)(uintptr_t)VmPtrs()[idx], 12);
}
void PushBytes(const void* p, uint32_t size) {
    uint8_t* data = VmData();
    uint32_t off = VmOffset();
    memcpy(data + off, p, size);
    VmPtrs()[VmIndex()] = (uint32_t)(uintptr_t)(data + off);
    *(uint32_t*)0x00A7E188 += 1;
    *(uint32_t*)0x00A7E18C += size;
}
void PushI(int32_t v) { PushBytes(&v, 4); }
void PushF(float v) { PushBytes(&v, 4); }
void PushVec(const float* v) { PushBytes(v, 12); }

// SM64_Ready() -> int: 1 when libsm64 runs with the ROM and the level's surfaces are known
void* __cdecl N_Ready(void* ip) {
    ScanCollisionSets();
    PushI(s_libReady && (s_surfacesGiven || !s_sets.empty() || EnsureSurfaces()) ? 1 : 0);
    return (uint8_t*)ip + 4;
}
// SM64_MarioCreate(vector pos) -> int id (-1: none)
void* __cdecl N_MarioCreate(void* ip) { float p[3]; PopVec(p); PushI(MarioCreate(p)); return (uint8_t*)ip + 4; }
// SM64_MarioCreateAtStart() -> int id: at the level's own Mario start
void* __cdecl N_MarioCreateAtStart(void* ip) {
    float g[3] = { 0, 0, 0 };
    if (EnsureSurfaces()) ToGame(s_level.spawn, g);
    PushI(MarioCreate(g));
    return (uint8_t*)ip + 4;
}
// SM64_MarioInput(float stickX, float stickY, int buttons (1 A, 2 B, 4 Z), vector camLook (game units, direction))
void* __cdecl N_MarioInput(void* ip) {
    float look[3];
    PopVec(look);
    uint32_t buttons = Pop();
    float sy = PopF(), sx = PopF();
    if (s_mario.id >= 0) {
        s_mario.inputs.stickX = sx;
        s_mario.inputs.stickY = sy;
        s_mario.inputs.buttonA = (buttons & 1) ? 1 : 0;
        s_jumpHeld = s_mario.inputs.buttonA != 0;
        if (!s_jumpHeld) s_rapidPhase = 1;        // so the first press of a held jump is not waited for
        s_mario.inputs.buttonB = (buttons & 2) ? 1 : 0;
        s_mario.inputs.buttonZ = (buttons & 4) ? 1 : 0;
        s_mario.inputs.camLookX = look[0];       // a direction: the game's x, -y is SM64's x, z
        s_mario.inputs.camLookZ = -look[1];
    }
    return (uint8_t*)ip + 4;
}
// SM64_MarioTick(float dt) -> int: the ticks run (30 Hz steps owed by the time passed)
void* __cdecl N_MarioTick(void* ip) {
    float dt = PopF();
    int n = 0;
    s_lastTick = NowSeconds();
    if (s_mario.id >= 0 && s_libReady) {
        if (dt < 0) dt = 0;
        if (dt > 0.25f) dt = 0.25f;
        s_accum += dt;
        while (s_accum >= 1.0 / 30.0 && n < 4) { MarioTick(); s_accum -= 1.0 / 30.0; ++n; }
        if (n) HostHeapCheck("after a step");
        if (n == 4) s_accum = 0;
    }
    PushI(n);
    return (uint8_t*)ip + 4;
}
// SM64_MarioPosGet() -> vector (game units)
void* __cdecl N_MarioPosGet(void* ip) {
    float g[3] = { 0, 0, 0 };
    if (s_mario.id >= 0) ToGame(s_mario.state.position, g);
    PushVec(g);
    return (uint8_t*)ip + 4;
}
// SM64_MarioPosSet(vector pos)
void* __cdecl N_MarioPosSet(void* ip) {
    float g[3], sm[3];
    PopVec(g);
    if (s_mario.id >= 0) {
        // Two very different things come through here: a lift carrying him a little, every frame, and the level
        // putting him somewhere else.  A small move takes the step before it along, so what is drawn stays a blend
        // of two places he really was; a big one is a teleport, and blending through it would drag him across the
        // level in front of the player.
        float had[3];
        ToGame(s_mario.state.position, had);
        float d[3] = { g[0] - had[0], g[1] - had[1], g[2] - had[2] };
        float moved = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);   // ("far" is one of windows.h's own words)
        if (moved > 0.001f) { ++s_moveN; s_moveSum += moved; if (moved > s_moveMax) s_moveMax = moved; }
        if (d[0] * d[0] + d[1] * d[1] + d[2] * d[2] > 4.0f) {
            InterpBreak();
        } else {
            // His body has to go with him now, not at the next step.  The triangles are only rebuilt by a step,
            // and a step is 33 ms - at the rate this game draws that is a dozen frames of him standing where he
            // was while the camera, which is hung off this position, has already left.  So the triangles are
            // carried the same distance, in the units they are held in, and the step kept for the blend with them.
            float dsm[3] = { d[0] / s_level.scale, d[2] / s_level.scale, -d[1] / s_level.scale };
            for (int i = 0; i < s_mario.triangles * 3; ++i) {
                s_mario.position[i * 3 + 0] += dsm[0];
                s_mario.position[i * 3 + 1] += dsm[1];
                s_mario.position[i * 3 + 2] += dsm[2];
            }
            if (s_mario.interp) {
                for (size_t i = 0; i + 2 < s_mario.prevGame.size(); i += 3) {
                    s_mario.prevGame[i] += d[0];
                    s_mario.prevGame[i + 1] += d[1];
                    s_mario.prevGame[i + 2] += d[2];
                }
                s_mario.prevRoot[0] += d[0];
                s_mario.prevRoot[1] += d[1];
                s_mario.prevRoot[2] += d[2];
            }
        }
        ToSm64(g, sm);
        p_set_position(s_mario.id, sm[0], sm[1], sm[2]);
        s_mario.state.position[0] = sm[0];       // the state is only refreshed by a tick, and this moved him now
        s_mario.state.position[1] = sm[1];
        s_mario.state.position[2] = sm[2];
    }
    return (uint8_t*)ip + 4;
}
// SM64_MarioPosSmooth() -> vector: where he is being drawn, which is between the last two steps.  The camera and
// the cart hang off this rather than off the step itself, or they move at 30 Hz while he moves smoothly.
void* __cdecl N_MarioPosSmooth(void* ip) {
    float g[3] = { 0, 0, 0 };
    if (s_mario.id >= 0) {
        ToGame(s_mario.state.position, g);
        if (InterpOn()) {
            float k = InterpDelta();
            for (int c = 0; c < 3; ++c) g[c] = s_mario.prevRoot[c] + (g[c] - s_mario.prevRoot[c]) * k;
        }
    }
    PushVec(g);
    return (uint8_t*)ip + 4;
}
// SM64_MarioLaunch(vector velocity, int flip): throw him, the velocity in the game's units a second.  flip = 1
// puts him in his front flip (SM64's triple jump, which lands by itself and keeps turning until it does), 0 in a
// plain fall.  The script uses it to spawn him with a hop when M is pressed, and with nothing but a fall when
// the cart is already in the air - so nothing pressed twice can lift him.
void* __cdecl N_MarioLaunch(void* ip) {
    uint32_t flip = Pop();
    float v[3];
    PopVec(v);
    if (s_mario.id >= 0 && p_set_action && p_set_velocity) {
        // game units a second -> SM64 units a step, and the game's axes to SM64's (its y up, its z the game's -y)
        float k = 1.0f / (s_level.scale != 0.0f ? s_level.scale : RGHC_SCALE) / 30.0f;
        float sx = v[0] * k, sy = v[2] * k, sz = -v[1] * k;
        p_set_action(s_mario.id, flip ? ACT_TRIPLE_JUMP : ACT_FREEFALL);   // sets the jump's own velocity...
        p_set_velocity(s_mario.id, sx, sy, sz);                             // ...which this replaces
        float fwd = sqrtf(sx * sx + sz * sz);
        if (p_set_forward_velocity) p_set_forward_velocity(s_mario.id, fwd);
        if (fwd > 0.01f && p_set_faceangle) p_set_faceangle(s_mario.id, atan2f(sx, sz));
        s_flipLoop = flip != 0;
        s_flipFrame = -1;
        s_launchHold = fwd > 0.01f;
        s_launchFwd = fwd;
        SmLog("launched at %.1f %.1f %.1f a second%s", v[0], v[1], v[2], flip ? ", flipping" : "");
    }
    return (uint8_t*)ip + 4;
}
// SM64_MarioFlip(): keep him in the front flip with no motion of his own - for when something of the game's is
// carrying him (a jump panel taking the cart along its arc) and the script puts him where it went every frame
void* __cdecl N_MarioFlip(void* ip) {
    if (s_mario.id >= 0 && p_set_action && p_set_velocity) {
        if (s_mario.state.action != ACT_TRIPLE_JUMP) {
            p_set_action(s_mario.id, ACT_TRIPLE_JUMP);
            s_mario.state.action = ACT_TRIPLE_JUMP;
            s_flipLoop = true;
            s_flipFrame = -1;
        }
        p_set_velocity(s_mario.id, 0.0f, 0.0f, 0.0f);
        if (p_set_forward_velocity) p_set_forward_velocity(s_mario.id, 0.0f);
        s_launchHold = false;
    }
    return (uint8_t*)ip + 4;
}
// SM64_MarioWind(vector accel): a fan is blowing on him - accel in the game's units a second each second, the
// push the fan gives the cart's dynamics.  Given every frame it blows; applied at the next step.
void* __cdecl N_MarioWind(void* ip) {
    float v[3];
    PopVec(v);
    if (s_mario.id >= 0) {
        s_wind[0] = v[0]; s_wind[1] = v[1]; s_wind[2] = v[2];
        s_windOn = true;
    }
    return (uint8_t*)ip + 4;
}
// SM64_MarioFaceGet() -> vector: the way he faces, a unit direction in the game's plane.  SM64 faces along
// (sin a, cos a) in its x, z; its z is the game's -y.
void* __cdecl N_MarioFaceGet(void* ip) {
    float g[3] = { 0, -1, 0 };
    if (s_mario.id >= 0) {
        float a = s_mario.state.faceAngle;
        g[0] = sinf(a);
        g[1] = -cosf(a);
    }
    PushVec(g);
    return (uint8_t*)ip + 4;
}
// SM64_MarioAngleGet() -> float: Mario's facing, radians in the game's plane (0 = +x, counter-clockwise)
void* __cdecl N_MarioAngleGet(void* ip) {
    float a = 0;
    if (s_mario.id >= 0) a = s_mario.state.faceAngle;      // SM64: radians, 0 = +z (game -y); turning is mirrored
    PushF(a);
    return (uint8_t*)ip + 4;
}
// SM64_MarioStateGet(int what) -> int: 0 action, 1 health, 2 animID, 3 flags, 4 particle flags, 5 forward speed x1000
void* __cdecl N_MarioStateGet(void* ip) {
    uint32_t what = Pop();
    int32_t v = 0;
    if (s_mario.id >= 0) {
        switch (what) {
        case 0: v = (int32_t)s_mario.state.action; break;
        case 1: v = s_mario.state.health; break;
        case 2: v = s_mario.state.animID; break;
        case 3: v = (int32_t)s_mario.state.flags; break;
        case 4: v = (int32_t)s_mario.state.particleFlags; break;
        case 5: v = (int32_t)(s_mario.state.forwardVelocity * 1000.0f); break;
        default: break;
        }
    }
    PushI(v);
    return (uint8_t*)ip + 4;
}
// SM64_MarioActionSet(int action)
void* __cdecl N_MarioActionSet(void* ip) { uint32_t a = Pop(); if (s_mario.id >= 0) p_set_action(s_mario.id, a); return (uint8_t*)ip + 4; }
// SM64_MarioDamage(int damage, vector from)
void* __cdecl N_MarioDamage(void* ip) {
    float g[3], sm[3];
    PopVec(g);
    uint32_t dmg = Pop();
    if (s_mario.id >= 0) { ToSm64(g, sm); p_take_damage(s_mario.id, dmg, 0, sm[0], sm[1], sm[2]); }
    return (uint8_t*)ip + 4;
}
// SM64_MarioDelete()
void* __cdecl N_MarioDelete(void* ip) {
    if (s_mario.id >= 0) { p_mario_delete(s_mario.id); s_mario.id = -1; s_mario.valid = false; SmLog("Mario deleted"); }
    ObjectsForget();                             // the library takes what he spawned with him
    s_menuOpen = false;
    s_bedOn = false;                             // the swimming bed goes with him
    DynClear();                                  // and the movable things, placed again for the next Mario
    s_launchHold = false;
    s_flipLoop = false;
    return (uint8_t*)ip + 4;
}
// SM64_ControlsGet() -> vector (stick x, stick y, buttons: 1 A, 2 B, 4 Z), from the keyboard and the pad
void* __cdecl N_ControlsGet(void* ip) {
    float v[3] = { 0, 0, 0 };
    if (Focused() && !s_menuOpen) {              // the menu has the keys while it is open
        bool l = ((GetAsyncKeyState(VK_LEFT) | GetAsyncKeyState('A')) & 0x8000) != 0;
        bool r = ((GetAsyncKeyState(VK_RIGHT) | GetAsyncKeyState('D')) & 0x8000) != 0;
        bool u = ((GetAsyncKeyState(VK_UP) | GetAsyncKeyState('W')) & 0x8000) != 0;
        bool d = ((GetAsyncKeyState(VK_DOWN) | GetAsyncKeyState('S')) & 0x8000) != 0;
        v[0] = (r ? 1.0f : 0.0f) - (l ? 1.0f : 0.0f);
        v[1] = (d ? 1.0f : 0.0f) - (u ? 1.0f : 0.0f);
        if (v[0] != 0 && v[1] != 0) { v[0] *= 0.7071f; v[1] *= 0.7071f; }
        int b = 0;
        if (GetAsyncKeyState(VK_SPACE) & 0x8000) b |= 1;   // Q, if it is on, takes this apart in the tick
        if ((GetAsyncKeyState(VK_LBUTTON) | GetAsyncKeyState('F')) & 0x8000) b |= 2;
        if ((GetAsyncKeyState(VK_CONTROL) | GetAsyncKeyState(VK_RBUTTON)) & 0x8000) b |= 4;
        XState st;
        if (PadState(st)) {
            float sx = st.lx / 32767.0f, sy = st.ly / 32767.0f;
            if (fabsf(sx) > 0.2f || fabsf(sy) > 0.2f) { v[0] = sx; v[1] = -sy; }
            if (st.buttons & 0x1000) b |= 1;                     // A
            if (st.buttons & 0x4000) b |= 2;                     // X
            if ((st.buttons & 0x2000) || st.lt > 60 || st.rt > 60) b |= 4;   // B, a trigger
            if (fabsf(st.rx / 32767.0f) > 0.2f) s_lookDx += st.rx / 32767.0f * 12.0f;
            if (fabsf(st.ry / 32767.0f) > 0.2f) s_lookDy -= st.ry / 32767.0f * 12.0f;
        }
        v[2] = (float)b;
    }
    PushVec(v);
    return (uint8_t*)ip + 4;
}
// SM64_ThreatGet(int k): where the k-th enemy or boss Mario has spawned stands, in the game's units - or a point
// far below the world when there is no k-th one.  SM64_ThreatCount(): how many there are.
void* __cdecl N_ThreatGet(void* ip) {
    int32_t k = (int32_t)Pop();
    float v[3] = { 0.0f, 0.0f, -5000.0f };
    if (k >= 0 && k < s_threatCount) { v[0] = s_threats[k][0]; v[1] = s_threats[k][1]; v[2] = s_threats[k][2]; }
    PushVec(v);
    return (uint8_t*)ip + 4;
}
void* __cdecl N_ThreatCount(void* ip) {
    uint32_t n = (uint32_t)s_threatCount;
    PushBytes(&n, 4);
    return (uint8_t*)ip + 4;
}
// SM64_PreyReport(vector pos, object me): a human of the level saying where it is (every frame, from its reflex
// track).  1 back when one of SM64's enemies has touched it since it last asked: its track takes the hit.
void* __cdecl N_PreyReport(void* ip) {
    uint32_t obj = Pop();
    float g[3];
    PopVec(g);
    uint32_t hit = 0;
    if (Driven() && p_prey_set) {
        bool found = false;
        for (size_t i = 0; i < s_preyReports.size(); ++i)
            if (s_preyReports[i].obj == obj) { memcpy(s_preyReports[i].g, g, 12); found = true; break; }
        if (!found && s_preyReports.size() < 64) { PreyReport r; r.obj = obj; memcpy(r.g, g, 12); s_preyReports.push_back(r); }
        for (size_t i = 0; i < s_preyHitPending.size(); ++i)
            if (s_preyHitPending[i] == obj) { hit = 1; s_preyHitPending.erase(s_preyHitPending.begin() + i); break; }
    }
    PushBytes(&hit, 4);
    return (uint8_t*)ip + 4;
}
// SM64_CountsSet(int coins, int stars): what the HUD counts, from the game's own (the rabbids in the cart and the
// XL items it has collected)
void* __cdecl N_CountsSet(void* ip) {
    int32_t stars = (int32_t)Pop(), coins = (int32_t)Pop();
    s_coins = coins < 0 ? 0 : coins;
    s_stars = stars < 0 ? 0 : stars;
    return (uint8_t*)ip + 4;
}
// SM64_LightSet(int ambient, int ambient2): the world's own ambient terms, so Mario is lit like the level he is
// standing in rather than at full brightness
void* __cdecl N_LightSet(void* ip) {
    uint32_t a2 = Pop(), a1 = Pop();             // popped last first
    SetAmbient(s_ambTop, a1);
    SetAmbient(s_ambBottom, a2);
    return (uint8_t*)ip + 4;
}
// SM64_Sound(int which) -> void: one of his own sounds (0 = the coin ding when the level counts something in)
void* __cdecl N_Sound(void* ip) {
    int which = (int)Pop();
    if (s_audioReady && AudioOpen() && p_play_sound_global) {
        if (p_set_sound_volume) p_set_sound_volume(s_musicVolume / 100.0f);
        p_play_sound_global(which == 0 ? 0x38118081 : 0x70188081);   // SOUND_GENERAL_COIN / BOWSER_LAUGH
    }
    return (uint8_t*)ip + 4;
}
// SM64_ModeSet(int on) / SM64_ModeGet() -> int: whether the level is Mario's.  The script model is built again
// from nothing when the world is reinitialised - after a death, or when the rabbids fall - and the mode has to
// survive that, so it is kept here rather than in the model's own variables.
void* __cdecl N_ModeSet(void* ip) {
    s_modeOn = Pop() != 0;
    return (uint8_t*)ip + 4;
}
void* __cdecl N_ModeGet(void* ip) {
    PushI(s_modeOn ? 1 : 0);
    return (uint8_t*)ip + 4;
}
// SM64_Channel(int channel, int volume): one channel of the music at that volume (0..127), for working out
// which instrument is on which channel and for tuning the water mix
void* __cdecl N_Channel(void* ip) {
    int vol = (int)Pop();
    int ch = (int)Pop();
    if (p_channel_fade && ch >= 0 && ch < 16) p_channel_fade(0, (uint8_t)ch, (uint8_t)(vol < 0 ? 0 : vol > 127 ? 127 : vol), 5);
    return (uint8_t*)ip + 4;
}
// SM64_WaterSet(float z): where the water's surface is, in the game's height.  The game is what knows it - the
// cart says when it is on water (PJ_BUGROB_Jetski_IsOn) - so the script hands the height over and SM64 swims him.
void* __cdecl N_WaterSet(void* ip) {
    float z = PopF();
    if (s_mario.id >= 0 && p_set_water_level) {
        int level = (int)((z - s_level.at[2]) / (s_level.scale != 0.0f ? s_level.scale : 1.0f));
        if (level < -30000) level = -30000;
        if (level > 30000) level = 30000;
        p_set_water_level(s_mario.id, level);
    }
    return (uint8_t*)ip + 4;
}
// SM64_MarioKill(): the level killed him - the script says so, because the game is what knows about its own
// death barriers.  He loses the life on the next tick, like any other way of running out of health.
void* __cdecl N_MarioKill(void* ip) {
    if (s_mario.id >= 0 && p_set_mario_health) p_set_mario_health(s_mario.id, 0);
    return (uint8_t*)ip + 4;
}
// SM64_ToggleGet() -> int: 1 on the frame M was pressed (the script swaps the rabbids and Mario over)
void* __cdecl N_ToggleGet(void* ip) {
    s_levelSeen = NowSeconds();                  // the script runs only in a level: this is what says one is up
    PushI(s_togglePending ? 1 : 0);
    s_togglePending = false;
    return (uint8_t*)ip + 4;
}
// SM64_HudSet(int on): 1 = SM64's HUD is drawn over the picture (the game's own is hidden by the script)
void* __cdecl N_HudSet(void* ip) {
    int32_t on = (int32_t)Pop();
    if ((on != 0) != s_hudOn) s_hudMoved = NowSeconds();
    s_hudOn = on != 0;
    if (s_hudOn) { s_lives = 4; s_deadTicks = 0; }
    return (uint8_t*)ip + 4;
}
// SM64_MouseLookGet() -> vector (dx, dy, 0): the mouse's motion since the last call (the cursor is captured while
// this is asked for every frame)
void* __cdecl N_MouseLookGet(void* ip) {
    float v[3] = { s_lookDx, s_lookDy, 0 };
    if (s_menuOpen) v[0] = v[1] = 0;             // the camera stays while the menu is up
    s_lookDx = s_lookDy = 0;
    s_lookWantedUntil = NowSeconds() + 0.25;
    PushVec(v);
    return (uint8_t*)ip + 4;
}

struct NativeDef { uint32_t word; const char* name; void* (__cdecl* fn)(void*); uint8_t nargs; uint8_t argsizes[4]; uint16_t retsize; };
const NativeDef kNatives[] = {                   // argument sizes in the order the handler pops them (last first)
    { 0x7D00FFFF, "SM64_Ready", N_Ready, 0, { 0 }, 4 },
    { 0x7D01FFFF, "SM64_MarioCreate", N_MarioCreate, 1, { 12 }, 4 },
    { 0x7D02FFFF, "SM64_MarioCreateAtStart", N_MarioCreateAtStart, 0, { 0 }, 4 },
    { 0x7D03FFFF, "SM64_MarioInput", N_MarioInput, 4, { 12, 4, 4, 4 }, 0 },
    { 0x7D04FFFF, "SM64_MarioTick", N_MarioTick, 1, { 4 }, 4 },
    { 0x7D05FFFF, "SM64_MarioPosGet", N_MarioPosGet, 0, { 0 }, 12 },
    { 0x7D06FFFF, "SM64_MarioPosSet", N_MarioPosSet, 1, { 12 }, 0 },
    { 0x7D07FFFF, "SM64_MarioAngleGet", N_MarioAngleGet, 0, { 0 }, 4 },
    { 0x7D08FFFF, "SM64_MarioStateGet", N_MarioStateGet, 1, { 4 }, 4 },
    { 0x7D09FFFF, "SM64_MarioActionSet", N_MarioActionSet, 1, { 4 }, 0 },
    { 0x7D0AFFFF, "SM64_MarioDamage", N_MarioDamage, 2, { 12, 4 }, 0 },
    { 0x7D0BFFFF, "SM64_MarioDelete", N_MarioDelete, 0, { 0 }, 0 },
    { 0x7D0CFFFF, "SM64_ControlsGet", N_ControlsGet, 0, { 0 }, 12 },
    { 0x7D0DFFFF, "SM64_MouseLookGet", N_MouseLookGet, 0, { 0 }, 12 },
    { 0x7D0EFFFF, "SM64_ToggleGet", N_ToggleGet, 0, { 0 }, 4 },
    { 0x7D0FFFFF, "SM64_HudSet", N_HudSet, 1, { 4 }, 0 },
    { 0x7D10FFFF, "SM64_LightSet", N_LightSet, 2, { 4, 4 }, 0 },
    { 0x7D11FFFF, "SM64_CountsSet", N_CountsSet, 2, { 4, 4 }, 0 },
    { 0x7D12FFFF, "SM64_MarioKill", N_MarioKill, 0, { 0 }, 0 },
    { 0x7D13FFFF, "SM64_WaterSet", N_WaterSet, 1, { 4 }, 0 },
    { 0x7D14FFFF, "SM64_Sound", N_Sound, 1, { 4 }, 0 },
    { 0x7D15FFFF, "SM64_ModeSet", N_ModeSet, 1, { 4 }, 0 },
    { 0x7D16FFFF, "SM64_ModeGet", N_ModeGet, 0, { 0 }, 4 },
    { 0x7D17FFFF, "SM64_Channel", N_Channel, 2, { 4, 4 }, 0 },
    { 0x7D18FFFF, "SM64_MarioPosSmooth", N_MarioPosSmooth, 0, { 0 }, 12 },
    { 0x7D19FFFF, "SM64_MarioLaunch", N_MarioLaunch, 2, { 4, 12 }, 0 },
    { 0x7D1AFFFF, "SM64_MarioFlip", N_MarioFlip, 0, { 0 }, 0 },
    { 0x7D1BFFFF, "SM64_MarioFaceGet", N_MarioFaceGet, 0, { 0 }, 12 },
    { 0x7D1CFFFF, "SM64_MarioWind", N_MarioWind, 1, { 12 }, 0 },
    { 0x7D1DFFFF, "SM64_ThreatGet", N_ThreatGet, 1, { 4 }, 12 },
    { 0x7D1EFFFF, "SM64_ThreatCount", N_ThreatCount, 0, { 0 }, 4 },
    { 0x7D1FFFFF, "SM64_PreyReport", N_PreyReport, 2, { 4, 12 }, 4 },
};

// The engine's native table: TOOsarray {data, element size 12, capacity, count} at 0x00A718AC, elements {word,
// handler, word}, sorted by word; TOOsarray::p_Add(&table, word, entry) at 006EA1C0 keeps the order and grows it.
const uint32_t NATIVES = 0x00A718AC;
const uint32_t TOO_ADD = 0x006EA1C0;
typedef void* (__cdecl* TooAddFn)(void* table, uint32_t word, void* entry);   // as RegisterFunctions calls it
int s_registered;

bool TableReady() {
    uint32_t* t = (uint32_t*)(uintptr_t)NATIVES;
    return t[0] && (t[1] == 12 || t[1] == 16) && t[3] > 100 && t[3] < 10000;
}

// The table's TOOsarray reads {data, element size (16 on the PC: word, handler, word, spare), growth, count}; the
// elements are sorted by word.  The array is copied into one with room for the additions (its own allocation:
// the engine's capacity is not readable) and the words inserted in order.
bool RegisterNatives() {
    uint32_t* t = (uint32_t*)(uintptr_t)NATIVES;
    if (!TableReady()) return false;
    const uint32_t stride = t[1] / 4;
    const uint32_t add = sizeof(kNatives) / sizeof(kNatives[0]);
    uint32_t n = t[3];
    uint32_t* old = (uint32_t*)(uintptr_t)t[0];
    uint32_t* e = (uint32_t*)calloc(n + add + 16, t[1]);
    if (!e) return false;
    memcpy(e, old, n * t[1]);
    for (size_t i = 0; i < add; ++i) {
        uint32_t word = kNatives[i].word;
        uint32_t pos = 0;
        while (pos < n && e[pos * stride] < word) ++pos;
        if (pos < n && e[pos * stride] == word) { e[pos * stride + 1] = (uint32_t)(uintptr_t)kNatives[i].fn; continue; }
        memmove(&e[(pos + 1) * stride], &e[pos * stride], (n - pos) * t[1]);
        memset(&e[pos * stride], 0, t[1]);
        e[pos * stride] = word;
        e[pos * stride + 1] = (uint32_t)(uintptr_t)kNatives[i].fn;
        e[pos * stride + 2] = word;
        ++n;
        ++s_registered;
    }
    t[0] = (uint32_t)(uintptr_t)e;               // the engine reads the table through this cell at every call
    t[3] = n;
    for (size_t i = 0; i < sizeof(kNatives) / sizeof(kNatives[0]); ++i) {     // the Lua bridge sees them too
        wmnatives::Native n;
        memset(&n, 0, sizeof(n));
        n.word = kNatives[i].word;
        n.name = kNatives[i].name;
        n.nargs = kNatives[i].nargs;
        memcpy(n.argsizes, kNatives[i].argsizes, 4);
        n.retsize = kNatives[i].retsize;
        LuaAddNative(n);
    }
    return true;
}

// The table is built after this DLL is loaded: ViD::b_Create fills it (2355 registrations), sorts it (TOOsarray
// 006EA390 with 0 at 004E93A2) and then calls 004FCC30 (a cdecl void()) at 004E93AC.  That call is redirected here:
// the original runs, then the SM64_* words are inserted, before any script is loaded and resolved.
const uint32_t CALL_AFTER_TABLE = 0x004E93AC;
const uint32_t AFTER_TABLE_FN = 0x004FCC30;
typedef void (__cdecl* VoidFn)();
bool s_hooked;

void __cdecl AfterTableHook() {
    ((VoidFn)(uintptr_t)AFTER_TABLE_FN)();
    uint32_t* t = (uint32_t*)(uintptr_t)NATIVES;
    if (RegisterNatives()) Log("SM64: %d SM64_* script natives registered after the engine built its table (%u words)", s_registered, t[3]);
    else Log("SM64: after the table's build it reads {%08X, %u, %u, %u}: the natives are registered at the first frame", t[0], t[1], t[2], t[3]);
}

bool HookAfterTable() {
    ProcessImage img;
    uint8_t site[5];
    if (!img.Read(CALL_AFTER_TABLE, site, 5) || site[0] != 0xE8) return false;
    int32_t rel;
    memcpy(&rel, site + 1, 4);
    if ((uint32_t)(CALL_AFTER_TABLE + 5 + rel) != AFTER_TABLE_FN) return false;
    int32_t nrel = (int32_t)((uint32_t)(uintptr_t)&AfterTableHook - (CALL_AFTER_TABLE + 5));
    return WriteCode(CALL_AFTER_TABLE + 1, &nrel, 4);
}

}  // namespace

// ------------------------------------------------------------------------------------------------ module entry points
void Sm64Attach(const std::string& /*iniPath*/) {
    s_inAttach = true;
    s_mario.id = -1;
    // The mod keeps its own settings: mods\It's a me!\config.ini, beside the mod itself, and the mod being enabled
    // in the launcher is what turns Mario on.  wiimote.ini is the platform's file and has nothing about him in it.
    // The ROM can simply be dropped in the mod's folder; config.ini's rom= names one kept somewhere else.
    std::string v;
    s_enabled = ModsEnabledIs(MOD_NAME);
    if (!s_enabled) {
        Log("SM64: off (the \"%s\" mod is not enabled)", MOD_NAME);
        s_inAttach = false;
        return;
    }
    s_log = ModsSettingInt(MOD_NAME, "", "log", 0) != 0;
    s_debugFlags = (uint32_t)ModsSettingInt(MOD_NAME, "", "debug", 0);
    if (ModsSetting(MOD_NAME, "", "rom", v) && !v.empty()) s_romPath = v;
    else s_romPath = ModsFileByExt(MOD_NAME, "z64");
    s_musicSeq = ModsSettingInt(MOD_NAME, "", "music", s_musicSeq);
    s_musicVolume = ModsSettingInt(MOD_NAME, "", "music_volume", s_musicVolume);
    s_waterChannels = (uint16_t)ModsSettingInt(MOD_NAME, "", "water_channels", s_waterChannels);
    s_muteChannels = (uint16_t)ModsSettingInt(MOD_NAME, "", "music_mute", s_muteChannels);
    if (ModsSetting(MOD_NAME, "", "silence_groups", v)) {
        s_quietGroupN = 0;
        const char* p = v.c_str();
        while (*p && s_quietGroupN < 8) {
            while (*p == ' ' || *p == ',') ++p;
            if (!*p) break;
            s_quietGroups[s_quietGroupN++] = (int)strtol(p, NULL, 0);
            while (*p && *p != ',') ++p;
        }
    }
    if (ModsSetting(MOD_NAME, "", "audio_dump", v)) s_audioDumpPath = v;
    // next to the executable, else the copy the mod carries in its own folder (a downloaded mod brings one)
    std::string lib = g_dllDir + "sm64.dll";
    s_lib = LoadLibraryA(lib.c_str());
    if (!s_lib) {
        std::string own = ModsFolder(MOD_NAME) + "\\sm64.dll";
        s_lib = LoadLibraryA(own.c_str());
        if (s_lib) lib = own;
    }
    if (!s_lib) {
        SmLog("sm64.dll is neither next to the executable nor in %s: Mario is off", ModsFolder(MOD_NAME).c_str());
        s_enabled = false; s_inAttach = false; return;
    }
#define GET(sym, type, var) var = (type)GetProcAddress(s_lib, sym); if (!var) { SmLog("sm64.dll has no %s: Mario is off", sym); s_enabled = false; s_inAttach = false; return; }
    GET("sm64_global_init", sm64_global_init_t, p_global_init)
    GET("sm64_global_terminate", sm64_global_terminate_t, p_global_terminate)
    GET("sm64_static_surfaces_load", sm64_static_surfaces_load_t, p_surfaces_load)
    GET("sm64_mario_create", sm64_mario_create_t, p_mario_create)
    GET("sm64_mario_tick", sm64_mario_tick_t, p_mario_tick)
    GET("sm64_mario_delete", sm64_mario_delete_t, p_mario_delete)
    GET("sm64_set_mario_position", sm64_set_mario_position_t, p_set_position)
    GET("sm64_set_mario_faceangle", sm64_set_mario_faceangle_t, p_set_faceangle)
    GET("sm64_set_mario_velocity", sm64_set_mario_velocity_t, p_set_velocity)
    GET("sm64_set_mario_action", sm64_set_mario_action_t, p_set_action)
    GET("sm64_mario_take_damage", sm64_mario_take_damage_t, p_take_damage)
    GET("sm64_set_mario_water_level", sm64_set_mario_water_level_t, p_set_water_level)
    GET("sm64_register_debug_print_function", sm64_register_debug_print_function_t, p_register_debug_print)
#undef GET
#define OPT(sym, type, var) var = (type)GetProcAddress(s_lib, sym);
    OPT("sm64_audio_init", sm64_audio_init_t, p_audio_init)
    OPT("sm64_audio_tick", sm64_audio_tick_t, p_audio_tick)
    OPT("sm64_play_music", sm64_play_music_t, p_play_music)
    OPT("sm64_stop_background_music", sm64_stop_background_music_t, p_stop_music)
    OPT("sm64_set_sound_volume", sm64_set_sound_volume_t, p_set_sound_volume)
    OPT("sm64_play_sound_global", sm64_play_sound_global_t, p_play_sound_global)
    OPT("sm64_seq_channel_fade", sm64_seq_channel_fade_t, p_channel_fade)
    OPT("sm64_seq_channel_mute", sm64_seq_channel_mute_t, p_channel_mute)
    OPT("sm64_mario_interact_cap", sm64_mario_interact_cap_t, p_interact_cap)
    OPT("sm64_set_mario_state", sm64_set_mario_state_t, p_set_mario_state)
    OPT("sm64_set_mario_health", sm64_set_mario_health_t, p_set_mario_health)
    OPT("sm64_set_mario_forward_velocity", sm64_set_mario_forward_velocity_t, p_set_forward_velocity)
    OPT("sm64_set_mario_anim_frame", sm64_set_mario_anim_frame_t, p_set_anim_frame)
    OPT("sm64_surface_object_create", sm64_surface_object_create_t, p_surface_object_create)
    OPT("sm64_surface_object_delete", sm64_surface_object_delete_t, p_surface_object_delete)
    OPT("sm64_set_camera", sm64_set_camera_t, p_set_camera)
    OPT("sm64_set_object_geometry", sm64_set_object_geometry_t, p_set_object_geometry)
    OPT("sm64_menu_count", sm64_menu_count_t, p_menu_count)
    OPT("sm64_menu_name", sm64_menu_name_t, p_menu_name)
    OPT("sm64_menu_category", sm64_menu_category_t, p_menu_category)
    OPT("sm64_object_spawn", sm64_object_spawn_t, p_object_spawn)
    OPT("sm64_objects_clear", sm64_objects_clear_t, p_objects_clear)
    OPT("sm64_objects_count", sm64_objects_count_t, p_objects_count)
    OPT("sm64_objects_shift", sm64_objects_shift_t, p_objects_shift)
    OPT("sm64_texture_count", sm64_texture_count_t, p_texture_count)
    OPT("sm64_texture_size", sm64_texture_size_t, p_texture_size)
    OPT("sm64_texture_rgba", sm64_texture_rgba_t, p_texture_rgba)
    OPT("sm64_set_debug", sm64_set_debug_t, p_set_debug)
    OPT("sm64_dialog_state", sm64_dialog_state_t, p_dialog_state)
    OPT("sm64_dialog_close", sm64_dialog_close_t, p_dialog_close)
    OPT("sm64_hud_counts", sm64_hud_counts_t, p_hud_counts)
    OPT("sm64_threats", sm64_threats_t, p_threats)
    OPT("sm64_prey_set", sm64_prey_set_t, p_prey_set)
    OPT("sm64_prey_hits", sm64_prey_hits_t, p_prey_hits)
    SmLog("%s at %p, wiimote.dll at %p (a crash's addresses are offsets from these)", lib.c_str(), (void*)s_lib, (void*)GetModuleHandleA("wiimote.dll"));
#undef OPT
    {                                            // the engine's object routines the movable collision calls
        ProcessImage img;
        const struct { uint32_t va, crc; const char* name; } kEngine[] = {
            { LOA_KEY_SEARCH, 0x508739C4u, "LOA_pt_KeySearchRealAddress" },
            { OBJ_HORIZON_GET, 0xE987B4D8u, "OBJ_HorizonGet_C" },
            { OBJ_SIGHT_GET, 0xD294B40Fu, "OBJ_SightGet_C" },
            { OBJ_BANKING_GET, 0x9FA1B576u, "OBJ_BankingGet_C" },
            { OBJ_POS_GET, 0xC9BB25F3u, "OBJ_PosGet_C" },
        };
        s_engineObjOk = true;
        for (size_t i = 0; i < sizeof(kEngine) / sizeof(kEngine[0]); ++i) {
            uint32_t got = 0;
            if (!CheckCrc(img, kEngine[i].va, 32, kEngine[i].crc, &got)) {
                s_engineObjOk = false;
                SmLog("%s is not at %08X in this executable (%08X): the level's movable things are not collision",
                      kEngine[i].name, kEngine[i].va, got);
            }
        }
    }
    std::vector<uint8_t> rom;
    if (s_romPath.empty() || !ReadFileBytes(s_romPath, rom)) {
        SmLog("your Super Mario 64 (USA) .z64 is %s: Mario is off. Drop the ROM into mods\\%s\\, or name it with "
              "rom= in that folder's config.ini%s%s", s_romPath.empty() ? "not set" : "not readable", MOD_NAME,
              s_romPath.empty() ? "" : " (tried ", s_romPath.empty() ? "" : s_romPath.c_str());
        s_enabled = false;
        s_inAttach = false;
        return;
    }
    if (rom.size() != 8 * 1024 * 1024 || rom[0] != 0x80 || rom[1] != 0x37) {
        SmLog("%s is not an 8 MB big-endian .z64 ROM: Mario is off", s_romPath.c_str());
        s_enabled = false;
        s_inAttach = false;
        return;
    }
    p_register_debug_print(DebugPrint);
    s_texture = (uint8_t*)malloc(SM64_TEXTURE_WIDTH * SM64_TEXTURE_HEIGHT * 4);
    if (p_set_debug && s_debugFlags) { p_set_debug(s_debugFlags); SmLog("debug=%u: the library's diagnostics are on", s_debugFlags); }
    p_global_init(&rom[0], s_texture);
    s_libReady = true;
    if (s_log) {                                 // the atlas as the library built it, to look at its alpha
        FILE* f = fopen((g_dllDir + "sm64_atlas.raw").c_str(), "wb");
        if (f) {
            fwrite(s_texture, 1, SM64_TEXTURE_WIDTH * SM64_TEXTURE_HEIGHT * 4, f);
            fclose(f);
            SmLog("atlas written to sm64_atlas.raw (%dx%d RGBA)", SM64_TEXTURE_WIDTH, SM64_TEXTURE_HEIGHT);
        }
    }
    if (p_audio_init && p_audio_tick) {
        p_audio_init(&rom[0]);
        s_audioReady = true;
        SmLog("SM64's audio engine is up: its music loops the way its sequences say, and Mario's effects come with it");
    }
    SmLog("libsm64 up with %s (%u KB)", s_romPath.c_str(), (unsigned)(rom.size() / 1024));
    MenuBuild();
    if (ObjectsAvailable())
        SmLog("the object engine is in this sm64.dll: %d things to spawn in %u categories, %d pictures out of the ROM (T opens the menu while he has the level)",
              p_menu_count(), (unsigned)s_menuCats.size(), p_texture_count());
    else
        SmLog("this sm64.dll has no object engine: the T menu is off");
    uint32_t* t = (uint32_t*)(uintptr_t)NATIVES;
    if (t[0] && t[1] == 12 && t[3] > 100) {          // the table exists already (a late load): now
        RegisterNatives();
        SmLog("%d SM64_* script natives registered", s_registered);
    } else {
        s_hooked = HookAfterTable();
        SmLog(s_hooked ? "SM64_* script natives are registered once the engine has built its native table"
                       : "the call after the native table's build is not where expected: SM64_* natives NOT registered");
    }
    AfxSetSceneDraw(SceneDraw, SceneLost);
    HudDecode(rom);
    GfxOverlay hud;
    hud.draw = HudDraw;
    hud.lost = HudLost;
    GfxSetHud(hud);
    s_inAttach = false;
}

void Sm64AfterConfig() {
    for (size_t i = 0; i < s_pending.size(); ++i) Log("SM64: %s", s_pending[i].c_str());
    s_pending.clear();
}

bool Sm64Enabled() { return s_enabled && s_libReady; }

void Sm64Frame() {                              // once per frame on the main thread (MagmaUpdateHook)
    if (!s_enabled || !s_libReady) return;
    MouseLookFrame();
    ToggleFrame();
    AudioFrame();
    DynFrame();                                  // the level's movable things, where the engine has them now
    // SM64's music is Mario's: it plays while he has the level and stops when he hands it back, and the game's own
    // music is only down for that time.  [sm64] music=0 leaves the game's music alone throughout.
    bool wantMusic = s_musicSeq > 0 && s_audioReady && s_hudOn && (NowSeconds() - s_levelSeen) < 0.5;
    if (wantMusic && !s_musicOn) MusicStart();
    else if (!wantMusic && s_musicOn) MusicStop();
    WaterMusicDynamic((s_mario.state.action & 0x00002000u) != 0);   // ACT_FLAG_SWIMMING
    bool quiet = s_hudOn && Driven();            // his level, so the rabbids are not heard in it
    if (quiet != s_quietApplied) {
        s_quietApplied = quiet;
        OptionsSilenceExtra(quiet ? s_quietGroups : NULL, quiet ? s_quietGroupN : 0);
    }
    if (s_registered) return;
    static int tries;
    if (RegisterNatives()) Log("SM64: %d SM64_* script natives registered at frame %d (table of %u words)", s_registered, tries, *(uint32_t*)(uintptr_t)(NATIVES + 12));
    else if (++tries == 600) Log("SM64: the native table never became usable: SM64_* natives NOT registered");
}
