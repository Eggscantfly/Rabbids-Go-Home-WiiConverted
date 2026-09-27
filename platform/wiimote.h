// wiimote.dll for the Rabbids Go Home PC executable - shared declarations.
//
// The PC executable has two Wii remote paths (see docs/platform.md):
//   * the game's own input layer (script ids 20.., pointer/accelerometer natives) runs the RVL KPAD library
//     compiled into the exe on top of a WPAD layer that is a TCP client of 127.0.0.1:4242.  This DLL hosts
//     that WPAD server and streams a virtual Wii remote + Nunchuk at 200 Hz.  This is the path the game uses.
//   * Gear::Input::GamePadWii calls the Wrap* exports of wiimote.dll.  The game ignores those devices (its connect
//     callback only accepts "GamePadWiiDevkit"), so the exports are implemented for completeness only.
//
// The input side is a source of WmState (what a real remote measures, in KPAD units).  PcSource builds it from
// keyboard/mouse/XInput; a Bluetooth HID source can later fill the same structure (or raw WpadRaw directly).
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <stdint.h>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------------------------------------------
// RVL library values used by the executable's KPAD/WPAD code
// ---------------------------------------------------------------------------------------------------------------
enum : uint16_t {
    WPAD_BUTTON_LEFT  = 0x0001,     // D-pad, remote held vertically
    WPAD_BUTTON_RIGHT = 0x0002,
    WPAD_BUTTON_DOWN  = 0x0004,
    WPAD_BUTTON_UP    = 0x0008,
    WPAD_BUTTON_PLUS  = 0x0010,
    WPAD_BUTTON_2     = 0x0100,
    WPAD_BUTTON_1     = 0x0200,
    WPAD_BUTTON_B     = 0x0400,
    WPAD_BUTTON_A     = 0x0800,
    WPAD_BUTTON_MINUS = 0x1000,
    WPAD_BUTTON_Z     = 0x2000,     // Nunchuk Z (WPAD puts the Nunchuk buttons in the core button word)
    WPAD_BUTTON_C     = 0x4000,     // Nunchuk C
    WPAD_BUTTON_HOME  = 0x8000,
};

enum {
    WPAD_DEV_CORE      = 0,
    WPAD_DEV_FREESTYLE = 1,         // Nunchuk attached
    WPAD_DEV_NOT_FOUND = 0xFD,
};

enum {
    WPAD_FMT_CORE              = 0,
    WPAD_FMT_CORE_ACC          = 1,
    WPAD_FMT_CORE_ACC_DPD      = 2,
    WPAD_FMT_FREESTYLE         = 3,
    WPAD_FMT_FREESTYLE_ACC     = 4,
    WPAD_FMT_FREESTYLE_ACC_DPD = 5,
};

// Virtual buttons of the mapping (index into Config::keys / Config::pad)
enum WmBtn {
    WB_A, WB_B, WB_Z, WB_C, WB_1, WB_2, WB_PLUS, WB_MINUS, WB_HOME,
    WB_DUP, WB_DDOWN, WB_DLEFT, WB_DRIGHT,
    WB_STICK_UP, WB_STICK_DOWN, WB_STICK_LEFT, WB_STICK_RIGHT,
    WB_SHAKE, WB_TILT_LEFT, WB_TILT_RIGHT, WB_POINTER_CENTER, WB_CONNECT_TOGGLE,
    WB_COUNT
};
extern const char* const g_btnNames[WB_COUNT];

// ---------------------------------------------------------------------------------------------------------------
// Device-independent remote state
// ---------------------------------------------------------------------------------------------------------------
struct WmVec2 { float x, y; };
struct WmVec3 { float x, y, z; };

struct WmState {
    bool     connected;
    bool     nunchuk;
    uint16_t buttons;       // WPAD_BUTTON_*
    WmVec3   acc;           // remote acceleration, KPAD frame, g (at rest, buttons up: 0,-1,0)
    bool     pointerValid;  // sensor bar visible
    WmVec2   pointer;       // KPAD pos: -1..1, (-1,-1) = top-left of the screen
    float    roll;          // radians, KPAD horizon = (cos, sin)
    float    dist;          // metres to the sensor bar
    WmVec2   stick;         // Nunchuk stick -1..1, +y = up
    WmVec3   fsAcc;         // Nunchuk acceleration, KPAD frame, g
    bool     rumble;
};

// Raw WPAD sample as the engine's WPAD client stores it (WPADFSStatus, 50 bytes)
#pragma pack(push, 1)
struct WpadObj {
    int16_t  x, y;          // 0..1023, 0..767 camera pixels
    uint16_t size;          // 0 = no object
    uint8_t  traceId;
    uint8_t  pad;           // not transmitted
};
struct WpadRaw {
    uint16_t button;
    int16_t  accX, accY, accZ;
    WpadObj  obj[4];
    uint8_t  dev;
    int8_t   err;
    int16_t  fsAccX, fsAccY, fsAccZ;
    int8_t   fsStickX, fsStickY;
};
#pragma pack(pop)

// ---------------------------------------------------------------------------------------------------------------
// Configuration (wiimote.ini next to the DLL)
// ---------------------------------------------------------------------------------------------------------------
enum { MAX_BIND = 6 };

struct Config {
    // [general]
    bool   enabled        = true;
    bool   log            = false;
    bool   logVerbose     = false;
    int    channel        = 0;
    bool   nunchuk        = true;
    int    port           = 4242;
    int    rateHz         = 200;
    bool   requireFocus   = true;
    bool   gearPad        = false;
    bool   rumble         = true;
    bool   connectAtStart = true;
    // [pointer]
    bool   mouse          = true;
    bool   padStick       = true;
    float  padSpeed       = 1.4f;
    float  dist           = 2.0f;
    int    sensorBar      = 0;
    bool   centerAuto     = true;
    float  centerY        = 0.2f;
    // [motion]
    float  shakeG         = 3.0f;
    float  shakeHz        = 6.0f;
    int    shakeTarget    = 0;      // 0 remote, 1 nunchuk, 2 both
    float  tiltDeg        = 35.0f;
    float  tiltSpeedDeg   = 90.0f;
    // [keys]  virtual-key codes per virtual button
    int    keys[WB_COUNT][MAX_BIND] = {};
    // [xinput]
    bool   xinput         = true;
    int    padIndex       = -1;
    float  deadzone       = 0.25f;
    int    pad[WB_COUNT][MAX_BIND] = {};   // XINPUT button bits, plus PAD_LT / PAD_RT pseudo bits
    // [save]  (read again in DllMain by the save layer, see wm_sav.cpp)
    bool   savEnabled          = true;
    bool   savChannelInstalled = true;
};

enum { PAD_LT = 0x10000, PAD_RT = 0x20000 };

extern Config      g_cfg;
extern std::string g_dllDir;

void LoadConfig(const std::string& iniPath, Config& cfg);
void Log(const char* fmt, ...);
void LogOpen();
double NowSeconds();
void WmEnsureInit();                            // one-time init (config, log, WPAD server)

// ---------------------------------------------------------------------------------------------------------------
// Wii save natives (wm_sav.cpp)
// ---------------------------------------------------------------------------------------------------------------
void SavAttach(const std::string& iniPath);     // DllMain: verify the PC executable and install the SAV handlers
void SavAfterConfig();                          // after LoadConfig: flush the DllMain log lines

// ---------------------------------------------------------------------------------------------------------------
// Controls mode (wm_ctl.cpp): [controls] mode=wii (virtual Wii remote) | pc (no virtual remote; the Wii pointer and
// motion natives are served from the engine's mouse state)
// ---------------------------------------------------------------------------------------------------------------
void CtlAttach(const std::string& iniPath);     // DllMain: read the mode; pc: verify the PC executable, replace natives
void CtlAfterConfig();                          // after LoadConfig: flush the DllMain log lines
bool CtlPcMode();                               // true: the virtual remote (input polling, WPAD server) is not started

// ---------------------------------------------------------------------------------------------------------------
// Video natives and instrumentation (wm_video.cpp): [video] natives=1 log=0
// ---------------------------------------------------------------------------------------------------------------
void VideoAttach(const std::string& iniPath);   // DllMain: verify, replace the stub natives, install the log hooks
void VideoAfterConfig();                        // after LoadConfig: flush the DllMain log lines
void VideoTick();                               // once per frame: close the log of videos that stopped updating

// ---------------------------------------------------------------------------------------------------------------
// Wii after effects (wm_afx.cpp): [video] afx=1 afx_dump=0
// ---------------------------------------------------------------------------------------------------------------
void AfxAttach(const std::string& iniPath);     // DllMain: verify, replace K3D::AFX_User with the Wii effects
void AfxAfterConfig();                          // after LoadConfig: flush the DllMain log lines
bool AfxInstalled();                            // the effects are drawn by this DLL ([video] afx=1, verified)
bool AfxUserEnabled();                          // the Options screen's switch (default on)
bool AfxDefaultEnabled();
void AfxSetUserEnabled(bool on);

// ---------------------------------------------------------------------------------------------------------------
// Frame pacing (wm_timing.cpp): [timing] fps_cap=60
// ---------------------------------------------------------------------------------------------------------------
void TimingAttach(const std::string& iniPath);  // DllMain: verify and hook the end of ViD::OneFrame
void TimingAfterConfig();                       // after LoadConfig: flush the DllMain log lines
bool TimingInstalled();
double TimingCap();                             // Hz, 0 = uncapped
double TimingIniCap();                          // [timing] fps_cap
void TimingSetCap(double hz);                   // live (the Options screen's FRAME RATE); below 10 = uncapped

// ---------------------------------------------------------------------------------------------------------------
// Doppler speeds of sound sources (wm_audio.cpp): [audio] doppler=wii|engine doppler_rate=60 doppler_log=0
// ---------------------------------------------------------------------------------------------------------------
void AudioAttach(const std::string& iniPath);   // DllMain: verify, replace the source velocity block
void AudioAfterConfig();                        // after LoadConfig: flush the DllMain log lines

// ---------------------------------------------------------------------------------------------------------------
// Script natives answered like the Wii executable (wm_script.cpp): [script] platform=auto|wii|pc pointer_state=1
// return_to_menu=quit
// ---------------------------------------------------------------------------------------------------------------
void ScriptAttach(const std::string& iniPath);  // DllMain, after CtlAttach: verify, replace the natives per word
void ScriptAfterConfig();                       // after LoadConfig: flush the DllMain log lines
bool ScriptPointerOn(int id);                   // false while the scripts switched controller id's pointer off

// ---------------------------------------------------------------------------------------------------------------
// Options screen of the pause menu (wm_options.cpp): [options] enabled=1; control bindings ([controls] mode=pc),
// volumes and graphics settings, saved in options.ini next to the DLL
// ---------------------------------------------------------------------------------------------------------------
void OptionsAttach(const std::string& iniPath); // DllMain, after CtlAttach: verify, hook input poll and Magma update
void OptionsAfterConfig();                      // after LoadConfig: flush the DllMain log lines
void HangWatchFrame();                          // wm_hang.cpp: once per frame on the main thread (hang watch)

// ---------------------------------------------------------------------------------------------------------------
// Wii remote motion in controls mode pc (wm_motion.cpp): the accelerometers and the horizon the Wii-only scripts read,
// built once per frame from the Options bindings (wm_options.cpp) and served by the natives of wm_ctl.cpp
// ---------------------------------------------------------------------------------------------------------------
enum { MOTION_AXES = 3 };                       // the remote's x (sideways), y (up and down), z (along it: maraca)
struct MotionInput {
    double t;                                   // seconds (NowSeconds)
    bool   on;                                  // the game reads the controls (not the Options page)
    bool   attack;                              // SCREAM: the remote and the Nunchuk shake up and down
    bool   shake[MOTION_AXES];
    double hz[MOTION_AXES];                     // the rhythm of the input shaking the axis (mouse, wheel), 0 = default
    bool   tiltLeft, tiltRight;
    int    rotate;                              // a circular shake: 1 clockwise as the player sees it, -1 the other way
    double rotateHz;                            // the rhythm of the input driving it (the mouse's circles), 0 = default
    bool   noPad;                               // no controller is connected: the keyboard stands in for the Nunchuk stick
    float  move[2];                             // the movement actions as a stick: x right, y forward, -1..1
};
void MotionFrame(const MotionInput& in);        // once per frame, after the input poll
bool MotionActive();                            // at least one frame was built (else the natives answer "at rest")
int  MotionSampleCount();                       // this frame's accelerometer samples (IO_JoystickSampleNumberGet)
void MotionAccel(int sensor, int sample, float out[3]);  // sensor 0 remote, 1 Nunchuk; sample 0 = the newest
void MotionHorizon(float out[2]);               // (cos roll, sin roll)
void MotionPolled();                            // a script read the remote's accelerometer samples this frame (the
                                                // shake detection of the Wii-only scripts; levels only read IO_JoystickAccelGet)
bool MotionStick(float out[2]);                 // the movement actions as controller 0's Nunchuk stick: only without
                                                // a controller and while such scripts run (polled within 0.5 s)

// ---------------------------------------------------------------------------------------------------------------
// Graphics settings of the Options screen (wm_gfx.cpp), applied live from wm_options.cpp
// ---------------------------------------------------------------------------------------------------------------
namespace wmpatch { struct Image; }
void FixesAttach();                             // wm_fixes.cpp, DllMain: widescreen, controller scan stutter
void FixesAfterConfig();
int  FixesVerify(const wmpatch::Image& img, std::string& rep);
void ModsAttach(const std::string& iniPath);    // wm_mods.cpp, DllMain: mods read from the game folder ([mods])
void ModsAfterConfig();
bool ModsInstalled();                           // the loader's hooks are in (an enabled mod gives files)
int  ModsVerify(const wmpatch::Image& img, std::string& rep);
const std::vector<std::string>& ModsEnabled();  // the enabled mods' folder names, in order
bool ModsEnabledIs(const std::string& mod);     // is this one of them
std::string ModsFolder(const std::string& mod); // where it lives
// A mod's own settings, from its own mods\<mod>\config.ini - a mod keeps what it needs beside itself rather than
// in wiimote.ini, which is the platform's file.  `section` may be "" for keys before any [section].
bool ModsSetting(const std::string& mod, const char* section, const char* key, std::string& value);
int  ModsSettingInt(const std::string& mod, const char* section, const char* key, int fallback);
std::string ModsFileByExt(const std::string& mod, const char* ext);   // a file the mod simply carries
bool ModsWorldSeen(uint32_t key);               // the engine has looked up this world's package (which level it is)
void Sm64Attach(const std::string& iniPath);    // wm_sm64.cpp, DllMain: Mario through libsm64 ([sm64])
void Sm64AfterConfig();
bool Sm64Enabled();
void Sm64Frame();                               // once per frame (main thread)

// wm_media.cpp - what the platform lends a mod so that the mod itself can stay a script: pictures, sounds, music,
// and the switches that let something else have the screen.  None of it knows about any particular mod.
struct IDirect3DDevice9;
int  MediaImage(const void* bytes, size_t n);   // a png/jpg/bmp in memory -> a number to draw it by (0: not one)
bool MediaImageSize(int id, int& w, int& h);
void MediaImageFree(int id);
void MediaDrawBegin(IDirect3DDevice9* dev, const RECT& picture);   // wm_lua.cpp, around a mod's draw handlers
void MediaDrawEnd();
bool MediaDrawing();
RECT MediaPicture();
void MediaRect(float x, float y, float w, float h, uint32_t colour);
void MediaDrawImage(int id, float x, float y, float w, float h, float u0, float v0, float u1, float v1,
                    uint32_t tint);
void MediaDeviceLost();
bool MediaSound(const void* bytes, size_t n, int volume);          // a wav or ogg in memory, played once
void MediaSoundStopAll();
bool MediaMusic(const char* path, int volume, bool loop);          // an ogg file, streamed
void MediaMusicStop();
void MediaFreeze(bool on);                      // the engine's frame time held at nothing: the world stands still
bool MediaFrozen();
void MediaBlockInput(bool on);                  // the game is handed a controller nobody is touching
bool MediaInputBlocked();
void MediaMuteGame(bool on);                    // the game's own music, effects and voices down
void MediaReset();
struct IDirect3DDevice9;
void AfxSetSceneDraw(void (*draw)(IDirect3DDevice9* dev, uint32_t view), void (*lost)());   // wm_afx.cpp: drawn into
                                                // the 3D image right after the engine's geometry (before the effects)
void OptionsSilenceMusic(bool on);              // wm_options.cpp: the game's music group down while SM64's plays
void OptionsSilenceEffects(bool on);            // and its effects group, while Mario has the level
void OptionsSilenceVoices(bool on);             // and the rabbids' voices (the dialogue group)
void OptionsSilenceExtra(const int* groups, int n);  // named mix groups silenced outright (the rabbids' own)
void LuaAttach(const std::string& iniPath);     // wm_lua.cpp, DllMain: the enabled mods' Lua scripts ([mods] lua)
void LuaAfterConfig();
void LuaFrame();                                // once per frame on the main thread: the scripts' frame callbacks

namespace wmnatives {                           // wm_natives_table.cpp: the engine's script natives by word
struct Native { uint32_t word; const char* name; uint8_t nargs; uint8_t argsizes[10]; uint16_t retsize; };
extern const Native kTable[];
extern const int kCount;
}
void LuaAddNative(const wmnatives::Native& n);  // a native the platform layer registered: wc.call / wc.native see it
void GfxAttach();                               // from OptionsAttach: verify, hook the display frame start
void GfxAfterConfig();
bool GfxInstalled();
bool GfxResolutionInstalled();                  // the display's client area call and the cursor imports
int  GfxVerify(const wmpatch::Image& img, std::string& rep);
void GfxStartupVsync(int vsync);                // before the device exists: the /vsync option (-1 = unchanged)
int  GfxGameVsync();                            // the command line's /vsync (the game's own setting)
int  GfxVsync();                                // the renderer's VSync setting
void GfxSetVsync(int on);                       // live: one device reset
bool GfxClientSize(int& w, int& h);
bool GfxMonitorSize(int& w, int& h, bool workArea);
bool GfxBorderless();
bool GfxWindowed();                             // not /fullscreen: the window settings apply
void GfxSetResolution(int height);              // 0 = the window's size, else the height rendered (stretched)
bool GfxRenderSize(int height, int& w, int& h); // the size rendered for a height in the current window
void GfxSetBorderless(bool on);
void GfxSetHighDetail(bool on);                 // K3D_ForceNoLod every frame while on
void GfxSetAniso(int level);                    // 0 = the game's filters
void GfxFrame();                                // once per frame (main thread)
struct IDirect3DDevice9;
struct GfxOverlay {                             // what the Lua module draws over every picture (wm_lua.cpp)
    void (*draw)(IDirect3DDevice9* dev, const RECT& picture) = NULL;   // picture: the presented part of the buffer
    void (*lost)() = NULL;                      // before a device reset: release the default-pool objects
    void (*restored)() = NULL;                  // after a successful reset
};
IDirect3DDevice9* GfxDevice();                  // the renderer's device (NULL before it exists)
HWND GfxWindow();                               // the game window
RECT GfxPicture();                              // the part of the back buffer presented last (the picture's pixels)
void GfxSetOverlay(const GfxOverlay& overlay);
void GfxSetHud(const GfxOverlay& hud);          // a second slot, drawn over the first (wm_sm64.cpp: SM64's HUD)
void GfxSetTop(const GfxOverlay& top);          // a third, over both (wm_battle.cpp: a battle covers everything)

// ---------------------------------------------------------------------------------------------------------------
// Patching helpers shared by wm_sav.cpp and wm_ctl.cpp (wm_patch.cpp)
// ---------------------------------------------------------------------------------------------------------------
namespace wmpatch {

struct Image {                                  // the PC executable: in this process, or its file
    virtual bool Read(uint32_t va, void* out, uint32_t n) const = 0;
    virtual ~Image() {}
};

struct ProcessImage : Image {
    bool Read(uint32_t va, void* out, uint32_t n) const;
};

struct FileImage : Image {
    struct Sec { uint32_t va, vsize, rsize, rptr; bool code; };
    std::vector<uint8_t> data;
    std::vector<Sec> secs;
    bool Load(const char* path);
    uint32_t U32(size_t off) const;
    bool Read(uint32_t va, void* out, uint32_t n) const;
};

uint32_t Crc32(const uint8_t* p, size_t n);
void Report(std::string& rep, bool ok, const char* fmt, ...);      // "  PASS  ..." / "  FAIL  ..." line
bool CheckCrc(const Image& img, uint32_t va, uint32_t len, uint32_t want, uint32_t* got);
bool WriteCode(uint32_t va, const void* bytes, uint32_t n);
// Replace the handler of the native table elements whose (key & keyMask) matches: 1 = done (or already ours),
// -1 = an element holds another handler (left alone, see notes), 0 = no such element / no table.
int PatchNativeTable(uint32_t key, uint32_t keyMask, uint32_t oldFn, uint32_t newFn, std::string& notes);
int PatchNativeTableAt(uint32_t table, uint32_t key, uint32_t keyMask, uint32_t oldFn, uint32_t newFn,
                       std::string& notes);
// Every element holding oldFn gets newFn; returns how many elements hold newFn afterwards.
int PatchNativeHandler(uint32_t oldFn, uint32_t newFn, std::string& notes);
int PatchNativeHandlerAt(uint32_t table, uint32_t oldFn, uint32_t newFn, std::string& notes);
bool CheckRegisterScriptEntry(const Image& img, uint32_t reg, uint32_t handler, uint32_t key);  // ViD::RegisterScript
bool CheckModuleEntry(const Image& img, uint32_t reg, uint32_t handler);                        // module registrar
bool ReadIniKey(const std::string& path, const char* section, const char* key, std::string& value);  // no profile API

}  // namespace wmpatch

// ---------------------------------------------------------------------------------------------------------------
// PC input source
// ---------------------------------------------------------------------------------------------------------------
struct WmTestInput {            // test-only injection (WiimoteTestInject), replaces keyboard/mouse/pad polling
    uint16_t buttons;
    uint16_t pad0;
    int32_t  pointerValid;
    float    px, py;
    float    stickX, stickY;
    float    rollDeg;
    int32_t  shake;
};

void PcSourceInit();
void PcSourcePoll(double t, WmState& out);      // one sample at time t (seconds)
void PcSourceSetRumble(bool on);
void PcSourceInject(const WmTestInput* in);     // NULL = back to real input

// ---------------------------------------------------------------------------------------------------------------
// WPAD synthesis and the TCP server
// ---------------------------------------------------------------------------------------------------------------
void WpadFromState(const WmState& st, float centerY, WpadRaw& out);
float KpadCenterY();                            // KPAD DPD centre used for the synthesis

bool ServerStart();                             // bind + listen (synchronous), start the I/O thread
void ServerStop();
bool ServerLatest(WmState& out, uint32_t* seq); // latest sample (for the Gear exports), seq counts samples
