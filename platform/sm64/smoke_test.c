// smoke_test.c - runs sm64.dll without the game: the ROM, a flat floor, Mario, and then every entry of the spawn
// menu in turn, each ticked for a while, to see that nothing crashes and that they draw.  Built by
// build_smoke_test.bat (32-bit, like the DLL); run it in a folder with sm64.dll and give it the ROM's path:
//   smoke_test.exe "<rom>.z64" [frames per entry] [only this entry] [verbose]
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <windows.h>

#define SM64_LIB_FN
#include "libsm64-master/src/libsm64.h"

typedef void (*fn_global_init)(const uint8_t *, uint8_t *);
typedef void (*fn_global_terminate)(void);
typedef void (*fn_register_debug_print_function)(void (*)(const char *));
typedef void (*fn_static_surfaces_load)(const struct SM64Surface *, uint32_t);
typedef int32_t (*fn_mario_create)(float, float, float);
typedef void (*fn_mario_tick)(int32_t, const struct SM64MarioInputs *, struct SM64MarioState *, struct SM64MarioGeometryBuffers *);
typedef void (*fn_mario_delete)(int32_t);
typedef void (*fn_set_camera)(float, float, float, float, float, float, float, float, float);
typedef void (*fn_set_object_geometry)(struct SM64ObjectGeometryBuffers *);
typedef int32_t (*fn_menu_count)(void);
typedef const char *(*fn_menu_name)(int32_t);
typedef const char *(*fn_menu_category)(int32_t);
typedef int32_t (*fn_object_spawn)(int32_t, float, float, float, float);
typedef void (*fn_objects_clear)(void);
typedef int32_t (*fn_objects_count)(void);
typedef int32_t (*fn_texture_count)(void);
typedef int32_t (*fn_texture_size)(int32_t, int32_t *, int32_t *);
typedef const uint8_t *(*fn_texture_rgba)(int32_t);
typedef void (*fn_set_mario_position)(int32_t, float, float, float);
typedef void (*fn_objects_shift)(float, float, float);
typedef void (*fn_audio_init)(const uint8_t *);
typedef uint32_t (*fn_audio_tick)(uint32_t, uint32_t, int16_t *);
typedef int32_t (*fn_object_info)(int32_t, float *, int32_t *);

static int g_verbose;
static void debug_print(const char *s) { if (g_verbose) printf("  lib: %s\n", s); }

// a crash: where, and the frames above it (the library is built with frame pointers), as offsets into sm64.dll
// for llvm-symbolizer --obj=sm64.dll
static LONG WINAPI on_crash(EXCEPTION_POINTERS *info)
{
    HMODULE lib = GetModuleHandleA("sm64.dll");
    DWORD base = (DWORD)(uintptr_t)lib;
    CONTEXT *c = info->ContextRecord;
    DWORD eip = c->Eip, ebp = c->Ebp;
    printf("\nCRASH: code %08lX at %08lX", info->ExceptionRecord->ExceptionCode, eip);
    if (info->ExceptionRecord->NumberParameters >= 2)
        printf(" (%s %08lX)", info->ExceptionRecord->ExceptionInformation[0] ? "writing" : "reading", (DWORD)info->ExceptionRecord->ExceptionInformation[1]);
    printf("\n  sm64.dll at %08lX\n", base);
    printf("  frame 0: sm64.dll+%08lX\n", eip - base);
    for (int i = 1; i < 24 && ebp > 0x10000 && !IsBadReadPtr((void *)ebp, 8); ++i) {
        DWORD ret = *(DWORD *)(ebp + 4);
        DWORD next = *(DWORD *)ebp;
        if (ret >= base && ret < base + 0x2000000) printf("  frame %d: sm64.dll+%08lX\n", i, ret - base);
        else printf("  frame %d: %08lX\n", i, ret);
        if (next <= ebp) break;
        ebp = next;
    }
    {
        DWORD *sp = (DWORD *)c->Esp;
        int shown = 0;
        printf("  on the stack:");
        for (int i = 0; i < 600 && shown < 40 && !IsBadReadPtr(sp + i, 4); ++i) {
            DWORD w = sp[i];
            if (w >= base && w < base + 0x600000) { printf(" sm64.dll+%08lX", w - base); ++shown; }
        }
        printf("\n");
    }
    fflush(stdout);
    ExitProcess(3);
    return EXCEPTION_CONTINUE_SEARCH;
}

#define CAP 60000

int main(int argc, char **argv)
{
    const char *romPath = argc > 1 ? argv[1] : "sm64.z64";
    int frames = argc > 2 ? atoi(argv[2]) : 150;
    int only = argc > 3 ? atoi(argv[3]) : -1;
    g_verbose = argc > 4 ? atoi(argv[4]) : 0;

    AddVectoredExceptionHandler(1, on_crash);
    HMODULE lib = LoadLibraryA("sm64.dll");
    if (!lib) { printf("no sm64.dll here (%lu)\n", GetLastError()); return 1; }
#define GET(name) fn_##name p_##name = (fn_##name)GetProcAddress(lib, "sm64_" #name); if (!p_##name) { printf("no sm64_%s\n", #name); return 1; }
    GET(global_init) GET(global_terminate) GET(register_debug_print_function) GET(static_surfaces_load)
    GET(mario_create) GET(mario_tick) GET(mario_delete) GET(set_camera) GET(set_object_geometry)
    GET(menu_count) GET(menu_name) GET(menu_category) GET(object_spawn) GET(objects_clear) GET(objects_count)
    GET(texture_count) GET(texture_size) GET(texture_rgba) GET(set_mario_position) GET(objects_shift) GET(audio_init)
    GET(object_info) GET(audio_tick)
#undef GET

    FILE *f = fopen(romPath, "rb");
    if (!f) { printf("cannot read %s\n", romPath); return 1; }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *rom = malloc(n);
    fread(rom, 1, n, f);
    fclose(f);
    if (n != 8 * 1024 * 1024 || rom[0] != 0x80) { printf("%s is not an 8 MB big-endian ROM\n", romPath); return 1; }

    uint8_t *atlas = malloc(SM64_TEXTURE_WIDTH * SM64_TEXTURE_HEIGHT * 4);
    p_register_debug_print_function(debug_print);
    printf("ROM read (%ld bytes), %d textures in the table; initialising...\n", n, p_texture_count());
    fflush(stdout);
    p_global_init(rom, atlas);
    p_audio_init(rom);                           // the sounds the objects make want the engine's lists
    printf("library up\n");
    fflush(stdout);
    {
        int ok = 0, w, h, i;
        for (i = 0; i < p_texture_count(); ++i) if (p_texture_size(i, &w, &h) && p_texture_rgba(i)) ++ok;
        printf("  %d of them decoded\n", ok);
    }

    // a floor: a 16000-wide square at y = 0, and four walls around it
    struct SM64Surface surf[10];
    memset(surf, 0, sizeof(surf));
    int32_t s = 8000;
    int32_t quad[4][3] = { { -s, 0, -s }, { s, 0, -s }, { s, 0, s }, { -s, 0, s } };
    int32_t tri0[3] = { 0, 2, 1 }, tri1[3] = { 0, 3, 2 };
    for (int i = 0; i < 3; ++i) { memcpy(surf[0].vertices[i], quad[tri0[i]], 12); memcpy(surf[1].vertices[i], quad[tri1[i]], 12); }
    int ns = 2;
    for (int side = 0; side < 4; ++side) {
        int32_t a[3], b[3];
        memcpy(a, quad[side], 12);
        memcpy(b, quad[(side + 1) & 3], 12);
        int32_t at[3] = { a[0], 3000, a[2] }, bt[3] = { b[0], 3000, b[2] };
        memcpy(surf[ns].vertices[0], a, 12); memcpy(surf[ns].vertices[1], bt, 12); memcpy(surf[ns].vertices[2], b, 12); ++ns;
        memcpy(surf[ns].vertices[0], a, 12); memcpy(surf[ns].vertices[1], at, 12); memcpy(surf[ns].vertices[2], bt, 12); ++ns;
    }
    p_static_surfaces_load(surf, ns);

    // the camera: from the front by default, or from the right (+x) when asked, to see billboards turn
    float camBack[3] = { 0.0f, 0.287f, 0.958f };
    if (argc > 5 && atoi(argv[5]) == 1) { p_set_camera(1800.0f, 300.0f, -400.0f, -1.0f, -0.15f, 0.0f, 0.0f, 1.0f, 0.0f); camBack[0] = 0.989f; camBack[1] = 0.148f; camBack[2] = 0.0f; printf("camera on the +x side, looking -x\n"); }
    else p_set_camera(0.0f, 600.0f, 1800.0f, 0.0f, -0.3f, -1.0f, 0.0f, 1.0f, 0.0f);

    int32_t mario = p_mario_create(0.0f, 50.0f, 0.0f);
    printf("mario %d\n", mario);
    fflush(stdout);
    if (mario < 0) return 1;

    struct SM64MarioGeometryBuffers mbuf;
    mbuf.position = malloc(SM64_GEO_MAX_TRIANGLES * 9 * sizeof(float));
    mbuf.normal = malloc(SM64_GEO_MAX_TRIANGLES * 9 * sizeof(float));
    mbuf.color = malloc(SM64_GEO_MAX_TRIANGLES * 9 * sizeof(float));
    mbuf.uv = malloc(SM64_GEO_MAX_TRIANGLES * 6 * sizeof(float));
    struct SM64ObjectGeometryBuffers obuf;
    obuf.position = malloc(CAP * 9 * sizeof(float));
    obuf.normal = malloc(CAP * 9 * sizeof(float));
    obuf.color = malloc(CAP * 12 * sizeof(float));
    obuf.uv = malloc(CAP * 6 * sizeof(float));
    obuf.texture = malloc(CAP * sizeof(uint16_t));
    obuf.flags = malloc(CAP);
    obuf.layer = malloc(CAP);
    obuf.object = malloc(CAP * sizeof(uint16_t));
    obuf.capacity = CAP;
    obuf.numTrianglesUsed = 0;
    p_set_object_geometry(&obuf);

    struct SM64MarioInputs in;
    struct SM64MarioState st;
    memset(&in, 0, sizeof(in));
    in.camLookX = 0.0f;
    in.camLookZ = -1.0f;

    // a few frames of Mario alone
    for (int i = 0; i < 30; ++i) p_mario_tick(mario, &in, &st, &mbuf);
    printf("mario alone: %d triangles, %u object triangles, at %.0f %.0f %.0f action %08X\n", mbuf.numTrianglesUsed,
           obuf.numTrianglesUsed, st.position[0], st.position[1], st.position[2], st.action);
    fflush(stdout);

    int count = p_menu_count();
    int failures = 0;
    // the grab test: the thing spawned facing away, Mario walks up behind it, punches to grab it, holds it a
    // moment and throws it, for the things he can carry (King Bob-omb, Bob-ombs, Bowser's tail).  Extra args:
    // the frame of the punch (he must still be touching its back then) and the frame of the throw.
    if (argc > 6 && atoi(argv[6]) == 1 && only >= 0) {
        int punchAt = argc > 7 ? atoi(argv[7]) : 18;
        int throwAt = argc > 8 ? atoi(argv[8]) : 90;
        int total = argc > 9 ? atoi(argv[9]) : 500;
        int hostlike = argc > 10 ? atoi(argv[10]) : 0;
        static int16_t audio[8 * 1024];                                        // two buffers of up to 544 stereo samples
        printf("grab test: %s (punch at %d, throw at %d, %d frames)\n", p_menu_name(only), punchAt, throwAt, total);
        fflush(stdout);
        p_set_mario_position(mario, 0.0f, 50.0f, 0.0f);
        int32_t slot = p_object_spawn(only, 0.0f, 50.0f, -350.0f, 3.14159f);   // facing away from him (he faces -z? yaw 0 = +z)
        printf("  spawned in slot %d\n", slot);
        uint32_t lastAction = 0xFFFFFFFF;
        for (int i = 0; i < total; ++i) {
            memset(&in, 0, sizeof(in));
            in.camLookZ = -1.0f;
            if (i < punchAt) in.stickY = -1.0f;                                  // walk up behind it (stick up moves him +z here, so down)
            in.buttonB = (i == punchAt || i == punchAt + 1 || i == throwAt || i == throwAt + 1);   // grab, later throw
            if (i >= throwAt + 100 && i < throwAt + 160) in.stickY = -1.0f;      // and walk into whatever it did next
            if (hostlike & 1) {                                                  // what the game does around a step
                float a = (float)i * 0.05f;
                p_set_camera(st.position[0] + 1500.0f * sinf(a), st.position[1] + 500.0f, st.position[2] + 1500.0f * cosf(a),
                             -sinf(a), -0.3f, -cosf(a), 0.0f, 1.0f, 0.0f);
            }
            if ((hostlike & 2) && i % 10 == 5) {
                p_static_surfaces_load(surf, ns);
                p_objects_shift(7.0f, 0.0f, -7.0f);
                p_set_mario_position(mario, st.position[0] + 7.0f, st.position[1], st.position[2] - 7.0f);
            }
            if (hostlike & 4) p_audio_tick(0, 1024, audio);
            p_mario_tick(mario, &in, &st, &mbuf);
            if (i % 25 == 0 || st.action != lastAction) {
                printf("  frame %3d: mario action %08X at %.0f %.0f %.0f hp %d, %d objects, %u tris\n", i, st.action, st.position[0], st.position[1], st.position[2], st.health, p_objects_count(), obuf.numTrianglesUsed);
                lastAction = st.action;
            }
            fflush(stdout);
        }
        p_objects_clear();
        printf("grab test done\n");
        p_mario_delete(mario);
        p_global_terminate();
        return 0;
    }
    for (int e = 0; e < count; ++e) {
        if (only >= 0 && e != only) continue;
        printf("[%3d] %-24s %-10s ", e, p_menu_name(e), p_menu_category(e));
        fflush(stdout);
        p_set_mario_position(mario, 0.0f, 50.0f, 0.0f);
        int32_t slot = p_object_spawn(e, 0.0f, 50.0f, -400.0f, 3.14159f);
        if (slot < 0) { printf("NOT SPAWNED\n"); ++failures; continue; }
        uint32_t maxTris = 0;
        int32_t last = 0;
        int textured = 0, lit = 0, unlit = 0;
        for (int i = 0; i < frames; ++i) {
            // walk towards it for the second half, punch now and then
            in.stickY = (i > frames / 2) ? 0.6f : 0.0f;
            in.stickX = 0.0f;
            in.buttonB = (i % 40 == 0);
            in.buttonA = (i % 70 == 5);
            p_mario_tick(mario, &in, &st, &mbuf);
            if (obuf.numTrianglesUsed > maxTris) maxTris = obuf.numTrianglesUsed;
            last = p_objects_count();
        }
        for (uint32_t t = 0; t < obuf.numTrianglesUsed; ++t) {
            if (obuf.texture[t] != 0xFFFF) ++textured;
            if (obuf.flags[t] & SM64_TRI_LIT) ++lit; else ++unlit;
        }
        // where do the triangles lie
        float lo[3] = { 1e9f, 1e9f, 1e9f }, hi[3] = { -1e9f, -1e9f, -1e9f };
        for (uint32_t t = 0; t < obuf.numTrianglesUsed * 3; ++t)
            for (int c = 0; c < 3; ++c) {
                float v = obuf.position[t * 3 + c];
                if (v < lo[c]) lo[c] = v;
                if (v > hi[c]) hi[c] = v;
            }
        printf("ok: %d objects left, up to %u tris (%d textured, %d lit, %d unlit), mario hp %d at %.0f %.0f %.0f",
               last, maxTris, textured, lit, unlit, st.health, st.position[0], st.position[1], st.position[2]);
        if (obuf.numTrianglesUsed) printf(", bounds x %.0f..%.0f y %.0f..%.0f z %.0f..%.0f", lo[0], hi[0], lo[1], hi[1], lo[2], hi[2]);
        printf("\n");
        if (g_verbose) {                         // what each object drew this frame
            for (int slot = 0; slot < 240; ++slot) {
                float pos[3]; int32_t ent = -1;
                if (!p_object_info(slot, pos, &ent)) continue;
                int tris = 0, tex = 0, facing = 0;
                float nrm[3] = { 0, 0, 0 };
                for (uint32_t t = 0; t < obuf.numTrianglesUsed; ++t) if (obuf.object[t] == slot) {
                    // each triangle's own normal from its corners: a billboard's faces the camera (either way round)
                    const float *a = &obuf.position[t * 9], *b = a + 3, *c = a + 6;
                    float e1[3] = { b[0] - a[0], b[1] - a[1], b[2] - a[2] }, e2[3] = { c[0] - a[0], c[1] - a[1], c[2] - a[2] };
                    float n3[3] = { e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0] };
                    float len = sqrtf(n3[0] * n3[0] + n3[1] * n3[1] + n3[2] * n3[2]);
                    if (len > 0) { n3[0] /= len; n3[1] /= len; n3[2] /= len; }
                    float dot = n3[0] * camBack[0] + n3[1] * camBack[1] + n3[2] * camBack[2];
                    if (dot > 0.97f || dot < -0.97f) ++facing;
                    if (tris == 0) { nrm[0] = n3[0]; nrm[1] = n3[1]; nrm[2] = n3[2]; }
                    ++tris; if (obuf.texture[t] != 0xFFFF) ++tex;
                }
                printf("      slot %3d: %-22s at %6.0f %6.0f %6.0f, %d tris (%d textured, %d square to the camera), first normal %.2f %.2f %.2f\n", slot, ent >= 0 ? p_menu_name(ent) : "(other behaviour)", pos[0], pos[1], pos[2], tris, tex, facing, nrm[0], nrm[1], nrm[2]);
            }
        }
        fflush(stdout);
        p_objects_clear();
        if (p_objects_count() != 0) printf("   !! %d objects survived the clear\n", p_objects_count());
        // and a few frames with nothing spawned
        for (int i = 0; i < 10; ++i) p_mario_tick(mario, &in, &st, &mbuf);
        if (st.health < 0x100) { p_mario_delete(mario); mario = p_mario_create(0.0f, 50.0f, 0.0f); printf("   (Mario died: made again as %d)\n", mario); }
    }
    // everything at once, then a shift
    if (only < 0) {
        int spawned = 0;
        for (int e = 0; e < count; ++e) {
            float a = (float)e / count * 6.2831f;
            if (p_object_spawn(e, 1500.0f * sinf(a), 50.0f, 1500.0f * cosf(a), a) >= 0) ++spawned;
        }
        printf("all %d at once: %d objects\n", spawned, p_objects_count());
        fflush(stdout);
        for (int i = 0; i < frames; ++i) p_mario_tick(mario, &in, &st, &mbuf);
        printf("  after %d frames: %d objects, %u triangles\n", frames, p_objects_count(), obuf.numTrianglesUsed);
        p_objects_shift(100.0f, 0.0f, -100.0f);
        for (int i = 0; i < 30; ++i) p_mario_tick(mario, &in, &st, &mbuf);
        printf("  after a shift: %d objects, %u triangles\n", p_objects_count(), obuf.numTrianglesUsed);
        p_objects_clear();
    }
    p_mario_delete(mario);
    p_global_terminate();
    printf("done, %d entries failed to spawn\n", failures);
    return failures ? 2 : 0;
}
