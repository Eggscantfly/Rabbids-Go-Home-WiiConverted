// wmtest.cpp - offline test of wiimote.dll (32-bit console).  It does not start the game.
//
// Loads wiimote.dll with WIIMOTE_INI / WIIMOTE_TEST set, calls the exports in the engine's order, connects to the
// DLL's WPAD server the way the PC executable's WPAD client does (0x007F3CD0: WSAEventSelect, one message per FD_READ,
// exact recv sizes, a short read drops the connection), answers like the KPAD sampling callback (0x007F21D0:
// DPD control, then data format) and decodes every sample with a port of the engine's KPAD code
// (0x007F0990 acc, 0x007F1C90 stick, 0x007F0D40..0x007F1590 DPD, first acquisition, no smoothing).
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define _WIN32_WINNT 0x0601
#include <winsock2.h>
#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

struct WmTestInput {
    uint16_t buttons;
    uint16_t pad0;
    int32_t  pointerValid;
    float    px, py;
    float    stickX, stickY;
    float    rollDeg;
    int32_t  shake;
};

#pragma pack(push, 1)
struct Info {
    int32_t chan;
    uint8_t addr[6];
    int32_t devType;
    int32_t dataFormat;
    uint8_t dpd[4];
    int32_t infoDpd, infoSpeaker, infoAttach, infoLowBat, infoNearEmpty;
    uint8_t battery, led, protocol, firmware;
    int16_t accZero[3], acc1g[3], fsAcc[6];
    uint8_t fsStick[6];
};
#pragma pack(pop)

struct Obj { int16_t x, y; uint16_t size; uint8_t id; };

struct Sample {
    uint16_t button;
    int16_t  ax, ay, az;
    Obj      obj[4];
    uint8_t  dev;
    int8_t   err;
    int16_t  fx, fy, fz;
    int8_t   sx, sy;
    uint8_t  fmt;
    double   t;
};

struct Kpad {
    uint32_t hold;
    float    ax, ay, az;
    bool     fsOk;
    float    fx, fy, fz;
    bool     stickOk;
    float    sx, sy;
    int      dpd;
    float    px, py, hx, hy, dist;
};

const char* const kExports[] = {
    "WrapKPADRead", "WrapKPADReadEx", "WrapKPADInit", "WrapKPADEnableAimingMode", "WrapKPADSetPosParam",
    "WrapKPADSetDistParam", "WrapKPADSetAccParam", "WrapKPADSetHoriParam", "WrapWPADRegisterAllocator",
    "WrapWPADControlMotor", "WrapWPADShutdown", "WrapWPADGetSensorBarPosition", "WrapKMPLSRead",
    "WrapKPADGetUnifiedWpadStatus", "WrapKPADEnableMpls", "WrapKPADDisableMpls", "WrapKMPLSIsEnableZeroPlay",
    "WrapKMPLSGetZeroPlayParam", "WrapKMPLSIsEnableZeroDrift", "WrapKMPLSGetZeroDriftParam",
    "WrapKMPLSIsEnableDirRevise", "WrapKMPLSGetDirReviseParam", "WrapKMPLSIsEnableAccRevise",
    "WrapKMPLSGetAccReviseParam", "WrapKMPLSIsEnableDpdRevise", "WrapKMPLSGetDpdReviseParam",
    "WrapKMPLSEnableZeroPlay", "WrapKMPLSDisableZeroPlay", "WrapKMPLSSetZeroPlayParam", "WrapKMPLSInitZeroPlayParam",
    "WrapKMPLSEnableZeroDrift", "WrapKMPLSDisableZeroDrift", "WrapKMPLSSetZeroDriftParam",
    "WrapKMPLSInitZeroDriftParam", "WrapKMPLSEnableDirRevise", "WrapKMPLSDisableDirRevise",
    "WrapKMPLSSetDirReviseParam", "WrapKMPLSInitDirReviseParam", "WrapKMPLSEnableAccRevise",
    "WrapKMPLSDisableAccRevise", "WrapKMPLSSetAccReviseParam", "WrapKMPLSInitAccReviseParam",
    "WrapKMPLSEnableDpdRevise", "WrapKMPLSDisableDpdRevise", "WrapKMPLSSetDpdReviseParam",
    "WrapKMPLSInitDpdReviseParam",
};

typedef int32_t (__cdecl* ReadFn)(int32_t, uint8_t*, uint32_t);
typedef int32_t (__cdecl* ReadExFn)(int32_t, uint8_t*, uint32_t, int32_t*);
typedef void    (__cdecl* VoidFn)(void);
typedef void    (__cdecl* ChanFn)(int32_t);
typedef void    (__cdecl* Ptr2Fn)(void*, void*);
typedef void    (__cdecl* MotorFn)(int32_t, uint32_t);
typedef uint8_t (__cdecl* U8Fn)(void);
typedef float   (__cdecl* FChanFn)(int32_t);
typedef void    (__cdecl* InjectFn)(const WmTestInput*);

const int kPort = 42424;
const char* const kTestIni =
    "[general]\n"
    "enabled=1\n"
    "log=1\n"
    "log_verbose=1\n"
    "channel=0\n"
    "nunchuk=1\n"
    "port=42424\n"
    "rate_hz=200\n"
    "require_focus=0\n"
    "gear_pad=1\n"
    "rumble=0\n"
    "[mods]\n"
    "enabled=0\n"
    "[controls]\n"
    "mode=wii\n"                                   // the virtual remote is what this test exercises
    "[pointer]\n"
    "mouse=0\n"
    "pad_stick=0\n"
    "dist=2.0\n"
    "sensor_bar=0\n"
    "kpad_center_y=auto\n"
    "[motion]\n"
    "shake_g=3.0\n"
    "shake_hz=6.0\n"
    "shake_target=remote\n"
    "[xinput]\n"
    "enabled=0\n";

HMODULE             g_dll;
SOCKET              g_sock = INVALID_SOCKET;
WSAEVENT            g_evt;
Info                g_info[4];
uint8_t             g_sensorBar = 0xEE;
int32_t             g_motor = -1;
int                 g_lastDpdCmd;       // KPAD channel +0x565
bool                g_dpdBusy;          // KPAD channel +0x563
std::vector<Sample> g_samples;
int                 g_msgs[9];
int                 g_cmds[6];
std::string         g_err;
int                 g_pass, g_fail;
InjectFn            g_inject;

double Now() {
    static LARGE_INTEGER f;
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart / (double)f.QuadPart;
}

void Check(bool ok, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    printf(ok ? "  PASS  " : "  FAIL  ");
    vprintf(fmt, ap);
    printf("\n");
    va_end(ap);
    if (ok) ++g_pass; else ++g_fail;
}

bool Near(double a, double b, double tol) { return fabs(a - b) <= tol; }

bool RecvExact(void* p, int n) {
    int r = recv(g_sock, (char*)p, n, 0);
    if (r == n) return true;
    if (g_err.empty()) {
        char b[160];
        sprintf(b, "short read: %d of %d bytes (WSA error %d) - the engine would drop the connection", r, n,
                WSAGetLastError());
        g_err = b;
    }
    return false;
}

void SendCmd(uint32_t cmd, uint32_t chan, uint32_t arg) {
    uint32_t m[3] = { cmd, chan, arg };
    send(g_sock, (const char*)m, sizeof(m), 0);
    if (cmd < 6) ++g_cmds[cmd];
}

// 0x007F21D0: the KPAD sampling callback asks for the DPD mode and the data format its table wants
void SamplingCallback(int chan, const Sample& s) {
    static const uint8_t kTable[6][2] = { {0, 1}, {3, 2}, {0, 4}, {1, 5}, {0, 7}, {1, 8} };
    int dev = g_info[chan].devType;
    int e;
    if (dev == 0) e = 0;
    else if (dev == 1) e = 2;
    else if (dev == 2) e = 4;
    else return;
    e += 1;                                              // KPAD DPD enabled (+0x564 = 1 after KPADInit)
    int cur = g_info[chan].dpd[0] ? g_lastDpdCmd : 0;
    if (cur != kTable[e][0]) {
        if (!g_dpdBusy) {
            g_dpdBusy = true;
            SendCmd(4, (uint32_t)chan, kTable[e][0]);
            g_lastDpdCmd = kTable[e][0];
        }
    } else if (s.fmt != kTable[e][1]) {
        SendCmd(3, (uint32_t)chan, kTable[e][1]);
    }
}

bool HandleMessage() {
    uint8_t type = 0;
    if (!RecvExact(&type, 1)) return false;
    if (type == 1) {
        if (!RecvExact(&g_sensorBar, 1) || !RecvExact(&g_motor, 4)) return false;
    } else if (type >= 2 && type <= 7) {
        Info in;
        if (!RecvExact(&in, sizeof(in))) return false;
        if ((uint32_t)in.chan > 3) {
            g_err = "channel info for a bad channel";
            return false;
        }
        g_info[in.chan] = in;
        if (type == 6) g_dpdBusy = false;               // ControlDpd callback 0x007F2000
    } else if (type == 8) {
        int32_t chan = -1;
        if (!RecvExact(&chan, 4)) return false;
        if ((uint32_t)chan > 3) {
            g_err = "sample for a bad channel";
            return false;
        }
        Sample s;
        memset(&s, 0, sizeof(s));
        int dev = g_info[chan].devType;
        if (dev == 0 || dev == 1) {
            if (!RecvExact(&s.button, 2) || !RecvExact(&s.ax, 2) || !RecvExact(&s.ay, 2) || !RecvExact(&s.az, 2))
                return false;
            for (int i = 0; i < 4; ++i) {
                if (!RecvExact(&s.obj[i].x, 2) || !RecvExact(&s.obj[i].y, 2) || !RecvExact(&s.obj[i].size, 2) ||
                    !RecvExact(&s.obj[i].id, 1))
                    return false;
            }
            if (!RecvExact(&s.dev, 1) || !RecvExact(&s.err, 1)) return false;
        }
        if (dev == 1) {
            if (!RecvExact(&s.fx, 2) || !RecvExact(&s.fy, 2) || !RecvExact(&s.fz, 2) || !RecvExact(&s.sx, 1) ||
                !RecvExact(&s.sy, 1))
                return false;
        }
        s.fmt = (uint8_t)g_info[chan].dataFormat;        // record +0x3A = WPAD data format (0x007F29C0)
        s.t = Now();
        g_samples.push_back(s);
        SamplingCallback(chan, s);
    } else {
        char b[64];
        sprintf(b, "unknown message type %u", type);
        g_err = b;
        return false;
    }
    ++g_msgs[type];
    return true;
}

void Pump(int ms) {
    double end = Now() + ms / 1000.0;
    while (Now() < end && g_err.empty()) {
        DWORD w = WSAWaitForMultipleEvents(1, &g_evt, FALSE, 10, FALSE);
        if (w != WSA_WAIT_EVENT_0) continue;
        WSANETWORKEVENTS ne;
        if (WSAEnumNetworkEvents(g_sock, g_evt, &ne) != 0) {
            g_err = "WSAEnumNetworkEvents failed";
            break;
        }
        if (ne.lNetworkEvents & FD_READ) HandleMessage();   // one message per FD_READ, like the engine
        if ((ne.lNetworkEvents & FD_CLOSE) && g_err.empty()) g_err = "the server closed the connection";
    }
}

float ClampF(float v, float m) { return v > m ? m : (v < -m ? -m : v); }

float StickAxis(int v) {                                  // 0x007F1C90 with min 15, max 71
    const int mn = 15, mx = 71;
    int a = v < 0 ? -v : v;
    float r = a <= mn ? 0.0f : (a < mx ? (float)(a - mn) / (float)(mx - mn) : 1.0f);
    return v < 0 ? -r : r;
}

Kpad Decode(const Sample& s, float cy) {
    const double K = 0.2 / 0.38386398553848267;           // channel +0x554
    const double S = 1.25 / 0.55;                         // channel +0xC4
    Kpad k;
    memset(&k, 0, sizeof(k));
    k.hold = s.button;
    if (s.err != 0) return k;
    const Info& in = g_info[0];
    int ux = (in.acc1g[0] - in.accZero[0]) * 4;           // 0x007F2A30 (z uses the y calibration)
    int uy = (in.acc1g[1] - in.accZero[1]) * 4;
    int uz = uy;
    float scx = 0.01f, scy = 0.01f, scz = 0.01f;          // 008A0D40 default
    if (ux * uy * uz != 0) {
        scx = 1.0f / ux;
        scy = 1.0f / uy;
        scz = 1.0f / uz;
    }
    k.ax = ClampF(-s.ax * scx, 3.4f);                     // 0x007F0990
    k.ay = ClampF(-s.az * scz, 3.4f);
    k.az = ClampF(s.ay * scy, 3.4f);
    if (s.dev == 1 && (s.fmt == 4 || s.fmt == 5)) {
        k.fsOk = true;
        k.fx = ClampF(-s.fx * 0.005f, 2.1f);
        k.fy = ClampF(-s.fz * 0.005f, 2.1f);
        k.fz = ClampF(s.fy * 0.005f, 2.1f);
    }
    if (s.dev == 1 && (s.fmt == 3 || s.fmt == 4 || s.fmt == 5)) {
        k.stickOk = true;
        k.sx = StickAxis(s.sx);
        k.sy = StickAxis(s.sy);
        float m2 = k.sx * k.sx + k.sy * k.sy;
        if (m2 > 1.0f) {
            float m = (float)sqrt(m2);
            k.sx /= m;
            k.sy /= m;
        }
    }
    if (!(s.fmt == 2 || s.fmt == 5 || s.fmt == 8)) return k;
    struct P { double x, y; bool ok; } p[4];
    for (int i = 0; i < 4; ++i) {                          // 0x007F0D40 + window 0x007F0DB0
        p[i].ok = false;
        p[i].x = p[i].y = 0.0;
        if (s.obj[i].size == 0) continue;
        p[i].x = s.obj[i].x * 0.001953125 - 0.9990234375;
        p[i].y = s.obj[i].y * 0.001953125 - 0.7490234375;
        p[i].ok = p[i].x > -0.95 && p[i].x < 0.95 && p[i].y > -0.7 && p[i].y < 0.7;
    }
    double ax = k.ax, ay = k.ay, az = k.az;
    double n2 = sqrt(ax * ax + ay * ay), n3 = sqrt(ax * ax + ay * ay + az * az);
    if (n3 <= 0.0 || n2 / n3 <= 0.7) return k;            // acc_vertical.x gate (0x007F19E0)
    if (n2 <= 0.0 || n2 >= 2.0) return k;
    double hx = -ay / n2, hy = -ax / n2;                  // accelerometer horizon (0x007F0620)
    int bi = -1, bj = -1;
    double best = 0.9;
    for (int i = 0; i < 4; ++i) {                          // pair choice (0x007F0F10)
        for (int j = i + 1; j < 4; ++j) {
            if (!p[i].ok || !p[j].ok) continue;
            double dx = p[j].x - p[i].x, dy = p[j].y - p[i].y, d = sqrt(dx * dx + dy * dy);
            if (d <= 0.0) continue;
            double est = K / d;
            if (!(est > K && est < 3.0)) continue;
            double dot = hx * (dx / d) + hy * (-dy / d);
            int a = i, b = j;
            if (dot < 0.0) {
                dot = -dot;
                a = j;
                b = i;
            }
            if (dot > best) {
                best = dot;
                bi = a;
                bj = b;
            }
        }
    }
    if (bi < 0) return k;
    double dx = p[bj].x - p[bi].x, dy = p[bj].y - p[bi].y, d = sqrt(dx * dx + dy * dy);
    double hzx = dx / d, hzy = -dy / d;                   // 0x007F14D0
    if (hx * hzx + hy * hzy <= 0.9) return k;             // agreement check (0x007F19E0)
    double mx = (p[bi].x + p[bj].x) * 0.5, my = (p[bi].y + p[bj].y) * 0.5;
    k.px = (float)((0.0 - (mx * hzx - my * hzy)) * S);    // 0x007F1590, first acquisition
    k.py = (float)((cy - (hzx * my + mx * hzy)) * S);
    k.hx = (float)hzx;
    k.hy = (float)hzy;
    k.dist = (float)(K / d);
    k.dpd = 2;
    return k;
}

Kpad Last() { return Decode(g_samples.back(), 0.2f); }

void Apply(const WmTestInput& in, int ms) {
    g_inject(&in);
    Pump(ms);
}

bool Connect() {
    WSADATA wd;
    WSAStartup(MAKEWORD(2, 2), &wd);
    for (int attempt = 0; attempt < 30; ++attempt) {
        g_sock = WSASocketA(AF_INET, SOCK_STREAM, IPPROTO_TCP, NULL, 0, 0);
        char one = 1;
        setsockopt(g_sock, SOL_SOCKET, SO_REUSEADDR, &one, 1);
        setsockopt(g_sock, IPPROTO_TCP, TCP_NODELAY, &one, 1);
        sockaddr_in a;
        memset(&a, 0, sizeof(a));
        a.sin_family = AF_INET;
        a.sin_port = htons(kPort);
        a.sin_addr.s_addr = inet_addr("127.0.0.1");
        if (WSAConnect(g_sock, (sockaddr*)&a, sizeof(a), NULL, NULL, NULL, NULL) == 0) {
            g_evt = WSACreateEvent();
            WSAEventSelect(g_sock, g_evt, FD_READ | FD_CLOSE);
            return true;
        }
        closesocket(g_sock);
        g_sock = INVALID_SOCKET;
        Sleep(100);
    }
    return false;
}

// ---- "wmtest sav <PC executable>": the save layer (patch sites in the exe file + self-test on a fake engine) ----
int SavMain(int argc, char** argv) {
    typedef int (__cdecl* CheckFn)(const char*, char*, int);
    typedef int (__cdecl* SelfTestFn)(const char*, char*, int);
    char exePath[MAX_PATH];
    GetModuleFileNameA(NULL, exePath, MAX_PATH);
    std::string dir(exePath);
    dir = dir.substr(0, dir.find_last_of("\\/") + 1);
    std::string testDir = dir + "test\\";
    CreateDirectoryA(testDir.c_str(), NULL);
    std::string iniPath = testDir + "wmsav.ini";
    FILE* f = fopen(iniPath.c_str(), "w");
    if (!f) {
        printf("cannot write %s\n", iniPath.c_str());
        return 2;
    }
    fputs("[general]\nenabled=0\nlog=1\nlog_verbose=1\n[save]\nenabled=1\nchannel_installed=1\n", f);
    fclose(f);
    SetEnvironmentVariableA("WIIMOTE_INI", iniPath.c_str());
    if (argc < 3) {
        printf("usage: wmtest sav <PC executable>\n");
        return 2;
    }
    std::string target = argv[2];
    printf("wmtest sav: %swiimote.dll, exe %s\n", dir.c_str(), target.c_str());
    HMODULE dll = LoadLibraryA((dir + "wiimote.dll").c_str());
    if (!dll) {
        printf("  FAIL  LoadLibrary error %lu\n", GetLastError());
        return 2;
    }
    CheckFn check = (CheckFn)GetProcAddress(dll, "WiimoteSavCheckExe");
    SelfTestFn self = (SelfTestFn)GetProcAddress(dll, "WiimoteSavSelfTest");
    if (!check || !self) {
        printf("  FAIL  WiimoteSavCheckExe / WiimoteSavSelfTest missing\n");
        return 2;
    }
    std::vector<char> buf(1 << 20);
    printf("[patch sites in the exe file]\n");
    int bad = check(target.c_str(), &buf[0], (int)buf.size());
    printf("%s", &buf[0]);
    std::string work = testDir + "savtest";
    CreateDirectoryA(work.c_str(), NULL);
    printf("[self-test on a fake engine, files in %s]\n", work.c_str());
    int fails = self(work.c_str(), &buf[0], (int)buf.size());
    printf("%s", &buf[0]);
    printf("\nexe check: %d difference(s); self-test: %d failure(s)\n", bad, fails);
    return (bad != 0 || fails != 0) ? 1 : 0;
}

// ---- "wmtest ctl <PC executable>": [controls] mode=pc (patch sites in the exe file + the pointer/motion natives on a
//      fake engine) ----
int CtlMain(int argc, char** argv) {
    typedef int (__cdecl* CheckFn)(const char*, char*, int);
    typedef int (__cdecl* SelfTestFn)(char*, int);
    char exePath[MAX_PATH];
    GetModuleFileNameA(NULL, exePath, MAX_PATH);
    std::string dir(exePath);
    dir = dir.substr(0, dir.find_last_of("\\/") + 1);
    std::string testDir = dir + "test\\";
    CreateDirectoryA(testDir.c_str(), NULL);
    std::string iniPath = testDir + "wmctl.ini";
    FILE* f = fopen(iniPath.c_str(), "w");
    if (!f) {
        printf("cannot write %s\n", iniPath.c_str());
        return 2;
    }
    fputs("[general]\nenabled=0\nlog=1\n[save]\nenabled=0\n[controls]\nmode=pc\n[pointer]\ndist=1.5\n", f);
    fclose(f);
    SetEnvironmentVariableA("WIIMOTE_INI", iniPath.c_str());
    if (argc < 3) {
        printf("usage: wmtest ctl <PC executable>\n");
        return 2;
    }
    std::string target = argv[2];
    printf("wmtest ctl: %swiimote.dll, exe %s\n", dir.c_str(), target.c_str());
    HMODULE dll = LoadLibraryA((dir + "wiimote.dll").c_str());
    if (!dll) {
        printf("  FAIL  LoadLibrary error %lu\n", GetLastError());
        return 2;
    }
    CheckFn check = (CheckFn)GetProcAddress(dll, "WiimoteCtlCheckExe");
    SelfTestFn self = (SelfTestFn)GetProcAddress(dll, "WiimoteCtlSelfTest");
    if (!check || !self) {
        printf("  FAIL  WiimoteCtlCheckExe / WiimoteCtlSelfTest missing\n");
        return 2;
    }
    std::vector<char> buf(1 << 16);
    printf("[patch sites in the exe file]\n");
    int bad = check(target.c_str(), &buf[0], (int)buf.size());
    printf("%s", &buf[0]);
    printf("[pointer and motion natives on a fake engine, [pointer] dist=1.5]\n");
    int fails = self(&buf[0], (int)buf.size());
    printf("%s", &buf[0]);
    printf("\nexe check: %d difference(s); self-test: %d failure(s)\n", bad, fails);
    return (bad != 0 || fails != 0) ? 1 : 0;
}

// ---- "wmtest video|timing <PC executable>": the module's patch sites in the exe file + its self-test ----
int PairMain(int argc, char** argv, const char* mode, const char* ini, const char* checkName, const char* selfName,
             const char* selfTitle) {
    typedef int (__cdecl* CheckFn)(const char*, char*, int);
    typedef int (__cdecl* SelfTestFn)(char*, int);
    char exePath[MAX_PATH];
    GetModuleFileNameA(NULL, exePath, MAX_PATH);
    std::string dir(exePath);
    dir = dir.substr(0, dir.find_last_of("\\/") + 1);
    std::string testDir = dir + "test\\";
    CreateDirectoryA(testDir.c_str(), NULL);
    std::string iniPath = testDir + "wm" + mode + ".ini";
    FILE* f = fopen(iniPath.c_str(), "w");
    if (!f) {
        printf("cannot write %s\n", iniPath.c_str());
        return 2;
    }
    fputs(ini, f);
    fclose(f);
    SetEnvironmentVariableA("WIIMOTE_INI", iniPath.c_str());
    if (argc < 3) {
        printf("usage: wmtest %s <PC executable>\n", mode);
        return 2;
    }
    std::string target = argv[2];
    printf("wmtest %s: %swiimote.dll, exe %s\n", mode, dir.c_str(), target.c_str());
    HMODULE dll = LoadLibraryA((dir + "wiimote.dll").c_str());
    if (!dll) {
        printf("  FAIL  LoadLibrary error %lu\n", GetLastError());
        return 2;
    }
    CheckFn check = (CheckFn)GetProcAddress(dll, checkName);
    SelfTestFn self = (SelfTestFn)GetProcAddress(dll, selfName);
    if (!check || !self) {
        printf("  FAIL  %s / %s missing\n", checkName, selfName);
        return 2;
    }
    std::vector<char> buf(1 << 16);
    printf("[patch sites in the exe file]\n");
    int bad = check(target.c_str(), &buf[0], (int)buf.size());
    printf("%s", &buf[0]);
    printf("[%s]\n", selfTitle);
    int fails = self(&buf[0], (int)buf.size());
    printf("%s", &buf[0]);
    printf("\nexe check: %d difference(s); self-test: %d failure(s)\n", bad, fails);
    return (bad != 0 || fails != 0) ? 1 : 0;
}

}  // namespace

int main(int argc, char** argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    if (argc > 1 && strcmp(argv[1], "sav") == 0) return SavMain(argc, argv);
    if (argc > 1 && strcmp(argv[1], "ctl") == 0) return CtlMain(argc, argv);
    if (argc > 1 && strcmp(argv[1], "video") == 0)
        return PairMain(argc, argv, "video",
                        "[general]\nenabled=0\nlog=1\n[save]\nenabled=0\n[video]\nnatives=1\nlog=1\n[timing]\nfps_cap=0\n",
                        "WiimoteVideoCheckExe", "WiimoteVideoSelfTest", "video natives and video accounting on a fake engine");
    if (argc > 1 && strcmp(argv[1], "afx") == 0)
        return PairMain(argc, argv, "afx", "[general]\nenabled=0\nlog=1\n[save]\nenabled=0\n[video]\nafx=0\n[timing]\nfps_cap=0\n",
                        "WiimoteAfxCheckExe", "WiimoteAfxSelfTest",
                        "after effect parameters, konst quantisation, BigBlur schedule");
    if (argc > 1 && strcmp(argv[1], "timing") == 0)
        return PairMain(argc, argv, "timing", "[general]\nenabled=0\nlog=1\n[save]\nenabled=0\n[timing]\nfps_cap=60\n",
                        "WiimoteTimingCheckExe", "WiimoteTimingSelfTest",
                        "pacing math, simulated engine frame loop, real clock");
    if (argc > 1 && strcmp(argv[1], "script") == 0)
        return PairMain(argc, argv, "script", "[general]\nenabled=0\nlog=1\n[save]\nenabled=0\n[timing]\nfps_cap=0\n"
                        "[script]\nenabled=0\n", "WiimoteScriptCheckExe", "WiimoteScriptSelfTest",
                        "script natives on a fake script stack");
    if (argc > 1 && strcmp(argv[1], "fixes") == 0)
        return PairMain(argc, argv, "fixes", "[general]\nenabled=0\nlog=1\n[save]\nenabled=0\n[timing]\nfps_cap=0\n",
                        "WiimoteFixesCheckExe", "WiimoteFixesSelfTest",
                        "the display's viewport with and without the widescreen height, game controller count");
    if (argc > 1 && strcmp(argv[1], "audio") == 0)
        return PairMain(argc, argv, "audio", "[general]\nenabled=0\nlog=1\n[save]\nenabled=0\n[timing]\nfps_cap=0\n"
                        "[audio]\ndoppler=engine\n", "WiimoteAudioCheckExe", "WiimoteAudioSelfTest",
                        "sound source velocities: the engine's and the windowed ones at several frame rates");
    if (argc > 1 && strcmp(argv[1], "mods") == 0)
        return PairMain(argc, argv, "mods", "[general]\nenabled=0\nlog=1\n[save]\nenabled=0\n[timing]\nfps_cap=0\n"
                        "[mods]\nenabled=0\n", "WiimoteModsCheckExe", "WiimoteModsSelfTest",
                        "package record replacement, the synthesized file header, key names, the enabled list");
    if (argc > 1 && strcmp(argv[1], "motion") == 0)
        return PairMain(argc, argv, "motion", "[general]\nenabled=0\nlog=1\n[save]\nenabled=0\n[timing]\nfps_cap=0\n"
                        "[options]\nenabled=0\n", "WiimoteOptionsCheckExe", "WiimoteMotionSelfTest",
                        "the Wii remote's motion from the bindings, and WII_LIB_MOVE's shake detection on it");
    if (argc > 1 && strcmp(argv[1], "options") == 0)
        return PairMain(argc, argv, "options", "[general]\nenabled=0\nlog=1\n[save]\nenabled=0\n[timing]\nfps_cap=0\n"
                        "[options]\nenabled=0\n", "WiimoteOptionsCheckExe", "WiimoteOptionsSelfTest",
                        "input names, options.ini, actions on a fake engine, the Options page's navigation");
    char exePath[MAX_PATH];
    GetModuleFileNameA(NULL, exePath, MAX_PATH);
    std::string dir(exePath);
    dir = dir.substr(0, dir.find_last_of("\\/") + 1);
    std::string dllPath = dir + "wiimote.dll";
    std::string testDir = dir + "test\\";
    CreateDirectoryA(testDir.c_str(), NULL);
    std::string iniPath = testDir + "wmtest.ini";
    FILE* f = fopen(iniPath.c_str(), "w");
    if (!f) {
        printf("cannot write %s\n", iniPath.c_str());
        return 2;
    }
    fputs(kTestIni, f);
    fclose(f);
    SetEnvironmentVariableA("WIIMOTE_INI", iniPath.c_str());
    SetEnvironmentVariableA("WIIMOTE_TEST", "1");
    for (int i = 0; i < 4; ++i) g_info[i].devType = 0xFD;  // engine default (0x007F41E0)

    printf("wmtest: %s\n[exports]\n", dllPath.c_str());
    g_dll = LoadLibraryA(dllPath.c_str());
    if (!g_dll) {
        printf("  FAIL  LoadLibrary error %lu\n", GetLastError());
        return 2;
    }
    int missing = 0;
    const int nExports = (int)(sizeof(kExports) / sizeof(kExports[0]));
    for (int i = 0; i < nExports; ++i) {
        if (!GetProcAddress(g_dll, kExports[i])) {
            printf("  missing %s\n", kExports[i]);
            ++missing;
        }
    }
    Check(missing == 0, "%d engine exports present (%d missing)", nExports - missing, missing);
    g_inject = (InjectFn)GetProcAddress(g_dll, "WiimoteTestInject");
    Check(g_inject != NULL, "WiimoteTestInject present");
    if (missing || !g_inject) return 1;

    // engine order: Gear init 0x007E18D0, then the end of KPADInit 0x007F1F80
    ((Ptr2Fn)GetProcAddress(g_dll, "WrapWPADRegisterAllocator"))(NULL, NULL);
    ((VoidFn)GetProcAddress(g_dll, "WrapKPADInit"))();
    ((ChanFn)GetProcAddress(g_dll, "WrapKPADEnableAimingMode"))(0);
    MotorFn motor = (MotorFn)GetProcAddress(g_dll, "WrapWPADControlMotor");
    for (int c = 3; c >= 0; --c) motor(c, 0);

    WmTestInput rest;
    memset(&rest, 0, sizeof(rest));
    rest.pointerValid = 1;
    g_inject(&rest);

    printf("[protocol]\n");
    bool connected = Connect();
    Check(connected, "WPAD client connects to 127.0.0.1:%d", kPort);
    if (!connected) return 1;
    Pump(500);
    Check(g_err.empty(), "no protocol error during the handshake %s", g_err.c_str());
    Check(g_msgs[1] == 1 && g_sensorBar == 0 && g_motor == 1, "hello (type 1) x%d: sensor bar %u, motor enabled %d",
          g_msgs[1], g_sensorBar, g_motor);
    Check(g_msgs[2] == 1 && g_info[0].devType == 1 && g_info[0].infoAttach == 1,
          "connect (type 2) x%d: channel 0 dev_type %d attach %d", g_msgs[2], g_info[0].devType, g_info[0].infoAttach);
    Check(g_info[1].devType == 0xFD && g_info[2].devType == 0xFD && g_info[3].devType == 0xFD,
          "channels 1..3 stay WPAD_DEV_NOT_FOUND");
    int unit = (g_info[0].acc1g[0] - g_info[0].accZero[0]) * 4;
    Check(unit == 100, "calibration: WPADGetAccGravityUnit = %d counts per g", unit);
    Check(g_info[0].dataFormat == 5 && g_info[0].dpd[0] == 1 && g_msgs[6] >= 1 && g_msgs[7] >= 1,
          "DPD/format handshake: format %d dpd %u (cmd 4 x%d -> type 6 x%d, cmd 3 x%d -> type 7 x%d)",
          g_info[0].dataFormat, g_info[0].dpd[0], g_cmds[4], g_msgs[6], g_cmds[3], g_msgs[7]);
    Check(g_samples.size() > 50, "samples (type 8) received: %u", (unsigned)g_samples.size());

    Pump(500);
    size_t n0 = g_samples.size();
    double t0 = Now();
    Pump(2000);
    double t1 = Now();
    size_t n1 = g_samples.size();
    double rate = (n1 - n0) / (t1 - t0);
    double span = g_samples[n1 - 1].t - g_samples[n0].t;
    printf("    %u samples in %.3f s (first-to-last receive span %.3f s)\n", (unsigned)(n1 - n0), t1 - t0, span);
    Check(rate > 180.0 && rate < 220.0, "sample rate %.1f Hz", rate);

    printf("[rest]\n");
    Apply(rest, 150);
    Kpad k = Last();
    Check(Near(k.ax, 0, 0.02) && Near(k.ay, -1, 0.02) && Near(k.az, 0, 0.02), "remote acc (%.3f, %.3f, %.3f) = (0, -1, 0)",
          k.ax, k.ay, k.az);
    Check(k.fsOk && Near(k.fx, 0, 0.02) && Near(k.fy, -1, 0.02) && Near(k.fz, 0, 0.02),
          "Nunchuk acc (%.3f, %.3f, %.3f) = (0, -1, 0)", k.fx, k.fy, k.fz);
    Check(k.stickOk && k.sx == 0.0f && k.sy == 0.0f, "Nunchuk stick centred (%.3f, %.3f)", k.sx, k.sy);
    Check(k.dpd == 2 && Near(k.px, 0, 0.01) && Near(k.py, 0, 0.01) && Near(k.hx, 1, 0.001) && Near(k.hy, 0, 0.001) &&
          Near(k.dist, 2.0, 0.03), "pointer at the centre: dpd %d pos (%.4f, %.4f) horizon (%.3f, %.3f) dist %.3f",
          k.dpd, k.px, k.py, k.hx, k.hy, k.dist);

    printf("[pointer]\n");
    int bad = 0;
    double worst = 0.0;
    for (int iy = 0; iy <= 4; ++iy) {
        for (int ix = 0; ix <= 4; ++ix) {
            WmTestInput in = rest;
            in.px = -1.0f + 0.5f * ix;
            in.py = -1.0f + 0.5f * iy;
            Apply(in, 60);
            Kpad q = Last();
            double e = std::max(fabs(q.px - in.px), fabs(q.py - in.py));
            if (q.dpd != 2 || e > 0.01) {
                ++bad;
                printf("    (%.2f, %.2f) -> dpd %d (%.3f, %.3f)\n", in.px, in.py, q.dpd, q.px, q.py);
            }
            worst = std::max(worst, e);
        }
    }
    Check(bad == 0, "25 positions from (-1,-1) to (1,1) decode to the same KPAD pos (worst error %.4f)", worst);
    const float rolls[2] = { -25.0f, 25.0f };
    for (int i = 0; i < 2; ++i) {
        WmTestInput in = rest;
        in.rollDeg = rolls[i];
        in.px = 0.6f;
        in.py = -0.4f;
        Apply(in, 100);
        Kpad q = Last();
        double ang = atan2(q.hy, q.hx) * 180.0 / 3.14159265358979;
        double sr = sin(rolls[i] * 3.14159265358979 / 180.0), cr = cos(rolls[i] * 3.14159265358979 / 180.0);
        Check(q.dpd == 2 && Near(q.px, 0.6, 0.01) && Near(q.py, -0.4, 0.01) && fabs(ang - rolls[i]) < 0.5 &&
              Near(q.ax, -sr, 0.02) && Near(q.ay, -cr, 0.02),
              "roll %+.0f: pos (%.3f, %.3f) horizon angle %+.2f, acc (%.3f, %.3f, %.3f)", rolls[i], q.px, q.py, ang,
              q.ax, q.ay, q.az);
    }
    WmTestInput off = rest;
    off.pointerValid = 0;
    Apply(off, 80);
    k = Last();
    const Sample& ls = g_samples.back();
    Check(k.dpd == 0 && ls.obj[0].size == 0 && ls.obj[1].size == 0, "pointer not valid: no IR object, dpd_valid_fg 0");

    printf("[buttons]\n");
    static const struct { uint16_t bit; const char* name; } kBits[] = {
        {0x0800, "A"}, {0x0400, "B"}, {0x0200, "1"}, {0x0100, "2"}, {0x0010, "PLUS"}, {0x1000, "MINUS"},
        {0x8000, "HOME"}, {0x0008, "UP"}, {0x0004, "DOWN"}, {0x0001, "LEFT"}, {0x0002, "RIGHT"}, {0x2000, "Z"},
        {0x4000, "C"},
    };
    bad = 0;
    uint16_t all = 0;
    for (size_t i = 0; i < sizeof(kBits) / sizeof(kBits[0]); ++i) {
        WmTestInput in = rest;
        in.buttons = kBits[i].bit;
        all |= kBits[i].bit;
        Apply(in, 40);
        if (Last().hold != kBits[i].bit) {
            ++bad;
            printf("    %s -> %04X\n", kBits[i].name, Last().hold);
        }
    }
    WmTestInput allIn = rest;
    allIn.buttons = all;
    Apply(allIn, 40);
    Check(bad == 0 && Last().hold == all, "13 buttons reach the WPAD button word (all together %04X)", Last().hold);

    printf("[stick]\n");
    static const float kSticks[][2] = { {1, 0}, {0, 1}, {-1, 0}, {0, -1}, {0.5f, 0.5f}, {-0.3f, 0.2f}, {0.707f, -0.707f},
                                        {0.1f, 0.0f} };
    bad = 0;
    for (size_t i = 0; i < sizeof(kSticks) / sizeof(kSticks[0]); ++i) {
        WmTestInput in = rest;
        in.stickX = kSticks[i][0];
        in.stickY = kSticks[i][1];
        Apply(in, 40);
        Kpad q = Last();
        if (!q.stickOk || !Near(q.sx, in.stickX, 0.02) || !Near(q.sy, in.stickY, 0.02)) {
            ++bad;
            printf("    (%.2f, %.2f) -> (%.3f, %.3f)\n", in.stickX, in.stickY, q.sx, q.sy);
        }
    }
    Check(bad == 0, "8 stick positions decode within 0.02");

    printf("[shake]\n");
    WmTestInput sh = rest;
    sh.shake = 1;
    size_t s0 = g_samples.size();
    Apply(sh, 400);
    sh.shake = 0;
    Apply(sh, 300);
    double peak = 0.0, fsDev = 0.0;
    int above = 0;
    for (size_t i = s0; i < g_samples.size(); ++i) {
        Kpad q = Decode(g_samples[i], 0.2f);
        double m = sqrt(q.ax * q.ax + q.ay * q.ay + q.az * q.az);
        peak = std::max(peak, m);
        if (m > 1.45) ++above;
        fsDev = std::max(fsDev, (double)fabs(q.fy + 1.0f) + fabs(q.fz));
    }
    Check(peak > 2.3 && above >= 10, "shake: peak %.2f g, %d samples above 1.45 g", peak, above);
    Check(fsDev < 0.05, "shake_target=remote leaves the Nunchuk at rest (max deviation %.3f g)", fsDev);
    k = Last();
    Check(Near(k.ay, -1, 0.02) && Near(k.az, 0, 0.02), "back at rest after the shake (%.3f, %.3f, %.3f)", k.ax, k.ay,
          k.az);

    printf("[rumble]\n");
    SendCmd(5, 0, 1);
    Pump(50);
    SendCmd(5, 0, 0);
    Pump(50);
    Check(g_err.empty(), "ControlMotor commands keep the connection %s", g_err.c_str());

    printf("[gear exports]\n");
    ReadFn rd = (ReadFn)GetProcAddress(g_dll, "WrapKPADRead");
    ReadExFn rdx = (ReadExFn)GetProcAddress(g_dll, "WrapKPADReadEx");
    WmTestInput g = rest;
    g.buttons = 0x0800 | 0x2000;
    g.px = 0.25f;
    g.py = -0.5f;
    g.stickX = 0.5f;
    Apply(g, 60);
    std::vector<uint8_t> buf(16 * 0xF0, 0xAA);
    int n = rd(0, buf.data(), 16);
    uint32_t hold;
    float px, py, sx;
    memcpy(&hold, &buf[0x00], 4);
    memcpy(&px, &buf[0x20], 4);
    memcpy(&py, &buf[0x24], 4);
    memcpy(&sx, &buf[0x60], 4);
    Check(n == 1 && buf[0x5C] == 1 && buf[0x5D] == 0 && buf[0x5E] == 2 && buf[0x5F] == 5 && hold == 0x2800 &&
          Near(px, 0.25, 0.001) && Near(py, -0.5, 0.001) && Near(sx, 0.5, 0.001),
          "WrapKPADRead(0): count %d dev %u err %d dpd %d fmt %u hold %04X pos (%.3f, %.3f) stick x %.3f", n, buf[0x5C],
          (int8_t)buf[0x5D], (int8_t)buf[0x5E], buf[0x5F], hold, px, py, sx);
    int32_t err = 5;
    n = rdx(0, buf.data(), 16, &err);
    Check(n == 1 && err == 0, "WrapKPADReadEx(0): count %d err %d", n, err);
    n = rd(1, buf.data(), 16);
    Check(n == 0 && buf[0x5C] == 0xFD && (int8_t)buf[0x5D] == -1, "WrapKPADRead(1): no controller (count %d dev %u)", n,
          buf[0x5C]);
    float zp = ((FChanFn)GetProcAddress(g_dll, "WrapKMPLSIsEnableZeroPlay"))(0);
    Check(zp < 0.0f, "WrapKMPLSIsEnableZeroPlay(0) = %.1f (disabled)", zp);
    uint8_t bar = ((U8Fn)GetProcAddress(g_dll, "WrapWPADGetSensorBarPosition"))();
    Check(bar == 0, "WrapWPADGetSensorBarPosition = %u", bar);

    Pump(100);
    Check(g_err.empty(), "no protocol error during the whole run %s", g_err.c_str());
    printf("\n%d passed, %d failed\n", g_pass, g_fail);
    closesocket(g_sock);
    return g_fail ? 1 : 0;
}
