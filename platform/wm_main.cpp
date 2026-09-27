// wm_main.cpp - DllMain, configuration, log and the exports of wiimote.dll.
#include "wiimote.h"

#include <cctype>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <share.h>

Config      g_cfg;
std::string g_dllDir;

const char* const g_btnNames[WB_COUNT] = {
    "A", "B", "Z", "C", "1", "2", "PLUS", "MINUS", "HOME",
    "DUP", "DDOWN", "DLEFT", "DRIGHT",
    "STICK_UP", "STICK_DOWN", "STICK_LEFT", "STICK_RIGHT",
    "SHAKE", "TILT_LEFT", "TILT_RIGHT", "POINTER_CENTER", "CONNECT_TOGGLE",
};

namespace {

std::string      s_iniPath;
bool             s_testMode;
CRITICAL_SECTION s_logCs;
CRITICAL_SECTION s_gearCs;
FILE*            s_logFile;
bool             s_logFailed;
INIT_ONCE        s_once = INIT_ONCE_STATIC_INIT;

// ---------------------------------------------------------------------------------------------------------------
// ini helpers
// ---------------------------------------------------------------------------------------------------------------
const char* const kUnset = "\x01";

std::string Trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t' || s[a] == '\r' || s[a] == '\n')) ++a;
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r' || s[b - 1] == '\n')) --b;
    return s.substr(a, b - a);
}

std::string Upper(std::string s) {
    for (size_t i = 0; i < s.size(); ++i) s[i] = (char)toupper((unsigned char)s[i]);
    return s;
}

std::string IniRaw(const char* sec, const char* key, bool& present) {
    char buf[1024];
    GetPrivateProfileStringA(sec, key, kUnset, buf, sizeof(buf), s_iniPath.c_str());
    if (strcmp(buf, kUnset) == 0) {
        present = false;
        return std::string();
    }
    present = true;
    std::string s(buf);
    for (size_t i = 0; i < s.size(); ++i) {       // inline comments: "value ; comment"
        if ((s[i] == ';' || s[i] == '#') && (i == 0 || s[i - 1] == ' ' || s[i - 1] == '\t')) {
            s.resize(i);
            break;
        }
    }
    return Trim(s);
}

std::string IniStr(const char* sec, const char* key, const char* def) {
    bool present;
    std::string s = IniRaw(sec, key, present);
    return present ? s : std::string(def);
}

int IniInt(const char* sec, const char* key, int def) {
    bool present;
    std::string s = IniRaw(sec, key, present);
    if (!present || s.empty()) return def;
    return (int)strtol(s.c_str(), NULL, 0);
}

float IniFloat(const char* sec, const char* key, float def) {
    bool present;
    std::string s = IniRaw(sec, key, present);
    if (!present || s.empty()) return def;
    return (float)atof(s.c_str());
}

bool IniBool(const char* sec, const char* key, bool def) {
    bool present;
    std::string s = Upper(IniRaw(sec, key, present));
    if (!present || s.empty()) return def;
    return s == "1" || s == "TRUE" || s == "YES" || s == "ON";
}

struct NameCode { const char* name; int code; };

const NameCode kKeyNames[] = {
    {"LBUTTON", 0x01}, {"RBUTTON", 0x02}, {"MBUTTON", 0x04}, {"XBUTTON1", 0x05}, {"XBUTTON2", 0x06},
    {"BACK", 0x08}, {"BACKSPACE", 0x08}, {"TAB", 0x09}, {"RETURN", 0x0D}, {"ENTER", 0x0D},
    {"SHIFT", 0x10}, {"CONTROL", 0x11}, {"CTRL", 0x11}, {"MENU", 0x12}, {"ALT", 0x12}, {"PAUSE", 0x13},
    {"CAPITAL", 0x14}, {"CAPSLOCK", 0x14}, {"ESCAPE", 0x1B}, {"ESC", 0x1B}, {"SPACE", 0x20},
    {"PRIOR", 0x21}, {"PAGEUP", 0x21}, {"NEXT", 0x22}, {"PAGEDOWN", 0x22}, {"END", 0x23}, {"HOME", 0x24},
    {"LEFT", 0x25}, {"UP", 0x26}, {"RIGHT", 0x27}, {"DOWN", 0x28}, {"INSERT", 0x2D}, {"DELETE", 0x2E},
    {"MULTIPLY", 0x6A}, {"ADD", 0x6B}, {"SUBTRACT", 0x6D}, {"DECIMAL", 0x6E}, {"DIVIDE", 0x6F},
    {"LSHIFT", 0xA0}, {"RSHIFT", 0xA1}, {"LCONTROL", 0xA2}, {"LCTRL", 0xA2}, {"RCONTROL", 0xA3},
    {"RCTRL", 0xA3}, {"LMENU", 0xA4}, {"LALT", 0xA4}, {"RMENU", 0xA5}, {"RALT", 0xA5},
    {"OEM_1", 0xBA}, {"OEM_PLUS", 0xBB}, {"OEM_COMMA", 0xBC}, {"OEM_MINUS", 0xBD}, {"OEM_PERIOD", 0xBE},
    {"OEM_2", 0xBF}, {"OEM_3", 0xC0}, {"OEM_4", 0xDB}, {"OEM_5", 0xDC}, {"OEM_6", 0xDD}, {"OEM_7", 0xDE},
};

const NameCode kPadNames[] = {
    {"DUP", 0x0001}, {"DDOWN", 0x0002}, {"DLEFT", 0x0004}, {"DRIGHT", 0x0008},
    {"START", 0x0010}, {"BACK", 0x0020}, {"LS", 0x0040}, {"RS", 0x0080},
    {"LB", 0x0100}, {"RB", 0x0200}, {"A", 0x1000}, {"B", 0x2000}, {"X", 0x4000}, {"Y", 0x8000},
    {"LT", PAD_LT}, {"RT", PAD_RT},
};

int ParseKeyName(const std::string& tok) {
    std::string t = Upper(Trim(tok));
    if (t.empty()) return 0;
    if (t.size() > 2 && t[0] == '0' && t[1] == 'X') return (int)(strtol(t.c_str(), NULL, 16) & 0xFF);
    if (t.size() == 1 && ((t[0] >= 'A' && t[0] <= 'Z') || (t[0] >= '0' && t[0] <= '9'))) return t[0];
    if (t[0] == 'F' && t.size() <= 3 && isdigit((unsigned char)t[1])) {
        int n = atoi(t.c_str() + 1);
        if (n >= 1 && n <= 24) return 0x6F + n;
    }
    if (t.size() == 7 && t.compare(0, 6, "NUMPAD") == 0 && isdigit((unsigned char)t[6])) return 0x60 + (t[6] - '0');
    for (size_t i = 0; i < sizeof(kKeyNames) / sizeof(kKeyNames[0]); ++i)
        if (t == kKeyNames[i].name) return kKeyNames[i].code;
    return -1;
}

int ParsePadName(const std::string& tok) {
    std::string t = Upper(Trim(tok));
    if (t.empty()) return 0;
    for (size_t i = 0; i < sizeof(kPadNames) / sizeof(kPadNames[0]); ++i)
        if (t == kPadNames[i].name) return kPadNames[i].code;
    return -1;
}

void ParseBindList(const std::string& list, int* out, bool pad, const char* where) {
    for (int i = 0; i < MAX_BIND; ++i) out[i] = 0;
    int n = 0;
    size_t start = 0;
    while (start <= list.size()) {
        size_t comma = list.find(',', start);
        std::string tok = list.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
        if (!Trim(tok).empty()) {
            int code = pad ? ParsePadName(tok) : ParseKeyName(tok);
            if (code < 0) Log("config: unknown %s name '%s' in %s", pad ? "pad" : "key", Trim(tok).c_str(), where);
            else if (code && n < MAX_BIND) out[n++] = code;
        }
        if (comma == std::string::npos) break;
        start = comma + 1;
    }
}

const char* const kDefKeys[WB_COUNT] = {
    "SPACE,RETURN,LBUTTON",   // A
    "LSHIFT,RBUTTON",         // B
    "LCONTROL",               // Z
    "C",                      // C
    "1",                      // 1
    "2",                      // 2
    "TAB",                    // PLUS
    "ESCAPE",                 // MINUS
    "H",                      // HOME
    "I,NUMPAD8",              // D-pad up
    "K,NUMPAD2",              // D-pad down
    "J,NUMPAD4",              // D-pad left
    "L,NUMPAD6",              // D-pad right
    "W,UP",                   // Nunchuk stick
    "S,DOWN",
    "A,LEFT",
    "D,RIGHT",
    "F,MBUTTON",              // shake
    "Q",                      // tilt (roll) left
    "E",                      // tilt (roll) right
    "",                       // pointer to the centre
    "",                       // connect / disconnect the remote
};

const char* const kDefPad[WB_COUNT] = {
    "A", "B,RT", "LT", "LB", "RB", "Y", "START", "BACK", "RS",
    "DUP", "DDOWN", "DLEFT", "DRIGHT",
    "", "", "", "",           // the left stick drives the Nunchuk stick
    "X",                      // shake
    "", "",
    "LS",                     // pointer to the centre
    "",
};

}  // namespace

void LoadConfig(const std::string& iniPath, Config& c) {
    s_iniPath = iniPath;
    c = Config();
    c.enabled        = IniBool("general", "enabled", c.enabled);
    c.log            = IniBool("general", "log", c.log);
    c.logVerbose     = IniBool("general", "log_verbose", c.logVerbose);
    c.channel        = IniInt("general", "channel", c.channel);
    c.nunchuk        = IniBool("general", "nunchuk", c.nunchuk);
    c.port           = IniInt("general", "port", c.port);
    c.rateHz         = IniInt("general", "rate_hz", c.rateHz);
    c.requireFocus   = IniBool("general", "require_focus", c.requireFocus);
    c.gearPad        = IniBool("general", "gear_pad", c.gearPad);
    c.rumble         = IniBool("general", "rumble", c.rumble);
    c.connectAtStart = IniBool("general", "connect_at_start", c.connectAtStart);
    if (c.channel < 0 || c.channel > 3) c.channel = 0;
    if (c.rateHz < 50) c.rateHz = 50;
    if (c.rateHz > 1000) c.rateHz = 1000;

    c.mouse     = IniBool("pointer", "mouse", c.mouse);
    c.padStick  = IniBool("pointer", "pad_stick", c.padStick);
    c.padSpeed  = IniFloat("pointer", "pad_speed", c.padSpeed);
    c.dist      = IniFloat("pointer", "dist", c.dist);
    c.sensorBar = IniInt("pointer", "sensor_bar", c.sensorBar) ? 1 : 0;
    std::string center = Upper(IniStr("pointer", "kpad_center_y", "auto"));
    if (center.empty() || center == "AUTO") {
        c.centerAuto = true;
    } else {
        c.centerAuto = false;
        c.centerY = (float)atof(center.c_str());
    }
    if (c.dist < 0.8f) c.dist = 0.8f;       // KPAD accepts 0.52..3.0 m; the game's pointer state wants 0.7..5.0
    if (c.dist > 2.9f) c.dist = 2.9f;

    c.shakeG       = IniFloat("motion", "shake_g", c.shakeG);
    c.shakeHz      = IniFloat("motion", "shake_hz", c.shakeHz);
    c.tiltDeg      = IniFloat("motion", "tilt_deg", c.tiltDeg);
    c.tiltSpeedDeg = IniFloat("motion", "tilt_speed_deg", c.tiltSpeedDeg);
    std::string target = Upper(IniStr("motion", "shake_target", "remote"));
    c.shakeTarget = target == "NUNCHUK" ? 1 : (target == "BOTH" ? 2 : 0);

    c.savEnabled          = IniBool("save", "enabled", c.savEnabled);
    c.savChannelInstalled = IniBool("save", "channel_installed", c.savChannelInstalled);

    c.xinput   = IniBool("xinput", "enabled", c.xinput);
    c.padIndex = IniInt("xinput", "pad", c.padIndex);
    c.deadzone = IniFloat("xinput", "deadzone", c.deadzone);
    if (c.deadzone < 0.0f) c.deadzone = 0.0f;
    if (c.deadzone > 0.9f) c.deadzone = 0.9f;

    for (int b = 0; b < WB_COUNT; ++b) {
        std::string where = std::string("[keys] ") + g_btnNames[b];
        ParseBindList(IniStr("keys", g_btnNames[b], kDefKeys[b]), c.keys[b], false, where.c_str());
        where = std::string("[xinput] ") + g_btnNames[b];
        ParseBindList(IniStr("xinput", g_btnNames[b], kDefPad[b]), c.pad[b], true, where.c_str());
    }
}

double NowSeconds() {
    static LARGE_INTEGER freq;
    if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart / (double)freq.QuadPart;
}

void LogOpen() {
    if (s_logFile || s_logFailed || !g_cfg.log) return;
    std::string path = g_dllDir + "wiimote_log.txt";
    s_logFile = _fsopen(path.c_str(), "w", _SH_DENYWR);
    if (!s_logFile) s_logFailed = true;
}

void Log(const char* fmt, ...) {
    if (!g_cfg.log) return;
    EnterCriticalSection(&s_logCs);
    LogOpen();
    if (s_logFile) {
        SYSTEMTIME t;
        GetLocalTime(&t);
        fprintf(s_logFile, "%02u:%02u:%02u.%03u ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
        va_list ap;
        va_start(ap, fmt);
        vfprintf(s_logFile, fmt, ap);
        va_end(ap);
        fputc('\n', s_logFile);
        fflush(s_logFile);
    }
    LeaveCriticalSection(&s_logCs);
}

// ---------------------------------------------------------------------------------------------------------------
// start-up
// ---------------------------------------------------------------------------------------------------------------
namespace {

BOOL CALLBACK InitOnceProc(PINIT_ONCE, PVOID, PVOID*) {
    LoadConfig(s_iniPath, g_cfg);
    bool iniFound = GetFileAttributesA(s_iniPath.c_str()) != INVALID_FILE_ATTRIBUTES;
    Log("wiimote.dll (built " __DATE__ " " __TIME__ ") - virtual Wii remote and Wii save layer for the RGH PC executable");
    Log("ini %s (%s)", s_iniPath.c_str(), iniFound ? "found" : "missing, using defaults");
    Log("enabled=%d channel=%d (script id %d) nunchuk=%d port=%d rate=%d Hz gear_pad=%d require_focus=%d "
        "xinput=%d mouse=%d dist=%.2f sensor_bar=%d centre=%s", g_cfg.enabled, g_cfg.channel, 20 + g_cfg.channel,
        g_cfg.nunchuk, g_cfg.port, g_cfg.rateHz, g_cfg.gearPad, g_cfg.requireFocus, g_cfg.xinput, g_cfg.mouse,
        g_cfg.dist, g_cfg.sensorBar, g_cfg.centerAuto ? "auto" : "fixed");
    SavAfterConfig();
    CtlAfterConfig();
    VideoAfterConfig();
    AfxAfterConfig();
    TimingAfterConfig();
    AudioAfterConfig();
    ScriptAfterConfig();
    OptionsAfterConfig();
    FixesAfterConfig();
    ModsAfterConfig();
    LuaAfterConfig();
    Sm64AfterConfig();
    if (CtlPcMode() || !g_cfg.enabled) return TRUE;     // [controls] mode=pc: no virtual remote, no input polling
    PcSourceInit();
    if (s_testMode) {
        WmTestInput zero;
        memset(&zero, 0, sizeof(zero));
        PcSourceInject(&zero);
        Log("test mode (WIIMOTE_TEST): input comes from WiimoteTestInject");
    }
    ServerStart();
    return TRUE;
}

DWORD WINAPI StartThreadProc(LPVOID) {
    InitOnceExecuteOnce(&s_once, InitOnceProc, NULL, NULL);
    return 0;
}

}  // namespace

void WmEnsureInit() {
    InitOnceExecuteOnce(&s_once, InitOnceProc, NULL, NULL);
}

static void EnsureInit() {
    WmEnsureInit();
}

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(module);
        InitializeCriticalSection(&s_logCs);
        InitializeCriticalSection(&s_gearCs);
        char path[MAX_PATH];
        DWORD n = GetModuleFileNameA(module, path, MAX_PATH);
        std::string p(path, n);
        size_t slash = p.find_last_of("\\/");
        g_dllDir = slash == std::string::npos ? std::string() : p.substr(0, slash + 1);
        char env[MAX_PATH];
        DWORD e = GetEnvironmentVariableA("WIIMOTE_INI", env, MAX_PATH);
        s_iniPath = (e > 0 && e < MAX_PATH) ? std::string(env) : g_dllDir + "wiimote.ini";
        s_testMode = GetEnvironmentVariableA("WIIMOTE_TEST", env, MAX_PATH) > 0;
        // Wii save natives: verify the PC executable and redirect the SAV handlers now, before any script runs.
        SavAttach(s_iniPath);
        // [controls] mode=pc: the Wii pointer/motion natives are replaced now as well (no virtual remote is started).
        CtlAttach(s_iniPath);
        // [video] natives/log and [timing] fps_cap: verified per-word natives, player hooks, the frame pacer.
        VideoAttach(s_iniPath);
        // [video] afx: the level after effects (colour grade, glow) the PC executable never draws.
        AfxAttach(s_iniPath);
        TimingAttach(s_iniPath);
        // [audio] doppler: sound source velocities measured over a Wii frame (no Doppler flicker uncapped).
        AudioAttach(s_iniPath);
        // [script]: ViD_PlatformCurrentGet (follows [controls] mode, so after CtlAttach), IO_JoystickPointerStateSet,
        // WII_ReturnToMenu answered like the Wii executable.
        ScriptAttach(s_iniPath);
        // [options]: the Options page of the pause menu, control bindings (mode=pc, so after CtlAttach), volumes.
        OptionsAttach(s_iniPath);
        // always on: widescreen (the picture fills the window) and no controller rescans every 2 s.
        FixesAttach();
        // [mods]: the enabled mods' files (options.ini [mods], the launcher's list) served in place of the archive's.
        ModsAttach(s_iniPath);
        // [mods] lua: the enabled mods' main.lua, run from the first frame on.
        LuaAttach(s_iniPath);
        Sm64Attach(s_iniPath);
        // The I/O thread runs inside this DLL until the process ends: never let FreeLibrary unmap it.
        HMODULE pinned;
        GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_PIN | GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                           (LPCSTR)&DllMain, &pinned);
        // The engine calls WrapWPADRegisterAllocator / WrapKPADInit long before its KPAD connects to the WPAD
        // server; start anyway from a thread in case those calls are skipped (DirectInput8Create failure).
        HANDLE t = CreateThread(NULL, 0, StartThreadProc, NULL, 0, NULL);
        if (t) CloseHandle(t);
    }
    return TRUE;
}

// ---------------------------------------------------------------------------------------------------------------
// Gear::Input::GamePadWii side (Wrap* exports).  KPADStatus of this KPAD version is 0xF0 bytes (MotionPlus era).
// ---------------------------------------------------------------------------------------------------------------
namespace {

const int KPAD_STATUS_SIZE = 0xF0;

struct GearChan {
    bool     have;
    uint32_t seq;
    uint32_t hold, trig, release;
    WmVec3   acc, accPrev;
    WmVec2   pos, vec;
    WmVec2   hori, horiVec;
    float    dist, distVec;
    WmVec3   fsAcc, fsAccPrev;
    float    params[4][2];                 // pos, dist, acc, hori: play radius, sensitivity
    bool     aiming;
    bool     mplsOn[5];                    // zero play, zero drift, dir revise, acc revise, dpd revise
    float    zeroPlay, zeroDrift[3], dirRevise, accRevise[2], dpdRevise;
};

GearChan s_gear[4];

inline void PutF(uint8_t* b, int off, float v) { memcpy(b + off, &v, 4); }
inline void PutU(uint8_t* b, int off, uint32_t v) { memcpy(b + off, &v, 4); }
inline float Len3(const WmVec3& v) { return (float)sqrt(v.x * v.x + v.y * v.y + v.z * v.z); }

int FillKpadStatus(int chan, uint8_t* b) {
    WmState st;
    uint32_t seq = 0;
    memset(b, 0, KPAD_STATUS_SIZE);
    if (!ServerLatest(st, &seq) || !st.connected) {
        b[0x5C] = WPAD_DEV_NOT_FOUND;
        b[0x5D] = 0xFF;                    // WPAD_ERR_NO_CONTROLLER
        return 0;
    }
    EnterCriticalSection(&s_gearCs);
    GearChan& g = s_gear[chan];
    if (!g.have || g.seq != seq) {         // derive trig/release/speeds once per new sample, not once per call
        uint32_t hold = st.buttons;
        g.trig = hold & ~g.hold;
        g.release = g.hold & ~hold;
        g.hold = hold;
        g.accPrev = g.have ? g.acc : st.acc;
        g.acc = st.acc;
        g.fsAccPrev = g.have ? g.fsAcc : st.fsAcc;
        g.fsAcc = st.fsAcc;
        if (st.pointerValid) {
            WmVec2 hori = { (float)cos(st.roll), (float)sin(st.roll) };
            g.vec = g.have ? WmVec2{ st.pointer.x - g.pos.x, st.pointer.y - g.pos.y } : WmVec2{ 0, 0 };
            g.horiVec = g.have ? WmVec2{ hori.x - g.hori.x, hori.y - g.hori.y } : WmVec2{ 0, 0 };
            g.distVec = g.have ? st.dist - g.dist : 0.0f;
            g.pos = st.pointer;
            g.hori = hori;
            g.dist = st.dist;
        } else {
            g.vec = WmVec2{ 0, 0 };
            g.horiVec = WmVec2{ 0, 0 };
            g.distVec = 0.0f;
            if (!g.have) {
                g.hori = WmVec2{ 1, 0 };
                g.dist = st.dist;
            }
        }
        g.seq = seq;
        g.have = true;
    }
    PutU(b, 0x00, g.hold);
    PutU(b, 0x04, g.trig);
    PutU(b, 0x08, g.release);
    PutF(b, 0x0C, g.acc.x);
    PutF(b, 0x10, g.acc.y);
    PutF(b, 0x14, g.acc.z);
    PutF(b, 0x18, Len3(g.acc));
    WmVec3 da = { g.acc.x - g.accPrev.x, g.acc.y - g.accPrev.y, g.acc.z - g.accPrev.z };
    PutF(b, 0x1C, Len3(da));
    PutF(b, 0x20, g.pos.x);
    PutF(b, 0x24, g.pos.y);
    PutF(b, 0x28, g.vec.x);
    PutF(b, 0x2C, g.vec.y);
    PutF(b, 0x30, (float)sqrt(g.vec.x * g.vec.x + g.vec.y * g.vec.y));
    PutF(b, 0x34, g.hori.x);
    PutF(b, 0x38, g.hori.y);
    PutF(b, 0x3C, g.horiVec.x);
    PutF(b, 0x40, g.horiVec.y);
    PutF(b, 0x44, (float)sqrt(g.horiVec.x * g.horiVec.x + g.horiVec.y * g.horiVec.y));
    PutF(b, 0x48, g.dist);
    PutF(b, 0x4C, g.distVec);
    PutF(b, 0x50, (float)fabs(g.distVec));
    float n = Len3(g.acc);
    if (n > 0.0f) {
        PutF(b, 0x54, (float)sqrt(g.acc.x * g.acc.x + g.acc.y * g.acc.y) / n);   // acc_vertical (0x007F0820)
        PutF(b, 0x58, -g.acc.z / n);
    }
    b[0x5C] = st.nunchuk ? WPAD_DEV_FREESTYLE : WPAD_DEV_CORE;
    b[0x5D] = 0;                                                              // WPAD_ERR_NONE
    b[0x5E] = st.pointerValid ? 2 : 0;                                        // dpd_valid_fg
    b[0x5F] = st.nunchuk ? WPAD_FMT_FREESTYLE_ACC_DPD : WPAD_FMT_CORE_ACC_DPD;
    if (st.nunchuk) {
        PutF(b, 0x60, st.stick.x);
        PutF(b, 0x64, st.stick.y);
        PutF(b, 0x68, g.fsAcc.x);
        PutF(b, 0x6C, g.fsAcc.y);
        PutF(b, 0x70, g.fsAcc.z);
        PutF(b, 0x74, Len3(g.fsAcc));
        WmVec3 df = { g.fsAcc.x - g.fsAccPrev.x, g.fsAcc.y - g.fsAccPrev.y, g.fsAcc.z - g.fsAccPrev.z };
        PutF(b, 0x78, Len3(df));
    }
    LeaveCriticalSection(&s_gearCs);
    return 1;
}

int32_t GearRead(int32_t chan, uint8_t* bufs, uint32_t length, int32_t* err) {
    EnsureInit();
    int32_t n = 0;
    if (bufs && length) {
        if (!CtlPcMode() && g_cfg.enabled && g_cfg.gearPad && chan == g_cfg.channel) {
            n = FillKpadStatus(chan, bufs);
        } else {
            memset(bufs, 0, KPAD_STATUS_SIZE);
            bufs[0x5C] = WPAD_DEV_NOT_FOUND;
            bufs[0x5D] = 0xFF;
        }
    }
    if (err) *err = n ? 0 : -1;
    return n;
}

inline bool ChanOk(int32_t chan) { return (uint32_t)chan < 4; }

void SetParam(int32_t chan, int which, float a, float b) {
    EnsureInit();
    if (!ChanOk(chan)) return;
    EnterCriticalSection(&s_gearCs);
    s_gear[chan].params[which][0] = a;
    s_gear[chan].params[which][1] = b;
    LeaveCriticalSection(&s_gearCs);
}

void MplsSet(int32_t chan, int which, bool on) {
    EnsureInit();
    if (ChanOk(chan)) s_gear[chan].mplsOn[which] = on;
}

float MplsIs(int32_t chan, int which) {
    EnsureInit();
    // KPAD returns the correction strength while enabled and a negative value while disabled
    return (ChanOk(chan) && s_gear[chan].mplsOn[which]) ? 0.0f : -1.0f;
}

inline void PutOut(float* p, float v) { if (p) *p = v; }

}  // namespace

extern "C" {

// ---- KPAD / WPAD ----
int32_t __cdecl WrapKPADRead(int32_t chan, uint8_t* bufs, uint32_t length) {
    return GearRead(chan, bufs, length, NULL);
}

int32_t __cdecl WrapKPADReadEx(int32_t chan, uint8_t* bufs, uint32_t length, int32_t* err) {
    return GearRead(chan, bufs, length, err);
}

void __cdecl WrapKPADInit(void) {
    EnsureInit();
    Log("WrapKPADInit");
}

void __cdecl WrapKPADEnableAimingMode(int32_t chan) {
    EnsureInit();
    if (ChanOk(chan)) s_gear[chan].aiming = true;
}

void __cdecl WrapKPADSetPosParam(int32_t chan, float playRadius, float sensitivity)  { SetParam(chan, 0, playRadius, sensitivity); }
void __cdecl WrapKPADSetDistParam(int32_t chan, float playRadius, float sensitivity) { SetParam(chan, 1, playRadius, sensitivity); }
void __cdecl WrapKPADSetAccParam(int32_t chan, float playRadius, float sensitivity)  { SetParam(chan, 2, playRadius, sensitivity); }
void __cdecl WrapKPADSetHoriParam(int32_t chan, float playRadius, float sensitivity) { SetParam(chan, 3, playRadius, sensitivity); }

void __cdecl WrapWPADRegisterAllocator(void* alloc, void* freeFn) {
    EnsureInit();
    Log("WrapWPADRegisterAllocator(%p, %p)", alloc, freeFn);
}

void __cdecl WrapWPADControlMotor(int32_t chan, uint32_t command) {
    EnsureInit();
    if (!CtlPcMode() && g_cfg.enabled && g_cfg.gearPad && chan == g_cfg.channel) PcSourceSetRumble(command == 1);
}

void __cdecl WrapWPADShutdown(void) {
    EnsureInit();
    Log("WrapWPADShutdown (the WPAD server keeps running until the process exits)");
}

uint8_t __cdecl WrapWPADGetSensorBarPosition(void) {
    EnsureInit();
    return (uint8_t)g_cfg.sensorBar;
}

int32_t __cdecl WrapKMPLSRead(int32_t, void*, uint32_t) {
    EnsureInit();
    return 0;                              // no MotionPlus
}

void __cdecl WrapKPADGetUnifiedWpadStatus(int32_t, void*, uint32_t) {
    EnsureInit();                          // no caller in the PC executable; buffer layout unknown, left untouched
}

void __cdecl WrapKPADEnableMpls(int32_t chan, uint8_t mode) {
    EnsureInit();
    Log("WrapKPADEnableMpls(%d, %u) - no MotionPlus", chan, mode);
}

void __cdecl WrapKPADDisableMpls(int32_t) {
    EnsureInit();
}

// ---- MotionPlus corrections (state only) ----
float __cdecl WrapKMPLSIsEnableZeroPlay(int32_t chan)  { return MplsIs(chan, 0); }
float __cdecl WrapKMPLSIsEnableZeroDrift(int32_t chan) { return MplsIs(chan, 1); }
float __cdecl WrapKMPLSIsEnableDirRevise(int32_t chan) { return MplsIs(chan, 2); }
float __cdecl WrapKMPLSIsEnableAccRevise(int32_t chan) { return MplsIs(chan, 3); }
float __cdecl WrapKMPLSIsEnableDpdRevise(int32_t chan) { return MplsIs(chan, 4); }

void __cdecl WrapKMPLSGetZeroPlayParam(int32_t chan, float* radius) {
    EnsureInit();
    PutOut(radius, ChanOk(chan) ? s_gear[chan].zeroPlay : 0.0f);
}

void __cdecl WrapKMPLSGetZeroDriftParam(int32_t chan, float* a, float* b, float* c) {
    EnsureInit();
    const GearChan* g = ChanOk(chan) ? &s_gear[chan] : NULL;
    PutOut(a, g ? g->zeroDrift[0] : 0.0f);
    PutOut(b, g ? g->zeroDrift[1] : 0.0f);
    PutOut(c, g ? g->zeroDrift[2] : 0.0f);
}

void __cdecl WrapKMPLSGetDirReviseParam(int32_t chan, float* p) {
    EnsureInit();
    PutOut(p, ChanOk(chan) ? s_gear[chan].dirRevise : 0.0f);
}

void __cdecl WrapKMPLSGetAccReviseParam(int32_t chan, float* p, float* range) {
    EnsureInit();
    PutOut(p, ChanOk(chan) ? s_gear[chan].accRevise[0] : 0.0f);
    PutOut(range, ChanOk(chan) ? s_gear[chan].accRevise[1] : 0.0f);
}

void __cdecl WrapKMPLSGetDpdReviseParam(int32_t chan, float* p) {
    EnsureInit();
    PutOut(p, ChanOk(chan) ? s_gear[chan].dpdRevise : 0.0f);
}

void __cdecl WrapKMPLSEnableZeroPlay(int32_t chan)   { MplsSet(chan, 0, true); }
void __cdecl WrapKMPLSDisableZeroPlay(int32_t chan)  { MplsSet(chan, 0, false); }
void __cdecl WrapKMPLSEnableZeroDrift(int32_t chan)  { MplsSet(chan, 1, true); }
void __cdecl WrapKMPLSDisableZeroDrift(int32_t chan) { MplsSet(chan, 1, false); }
void __cdecl WrapKMPLSEnableDirRevise(int32_t chan)  { MplsSet(chan, 2, true); }
void __cdecl WrapKMPLSDisableDirRevise(int32_t chan) { MplsSet(chan, 2, false); }
void __cdecl WrapKMPLSEnableAccRevise(int32_t chan)  { MplsSet(chan, 3, true); }
void __cdecl WrapKMPLSDisableAccRevise(int32_t chan) { MplsSet(chan, 3, false); }
void __cdecl WrapKMPLSEnableDpdRevise(int32_t chan)  { MplsSet(chan, 4, true); }
void __cdecl WrapKMPLSDisableDpdRevise(int32_t chan) { MplsSet(chan, 4, false); }

void __cdecl WrapKMPLSSetZeroPlayParam(int32_t chan, float radius) {
    EnsureInit();
    if (ChanOk(chan)) s_gear[chan].zeroPlay = radius;
}

void __cdecl WrapKMPLSInitZeroPlayParam(int32_t chan) {
    EnsureInit();
    if (ChanOk(chan)) s_gear[chan].zeroPlay = 0.0f;
}

void __cdecl WrapKMPLSSetZeroDriftParam(int32_t chan, float a, float b, float c) {
    EnsureInit();
    if (!ChanOk(chan)) return;
    s_gear[chan].zeroDrift[0] = a;
    s_gear[chan].zeroDrift[1] = b;
    s_gear[chan].zeroDrift[2] = c;
}

void __cdecl WrapKMPLSInitZeroDriftParam(int32_t chan) {
    EnsureInit();
    if (!ChanOk(chan)) return;
    s_gear[chan].zeroDrift[0] = s_gear[chan].zeroDrift[1] = s_gear[chan].zeroDrift[2] = 0.0f;
}

void __cdecl WrapKMPLSSetDirReviseParam(int32_t chan, float p) {
    EnsureInit();
    if (ChanOk(chan)) s_gear[chan].dirRevise = p;
}

void __cdecl WrapKMPLSInitDirReviseParam(int32_t chan) {
    EnsureInit();
    if (ChanOk(chan)) s_gear[chan].dirRevise = 0.0f;
}

void __cdecl WrapKMPLSSetAccReviseParam(int32_t chan, float p, float range) {
    EnsureInit();
    if (!ChanOk(chan)) return;
    s_gear[chan].accRevise[0] = p;
    s_gear[chan].accRevise[1] = range;
}

void __cdecl WrapKMPLSInitAccReviseParam(int32_t chan) {
    EnsureInit();
    if (ChanOk(chan)) s_gear[chan].accRevise[0] = s_gear[chan].accRevise[1] = 0.0f;
}

void __cdecl WrapKMPLSSetDpdReviseParam(int32_t chan, float p) {
    EnsureInit();
    if (ChanOk(chan)) s_gear[chan].dpdRevise = p;
}

void __cdecl WrapKMPLSInitDpdReviseParam(int32_t chan) {
    EnsureInit();
    if (ChanOk(chan)) s_gear[chan].dpdRevise = 0.0f;
}

// ---- test only (wmtest.exe; the engine never calls this) ----
void __cdecl WiimoteTestInject(const WmTestInput* in) {
    EnsureInit();
    PcSourceInject(in);
}

}  // extern "C"
