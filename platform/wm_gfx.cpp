// wm_gfx.cpp - the graphics settings of the Options screen (wm_options.cpp), applied while the game runs: display mode
// (window or borderless full screen), resolution, VSync, high detail, texture filtering and the Wii effects.
//
// Resolution.  The display draws at its K3D screen width and height (display +0x948 / +0x94C: K3D::SetScreenWidth /
// SetScreenHeight, which ViD::RenderOneViewpoint sets for each view).  At the start of every frame the display
// (0041ABC0) reads the window's client area (0041ABF3: call GetClientRect) and, when it changed, derives them from it
// (the viewport fitted to K3D_ScreenRatio / K3D_ScreenVSize, see wm_fixes.cpp) and stores the size in the renderer
// (0041A2D0), which presents that part of the back buffer onto the same part of the window (00486EF0: Present with
// source = destination = that size).  A resolution replaces the client area the display reads (the module's call at
// 0041ABF3) by the chosen height and the width of the window's shape, so the game renders at that size; Present
// (IDirect3DDevice9 vtable slot 17, wrapped) then gets no destination rectangle, which stretches the rendered part over
// the whole window.  The mouse follows: the executable's imports of GetCursorPos / SetCursorPos (0089D4A0 / 0089D4D8,
// used by the mouse update 006CC2A0 and IO_MouseSetPos 006CC540) convert between window and render pixels around the
// window's origin.
//
// Back buffer.  When the rendered size grows past the back buffer the display forces a device reset (0041B023:
// 00488090 with force 1), and the reset rebuilds the present parameters from the renderer's configuration (00487860:
// size +0x10 / +0x14, VSync +0x20), which keeps the size the game started with, so a larger window or resolution got
// a back buffer that was still too small.  The module's call at the start of the display frame (0041ABD0, the device
// check with force 0, right before the client area is read) raises the configured size first: to the monitor's size
// (one reset while a window is dragged larger), or the rendered size when that is larger (at most 8000: the display
// compares 13-bit sizes).  Windows can be dragged to any size.  Borderless full screen is a popup window over the
// monitor.  With /fullscreen (00A72370) the display keeps a fixed size: no window settings, no resolution.
//
// VSync.  The renderer's configuration +0x20 (0 off, 1 on; 00492BB0 turns it into the presentation interval) is read by
// the same reset.  A change sets it and forces one reset: the module's call at the start of the display frame passes
// force 1 once.  Each reset attempt waited 1000 ms (Sleep before IDirect3DDevice9::Reset in 00487140); the wait is
// 50 ms.  At start-up the command line option /vsync (00A72378) is set instead, before the device is created.
//
// High detail.  K3D_ForceNoLod_C (005EC770) stores its argument at display +0x69410: 1 draws every model at its best
// level of detail.  The module writes 1 every frame while the option is on (scripts may write 0 in between).
//
// Texture filtering.  IDirect3DDevice9::SetSamplerState (vtable slot 69, shared by every device of d3d9.dll) is
// wrapped: a linear minification filter becomes anisotropic with the chosen level (clamped to the device's
// MaxAnisotropy).
#include "wiimote.h"

#include <d3d9.h>

#include <cctype>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>

using namespace wmpatch;

namespace {

const uint32_t G_DISPLAY          = 0x009DE178;  // K3D_gpo_Display
const uint32_t DISPLAY_NO_LOD     = 0x69410;
const uint32_t G_RENDERER         = 0x00A6E834;  // the renderer: configuration, device +0x26F0
const uint32_t R_WIDTH = 0x10, R_HEIGHT = 0x14, R_VSYNC = 0x20, R_DEVICE = 0x26F0;
const uint32_t G_HWND             = 0x009DE000;  // the game window (createMainWindow)
const uint32_t G_OPT_VSYNC        = 0x00A72378;  // /vsync, read when the device is created (0041A620)
const uint32_t G_OPT_FULLSCREEN   = 0x00A72370;  // /fullscreen: the display keeps a fixed size
const int      MAX_BACK_BUFFER    = 8000;        // the display compares 13-bit sizes (0041AFF0)
const uint32_t CALL_DEVICE_CHECK  = 0x0041ABD0;  // display frame start: call 00488090 (renderer, force)
const uint32_t DEVICE_CHECK       = 0x00488090;
const uint32_t CALL_CLIENT_RECT   = 0x0041ABF3;  // display frame start: call [GetClientRect] (FF 15, 6 bytes)
const uint32_t IAT_GET_CLIENT_RECT = 0x0089D4F4;
const uint32_t IAT_GET_CURSOR_POS = 0x0089D4A0;
const uint32_t IAT_SET_CURSOR_POS = 0x0089D4D8;
const uint32_t RESET_SLEEP_ARG    = 0x00487151;  // imm32 of "push 1000; call Sleep" before each Reset attempt
const int      PRESENT_SLOT       = 17;          // IDirect3DDevice9::Present
const uint32_t PRESENT_ROUTINE    = 0x00486EF0;  // the renderer's present: [device vtable+0x44] (Present) twice
const uint32_t PRESENT_SITE_A     = 0x00486F37;  // 8B 52 44 51 51: mov edx,[edx+44h]; push ecx; push ecx
const uint32_t PRESENT_SITE_B     = 0x00486F46;  // 8B 51 44 6A 00: mov edx,[ecx+44h]; push 0
const int      RESET_SLOT         = 16;          // IDirect3DDevice9::Reset
const int      SAMPLER_SLOT       = 69;          // IDirect3DDevice9::SetSamplerState

struct Code { uint32_t va, len, crc; const char* what; };
const Code kCode[] = {
    { 0x00488090, 0x092, 0x994FB737, "device check / forced reset" },
    { 0x00487140, 0x040, 0x6C47F0A1, "device reset loop" },
    { 0x00487860, 0x0DA, 0x49C59C11, "present parameters from the renderer configuration" },
    { 0x0041ABC0, 0x040, 0x515D8BFB, "display frame start" },
    { 0x0041AC21, 0x006, 0xBAB5D6FE, "display frame start: /fullscreen keeps a fixed size" },
    { 0x0041AFF0, 0x038, 0x28E1C5F7, "display frame start: reset when the window outgrows the back buffer" },
    { 0x00486EF0, 0x060, 0x651EF973, "present (windowed: the client-size part of the back buffer)" },
    { 0x00492BB0, 0x028, 0x4D874CEA, "VSync -> presentation interval" },
    { 0x005EC770, 0x011, 0xC5E4CD8B, "K3D_ForceNoLod_C" },
    { 0x00409800, 0x040, 0x3D0EF77B, "createMainWindow" },
    { 0x0041A620, 0x040, 0xAA44157E, "display device creation" },
    { 0x006CC2A0, 0x0DA, 0x78B30402, "mouse update (GetCursorPos, client origin, SetCursorPos)" },
    { 0x006CC540, 0x09F, 0xA8C47C37, "IO_MouseSetPos (SetCursorPos)" },
};
const uint32_t SITE_CRC = 0xC7D11ADB;            // [0041ABD0 - 0x10, + 0x10)
const uint32_t CLIENT_RECT_SITE_CRC = 0x56941D62;  // [0041ABF3 - 0x10, + 0x10)

std::vector<std::string> s_pending;
bool s_inAttach;
bool s_installed;                                // device check, reset wait
bool s_scaleInstalled;                           // client area call, cursor imports (the resolution)
bool s_resetPending;
int  s_aniso;                                    // 0 = the game's filters
bool s_highDetail, s_highDetailWas;
bool s_deviceHooked, s_deviceFailed;
UINT s_maxAniso = 16;

// resolution: 0 = the window's size; the sizes the display was given last (main thread)
int  s_resolution;
bool s_scaled;
int  s_clientW, s_clientH, s_renderW, s_renderH;

typedef HRESULT (STDMETHODCALLTYPE* SetSamplerFn)(IDirect3DDevice9*, DWORD, D3DSAMPLERSTATETYPE, DWORD);
typedef HRESULT (STDMETHODCALLTYPE* PresentFn)(IDirect3DDevice9*, const RECT*, const RECT*, HWND, const RGNDATA*);
typedef HRESULT (STDMETHODCALLTYPE* ResetFn)(IDirect3DDevice9*, D3DPRESENT_PARAMETERS*);
ResetFn s_origResetGfx;
GfxOverlay s_overlay;                            // wm_lua.cpp: drawn at the end of every scene
GfxOverlay s_hud;                                // wm_sm64.cpp: SM64's HUD, under it
GfxOverlay s_top;                                // wm_battle.cpp: over both - a battle covers the whole picture
SetSamplerFn s_origSampler;
bool s_presentPatched;                           // the present routine calls PresentHook
void** s_hookedVt;                               // the device vtable the sampler / reset hooks were put in

// window state for borderless full screen
bool  s_borderless;
LONG  s_savedStyle;
RECT  s_savedRect;

void GfxLog(const char* fmt, ...) {
    char buf[600];
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf(buf, sizeof(buf) - 1, fmt, ap);
    buf[sizeof(buf) - 1] = 0;
    va_end(ap);
    if (s_inAttach) s_pending.push_back(buf);
    else Log("GFX: %s", buf);
}

HWND Window() { return *(HWND*)(uintptr_t)G_HWND; }
}  // namespace
HWND GfxWindow() { return Window(); }
namespace {
uint8_t* Renderer() { return *(uint8_t**)(uintptr_t)G_RENDERER; }

bool MonitorRect(HWND w, RECT& r, bool work) {
    HMONITOR m = MonitorFromWindow(w, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi;
    mi.cbSize = sizeof(mi);
    if (!m || !GetMonitorInfoA(m, &mi)) return false;
    r = work ? mi.rcWork : mi.rcMonitor;
    return true;
}

// the size the game renders at for a client area: the chosen height, the width of the window's shape (even)
bool RenderSize(int cw, int ch, int& rw, int& rh) {
    rw = cw;
    rh = ch;
    if (s_resolution <= 0 || cw <= 0 || ch <= 0 || *(int32_t*)(uintptr_t)G_OPT_FULLSCREEN) return false;
    rh = s_resolution;
    rw = (int)floor((double)cw * rh / ch + 0.5);
    rw += rw & 1;
    if (rw > MAX_BACK_BUFFER) rw = MAX_BACK_BUFFER;
    if (rh > MAX_BACK_BUFFER) rh = MAX_BACK_BUFFER;
    if (rw < 16) rw = 16;
    return rw != cw || rh != ch;
}

// the configured back buffer size (used by the next reset) holds what the display renders: raised to the monitor's
// size, or the rendered size when that is larger
void RaiseBackBuffer(uint8_t* r) {
    HWND hwnd = Window();
    RECT rc, mon;
    if (!r || !hwnd || !GetClientRect(hwnd, &rc)) return;
    int w, h;
    RenderSize(rc.right - rc.left, rc.bottom - rc.top, w, h);
    uint32_t cw = *(uint32_t*)(r + R_WIDTH), ch = *(uint32_t*)(r + R_HEIGHT);
    if (w <= 0 || h <= 0 || ((uint32_t)w <= cw && (uint32_t)h <= ch)) return;
    int nw = w, nh = h;
    if (MonitorRect(hwnd, mon, false)) {
        if (mon.right - mon.left > nw) nw = mon.right - mon.left;
        if (mon.bottom - mon.top > nh) nh = mon.bottom - mon.top;
    }
    if ((uint32_t)nw < cw) nw = (int)cw;
    if ((uint32_t)nh < ch) nh = (int)ch;
    if (nw > MAX_BACK_BUFFER) nw = MAX_BACK_BUFFER;
    if (nh > MAX_BACK_BUFFER) nh = MAX_BACK_BUFFER;
    *(uint32_t*)(r + R_WIDTH) = (uint32_t)nw;
    *(uint32_t*)(r + R_HEIGHT) = (uint32_t)nh;
    GfxLog("back buffer %ux%u -> %dx%d for %dx%d", cw, ch, nw, nh, w, h);
}

typedef int (__thiscall* DeviceCheckFn)(void* renderer, int force);

int __fastcall DeviceCheckHook(void* renderer, void* /*edx*/, int force) {
    if (!*(int32_t*)(uintptr_t)G_OPT_FULLSCREEN) RaiseBackBuffer((uint8_t*)renderer);
    if (s_resetPending) {
        s_resetPending = false;
        force = 1;
        GfxLog("device reset (%s)", "settings changed");
    }
    return ((DeviceCheckFn)(uintptr_t)DEVICE_CHECK)(renderer, force);
}

// the display's client area: the rendered size
BOOL WINAPI ClientRectHook(HWND hwnd, LPRECT rc) {
    BOOL ok = GetClientRect(hwnd, rc);
    if (!ok || !rc) return ok;
    int cw = rc->right - rc->left, ch = rc->bottom - rc->top, rw, rh;
    bool scaled = RenderSize(cw, ch, rw, rh);
    if (scaled != s_scaled || (scaled && (rw != s_renderW || rh != s_renderH))) {
        if (scaled) GfxLog("rendering at %dx%d, stretched over the %dx%d window", rw, rh, cw, ch);
        else GfxLog("rendering at the window's size (%dx%d)", cw, ch);
    }
    s_clientW = cw;
    s_clientH = ch;
    s_renderW = rw;
    s_renderH = rh;
    s_scaled = scaled;
    if (scaled) {
        rc->left = rc->top = 0;
        rc->right = rw;
        rc->bottom = rh;
    }
    return ok;
}

bool WindowOrigin(POINT& o) {
    HWND hwnd = Window();
    o.x = o.y = 0;
    return hwnd && ClientToScreen(hwnd, &o) && s_clientW > 0 && s_clientH > 0 && s_renderW > 0 && s_renderH > 0;
}

BOOL WINAPI GetCursorPosHook(LPPOINT p) {
    BOOL ok = GetCursorPos(p);
    POINT o;
    if (ok && p && s_scaled && WindowOrigin(o)) {
        p->x = o.x + MulDiv(p->x - o.x, s_renderW, s_clientW);
        p->y = o.y + MulDiv(p->y - o.y, s_renderH, s_clientH);
    }
    return ok;
}

BOOL WINAPI SetCursorPosHook(int x, int y) {
    POINT o;
    if (s_scaled && WindowOrigin(o)) {
        x = o.x + MulDiv(x - o.x, s_clientW, s_renderW);
        y = o.y + MulDiv(y - o.y, s_clientH, s_renderH);
    }
    return SetCursorPos(x, y);
}

void DrawOverlay(IDirect3DDevice9* dev, const RECT* src);
RECT s_picture;                                  // the part of the back buffer the engine presents (the picture)

HRESULT STDMETHODCALLTYPE PresentHook(IDirect3DDevice9* dev, const RECT* src, const RECT* dst, HWND wnd,
                                      const RGNDATA* dirty) {
    if (s_overlay.draw || s_hud.draw || s_top.draw) DrawOverlay(dev, src);
    if (s_scaled && src) dst = NULL;                 // the rendered part stretched over the whole window
    PresentFn real = (PresentFn)(*(void***)dev)[PRESENT_SLOT];     // whatever the device's vtable holds now
    return real(dev, src, dst, wnd, dirty);
}

// the present routine's two "mov edx, [vtable+44h]" become calls of stubs that put PresentHook in edx instead
bool PatchPresentRoutine() {
    static const uint8_t wantA[5] = { 0x8B, 0x52, 0x44, 0x51, 0x51 }, wantB[5] = { 0x8B, 0x51, 0x44, 0x6A, 0x00 };
    ProcessImage img;
    uint8_t a[5], b[5];
    if (!img.Read(PRESENT_SITE_A, a, 5) || !img.Read(PRESENT_SITE_B, b, 5)) return false;
    if (memcmp(a, wantA, 5) != 0 || memcmp(b, wantB, 5) != 0) return false;
    uint8_t* stubs = (uint8_t*)VirtualAlloc(NULL, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!stubs) return false;
    uint32_t hook = (uint32_t)(uintptr_t)&PresentHook;
    // stub A: add esp, 4 (the call's return address); push ecx; push ecx; mov edx, hook; jmp 00486F3C
    uint8_t* p = stubs;
    const uint8_t sa[] = { 0x83, 0xC4, 0x04, 0x51, 0x51, 0xBA };
    memcpy(p, sa, sizeof(sa));
    p += sizeof(sa);
    memcpy(p, &hook, 4);
    p += 4;
    *p++ = 0xE9;
    int32_t rel = (int32_t)((PRESENT_SITE_A + 5) - ((uint32_t)(uintptr_t)p + 4));
    memcpy(p, &rel, 4);
    p += 4;
    // stub B: add esp, 4; push 0; mov edx, hook; jmp 00486F4B
    uint8_t* stubB = p;
    const uint8_t sb[] = { 0x83, 0xC4, 0x04, 0x6A, 0x00, 0xBA };
    memcpy(p, sb, sizeof(sb));
    p += sizeof(sb);
    memcpy(p, &hook, 4);
    p += 4;
    *p++ = 0xE9;
    rel = (int32_t)((PRESENT_SITE_B + 5) - ((uint32_t)(uintptr_t)p + 4));
    memcpy(p, &rel, 4);
    uint8_t callA[5] = { 0xE8 }, callB[5] = { 0xE8 };
    rel = (int32_t)((uint32_t)(uintptr_t)stubs - (PRESENT_SITE_A + 5));
    memcpy(callA + 1, &rel, 4);
    rel = (int32_t)((uint32_t)(uintptr_t)stubB - (PRESENT_SITE_B + 5));
    memcpy(callB + 1, &rel, 4);
    return WriteCode(PRESENT_SITE_A, callA, 5) && WriteCode(PRESENT_SITE_B, callB, 5);
}

IDirect3DStateBlock9* s_overlayState;            // the device state around the overlay (default pool: reset-aware)

void DrawOverlay(IDirect3DDevice9* dev, const RECT* src) {
    // on the back buffer, inside a scene of its own (Present is called outside the engine's scenes), with the whole
    // buffer as the viewport, no scissor, no depth test; the engine's state is put back afterwards
    IDirect3DSurface9* back = NULL;
    if (FAILED(dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &back)) || !back) return;
    if (!s_overlayState && FAILED(dev->CreateStateBlock(D3DSBT_ALL, &s_overlayState))) s_overlayState = NULL;
    if (s_overlayState) s_overlayState->Capture();
    IDirect3DSurface9* was = NULL;
    IDirect3DSurface9* depth = NULL;
    dev->GetRenderTarget(0, &was);
    dev->GetDepthStencilSurface(&depth);
    if (was != back) dev->SetRenderTarget(0, back);
    dev->SetDepthStencilSurface(NULL);
    D3DSURFACE_DESC desc;
    if (SUCCEEDED(back->GetDesc(&desc))) {
        D3DVIEWPORT9 vp = { 0, 0, desc.Width, desc.Height, 0.0f, 1.0f };
        dev->SetViewport(&vp);
        RECT whole = { 0, 0, (LONG)desc.Width, (LONG)desc.Height };
        s_picture = src ? *src : whole;
    }
    dev->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
    dev->SetRenderState(D3DRS_ZENABLE, FALSE);
    HRESULT hs = dev->BeginScene();
    static int logged = 0;
    if (SUCCEEDED(hs)) {
        if (s_hud.draw) s_hud.draw(dev, s_picture);       // part of the picture: the Lua layer draws over it
        if (s_overlay.draw) s_overlay.draw(dev, s_picture);
        if (s_top.draw) s_top.draw(dev, s_picture);
        dev->EndScene();
    } else if (logged++ < 3) {
        GfxLog("overlay: BeginScene failed (%08X): the scene is still open at Present", (unsigned)hs);
    }
    if (was && was != back) dev->SetRenderTarget(0, was);
    dev->SetDepthStencilSurface(depth);
    if (s_overlayState) s_overlayState->Apply();
    if (depth) depth->Release();
    if (was) was->Release();
    back->Release();
}

HRESULT STDMETHODCALLTYPE ResetHookGfx(IDirect3DDevice9* dev, D3DPRESENT_PARAMETERS* pp) {
    if (s_overlayState) { s_overlayState->Release(); s_overlayState = NULL; }
    if (s_overlay.lost) s_overlay.lost();
    if (s_hud.lost) s_hud.lost();
    if (s_top.lost) s_top.lost();
    HRESULT hr = s_origResetGfx(dev, pp);
    if (SUCCEEDED(hr)) {
        if (s_overlay.restored) s_overlay.restored();
        if (s_hud.restored) s_hud.restored();
        if (s_top.restored) s_top.restored();
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE SetSamplerHook(IDirect3DDevice9* dev, DWORD sampler, D3DSAMPLERSTATETYPE type, DWORD value) {
    if (s_aniso > 1 && type == D3DSAMP_MINFILTER && value == D3DTEXF_LINEAR) {
        s_origSampler(dev, sampler, D3DSAMP_MAXANISOTROPY, (DWORD)((UINT)s_aniso < s_maxAniso ? s_aniso : s_maxAniso));
        value = D3DTEXF_ANISOTROPIC;
    }
    return s_origSampler(dev, sampler, type, value);
}

bool HookSlot(void** vt, int slot, void* hook, void** orig) {
    if (vt[slot] == hook) return true;
    *orig = vt[slot];
    return WriteCode((uint32_t)(uintptr_t)&vt[slot], &hook, 4);
}

void HookDevice() {
    uint8_t* r = Renderer();
    IDirect3DDevice9* dev = r ? *(IDirect3DDevice9**)(r + R_DEVICE) : NULL;
    if (!dev) return;
    void** vt = *(void***)dev;
    D3DCAPS9 caps;
    if (SUCCEEDED(dev->GetDeviceCaps(&caps)) && caps.MaxAnisotropy >= 1) s_maxAniso = caps.MaxAnisotropy;
    void* origSampler = s_origSampler;
    bool sampler = HookSlot(vt, SAMPLER_SLOT, (void*)&SetSamplerHook, &origSampler);
    s_origSampler = (SetSamplerFn)origSampler;
    bool present = s_presentPatched || (s_presentPatched = PatchPresentRoutine());
    void* origReset = s_origResetGfx;
    bool reset = HookSlot(vt, RESET_SLOT, (void*)&ResetHookGfx, &origReset);
    s_origResetGfx = (ResetFn)origReset;
    if (!reset) GfxLog("the Reset hook (overlay) is NOT installed");
    s_hookedVt = vt;
    s_deviceHooked = true;
    s_deviceFailed = !sampler || !present;
    static int logs = 0;
    if (logs++ < 1)
        GfxLog("device hooks: texture filtering %s (anisotropy up to %u), present routine %s", sampler ? "installed" :
               "NOT installed", s_maxAniso, present ? "patched" : "NOT patched");
}

bool ImportIs(uint32_t slot, const char* module, const char* name) {
    HMODULE m = GetModuleHandleA(module);
    void* fn = m ? (void*)GetProcAddress(m, name) : NULL;
    return fn && *(void**)(uintptr_t)slot == fn;
}

}  // namespace

// ---------------------------------------------------------------------------------------------------------------
// interface (wiimote.h)
// ---------------------------------------------------------------------------------------------------------------
int GfxVerify(const Image& img, std::string& rep) {
    int bad = 0;
    uint32_t got = 0;
    for (size_t i = 0; i < sizeof(kCode) / sizeof(kCode[0]); ++i) {
        bool ok = CheckCrc(img, kCode[i].va, kCode[i].len, kCode[i].crc, &got);
        Report(rep, ok, "%-56s %08X len 0x%03X crc %08X (want %08X)", kCode[i].what, kCode[i].va, kCode[i].len, got,
               kCode[i].crc);
        if (!ok) ++bad;
    }
    uint8_t call[6] = { 0 };
    int32_t rel = 0;
    bool site = img.Read(CALL_DEVICE_CHECK, call, 5) && call[0] == 0xE8 &&
                (memcpy(&rel, call + 1, 4), CALL_DEVICE_CHECK + 5 + (uint32_t)rel == DEVICE_CHECK) &&
                CheckCrc(img, CALL_DEVICE_CHECK - 0x10, 0x20, SITE_CRC, &got);
    Report(rep, site, "%-56s call at %08X -> %08X, crc %08X (want %08X)", "display frame start -> device check",
           CALL_DEVICE_CHECK, DEVICE_CHECK, got, SITE_CRC);
    if (!site) ++bad;
    uint32_t slot = 0;
    bool rect = img.Read(CALL_CLIENT_RECT, call, 6) && call[0] == 0xFF && call[1] == 0x15 &&
                (memcpy(&slot, call + 2, 4), slot == IAT_GET_CLIENT_RECT) &&
                CheckCrc(img, CALL_CLIENT_RECT - 0x10, 0x20, CLIENT_RECT_SITE_CRC, &got);
    Report(rep, rect, "%-56s call [%08X] at %08X, crc %08X (want %08X)", "display frame start -> GetClientRect",
           IAT_GET_CLIENT_RECT, CALL_CLIENT_RECT, got, CLIENT_RECT_SITE_CRC);
    if (!rect) ++bad;
    uint32_t sleepMs = 0;
    bool wait = img.Read(RESET_SLEEP_ARG, &sleepMs, 4) && sleepMs == 1000;
    Report(rep, wait, "%-56s %08X = %u (want 1000)", "wait before each device reset attempt", RESET_SLEEP_ARG, sleepMs);
    if (!wait) ++bad;
    return bad;
}

void GfxAttach() {
    s_inAttach = true;
    ProcessImage img;
    uint8_t probe[5];
    if (!img.Read(CALL_DEVICE_CHECK, probe, 5)) {
        GfxLog("host process is not the RGH PC executable: graphics settings not installed");
        s_inAttach = false;
        return;
    }
    std::string rep;
    int bad = GfxVerify(img, rep);
    if (bad) {
        GfxLog("the PC executable differs from the expected 2010 build in %d place(s): graphics settings NOT installed",
               bad);
        size_t pos = 0;
        while (pos < rep.size()) {
            size_t end = rep.find('\n', pos);
            std::string line = rep.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
            if (line.find("FAIL") != std::string::npos) GfxLog("%s", line.c_str());
            pos = end == std::string::npos ? rep.size() : end + 1;
        }
        s_inAttach = false;
        return;
    }
    int32_t rel = (int32_t)((uint32_t)(uintptr_t)&DeviceCheckHook - (CALL_DEVICE_CHECK + 5));
    uint32_t shortWait = 50;
    s_installed = WriteCode(CALL_DEVICE_CHECK + 1, &rel, 4) && WriteCode(RESET_SLEEP_ARG, &shortWait, 4);
    // the resolution: the display's client area call and the cursor imports
    bool imports = ImportIs(IAT_GET_CLIENT_RECT, "user32.dll", "GetClientRect") &&
                   ImportIs(IAT_GET_CURSOR_POS, "user32.dll", "GetCursorPos") &&
                   ImportIs(IAT_SET_CURSOR_POS, "user32.dll", "SetCursorPos");
    if (s_installed && imports) {
        uint8_t call[6] = { 0xE8, 0, 0, 0, 0, 0x90 };
        int32_t r2 = (int32_t)((uint32_t)(uintptr_t)&ClientRectHook - (CALL_CLIENT_RECT + 5));
        memcpy(call + 1, &r2, 4);
        void* getPos = (void*)&GetCursorPosHook, *setPos = (void*)&SetCursorPosHook;
        s_scaleInstalled = WriteCode(IAT_GET_CURSOR_POS, &getPos, 4) && WriteCode(IAT_SET_CURSOR_POS, &setPos, 4) &&
                           WriteCode(CALL_CLIENT_RECT, call, 6);
    }
    GfxLog("PC executable verified: graphics settings %s, resolution %s", s_installed ? "installed" : "NOT installed",
           s_scaleInstalled ? "installed" : imports ? "NOT installed" : "NOT installed (imports differ)");
    s_inAttach = false;
}

void GfxAfterConfig() {
    for (size_t i = 0; i < s_pending.size(); ++i) Log("GFX: %s", s_pending[i].c_str());
    s_pending.clear();
}

bool GfxInstalled() { return s_installed; }

RECT GfxPicture() { return s_picture; }

IDirect3DDevice9* GfxDevice() {
    uint8_t* r = Renderer();
    return r ? *(IDirect3DDevice9**)(r + R_DEVICE) : NULL;
}

void GfxSetOverlay(const GfxOverlay& overlay) { s_overlay = overlay; }
void GfxSetHud(const GfxOverlay& hud) { s_hud = hud; }
void GfxSetTop(const GfxOverlay& top) { s_top = top; }

bool GfxResolutionInstalled() { return s_installed && s_scaleInstalled; }

void GfxStartupVsync(int vsync) {
    if (s_installed && vsync >= 0) *(uint32_t*)(uintptr_t)G_OPT_VSYNC = vsync ? 1 : 0;
}

int GfxGameVsync() {
    std::string cmd = GetCommandLineA();
    for (size_t i = 0; i < cmd.size(); ++i) cmd[i] = (char)tolower((unsigned char)cmd[i]);
    size_t at = cmd.find("/vsync");
    if (at == std::string::npos) return 0;
    at += 6;
    if (at < cmd.size() && cmd[at] == ':') return atoi(cmd.c_str() + at + 1) ? 1 : 0;
    return 1;
}

int GfxVsync() {
    if (!s_installed) return 0;
    uint8_t* r = Renderer();
    if (r) return *(uint32_t*)(r + R_VSYNC) ? 1 : 0;
    return *(uint32_t*)(uintptr_t)G_OPT_VSYNC ? 1 : 0;
}

void GfxSetVsync(int on) {
    uint8_t* r = Renderer();
    if (!s_installed || !r) return;
    uint32_t v = on ? 1 : 0;
    if (*(uint32_t*)(r + R_VSYNC) == v) return;
    *(uint32_t*)(r + R_VSYNC) = v;
    s_resetPending = true;
    GfxLog("VSync %s", on ? "on" : "off");
}

bool GfxClientSize(int& w, int& h) {
    if (!s_installed) return false;
    HWND hwnd = Window();
    RECT rc;
    if (!hwnd || !GetClientRect(hwnd, &rc)) return false;
    w = rc.right - rc.left;
    h = rc.bottom - rc.top;
    return w > 0 && h > 0;
}

bool GfxRenderSize(int height, int& w, int& h) {
    int cw, ch;
    if (!GfxClientSize(cw, ch)) return false;
    int keep = s_resolution;
    s_resolution = height;
    RenderSize(cw, ch, w, h);
    s_resolution = keep;
    return true;
}

bool GfxMonitorSize(int& w, int& h, bool work) {
    if (!s_installed) return false;
    HWND hwnd = Window();
    RECT r;
    if (!hwnd || !MonitorRect(hwnd, r, work)) return false;
    w = r.right - r.left;
    h = r.bottom - r.top;
    return true;
}

bool GfxBorderless() { return s_borderless; }

bool GfxWindowed() { return s_installed && !*(int32_t*)(uintptr_t)G_OPT_FULLSCREEN; }

void GfxSetResolution(int height) {
    if (height < 0) height = 0;
    if (height == s_resolution) return;
    s_resolution = height;
    if (height) GfxLog("resolution %d lines", height);
    else GfxLog("resolution: the window's size");
}

void GfxSetBorderless(bool on) {
    if (!GfxWindowed() || on == s_borderless) return;
    HWND hwnd = Window();
    if (!hwnd) return;
    if (on) {
        RECT mon;
        if (!MonitorRect(hwnd, mon, false)) return;
        s_savedStyle = GetWindowLongA(hwnd, GWL_STYLE);
        GetWindowRect(hwnd, &s_savedRect);
        SetWindowLongA(hwnd, GWL_STYLE, (s_savedStyle & ~(WS_OVERLAPPEDWINDOW)) | WS_POPUP | WS_VISIBLE);
        SetWindowPos(hwnd, HWND_TOP, mon.left, mon.top, mon.right - mon.left, mon.bottom - mon.top,
                     SWP_FRAMECHANGED | SWP_NOACTIVATE);
        s_borderless = true;
        GfxLog("borderless full screen %ldx%ld", mon.right - mon.left, mon.bottom - mon.top);
    } else {
        SetWindowLongA(hwnd, GWL_STYLE, s_savedStyle ? s_savedStyle : (LONG)(WS_OVERLAPPEDWINDOW | WS_VISIBLE));
        SetWindowPos(hwnd, HWND_NOTOPMOST, s_savedRect.left, s_savedRect.top, s_savedRect.right - s_savedRect.left,
                     s_savedRect.bottom - s_savedRect.top, SWP_FRAMECHANGED | SWP_NOACTIVATE);
        s_borderless = false;
        GfxLog("windowed");
    }
}

void GfxSetHighDetail(bool on) { s_highDetail = on; }
void GfxSetAniso(int level) { s_aniso = level; }

void GfxFrame() {
    if (!s_installed) return;
    uint8_t* disp = *(uint8_t**)(uintptr_t)G_DISPLAY;
    if (disp) {
        if (s_highDetail) *(int32_t*)(disp + DISPLAY_NO_LOD) = 1;
        else if (s_highDetailWas) *(int32_t*)(disp + DISPLAY_NO_LOD) = 0;
        s_highDetailWas = s_highDetail;
    }
    if (!s_deviceHooked) HookDevice();
    else {
        // d3d9 gives the device a fresh vtable copy at Reset: the vtable hooks go back in when the copy changed
        IDirect3DDevice9* dev = GfxDevice();
        if (dev && *(void***)dev != s_hookedVt) {
            static int rehooks = 0;
            if (rehooks++ < 3) GfxLog("the device's vtable changed (%p -> %p): hooks put back", (void*)s_hookedVt, *(void**)dev);
            HookDevice();
        }
    }
}
