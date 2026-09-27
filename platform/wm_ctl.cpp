// wm_ctl.cpp - controls mode ([controls] mode=pc|wii in wiimote.ini; pc is the default).
//
// wii (default)  The virtual Wii remote + Nunchuk (wm_input.cpp, wm_proto.cpp) feeds the engine's WPAD client, so every
//                controller read of the scripts sees a Wii remote driven by keyboard, mouse and pad.
// pc             PC-native controls.  The virtual remote is not started: no WPAD server, so no button, stick, shake or
//                tilt input comes from this DLL and the engine registers no Wii remote slot.  The scripts read keys
//                and mouse buttons with the PC natives (IO_Keyboard*, IO_MouseButton*).  The Wii-only pointer and
//                motion natives, which would read an absent remote, are replaced per word in the script native table:
//                the pointer is the engine's own mouse position (the value IO_MousePosGet returns) in KPAD units, the
//                motion reads are the remote the Options bindings shake and tilt (wm_motion.cpp; at rest, held level,
//                until the first frame).  Magma menus take their clicks from the mouse buttons by themselves while
//                the cursor's controller slot is empty (FUN_007FD0A0).
#include "wiimote.h"

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <vector>

using namespace wmpatch;

namespace {

typedef void* (__cdecl* NativeFn)(void* ip);

// ---------------------------------------------------------------------------------------------------------------
// engine state read by the handlers (the PC executable's addresses; the self-test points them at fakes)
// ---------------------------------------------------------------------------------------------------------------
struct CtlCtx {
    uint32_t* vmIndex;      // 0x00A7E188 script stack: number of entries
    uint32_t* vmOffset;     // 0x00A7E18C script stack: bytes used in the value area
    uint32_t* vmData;       // 0x00A7E198 script stack: value area
    uint32_t* vmPtrs;       // 0x00A7E19C script stack: entry pointers
    int32_t*  ioMouse;      // 0x00A90F28 IO_Mouse: 1 from the engine's mouse init (FUN_006CC1D0), IO_MouseEnable(0/1)
    int32_t*  cursor;       // 0x00A90F18 {x, y}: cursor relative to the client area (Mouse_On_Game, every frame)
    int32_t*  viewport;     // 0x00A90EC8 {height, width, top, left} of the 3D viewport (K3D::AssignViewport)
};

CtlCtx s_game = {
    (uint32_t*)0x00A7E188, (uint32_t*)0x00A7E18C, (uint32_t*)0x00A7E198, (uint32_t*)0x00A7E19C,
    (int32_t*)0x00A90F28, (int32_t*)0x00A90F18, (int32_t*)0x00A90EC8,
};
CtlCtx* s_ctx = &s_game;

bool         s_pcMode;
bool         s_inAttach;                // DllMain: log lines wait until the configuration is loaded
bool         s_verified;
std::vector<std::string> s_pending;
std::string* s_testLog;                 // self-test: collect log lines here
float        s_lastX, s_lastY;          // KPAD keeps the last position while the pointer is not valid
LONG         s_seen[10];                // "first call" log per handler

void CtlLog(const char* fmt, ...) {
    char buf[600];
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
        Log("CTL: %s", buf);
    }
}

// ---------------------------------------------------------------------------------------------------------------
// script VM protocol (engine handlers 005EDD90 & co): arguments are popped last first, one result is pushed
// ---------------------------------------------------------------------------------------------------------------
inline uint32_t* Ptrs() { return (uint32_t*)(uintptr_t)*s_ctx->vmPtrs; }

// argument i (0 = first) of a call that passed n int arguments, read before they are popped
int32_t Arg(int n, int i) { return *(int32_t*)(uintptr_t)Ptrs()[*s_ctx->vmIndex - n + i]; }

void PopInts(int n) {
    *s_ctx->vmIndex -= n;
    *s_ctx->vmOffset -= 4 * n;
}

uint8_t* PushEntry(uint32_t size) {
    uint8_t* at = (uint8_t*)(uintptr_t)(*s_ctx->vmData + *s_ctx->vmOffset);
    Ptrs()[*s_ctx->vmIndex] = (uint32_t)(uintptr_t)at;
    *s_ctx->vmIndex += 1;
    *s_ctx->vmOffset += size;
    return at;
}

void PushInt(int32_t v) { memcpy(PushEntry(4), &v, 4); }

void PushVec(float x, float y, float z) {
    float v[3] = { x, y, z };
    memcpy(PushEntry(12), v, 12);
}

// ---------------------------------------------------------------------------------------------------------------
// pointer from the engine's mouse state
// ---------------------------------------------------------------------------------------------------------------
struct Pointer { float x, y; bool valid; };

// Same position as FUN_006CC490 (IO_MousePosGet): (cursor - viewport origin) / viewport size, inside when both lie in
// [0, 1], nothing while IO_Mouse is 0.  KPAD units: (-1, -1) top-left .. (1, 1) bottom-right (the Wii scripts'
// Bunnies_Pointer_Pos_Convert turns p into the Magma cursor position (p + 1) / 2).
Pointer ReadPointer() {
    const CtlCtx& c = *s_ctx;
    Pointer p = { s_lastX, s_lastY, false };
    if (!ScriptPointerOn(0)) return p;          // the scripts switched the pointer off (wm_script.cpp)
    int32_t h = c.viewport[0], w = c.viewport[1];
    if (*c.ioMouse == 0 || w <= 0 || h <= 0) return p;
    float x = (float)(c.cursor[0] - c.viewport[3]) / (float)w;
    float y = (float)(c.cursor[1] - c.viewport[2]) / (float)h;
    if (!(x >= 0.0f && x <= 1.0f && y >= 0.0f && y <= 1.0f)) return p;
    p.x = s_lastX = x * 2.0f - 1.0f;
    p.y = s_lastY = y * 2.0f - 1.0f;
    p.valid = true;
    return p;
}

float Dist() {                                  // [pointer] dist, clamped like LoadConfig
    float d = g_cfg.dist;
    if (!(d >= 0.8f)) d = 0.8f;
    if (d > 2.9f) d = 2.9f;
    return d;
}

void FirstCall(int h, const char* name, int32_t id) {
    if (InterlockedExchange(&s_seen[h], 1) == 0) CtlLog("first call: %s(%d)", name, id);
}

// ---------------------------------------------------------------------------------------------------------------
// handlers (the stack effect of the engine handlers they replace; every controller id reads the mouse)
// ---------------------------------------------------------------------------------------------------------------
void* __cdecl H_Pointer(void* ip) {             // IO_JoystickPointerGet / IO_JoystickGetPointer(id) -> (x, y, dist)
    FirstCall(0, "IO_JoystickPointerGet", Arg(1, 0));
    PopInts(1);
    Pointer p = ReadPointer();
    PushVec(p.x, p.y, Dist());
    return (uint8_t*)ip + 4;
}

void* __cdecl H_PointerState(void* ip) {        // IO_JoystickPointerStateGet / IO_JoystickGetPointerState(id)
    FirstCall(1, "IO_JoystickPointerStateGet", Arg(1, 0));
    PopInts(1);
    PushInt(ReadPointer().valid ? 1 : -2);      // FUN_006F5710: 1 valid, -2 no valid pointer
    return (uint8_t*)ip + 4;
}

void* __cdecl H_DpdValid(void* ip) {            // IO_JoystickDPDValidGet(id): dpd_valid_fg, 2 = both dots seen
    FirstCall(2, "IO_JoystickDPDValidGet", Arg(1, 0));
    PopInts(1);
    PushInt(ReadPointer().valid ? 2 : 0);
    return (uint8_t*)ip + 4;
}

void* __cdecl H_Horizon(void* ip) {             // IO_JoystickHorizonGet(id) -> (horizon.x, horizon.y, dist): the roll
    FirstCall(3, "IO_JoystickHorizonGet", Arg(1, 0));
    PopInts(1);
    float h[2];
    MotionHorizon(h);
    PushVec(h[0], h[1], Dist());
    return (uint8_t*)ip + 4;
}

void* __cdecl H_Accel(void* ip) {               // IO_JoystickAccelGet(id, sensor): 0 remote, 1 Nunchuk; the newest sample
    FirstCall(4, "IO_JoystickAccelGet", Arg(2, 0));
    int32_t sensor = Arg(2, 1);
    PopInts(2);
    float v[3];
    MotionAccel(sensor, 0, v);
    PushVec(v[0], v[1], v[2]);
    return (uint8_t*)ip + 4;
}

void* __cdecl H_AccelSample(void* ip) {         // IO_JoystickAccelSampleGet(id, sample, sensor) (FUN_006ECFB0)
    FirstCall(5, "IO_JoystickAccelSampleGet", Arg(3, 0));
    MotionPolled();
    int32_t sample = Arg(3, 1), sensor = Arg(3, 2);
    PopInts(3);
    float v[3];
    MotionAccel(sensor, sample, v);
    PushVec(v[0], v[1], v[2]);
    return (uint8_t*)ip + 4;
}

void* __cdecl H_PointerSample(void* ip) {       // IO_JoystickPointerSampleGet(id, sample): the one sample = pointer
    FirstCall(6, "IO_JoystickPointerSampleGet", Arg(2, 0));
    PopInts(2);
    Pointer p = ReadPointer();
    PushVec(p.x, p.y, Dist());
    return (uint8_t*)ip + 4;
}

void* __cdecl H_PointerStateSample(void* ip) {  // IO_JoystickPointerStateSampleGet(id, sample)
    FirstCall(7, "IO_JoystickPointerStateSampleGet", Arg(2, 0));
    PopInts(2);
    PushInt(ReadPointer().valid ? 1 : -2);
    return (uint8_t*)ip + 4;
}

void* __cdecl H_SampleNumber(void* ip) {        // IO_JoystickSampleNumberGet(id): the frame's accelerometer samples
    FirstCall(8, "IO_JoystickSampleNumberGet", Arg(1, 0));
    MotionPolled();
    PopInts(1);
    PushInt(MotionSampleCount());
    return (uint8_t*)ip + 4;
}

// IO_JoystickStickGet(id, stick) -> (x, y, 0): controller 0's Nunchuk stick from the movement actions for the
// Wii-only scripts (wm_motion.cpp MotionStick); any other read is the engine's
const uint32_t kStickGetEngine = 0x005ED7C0;
NativeFn s_stickEngine = (NativeFn)(uintptr_t)kStickGetEngine;       // the self-test points it at a stand-in

void* __cdecl H_Stick(void* ip) {
    int32_t id = Arg(2, 0), stick = Arg(2, 1);
    float v[2];
    if (id == 0 && stick == 0 && MotionStick(v)) {
        FirstCall(9, "IO_JoystickStickGet", id);
        PopInts(2);
        PushVec(v[0], v[1], 0.0f);
        return (uint8_t*)ip + 4;
    }
    return s_stickEngine(ip);
}

// ---------------------------------------------------------------------------------------------------------------
// the PC executable (2010 build): what is verified and what is replaced
// ---------------------------------------------------------------------------------------------------------------
struct CtlWord {
    uint32_t key;           // native table key (ViD::RegisterScript 005428E0)
    uint32_t oldFn, len, crc;
    uint32_t reg;           // imm32 of "mov [esp+x], handler" in ViD::RegisterScript
    NativeFn fn;
    const char* name;
};

const CtlWord kCtlWords[] = {
    { 0x0098FFFF, 0x005EDE50, 0x086, 0xEE00C0BA, 0x0054308C, H_Pointer, "IO_JoystickPointerGet" },
    { 0x0085FFFF, 0x005EDEE0, 0x005, 0x0F7F04A3, 0x00542E1C, H_Pointer, "IO_JoystickGetPointer" },
    { 0x0099FFFF, 0x005EDF60, 0x06A, 0xDD837B4E, 0x005430C0, H_PointerState, "IO_JoystickPointerStateGet" },
    { 0x0086FFFF, 0x005EE040, 0x005, 0x6440A43F, 0x00542E50, H_PointerState, "IO_JoystickGetPointerState" },
    { 0x00B2FFFF, 0x005EDEF0, 0x06A, 0x50BE37E4, 0x00543467, H_DpdValid, "IO_JoystickDPDValidGet" },
    { 0x00A6FFFF, 0x005EE050, 0x086, 0x02E07239, 0x00543397, H_Horizon, "IO_JoystickHorizonGet" },
    { 0x0097FFFF, 0x005EDD90, 0x0A3, 0x5100C93C, 0x00543058, H_Accel, "IO_JoystickAccelGet" },
    { 0x009DFFFF, 0x005EE330, 0x0B9, 0xA9116F5C, 0x005431B5, H_AccelSample, "IO_JoystickAccelSampleGet" },
    { 0x009EFFFF, 0x005EE3F0, 0x0A3, 0x3EBB4B06, 0x005431F7, H_PointerSample, "IO_JoystickPointerSampleGet" },
    { 0x009FFFFF, 0x005EE4A0, 0x07F, 0x0CEEC2C7, 0x0054322B, H_PointerStateSample, "IO_JoystickPointerStateSampleGet" },
    { 0x009BFFFF, 0x005EE200, 0x068, 0x6D4D322E, 0x0054315C, H_SampleNumber, "IO_JoystickSampleNumberGet" },
    { 0x008CFFFF, kStickGetEngine, 0x0A3, 0x07BF1146, 0x00542F20, H_Stick, "IO_JoystickStickGet" },
};
const size_t N_WORDS = sizeof(kCtlWords) / sizeof(kCtlWords[0]);
bool s_wordDone[sizeof(kCtlWords) / sizeof(kCtlWords[0])];

struct CtlCode { uint32_t va, len, crc; const char* what; };

// engine code the pc mode relies on (globals read by the handlers, the mouse update, the Magma mouse path)
const CtlCode kCtlEngine[] = {
    { 0x006CC490, 0x0AA, 0xE3C40F96, "FUN_006cc490 mouse position" },
    { 0x006CC2A0, 0x192, 0x3295EE74, "Mouse_On_Game (per-frame mouse)" },
    { 0x006CC1D0, 0x0A9, 0x2B81E791, "FUN_006cc1d0 mouse init" },
    { 0x006CC600, 0x023, 0x61E535D0, "FUN_006cc600 mouse just pressed" },
    { 0x006CC630, 0x023, 0x77896A48, "FUN_006cc630 mouse just released" },
    { 0x006EC950, 0x038, 0x7F8BA936, "FUN_006ec950 controller present" },
    { 0x007FD0A0, 0x223, 0xF3D7BF44, "FUN_007fd0a0 Magma cursor buttons" },
    { 0x005EB9C0, 0x085, 0xDB5D687B, "ViD_MenuGameCursorMove handler" },
};

int CtlVerify(const Image& img, std::string& rep) {
    int bad = 0;
    uint32_t got = 0;
    for (size_t i = 0; i < N_WORDS; ++i) {
        const CtlWord& w = kCtlWords[i];
        bool crcOk = CheckCrc(img, w.oldFn, w.len, w.crc, &got);
        bool regOk = CheckRegisterScriptEntry(img, w.reg, w.oldFn, w.key);
        Report(rep, crcOk && regOk, "%-34s word %08X handler %08X len 0x%03X crc %08X (want %08X), registered at %08X%s",
               w.name, w.key, w.oldFn, w.len, got, w.crc, w.reg, regOk ? "" : " (registration differs)");
        if (!(crcOk && regOk)) ++bad;
    }
    for (size_t i = 0; i < sizeof(kCtlEngine) / sizeof(kCtlEngine[0]); ++i) {
        const CtlCode& e = kCtlEngine[i];
        bool ok = CheckCrc(img, e.va, e.len, e.crc, &got);
        Report(rep, ok, "%-34s %08X len 0x%03X crc %08X (want %08X)", e.what, e.va, e.len, got, e.crc);
        if (!ok) ++bad;
    }
    return bad;
}

// replace the handler of every word in the native table (built before the DLL loads); returns the words done
int InstallWords() {
    int done = 0;
    for (size_t i = 0; i < N_WORDS; ++i) {
        const CtlWord& w = kCtlWords[i];
        if (!s_wordDone[i]) {
            std::string notes;
            int r = PatchNativeTable(w.key, 0xFFFFFFFF, w.oldFn, (uint32_t)(uintptr_t)w.fn, notes);
            size_t pos = 0;
            while (pos < notes.size()) {
                size_t end = notes.find('\n', pos);
                CtlLog("%s: %s", w.name, notes.substr(pos, end == std::string::npos ? std::string::npos : end - pos).c_str());
                pos = end == std::string::npos ? notes.size() : end + 1;
            }
            s_wordDone[i] = r == 1;
        }
        if (s_wordDone[i]) ++done;
    }
    return done;
}

void AttachPc() {
    ProcessImage img;
    uint8_t probe[5];
    if (!img.Read(kCtlWords[0].oldFn, probe, 5)) {
        CtlLog("mode=pc: host process is not the RGH PC executable (no code at %08X): pointer natives not installed",
               kCtlWords[0].oldFn);
        return;
    }
    std::string rep;
    int bad = CtlVerify(img, rep);
    if (bad) {
        CtlLog("mode=pc: the PC executable differs from the expected 2010 build in %d place(s): pointer/motion natives "
               "NOT installed (the virtual Wii remote stays off)", bad);
        size_t pos = 0;
        while (pos < rep.size()) {
            size_t end = rep.find('\n', pos);
            std::string line = rep.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
            if (line.find("FAIL") != std::string::npos) CtlLog("%s", line.c_str());
            pos = end == std::string::npos ? rep.size() : end + 1;
        }
        return;
    }
    int reg = 0;
    for (size_t i = 0; i < N_WORDS; ++i) {       // in case ViD::RegisterScript runs (again) later
        uint32_t v = (uint32_t)(uintptr_t)kCtlWords[i].fn;
        if (WriteCode(kCtlWords[i].reg, &v, 4)) ++reg;
    }
    s_verified = true;
    int done = InstallWords();
    CtlLog("mode=pc: PC executable verified; %d/%d pointer/motion natives replaced in the native table, %d/%d "
           "registration entries; the virtual Wii remote is off", done, (int)N_WORDS, reg, (int)N_WORDS);
}

// ---------------------------------------------------------------------------------------------------------------
// self-test (wmtest ctl): the handlers on a fake script stack and fake mouse globals
// ---------------------------------------------------------------------------------------------------------------
struct FakeCtl {
    uint32_t index, offset, data, ptrs;
    uint8_t  values[256];
    uint32_t entries[32];
    int32_t  ioMouse;
    int32_t  cursor[2];
    int32_t  viewport[4];
};

struct CallResult { bool stackOk; int32_t i; float v[3]; };

// push "below" older int entries, then the arguments; run the handler; check it popped exactly the arguments
CallResult CallNative(FakeCtl& f, NativeFn fn, int nargs, const int32_t* args, uint32_t resultSize, int below = 0) {
    f.index = 0;
    f.offset = 0;
    for (int i = 0; i < below + nargs; ++i) {
        int32_t v = i < below ? 0x5EC0 + i : args[i - below];
        memcpy(f.values + f.offset, &v, 4);
        f.entries[f.index++] = (uint32_t)(uintptr_t)(f.values + f.offset);
        f.offset += 4;
    }
    uint32_t word = 0;
    void* next = fn(&word);
    CallResult r;
    memset(&r, 0, sizeof(r));
    uint32_t base = 4 * (uint32_t)below;
    r.stackOk = next == (void*)((uint8_t*)&word + 4) && f.index == (uint32_t)below + 1 &&
                f.offset == base + resultSize && f.entries[below] == (uint32_t)(uintptr_t)(f.values + base);
    for (int i = 0; i < below; ++i) {
        int32_t v = 0;
        memcpy(&v, f.values + 4 * i, 4);
        if (f.entries[i] != (uint32_t)(uintptr_t)(f.values + 4 * i) || v != 0x5EC0 + i) r.stackOk = false;
    }
    if (resultSize == 4) memcpy(&r.i, f.values + base, 4);
    else memcpy(r.v, f.values + base, 12);
    return r;
}

inline bool Near(float a, float b) { return a - b < 1e-4f && b - a < 1e-4f; }

// stands in for the engine's IO_JoystickStickGet handler: pops (id, stick), pushes (9, 9, 9)
void* __cdecl TestStickEngine(void* ip) {
    PopInts(2);
    PushVec(9.0f, 9.0f, 9.0f);
    return (uint8_t*)ip + 4;
}

#define CTL_EXPECT(cond, ...) do { bool ok_ = (cond); Report(rep, ok_, __VA_ARGS__); if (!ok_) ++fails; } while (0)

int RunCtlSelfTest(std::string& rep) {
    FakeCtl* f = new FakeCtl();
    memset(f, 0, sizeof(*f));
    f->data = (uint32_t)(uintptr_t)f->values;
    f->ptrs = (uint32_t)(uintptr_t)f->entries;
    CtlCtx ctx = { &f->index, &f->offset, &f->data, &f->ptrs, &f->ioMouse, f->cursor, f->viewport };
    CtlCtx* oldCtx = s_ctx;
    std::string* oldLog = s_testLog;
    float oldX = s_lastX, oldY = s_lastY;
    s_ctx = &ctx;
    s_testLog = &rep;
    s_lastX = s_lastY = 0.0f;
    memset(s_seen, 0, sizeof(s_seen));
    int fails = 0;
    float dist = Dist();
    CallResult r;
    const int32_t id0[1] = { 0 }, id20[1] = { 20 };

    CTL_EXPECT(dist >= 0.8f && dist <= 2.9f, "pointer distance from [pointer] dist: %.2f", dist);
    bool distinct = true;
    for (size_t i = 0; i < N_WORDS; ++i)
        for (size_t j = 0; j < i; ++j)
            if (kCtlWords[i].key == kCtlWords[j].key || kCtlWords[i].reg == kCtlWords[j].reg) distinct = false;
    CTL_EXPECT(distinct && kCtlWords[0].fn == kCtlWords[1].fn && kCtlWords[2].fn == kCtlWords[3].fn,
               "word table: %d distinct words and registrations, GetPointer/GetPointerState share the handlers",
               (int)N_WORDS);

    f->ioMouse = 1;
    f->viewport[0] = 720;
    f->viewport[1] = 1280;
    f->cursor[0] = 640;
    f->cursor[1] = 360;
    r = CallNative(*f, H_Pointer, 1, id0, 12);
    CTL_EXPECT(r.stackOk && Near(r.v[0], 0.0f) && Near(r.v[1], 0.0f) && Near(r.v[2], dist),
               "IO_JoystickPointerGet(0), cursor at the centre of a 1280x720 viewport -> (%.4f, %.4f, %.2f)", r.v[0],
               r.v[1], r.v[2]);
    r = CallNative(*f, H_DpdValid, 1, id0, 4);
    CTL_EXPECT(r.stackOk && r.i == 2, "IO_JoystickDPDValidGet(0) inside -> %d (want 2)", r.i);
    r = CallNative(*f, H_PointerState, 1, id0, 4);
    CTL_EXPECT(r.stackOk && r.i == 1, "IO_JoystickPointerStateGet(0) inside -> %d (want 1)", r.i);

    f->cursor[0] = 0;
    f->cursor[1] = 0;
    r = CallNative(*f, H_Pointer, 1, id0, 12);
    CTL_EXPECT(r.stackOk && Near(r.v[0], -1.0f) && Near(r.v[1], -1.0f), "top-left corner -> (%.4f, %.4f) (want -1, -1)",
               r.v[0], r.v[1]);
    f->cursor[0] = 1280;
    f->cursor[1] = 720;
    r = CallNative(*f, H_Pointer, 1, id0, 12, 1);
    CTL_EXPECT(r.stackOk && Near(r.v[0], 1.0f) && Near(r.v[1], 1.0f),
               "bottom-right corner, one older stack entry kept -> (%.4f, %.4f) (want 1, 1)", r.v[0], r.v[1]);

    f->viewport[1] = 960;                       // letterboxed: 960x720 at x 160
    f->viewport[3] = 160;
    f->cursor[0] = 160 + 720;
    f->cursor[1] = 180;
    r = CallNative(*f, H_Pointer, 1, id20, 12);
    CTL_EXPECT(r.stackOk && Near(r.v[0], 0.5f) && Near(r.v[1], -0.5f),
               "IO_JoystickPointerGet(20), 960x720 viewport at x 160 -> (%.4f, %.4f) (want 0.5, -0.5)", r.v[0], r.v[1]);

    f->cursor[0] = 100;                         // left of the viewport
    r = CallNative(*f, H_DpdValid, 1, id0, 4);
    CTL_EXPECT(r.stackOk && r.i == 0, "outside the viewport: IO_JoystickDPDValidGet -> %d (want 0)", r.i);
    r = CallNative(*f, H_PointerState, 1, id0, 4);
    CTL_EXPECT(r.stackOk && r.i == -2, "outside the viewport: IO_JoystickPointerStateGet -> %d (want -2)", r.i);
    r = CallNative(*f, H_Pointer, 1, id0, 12);
    CTL_EXPECT(r.stackOk && Near(r.v[0], 0.5f) && Near(r.v[1], -0.5f),
               "outside the viewport: the last position is kept -> (%.4f, %.4f)", r.v[0], r.v[1]);

    f->cursor[0] = 160 + 240;
    f->cursor[1] = 540;
    const int32_t ps[2] = { 0, 5 };
    r = CallNative(*f, H_PointerSample, 2, ps, 12);
    CTL_EXPECT(r.stackOk && Near(r.v[0], -0.5f) && Near(r.v[1], 0.5f) && Near(r.v[2], dist),
               "IO_JoystickPointerSampleGet(0, 5) -> (%.4f, %.4f, %.2f) (want -0.5, 0.5)", r.v[0], r.v[1], r.v[2]);
    r = CallNative(*f, H_PointerStateSample, 2, ps, 4, 1);
    CTL_EXPECT(r.stackOk && r.i == 1, "IO_JoystickPointerStateSampleGet(0, 5) -> %d (want 1)", r.i);
    r = CallNative(*f, H_SampleNumber, 1, id0, 4);
    CTL_EXPECT(r.stackOk && r.i == 1, "IO_JoystickSampleNumberGet(0) -> %d (want 1)", r.i);

    f->ioMouse = 0;                             // IO_MouseEnable(0)
    r = CallNative(*f, H_DpdValid, 1, id0, 4);
    CallResult s = CallNative(*f, H_PointerState, 1, id0, 4);
    CTL_EXPECT(r.stackOk && s.stackOk && r.i == 0 && s.i == -2, "IO_Mouse 0: DPDValid %d, PointerState %d (want 0, -2)",
               r.i, s.i);
    f->ioMouse = 1;
    f->viewport[0] = 0;
    f->viewport[1] = 0;
    r = CallNative(*f, H_DpdValid, 1, id0, 4);
    CTL_EXPECT(r.stackOk && r.i == 0, "empty viewport: DPDValid %d (want 0, no division by zero)", r.i);

    r = CallNative(*f, H_Horizon, 1, id0, 12);
    CTL_EXPECT(r.stackOk && Near(r.v[0], 1.0f) && Near(r.v[1], 0.0f) && Near(r.v[2], dist),
               "IO_JoystickHorizonGet(0) -> (%.4f, %.4f, %.2f) (want 1, 0, dist)", r.v[0], r.v[1], r.v[2]);
    const int32_t a0[2] = { 0, 0 }, a1[2] = { 20, 1 }, a2[2] = { 0, 2 };
    r = CallNative(*f, H_Accel, 2, a0, 12);
    CallResult r1 = CallNative(*f, H_Accel, 2, a1, 12);
    CallResult r2 = CallNative(*f, H_Accel, 2, a2, 12, 1);
    CTL_EXPECT(r.stackOk && r1.stackOk && r2.stackOk && Near(r.v[1], -1.0f) && Near(r.v[0], 0.0f) &&
               Near(r.v[2], 0.0f) && Near(r1.v[1], -1.0f) && Near(r2.v[0], 0.0f) && Near(r2.v[1], 0.0f) &&
               Near(r2.v[2], 0.0f),
               "IO_JoystickAccelGet: sensor 0 (%.1f, %.1f, %.1f), sensor 1 y %.1f, sensor 2 y %.1f (want rest (0, -1, 0), "
               "sensor 2 zero)", r.v[0], r.v[1], r.v[2], r1.v[1], r2.v[1]);
    const int32_t as0[3] = { 0, 15, 1 }, as2[3] = { 0, 3, 2 };
    r = CallNative(*f, H_AccelSample, 3, as0, 12);
    r1 = CallNative(*f, H_AccelSample, 3, as2, 12, 1);
    CTL_EXPECT(r.stackOk && r1.stackOk && Near(r.v[1], -1.0f) && Near(r1.v[1], 0.0f),
               "IO_JoystickAccelSampleGet(0, 15, 1) y %.1f, (0, 3, 2) y %.1f (want -1, 0)", r.v[1], r1.v[1]);

    // IO_JoystickStickGet: the movement actions for controller 0's stick 0 while MotionStick says so, else the engine
    NativeFn oldStick = s_stickEngine;
    s_stickEngine = TestStickEngine;
    MotionInput mi;
    memset(&mi, 0, sizeof(mi));
    mi.on = true;
    mi.noPad = true;
    mi.move[0] = -1.0f;
    mi.t = 500.0;
    MotionFrame(mi);
    const int32_t st00[2] = { 0, 0 }, st01[2] = { 0, 1 }, st10[2] = { 10, 0 };
    r = CallNative(*f, H_Stick, 2, st00, 12);               // no accelerometer read yet: the engine's
    CallNative(*f, H_SampleNumber, 1, id0, 4);              // a Wii-only script polls the remote
    mi.t += 1.0 / 60.0;
    MotionFrame(mi);
    r1 = CallNative(*f, H_Stick, 2, st00, 12, 1);
    r2 = CallNative(*f, H_Stick, 2, st01, 12);
    CallResult r3 = CallNative(*f, H_Stick, 2, st10, 12);
    CTL_EXPECT(r.stackOk && Near(r.v[0], 9.0f) && r1.stackOk && Near(r1.v[0], -1.0f) && Near(r1.v[1], 0.0f) &&
               Near(r1.v[2], 0.0f) && r2.stackOk && Near(r2.v[0], 9.0f) && r3.stackOk && Near(r3.v[0], 9.0f),
               "IO_JoystickStickGet(0, 0): the engine's before a script reads the accelerometers (%.0f), then MOVE LEFT "
               "held = (%.1f, %.1f, %.1f); stick 1 and controller 10 stay the engine's (%.0f, %.0f)", r.v[0], r1.v[0],
               r1.v[1], r1.v[2], r2.v[0], r3.v[0]);
    s_stickEngine = oldStick;
    memset(&mi, 0, sizeof(mi));
    mi.t = 501.0;
    MotionFrame(mi);

    // native table replacement (the in-game path) on a fake TOOsarray {data, element size, capacity, count}
    uint32_t elems[4 * 3] = {
        0x0098FFFF, 0x005EDE50, 0,              // expected engine handler: replaced
        0x0085FFFF, 0x12345678, 0,              // another handler: left alone
        0x1E3C0003, 0x005ECA50, 0,              // save-layer style key (word << 16, low word ignored)
        0x0097FFFF, 0x005EDD90, 0,              // not asked for: untouched
    };
    uint32_t table[4] = { (uint32_t)(uintptr_t)elems, 12, 4, 4 };
    uint32_t tva = (uint32_t)(uintptr_t)table;
    std::string notes;
    int t1 = PatchNativeTableAt(tva, 0x0098FFFF, 0xFFFFFFFF, 0x005EDE50, 0xAABBCCDD, notes);
    int t2 = PatchNativeTableAt(tva, 0x0098FFFF, 0xFFFFFFFF, 0x005EDE50, 0xAABBCCDD, notes);
    int t3 = PatchNativeTableAt(tva, 0x0085FFFF, 0xFFFFFFFF, 0x005EDEE0, 0xAABBCCDD, notes);
    int t4 = PatchNativeTableAt(tva, 0x1E3C0000, 0xFFFF0000, 0x005ECA50, 0xAABBCC00, notes);
    int t5 = PatchNativeTableAt(tva, 0x00A6FFFF, 0xFFFFFFFF, 0x005EE050, 0xAABBCCDD, notes);
    CTL_EXPECT(t1 == 1 && t2 == 1 && t3 == -1 && t4 == 1 && t5 == 0 && elems[1] == 0xAABBCCDD &&
               elems[4] == 0x12345678 && elems[7] == 0xAABBCC00 && elems[10] == 0x005EDD90 && elems[0] == 0x0098FFFF,
               "native table on a fake table: replace %d, again %d, other handler %d (kept), word mask %d, absent %d "
               "(want 1 1 -1 1 0)", t1, t2, t3, t4, t5);

    s_ctx = oldCtx;
    s_testLog = oldLog;
    s_lastX = oldX;
    s_lastY = oldY;
    memset(s_seen, 0, sizeof(s_seen));
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
void CtlAttach(const std::string& iniPath) {
    s_inAttach = true;
    std::string mode;
    s_pcMode = true;                                   // the default: PC-native controls (mode=wii for the virtual remote)
    if (ReadIniKey(iniPath, "controls", "mode", mode)) {
        if (_stricmp(mode.c_str(), "wii") == 0) s_pcMode = false;
        else if (_stricmp(mode.c_str(), "pc") != 0) CtlLog("[controls] mode=%s is neither wii nor pc: pc", mode.c_str());
    }
    if (s_pcMode) AttachPc();
    s_inAttach = false;
}

void CtlAfterConfig() {
    for (size_t i = 0; i < s_pending.size(); ++i) Log("CTL: %s", s_pending[i].c_str());
    s_pending.clear();
    if (!s_pcMode) return;
    if (s_verified) {
        int before = 0;
        for (size_t i = 0; i < N_WORDS; ++i) before += s_wordDone[i] ? 1 : 0;
        int done = before < (int)N_WORDS ? InstallWords() : before;
        if (done != before) Log("CTL: %d/%d pointer/motion natives replaced after start-up", done, (int)N_WORDS);
    }
    Log("CTL: controls mode=pc: no virtual Wii remote (WPAD server not started; [keys] and [xinput] unused), pointer "
        "natives follow the mouse, motion natives the Options bindings (wm_motion.cpp), dist %.2f", Dist());
}

bool CtlPcMode() { return s_pcMode; }

extern "C" {

// wmtest only: verify the pc mode patch sites against an exe file.  Returns the number of differences (-1: unreadable).
int __cdecl WiimoteCtlCheckExe(const char* exePath, char* out, int outSize) {
    FileImage img;
    std::string rep;
    int bad = -1;
    if (!img.Load(exePath)) rep = "  FAIL  cannot read " + std::string(exePath ? exePath : "(null)") + "\n";
    else bad = CtlVerify(img, rep);
    CopyOut(rep, out, outSize);
    return bad;
}

// wmtest only: the pointer/motion handlers on a fake engine.  Returns the number of failures.
int __cdecl WiimoteCtlSelfTest(char* out, int outSize) {
    WmEnsureInit();                             // [pointer] dist
    std::string rep;
    int fails = RunCtlSelfTest(rep);
    CopyOut(rep, out, outSize);
    return fails;
}

}  // extern "C"
