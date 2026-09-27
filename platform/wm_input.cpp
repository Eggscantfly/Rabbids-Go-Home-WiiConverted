// wm_input.cpp - PC input source: keyboard, mouse (IR pointer) and XInput -> WmState (one 200 Hz sample).
#include "wiimote.h"

#include <cmath>
#include <cstring>

namespace {

struct XGamepad {
    WORD  wButtons;
    BYTE  bLeftTrigger;
    BYTE  bRightTrigger;
    SHORT sThumbLX, sThumbLY, sThumbRX, sThumbRY;
};
struct XState { DWORD dwPacketNumber; XGamepad Gamepad; };
struct XVibration { WORD wLeftMotorSpeed, wRightMotorSpeed; };
typedef DWORD (WINAPI* XGetStateFn)(DWORD, XState*);
typedef DWORD (WINAPI* XSetStateFn)(DWORD, XVibration*);

const float kPi = 3.14159265f;

const uint16_t kBtnBits[WB_COUNT] = {
    WPAD_BUTTON_A, WPAD_BUTTON_B, WPAD_BUTTON_Z, WPAD_BUTTON_C, WPAD_BUTTON_1, WPAD_BUTTON_2,
    WPAD_BUTTON_PLUS, WPAD_BUTTON_MINUS, WPAD_BUTTON_HOME,
    WPAD_BUTTON_UP, WPAD_BUTTON_DOWN, WPAD_BUTTON_LEFT, WPAD_BUTTON_RIGHT,
    0, 0, 0, 0, 0, 0, 0, 0, 0,
};

XGetStateFn s_xGet;
XSetStateFn s_xSet;
int         s_padIdx = -1;
bool        s_padOk;
double      s_padScanT = -100.0;
XState      s_pad;

HWND        s_hwnd;
double      s_hwndT = -100.0;
POINT       s_lastCursor;
bool        s_haveCursor;
int         s_ptrSource;                // 0 none yet, 1 mouse, 2 pad right stick
WmVec2      s_padPtr;

double      s_lastT = -1.0;
float       s_roll;
double      s_shakeStart = -1.0;
double      s_shakeEnd = -1.0;
bool        s_shakeHeld;
bool        s_connected = true;
bool        s_toggleHeld;
bool        s_centerHeld;

LONG        s_rumbleWanted;
LONG        s_rumbleApplied = -1;

CRITICAL_SECTION s_injCs;
bool        s_injInit;
bool        s_injActive;
WmTestInput s_inj;

struct FindCtx { DWORD pid; HWND best; LONGLONG area; };

BOOL CALLBACK EnumWindowsProc(HWND h, LPARAM lp) {
    FindCtx* c = (FindCtx*)lp;
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    if (pid != c->pid || !IsWindowVisible(h) || GetWindow(h, GW_OWNER) != NULL) return TRUE;
    RECT r;
    if (!GetClientRect(h, &r)) return TRUE;
    LONGLONG area = (LONGLONG)(r.right - r.left) * (LONGLONG)(r.bottom - r.top);
    if (area > c->area) {
        c->area = area;
        c->best = h;
    }
    return TRUE;
}

void RefreshWindow(double t) {
    if (s_hwnd && IsWindow(s_hwnd) && t - s_hwndT < 2.0) return;
    if (t - s_hwndT < 0.5) return;
    s_hwndT = t;
    FindCtx c = { GetCurrentProcessId(), NULL, 0 };
    EnumWindows(EnumWindowsProc, (LPARAM)&c);
    if (c.best != s_hwnd) {
        s_hwnd = c.best;
        Log("game window %p", (void*)c.best);
    }
}

bool HasFocus() {
    if (!g_cfg.requireFocus) return true;
    HWND f = GetForegroundWindow();
    if (!f) return false;
    DWORD pid = 0;
    GetWindowThreadProcessId(f, &pid);
    return pid == GetCurrentProcessId();
}

void PollPad(double t) {
    s_padOk = false;
    if (!s_xGet) return;
    if (s_padIdx >= 0) {
        if (s_xGet((DWORD)s_padIdx, &s_pad) == ERROR_SUCCESS) {
            s_padOk = true;
            return;
        }
        Log("XInput pad %d lost", s_padIdx);
        s_padIdx = -1;
        s_rumbleApplied = -1;
        s_padScanT = t;
        return;
    }
    if (t - s_padScanT < 1.0) return;       // XInputGetState on an empty port is slow: rescan once a second
    s_padScanT = t;
    int first = 0, last = 3;
    if (g_cfg.padIndex >= 0 && g_cfg.padIndex <= 3) first = last = g_cfg.padIndex;
    for (int i = first; i <= last; ++i) {
        if (s_xGet((DWORD)i, &s_pad) == ERROR_SUCCESS) {
            s_padIdx = i;
            s_padOk = true;
            Log("XInput pad %d in use", i);
            return;
        }
    }
}

WmVec2 Thumb(SHORT sx, SHORT sy, float dz) {
    float x = sx / 32767.0f, y = sy / 32767.0f;
    if (x < -1.0f) x = -1.0f;
    if (y < -1.0f) y = -1.0f;
    float m = (float)sqrt(x * x + y * y);
    WmVec2 r = { 0.0f, 0.0f };
    if (m <= dz || m <= 0.0f) return r;
    float mc = m > 1.0f ? 1.0f : m;
    float k = ((mc - dz) / (1.0f - dz)) / m;
    r.x = x * k;
    r.y = y * k;
    return r;
}

bool PadHas(int code) {
    if (!s_padOk || !code) return false;
    if (code == PAD_LT) return s_pad.Gamepad.bLeftTrigger > 30;
    if (code == PAD_RT) return s_pad.Gamepad.bRightTrigger > 30;
    return (s_pad.Gamepad.wButtons & code) != 0;
}

void ApplyRumble() {
    LONG want = s_rumbleWanted;
    if (!s_xSet || s_padIdx < 0 || want == s_rumbleApplied) return;
    XVibration v;
    v.wLeftMotorSpeed = v.wRightMotorSpeed = (WORD)(want ? 40000 : 0);
    s_xSet((DWORD)s_padIdx, &v);
    s_rumbleApplied = want;
}

inline float Clamp1(float v) { return v > 1.0f ? 1.0f : (v < -1.0f ? -1.0f : v); }

}  // namespace

void PcSourceInit() {
    InitializeCriticalSection(&s_injCs);
    s_injInit = true;
    s_connected = g_cfg.connectAtStart;
    if (g_cfg.xinput) {
        static const char* const dlls[] = { "xinput1_4.dll", "xinput1_3.dll", "xinput9_1_0.dll" };
        for (int i = 0; i < 3 && !s_xGet; ++i) {
            HMODULE m = LoadLibraryA(dlls[i]);
            if (!m) continue;
            s_xGet = (XGetStateFn)GetProcAddress(m, "XInputGetState");
            s_xSet = (XSetStateFn)GetProcAddress(m, "XInputSetState");
            if (s_xGet) Log("XInput: %s", dlls[i]);
        }
        if (!s_xGet) Log("XInput: not available");
    }
}

void PcSourceSetRumble(bool on) {
    InterlockedExchange(&s_rumbleWanted, (on && g_cfg.rumble) ? 1 : 0);
}

void PcSourceInject(const WmTestInput* in) {
    if (!s_injInit) return;
    EnterCriticalSection(&s_injCs);
    s_injActive = in != NULL;
    if (in) s_inj = *in;
    LeaveCriticalSection(&s_injCs);
}

void PcSourcePoll(double t, WmState& st) {
    double dt = s_lastT >= 0.0 ? t - s_lastT : 0.0;
    if (dt < 0.0 || dt > 0.25) dt = 0.0;
    s_lastT = t;

    WmTestInput inj;
    bool injected = false;
    if (s_injInit) {
        EnterCriticalSection(&s_injCs);
        injected = s_injActive;
        if (injected) inj = s_inj;
        LeaveCriticalSection(&s_injCs);
    }

    bool btn[WB_COUNT];
    memset(btn, 0, sizeof(btn));
    uint16_t buttons = 0;
    WmVec2 stick = { 0.0f, 0.0f };
    WmVec2 ptr = { 0.0f, 0.0f };
    bool ptrValid = false;

    if (injected) {
        buttons = inj.buttons;
        ptrValid = inj.pointerValid != 0;
        ptr.x = inj.px;
        ptr.y = inj.py;
        stick.x = inj.stickX;
        stick.y = inj.stickY;
        s_roll = inj.rollDeg * kPi / 180.0f;
        btn[WB_SHAKE] = inj.shake != 0;
    } else {
        RefreshWindow(t);
        bool focus = HasFocus();
        PollPad(t);
        bool padUse = s_padOk && focus;

        for (int b = 0; b < WB_COUNT; ++b) {
            for (int k = 0; k < MAX_BIND && !btn[b]; ++k) {
                int vk = g_cfg.keys[b][k];
                if (vk && focus && (GetAsyncKeyState(vk) & 0x8000)) btn[b] = true;
                if (padUse && PadHas(g_cfg.pad[b][k])) btn[b] = true;
            }
            if (btn[b]) buttons |= kBtnBits[b];
        }

        // Nunchuk stick: keys, replaced by the pad's left stick when it is deflected
        stick.x = (btn[WB_STICK_RIGHT] ? 1.0f : 0.0f) - (btn[WB_STICK_LEFT] ? 1.0f : 0.0f);
        stick.y = (btn[WB_STICK_UP] ? 1.0f : 0.0f) - (btn[WB_STICK_DOWN] ? 1.0f : 0.0f);
        if (padUse) {
            WmVec2 ls = Thumb(s_pad.Gamepad.sThumbLX, s_pad.Gamepad.sThumbLY, g_cfg.deadzone);
            if (ls.x != 0.0f || ls.y != 0.0f) stick = ls;
        }

        // IR pointer: the mouse inside the game window, or a pointer moved by the pad's right stick
        WmVec2 mpos = { 0.0f, 0.0f };
        bool inside = false, moved = false;
        if (g_cfg.mouse && s_hwnd && focus) {
            POINT p;
            if (GetCursorPos(&p)) {
                moved = s_haveCursor && (p.x != s_lastCursor.x || p.y != s_lastCursor.y);
                s_lastCursor = p;
                s_haveCursor = true;
                POINT c = p;
                RECT r;
                if (ScreenToClient(s_hwnd, &c) && GetClientRect(s_hwnd, &r) && r.right > 0 && r.bottom > 0 &&
                    c.x >= 0 && c.y >= 0 && c.x < r.right && c.y < r.bottom) {
                    inside = true;
                    mpos.x = ((float)c.x + 0.5f) / (float)r.right * 2.0f - 1.0f;
                    mpos.y = ((float)c.y + 0.5f) / (float)r.bottom * 2.0f - 1.0f;
                }
            }
        }
        if (inside && (moved || s_ptrSource == 0)) s_ptrSource = 1;
        if (g_cfg.padStick && padUse) {
            WmVec2 rs = Thumb(s_pad.Gamepad.sThumbRX, s_pad.Gamepad.sThumbRY, g_cfg.deadzone);
            if (rs.x != 0.0f || rs.y != 0.0f) {
                if (s_ptrSource == 1 && inside) s_padPtr = mpos;
                s_ptrSource = 2;
                s_padPtr.x = Clamp1(s_padPtr.x + rs.x * g_cfg.padSpeed * 2.0f * (float)dt);
                s_padPtr.y = Clamp1(s_padPtr.y - rs.y * g_cfg.padSpeed * 2.0f * (float)dt);
            }
        }
        if (btn[WB_POINTER_CENTER] && !s_centerHeld) {
            s_padPtr.x = s_padPtr.y = 0.0f;
            s_ptrSource = 2;
        }
        s_centerHeld = btn[WB_POINTER_CENTER];
        if (s_ptrSource == 1) {
            ptrValid = inside;
            ptr = mpos;
        } else if (s_ptrSource == 2) {
            ptrValid = true;
            ptr = s_padPtr;
        }

        // roll (tilt keys), ramped so KPAD's smoothed accelerometer horizon keeps agreeing with the IR dots
        float target = 0.0f;
        if (btn[WB_TILT_LEFT]) target -= g_cfg.tiltDeg * kPi / 180.0f;
        if (btn[WB_TILT_RIGHT]) target += g_cfg.tiltDeg * kPi / 180.0f;
        float step = g_cfg.tiltSpeedDeg * kPi / 180.0f * (float)dt;
        float diff = target - s_roll;
        if (diff > step) diff = step;
        if (diff < -step) diff = -step;
        s_roll += diff;

        if (btn[WB_CONNECT_TOGGLE] && !s_toggleHeld) {
            s_connected = !s_connected;
            Log("virtual remote %s", s_connected ? "connected" : "disconnected");
        }
        s_toggleHeld = btn[WB_CONNECT_TOGGLE];

        ApplyRumble();
    }

    if (!g_cfg.nunchuk) buttons &= (uint16_t)~(WPAD_BUTTON_Z | WPAD_BUTTON_C);
    float sm = (float)sqrt(stick.x * stick.x + stick.y * stick.y);
    if (sm > 1.0f) {
        stick.x /= sm;
        stick.y /= sm;
    }

    // shake: a sine burst while held, finished to a whole number of cycles on release
    bool shake = btn[WB_SHAKE];
    double period = 1.0 / (g_cfg.shakeHz > 0.5f ? g_cfg.shakeHz : 0.5f);
    if (shake) {
        if (s_shakeStart < 0.0) s_shakeStart = t;
        s_shakeEnd = -1.0;
    } else if (s_shakeHeld && s_shakeStart >= 0.0) {
        double cycles = ceil((t - s_shakeStart) / period);
        if (cycles < 1.0) cycles = 1.0;
        s_shakeEnd = s_shakeStart + cycles * period;
    }
    s_shakeHeld = shake;
    float sv = 0.0f;
    if (s_shakeStart >= 0.0) {
        if (s_shakeEnd >= 0.0 && t >= s_shakeEnd) {
            s_shakeStart = s_shakeEnd = -1.0;
        } else {
            sv = g_cfg.shakeG * (float)sin(2.0 * 3.14159265358979 * (t - s_shakeStart) / period);
        }
    }

    st.connected = s_connected;
    st.nunchuk = g_cfg.nunchuk;
    st.buttons = buttons;
    st.pointerValid = ptrValid && ScriptPointerOn(g_cfg.channel);     // IO_JoystickPointerStateSet (wm_script.cpp)
    st.pointer = ptr;
    st.roll = s_roll;
    st.dist = g_cfg.dist;
    st.stick = stick;
    // KPAD frame: at rest (buttons up, pointing at the screen) gravity reads (0, -1, 0); a roll of r reads
    // (-sin r, -cos r, 0), which KPAD turns into the horizon (cos r, sin r) the IR dots also show.
    const float SY = 0.24f, SZ = 0.97f;         // shake mostly along the remote's length (KPAD z)
    st.acc.x = -(float)sin(s_roll);
    st.acc.y = -(float)cos(s_roll);
    st.acc.z = 0.0f;
    st.fsAcc.x = 0.0f;
    st.fsAcc.y = -1.0f;
    st.fsAcc.z = 0.0f;
    if (g_cfg.shakeTarget != 1) {
        st.acc.y += SY * sv;
        st.acc.z += SZ * sv;
    }
    if (g_cfg.shakeTarget != 0) {
        st.fsAcc.y += SY * sv;
        st.fsAcc.z += SZ * sv;
    }
    st.rumble = s_rumbleWanted != 0;
}
