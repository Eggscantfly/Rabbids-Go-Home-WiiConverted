#pragma once

// libsm64_objects_glue.h - what the object engine brought over from the decompilation (import-objects.py) expects
// of the game around it, and what libsm64 keeps per Mario instance behind macros (shim.h).  Every ported .c file
// includes this.
#include "include/PR/ultratypes.h"
#include "include/types.h"
#include "shim.h"
#include "global_state.h"
#include "engine/surface_collision.h"

#include "include/level_table.h"
#include "include/course_table.h"
#include "engine/behavior_script.h"

// the decompilation's array of players is libsm64's one MarioState
#define gMarioStates (&g_state->mgMarioStateVal)
#define gBodyStates (g_state->mgBodyStates)
// ... and its newer names for what libsm64's structures still call otherwise
#define gettingBlownGravity unkC4
#define FLOOR_LOWER_LIMIT_MISC (FLOOR_LOWER_LIMIT + 1000)
#define FLOOR_LOWER_LIMIT_SHADOW (FLOOR_LOWER_LIMIT + 1000.0)

// the ways Mario is asked to look at what talks to him (mario_actions_cutscene.h of the newer decompilation)
#define MARIO_DIALOG_STOP 0
#define MARIO_DIALOG_LOOK_FRONT 1
#define MARIO_DIALOG_LOOK_UP 2
#define MARIO_DIALOG_LOOK_DOWN 3
#define MARIO_DIALOG_STATUS_NONE 0
#define MARIO_DIALOG_STATUS_START 1
#define MARIO_DIALOG_STATUS_SPEAK 2
#define MARIO_DIALOG_STATUS_INTERRUPT 3

// the zero and one vectors the decompilation keeps in graph_node.c (libsm64_stubs.c has them)
extern Vec3f gVec3fZero;
extern Vec3s gVec3sZero;
extern Vec3f gVec3fOne;
extern Vec3s gVec3sOne;
extern u64 osClockRate;

// camera.c keeps this in its own file; the intro scene names it
extern u32 gCutsceneObjSpawn;
// the file select's yellow background and buttons, which the behaviour scripts name
void beh_yellow_background_menu_init(void);
void beh_yellow_background_menu_loop(void);
void bhv_menu_button_manager_init(void);
void bhv_menu_button_manager_loop(void);
void bhv_menu_button_init(void);
void bhv_menu_button_loop(void);
void bhv_act_selector_init(void);
void bhv_act_selector_loop(void);
void bhv_act_selector_star_type_loop(void);

// where an object believes it is: gCurrLevelNum is set from this before each object's behaviour runs, so Bowser
// fights the way he does in the arena the spawn menu named (0 = the level everything else believes it is in)
#define LIBSM64_OBJECT_LEVEL(obj) ((obj)->unused1)
#define LIBSM64_DEFAULT_LEVEL LEVEL_BOB
#define LIBSM64_MODEL_SLOTS 0x200

// the pool is never allowed to hang: when it is full and nothing unimportant is loaded, the oldest thing that is
// neither Mario nor what he holds gives up its slot (spawn_object.c)
struct Object *libsm64_find_droppable_object(void);
// gCurrLevelNum for the object about to be updated (object_list_processor.c)
void libsm64_before_object_update(struct Object *obj);
// no text boxes: a conversation is over the moment it is asked for (object_helpers.c's two are renamed away)
s32 cur_obj_update_dialog(s32 actionArg, s32 dialogFlags, s32 dialogID, s32 unused);
// the text box (libsm64_stubs.c): asked for by an object, counted down each tick, shown by the host
s32 libsm64_dialog_request(struct Object *o, s32 dialogID, s32 wait);
void libsm64_dialog_tick(void);
int32_t libsm64_dialog_frames(void);
void libsm64_dialog_close(void);
void libsm64_after_object_update(struct Object *obj);
int32_t libsm64_stars_collected(void);
int32_t libsm64_stars_collected(void);
s32 cur_obj_update_dialog_with_cutscene(s32 actionArg, s32 dialogFlags, s32 cutsceneTable, s32 dialogID);
// the camera (libsm64_stubs.c): where the host says it is, in SM64 units, for sounds, billboards and behaviours
void libsm64_camera_set(const f32 pos[3], const f32 look[3], const f32 up[3]);
void libsm64_camera_basis(Vec3f right, Vec3f up, Vec3f back);
void libsm64_camera_pos(Vec3f out);
void libsm64_camera_relative(const Vec3f world, Vec3f out);
