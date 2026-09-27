#pragma once

#include "include/types.h"
#include "game/area.h"
#include "game/level_update.h"
#include "../libsm64.h"
#include "../play_sound.h"
#include "global_state.h"

#include "include/course_table.h"

// the decompilation renamed this input bit; libsm64's Mario code keeps the old name
#define INPUT_UNKNOWN_10 INPUT_STOMPED

// what libsm64 keeps per Mario instance (global_state.h)
#define gGlobalTimer         (g_state->mgGlobalTimer)
#define gSpecialTripleJump   (g_state->mgSpecialTripleJump)
#define gCurrLevelNum        (g_state->mgCurrLevelNum)
#define gCameraMovementFlags (g_state->mgCameraMovementFlags)
//#define gAudioRandom         (g_state->mgAudioRandom)
#define gShowDebugText       (g_state->mgShowDebugText)
#define gDebugLevelSelect    (g_state->mgDebugLevelSelect)
#define gCurrSaveFileNum     (g_state->mgCurrSaveFileNum)
#define gController          (g_state->mgController)
#define gMarioSpawnInfoVal   (g_state->mgMarioSpawnInfoVal)
#define gMarioSpawnInfo      (&g_state->mgMarioSpawnInfoVal)
#define gCurrentArea         (g_state->mgCurrentArea)
#define gCurrentObject       (g_state->mgCurrentObject)
#define gMarioObject         (g_state->mgMarioObject)
#define D_80339D10           (g_state->mD_80339D10)
#define gMarioState          (&g_state->mgMarioStateVal)
#define gAreaUpdateCounter   (g_state->mgAreaUpdateCounter)

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"

static void *segmented_to_virtual(const void *addr) { return (void*)addr; }
static void *virtual_to_segmented(u32 segment, const void *addr) { (void)segment; return (void*)addr; }
static void func_80320A4C(u8 bankIndex, u8 arg1) { (void)bankIndex; (void)arg1; }

#pragma GCC diagnostic pop

// what the object engine and its stubs provide (libsm64_stubs.c and the ported game files)
struct Camera;
void set_camera_mode(struct Camera *c, s16 mode, s16 frames);
void set_camera_shake_from_hit(s16 shake);
void print_text_fmt_int(s32 x, s32 y, const char *str, s32 n);
s16 level_trigger_warp(struct MarioState *m, s32 warpOp);
u16 level_control_timer(s32 timerOp);
void load_level_init_text(u32 arg);
void play_infinite_stairs_music(void);
s32 save_file_get_total_star_count(s32 fileIndex, s32 minCourse, s32 maxCourse);
u32 save_file_get_flags(void);
void save_file_set_flags(u32 flags);
void save_file_clear_flags(u32 flags);
void spawn_wind_particles(s16 pitch, s16 yaw);
void spawn_default_star(f32 sp20, f32 sp24, f32 sp28);
void raise_background_noise(s32 a);
void lower_background_noise(s32 a);
void play_shell_music(void);
void stop_shell_music(void);
void enable_time_stop(void);
void disable_time_stop(void);
