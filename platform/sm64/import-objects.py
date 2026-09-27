#!/usr/bin/env python3
"""import-objects.py - bring SM64's object engine into libsm64.

libsm64 keeps only Mario: his actions, his collision against surfaces and his model.  Everything else in the game -
the enemies, the bosses, the items, the platforms - is an "object" driven by a behaviour script, and libsm64 stubs
that whole engine out.  This script copies it back in from the SM64 decompilation (the sm64-master tree, given as the
first argument or found beside the RGHPCPort project):

  - the object engine (src/game: object_list_processor, spawn_object, object_helpers, obj_behaviors, obj_behaviors_2,
    behavior_actions with every behaviors/*.inc.c, interaction, object_collision, spawn_sound, platform_displacement,
    mario_misc; src/engine/behavior_script) and the behaviour scripts (data/behavior_data.c),
  - the actors (actors/<name>/model, geo, anims, collision) as the decompilation groups them, with each texture
    replaced by a small descriptor: the pictures themselves are read out of the player's ROM at run time, from the
    places assets.json gives (as libsm64 already does for Mario's), so nothing of the game ships with the library,
  - the level data the behaviours reach for (collision plates of the moving platforms, race paths),
  - a table of every model id the level scripts bind to an actor's geo layout (LOAD_MODEL_FROM_GEO / _DL),
  - and the list of things the spawn menu offers, curated below.

Everything is written under libsm64-master/src/decomp/ next to libsm64's own copies of the decompilation; the
decompilation itself is never compiled from where it lies.  Run it again whenever the decompilation or the list
below changes; the build (build.bat) compiles what it writes.  The hand-written glue (libsm64_stubs.c, objects.c,
surface_load.c, the gfx adapter) is not touched here.
"""
import glob
import json
import os
import re
import shutil
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
DECOMP = sys.argv[1] if len(sys.argv) > 1 else os.path.normpath(os.path.join(HERE, '..', '..', 'sm64-master'))
LIB = os.path.join(HERE, 'libsm64-master')
SRC = os.path.join(LIB, 'src')
DEC = os.path.join(SRC, 'decomp')

if not os.path.isfile(os.path.join(DECOMP, 'assets.json')):
    sys.exit('no assets.json in %s: give the sm64-master folder as the first argument' % DECOMP)
if not os.path.isfile(os.path.join(SRC, 'libsm64.c')):
    sys.exit('libsm64 is not at %s' % LIB)


def read(path):
    with open(path, 'r', encoding='utf-8', errors='replace') as f:
        return f.read()


def write(path, text):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, 'w', encoding='utf-8', newline='\n') as f:
        f.write(text)


def copy_patched(src, dst, patches=()):
    text = read(src)
    for pat, rep in patches:
        text = re.sub(pat, rep, text, flags=re.M)
    write(dst, text)


# ------------------------------------------------------------------------------------------------ textures
assets = json.load(open(os.path.join(DECOMP, 'assets.json')))
textures = []            # (symbol, key, w, h, fmt, bits, mio0, offset)
texture_index = {}       # key -> index

FORMATS = {'rgba': 0, 'ia': 1, 'i': 2, 'ci': 3}


def texture_id(key):
    """the descriptor index of an assets.json picture (actors/goomba/goomba_body.rgba16), made on first sight"""
    if key in texture_index:
        return texture_index[key]
    entry = assets.get(key + '.png')
    if entry is None:
        raise KeyError('assets.json has no %s.png' % key)
    w, h = entry[0], entry[1]
    where = entry[-1]
    if 'us' not in where:
        return -1            # not in the US ROM (the Chinese release's power meter labels)
    mio0, offset = where['us']
    m = re.match(r'(rgba|ia|i|ci)(\d+)$', key.split('.')[-1])
    if not m:
        raise ValueError('unknown texture format in ' + key)
    fmt, bits = FORMATS[m.group(1)], int(m.group(2))
    idx = len(textures)
    textures.append((key, w, h, fmt, bits, mio0, offset))
    texture_index[key] = idx
    return idx


TEX_DECL = re.compile(r'^(ALIGNED8\s+)?(static\s+)?const\s+(?:Texture|u8)\s+(\w+)\[\]\s*=\s*\{\s*\n#include "([^"]+)\.inc\.c"\s*\n\};', re.M)


def rewrite_textures(text):
    """a texture array filled from a picture becomes a one-entry descriptor array naming the picture's index"""
    def rep(m):
        idx = texture_id(m.group(4))
        static = m.group(2) or ''
        if idx < 0:
            return '%sconst SM64TexRef %s[1] = {{ -1 }};' % (static, m.group(3))
        return '%sconst SM64TexRef %s[1] = {{ %d }}; /* %s */' % (static, m.group(3), idx, m.group(4))
    text = TEX_DECL.sub(rep, text)
    # a table of pictures (the power meter's) holds descriptors now
    text = re.sub(r'const (?:Texture|u8) \*const (\w+)\[\]', r'const SM64TexRef *const \1[]', text)
    return text


# ------------------------------------------------------------------------------------------------ actors
ACTORS_SRC = os.path.join(DECOMP, 'actors')
ACTORS_DST = os.path.join(DEC, 'actors')
if os.path.isdir(ACTORS_DST):
    shutil.rmtree(ACTORS_DST)
os.makedirs(ACTORS_DST)

actor_files = 0
for folder in sorted(os.listdir(ACTORS_SRC)):
    src_dir = os.path.join(ACTORS_SRC, folder)
    if not os.path.isdir(src_dir):
        continue
    if folder == 'mario':
        continue             # libsm64 carries Mario's own model (src/decomp/mario, from import-mario-geo.py)
    for root, dirs, files in os.walk(src_dir):
        for fn in files:
            if not fn.endswith('.inc.c'):
                continue
            rel = os.path.relpath(os.path.join(root, fn), ACTORS_SRC)
            text = read(os.path.join(root, fn))
            text = rewrite_textures(text)
            write(os.path.join(ACTORS_DST, rel), text)
            actor_files += 1

# the group translation units, as the decompilation has them (one per group keeps their file-local names apart),
# without Mario, whose model and geo libsm64 provides itself
GROUP_HEADER = '#include "config.h"\n#include "actor_textures.h"\n'
for fn in sorted(os.listdir(ACTORS_SRC)):
    if not (fn.endswith('.c') or fn.endswith('.h')):
        continue
    text = read(os.path.join(ACTORS_SRC, fn))
    if fn.endswith('.c'):
        text = re.sub(r'^#include "mario/[^"]*"\s*$', '// (Mario: libsm64 has his own)', text, flags=re.M)
        text = text.replace('#include <ultra64.h>', '#include "ultra64.h"')
        text = text.replace('#include <PR/ultratypes.h>', '#include "PR/ultratypes.h"')
        text = text.replace('#include <PR/gbi.h>', '#include "PR/gbi.h"')
        # the display-list macros must be the adapter's: gfx_macros.h replaces gbi.h's
        text = GROUP_HEADER + '#include "../../gfx_macros.h"\n' + text
    else:
        text = re.sub(r'extern const (?:Texture|u8) (\w+)\[\];', r'extern const SM64TexRef \1[];', text)
        text = re.sub(r'extern const (?:Texture|u8) \*const (\w+)\[\];', r'extern const SM64TexRef *const \1[];', text)
        text = GROUP_HEADER + text
    write(os.path.join(ACTORS_DST, fn), text)

# ------------------------------------------------------------------------------------------------ the object engine
GAME_SRC = os.path.join(DECOMP, 'src', 'game')
GAME_DST = os.path.join(DEC, 'game')

# externs of things libsm64 keeps per Mario instance behind macros (shim.h): a declaration would expand to nonsense
SHIM_EXTERNS = [
    r'^extern struct Object \*gMarioObject;\s*$',
    r'^extern struct Object \*gCurrentObject;\s*$',
    r'^extern struct MarioState gMarioStates\[\];\s*$',
    r'^extern struct MarioState \*gMarioState;\s*$',
    r'^extern struct Area \*gCurrentArea;\s*$',
    r'^extern s16 gCurrLevelNum;\s*$',
    r'^extern s16 gCurrSaveFileNum;\s*$',
    r'^extern u32 gGlobalTimer;\s*$',
    r'^extern u16 gAreaUpdateCounter;\s*$',
    r'^extern s16 gCameraMovementFlags;\s*$',
    r'^extern u8 gSpecialTripleJump;\s*$',
    r'^extern struct MarioBodyState gBodyStates\[2\];\s*$',
    r'^extern struct SpawnInfo \*gMarioSpawnInfo;\s*$',
    r'^extern struct Controller gControllers\[3\];\s*$',
]
SHIM_PATCHES = [(p, '/* libsm64: per-instance, see shim.h */') for p in SHIM_EXTERNS]

GAME_FILES = {
    # name: extra patches
    'object_list_processor.c': [
        # the level an object believes it is in is set before its behaviour runs
        (r'^(\s*)cur_obj_update\(\);\s*$', r'\1libsm64_before_object_update(gCurrentObject);\n\1cur_obj_update();\n\1libsm64_after_object_update(gCurrentObject);'),
        (r'^struct Object \*gMarioObject;\s*$', '/* gMarioObject: libsm64 per-instance (shim.h) */'),
        (r'^struct Object \*gCurrentObject;\s*$', '/* gCurrentObject: libsm64 per-instance (shim.h) */'),
        (r'^struct Object \*gLuigiObject;\s*$', 'struct Object *gLuigiObject;'),
        (r'#include "profiler.h"', '#include "libsm64_objects_glue.h"'),
        (r'#include "debug.h"', '#include "debug.h"\n#include "engine/surface_load.h"'),
    ],
    'object_list_processor.h': [],
    'spawn_object.c': [
        # the pool is never allowed to hang the game: an old object is dropped instead
        (r'if \(unimportantObj == NULL\) \{\s*// We\'ve met with a terrible fate\.\s*while \(TRUE\) \{\s*\}\s*\}',
         'if (unimportantObj == NULL) {\n            unimportantObj = libsm64_find_droppable_object();\n            if (unimportantObj == NULL) {\n                return NULL;\n            }\n        }'),
        (r'#include "spawn_object.h"', '#include "spawn_object.h"\n#include "libsm64_objects_glue.h"'),
    ],
    'spawn_object.h': [],
    'object_helpers.c': [
        # the dialog machinery is the glue's (no text boxes here), and the two matrix helpers are libsm64's math_util's
        (r'^s32 cur_obj_update_dialog\(', 's32 cur_obj_update_dialog_unused('),
        (r'^s32 cur_obj_update_dialog_with_cutscene\(', 's32 cur_obj_update_dialog_with_cutscene_unused('),
        (r'^void linear_mtxf_mul_vec3f\(', 'static void unused_linear_mtxf_mul_vec3f('),
        (r'^void linear_mtxf_transpose_mul_vec3f\(', 'static void unused_linear_mtxf_transpose_mul_vec3f('),
        (r'#include "spawn_sound.h"', '#include "spawn_sound.h"\n#include "libsm64_objects_glue.h"'),
        # what an object spawns believes it is in the level its parent is in
        (r'(struct Object \*spawn_object_at_origin\([^)]*\)\s*\{[^}]*?obj->parentObj = parent;)',
         r'\1\n    obj->unused1 = parent->unused1; /* libsm64: the level the parent believes it is in */'),
    ],
    'object_helpers.h': [],
    'obj_behaviors.c': [(r'#include "rumble_init.h"', '#include "rumble_init.h"\n#include "libsm64_objects_glue.h"')],
    'obj_behaviors.h': [],
    'obj_behaviors_2.c': [(r'#include "spawn_sound.h"', '#include "spawn_sound.h"\n#include "libsm64_objects_glue.h"')],
    'obj_behaviors_2.h': [],
    'behavior_actions.c': [(r'#include "rumble_init.h"', '#include "rumble_init.h"\n#include "libsm64_objects_glue.h"')],
    'behavior_actions.h': [],
    'interaction.c': [(r'#include "rumble_init.h"', '#include "rumble_init.h"\n#include "libsm64_objects_glue.h"'),
                      # every star is the 100-coin star's kind: there is no level to leave, and no ending
                      (r'u32 noExit = \(o->oInteractionSubtype & INT_SUBTYPE_NO_EXIT\) != 0;', 'u32 noExit = TRUE;  /* libsm64: no level to leave */'),
                      (r'u32 grandStar = \(o->oInteractionSubtype & INT_SUBTYPE_GRAND_STAR\) != 0;', 'u32 grandStar = FALSE;  /* libsm64: no ending here */')],
    'interaction.h': [
        # libsm64's Mario code still uses the older names of three status bits
        (r'^#define INT_STATUS_MARIO_SHOCKWAVE(.*)$',
         r'#define INT_STATUS_MARIO_SHOCKWAVE\1\n#define INT_STATUS_HOOT_GRABBED_BY_MARIO INT_STATUS_MARIO_STUNNED\n#define INT_STATUS_MARIO_UNK1 INT_STATUS_MARIO_KNOCKBACK_DMG\n#define INT_STATUS_HIT_BY_SHOCKWAVE INT_STATUS_MARIO_SHOCKWAVE'),
    ],
    'object_collision.c': [(r'#include "spawn_object.h"', '#include "spawn_object.h"\n#include "libsm64_objects_glue.h"')],
    'object_collision.h': [],
    'spawn_sound.c': [(r'#include "spawn_sound.h"', '#include "spawn_sound.h"\n#include "audio/external.h"\n#include "libsm64_objects_glue.h"')],
    'spawn_sound.h': [],
    'platform_displacement.c': [(r'#include "types.h"', '#include "types.h"\n#include "libsm64_objects_glue.h"')],
    'platform_displacement.h': [],
    'mario_misc.c': [
        (r'#include "goddard/renderer.h"', '#include "libsm64_objects_glue.h"'),
        (r'^struct MarioBodyState gBodyStates\[2\];.*$', '/* gBodyStates: libsm64 per-instance (shim.h) */'),
        # no Goddard: Mario's head on the file select is not drawn
        (r'(?s)Gfx \*geo_draw_mario_head_goddard\(s32 callContext, struct GraphNode \*node, Mat4 \*c\) \{.*?\n\}\n',
         'Gfx *geo_draw_mario_head_goddard(s32 callContext, struct GraphNode *node, Mat4 *c) {\n    (void) callContext; (void) node; (void) c;\n    return NULL;\n}\n'),
    ],
    'mario_misc.h': [],
    'camera.h': [],
    'level_update.h': [],
    'area.h': [(r'^    /\*0x00\*/ Vec3s startPos;', '    /*0x00*/ Vec3f startPos; // libsm64: float')],
    'save_file.h': [],
    'sound_init.h': [],
    'game_init.h': [],
    'ingame_menu.h': [],
    'envfx_snow.h': [],
    'envfx_bubbles.h': [],
    'paintings.h': [],
    'debug.h': [],
    'print.h': [],
    'main.h': [],
    'rumble_init.h': [],
    'shadow.h': [],
    'segment2.h': [],
    'moving_texture.h': [],
    'skybox.h': [],
    'profiler.h': [],
    'level_geo.h': [],
    'hud.h': [],
    'macro_special_objects.h': [],
    'screen_transition.h': [],
    'geo_misc.h': [],
}
for name, patches in GAME_FILES.items():
    src = os.path.join(GAME_SRC, name)
    if not os.path.isfile(src):
        print('  (no %s in the decompilation)' % name)
        continue
    copy_patched(src, os.path.join(GAME_DST, name), SHIM_PATCHES + patches)

# every behaviour
BEH_PATCHES = {
    # a boss's star flies to its level's spot, which is not here: it comes up where the boss stood instead
    'spawn_star.inc.c': [(r'void spawn_default_star\(f32 homeX, f32 homeY, f32 homeZ\) \{',
                          'void spawn_default_star(f32 homeX, f32 homeY, f32 homeZ) {\n    homeX = o->oPosX; homeY = o->oPosY + 300.0f; homeZ = o->oPosZ; /* libsm64 */')],
}
BEH_DST = os.path.join(GAME_DST, 'behaviors')
if os.path.isdir(BEH_DST):
    shutil.rmtree(BEH_DST)
for fn in sorted(os.listdir(os.path.join(GAME_SRC, 'behaviors'))):
    copy_patched(os.path.join(GAME_SRC, 'behaviors', fn), os.path.join(BEH_DST, fn), BEH_PATCHES.get(fn, []))

ENGINE_SRC = os.path.join(DECOMP, 'src', 'engine')
ENGINE_DST = os.path.join(DEC, 'engine')
copy_patched(os.path.join(ENGINE_SRC, 'behavior_script.c'), os.path.join(ENGINE_DST, 'behavior_script.c'),
             SHIM_PATCHES + [(r'#include "surface_collision.h"', '#include "surface_collision.h"\n#include "libsm64_objects_glue.h"')])
copy_patched(os.path.join(ENGINE_SRC, 'behavior_script.h'), os.path.join(ENGINE_DST, 'behavior_script.h'))
# surface_load.h: the declarations the behaviours use; the implementation is libsm64's own (surface_load.c here)
copy_patched(os.path.join(ENGINE_SRC, 'surface_load.h'), os.path.join(ENGINE_DST, 'surface_load.h'), [
    (r'^extern SpatialPartitionCell.*$', '/* libsm64: no spatial partition */'),
    (r'^extern struct SurfaceNode \*sSurfaceNodePool;.*$', ''),
    (r'^extern struct Surface \*sSurfacePool;.*$', ''),
    (r'^extern s16 sSurfacePoolSize;.*$', ''),
])

copy_patched(os.path.join(DECOMP, 'data', 'behavior_data.c'), os.path.join(DEC, 'data', 'behavior_data.c'), [
    (r'#include "menu/file_select.h"', '#include "libsm64_objects_glue.h"'),
])

INCLUDE_SRC = os.path.join(DECOMP, 'include')
INCLUDE_DST = os.path.join(DEC, 'include')
INCLUDE_FILES = ['behavior_data.h', 'model_ids.h', 'object_constants.h', 'object_fields.h', 'sounds.h', 'config.h',
                 'course_table.h', 'level_table.h', 'dialog_ids.h', 'macro_presets.h', 'special_presets.h',
                 'helper_macros.h', 'sm64.h', 'surface_terrains.h', 'geo_commands.h', 'level_misc_macros.h',
                 'make_const_nonconst.h', 'seq_ids.h', 'mario_animation_ids.h', 'mario_geo_switch_case_ids.h',
                 'level_commands.h', 'segment_symbols.h', 'segments.h', 'textures.h', 'moving_texture_macros.h']
for name in INCLUDE_FILES:
    src = os.path.join(INCLUDE_SRC, name)
    if os.path.isfile(src):
        copy_patched(src, os.path.join(INCLUDE_DST, name))

# sm64.h names the newer headers; the audio engine keeps its older names for the sound words on top of them
write(os.path.join(INCLUDE_DST, 'audio_defines.h'), '''#ifndef AUDIO_DEFINES_H
#define AUDIO_DEFINES_H
// libsm64: the sound words are sounds.h's (the decompilation renamed this file); these are the few older names the
// audio engine here still uses.
#include "sounds.h"
#define SOUND_STATUS_STARTING SOUND_STATUS_WAITING
#define SOUND_LO_BITFLAG_UNK1 SOUND_LOWER_BACKGROUND_MUSIC
#define SOUND_LO_BITFLAG_UNK8 SOUND_DISCRETE
#define SOUND_NO_FREQUENCY_LOSS SOUND_CONSTANT_FREQUENCY
#define SOUND_OBJ_WHOMP_LOWPRIO SOUND_ARG_LOAD(SOUND_BANK_OBJ, 0x16, 0x60, SOUND_DISCRETE)
#define SOUND_MENU_PAUSE_HIGHPRIO SOUND_ARG_LOAD(SOUND_BANK_MENU, 0x02, 0xFF, SOUND_DISCRETE)
#define NO_SOUND 0
#endif
''')

# the level headers the behaviours and the behaviour scripts name, and the level data they use
LEVELS_SRC = os.path.join(DECOMP, 'levels')
LEVELS_DST = os.path.join(DEC, 'levels')
if os.path.isdir(LEVELS_DST):
    shutil.rmtree(LEVELS_DST)
for lvl in sorted(os.listdir(LEVELS_SRC)):
    hdr = os.path.join(LEVELS_SRC, lvl, 'header.h')
    if os.path.isfile(hdr):
        copy_patched(hdr, os.path.join(LEVELS_DST, lvl, 'header.h'))
for name in ('level_defines.h', 'course_defines.h'):
    if os.path.isfile(os.path.join(LEVELS_SRC, name)):
        copy_patched(os.path.join(LEVELS_SRC, name), os.path.join(LEVELS_DST, name))

# what data symbols the ported code names, and which files of the levels define them
ported_text = ''
for root, dirs, files in os.walk(GAME_DST):
    for fn in files:
        if fn.endswith('.c'):
            ported_text += read(os.path.join(root, fn))
ported_text += read(os.path.join(DEC, 'data', 'behavior_data.c'))
ported_text += read(os.path.join(ENGINE_DST, 'behavior_script.c'))
wanted = set(re.findall(r'\b([a-z0-9_]+_seg[0-9a-f]_[A-Za-z0-9_]+)\b', ported_text))
wanted |= set(re.findall(r'\b(inside_castle_seg7_\w+|castle_grounds_seg7_\w+)\b', ported_text))
actors_text = ''
for root, dirs, files in os.walk(ACTORS_DST):
    for fn in files:
        if fn.endswith('.c') or fn.endswith('.h'):
            actors_text += read(os.path.join(root, fn))
defined_by_actors = set(re.findall(r'^(?:const|static const|ALIGNED8 static const|ALIGNED8 const)\s+(?:struct\s+)?\w+\s+(?:\*const\s+)?(\w+)\[\]', actors_text, re.M))
missing = sorted(w for w in wanted if w not in defined_by_actors)

level_data_files = []
DEF_RE = re.compile(r'^(?:const\s+)?(?:Collision|Trajectory|s16|u8|Gfx|GeoLayout|Texture|Vtx|Lights1|Movtex|MacroObject|struct Animation)\s+(?:\*const\s+)?(\w+)\[\]', re.M)
level_defs = {}
for root, dirs, files in os.walk(LEVELS_SRC):
    for fn in files:
        if fn.endswith('.inc.c') and ('collision' in fn or 'trajectory' in fn or 'path' in fn):
            path = os.path.join(root, fn)
            for sym in DEF_RE.findall(read(path)):
                level_defs[sym] = path
still_missing = []
for sym in missing:
    path = level_defs.get(sym)
    if path is None:
        still_missing.append(sym)
        continue
    if path not in level_data_files:
        level_data_files.append(path)
for path in level_data_files:
    rel = os.path.relpath(path, LEVELS_SRC)
    copy_patched(path, os.path.join(LEVELS_DST, rel))
level_data_c = ['// generated by import-objects.py: the level data the behaviours reach for (collision plates of moving',
                '// platforms, race and flight paths).  Nothing else of the levels is here.',
                '#include "ultra64.h"', '#include "sm64.h"', '#include "surface_terrains.h"', '#include "level_misc_macros.h"',
                '#include "special_presets.h"', '#include "macro_presets.h"', '#include "macros.h"', '#include "types.h"', '']
for path in level_data_files:
    level_data_c.append('#include "%s"' % os.path.relpath(path, LEVELS_SRC).replace('\\', '/'))
write(os.path.join(LEVELS_DST, 'level_data.c'), '\n'.join(level_data_c) + '\n')
if still_missing:
    print('  data symbols the levels do not define as collision or paths (left to the glue): %s' % ', '.join(still_missing))

# ------------------------------------------------------------------------------------------------ models
# SM64 numbers its models per level: 0x56 is the Bully in Lethal Lava Land, Hoot in Whomp's Fortress, King Bob-omb
# in Bob-omb Battlefield.  So every level script's bindings are kept, with the level, and an object looks its
# models up in the table of the level it believes it is in (objects.c).
MODEL_RE = re.compile(r'LOAD_MODEL_FROM_(GEO|DL)\s*\(\s*(MODEL_[A-Z0-9_]+)\s*,\s*([a-zA-Z0-9_]+)(?:\s*,\s*(LAYER_[A-Z_]+))?\s*\)')
folder_level = {}        # levels/<folder> -> LEVEL_x
for m in re.finditer(r'DEFINE_LEVEL\(\s*"[^"]*",\s*(LEVEL_\w+),\s*\w+,\s*(\w+),', read(os.path.join(LEVELS_SRC, 'level_defines.h'))):
    folder_level[m.group(2)] = m.group(1)
script_files = [os.path.join(LEVELS_SRC, 'scripts.c')] + sorted(glob.glob(os.path.join(LEVELS_SRC, '*', 'script.c')))
bindings = []            # (level or '0', MODEL_x, kind, symbol, layer), in the scripts' order
seen_binding = set()
for path in script_files:
    folder = os.path.basename(os.path.dirname(path))
    level = folder_level.get(folder, '0') if os.path.basename(path) == 'script.c' else '0'
    for m in MODEL_RE.finditer(read(path)):
        kind, model, sym, layer = m.groups()
        if sym not in defined_by_actors:
            continue      # a level's own geometry
        key = (level, model, sym)
        if key in seen_binding:
            continue
        seen_binding.add(key)
        bindings.append((level, model, kind, sym, layer))
model_ids = {}
for m in re.finditer(r'^#define (MODEL_[A-Z0-9_]+)\s+(0x[0-9A-Fa-f]+|\d+)', read(os.path.join(INCLUDE_SRC, 'model_ids.h')), re.M):
    model_ids[m.group(1)] = int(m.group(2), 0)
model_alias = dict(re.findall(r'^#define (MODEL_[A-Z0-9_]+)\s+(MODEL_[A-Z0-9_]+)', read(os.path.join(INCLUDE_SRC, 'model_ids.h')), re.M))


def model_value(name):
    while name in model_alias:
        name = model_alias[name]
    return model_ids.get(name)


models = {}              # MODEL_x (by name) -> [(level, sym)]: what the menu resolves a model name by
dl_models = set()        # ... and which of them are display lists rather than layouts (looked up by id at run time)
for level, model, kind, sym, layer in bindings:
    models.setdefault(model, []).append((level, sym))
    if kind == 'DL':
        dl_models.add(model)

# ------------------------------------------------------------------------------------------------ the menu
# What the spawn menu offers: name (the HUD font is capitals), category, behaviour, model, behaviour parameter
# (SM64's oBhvParams word: the second byte is what most behaviours read), and the level the thing believes it is in
# (0 = the default; Bowser fights differently in each of his three arenas).  Everything is one of the game's own
# behaviours on one of the game's own models, the way the level scripts place it.
E, B, I, H, O, C = 'ENEMIES', 'BOSSES', 'ITEMS', 'HAZARDS', 'OBJECTS', 'CREATURES'
MENU = [
    # enemies
    ("GOOMBA",                E, 'bhvGoomba',             'MODEL_GOOMBA',            0x00000000, 0),
    ("HUGE GOOMBA",           E, 'bhvGoomba',             'MODEL_GOOMBA',            0x00010000, 0),
    ("TINY GOOMBA",           E, 'bhvGoomba',             'MODEL_GOOMBA',            0x00020000, 0),
    ("GOOMBA TRIPLET",        E, 'bhvGoombaTripletSpawner', 'MODEL_NONE',            0x00000000, 0),
    ("KOOPA",                 E, 'bhvKoopa',              'MODEL_KOOPA_WITH_SHELL',  0x00010000, 0),
    ("KOOPA WITHOUT SHELL",   E, 'bhvKoopa',              'MODEL_KOOPA_WITHOUT_SHELL', 0x00000000, 0),
    ("TINY KOOPA",            E, 'bhvKoopa',              'MODEL_KOOPA_WITH_SHELL',  0x00040000, 0),
    ("BOB-OMB",               E, 'bhvBobomb',             'MODEL_BLACK_BOBOMB',      0x00000000, 0),
    ("BULLY",                 E, 'bhvSmallBully',         'MODEL_BULLY',             0x00000000, 0),
    ("CHILL BULLY",           E, 'bhvSmallChillBully',    'MODEL_CHILL_BULLY',       0x00000000, 0),
    ("CHUCKYA",               E, 'bhvChuckya',            'MODEL_CHUCKYA',           0x00000000, 0),
    ("HEAVE-HO",              E, 'bhvHeaveHo',            'MODEL_HEAVE_HO',          0x00000000, 0),
    ("AMP",                   E, 'bhvHomingAmp',          'MODEL_AMP',               0x00000000, 0),
    ("CIRCLING AMP",          E, 'bhvCirclingAmp',        'MODEL_AMP',               0x00020000, 0),
    ("BOO",                   E, 'bhvBoo',                'MODEL_BOO',               0x00000000, 0),
    ("BOO WITH CAGE",         E, 'bhvBooWithCage',        'MODEL_BOO',               0x00000000, 0),
    ("COURTYARD BOOS",        E, 'bhvCourtyardBooTriplet', 'MODEL_BOO_CASTLE',       0x00000000, 0),
    ("FLYING BOOKEND",        E, 'bhvFlyingBookend',      'MODEL_BOOKEND',           0x00000000, 0),
    ("HAUNTED CHAIR",         E, 'bhvHauntedChair',       'MODEL_HAUNTED_CHAIR',     0x00000000, 0),
    ("MAD PIANO",             E, 'bhvMadPiano',           'MODEL_MAD_PIANO',         0x00000000, 0),
    ("BULLET BILL",           E, 'bhvBulletBill',         'MODEL_BULLET_BILL',       0x00000000, 0),
    ("CHAIN CHOMP",           E, 'bhvChainChomp',         'MODEL_CHAIN_CHOMP',       0x00000000, 0),
    ("PIRANHA PLANT",         E, 'bhvPiranhaPlant',       'MODEL_PIRANHA_PLANT',     0x00000000, 0),
    ("FIRE PIRANHA PLANT",    E, 'bhvFirePiranhaPlant',   'MODEL_PIRANHA_PLANT',     0x00000000, 0),
    ("FLY GUY",               E, 'bhvFlyGuy',             'MODEL_FLYGUY',            0x00000000, 0),
    ("KLEPTO",                E, 'bhvKlepto',             'MODEL_KLEPTO',            0x00000000, 0),
    ("LAKITU",                E, 'bhvEnemyLakitu',        'MODEL_ENEMY_LAKITU',      0x00000000, 0),
    ("SPINY",                 E, 'bhvSpiny',              'MODEL_SPINY',             0x00000000, 0),
    ("MONTY MOLE",            E, 'bhvMontyMole',          'MODEL_MONTY_MOLE',        0x00000000, 0),
    ("MONTY MOLE HOLE",       E, 'bhvMontyMoleHole',      'MODEL_DL_MONTY_MOLE_HOLE', 0x00000000, 0),
    ("MR I",                  E, 'bhvMrI',                'MODEL_MR_I',              0x00000000, 0),
    ("MR BLIZZARD",           E, 'bhvMrBlizzard',         'MODEL_MR_BLIZZARD',       0x00010000, 0),
    ("POKEY",                 E, 'bhvPokey',              'MODEL_POKEY_HEAD',        0x00000000, 0),
    ("SCUTTLEBUG",            E, 'bhvScuttlebug',         'MODEL_SCUTTLEBUG',        0x00000000, 0),
    ("SKEETER",               E, 'bhvSkeeter',            'MODEL_SKEETER',           0x00000000, 0),
    ("SNUFIT",                E, 'bhvSnufit',             'MODEL_SNUFIT',            0x00000000, 0),
    ("SPINDRIFT",             E, 'bhvSpindrift',          'MODEL_SPINDRIFT',         0x00000000, 0),
    ("SWOOP",                 E, 'bhvSwoop',              'MODEL_SWOOP',             0x00000000, 0),
    ("THWOMP",                E, 'bhvThwomp',             'MODEL_THWOMP',            0x00000000, 0),
    ("WHOMP",                 E, 'bhvSmallWhomp',         'MODEL_WHOMP',             0x00000000, 0),
    ("BUBBA",                 E, 'bhvBubba',              'MODEL_BUBBA',             0x00000000, 0),
    ("SUSHI SHARK",           E, 'bhvSushiShark',         'MODEL_SUSHI',             0x00000000, 0),
    ("UNAGI",                 E, 'bhvUnagi',              'MODEL_UNAGI',             0x00000000, 0),
    ("CLAM",                  E, 'bhvClamShell',          'MODEL_CLAM_SHELL',        0x00000000, 0),
    ("MANTA RAY",             E, 'bhvMantaRay',           'MODEL_MANTA_RAY',         0x00000000, 0),
    ("WATER BOMB CANNON",     E, 'bhvWaterBombCannon',    'MODEL_NONE',              0x00000000, 0),
    ("BOWLING BALL",          E, 'bhvFreeBowlingBall',    'MODEL_BOWLING_BALL',      0x00000000, 0),
    ("FLAME THROWER",         E, 'bhvFlamethrower',       'MODEL_NONE',              0x00000000, 0),
    ("FIRE SPITTER",          E, 'bhvFireSpitter',        'MODEL_NONE',              0x00000000, 0),
    ("SNOWMAN",               E, 'bhvSLSnowmanWind',      'MODEL_NONE',              0x00000000, 0),
    ("MONEYBAG",              E, 'bhvMoneybag',           'MODEL_MONEYBAG',          0x00000000, 0),
    ("UKIKI",                 E, 'bhvUkiki',              'MODEL_UKIKI',             0x00000000, 0),
    # bosses
    ("KING BOB-OMB",          B, 'bhvKingBobomb',         'MODEL_KING_BOBOMB',       0x00000000, 0),
    ("WHOMP KING",            B, 'bhvWhompKingBoss',      'MODEL_WHOMP',             0x00000000, 0),
    ("BIG BULLY",             B, 'bhvBigBully',           'MODEL_BULLY_BOSS',        0x00000000, 0),
    ("BIG BULLY AND MINIONS", B, 'bhvBigBullyWithMinions', 'MODEL_BULLY_BOSS',       0x00000000, 0),
    ("BIG CHILL BULLY",       B, 'bhvBigChillBully',      'MODEL_BIG_CHILL_BULLY',   0x00000000, 0),
    ("BIG BOO",               B, 'bhvBalconyBigBoo',      'MODEL_BOO',               0x00000000, 0),
    ("BIG MR I",              B, 'bhvMrI',                'MODEL_MR_I',              0x00010000, 0),
    ("WIGGLER",               B, 'bhvWigglerHead',        'MODEL_WIGGLER_HEAD',      0x00000000, 0),
    ("EYEROK",                B, 'bhvEyerokBoss',         'MODEL_NONE',              0x00000000, 0),
    ("BOWSER DARK WORLD",     B, 'bhvBowser',             'MODEL_BOWSER',            0x00000000, 'LEVEL_BITDW'),
    ("BOWSER FIRE SEA",       B, 'bhvBowser',             'MODEL_BOWSER',            0x00000000, 'LEVEL_BITFS'),
    ("BOWSER IN THE SKY",     B, 'bhvBowser',             'MODEL_BOWSER',            0x00000000, 'LEVEL_BITS'),
    ("BOWSER MINE",           B, 'bhvBowserBomb',         'MODEL_BOWSER_BOMB',       0x00000000, 0),
    # items
    ("COIN",                  I, 'bhvYellowCoin',         'MODEL_YELLOW_COIN',       0x00000000, 0),
    ("RED COIN",              I, 'bhvRedCoin',            'MODEL_RED_COIN',          0x00000000, 0),
    ("BLUE COIN",             I, 'bhvBlueCoinJumping',    'MODEL_BLUE_COIN',         0x00000000, 0),
    ("COIN RING",             I, 'bhvCoinFormation',      'MODEL_NONE',              0x00020000, 0),
    ("COIN LINE",             I, 'bhvCoinFormation',      'MODEL_NONE',              0x00000000, 0),
    ("1UP MUSHROOM",          I, 'bhv1Up',                'MODEL_1UP',               0x00000000, 0),
    ("POWER STAR",            I, 'bhvStar',               'MODEL_STAR',              0x00000000, 0),
    ("WING CAP",              I, 'bhvWingCap',            'MODEL_MARIOS_WING_CAP',   0x00000000, 0),
    ("METAL CAP",             I, 'bhvMetalCap',           'MODEL_MARIOS_METAL_CAP',  0x00000000, 0),
    ("VANISH CAP",            I, 'bhvVanishCap',          'MODEL_MARIOS_CAP',        0x00000000, 0),
    ("HEART",                 I, 'bhvRecoveryHeart',      'heart_geo',               0x00000000, 0),
    ("BOX WITH WING CAP",     I, 'bhvExclamationBox',     'MODEL_EXCLAMATION_BOX',   0x00000000, 0),
    ("BOX WITH METAL CAP",    I, 'bhvExclamationBox',     'MODEL_EXCLAMATION_BOX',   0x00010000, 0),
    ("BOX WITH VANISH CAP",   I, 'bhvExclamationBox',     'MODEL_EXCLAMATION_BOX',   0x00020000, 0),
    ("BOX WITH KOOPA SHELL",  I, 'bhvExclamationBox',     'MODEL_EXCLAMATION_BOX',   0x00030000, 0),
    ("BOX WITH COINS",        I, 'bhvExclamationBox',     'MODEL_EXCLAMATION_BOX',   0x00060000, 0),
    ("BOX WITH 1UP",          I, 'bhvExclamationBox',     'MODEL_EXCLAMATION_BOX',   0x00070000, 0),
    ("BOX WITH STAR",         I, 'bhvExclamationBox',     'MODEL_EXCLAMATION_BOX',   0x00080000, 0),
    ("KOOPA SHELL",           I, 'bhvKoopaShell',         'MODEL_KOOPA_SHELL',       0x00000000, 0),
    ("BREAKABLE BOX",         I, 'bhvBreakableBox',       'MODEL_BREAKABLE_BOX',     0x00000000, 0),
    ("SMALL BOX",             I, 'bhvBreakableBoxSmall',  'MODEL_BREAKABLE_BOX_SMALL', 0x00000000, 0),
    ("CRAZY BOX",             I, 'bhvJumpingBox',         'MODEL_BREAKABLE_BOX_SMALL', 0x00000000, 0),
    ("METAL BOX",             I, 'bhvPushableMetalBox',   'MODEL_METAL_BOX',         0x00000000, 0),
    # hazards
    ("FLAME",                 H, 'bhvFlame',              'MODEL_RED_FLAME',         0x00000000, 0),
    ("BLUE FLAME",            H, 'bhvFlame',              'MODEL_BLUE_FLAME',        0x00000000, 0),
    ("BOUNCING FIREBALL",     H, 'bhvBouncingFireball',   'MODEL_NONE',              0x00000000, 0),
    ("TWEESTER",              H, 'bhvTweester',           'MODEL_TWEESTER',          0x00000000, 0),
    ("WATER BOMB",            H, 'bhvWaterBomb',          'MODEL_WATER_BOMB',        0x00000000, 0),
    # creatures (the friendly ones)
    ("BOB-OMB BUDDY",         C, 'bhvBobombBuddy',        'MODEL_BOBOMB_BUDDY',      0x00000000, 0),
    ("TOAD",                  C, 'bhvToadMessage',        'MODEL_TOAD',              0x00000000, 0),
    ("YOSHI",                 C, 'bhvYoshi',              'MODEL_YOSHI',             0x00000000, 0),
    ("MIPS",                  C, 'bhvMips',               'MODEL_MIPS',              0x00000000, 0),
    ("HOOT",                  C, 'bhvHoot',               'MODEL_HOOT',              0x00000000, 0),
    ("DORRIE",                C, 'bhvDorrie',             'MODEL_DORRIE',            0x00000000, 0),
    ("PENGUIN MOTHER",        C, 'bhvTuxiesMother',       'MODEL_PENGUIN',           0x00000000, 0),
    ("SMALL PENGUIN",         C, 'bhvSmallPenguin',       'MODEL_PENGUIN',           0x00000000, 0),
    ("WALKING PENGUIN",       C, 'bhvSLWalkingPenguin',   'MODEL_PENGUIN',           0x00000000, 0),
    ("BUTTERFLY",             C, 'bhvButterfly',          'MODEL_BUTTERFLY',         0x00000000, 0),
    ("FISH",                  C, 'bhvFishSpawner',        'MODEL_NONE',              0x00000000, 0),
    ("SEAWEED",               C, 'bhvSeaweed',            'MODEL_SEAWEED',           0x00000000, 0),
    # objects
    ("TREE",                  O, 'bhvTree',               'bubbly_tree_geo',         0x00000000, 0),
    ("PALM TREE",             O, 'bhvTree',               'palm_tree_geo',           0x00000000, 0),
    ("SNOW TREE",             O, 'bhvTree',               'snow_tree_geo',           0x00000000, 0),
    ("SPIKY TREE",            O, 'bhvTree',               'spiky_tree_geo',          0x00000000, 0),
    ("SIGNPOST",              O, 'bhvMessagePanel',       'MODEL_WOODEN_SIGNPOST',   0x00000000, 0),
    ("WOODEN POST",           O, 'bhvWoodenPost',         'MODEL_WOODEN_POST',       0x00000000, 0),
    ("CHECKERBOARD LIFT",     O, 'bhvCheckerboardElevatorGroup', 'MODEL_NONE',       0x00000000, 0),
    ("TREASURE CHESTS",       O, 'bhvTreasureChestsJRB',  'MODEL_NONE',              0x00000000, 0),
    ("CANNON",                O, 'bhvCannon',             'MODEL_CANNON_BASE',       0x00000000, 0),
    ("WARP PIPE",             O, 'bhvStaticObject',       'MODEL_BITS_WARP_PIPE',    0x00000000, 0),
    ("EXPLOSION",             O, 'bhvExplosion',          'MODEL_EXPLOSION',         0x00000000, 0),
]

behaviour_names = set(re.findall(r'^extern const BehaviorScript (bhv\w+)\[\];', read(os.path.join(INCLUDE_SRC, 'behavior_data.h')), re.M))
levels = {}
for m in re.finditer(r'^\s*DEFINE_LEVEL\(\s*[^,]+,\s*(LEVEL_\w+)', read(os.path.join(DECOMP, 'levels', 'level_defines.h')), re.M):
    levels[m.group(1)] = True
entries = []             # (name, category, bhv, geo symbol or None, model value, bhvParams, level, companion name)
problems = []
COMPANIONS = { 'MONTY MOLE': 'MONTY MOLE HOLE' }   # spawned first, at the same place, for things that need another
for name, cat, bhv, model, bparam, level in MENU:
    if bhv not in behaviour_names:
        problems.append('%s: no behaviour %s' % (name, bhv))
        continue
    if model.endswith('_geo'):
        if model not in defined_by_actors:
            problems.append('%s: no actor geo layout %s' % (name, model))
            continue
        entries.append((name, cat, bhv, model, 0, bparam, level or '0', COMPANIONS.get(name)))
        continue
    if model == 'MODEL_NONE':
        entries.append((name, cat, bhv, None, 0, bparam, level or '0', COMPANIONS.get(name)))
        continue
    if model not in models:
        problems.append('%s: model %s is not bound to an actor by any level script' % (name, model))
        continue
    candidates = models[model]
    chosen = None
    if level:
        for lvl, sym in candidates:
            if lvl == level:
                chosen = (lvl, sym)
                break
    if chosen is None:
        chosen = candidates[0]
    distinct = sorted(set(sym for lvl, sym in candidates))
    if len(distinct) > 1:
        print('  %s: %s is %s in different levels; taking %s (%s)' % (name, model, ', '.join(distinct), chosen[1], chosen[0]))
    geo = None if model in dl_models else chosen[1]      # a display list is found by its id in the level's table
    entries.append((name, cat, bhv, geo, model_value(model) or 0, bparam, level or chosen[0], COMPANIONS.get(name)))
if problems:
    print('  menu entries left out:\n    ' + '\n    '.join(problems))
entry_index = {e[0]: i for i, e in enumerate(entries)}

# ------------------------------------------------------------------------------------------------ generated C
out = ['// generated by import-objects.py - the pictures of the actors, where they lie in the US ROM (assets.json)',
       '#pragma once', '#include <stdint.h>', '',
       '// a texture in a model: the index of its picture in the table below (an array of one so the display-list',
       '// macros take its address the way they took the picture\'s)',
       'typedef struct SM64TexRef { int32_t id; } SM64TexRef;', '',
       'struct SM64ActorTexture { const char *name; uint16_t w, h; uint8_t fmt, bits; uint32_t mio0, offset; };',
       'extern const struct SM64ActorTexture gActorTextures[];',
       'extern const int32_t gActorTextureCount;', '']
write(os.path.join(ACTORS_DST, 'actor_textures.h'), '\n'.join(out) + '\n')
out = ['// generated by import-objects.py', '#include "actor_textures.h"', '',
       'const struct SM64ActorTexture gActorTextures[] = {']
for key, w, h, fmt, bits, mio0, offset in textures:
    out.append('    { "%s", %d, %d, %d, %d, %d, %d },' % (key, w, h, fmt, bits, mio0, offset))
out.append('};')
out.append('const int32_t gActorTextureCount = %d;' % len(textures))
write(os.path.join(ACTORS_DST, 'actor_textures.c'), '\n'.join(out) + '\n')

out = ['// generated by import-objects.py - every model id a level script binds to an actor\'s geo layout or display',
       '// list (LOAD_MODEL_FROM_GEO / LOAD_MODEL_FROM_DL), with the level whose script does it (0: the scripts',
       '// every level runs), for the per-level model tables of objects.c',
       '#include "ultra64.h"', '#include "sm64.h"', '#include "geo_commands.h"', '#include "model_ids.h"', '#include "level_table.h"',
       '#include "types.h"', '#include "actors/common0.h"', '#include "actors/common1.h"']
for g in range(0, 18):
    out.append('#include "actors/group%d.h"' % g)
out += ['#include "model_table.h"', '', 'const struct SM64ModelBinding gModelBindings[] = {']
for level, model, kind, sym, layer in bindings:
    value = model_value(model)
    if value is None:
        continue
    if kind == 'GEO':
        out.append('    { %s, 0x%02X /* %s */, %s, NULL, 0 },' % (level, value, model, sym))
    else:
        out.append('    { %s, 0x%02X /* %s */, NULL, %s, %s },' % (level, value, model, sym, layer or 'LAYER_OPAQUE'))
out += ['    { -1, -1, NULL, NULL, 0 },', '};']
write(os.path.join(ACTORS_DST, 'model_table.c'), '\n'.join(out) + '\n')
write(os.path.join(ACTORS_DST, 'model_table.h'), '''// generated by import-objects.py
#pragma once
#include "types.h"
struct SM64ModelBinding { int32_t level; int32_t model; const GeoLayout *geo; const Gfx *dl; int32_t layer; };
extern const struct SM64ModelBinding gModelBindings[];
''')

out = ['// generated by import-objects.py - what the spawn menu offers',
       '#include "ultra64.h"', '#include "sm64.h"', '#include "behavior_data.h"', '#include "model_ids.h"', '#include "level_table.h"',
       '#include "actors/common0.h"', '#include "actors/common1.h"']
for g in range(0, 18):
    out.append('#include "actors/group%d.h"' % g)
out += ['#include "menu_table.h"', '', 'const struct SM64MenuEntry gMenuEntries[] = {']
for name, cat, bhv, geo, model, bparam, level, companion in entries:
    out.append('    { "%s", "%s", %s, %s, 0x%02X, 0x%08X, %s, %d },' % (name, cat, bhv, geo or 'NULL', model, bparam, level,
                                                                     entry_index.get(companion, -1) if companion else -1))
out += ['};', 'const int32_t gMenuEntryCount = %d;' % len(entries)]
write(os.path.join(ACTORS_DST, 'menu_table.c'), '\n'.join(out) + '\n')
write(os.path.join(ACTORS_DST, 'menu_table.h'), '''// generated by import-objects.py
#pragma once
#include "types.h"
// an entry: its behaviour, its geo layout (NULL: nothing to draw), the model id the behaviour may know it by,
// the behaviour parameters, the level it believes it is in, and an entry spawned first at the same place (-1: none)
struct SM64MenuEntry { const char *name; const char *category; const BehaviorScript *behavior; const GeoLayout *geo; int32_t model; uint32_t bhvParams; int32_t level; int32_t companion; };
extern const struct SM64MenuEntry gMenuEntries[];
extern const int32_t gMenuEntryCount;
''')

# stub headers the ported files name
write(os.path.join(DEC, 'menu', 'file_select.h'), '#pragma once\n// libsm64: no file select\n')
write(os.path.join(DEC, 'goddard', 'renderer.h'), '#pragma once\n// libsm64: no Goddard head\n')

print('imported %d actor files, %d textures, %d model bindings, %d behaviours, %d level data files, %d menu entries'
      % (actor_files, len(textures), len(models), len(behaviour_names), len(level_data_files), len(entries)))
