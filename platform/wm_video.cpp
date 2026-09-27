// wm_video.cpp - Wii video natives and video instrumentation ([video] natives=1 log=0 in wiimote.ini).
//
// natives=1  The ten K3D_Bink "current video" and preload natives are empty stubs in the PC executable. They get real
//            handlers with the Wii semantics (K3D_Bink*__FPUl and the K3D_texpro_bink statics of the Wii
//            executable), mapped onto the PC Bink player (K3D_texpro_bink, vtable 008A17A4). The Wii tracks one current
//            video (mpo_Current: set by Run and b_Update, cleared by Stop). The PC updates every Bink texture on its
//            own and keeps only the background video (K3D+0x684B4), so the current video is the texture last started
//            by K3D_BinkPlay / ForcePlay / Switch / SwitchToKey while it runs, else the running background video. The
//            Wii preloads a video file into memory before switching to it; the PC opens videos straight from the
//            bigfile, so a preload completes at once.
// log=1      (with [general] log=1) Every video played is logged: frames shown and skipped by the decode step
//            (0045CC40), wall time against the video's own length, gaps between updates, updates blocked by DT == 0
//            or by the display flag, and the TIME_SetFactor value. Every video script call is logged too (first
//            call, then each change of its arguments or result).
#include "wiimote.h"

#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>

using namespace wmpatch;

namespace {

typedef void* (__cdecl* NativeFn)(void* ip);
typedef void* (__cdecl* FindFn)(uint32_t textureKey);
typedef int (__fastcall* ThisIntFn)(void* self, void* edx);
typedef float (__fastcall* ThisFloatFn)(void* self, void* edx);

// ---------------------------------------------------------------------------------------------------------------
// engine state and functions (the PC executable's addresses; the self-test points them at fakes)
// ---------------------------------------------------------------------------------------------------------------
struct VideoCtx {
    uint32_t*   vmIndex, *vmOffset, *vmData, *vmPtrs;  // script stack (0x00A7E188 / 18C / 198 / 19C)
    uint32_t*   displayCell;                            // 0x009DE178 K3D_gpo_Display
    float*      timeFactor;                             // 0x009D8DA8 TIME_SetFactor value
    float*      dt;                                     // 0x00A928BC
    FindFn      findByKey;                              // 0086D2A0: texture key -> its Bink player, or NULL
    ThisIntFn   isRunning;                              // 0047B640: running and not paused
    ThisIntFn   pause;                                  // 0047B6B0
    ThisFloatFn length;                                 // 0047B6E0: frames / fps
};

VideoCtx s_game = {
    (uint32_t*)0x00A7E188, (uint32_t*)0x00A7E18C, (uint32_t*)0x00A7E198, (uint32_t*)0x00A7E19C,
    (uint32_t*)0x009DE178, (float*)0x009D8DA8, (float*)0x00A928BC,
    (FindFn)0x0086D2A0, (ThisIntFn)0x0047B640, (ThisIntFn)0x0047B6B0, (ThisFloatFn)0x0047B6E0,
};
VideoCtx* s_ctx = &s_game;

// K3D_texpro_bink (PC) and its Wii counterparts
const uint32_t P_FLAG8 = 0x08;          // update even while DT is 0
const uint32_t P_OPT   = 0x0C;          // Wii +0x1C: bit 0 stop at the last frame, bit 3 pause at the last frame
const uint32_t P_STATE = 0x0D;          // Wii +0x1D: bit 0 running, bit 1 paused, bit 2 last frame reached
const uint32_t P_FPS   = 0x14;          // Wii +0x18 (float)
const uint32_t P_KEY   = 0x34;          // Wii +0x0C: Bink file key
const uint32_t P_HBINK = 0x44;          // Wii +0x38: HBINK {Width, Height, Frames +8, FrameNum +0xC, LastFrameNum +0x10}
const uint32_t VT_UPDATE = 0x1C, VT_STOP = 0x30, VT_GOTO = 0x3C, VT_DECODE = 0x40, VT_FRAMES = 0x54;
const uint32_t D_BGVIDEO = 0x684B4;     // K3D: background video (K3D::BVO_SetBgVideO)
const uint32_t D_BLOCKED = 0x550;       // K3D: procedural updates disabled
const uint32_t BINK_VTABLE = 0x008A17A4;
const uint32_t DECODE_STEP = 0x0045CC40, PLAYER_UPDATE = 0x0047B890;
const uint8_t  ST_RUNNING = 1, ST_PAUSED = 2, OPT_STOP_AT_END = 1;

inline uint8_t  Byte(void* p, uint32_t off) { return *((uint8_t*)p + off); }
inline uint32_t U32(void* p, uint32_t off) { uint32_t v; memcpy(&v, (uint8_t*)p + off, 4); return v; }
inline float    F32(void* p, uint32_t off) { float v; memcpy(&v, (uint8_t*)p + off, 4); return v; }
inline void*    VSlot(void* p, uint32_t off) { return (*(void***)p)[off / 4]; }

bool         s_natives;
bool         s_logCalls;
bool         s_logVideo;
bool         s_inAttach;
bool         s_verified;
std::vector<std::string> s_pending;
std::string* s_testLog;

uint32_t s_trackKey;                    // texture key of the last K3D_BinkPlay / ForcePlay / Switch / SwitchToKey
uint32_t s_preloadKey;                  // Wii msh_PreloadBinkFile
int32_t  s_preloading;                  // Wii msb_IsPreloading (0)
int32_t  s_preloadFinished = 1;         // Wii msb_IsPreloadFinished (.sdata 1)

void VidLog(const char* fmt, ...) {
    char buf[900];
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf(buf, sizeof(buf) - 1, fmt, ap);
    buf[sizeof(buf) - 1] = 0;
    va_end(ap);
    if (s_testLog) {
        *s_testLog += "    ";
        *s_testLog += buf;
        *s_testLog += "\n";
    } else if (s_inAttach) {
        s_pending.push_back(buf);
    } else {
        Log("VIDEO: %s", buf);
    }
}

double NowSec() {
    static LARGE_INTEGER freq;
    if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart / (double)freq.QuadPart;
}

// ---------------------------------------------------------------------------------------------------------------
// script VM protocol
// ---------------------------------------------------------------------------------------------------------------
inline uint32_t* Ptrs() { return (uint32_t*)(uintptr_t)*s_ctx->vmPtrs; }
int32_t Arg(int n, int i) { return *(int32_t*)(uintptr_t)Ptrs()[*s_ctx->vmIndex - n + i]; }

void PopInts(int n) {
    *s_ctx->vmIndex -= n;
    *s_ctx->vmOffset -= 4 * n;
}

uint8_t* PushEntry() {
    uint8_t* at = (uint8_t*)(uintptr_t)(*s_ctx->vmData + *s_ctx->vmOffset);
    Ptrs()[*s_ctx->vmIndex] = (uint32_t)(uintptr_t)at;
    *s_ctx->vmIndex += 1;
    *s_ctx->vmOffset += 4;
    return at;
}

void PushInt(int32_t v) { memcpy(PushEntry(), &v, 4); }
void PushFloat(float v) { memcpy(PushEntry(), &v, 4); }
inline void* Next(void* ip) { return (uint8_t*)ip + 4; }

// ---------------------------------------------------------------------------------------------------------------
// words: the stubs replaced, the words wrapped (current video tracking, call log)
// ---------------------------------------------------------------------------------------------------------------
enum {
    W_IS_RUNNING, W_PAUSE_CURRENT, W_STOP_CURRENT, W_GOTO_CURRENT, W_LENGTH_CURRENT,
    W_PRELOAD, W_IS_PRELOADING, W_PRELOADED_KEY, W_PRELOAD_FINISHED, W_CANCEL_PRELOAD,
    W_PLAY, W_FORCE_PLAY, W_SWITCH, W_SWITCH_TO_KEY, W_STOP_IF_RUNNING,
    W_PAUSE, W_GOTO, W_LENGTH, W_IS_IN_PAUSE,
    W_BVO_IS_RUNNING, W_BVO_RUN, W_BVO_PAUSE, W_BVO_STOP, W_BVO_IS_PAUSED, W_BVO_CURSOR, W_BVO_LENGTH, W_BVO_FRAME,
    W_BVO_PLAY,
    N_WORDS
};

enum Kind { K_STUB, K_TRACK, K_LOG };
enum Result { R_NONE, R_INT, R_FLOAT };

std::string s_lastNote[N_WORDS];
unsigned    s_noteCount[N_WORDS];
NativeFn    s_orig[N_WORDS];
bool        s_done[N_WORDS];

void NoteCall(int i, const char* name, const char* text) {
    if (!s_logCalls) return;
    if (s_noteCount[i] && s_lastNote[i] == text) return;
    if (s_noteCount[i] >= 200) {
        if (s_noteCount[i] == 200) VidLog("%s: further changes are not logged", name);
        s_noteCount[i] = 201;
        return;
    }
    VidLog("%s%s", s_noteCount[i] ? "" : "first call: ", text);
    s_lastNote[i] = text;
    ++s_noteCount[i];
}

void AppendValue(std::string& s, int32_t v) {
    char b[16];
    if (v > 0xFFFF || v < -0xFFFF) _snprintf(b, sizeof(b), "%08X", (uint32_t)v);
    else _snprintf(b, sizeof(b), "%d", v);
    b[sizeof(b) - 1] = 0;
    s += b;
}

// The Wii's current video on PC: the tracked texture while its player runs, else the running background video.
void* Current() {
    const VideoCtx& c = *s_ctx;
    if (s_trackKey) {
        void* p = c.findByKey(s_trackKey);
        if (p && (Byte(p, P_STATE) & ST_RUNNING)) return p;
    }
    uint32_t disp = *c.displayCell;
    if (disp) {
        void* bg = (void*)(uintptr_t)U32((void*)(uintptr_t)disp, D_BGVIDEO);
        if (bg && (Byte(bg, P_STATE) & ST_RUNNING)) return bg;
    }
    return NULL;
}

void Note(int i, const char* name, void* cur, const char* fmt, ...) {
    if (!s_logCalls) return;
    char buf[300];
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf(buf, sizeof(buf) - 1, fmt, ap);
    buf[sizeof(buf) - 1] = 0;
    va_end(ap);
    std::string t = buf;
    char c[64];
    if (cur) _snprintf(c, sizeof(c), " (current video: Bink key %08X)", U32(cur, P_KEY));
    else strcpy(c, " (no current video)");
    c[sizeof(c) - 1] = 0;
    t += c;
    NoteCall(i, name, t.c_str());
}

// ---- replacements for the ten stubs (stack effect of the stub they replace) ----
void* __cdecl H_IsRunning(void* ip) {           // Wii IsRunning: mpo_Current && b_IsRunning (running, not paused)
    void* cur = Current();
    int32_t r = (cur && s_ctx->isRunning(cur, NULL)) ? 1 : 0;
    PushInt(r);
    Note(W_IS_RUNNING, "K3D_BinkIsRunning", cur, "K3D_BinkIsRunning() -> %d", r);
    return Next(ip);
}

void* __cdecl H_PauseCurrent(void* ip) {        // Wii PauseCurrent: current with option bit 0 -> Pause(), 1
    void* cur = Current();
    int32_t r = 0;
    if (cur && (Byte(cur, P_OPT) & OPT_STOP_AT_END)) {
        s_ctx->pause(cur, NULL);
        r = 1;
    }
    PushInt(r);
    Note(W_PAUSE_CURRENT, "K3D_BinkPauseCurrentVideo", cur, "K3D_BinkPauseCurrentVideo() -> %d", r);
    return Next(ip);
}

void* __cdecl H_StopCurrent(void* ip) {         // Wii StopCurrent: Stop(0) on the current video (clears it), 1
    void* cur = Current();
    int32_t r = 0;
    if (cur) {
        ((void (__fastcall*)(void*, void*, uint32_t))VSlot(cur, VT_STOP))(cur, NULL, 0);
        s_trackKey = 0;
        r = 1;
    }
    PushInt(r);
    Note(W_STOP_CURRENT, "K3D_BinkStopCurrentVideo", cur, "K3D_BinkStopCurrentVideo() -> %d", r);
    return Next(ip);
}

void* __cdecl H_GotoCurrent(void* ip) {         // Wii GotoCurrent(frame): GoToFrame on the current video, 1
    int32_t frame = Arg(1, 0);
    PopInts(1);
    void* cur = Current();
    int32_t r = 0;
    if (cur) {
        ((void (__fastcall*)(void*, void*, int32_t))VSlot(cur, VT_GOTO))(cur, NULL, frame);
        r = 1;
    }
    PushInt(r);
    Note(W_GOTO_CURRENT, "K3D_BinkGotoCurrentVideo", cur, "K3D_BinkGotoCurrentVideo(%d) -> %d", frame, r);
    return Next(ip);
}

void* __cdecl H_LengthCurrent(void* ip) {       // Wii GetLengthCurrent: frames / fps of the current video, else 0
    void* cur = Current();
    float len = 0.0f;
    if (cur && U32(cur, P_HBINK) && F32(cur, P_FPS) > 0.0f) len = s_ctx->length(cur, NULL);
    PushFloat(len);
    Note(W_LENGTH_CURRENT, "K3D_BinkLengthGetCurrentVideo", cur, "K3D_BinkLengthGetCurrentVideo() -> %.2f", len);
    return Next(ip);
}

void* __cdecl H_Preload(void* ip) {             // Wii b_Preload(key, 1): 0 for key 0, 1 when already preloaded
    uint32_t key = (uint32_t)Arg(1, 0);
    PopInts(1);
    int32_t r = 1;
    if (!key) r = 0;
    else if (key != s_preloadKey) {             // PC: nothing to load in advance, the preload is finished at once
        s_preloadKey = key;
        s_preloading = 0;
        s_preloadFinished = 1;
    }
    PushInt(r);
    Note(W_PRELOAD, "K3D_BinkPreload", NULL, "K3D_BinkPreload(%08X) -> %d", key, r);
    return Next(ip);
}

void* __cdecl H_IsPreloading(void* ip) {
    PushInt(s_preloading);
    Note(W_IS_PRELOADING, "K3D_BinkIsPreloading", NULL, "K3D_BinkIsPreloading() -> %d", s_preloading);
    return Next(ip);
}

void* __cdecl H_PreloadedKey(void* ip) {
    PushInt((int32_t)s_preloadKey);
    Note(W_PRELOADED_KEY, "K3D_BinkGetPreloadedBinkKey", NULL, "K3D_BinkGetPreloadedBinkKey() -> %08X", s_preloadKey);
    return Next(ip);
}

void* __cdecl H_PreloadFinished(void* ip) {
    PushInt(s_preloadFinished);
    Note(W_PRELOAD_FINISHED, "K3D_BinkIsPreloadFinished", NULL, "K3D_BinkIsPreloadFinished() -> %d", s_preloadFinished);
    return Next(ip);
}

void* __cdecl H_CancelPreload(void* ip) {       // Wii FinishPreload(opened, 1): acts only while a video is current
    void* cur = Current();
    if (cur) {
        s_preloadKey = 0;
        s_preloading = 0;
        s_preloadFinished = 1;
    }
    Note(W_CANCEL_PRELOAD, "K3D_BinkCancelPreload", cur, "K3D_BinkCancelPreload()%s", cur ? " cancelled" : " ignored");
    return Next(ip);
}

// ---- wrapped words ----
struct Word {
    uint32_t key;                   // native table key; 0 = module word (BVO), matched by its handler
    uint32_t oldFn, len, crc;       // engine handler
    uint32_t reg;                   // registration imm32 (ViD::RegisterScript, or the BVO registrar for key 0)
    NativeFn fn;                    // replacement handler (K_STUB), else NULL (wrapped by a thunk)
    uint8_t  kind, result;
    const char* name;
};

void* WordCall(int i, void* ip);
#define VIDEO_THUNK(n) void* __cdecl Thunk##n(void* ip) { return WordCall(n, ip); }
VIDEO_THUNK(10) VIDEO_THUNK(11) VIDEO_THUNK(12) VIDEO_THUNK(13) VIDEO_THUNK(14) VIDEO_THUNK(15) VIDEO_THUNK(16)
VIDEO_THUNK(17) VIDEO_THUNK(18) VIDEO_THUNK(19) VIDEO_THUNK(20) VIDEO_THUNK(21) VIDEO_THUNK(22) VIDEO_THUNK(23)
VIDEO_THUNK(24) VIDEO_THUNK(25) VIDEO_THUNK(26) VIDEO_THUNK(27)

const Word kWords[N_WORDS] = {
    { 0x0134FFFF, 0x005ECA50, 0x043, 0x111E45B4, 0x00543875, H_IsRunning, K_STUB, R_INT, "K3D_BinkIsRunning" },
    { 0x0130FFFF, 0x005ECA50, 0x043, 0x111E45B4, 0x005437A6, H_PauseCurrent, K_STUB, R_INT, "K3D_BinkPauseCurrentVideo" },
    { 0x0133FFFF, 0x005ECA50, 0x043, 0x111E45B4, 0x00543841, H_StopCurrent, K_STUB, R_INT, "K3D_BinkStopCurrentVideo" },
    { 0x0131FFFF, 0x005D6F00, 0x057, 0xE19428D2, 0x005437DA, H_GotoCurrent, K_STUB, R_INT, "K3D_BinkGotoCurrentVideo" },
    { 0x0132FFFF, 0x005EC990, 0x042, 0xC0FCDE69, 0x00543802, H_LengthCurrent, K_STUB, R_FLOAT,
      "K3D_BinkLengthGetCurrentVideo" },
    { 0x0136FFFF, 0x005D6F00, 0x057, 0xE19428D2, 0x005438DD, H_Preload, K_STUB, R_INT, "K3D_BinkPreload" },
    { 0x0138FFFF, 0x005ECA50, 0x043, 0x111E45B4, 0x00543945, H_IsPreloading, K_STUB, R_INT, "K3D_BinkIsPreloading" },
    { 0x0139FFFF, 0x005ECA50, 0x043, 0x111E45B4, 0x00543979, H_PreloadedKey, K_STUB, R_INT, "K3D_BinkGetPreloadedBinkKey" },
    { 0x0137FFFF, 0x005225F0, 0x044, 0x707BC86A, 0x00543911, H_PreloadFinished, K_STUB, R_INT, "K3D_BinkIsPreloadFinished" },
    { 0x0135FFFF, 0x00560580, 0x008, 0xBAC82FF4, 0x005438A9, H_CancelPreload, K_STUB, R_NONE, "K3D_BinkCancelPreload" },
    { 0x012EFFFF, 0x005EC7C0, 0x030, 0xC81EE532, 0x0054373E, NULL, K_TRACK, R_NONE, "K3D_BinkPlay" },
    { 0x013AFFFF, 0x005EC7F0, 0x030, 0xC81EE532, 0x005439AD, NULL, K_TRACK, R_NONE, "K3D_BinkForcePlay" },
    { 0x013DFFFF, 0x005ECAF0, 0x045, 0xC5E62F76, 0x00543A49, NULL, K_TRACK, R_NONE, "K3D_BinkSwitch" },
    { 0x013BFFFF, 0x005ECAA0, 0x045, 0x4D2311E8, 0x005439E1, NULL, K_TRACK, R_NONE, "K3D_BinkSwitchToKey" },
    { 0x013CFFFF, 0x005EC9E0, 0x068, 0xDFE03210, 0x00543A15, NULL, K_TRACK, R_INT, "K3D_BinkStopIfRunning" },
    { 0x012CFFFF, 0x005EC820, 0x030, 0x02E59E4F, 0x005436C8, NULL, K_LOG, R_NONE, "K3D_BinkPause" },
    { 0x012DFFFF, 0x005EC8C0, 0x045, 0x7FC5C488, 0x0054370A, NULL, K_LOG, R_NONE, "K3D_BinkGoto" },
    { 0x012FFFFF, 0x005EC910, 0x071, 0x1FAAFB99, 0x00543772, NULL, K_LOG, R_FLOAT, "K3D_BinkLengthGet" },
    { 0x013EFFFF, 0x005EC850, 0x068, 0xFABDAAA7, 0x00543A7D, NULL, K_LOG, R_INT, "K3D_BinkIsInPause" },
    { 0, 0x00604BA0, 0x0E8, 0xAB3061E7, 0x0060496F, NULL, K_LOG, R_INT, "BVO_IsRunning" },
    { 0, 0x00604C90, 0x104, 0x59FEF118, 0x006049A5, NULL, K_LOG, R_INT, "BVO_Run" },
    { 0, 0x00604DA0, 0x0E2, 0x5BE074AB, 0x006049DB, NULL, K_LOG, R_INT, "BVO_Pause" },
    { 0, 0x00604F40, 0x104, 0x941AF1DF, 0x00604A11, NULL, K_LOG, R_INT, "BVO_Stop" },
    { 0, 0x00605140, 0x0E2, 0x746DD735, 0x00604AB3, NULL, K_LOG, R_INT, "BVO_IsPaused" },
    { 0, 0x00605230, 0x0EA, 0xC3D5E62F, 0x00604AE9, NULL, K_LOG, R_FLOAT, "BVO_CursorGet" },
    { 0, 0x00605320, 0x0EA, 0xBF46A29C, 0x00604B1F, NULL, K_LOG, R_FLOAT, "BVO word 00605320 (video length)" },
    { 0, 0x00605410, 0x0E2, 0xAEDF04EC, 0x00604B55, NULL, K_LOG, R_INT, "BVO word 00605410 (vtable +0x50)" },
    { 0, 0x00604E90, 0x0A7, 0x8540ADF6, 0x00604B80, NULL, K_LOG, R_NONE, "BVO word 00604E90 (play)" },
};

const NativeFn kThunks[N_WORDS] = {
    NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    Thunk10, Thunk11, Thunk12, Thunk13, Thunk14, Thunk15, Thunk16, Thunk17, Thunk18, Thunk19, Thunk20, Thunk21, Thunk22,
    Thunk23, Thunk24, Thunk25, Thunk26, Thunk27,
};

inline NativeFn Replacement(int i) { return kWords[i].kind == K_STUB ? kWords[i].fn : kThunks[i]; }

bool Needed(int i, bool natives, bool log) {
    switch (kWords[i].kind) {
    case K_STUB: return natives;
    case K_TRACK: return natives || log;
    default: return log;
    }
}

// Wrapped word: run the engine handler, then track the current video and log the call.
void* WordCall(int i, void* ip) {
    const Word& w = kWords[i];
    uint32_t before = *s_ctx->vmIndex;
    int32_t snap[4] = { 0, 0, 0, 0 };
    uint32_t ns = before < 4 ? before : 4;
    for (uint32_t k = 0; k < ns; ++k) memcpy(&snap[4 - ns + k], (void*)(uintptr_t)Ptrs()[before - ns + k], 4);
    NativeFn orig = s_orig[i] ? s_orig[i] : (NativeFn)(uintptr_t)w.oldFn;
    void* next = orig(ip);
    uint32_t after = *s_ctx->vmIndex;
    int pushed = w.result != R_NONE ? 1 : 0;
    int nargs = (int)before - (int)after + pushed;
    if (nargs < 0 || nargs > (int)ns) nargs = 0;
    int32_t res = 0;
    if (pushed && after > 0) memcpy(&res, (void*)(uintptr_t)Ptrs()[after - 1], 4);
    const int32_t* args = snap + 4 - nargs;
    if (s_natives && nargs >= 1) {
        if (i == W_PLAY || i == W_FORCE_PLAY || i == W_SWITCH || i == W_SWITCH_TO_KEY) s_trackKey = (uint32_t)args[0];
        if (i == W_STOP_IF_RUNNING && res == 1 && (uint32_t)args[0] == s_trackKey) s_trackKey = 0;
    }
    if (s_logCalls) {
        std::string t = w.name;
        t += "(";
        for (int k = 0; k < nargs; ++k) {
            if (k) t += ", ";
            AppendValue(t, args[k]);
        }
        t += ")";
        if (w.result == R_INT) {
            t += " -> ";
            AppendValue(t, res);
        } else if (w.result == R_FLOAT) {
            float f;
            memcpy(&f, &res, 4);
            char b[32];
            _snprintf(b, sizeof(b), " -> %.1f", f);
            b[sizeof(b) - 1] = 0;
            t += b;
        }
        NoteCall(i, w.name, t.c_str());
    }
    VideoTick();
    return next;
}

// ---------------------------------------------------------------------------------------------------------------
// played videos (decode step and player update hooks, log=1)
// ---------------------------------------------------------------------------------------------------------------
struct Session {
    bool     active;
    void*    player;
    uint32_t key, hbink, frames;
    float    fps;
    double   first, last, maxGap;
    uint32_t firstFrame, lastFrame;
    uint32_t updates, shown, advanced, skipped, catchups, maxJump, restarts, gaps100, gatedDt, gatedDisplay;
    float    factorMin, factorMax;
};

enum { MAX_SESSIONS = 8 };
Session s_sessions[MAX_SESSIONS];

// One decode step of a played video: frame number before and after, time of the step, current time factor.
void SessionStep(Session& s, uint32_t before, uint32_t after, double now, float factor) {
    if (s.updates) {
        double gap = now - s.last;
        if (gap > s.maxGap) s.maxGap = gap;
        if (gap > 0.1) ++s.gaps100;
    }
    ++s.updates;
    if (after > before) {
        uint32_t d = after - before;
        ++s.shown;
        s.advanced += d;
        if (d > 1) {
            ++s.catchups;
            s.skipped += d - 1;
            if (d > s.maxJump) s.maxJump = d;
        }
    } else if (after < before) {
        ++s.restarts;
    }
    s.last = now;
    s.lastFrame = after;
    if (factor < s.factorMin) s.factorMin = factor;
    if (factor > s.factorMax) s.factorMax = factor;
}

void EndSession(Session& s, const char* why) {
    if (!s.active) return;
    s.active = false;
    double wall = s.last - s.first;
    double played = s.fps > 0.0f ? s.advanced / s.fps : 0.0;
    VidLog("end of video, Bink key %08X (%s): frames %u..%u of %u (file %.2f s at %.2f fps); %.2f s of video in %.2f s "
           "wall (x%.2f); %u updates, %u shown, %u skipped in %u catch-ups (largest jump %u), %u loops or gotos; "
           "longest gap between updates %.0f ms, %u gaps over 100 ms; updates blocked: %u by DT == 0, %u by the "
           "display flag; time factor %.3f..%.3f", s.key, why, s.firstFrame, s.lastFrame, s.frames,
           s.fps > 0.0f ? s.frames / s.fps : 0.0f, s.fps, played, wall, wall > 0.0 ? played / wall : 0.0, s.updates,
           s.shown, s.skipped, s.catchups, s.maxJump, s.restarts, s.maxGap * 1000.0, s.gaps100, s.gatedDt,
           s.gatedDisplay, s.factorMin, s.factorMax);
}

Session* FindSession(void* player) {
    for (int i = 0; i < MAX_SESSIONS; ++i)
        if (s_sessions[i].active && s_sessions[i].player == player) return &s_sessions[i];
    return NULL;
}

Session& SessionFor(void* player, uint32_t key, uint32_t hbink, uint32_t frame, double now) {
    Session* s = FindSession(player);
    if (s && (s->key != key || s->hbink != hbink)) {
        EndSession(*s, "the texture switched to another video");
        s = NULL;
    }
    if (s) return *s;
    int slot = 0;
    for (int i = 0; i < MAX_SESSIONS; ++i) {
        if (!s_sessions[i].active) {
            slot = i;
            break;
        }
        if (s_sessions[i].last < s_sessions[slot].last) slot = i;
    }
    EndSession(s_sessions[slot], "too many videos at once");
    Session& n = s_sessions[slot];
    memset(&n, 0, sizeof(n));
    n.active = true;
    n.player = player;
    n.key = key;
    n.hbink = hbink;
    n.frames = hbink ? U32((void*)(uintptr_t)hbink, 0x08) : 0;
    n.fps = F32(player, P_FPS);
    n.first = n.last = now;
    n.firstFrame = n.lastFrame = frame;
    n.factorMin = 1e9f;
    n.factorMax = -1e9f;
    VidLog("video starts, Bink key %08X: %u frames at %.2f fps (%.2f s), frame %u, time factor %.3f", key, n.frames,
           n.fps, n.fps > 0.0f ? n.frames / n.fps : 0.0f, frame, *s_ctx->timeFactor);
    return n;
}

void __fastcall DecodeHook(void* self, void*) {
    uint32_t hb = U32(self, P_HBINK);
    uint32_t before = hb ? U32((void*)(uintptr_t)hb, 0x0C) : 0;
    ((void (__fastcall*)(void*, void*))(uintptr_t)DECODE_STEP)(self, NULL);
    if (!s_logVideo) return;
    uint32_t hb2 = U32(self, P_HBINK);
    uint32_t after = hb2 ? U32((void*)(uintptr_t)hb2, 0x0C) : 0;
    double now = NowSec();
    Session& s = SessionFor(self, U32(self, P_KEY), hb2, before, now);
    SessionStep(s, hb2 == hb ? before : after, after, now, *s_ctx->timeFactor);
    VideoTick();
}

int __fastcall UpdateHook(void* self, void*) {
    uint8_t st = Byte(self, P_STATE);
    bool dtGate = *s_ctx->dt == 0.0f && U32(self, P_FLAG8) == 0;
    uint32_t disp = *s_ctx->displayCell;
    bool displayGate = disp && U32((void*)(uintptr_t)disp, D_BLOCKED) != 0;
    int r = ((int (__fastcall*)(void*, void*))(uintptr_t)PLAYER_UPDATE)(self, NULL);
    if (s_logVideo && (st & ST_RUNNING) && (dtGate || displayGate)) {
        Session* s = FindSession(self);
        if (s) {
            if (displayGate) ++s->gatedDisplay;
            else ++s->gatedDt;
        }
    }
    return r;
}

// ---------------------------------------------------------------------------------------------------------------
// verification and installation
// ---------------------------------------------------------------------------------------------------------------
struct Code { uint32_t va, len, crc; const char* what; };

const Code kEngineNatives[] = {
    { 0x0086D2A0, 0x050, 0x681A9BAB, "FUN_0086d2a0 texture key -> player" },
    { 0x0047B640, 0x014, 0x4B230BD5, "FUN_0047b640 running, not paused" },
    { 0x0047B6B0, 0x020, 0xF032EC5C, "FUN_0047b6b0 pause" },
    { 0x0047B6E0, 0x01F, 0x1AB1780F, "FUN_0047b6e0 length (frames/fps)" },
    { 0x0045CBD0, 0x018, 0x61C853DB, "player Stop (vtable +0x30)" },
    { 0x0045CC00, 0x039, 0x16DDD5BD, "player Goto (vtable +0x3C)" },
    { 0x0045CD10, 0x00E, 0x27F9C3F6, "player frame count (vtable +0x54)" },
    { 0x0086D370, 0x03F, 0x9FFAC73C, "FUN_0086d370 StopIfRunning (K3D+0x684B4)" },
};

const Code kEngineLog[] = {
    { 0x0045CC40, 0x09B, 0x9FA6BE71, "player decode step (vtable +0x40)" },
    { 0x0047B890, 0x087, 0x85CF129B, "player update (vtable +0x1C)" },
    { 0x006D5B80, 0x13B, 0x77868446, "TIM_UpdateAfterFrame (time factor)" },
};

struct Slot { uint32_t off, fn; bool log; };

const Slot kSlots[] = {
    { VT_STOP, 0x0045CBD0, false }, { VT_GOTO, 0x0045CC00, false }, { VT_FRAMES, 0x0045CD10, false },
    { VT_UPDATE, 0x0047B890, true }, { VT_DECODE, 0x0045CC40, true },
};

int VideoVerify(const Image& img, std::string& rep, bool natives, bool log) {
    int bad = 0;
    uint32_t got = 0;
    for (int i = 0; i < N_WORDS; ++i) {
        if (!Needed(i, natives, log)) continue;
        const Word& w = kWords[i];
        bool crcOk = CheckCrc(img, w.oldFn, w.len, w.crc, &got);
        bool regOk = w.key ? CheckRegisterScriptEntry(img, w.reg, w.oldFn, w.key) : CheckModuleEntry(img, w.reg, w.oldFn);
        char key[16];
        if (w.key) _snprintf(key, sizeof(key), "%08X", w.key);
        else strcpy(key, "(module)");
        key[sizeof(key) - 1] = 0;
        Report(rep, crcOk && regOk, "%-34s word %-8s handler %08X len 0x%03X crc %08X (want %08X), registered at %08X%s",
               w.name, key, w.oldFn, w.len, got, w.crc, w.reg, regOk ? "" : " (registration differs)");
        if (!(crcOk && regOk)) ++bad;
    }
    for (size_t pass = 0; pass < 2; ++pass) {
        if ((pass == 0 && !natives) || (pass == 1 && !log)) continue;
        const Code* list = pass == 0 ? kEngineNatives : kEngineLog;
        size_t n = pass == 0 ? sizeof(kEngineNatives) / sizeof(kEngineNatives[0]) : sizeof(kEngineLog) / sizeof(kEngineLog[0]);
        for (size_t i = 0; i < n; ++i) {
            bool ok = CheckCrc(img, list[i].va, list[i].len, list[i].crc, &got);
            Report(rep, ok, "%-34s %08X len 0x%03X crc %08X (want %08X)", list[i].what, list[i].va, list[i].len, got,
                   list[i].crc);
            if (!ok) ++bad;
        }
    }
    for (size_t i = 0; i < sizeof(kSlots) / sizeof(kSlots[0]); ++i) {
        if (kSlots[i].log ? !log : !natives) continue;
        uint32_t v = 0;
        bool ok = img.Read(BINK_VTABLE + kSlots[i].off, &v, 4) && v == kSlots[i].fn;
        Report(rep, ok, "Bink player vtable %08X +0x%02X = %08X (want %08X)", BINK_VTABLE, kSlots[i].off, v, kSlots[i].fn);
        if (!ok) ++bad;
    }
    return bad;
}

void InstallTable() {
    for (int i = 0; i < N_WORDS; ++i) {
        if (!Needed(i, s_natives, s_logCalls) || s_done[i]) continue;
        const Word& w = kWords[i];
        uint32_t fn = (uint32_t)(uintptr_t)Replacement(i);
        std::string notes;
        int r = w.key ? PatchNativeTable(w.key, 0xFFFFFFFF, w.oldFn, fn, notes)
                      : (PatchNativeHandler(w.oldFn, fn, notes) > 0 ? 1 : 0);
        size_t pos = 0;
        while (pos < notes.size()) {
            size_t end = notes.find('\n', pos);
            VidLog("%s: %s", w.name, notes.substr(pos, end == std::string::npos ? std::string::npos : end - pos).c_str());
            pos = end == std::string::npos ? notes.size() : end + 1;
        }
        s_done[i] = r == 1;
    }
}

int CountDone() {
    int n = 0;
    for (int i = 0; i < N_WORDS; ++i)
        if (Needed(i, s_natives, s_logCalls) && s_done[i]) ++n;
    return n;
}

int CountNeeded() {
    int n = 0;
    for (int i = 0; i < N_WORDS; ++i)
        if (Needed(i, s_natives, s_logCalls)) ++n;
    return n;
}

bool IsOn(const std::string& v) {
    return _stricmp(v.c_str(), "1") == 0 || _stricmp(v.c_str(), "true") == 0 || _stricmp(v.c_str(), "yes") == 0 ||
           _stricmp(v.c_str(), "on") == 0;
}

// ---------------------------------------------------------------------------------------------------------------
// self-test (wmtest video)
// ---------------------------------------------------------------------------------------------------------------
struct FakeVideo {
    uint32_t index, offset, data, ptrs;
    uint8_t  values[256];
    uint32_t entries[32];
    uint32_t display;
    float    factor, dt;
    uint8_t  players[2][0x60];
    uint32_t hbinks[2][8];
    void*    vtable[32];
    int      stops, gotoFrame;
};

FakeVideo* s_fk;
std::vector<uint8_t>* s_fkDisplay;

void* __cdecl FkFind(uint32_t key) {
    if (key == 0x1A000001) return s_fk->players[0];
    if (key == 0x1A000002) return s_fk->players[1];
    return NULL;
}
int __fastcall FkIsRunning(void* self, void*) { uint8_t st = Byte(self, P_STATE); return (st & 1) && !(st & 2); }
int __fastcall FkPause(void* self, void*) {
    uint8_t* st = (uint8_t*)self + P_STATE;
    if (!(*st & ST_RUNNING)) return 0;
    *st |= ST_PAUSED;
    return 1;
}
float __fastcall FkLength(void* self, void*) {
    uint32_t hb = U32(self, P_HBINK);
    return (hb ? (float)U32((void*)(uintptr_t)hb, 8) : 0.0f) / F32(self, P_FPS);
}
void __fastcall FkStop(void* self, void*, uint32_t) { *((uint8_t*)self + P_STATE) = 0; ++s_fk->stops; }
void __fastcall FkGoto(void*, void*, int32_t frame) { s_fk->gotoFrame = frame; }
void* __cdecl FkPop1(void* ip) { PopInts(1); return Next(ip); }
void* __cdecl FkPop2(void* ip) { PopInts(2); return Next(ip); }
void* __cdecl FkStopIf(void* ip) {
    uint32_t key = (uint32_t)Arg(1, 0);
    PopInts(1);
    PushInt(key == 0x1A000001 ? 1 : 0);
    return Next(ip);
}

struct CallResult { bool stackOk; int32_t i; float f; };

CallResult Call(NativeFn fn, int nargs, const int32_t* args, int results) {
    FakeVideo& f = *s_fk;
    f.index = 1;                                // one older entry below the arguments
    f.offset = 4;
    int32_t marker = 0x5EC0;
    memcpy(f.values, &marker, 4);
    f.entries[0] = (uint32_t)(uintptr_t)f.values;
    for (int i = 0; i < nargs; ++i) {
        memcpy(f.values + f.offset, &args[i], 4);
        f.entries[f.index++] = (uint32_t)(uintptr_t)(f.values + f.offset);
        f.offset += 4;
    }
    uint32_t word = 0;
    void* next = fn(&word);
    CallResult r;
    memset(&r, 0, sizeof(r));
    int32_t m = 0;
    memcpy(&m, f.values, 4);
    r.stackOk = next == (void*)((uint8_t*)&word + 4) && f.index == (uint32_t)(1 + results) &&
                f.offset == (uint32_t)(4 + 4 * results) && m == 0x5EC0 && f.entries[0] == (uint32_t)(uintptr_t)f.values;
    if (results) {
        memcpy(&r.i, f.values + 4, 4);
        memcpy(&r.f, f.values + 4, 4);
    }
    return r;
}

#define VID_EXPECT(cond, ...) do { bool ok_ = (cond); Report(rep, ok_, __VA_ARGS__); if (!ok_) ++fails; } while (0)

void SetPlayer(int n, uint8_t state, uint8_t opt, float fps, uint32_t binkKey, uint32_t frames) {
    uint8_t* p = s_fk->players[n];
    memset(p, 0, 0x60);
    void** vt = s_fk->vtable;
    memcpy(p, &vt, 4);
    p[P_OPT] = opt;
    p[P_STATE] = state;
    memcpy(p + P_FPS, &fps, 4);
    memcpy(p + P_KEY, &binkKey, 4);
    s_fk->hbinks[n][2] = frames;
    uint32_t hb = frames ? (uint32_t)(uintptr_t)s_fk->hbinks[n] : 0;
    memcpy(p + P_HBINK, &hb, 4);
}

int RunVideoSelfTest(std::string& rep) {
    int fails = 0;
    FakeVideo* f = new FakeVideo();
    memset(f, 0, sizeof(*f));
    std::vector<uint8_t> display(D_BGVIDEO + 0x100, 0);
    s_fk = f;
    s_fkDisplay = &display;
    f->data = (uint32_t)(uintptr_t)f->values;
    f->ptrs = (uint32_t)(uintptr_t)f->entries;
    f->factor = 1.0f;
    f->vtable[VT_STOP / 4] = (void*)FkStop;
    f->vtable[VT_GOTO / 4] = (void*)FkGoto;
    VideoCtx ctx = { &f->index, &f->offset, &f->data, &f->ptrs, &f->display, &f->factor, &f->dt,
                     FkFind, FkIsRunning, FkPause, FkLength };
    VideoCtx* oldCtx = s_ctx;
    bool oldNatives = s_natives, oldLogCalls = s_logCalls;
    uint32_t oldTrack = s_trackKey, oldPreKey = s_preloadKey;
    int32_t oldPreloading = s_preloading, oldFinished = s_preloadFinished;
    NativeFn oldOrig[N_WORDS];
    memcpy(oldOrig, s_orig, sizeof(oldOrig));
    s_ctx = &ctx;
    s_testLog = &rep;
    s_natives = true;
    s_logCalls = false;
    s_trackKey = 0;
    s_preloadKey = 0;
    s_preloading = 0;
    s_preloadFinished = 1;
    const int32_t none[1] = { 0 };

    // word table
    bool distinct = true;
    for (int i = 0; i < N_WORDS; ++i)
        for (int j = 0; j < i; ++j)
            if ((kWords[i].key && kWords[i].key == kWords[j].key) || kWords[i].reg == kWords[j].reg) distinct = false;
    bool shapes = true;
    for (int i = 0; i < N_WORDS; ++i)
        if ((kWords[i].kind == K_STUB) != (kWords[i].fn != NULL) || (kWords[i].kind != K_STUB && !kThunks[i])) shapes = false;
    VID_EXPECT(distinct && shapes, "word table: %d words, distinct keys and registration sites, 10 replacements and "
               "18 wrappers", (int)N_WORDS);

    // no current video
    CallResult r0 = Call(H_IsRunning, 0, none, 1), r1 = Call(H_PauseCurrent, 0, none, 1),
               r2 = Call(H_StopCurrent, 0, none, 1), r4 = Call(H_LengthCurrent, 0, none, 1);
    const int32_t frame7[1] = { 7 };
    CallResult r3 = Call(H_GotoCurrent, 1, frame7, 1);
    VID_EXPECT(r0.stackOk && r1.stackOk && r2.stackOk && r3.stackOk && r4.stackOk && r0.i == 0 && r1.i == 0 && r2.i == 0 &&
               r3.i == 0 && r4.f == 0.0f, "no current video: IsRunning %d, PauseCurrent %d, StopCurrent %d, "
               "GotoCurrent %d, LengthGetCurrent %.1f (want 0, same stack effect as the stubs)", r0.i, r1.i, r2.i,
               r3.i, r4.f);

    // K3D_BinkPlay(texture 1A000001) through its wrapper
    s_orig[W_PLAY] = FkPop1;
    SetPlayer(0, ST_RUNNING, 0, 30.0f, 0x32000011, 441);
    const int32_t texA[1] = { 0x1A000001 };
    CallResult rp = Call(kThunks[W_PLAY], 1, texA, 0);
    CallResult ri = Call(H_IsRunning, 0, none, 1);
    VID_EXPECT(rp.stackOk && s_trackKey == 0x1A000001 && ri.i == 1,
               "K3D_BinkPlay(1A000001) wrapper keeps the engine's stack effect and makes it current: IsRunning %d", ri.i);
    CallResult rl = Call(H_LengthCurrent, 0, none, 1);
    VID_EXPECT(rl.stackOk && fabs(rl.f - 14.7f) < 0.001f, "K3D_BinkLengthGetCurrentVideo -> %.3f (441 frames at 30 fps)",
               rl.f);
    CallResult rpa = Call(H_PauseCurrent, 0, none, 1);
    VID_EXPECT(rpa.i == 0 && !(s_fk->players[0][P_STATE] & ST_PAUSED),
               "PauseCurrentVideo without option bit 0 -> %d and no pause (Wii PauseCurrent)", rpa.i);
    s_fk->players[0][P_OPT] = OPT_STOP_AT_END;
    rpa = Call(H_PauseCurrent, 0, none, 1);
    CallResult rip = Call(H_IsRunning, 0, none, 1);
    VID_EXPECT(rpa.i == 1 && (s_fk->players[0][P_STATE] & ST_PAUSED) && rip.i == 0,
               "PauseCurrentVideo with option bit 0 -> %d, paused, IsRunning now %d (running and paused = 0)", rpa.i,
               rip.i);
    const int32_t frame12[1] = { 12 };
    CallResult rg = Call(H_GotoCurrent, 1, frame12, 1);
    VID_EXPECT(rg.stackOk && rg.i == 1 && s_fk->gotoFrame == 12, "GotoCurrentVideo(12) -> %d, player Goto got %d", rg.i,
               s_fk->gotoFrame);
    CallResult rs = Call(H_StopCurrent, 0, none, 1);
    CallResult ri2 = Call(H_IsRunning, 0, none, 1);
    VID_EXPECT(rs.i == 1 && s_fk->stops == 1 && s_trackKey == 0 && ri2.i == 0,
               "StopCurrentVideo -> %d, player Stop called %d time(s), no current video afterwards (IsRunning %d)",
               rs.i, s_fk->stops, ri2.i);

    // background video fallback and unloaded texture
    SetPlayer(1, ST_RUNNING, 0, 25.0f, 0x32000022, 100);
    f->display = (uint32_t)(uintptr_t)&display[0];
    uint32_t bg = (uint32_t)(uintptr_t)s_fk->players[1];
    memcpy(&display[D_BGVIDEO], &bg, 4);
    s_trackKey = 0x1A000099;                    // tracked texture no longer loaded
    CallResult rb = Call(H_IsRunning, 0, none, 1);
    CallResult rbl = Call(H_LengthCurrent, 0, none, 1);
    VID_EXPECT(rb.i == 1 && fabs(rbl.f - 4.0f) < 0.001f,
               "unloaded tracked texture: the running background video is current (IsRunning %d, length %.2f)", rb.i,
               rbl.f);

    // StopIfRunning wrapper clears the tracked video
    SetPlayer(0, ST_RUNNING, 0, 30.0f, 0x32000011, 441);
    s_orig[W_SWITCH] = FkPop2;
    const int32_t sw[2] = { 0x1A000001, 3 };
    CallResult rsw = Call(kThunks[W_SWITCH], 2, sw, 0);
    s_orig[W_STOP_IF_RUNNING] = FkStopIf;
    CallResult rsi = Call(kThunks[W_STOP_IF_RUNNING], 1, texA, 1);
    VID_EXPECT(rsw.stackOk && rsi.stackOk && rsi.i == 1 && s_trackKey == 0,
               "K3D_BinkSwitch(1A000001, 3) tracks the texture, K3D_BinkStopIfRunning -> %d clears it", rsi.i);

    // preload state (Wii statics)
    CallResult pf = Call(H_PreloadFinished, 0, none, 1), pp = Call(H_IsPreloading, 0, none, 1),
               pk = Call(H_PreloadedKey, 0, none, 1);
    VID_EXPECT(pf.i == 1 && pp.i == 0 && pk.i == 0, "initial preload state: finished %d, preloading %d, key %d "
               "(Wii .sdata/.sbss values)", pf.i, pp.i, pk.i);
    const int32_t key0[1] = { 0 }, keyB[1] = { 0x3200ABCD };
    CallResult p0 = Call(H_Preload, 1, key0, 1), p1 = Call(H_Preload, 1, keyB, 1), p2 = Call(H_Preload, 1, keyB, 1);
    pk = Call(H_PreloadedKey, 0, none, 1);
    pf = Call(H_PreloadFinished, 0, none, 1);
    VID_EXPECT(p0.stackOk && p1.stackOk && p0.i == 0 && p1.i == 1 && p2.i == 1 && (uint32_t)pk.i == 0x3200ABCD && pf.i == 1,
               "Preload(0) -> %d, Preload(3200ABCD) -> %d, again -> %d, key %08X, finished %d", p0.i, p1.i, p2.i,
               (uint32_t)pk.i, pf.i);
    memset(&display[D_BGVIDEO], 0, 4);
    s_fk->players[0][P_STATE] = 0;
    CallResult pc = Call(H_CancelPreload, 0, none, 0);
    pk = Call(H_PreloadedKey, 0, none, 1);
    VID_EXPECT(pc.stackOk && (uint32_t)pk.i == 0x3200ABCD, "CancelPreload with no current video is ignored (Wii "
               "FinishPreload), key %08X", (uint32_t)pk.i);
    s_trackKey = 0x1A000001;
    s_fk->players[0][P_STATE] = ST_RUNNING;
    pc = Call(H_CancelPreload, 0, none, 0);
    pk = Call(H_PreloadedKey, 0, none, 1);
    VID_EXPECT(pk.i == 0, "CancelPreload while a video is current clears the preloaded key (%d)", pk.i);

    // played video accounting
    Session s;
    memset(&s, 0, sizeof(s));
    s.active = true;
    s.fps = 30.0f;
    s.factorMin = 1e9f;
    s.factorMax = -1e9f;
    double t = 0.0;
    uint32_t fr = 1;
    for (int i = 0; i < 30; ++i) {
        SessionStep(s, fr, fr + 1, t, 1.0f);
        ++fr;
        t += 1.0 / 30.0;
    }
    t += 0.25;                                  // a 250 ms stall, then an 8-frame jump
    SessionStep(s, fr, fr + 8, t, 1.0f);
    fr += 8;
    SessionStep(s, fr, 1, t + 0.033, 0.5f);     // loop
    VID_EXPECT(s.updates == 32 && s.shown == 31 && s.advanced == 38 && s.skipped == 7 && s.catchups == 1 &&
               s.maxJump == 8 && s.gaps100 == 1 && s.restarts == 1 && fabs(s.maxGap - (0.25 + 1.0 / 30.0)) < 1e-6 &&
               s.factorMin == 0.5f && s.factorMax == 1.0f,
               "decode accounting: %u updates, %u shown, %u advanced, %u skipped in %u catch-up (jump %u), %u gap "
               "over 100 ms, %u loop, factor %.1f..%.1f", s.updates, s.shown, s.advanced, s.skipped, s.catchups,
               s.maxJump, s.gaps100, s.restarts, s.factorMin, s.factorMax);

    // call log: first call, then changes only
    s_logCalls = true;
    unsigned oldCount = s_noteCount[W_IS_RUNNING];
    std::string oldNote = s_lastNote[W_IS_RUNNING];
    s_noteCount[W_IS_RUNNING] = 0;
    s_lastNote[W_IS_RUNNING].clear();
    size_t lines0 = rep.size();
    Call(H_IsRunning, 0, none, 1);
    Call(H_IsRunning, 0, none, 1);
    Call(H_IsRunning, 0, none, 1);
    s_fk->players[0][P_STATE] = 0;
    Call(H_IsRunning, 0, none, 1);
    unsigned logged = s_noteCount[W_IS_RUNNING];
    bool firstMarked = rep.find("first call: K3D_BinkIsRunning() -> 1", lines0) != std::string::npos;
    VID_EXPECT(logged == 2 && firstMarked, "call log: 4 calls with one change -> %u lines, first call marked", logged);
    s_noteCount[W_IS_RUNNING] = oldCount;
    s_lastNote[W_IS_RUNNING] = oldNote;

    s_ctx = oldCtx;
    s_testLog = NULL;
    s_natives = oldNatives;
    s_logCalls = oldLogCalls;
    s_trackKey = oldTrack;
    s_preloadKey = oldPreKey;
    s_preloading = oldPreloading;
    s_preloadFinished = oldFinished;
    memcpy(s_orig, oldOrig, sizeof(oldOrig));
    s_fk = NULL;
    s_fkDisplay = NULL;
    delete f;
    return fails;
}

void CopyOut(const std::string& rep, char* out, int outSize) {
    if (!out || outSize <= 0) return;
    size_t n = rep.size() < (size_t)outSize - 1 ? rep.size() : (size_t)outSize - 1;
    memcpy(out, rep.c_str(), n);
    out[n] = 0;
}

}  // namespace

// ---------------------------------------------------------------------------------------------------------------
// entry points
// ---------------------------------------------------------------------------------------------------------------
void VideoAttach(const std::string& iniPath) {
    s_inAttach = true;
    std::string v;
    bool natives = true, log = false, generalLog = false;
    if (ReadIniKey(iniPath, "video", "natives", v)) natives = IsOn(v);
    if (ReadIniKey(iniPath, "video", "log", v)) log = IsOn(v);
    if (ReadIniKey(iniPath, "general", "log", v)) generalLog = IsOn(v);
    log = log && generalLog;
    if (!natives && !log) {
        VidLog("[video] natives=0 log=0: the video natives are left untouched");
        s_inAttach = false;
        return;
    }
    ProcessImage img;
    uint8_t probe[5];
    if (!img.Read(kWords[0].oldFn, probe, 5)) {
        VidLog("host process is not the RGH PC executable (no code at %08X): video natives not installed", kWords[0].oldFn);
        s_inAttach = false;
        return;
    }
    std::string rep;
    int bad = VideoVerify(img, rep, natives, log);
    if (bad) {
        VidLog("the PC executable differs from the expected 2010 build in %d place(s): nothing installed", bad);
        size_t pos = 0;
        while (pos < rep.size()) {
            size_t end = rep.find('\n', pos);
            std::string line = rep.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
            if (line.find("FAIL") != std::string::npos) VidLog("%s", line.c_str());
            pos = end == std::string::npos ? rep.size() : end + 1;
        }
        s_inAttach = false;
        return;
    }
    s_natives = natives;
    s_logCalls = log;
    int reg = 0;
    for (int i = 0; i < N_WORDS; ++i) {
        if (!Needed(i, natives, log)) continue;
        s_orig[i] = (NativeFn)(uintptr_t)kWords[i].oldFn;
        uint32_t fn = (uint32_t)(uintptr_t)Replacement(i);
        if (WriteCode(kWords[i].reg, &fn, 4)) ++reg;    // in case the registration runs (again) later
    }
    int hooks = 0;
    if (log) {
        uint32_t d = (uint32_t)(uintptr_t)&DecodeHook, u = (uint32_t)(uintptr_t)&UpdateHook;
        if (WriteCode(BINK_VTABLE + VT_DECODE, &d, 4)) ++hooks;
        if (WriteCode(BINK_VTABLE + VT_UPDATE, &u, 4)) ++hooks;
        s_logVideo = hooks == 2;
    }
    s_verified = true;
    InstallTable();
    VidLog("PC executable verified: natives=%d log=%d; %d/%d words in the native table (%d registration entries), "
           "%d/%d player hooks", natives, log, CountDone(), CountNeeded(), reg, log ? 2 : 0, hooks);
    s_inAttach = false;
}

void VideoAfterConfig() {
    for (size_t i = 0; i < s_pending.size(); ++i) Log("VIDEO: %s", s_pending[i].c_str());
    s_pending.clear();
    if (s_verified && CountDone() < CountNeeded()) {
        InstallTable();
        Log("VIDEO: %d/%d words in the native table after start-up", CountDone(), CountNeeded());
    }
}

void VideoTick() {
    if (!s_logVideo) return;
    double now = NowSec();
    for (int i = 0; i < MAX_SESSIONS; ++i)
        if (s_sessions[i].active && now - s_sessions[i].last > 1.0) EndSession(s_sessions[i], "no update for 1 s");
}

extern "C" {

// wmtest only: verify every video patch site against an exe file. Returns the number of differences (-1: unreadable).
int __cdecl WiimoteVideoCheckExe(const char* exePath, char* out, int outSize) {
    FileImage img;
    std::string rep;
    int bad = -1;
    if (!img.Load(exePath)) rep = "  FAIL  cannot read " + std::string(exePath ? exePath : "(null)") + "\n";
    else bad = VideoVerify(img, rep, true, true);
    CopyOut(rep, out, outSize);
    return bad;
}

// wmtest only: the video handlers and the video accounting on a fake engine. Returns the number of failures.
int __cdecl WiimoteVideoSelfTest(char* out, int outSize) {
    std::string rep;
    int fails = RunVideoSelfTest(rep);
    CopyOut(rep, out, outSize);
    return fails;
}

}  // extern "C"
