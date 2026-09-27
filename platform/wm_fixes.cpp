// wm_fixes.cpp - fixes that are always on (each installed only when the PC executable matches): widescreen and the
// controller scan stutter.  Both follow the "Controller Stutter + RGH Widescreen Fix" DLL (the widescreen fix was
// found by Duckymomo).
//
// Widescreen.  The configuration (default.cfg: K3D_ScreenRatio = 3, 16:9; K3D_ScreenVSize = 720) gives the display a
// virtual screen height of 720 lines (00501C8F -> display +0x930).  Each time the client area changes, the display
// (0041AC51) fits a viewport of that ratio and at most that height into it and centres it (width = VSize x ratio,
// height = width / ratio, width = height x ratio, each clamped to the client area), so a window larger than 1280x720
// showed a 1280x720 picture with black borders.  The height is set to 10000: the viewport is the largest 16:9 area of
// the window (all of a 16:9 window).  The display fits its viewport again at the next frame (its stored client width,
// display +0x958, is cleared).
//
// Controller scans.  The controller update (006ED470, every frame from the input poll) enumerates the controllers again
// every 2 s (KPAD, XInput, DirectInput: 006F53B0, 006FCC90, 006FC690), and each enumeration stalls a frame.  After the
// first scan the timer test is made to skip them (jbe at 006ED4BE -> jmp).  A watcher counts the game controllers in
// the raw input device list (HID usage page 1, usage 4 joystick / 5 game pad) every second; when they change, one scan
// runs (the jbe is back until the last scan time 00A97AEC changes, at most 3 s).
#include "wiimote.h"

#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>

using namespace wmpatch;

namespace {

const uint32_t G_DISPLAY       = 0x009DE178;    // K3D_gpo_Display
const uint32_t DISP_VSIZE      = 0x930;         // K3D_ScreenVSize
const uint32_t DISP_CLIENT_W   = 0x958;         // the client width the viewport was fitted to
const int32_t  WIDE_VSIZE      = 10000;
const uint32_t SCAN_JUMP       = 0x006ED4BE;    // jbe (76 1C): skip the scans until 2 s passed
const uint32_t G_LAST_SCAN     = 0x00A97AEC;    // float: time of the last scan
const uint8_t  OP_JBE = 0x76, OP_JMP = 0xEB;

struct Code { uint32_t va, len, crc; const char* what; };
const Code kCode[] = {
    { 0x0041AC51, 0x076, 0x5FD8F605, "display: viewport fitted to K3D_ScreenRatio / K3D_ScreenVSize" },
    { 0x006ED470, 0x06C, 0x84851089, "controller update: scans every 2 s (jbe at 006ED4BE)" },
};

std::vector<std::string> s_pending;
bool s_inAttach;
bool s_wide, s_scans;
int  s_wideLogs;

void FixLog(const char* fmt, ...) {
    char buf[500];
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf(buf, sizeof(buf) - 1, fmt, ap);
    buf[sizeof(buf) - 1] = 0;
    va_end(ap);
    if (s_inAttach) s_pending.push_back(buf);
    else Log("FIXES: %s", buf);
}

// game controllers in the raw input device list: an order-independent signature and their number
uint64_t HidControllers(UINT* count) {
    *count = 0;
    UINT n = 0;
    if (GetRawInputDeviceList(NULL, &n, sizeof(RAWINPUTDEVICELIST)) != 0 || n == 0) return 0;
    std::vector<RAWINPUTDEVICELIST> list(n + 8);
    UINT size = (UINT)list.size();
    UINT got = GetRawInputDeviceList(&list[0], &size, sizeof(RAWINPUTDEVICELIST));
    if (got == (UINT)-1) return ~0ull;              // changed between the two calls: looked at again next time
    uint64_t sig = 0;
    for (UINT i = 0; i < got; ++i) {
        if (list[i].dwType != RIM_TYPEHID) continue;
        RID_DEVICE_INFO info;
        info.cbSize = sizeof(info);
        UINT infoSize = sizeof(info);
        if (GetRawInputDeviceInfoA(list[i].hDevice, RIDI_DEVICEINFO, &info, &infoSize) == (UINT)-1) continue;
        if (info.hid.usUsagePage == 1 && (info.hid.usUsage == 4 || info.hid.usUsage == 5)) {
            ++*count;
            sig += (uint64_t)(uintptr_t)list[i].hDevice * 0x9E3779B97F4A7C15ull + 1;
        }
    }
    return sig;
}

void SetScanJump(uint8_t op) {
    uint8_t cur = 0;
    ProcessImage img;
    if (img.Read(SCAN_JUMP, &cur, 1) && cur != op) WriteCode(SCAN_JUMP, &op, 1);
}

DWORD WINAPI FixesProc(LPVOID) {
    enum { WAIT_FIRST, SKIP, ALLOW } state = WAIT_FIRST;
    UINT count = 0;
    uint64_t pads = 0;
    double nextLook = 0, allowUntil = 0;
    float scanAt = 0;
    for (;;) {
        Sleep(200);
        double t = NowSeconds();
        if (s_wide) {
            uint8_t* disp = *(uint8_t**)(uintptr_t)G_DISPLAY;
            int32_t was = disp ? *(int32_t*)(disp + DISP_VSIZE) : WIDE_VSIZE;
            if (was != WIDE_VSIZE) {
                *(int32_t*)(disp + DISP_VSIZE) = WIDE_VSIZE;
                *(int32_t*)(disp + DISP_CLIENT_W) = 0;
                if (s_wideLogs++ < 4)
                    FixLog("widescreen: K3D_ScreenVSize %d -> %d (the picture fills the window)", was, WIDE_VSIZE);
            }
        }
        if (!s_scans) continue;
        float last = *(float*)(uintptr_t)G_LAST_SCAN;
        switch (state) {
        case WAIT_FIRST:
            if (last != 0.0f) {
                SetScanJump(OP_JMP);
                pads = HidControllers(&count);
                nextLook = t + 1.0;
                state = SKIP;
                FixLog("controller scans: first scan done (%u game controller(s)); the 2 s rescans are skipped until "
                       "controllers are connected or removed", count);
            }
            break;
        case SKIP:
            if (t >= nextLook) {
                nextLook = t + 1.0;
                UINT n = 0;
                uint64_t now = HidControllers(&n);
                if (now != pads && now != ~0ull) {
                    pads = now;
                    scanAt = last;
                    allowUntil = t + 3.0;
                    SetScanJump(OP_JBE);
                    state = ALLOW;
                    FixLog("game controllers changed (%u connected): the game scans them again", n);
                }
            }
            break;
        case ALLOW:
            if (last != scanAt || t > allowUntil) {
                SetScanJump(OP_JMP);
                nextLook = t + 1.0;
                state = SKIP;
            }
            break;
        }
    }
}

// the display's viewport for a client area (0041AC51, K3D_ScreenVSize != 0): {width, height, x, y}
void FitViewport(int cw, int ch, int vsize, double ratio, int out[4]) {
    int w = (int)floor(vsize * ratio + 0.5), x = (cw - w) / 2;
    if (cw - w < 0) w = cw, x = 0;
    int h = (int)floor(w / ratio + 0.5), y = (ch - h) / 2;
    if (ch - h < 0) h = ch, y = 0;
    w = (int)floor(h * ratio + 0.5);
    x = (cw - w) / 2;
    if (cw - w < 0) w = cw, x = 0;
    out[0] = w, out[1] = h, out[2] = x, out[3] = y;
}

}  // namespace

int FixesVerify(const Image& img, std::string& rep) {
    int bad = 0;
    uint32_t got = 0;
    for (size_t i = 0; i < sizeof(kCode) / sizeof(kCode[0]); ++i) {
        bool ok = CheckCrc(img, kCode[i].va, kCode[i].len, kCode[i].crc, &got);
        Report(rep, ok, "%-60s %08X len 0x%03X crc %08X (want %08X)", kCode[i].what, kCode[i].va, kCode[i].len, got,
               kCode[i].crc);
        if (!ok) ++bad;
    }
    return bad;
}

void FixesAttach() {
    s_inAttach = true;
    ProcessImage img;
    uint8_t probe[1];
    if (!img.Read(SCAN_JUMP, probe, 1)) {
        s_inAttach = false;                          // not the PC executable (wmtest): nothing to fix
        return;
    }
    std::string rep;
    int bad = FixesVerify(img, rep);
    bool display = GetProcAddress(GetModuleHandleA(NULL), "?K3D_gpo_Display@@3PAVK3D_S@@A") ==
                   (FARPROC)(uintptr_t)G_DISPLAY;
    uint32_t got = 0;
    s_wide = CheckCrc(img, kCode[0].va, kCode[0].len, kCode[0].crc, &got) && display;
    s_scans = CheckCrc(img, kCode[1].va, kCode[1].len, kCode[1].crc, &got) && probe[0] == OP_JBE;
    if (s_wide || s_scans) CloseHandle(CreateThread(NULL, 0, FixesProc, NULL, 0, NULL));
    FixLog("PC executable %s: widescreen %s, controller scan stutter fix %s", bad ? "differs" : "verified",
           s_wide ? "on" : "NOT installed", s_scans ? "on" : "NOT installed");
    s_inAttach = false;
}

void FixesAfterConfig() {
    for (size_t i = 0; i < s_pending.size(); ++i) Log("FIXES: %s", s_pending[i].c_str());
    s_pending.clear();
}

extern "C" {

// wmtest only: verify the fix sites against an exe file.  Returns the number of differences.
int __cdecl WiimoteFixesCheckExe(const char* exePath, char* out, int outSize) {
    FileImage img;
    std::string rep;
    int bad = -1;
    if (!img.Load(exePath)) rep = "  FAIL  cannot read " + std::string(exePath ? exePath : "(null)") + "\n";
    else bad = FixesVerify(img, rep);
    if (out && outSize > 0) {
        size_t n = rep.size() < (size_t)outSize - 1 ? rep.size() : (size_t)outSize - 1;
        memcpy(out, rep.data(), n);
        out[n] = 0;
    }
    return bad;
}

// wmtest only: the display's viewport with the configuration's height and with the fix; the controller count.
int __cdecl WiimoteFixesSelfTest(char* out, int outSize) {
    std::string rep;
    int fails = 0;
    const double r169 = (double)(16.0f / 9.0f);        // the executable's ratio table holds floats (0089EF14)
    struct Case { int cw, ch, vsize; int want[4]; const char* what; };
    const Case cases[] = {
        { 1920, 1080, 720,   { 1280, 720, 320, 180 }, "1920x1080 window, K3D_ScreenVSize 720: centred 1280x720" },
        { 1920, 1080, 10000, { 1920, 1080, 0, 0 },    "1920x1080 window, 10000: the whole window" },
        { 1280, 720, 10000,  { 1280, 720, 0, 0 },     "1280x720 window, 10000: the whole window" },
        { 1024, 768, 10000,  { 1024, 576, 0, 96 },    "1024x768 window, 10000: 16:9 band across the window" },
        { 854, 480, 720,     { 853, 480, 0, 0 },      "854x480 window, 720: 853x480 (width = height x ratio)" },
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        int v[4];
        FitViewport(cases[i].cw, cases[i].ch, cases[i].vsize, r169, v);
        bool ok = !memcmp(v, cases[i].want, sizeof(v));
        Report(rep, ok, "%s (got %dx%d at %d,%d)", cases[i].what, v[0], v[1], v[2], v[3]);
        if (!ok) ++fails;
    }
    UINT n = 0;
    uint64_t sig = HidControllers(&n);
    UINT n2 = 0;
    bool stable = HidControllers(&n2) == sig && n2 == n;
    Report(rep, stable, "raw input game controllers: %u (signature stable over two reads)", n);
    if (!stable) ++fails;
    if (out && outSize > 0) {
        size_t m = rep.size() < (size_t)outSize - 1 ? rep.size() : (size_t)outSize - 1;
        memcpy(out, rep.data(), m);
        out[m] = 0;
    }
    return fails;
}

}  // extern "C"
