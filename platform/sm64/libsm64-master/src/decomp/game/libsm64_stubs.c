// libsm64_stubs.c - the game around the object engine that libsm64 does not have: the camera, the text boxes, the
// save file, the level and area bookkeeping, the debug printing.  What the behaviours read is kept as plain
// variables set by the library (the camera's place, the level number); what they call either does the least that
// keeps them going (a conversation is over at once, a cutscene lasts a frame) or nothing at all.
#include <string.h>
#include <math.h>

#include "sm64.h"
#include "area.h"
#include "camera.h"
#include "level_update.h"
#include "level_table.h"
#include "course_table.h"
#include "save_file.h"
#include "ingame_menu.h"
#include "envfx_snow.h"
#include "envfx_bubbles.h"
#include "paintings.h"
#include "game_init.h"
#include "debug.h"
#include "print.h"
#include "sound_init.h"
#include "mario.h"
#include "mario_misc.h"
#include "object_list_processor.h"
#include "object_helpers.h"
#include "interaction.h"
#include "engine/graph_node.h"
#include "engine/math_util.h"
#include "libsm64_objects_glue.h"

// ------------------------------------------------------------------------------------------------ area / level
struct GraphNode *sLoadedGraphNodes[LIBSM64_MODEL_SLOTS];
struct GraphNode **gLoadedGraphNodes = sLoadedGraphNodes;
struct SpawnInfo gPlayerSpawnInfos[1];
struct GraphNode *D_8033A160[0x100];
struct Area gAreaData[8];
struct Area *gAreas = gAreaData;
struct WarpTransition gWarpTransition;
s16 gCurrCourseNum = COURSE_BOB;
s16 gCurrActNum = 1;
s16 gCurrAreaIndex = 0;
s16 gSavedCourseNum;
s16 gMenuOptSelectIndex;
s16 gSaveOptSelectIndex;
struct HudDisplay gHudDisplay = { 4, 0, 120, 8, 0, 0, 0 };   // all the stars: the things that wait for them come out
s8 gNeverEnteredCastle = 1;
struct CreditsEntry *gCurrCreditsEntry;
s16 sCurrPlayMode;
u16 D_80339ECA;
s16 sTransitionTimer;
void (*sTransitionUpdate)(s16 *);
struct WarpDest sWarpDest;
s16 D_80339EE0;
s16 sDelayedWarpOp;
s16 sDelayedWarpTimer;
s16 sSourceWarpNodeId;
s32 sDelayedWarpArg;
s8 sTimerRunning;

// ------------------------------------------------------------------------------------------------ text boxes
s8 gDialogCourseActNum;
s8 gHudFlash;
s32 gDialogResponse = DIALOG_RESPONSE_NONE;
u16 gMenuTextColorTransTimer;
s8 gLastDialogLineNum;
s32 gDialogVariable;
u16 gMenuTextAlpha;
s16 gCutsceneMsgXOffset;
s16 gCutsceneMsgYOffset;
s8 gRedCoinsCollected;
s16 gDialogID = -1;

// A text box.  The host draws one (with words of its own) while this counts down, five seconds unless the host
// closes it sooner.  An object that asks with wait set is held until its box is over, as SM64 holds it; other
// askers wait their turn; the answer to a question is yes.  One box at a time.
#define DIALOG_FRAMES 150
static s32 sDialogFrames;                        // > 0: a box is up
static struct Object *sDialogOwner;              // who is waiting on it, or NULL
static struct Object *sDialogDoneFor;            // whose box just ended, answered on its next ask
static s32 sDialogDoneAt, sDialogTicks;

static void dialog_end(void) {
    sDialogFrames = 0;
    sDialogDoneFor = sDialogOwner;
    sDialogDoneAt = sDialogTicks;
    sDialogOwner = NULL;
    gDialogID = -1;
    gDialogResponse = DIALOG_RESPONSE_YES;
}

s32 libsm64_dialog_request(struct Object *o, s32 dialogID, s32 wait) {
    if (dialogID == 13 || dialogID == 14) return 1;   /* DIALOG_013 / DIALOG_014 */   // "you got a star - save?": the one box not shown
    if (sDialogFrames > 0) {
        if (o != NULL && sDialogOwner == o) return 0;   // its own box is still up
        return wait ? 0 : 1;                       // another's is: wait for it, or never mind
    }
    if (o != NULL && sDialogDoneFor == o && sDialogTicks - sDialogDoneAt <= 2) {
        sDialogDoneFor = NULL;                     // the box it waited on is over
        return 1;
    }
    sDialogDoneFor = NULL;
    sDialogFrames = DIALOG_FRAMES;
    sDialogOwner = wait ? o : NULL;
    gDialogID = (s16) dialogID;
    gDialogResponse = DIALOG_RESPONSE_NONE;
    return wait ? 0 : 1;
}

void libsm64_dialog_tick(void) {
    sDialogTicks++;
    if (sDialogFrames <= 0) return;
    if (sDialogOwner != NULL && !(sDialogOwner->activeFlags & ACTIVE_FLAG_ACTIVE)) sDialogOwner = NULL;   // it went away
    if (--sDialogFrames == 0) dialog_end();
}

int32_t libsm64_dialog_frames(void) { return sDialogFrames; }
void libsm64_dialog_close(void) { if (sDialogFrames > 0) dialog_end(); }

s16 get_dialog_id(void) { return sDialogFrames > 0 ? gDialogID : -1; }
void create_dialog_box(s16 dialog) { libsm64_dialog_request(NULL, dialog, FALSE); }
void create_dialog_box_with_var(s16 dialog, s32 dialogVar) { (void)dialogVar; libsm64_dialog_request(NULL, dialog, FALSE); }
void create_dialog_inverted_box(s16 dialog) { libsm64_dialog_request(NULL, dialog, FALSE); }
void create_dialog_box_with_response(s16 dialog) { libsm64_dialog_request(NULL, dialog, FALSE); gDialogResponse = DIALOG_RESPONSE_YES; }
void reset_dialog_render_state(void) {}
void set_menu_mode(s16 mode) { (void)mode; }
void reset_red_coins_collected(void) { gRedCoinsCollected = 0; }
void set_cutscene_message(s16 xOffset, s16 yOffset, s16 msgIndex, s16 msgDuration) { (void)xOffset; (void)yOffset; (void)msgIndex; (void)msgDuration; }

// A conversation, asked for by an object standing before Mario: the object waits for its box, and the answer
// to a question is yes.  The object's own dialog state is left alone.
s32 cur_obj_update_dialog(s32 actionArg, s32 dialogFlags, s32 dialogID, s32 unused) {
    (void)actionArg; (void)dialogFlags; (void)unused;
    return libsm64_dialog_request(gCurrentObject, dialogID, TRUE);
}
s32 cur_obj_update_dialog_with_cutscene(s32 actionArg, s32 dialogFlags, s32 cutsceneTable, s32 dialogID) {
    (void)actionArg; (void)dialogFlags; (void)cutsceneTable;
    return libsm64_dialog_request(gCurrentObject, dialogID, TRUE);
}

// ------------------------------------------------------------------------------------------------ the camera
struct Camera sCameraStorage;
struct Camera *gCamera = &sCameraStorage;
struct LakituState gLakituState;
struct PlayerCameraState gPlayerCameraState[2];
s32 gObjCutsceneDone;
struct Object *gCutsceneFocus;
struct Object *gSecondCameraFocus;
u8 gRecentCutscene;
u32 gCutsceneObjSpawn;

static Vec3f sCamPos;
static Vec3f sCamRight = { 1.0f, 0.0f, 0.0f };
static Vec3f sCamUp = { 0.0f, 1.0f, 0.0f };
static Vec3f sCamBack = { 0.0f, 0.0f, 1.0f };

void libsm64_camera_set(const f32 pos[3], const f32 look[3], const f32 up[3]) {
    Vec3f f, r, u;
    f32 len;
    vec3f_copy(sCamPos, (f32 *)pos);
    vec3f_copy(f, (f32 *)look);
    len = sqrtf(f[0] * f[0] + f[1] * f[1] + f[2] * f[2]);
    if (len < 0.0001f) return;
    f[0] /= len; f[1] /= len; f[2] /= len;
    // right = forward x up, up = right x forward, back = -forward
    r[0] = f[1] * up[2] - f[2] * up[1];
    r[1] = f[2] * up[0] - f[0] * up[2];
    r[2] = f[0] * up[1] - f[1] * up[0];
    len = sqrtf(r[0] * r[0] + r[1] * r[1] + r[2] * r[2]);
    if (len < 0.0001f) { r[0] = 1.0f; r[1] = 0.0f; r[2] = 0.0f; len = 1.0f; }
    r[0] /= len; r[1] /= len; r[2] /= len;
    u[0] = r[1] * f[2] - r[2] * f[1];
    u[1] = r[2] * f[0] - r[0] * f[2];
    u[2] = r[0] * f[1] - r[1] * f[0];
    vec3f_copy(sCamRight, r);
    vec3f_copy(sCamUp, u);
    sCamBack[0] = -f[0]; sCamBack[1] = -f[1]; sCamBack[2] = -f[2];
    // what the behaviours and the sounds read
    vec3f_copy(gCamera->pos, sCamPos);
    gCamera->focus[0] = sCamPos[0] + f[0] * 100.0f;
    gCamera->focus[1] = sCamPos[1] + f[1] * 100.0f;
    gCamera->focus[2] = sCamPos[2] + f[2] * 100.0f;
    gCamera->yaw = atan2s(f[2], f[0]);
    gCamera->nextYaw = gCamera->yaw;
    vec3f_copy(gLakituState.curPos, sCamPos);
    vec3f_copy(gLakituState.goalPos, sCamPos);
    vec3f_copy(gLakituState.pos, sCamPos);
    vec3f_copy(gLakituState.curFocus, gCamera->focus);
    vec3f_copy(gLakituState.goalFocus, gCamera->focus);
    vec3f_copy(gLakituState.focus, gCamera->focus);
    gLakituState.yaw = gCamera->yaw;
    gLakituState.nextYaw = gCamera->yaw;
}

void libsm64_camera_basis(Vec3f right, Vec3f up, Vec3f back) {
    vec3f_copy(right, sCamRight);
    vec3f_copy(up, sCamUp);
    vec3f_copy(back, sCamBack);
}

void libsm64_camera_pos(Vec3f out) { vec3f_copy(out, sCamPos); }

// a world point as the camera sees it: x to its right, y up, z towards it (negative in front), for the sounds
void libsm64_camera_relative(const Vec3f world, Vec3f out) {
    Vec3f d = { world[0] - sCamPos[0], world[1] - sCamPos[1], world[2] - sCamPos[2] };
    out[0] = d[0] * sCamRight[0] + d[1] * sCamRight[1] + d[2] * sCamRight[2];
    out[1] = d[0] * sCamUp[0] + d[1] * sCamUp[1] + d[2] * sCamUp[2];
    out[2] = d[0] * sCamBack[0] + d[1] * sCamBack[1] + d[2] * sCamBack[2];
}

void set_camera_shake_from_hit(s16 shake) { (void)shake; }
void set_environmental_camera_shake(s16 shake) { (void)shake; }
void set_camera_shake_from_point(s16 shake, f32 posX, f32 posY, f32 posZ) { (void)shake; (void)posX; (void)posY; (void)posZ; }
void set_camera_mode(struct Camera *c, s16 mode, s16 frames) { (void)c; (void)mode; (void)frames; }
void transition_next_state(struct Camera *c, s16 frames) { (void)c; (void)frames; }
void set_fov_function(u8 func) { (void)func; }
void set_fov_shake(s16 amplitude, s16 decay, s16 shakeSpeed) { (void)amplitude; (void)decay; (void)shakeSpeed; }
void set_fov_shake_from_point_preset(u8 preset, f32 posX, f32 posY, f32 posZ) { (void)preset; (void)posX; (void)posY; (void)posZ; }
void set_hand_cam_shake(s16 shake) { (void)shake; }
void play_cutscene(struct Camera *c) { (void)c; }
void start_cutscene(struct Camera *c, u8 cutscene) { (void)c; (void)cutscene; }
void warp_camera(f32 x, f32 y, f32 z) { (void)x; (void)y; (void)z; }
void set_camera_pitch_shake(s16 mag, s16 decay, s16 inc) { (void)mag; (void)decay; (void)inc; }
void set_camera_yaw_shake(s16 mag, s16 decay, s16 inc) { (void)mag; (void)decay; (void)inc; }
void set_camera_roll_shake(s16 mag, s16 decay, s16 inc) { (void)mag; (void)decay; (void)inc; }
void set_pitch_shake_from_point(s16 mag, s16 decay, s16 inc, f32 maxDist, f32 posX, f32 posY, f32 posZ) { (void)mag; (void)decay; (void)inc; (void)maxDist; (void)posX; (void)posY; (void)posZ; }

// A cutscene an object asks for: it starts on the first call (1) and is over on the next (-1), so the object
// carries on the frame after, which is the whole of what the camera would have shown.
static struct Object *sCutsceneObj;
static u8 sCutsceneId;
static s16 cutscene_one_frame(u8 cutscene, struct Object *o) {
    if (sCutsceneObj == o && sCutsceneId == cutscene) {
        sCutsceneObj = NULL;
        sCutsceneId = 0;
        gObjCutsceneDone = TRUE;
        gCutsceneFocus = NULL;
        return -1;
    }
    sCutsceneObj = o;
    sCutsceneId = cutscene;
    gCutsceneFocus = o;
    gObjCutsceneDone = FALSE;
    return 1;
}
s16 cutscene_object_with_dialog(u8 cutscene, struct Object *o, s16 dialogID) { libsm64_dialog_request(o, dialogID, FALSE); return cutscene_one_frame(cutscene, o); }
s16 cutscene_object_without_dialog(u8 cutscene, struct Object *o) { return cutscene_one_frame(cutscene, o); }
s16 cutscene_object(u8 cutscene, struct Object *o) { return cutscene_one_frame(cutscene, o); }

// the parts of camera.c the behaviours use as arithmetic
s32 approach_f32_asymptotic_bool(f32 *current, f32 target, f32 multiplier) {
    if (multiplier > 1.0f) multiplier = 1.0f;
    *current = *current + (target - *current) * multiplier;
    return *current != target;
}
f32 approach_f32_asymptotic(f32 current, f32 target, f32 multiplier) {
    return current + (target - current) * multiplier;
}
s32 approach_s16_asymptotic_bool(s16 *current, s16 target, s16 divisor) {
    s16 temp = *current;
    if (divisor == 0) {
        *current = target;
    } else {
        temp -= target;
        temp -= temp / divisor;
        temp += target;
        *current = temp;
    }
    return *current != target;
}
s32 approach_s16_asymptotic(s16 current, s16 target, s16 divisor) {
    s16 temp = current;
    if (divisor == 0) {
        current = target;
    } else {
        temp -= target;
        temp -= temp / divisor;
        temp += target;
        current = temp;
    }
    return current;
}
s32 camera_approach_s16_symmetric_bool(s16 *current, s16 target, s16 increment) {
    s16 dist = target - *current;
    if (increment < 0) increment = -1 * increment;
    if (dist > 0) {
        dist -= increment;
        *current = (dist >= 0) ? target - dist : target;
    } else {
        dist += increment;
        *current = (dist <= 0) ? target - dist : target;
    }
    return *current != target;
}
s32 camera_approach_s16_symmetric(s16 current, s16 target, s16 increment) {
    s16 dist = target - current;
    if (increment < 0) increment = -1 * increment;
    if (dist > 0) {
        dist -= increment;
        current = (dist >= 0) ? target - dist : target;
    } else {
        dist += increment;
        current = (dist <= 0) ? target - dist : target;
    }
    return current;
}
s32 camera_approach_f32_symmetric_bool(f32 *current, f32 target, f32 increment) {
    f32 dist = target - *current;
    if (increment < 0) increment = -1 * increment;
    if (dist > 0) {
        dist -= increment;
        *current = (dist > 0) ? target - dist : target;
    } else {
        dist += increment;
        *current = (dist < 0) ? target - dist : target;
    }
    return *current != target;
}
f32 camera_approach_f32_symmetric(f32 value, f32 target, f32 increment) {
    f32 dist = target - value;
    if (increment < 0) increment = -1 * increment;
    if (dist > 0) {
        dist -= increment;
        value = (dist > 0) ? target - dist : target;
    } else {
        dist += increment;
        value = (dist < 0) ? target - dist : target;
    }
    return value;
}
s32 set_or_approach_f32_asymptotic(f32 *dst, f32 goal, f32 scale) {
    *dst = *dst + (goal - *dst) * scale;
    return *dst != goal;
}
s32 set_or_approach_s16_symmetric(s16 *current, s16 target, s16 increment) {
    return camera_approach_s16_symmetric_bool(current, target, increment);
}
void vec3f_sub(Vec3f dst, Vec3f src) { dst[0] -= src[0]; dst[1] -= src[1]; dst[2] -= src[2]; }
void object_pos_to_vec3f(Vec3f dst, struct Object *o) { dst[0] = o->oPosX; dst[1] = o->oPosY; dst[2] = o->oPosZ; }
void vec3f_to_object_pos(struct Object *o, Vec3f src) { o->oPosX = src[0]; o->oPosY = src[1]; o->oPosZ = src[2]; }
s16 calculate_pitch(Vec3f from, Vec3f to) {
    f32 dx = to[0] - from[0], dy = to[1] - from[1], dz = to[2] - from[2];
    return atan2s(sqrtf(dx * dx + dz * dz), dy);
}
s16 calculate_yaw(Vec3f from, Vec3f to) {
    f32 dx = to[0] - from[0], dz = to[2] - from[2];
    return atan2s(dz, dx);
}
f32 calc_abs_dist(Vec3f a, Vec3f b) {
    f32 dx = b[0] - a[0], dy = b[1] - a[1], dz = b[2] - a[2];
    return sqrtf(dx * dx + dy * dy + dz * dz);
}
s32 is_within_100_units_of_mario(f32 posX, f32 posY, f32 posZ) {
    Vec3f pos;
    vec3f_set(pos, posX, posY, posZ);
    return calc_abs_dist(gMarioStates[0].pos, pos) < 100.0f;
}
void random_vec3s(Vec3s dst, s16 xRange, s16 yRange, s16 zRange) {
    f32 randomFloat;
    randomFloat = random_float();
    dst[0] = randomFloat * xRange - xRange / 2;
    randomFloat = random_float();
    dst[1] = randomFloat * yRange - yRange / 2;
    randomFloat = random_float();
    dst[2] = randomFloat * zRange - zRange / 2;
}
void rotate_in_xz(Vec3f dst, Vec3f src, s16 yaw) {
    Vec3f tempVec;
    vec3f_copy(tempVec, src);
    dst[0] = tempVec[2] * sins(yaw) + tempVec[0] * coss(yaw);
    dst[1] = tempVec[1];
    dst[2] = tempVec[2] * coss(yaw) - tempVec[0] * sins(yaw);
}
void rotate_in_yz(Vec3f dst, Vec3f src, s16 pitch) {
    Vec3f tempVec;
    vec3f_copy(tempVec, src);
    dst[2] = -(tempVec[2] * coss(pitch) - tempVec[1] * sins(pitch));
    dst[1] = tempVec[2] * sins(pitch) + tempVec[1] * coss(pitch);
    dst[0] = tempVec[0];
}
void offset_rotated(Vec3f dst, Vec3f from, Vec3f to, Vec3s rotation) {
    Vec3f unusedCopy;
    Vec3f pitchRotated;
    vec3f_copy(unusedCopy, from);
    pitchRotated[2] = -(to[2] * coss(rotation[0]) - to[1] * sins(rotation[0]));
    pitchRotated[1] = to[2] * sins(rotation[0]) + to[1] * coss(rotation[0]);
    pitchRotated[0] = to[0];
    dst[0] = from[0] + pitchRotated[2] * sins(rotation[1]) + pitchRotated[0] * coss(rotation[1]);
    dst[1] = from[1] + pitchRotated[1];
    dst[2] = from[2] + pitchRotated[2] * coss(rotation[1]) - pitchRotated[0] * sins(rotation[1]);
}
void play_camera_buzz_if_cdown(void) {}
void play_camera_buzz_if_cbutton(void) {}
void play_camera_buzz_if_c_sideways(void) {}
void play_sound_cbutton_up(void) {}
void play_sound_cbutton_down(void) {}
void play_sound_cbutton_side(void) {}
void play_sound_button_change_blocked(void) {}
void play_sound_rbutton_changed(void) {}
void play_sound_if_cam_switched_to_lakitu_or_mario(void) {}

// ------------------------------------------------------------------------------------------------ the save file
// Everything is had: all the stars (so MIPS and Yoshi come out, and the caps' switches are pressed), no cap left
// anywhere, no cannon locked.
s32 save_file_get_total_star_count(s32 fileIndex, s32 minCourse, s32 maxCourse) { (void)fileIndex; (void)minCourse; (void)maxCourse; return 120; }
u32 save_file_get_flags(void) {
    return SAVE_FLAG_HAVE_WING_CAP | SAVE_FLAG_HAVE_METAL_CAP | SAVE_FLAG_HAVE_VANISH_CAP | SAVE_FLAG_HAVE_KEY_1 | SAVE_FLAG_HAVE_KEY_2
         | SAVE_FLAG_UNLOCKED_BASEMENT_DOOR | SAVE_FLAG_UNLOCKED_UPSTAIRS_DOOR | SAVE_FLAG_DDD_MOVED_BACK | SAVE_FLAG_MOAT_DRAINED
         | SAVE_FLAG_UNLOCKED_PSS_DOOR | SAVE_FLAG_UNLOCKED_WF_DOOR | SAVE_FLAG_UNLOCKED_CCM_DOOR | SAVE_FLAG_UNLOCKED_JRB_DOOR
         | SAVE_FLAG_UNLOCKED_BITDW_DOOR | SAVE_FLAG_UNLOCKED_BITFS_DOOR | SAVE_FLAG_UNLOCKED_50_STAR_DOOR;
}
void save_file_set_flags(u32 flags) { (void)flags; }
void save_file_clear_flags(u32 flags) { (void)flags; }
u32 save_file_get_star_flags(s32 fileIndex, s32 courseIndex) { (void)fileIndex; (void)courseIndex; return 0; }
void save_file_set_star_flags(s32 fileIndex, s32 courseIndex, u32 starFlags) { (void)fileIndex; (void)courseIndex; (void)starFlags; }
s32 save_file_get_course_star_count(s32 fileIndex, s32 courseIndex) { (void)fileIndex; (void)courseIndex; return 0; }
u32 save_file_get_max_coin_score(s32 courseIndex) { (void)courseIndex; return 0; }
s32 save_file_get_course_coin_score(s32 fileIndex, s32 courseIndex) { (void)fileIndex; (void)courseIndex; return 0; }
static s32 sStarsCollected;                      // what the HUD shows: every star Mario has caught this session
void save_file_collect_star_or_key(s16 coinScore, s16 starIndex) { (void)coinScore; (void)starIndex; sStarsCollected++; }
int32_t libsm64_stars_collected(void) { return sStarsCollected; }
s32 save_file_is_cannon_unlocked(void) { return TRUE; }
void save_file_set_cannon_unlocked(void) {}
void save_file_set_cap_pos(s16 x, s16 y, s16 z) { (void)x; (void)y; (void)z; }
s32 save_file_get_cap_pos(Vec3s capPos) { capPos[0] = capPos[1] = capPos[2] = 0; return FALSE; }
void save_file_do_save(s32 fileIndex) { (void)fileIndex; }
s32 save_file_exists(s32 fileIndex) { (void)fileIndex; return TRUE; }
u16 save_file_get_sound_mode(void) { return 0; }
void save_file_set_sound_mode(u16 mode) { (void)mode; }
void save_file_move_cap_to_default_location(void) {}
void disable_warp_checkpoint(void) {}
void check_if_should_set_warp_checkpoint(struct WarpNode *warpNode) { (void)warpNode; }
s32 check_warp_checkpoint(struct WarpNode *warpNode) { (void)warpNode; return FALSE; }

// ------------------------------------------------------------------------------------------------ level_update
s16 level_trigger_warp(struct MarioState *m, s32 warpOp) { (void)m; (void)warpOp; return 0; }
u16 level_control_timer(s32 timerOp) { (void)timerOp; return 0; }
void load_level_init_text(u32 arg) { (void)arg; }
void fade_into_special_warp(u32 arg, u32 color) { (void)arg; (void)color; }
void level_set_transition(s16 length, void (*updateFunction)(s16 *)) { (void)length; (void)updateFunction; }

// ------------------------------------------------------------------------------------------------ game_init
struct Controller *gPlayer1Controller;
struct Controller *gPlayer2Controller;
struct Controller *gPlayer3Controller;
struct DemoInput *gCurrDemoInput;
u8 gControllerBits;
void (*gGoddardVblankCallback)(void);
struct Controller gControllers[3];

// ------------------------------------------------------------------------------------------------ effects, paintings
s8 gEnvFxMode;
s16 gEnvFxBubbleConfig[10];
struct EnvFxParticle *gEnvFxBuffer;
Vec3i gSnowCylinderLastPos;
s16 gSnowParticleCount;
s16 gPaintingMarioFloorType;
f32 gPaintingMarioXPos, gPaintingMarioYPos, gPaintingMarioZPos;
struct Painting *gRipplingPainting;
s8 gDDDPaintingStatus;

// ------------------------------------------------------------------------------------------------ mario_misc, goddard
void gd_copy_p1_contpad(OSContPad *p1cont) { (void)p1cont; }
void *gdm_gettestdl(s32 id) { (void)id; return NULL; }
void gd_vblank(void) {}
s32 gd_sfx_to_play(void) { return 0; }

// ------------------------------------------------------------------------------------------------ sound
void play_painting_eject_sound(void) {}
void play_infinite_stairs_music(void) {}
void set_background_music(u16 a, u16 seqArgs, s16 fadeTimer) { (void)a; (void)seqArgs; (void)fadeTimer; }

// ------------------------------------------------------------------------------------------------ debug, print, profiler
s64 get_current_clock(void) { return 0; }
s64 get_clock_difference(s64 cycles) { (void)cycles; return 0; }
void set_text_array_x_y(s32 xOffset, s32 yOffset) { (void)xOffset; (void)yOffset; }
void print_debug_top_down_objectinfo(const char *str, s32 number) { (void)str; (void)number; }
void print_debug_top_down_mapinfo(const char *str, s32 number) { (void)str; (void)number; }
void print_debug_bottom_up(const char *str, s32 number) { (void)str; (void)number; }
void debug_unknown_level_select_check(void) {}
void reset_debug_objectinfo(void) {}
void stub_debug_5(void) {}
void try_print_debug_mario_object_info(void) {}
void try_do_mario_debug_object_spawn(void) {}
void try_print_debug_mario_level_info(void) {}
void print_text_fmt_int(s32 x, s32 y, const char *str, s32 n) { (void)x; (void)y; (void)str; (void)n; }
void print_text(s32 x, s32 y, const char *str) { (void)x; (void)y; (void)str; }
void print_text_centered(s32 x, s32 y, const char *str) { (void)x; (void)y; (void)str; }

// the castle's flags wave on an animation of the castle grounds, which is not here
const struct Animation *const castle_grounds_seg7_anims_flags[] = { NULL };
// the file select's yellow background and its buttons (menu/file_select.c), which the behaviour scripts name
void beh_yellow_background_menu_init(void) {}
void beh_yellow_background_menu_loop(void) {}
void bhv_menu_button_manager_init(void) {}
void bhv_menu_button_manager_loop(void) {}
void bhv_menu_button_init(void) {}
void bhv_menu_button_loop(void) {}
void bhv_act_selector_init(void) {}
void bhv_act_selector_loop(void) {}
void bhv_act_selector_star_type_loop(void) {}
// the opening and the ending (mario_actions_cutscene.c of the newer decompilation) are not here
void bhv_intro_scene_loop(void) {}
void bhv_intro_peach_loop(void) {}
void bhv_intro_lakitu_loop(void) {}
void bhv_end_birds_1_loop(void) {}
void bhv_end_birds_2_loop(void) {}
// the tick tock clock's treadmills scroll a texture of their level, which is not here
const Movtex ttc_movtex_tris_small_surface_treadmill[] = { 0 };
const Movtex ttc_movtex_tris_big_surface_treadmill[] = { 0 };
// the digits an orange number shows are pictures of segment 2, which is not here: it shows nothing
const Gfx dl_billboard_num_0[] = { gsSPEndDisplayList() };
const Gfx dl_billboard_num_1[] = { gsSPEndDisplayList() };
const Gfx dl_billboard_num_2[] = { gsSPEndDisplayList() };
const Gfx dl_billboard_num_3[] = { gsSPEndDisplayList() };
const Gfx dl_billboard_num_4[] = { gsSPEndDisplayList() };
const Gfx dl_billboard_num_5[] = { gsSPEndDisplayList() };
const Gfx dl_billboard_num_6[] = { gsSPEndDisplayList() };
const Gfx dl_billboard_num_7[] = { gsSPEndDisplayList() };
const Gfx dl_billboard_num_8[] = { gsSPEndDisplayList() };
const Gfx dl_billboard_num_9[] = { gsSPEndDisplayList() };
// the constant vectors of graph_node.c
Vec3f gVec3fZero = { 0.0f, 0.0f, 0.0f };
Vec3s gVec3sZero = { 0, 0, 0 };
Vec3f gVec3fOne = { 1.0f, 1.0f, 1.0f };
Vec3s gVec3sOne = { 1, 1, 1 };

// ------------------------------------------------------------------------------------------------ the pool
struct Object *libsm64_find_droppable_object(void) {
    static const s32 lists[] = { OBJ_LIST_UNIMPORTANT, OBJ_LIST_DEFAULT, OBJ_LIST_LEVEL, OBJ_LIST_DESTRUCTIVE,
                                 OBJ_LIST_PUSHABLE, OBJ_LIST_GENACTOR, OBJ_LIST_POLELIKE, OBJ_LIST_SURFACE, OBJ_LIST_SPAWNER };
    s32 i;
    for (i = 0; i < (s32)(sizeof(lists) / sizeof(lists[0])); i++) {
        struct ObjectNode *list = &gObjectLists[lists[i]];
        struct ObjectNode *node = list->next;
        while (node != list) {
            struct Object *obj = (struct Object *) node;
            node = node->next;
            if (obj == gMarioObject || obj == gCurrentObject) continue;
            if (gMarioStates[0].heldObj == obj || gMarioStates[0].riddenObj == obj || gMarioStates[0].usedObj == obj) continue;
            if (obj->parentObj == gCurrentObject) continue;
            return obj;
        }
    }
    return NULL;
}

// libsm64_before_object_update: objects.c (it also picks the level's model table)
