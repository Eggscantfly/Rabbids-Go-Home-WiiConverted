// wm_options.cpp - the Options screen of the pause menu ([options] in wiimote.ini): control bindings (up to three
// inputs per action, from the keyboard, the mouse or a pad) and four volumes.  Settings live in options.ini next to
// the DLL.
//
// The page.  The converter adds page PCOPT_P_Options to Common.mgb and an OPTIONS entry to the in-game pause menu
// (rghport/convert/menus.py).  The pause script pushes the page and waits while it is the top page; this module
// notices the page on top (MGM_GetTopLevelPageID_C, once per frame right before the Magma update in ViD::OneFrame),
// drives it through the Magma script exports (texts, frames, visibility, positions of the elements the converter gave
// generic objects PCOPT_G_<name>), reads the keyboard, mouse and pads itself, and pops the page on BACK.  While the
// page is up, and afterwards until every key and button is released, the game sees no input.
//
// Controls ([controls] mode=pc).  The PC scripts read keys and pads directly (IO_KeyboardKey*, IO_JoystickButton*,
// IO_JoystickMove).  The port's PC-controls scripts read one virtual key per action instead (unassigned virtual-key
// codes of the engine's keyboard state, below) whenever key 0x07 reads pressed, which this module sets every frame.
// Right after the engine's input poll (ViD::OneFrame -> 006CC740: GetKeyboardState, controllers, mouse) the module
// evaluates each action from its bindings - physical keys and mouse buttons from that keyboard state, pad buttons
// from the raw button word the controller update read (hooked calls in 006EC890), sticks through the engine's own
// readers and dead zones - and writes the virtual keys into the keyboard state.  When a pad is connected the movement
// actions also become the pad's left stick (the scripts' pad path reads that).  Menus keep their keys (arrows, Enter,
// Esc, mouse, pad buttons).
//
// Volumes.  The sound engine keeps a volume offset per sound group in millibels (SND_GroupOffsetSet_C; group volume =
// own offset + base volume + the parent group's volume, 0049C3E0).  Master is group 0 (MASTER), music the mix's
// USER_MUSIC (2, above MUSIC 12 and the radio groups 39-41), effects SFX (10), AMB (11) and HUD (14) with the groups
// below them, voices DIALOG (13).  The converter moves music the mix files elsewhere (radio songs in the ambience
// groups ...) to the music group without changing its volume (rghport/convert/sound.py); effects the mix keeps in
// MASTER (the death sound, jingles) stay there and follow the master slider only, since the game fades SFX on death.
// A slider sets 40 log10(percent / 100) dB (the gain squared: halfway is -12 dB), 0 % silence.
//
// Shift, Ctrl and Alt are one binding each for both sides (the combined VK_SHIFT / VK_CONTROL / VK_MENU of the
// keyboard state, as the PC scripts read both sides); side-specific names in options.ini load as the combined key.
//
// Graphics (wm_gfx.cpp).  Display mode, resolution, VSync, high detail, texture filtering and the Wii effects, applied
// live and again at the next start.
//
// Motion (wm_motion.cpp).  The Wii-only scripts still read the remote's accelerometers (the controls check that opens
// a new game, Inside Zee Wiimote).  SCREAM also shakes the remote and the Nunchuk, and five rows without a virtual key
// shake and tilt the remote: SHAKE LEFT/RIGHT, SHAKE UP/DOWN, MARACA SHAKE, TILT LEFT, TILT RIGHT, SHAKE CLOCKWISE,
// SHAKE COUNTER-CW.  Besides keys and buttons they take inputs of their own: a fast mouse wiggle along one axis
// (MOUSE_SHAKE_X / MOUSE_SHAKE_Y: two turns in a row of strokes of at least 4 % of the view's height at 0.8 heights
// a second, from the engine's per-frame mouse movement), the mouse moved in circles (MOUSE_CIRCLE_CW /
// MOUSE_CIRCLE_CCW: the direction of movement turning the same way by 0.85 of a turn within 0.9 s, on a path at
// least 0.45 times as high as wide or the reverse; it takes over from the wiggles its x and y strokes would be) and
// the scroll wheel (WHEEL, WHEEL_UP, WHEEL_DOWN: the engine's wheel bits; a shake lasts 0.3 s after a notch).  The other actions can take them too (the wheel presses them for one frame per notch).  A mouse
// button on a tilt row tilts only when held with the mouse still for a quarter of a second (it also clicks and drags).
#include "wiimote.h"

#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>

using namespace wmpatch;

namespace {

// ---------------------------------------------------------------------------------------------------------------
// the PC executable (2010 build)
// ---------------------------------------------------------------------------------------------------------------
const uint32_t CALL_INPUT_POLL  = 0x0050375A;   // ViD::OneFrame: call 006CC740 (keyboard, controllers, mouse)
const uint32_t INPUT_POLL       = 0x006CC740;
const uint32_t CALL_MGM_UPDATE  = 0x0050386B;   // ViD::OneFrame: call MGM_Interface::GetManager, then the Magma update
const uint32_t MGM_GET_MANAGER  = 0x007F70A0;
const uint32_t CALL_RAW_DINPUT  = 0x006EC8C7;   // 006EC890 (buttons -> logical masks): raw button word per device type
const uint32_t RAW_DINPUT       = 0x006FC810;
const uint32_t CALL_RAW_KPAD    = 0x006EC8E3;
const uint32_t RAW_KPAD         = 0x006F5450;
const uint32_t CALL_RAW_XINPUT  = 0x006EC8FF;
const uint32_t RAW_XINPUT       = 0x006FCD80;
const uint32_t XINPUT_STICK     = 0x006FCDF0;   // (slot, stick, float out[3]): raw thumb / trigger values
const uint32_t STICK_NORMALISE  = 0x006ECA50;   // (slot, stick, float out[3]): centre, dead zone, axis flips
const uint32_t MGM_SET_INT      = 0x005EB5E0;
const uint32_t MGM_SET_FLOAT    = 0x005EB610;
const uint32_t MGM_SET_WSTRING  = 0x005EB670;
const uint32_t MGM_GET_INT      = 0x005EB6A0;
const uint32_t MGM_POP_PAGE     = 0x005EB810;
const uint32_t MGM_POP_CURSOR   = 0x005EB900;
const uint32_t MGM_TOP_PAGE     = 0x005EB9A0;
const uint32_t SND_GROUP_SET    = 0x006286A0;   // (group, float dB)

struct CallSite { uint32_t va, target, crc; const char* what; };   // E8 rel32; CRC-32 of [va - 0x10, va + 0x10)
const CallSite kSites[] = {
    { 0x0050375A, 0x006CC740, 0xD8188AA4, "ViD::OneFrame -> input poll" },
    { 0x0050386B, 0x007F70A0, 0xB4646BBA, "ViD::OneFrame -> MGM_Interface::GetManager (Magma update)" },
    { 0x006EC8C7, 0x006FC810, 0x5B7EF5C9, "controller update -> DirectInput buttons" },
    { 0x006EC8E3, 0x006F5450, 0x3E547017, "controller update -> KPAD buttons" },
    { 0x006EC8FF, 0x006FCD80, 0x4C94252B, "controller update -> XInput buttons" },
};

struct Code { uint32_t va, len, crc; const char* what; };
const Code kCode[] = {
    { 0x006CC740, 0x096, 0x8E03CE44, "input poll (keyboard, controllers, mouse)" },
    { 0x006EDBB0, 0x01E, 0xB684A20F, "keyboard state buffers + GetKeyboardState" },
    { 0x006EDBD0, 0x015, 0x420F29B0, "IO_KeyboardKeyPressed" },
    { 0x006EDBF0, 0x026, 0x9F6479AF, "IO_KeyboardKeyJustPressed" },
    { 0x006EDC20, 0x026, 0xC5994CBB, "IO_KeyboardKeyJustReleased" },
    { 0x006CC5E0, 0x016, 0x3721BFAF, "IO_MouseButtonPressed" },
    { 0x006CC280, 0x01E, 0x65E7A68A, "mouse wheel message kept for the mouse update" },
    { 0x006CC2A0, 0x192, 0x3295EE74, "mouse update: movement 00A90F0C / scale 00A90F08, wheel bits 08 / 10" },
    { 0x006CC600, 0x023, 0x61E535D0, "IO_MouseButtonJustPressed" },
    { 0x006CC630, 0x023, 0x77896A48, "IO_MouseButtonJustReleased" },
    { 0x006EC890, 0x0B8, 0x303E435C, "controller buttons -> logical masks" },
    { 0x006EC950, 0x038, 0x7F8BA936, "IO_JoystickHere" },
    { 0x006EC990, 0x032, 0xABF81ADF, "IO_JoystickButtonPressed" },
    { 0x006ED3A0, 0x0C1, 0x921E52F0, "controller sticks per slot" },
    { 0x006FCDF0, 0x1C6, 0x3C6DAB7B, "XInput stick read" },
    { 0x006ECA50, 0x1B9, 0xF14E44DC, "stick normalisation" },
    { 0x006ECC10, 0x056, 0x219E64B6, "IO_JoystickMove / IO_JoystickStickGet" },
    { 0x007F70A0, 0x0AE, 0x4905E416, "MGM_Interface::GetManager" },
    { 0x005EB5E0, 0x02B, 0xBF1A0D1A, "MGM_SetIntProperty_C" },
    { 0x005EB610, 0x02E, 0x940DF1C0, "MGM_SetFloatProperty_C" },
    { 0x005EB670, 0x02B, 0x5528E722, "MGM_SetWStringProperty_C" },
    { 0x005EB6A0, 0x021, 0xC9A55718, "MGM_GetIntProperty_C" },
    { 0x005EB810, 0x026, 0xAFF9C0BD, "MGM_PopPage_C" },
    { 0x005EB900, 0x017, 0xEDFDB2E4, "MGM_PopMouseCursor_C" },
    { 0x005EB9A0, 0x011, 0xBFCF5805, "MGM_GetTopLevelPageID_C" },
    { 0x006286A0, 0x02D, 0x767ABA40, "SND_GroupOffsetSet_C" },
    { 0x0049CE10, 0x053, 0xFC6F7278, "sound group offset store" },
    { 0x0049C3E0, 0x122, 0xE515ECAA, "sound group volume (parents)" },
};

// engine state (the PC executable's addresses; the self-test points them at fakes)
struct OptCtx {
    uint8_t** kbCur;            // 00A97B00: this frame's GetKeyboardState buffer (IO_KeyboardKey* read it)
    int32_t*  windowActive;     // 009D8D38: input is read only while a game window has the focus
    int32_t*  slotOfId;         // 00A97A20: controller id (0..49) -> slot (0..9)
    int32_t*  connected;        // 00A979D0: per slot
    int32_t*  devType;          // 00A97980: per slot, 1 DirectInput, 2 KPAD, 3 XInput
    uint32_t* joyMask;          // 00A96B80: + slot * 0xA4, logical buttons
    float*    sticks;           // 00A96A08: + (slot * 3 + stick) * 12, the scripts' sticks (x, y, z)
    int32_t*  stickEnabled;     // 00A973E0: + (slot * 3 + stick) * 4 (IO_JoystickStickEnable)
    uint32_t* mouseCur;         // 00A90F04: mouse buttons (IO_MouseButton*), 08 wheel up, 10 wheel down this frame
    int32_t*  cursor;           // 00A90F18: {x, y} in the client area
    int32_t*  viewport;         // 00A90EC8: {height, width, top, left}
    uint32_t* sndGlobal;        // 00A6E88C: [..] + 0xA28 = sound manager
    float*    mouseMove;        // 00A90F0C: {x, y} this frame's movement / the scale (also while the cursor is held)
    float*    mouseScale;       // 00A90F08
};

OptCtx s_game = {
    (uint8_t**)0x00A97B00, (int32_t*)0x009D8D38, (int32_t*)0x00A97A20, (int32_t*)0x00A979D0,
    (int32_t*)0x00A97980, (uint32_t*)0x00A96B80, (float*)0x00A96A08, (int32_t*)0x00A973E0,
    (uint32_t*)0x00A90F04, (int32_t*)0x00A90F18, (int32_t*)0x00A90EC8, (uint32_t*)0x00A6E88C,
    (float*)0x00A90F0C, (float*)0x00A90F08,
};
OptCtx* s_ctx = &s_game;

// ---------------------------------------------------------------------------------------------------------------
// actions and bindings
// ---------------------------------------------------------------------------------------------------------------
const uint8_t VK_LAYER_ON = 0x07;   // pressed every frame while this layer runs: the scripts use the virtual keys

enum { N_SLOTS = 3, N_VOLUMES = 4 };

struct ActionDef {
    const char* key;            // options.ini
    const wchar_t* label;       // the page
    uint8_t vk;                 // virtual key the scripts read; 0 = the remote's motion (wm_motion.cpp)
    uint8_t vkPad;              // RUN: pad bindings (held); the other bindings on vk (the scripts toggle on release)
    const char* defaults;       // the PC release's keys and the pad buttons of its pad mapping
};

const ActionDef kActions[] = {
    { "move_forward", L"MOVE FORWARD", 0x88, 0,    "W,UP,PAD_LS_UP" },
    { "move_back",    L"MOVE BACK",    0x89, 0,    "S,DOWN,PAD_LS_DOWN" },
    { "move_left",    L"MOVE LEFT",    0x8A, 0,    "A,LEFT,PAD_LS_LEFT" },
    { "move_right",   L"MOVE RIGHT",   0x8B, 0,    "D,RIGHT,PAD_LS_RIGHT" },
    { "dash",         L"DASH",         0x8C, 0,    "SPACE,PAD_A" },
    { "run",          L"RUN",          0x8D, 0x8E, "SHIFT,PAD_A" },
    { "scream",       L"SCREAM",       0x8F, 0,    "SPACE,PAD_X" },
    { "throw",        L"THROW",        0x97, 0,    "MOUSE1,PAD_LT" },
    { "grab",         L"GRAB",         0x98, 0,    "RETURN,PAD_A" },
    { "boost",        L"SUPER BOOST",  0x99, 0,    "ALT,SHIFT,PAD_B" },
    { "swap",         L"SWAP PLAYERS", 0x9A, 0,    "TAB,PAD_START" },
    { "pause",        L"PAUSE",        0x9B, 0,    "ESCAPE,PAD_START" },
    { "shake_sideways", L"SHAKE LEFT/RIGHT", 0, 0,  "MOUSE_SHAKE_X" },
    { "shake_updown", L"SHAKE UP/DOWN", 0,    0,    "MOUSE_SHAKE_Y" },
    { "shake_maraca", L"MARACA SHAKE", 0,     0,    "WHEEL" },
    { "tilt_left",    L"TILT LEFT",    0,     0,    "MOUSE1" },
    { "tilt_right",   L"TILT RIGHT",   0,     0,    "MOUSE2" },
    { "shake_clockwise", L"SHAKE CLOCKWISE", 0, 0,  "MOUSE_CIRCLE_CW" },
    { "shake_counterclockwise", L"SHAKE COUNTER-CW", 0, 0, "MOUSE_CIRCLE_CCW" },
};
enum { N_ACTIONS = sizeof(kActions) / sizeof(kActions[0]) };
enum { A_FORWARD, A_BACK, A_LEFT, A_RIGHT, A_SCREAM = 6, A_SHAKE_X = 12, A_SHAKE_Y, A_MARACA, A_TILT_LEFT, A_TILT_RIGHT,
       A_ROTATE_CW, A_ROTATE_CCW };

// KEY: virtual key; MOUSE: 1..5; PAD: raw bit; AXIS: stick * 4 + direction; MOUSEMOVE: a fast wiggle, 0 x / 1 y, the
// mouse moved in circles, 2 clockwise / 3 counter-clockwise;
// WHEEL: 0 either way, 1 up, 2 down
enum BindKind : uint8_t { B_NONE, B_KEY, B_MOUSE, B_PAD, B_AXIS, B_MOUSEMOVE, B_WHEEL };
struct Bind { uint8_t kind, code; };

const uint8_t kMouseVk[6] = { 0, 0x01, 0x02, 0x04, 0x05, 0x06 };   // MOUSE1 left, 2 right, 3 middle, 4 X1, 5 X2

struct NameCode { const char* ini; const wchar_t* shown; int code; };

// keyboard names (the [keys] names of wiimote.ini where they exist)
const NameCode kKeys[] = {
    { "LBUTTON", L"LEFT CLICK", 0x01 }, { "RBUTTON", L"RIGHT CLICK", 0x02 }, { "MBUTTON", L"MIDDLE CLICK", 0x04 },
    { "XBUTTON1", L"MOUSE 4", 0x05 }, { "XBUTTON2", L"MOUSE 5", 0x06 }, { "BACK", L"BACKSPACE", 0x08 },
    { "TAB", L"TAB", 0x09 }, { "RETURN", L"ENTER", 0x0D }, { "PAUSE", L"PAUSE", 0x13 },
    { "CAPITAL", L"CAPS LOCK", 0x14 }, { "ESCAPE", L"ESC", 0x1B }, { "SPACE", L"SPACE", 0x20 },
    { "PRIOR", L"PAGE UP", 0x21 }, { "NEXT", L"PAGE DOWN", 0x22 }, { "END", L"END", 0x23 }, { "HOME", L"HOME", 0x24 },
    { "LEFT", L"LEFT", 0x25 }, { "UP", L"UP", 0x26 }, { "RIGHT", L"RIGHT", 0x27 }, { "DOWN", L"DOWN", 0x28 },
    { "INSERT", L"INSERT", 0x2D }, { "DELETE", L"DELETE", 0x2E }, { "MULTIPLY", L"NUM *", 0x6A },
    { "ADD", L"NUM +", 0x6B }, { "SUBTRACT", L"NUM -", 0x6D }, { "DECIMAL", L"NUM .", 0x6E },
    { "DIVIDE", L"NUM /", 0x6F }, { "SHIFT", L"SHIFT", 0x10 }, { "CONTROL", L"CTRL", 0x11 },
    { "ALT", L"ALT", 0x12 }, { "OEM_1", L";", 0xBA }, { "OEM_PLUS", L"=", 0xBB }, { "OEM_COMMA", L",", 0xBC },
    { "OEM_MINUS", L"-", 0xBD }, { "OEM_PERIOD", L".", 0xBE }, { "OEM_2", L"/", 0xBF }, { "OEM_3", L"`", 0xC0 },
    { "OEM_4", L"[", 0xDB }, { "OEM_5", L"\\", 0xDC }, { "OEM_6", L"]", 0xDD }, { "OEM_7", L"'", 0xDE },
};

// XInput raw button word (006FCD80: wButtons, 0x10000 left trigger, 0x20000 right trigger)
// Shift, Ctrl and Alt are one binding each for both sides (the keyboard state's combined VK_SHIFT / VK_CONTROL /
// VK_MENU, set for either key, as the PC scripts read both): the side-specific names and codes load as the combined key
const NameCode kKeyAliases[] = {
    { "LSHIFT", L"", 0x10 }, { "RSHIFT", L"", 0x10 }, { "LCONTROL", L"", 0x11 }, { "RCONTROL", L"", 0x11 },
    { "CTRL", L"", 0x11 }, { "LCTRL", L"", 0x11 }, { "RCTRL", L"", 0x11 }, { "LALT", L"", 0x12 }, { "RALT", L"", 0x12 },
    { "MENU", L"", 0x12 }, { "LMENU", L"", 0x12 }, { "RMENU", L"", 0x12 },
};

int CombinedModifier(int vk) { return vk >= 0xA0 && vk <= 0xA5 ? 0x10 + (vk - 0xA0) / 2 : vk; }

const NameCode kPadBits[] = {
    { "PAD_DUP", L"D-PAD UP", 0 }, { "PAD_DDOWN", L"D-PAD DOWN", 1 }, { "PAD_DLEFT", L"D-PAD LEFT", 2 },
    { "PAD_DRIGHT", L"D-PAD RIGHT", 3 }, { "PAD_START", L"PAD START", 4 }, { "PAD_BACK", L"PAD BACK", 5 },
    { "PAD_LS", L"PAD L3", 6 }, { "PAD_RS", L"PAD R3", 7 }, { "PAD_LB", L"PAD LB", 8 }, { "PAD_RB", L"PAD RB", 9 },
    { "PAD_A", L"PAD A", 12 }, { "PAD_B", L"PAD B", 13 }, { "PAD_X", L"PAD X", 14 }, { "PAD_Y", L"PAD Y", 15 },
    { "PAD_LT", L"PAD LT", 16 }, { "PAD_RT", L"PAD RT", 17 },
};

const NameCode kAxes[] = {
    { "PAD_LS_UP", L"L STICK UP", 0 }, { "PAD_LS_DOWN", L"L STICK DOWN", 1 }, { "PAD_LS_LEFT", L"L STICK LEFT", 2 },
    { "PAD_LS_RIGHT", L"L STICK RIGHT", 3 }, { "PAD_RS_UP", L"R STICK UP", 4 }, { "PAD_RS_DOWN", L"R STICK DOWN", 5 },
    { "PAD_RS_LEFT", L"R STICK LEFT", 6 }, { "PAD_RS_RIGHT", L"R STICK RIGHT", 7 },
};

const wchar_t* const kMouseNames[6] = { L"", L"LEFT CLICK", L"RIGHT CLICK", L"MIDDLE CLICK", L"MOUSE 4", L"MOUSE 5" };

const NameCode kMouseMoves[] = {
    { "MOUSE_SHAKE_X", L"MOUSE SHAKE X", 0 }, { "MOUSE_SHAKE_Y", L"MOUSE SHAKE Y", 1 },
    { "MOUSE_CIRCLE_CW", L"MOUSE CIRCLES CW", 2 }, { "MOUSE_CIRCLE_CCW", L"MOUSE CIRCLES CCW", 3 },
};
enum { N_MOUSEMOVES = sizeof(kMouseMoves) / sizeof(kMouseMoves[0]) };
const NameCode kWheel[] = {
    { "WHEEL", L"SCROLL WHEEL", 0 }, { "WHEEL_UP", L"WHEEL UP", 1 }, { "WHEEL_DOWN", L"WHEEL DOWN", 2 },
};

std::string Upper(std::string s) {
    for (size_t i = 0; i < s.size(); ++i) s[i] = (char)toupper((unsigned char)s[i]);
    return s;
}

std::string Trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
    return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
}

bool ParseBind(const std::string& tok, Bind& out) {
    std::string t = Upper(Trim(tok));
    out.kind = B_NONE;
    out.code = 0;
    if (t.empty()) return true;
    if (t.size() == 6 && t.compare(0, 5, "MOUSE") == 0 && t[5] >= '1' && t[5] <= '5') {
        out.kind = B_MOUSE;
        out.code = (uint8_t)(t[5] - '0');
        return true;
    }
    for (size_t i = 0; i < sizeof(kPadBits) / sizeof(kPadBits[0]); ++i)
        if (t == kPadBits[i].ini) { out.kind = B_PAD; out.code = (uint8_t)kPadBits[i].code; return true; }
    for (size_t i = 0; i < sizeof(kAxes) / sizeof(kAxes[0]); ++i)
        if (t == kAxes[i].ini) { out.kind = B_AXIS; out.code = (uint8_t)kAxes[i].code; return true; }
    for (size_t i = 0; i < sizeof(kMouseMoves) / sizeof(kMouseMoves[0]); ++i)
        if (t == kMouseMoves[i].ini) { out.kind = B_MOUSEMOVE; out.code = (uint8_t)kMouseMoves[i].code; return true; }
    for (size_t i = 0; i < sizeof(kWheel) / sizeof(kWheel[0]); ++i)
        if (t == kWheel[i].ini) { out.kind = B_WHEEL; out.code = (uint8_t)kWheel[i].code; return true; }
    if (t.size() > 7 && t.compare(0, 7, "PAD_BIT") == 0) {           // other devices' raw buttons
        int n = atoi(t.c_str() + 7);
        if (n >= 0 && n < 32) { out.kind = B_PAD; out.code = (uint8_t)n; return true; }
        return false;
    }
    int vk = -1;
    if (t.size() > 2 && t[0] == '0' && t[1] == 'X') vk = (int)(strtol(t.c_str(), NULL, 16) & 0xFF);
    else if (t.size() == 1 && ((t[0] >= 'A' && t[0] <= 'Z') || (t[0] >= '0' && t[0] <= '9'))) vk = t[0];
    else if (t[0] == 'F' && t.size() <= 3 && isdigit((unsigned char)t[1]) && atoi(t.c_str() + 1) >= 1 &&
             atoi(t.c_str() + 1) <= 24) vk = 0x6F + atoi(t.c_str() + 1);
    else if (t.size() == 7 && t.compare(0, 6, "NUMPAD") == 0 && isdigit((unsigned char)t[6])) vk = 0x60 + (t[6] - '0');
    else {
        for (size_t i = 0; i < sizeof(kKeys) / sizeof(kKeys[0]); ++i)
            if (t == kKeys[i].ini) vk = kKeys[i].code;
        for (size_t i = 0; i < sizeof(kKeyAliases) / sizeof(kKeyAliases[0]); ++i)
            if (t == kKeyAliases[i].ini) vk = kKeyAliases[i].code;
    }
    vk = CombinedModifier(vk);
    if (vk <= 0) return false;
    for (int m = 1; m <= 5; ++m)
        if (vk == kMouseVk[m]) { out.kind = B_MOUSE; out.code = (uint8_t)m; return true; }
    out.kind = B_KEY;
    out.code = (uint8_t)vk;
    return true;
}

std::string IniName(const Bind& b) {
    char buf[16];
    switch (b.kind) {
    case B_MOUSE:
        sprintf(buf, "MOUSE%d", b.code);
        return buf;
    case B_PAD:
        for (size_t i = 0; i < sizeof(kPadBits) / sizeof(kPadBits[0]); ++i)
            if (kPadBits[i].code == b.code) return kPadBits[i].ini;
        sprintf(buf, "PAD_BIT%d", b.code);
        return buf;
    case B_AXIS:
        return b.code < 8 ? kAxes[b.code].ini : "";
    case B_MOUSEMOVE:
        return b.code < N_MOUSEMOVES ? kMouseMoves[b.code].ini : "";
    case B_WHEEL:
        return b.code < 3 ? kWheel[b.code].ini : "";
    case B_KEY:
        if ((b.code >= 'A' && b.code <= 'Z') || (b.code >= '0' && b.code <= '9')) return std::string(1, (char)b.code);
        if (b.code >= 0x70 && b.code <= 0x87) { sprintf(buf, "F%d", b.code - 0x6F); return buf; }
        if (b.code >= 0x60 && b.code <= 0x69) { sprintf(buf, "NUMPAD%d", b.code - 0x60); return buf; }
        for (size_t i = 0; i < sizeof(kKeys) / sizeof(kKeys[0]); ++i)
            if (kKeys[i].code == b.code) return kKeys[i].ini;
        sprintf(buf, "0x%02X", b.code);
        return buf;
    default:
        return "";
    }
}

std::wstring ShownName(const Bind& b, int devType) {
    wchar_t buf[24];
    switch (b.kind) {
    case B_MOUSE:
        return b.code <= 5 ? kMouseNames[b.code] : L"MOUSE";
    case B_PAD:
        if (devType != 1)
            for (size_t i = 0; i < sizeof(kPadBits) / sizeof(kPadBits[0]); ++i)
                if (kPadBits[i].code == b.code) return kPadBits[i].shown;
        swprintf(buf, 24, L"BUTTON %d", b.code + 1);
        return buf;
    case B_AXIS:
        return b.code < 8 ? kAxes[b.code].shown : L"STICK";
    case B_MOUSEMOVE:
        return b.code < N_MOUSEMOVES ? kMouseMoves[b.code].shown : L"MOUSE SHAKE";
    case B_WHEEL:
        return b.code < 3 ? kWheel[b.code].shown : L"SCROLL WHEEL";
    case B_KEY:
        if ((b.code >= 'A' && b.code <= 'Z') || (b.code >= '0' && b.code <= '9'))
            return std::wstring(1, (wchar_t)b.code);
        if (b.code >= 0x70 && b.code <= 0x87) { swprintf(buf, 24, L"F%d", b.code - 0x6F); return buf; }
        if (b.code >= 0x60 && b.code <= 0x69) { swprintf(buf, 24, L"NUM %d", b.code - 0x60); return buf; }
        for (size_t i = 0; i < sizeof(kKeys) / sizeof(kKeys[0]); ++i)
            if (kKeys[i].code == b.code) return kKeys[i].shown;
        swprintf(buf, 24, L"KEY %02X", b.code);
        return buf;
    default:
        return L"-";
    }
}

struct GfxOptions {
    int32_t display;            // 0 window, 1 borderless full screen
    int32_t resolution;         // the height rendered (stretched over the window); 0 = the window's size
    int32_t vsync;              // -1 = the game's own (command line), 0 off, 1 on
    int32_t highDetail;         // 1 = every model at its best level of detail
    int32_t aniso;              // 0 = the game's filters, 2 / 4 / 8 / 16 = anisotropic
    int32_t wiiEffects;         // -1 = [video] afx, 0 off, 1 on
    int32_t frameRate;          // -1 = [timing] fps_cap, 0 = uncapped, else Hz
};

struct Settings {
    Bind       bind[N_ACTIONS][N_SLOTS];
    int        volume[N_VOLUMES];     // percent, 0..100
    GfxOptions gfx;
};

Settings s_set;

void ParseList(const char* list, Bind out[N_SLOTS], const char* where, std::vector<std::string>* problems) {
    for (int i = 0; i < N_SLOTS; ++i) out[i].kind = out[i].code = 0;
    std::string s(list);
    int n = 0;
    size_t start = 0;
    while (start <= s.size()) {
        size_t comma = s.find(',', start);
        std::string tok = s.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
        Bind b;
        if (!ParseBind(tok, b)) {
            if (problems) problems->push_back(std::string(where) + ": unknown input '" + Trim(tok) + "'");
        } else if (b.kind != B_NONE && n < N_SLOTS) {
            bool twice = false;
            for (int k = 0; k < n; ++k)
                if (out[k].kind == b.kind && out[k].code == b.code) twice = true;
            if (!twice) out[n++] = b;
        }
        if (comma == std::string::npos) break;
        start = comma + 1;
    }
}

void Defaults(Settings& st, bool controls, bool audio, bool graphics = false) {
    if (controls)
        for (int a = 0; a < N_ACTIONS; ++a) ParseList(kActions[a].defaults, st.bind[a], kActions[a].key, NULL);
    if (audio)
        for (int v = 0; v < N_VOLUMES; ++v) st.volume[v] = 100;
    if (graphics) {
        st.gfx.display = 0;
        st.gfx.resolution = 0;
        st.gfx.vsync = -1;
        st.gfx.highDetail = 0;
        st.gfx.aniso = 0;
        st.gfx.wiiEffects = -1;
        st.gfx.frameRate = -1;
    }
}

const char* const kVolumeKeys[N_VOLUMES] = { "master", "music", "effects", "voices" };
const wchar_t* const kVolumeLabels[N_VOLUMES] = { L"MASTER", L"MUSIC", L"EFFECTS", L"VOICES" };

// ---------------------------------------------------------------------------------------------------------------
// log
// ---------------------------------------------------------------------------------------------------------------
bool s_inAttach;
std::vector<std::string> s_pending;
std::string* s_testLog;

void OptLog(const char* fmt, ...) {
    char buf[700];
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
        Log("OPTIONS: %s", buf);
    }
}

// ---------------------------------------------------------------------------------------------------------------
// options.ini
// ---------------------------------------------------------------------------------------------------------------
std::string s_path;

int IniInt(const std::string& path, const char* section, const char* key, int def, int lo, int hi) {
    std::string v;
    if (!ReadIniKey(path, section, key, v) || Trim(v).empty()) return def;
    int n = atoi(v.c_str());
    return n < lo ? lo : n > hi ? hi : n;
}

void LoadSettings(const std::string& path, Settings& st, std::vector<std::string>& problems) {
    Defaults(st, true, true, true);
    if (GetFileAttributesA(path.c_str()) == INVALID_FILE_ATTRIBUTES) return;
    std::string v;
    for (int a = 0; a < N_ACTIONS; ++a)
        if (ReadIniKey(path, "controls", kActions[a].key, v))
            ParseList(v.c_str(), st.bind[a], kActions[a].key, &problems);
    for (int i = 0; i < N_VOLUMES; ++i) st.volume[i] = IniInt(path, "audio", kVolumeKeys[i], 100, 0, 100);
    if (ReadIniKey(path, "graphics", "display", v)) st.gfx.display = _stricmp(Trim(v).c_str(), "borderless") == 0;
    st.gfx.resolution = IniInt(path, "graphics", "resolution", 0, 0, 4320);
    if (st.gfx.resolution > 0 && st.gfx.resolution < 240) st.gfx.resolution = 240;
    st.gfx.vsync = IniInt(path, "graphics", "vsync", -1, -1, 1);
    st.gfx.highDetail = IniInt(path, "graphics", "high_detail", 0, 0, 1);
    int a = IniInt(path, "graphics", "texture_filter", 0, 0, 16);
    st.gfx.aniso = a >= 16 ? 16 : a >= 8 ? 8 : a >= 4 ? 4 : a >= 2 ? 2 : 0;
    st.gfx.wiiEffects = IniInt(path, "graphics", "wii_effects", -1, -1, 1);
    st.gfx.frameRate = IniInt(path, "graphics", "frame_rate", -1, -1, 1000);
    if (st.gfx.frameRate > 0 && st.gfx.frameRate < 10) st.gfx.frameRate = 10;
}

std::string FormatSettings(const Settings& st) {
    std::string out;
    out += "; options.ini - written by the Options screen of the pause menu (wiimote.dll).\n"
           "; [controls]: up to three inputs per action, comma separated: keyboard keys (W, SPACE, SHIFT,\n"
           ";   CONTROL and ALT (either side), RETURN, F1, NUMPAD0, 0x41 ...), MOUSE1 (left) .. MOUSE5, pad buttons\n"
           ";   PAD_A PAD_B PAD_X PAD_Y PAD_LB PAD_RB PAD_LT PAD_RT PAD_START PAD_BACK PAD_LS PAD_RS PAD_DUP\n"
           ";   PAD_DDOWN PAD_DLEFT PAD_DRIGHT (PAD_BIT<n> for other pads), sticks PAD_LS_UP PAD_LS_DOWN\n"
           ";   PAD_LS_LEFT PAD_LS_RIGHT PAD_RS_UP ..., a fast mouse wiggle MOUSE_SHAKE_X / MOUSE_SHAKE_Y, the\n"
           ";   mouse moved in circles MOUSE_CIRCLE_CW / MOUSE_CIRCLE_CCW, the scroll wheel WHEEL WHEEL_UP\n"
           ";   WHEEL_DOWN.  Empty = no input.  The shake_* and tilt_* actions move\n"
           ";   the Wii remote the Wii-only parts of the game read (SCREAM shakes it too).\n"
           "; [audio]: volumes in percent.\n"
           "; [graphics]: display = window or borderless; resolution = the height the game renders at, stretched\n"
           ";   over the window (0 = the window's size); vsync = 1 / 0 (-1 = the command line's); frame_rate = the\n"
           ";   cap in Hz, 0 = uncapped (-1 = wiimote.ini's [timing] fps_cap);\n"
           ";   high_detail = 1 / 0; texture_filter = 0 (the game's) or 2 / 4 / 8 / 16 (anisotropic);\n"
           ";   wii_effects = 1 / 0 (-1 = [video] afx).\n\n"
           "[controls]\n";
    for (int a = 0; a < N_ACTIONS; ++a) {
        out += kActions[a].key;
        out += "=";
        bool first = true;
        for (int s = 0; s < N_SLOTS; ++s) {
            if (st.bind[a][s].kind == B_NONE) continue;
            if (!first) out += ",";
            out += IniName(st.bind[a][s]);
            first = false;
        }
        out += "\n";
    }
    out += "\n[audio]\n";
    char buf[48];
    for (int i = 0; i < N_VOLUMES; ++i) {
        sprintf(buf, "%s=%d\n", kVolumeKeys[i], st.volume[i]);
        out += buf;
    }
    const GfxOptions& g = st.gfx;
    char gb[400];
    sprintf(gb, "\n[graphics]\ndisplay=%s\nresolution=%d\nvsync=%d\nframe_rate=%d\nhigh_detail=%d\n"
            "texture_filter=%d\nwii_effects=%d\n", g.display ? "borderless" : "window", g.resolution, g.vsync,
            g.frameRate, g.highDetail, g.aniso, g.wiiEffects);
    out += gb;
    return out;
}

// The sections of an options.ini this screen does not write - the launcher's [mods] (which mods are on) and [launch]
// (the game's command line) - as they stand, so saving a volume here does not throw them away.
std::string OtherSections(const std::string& path) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return "";
    std::string text;
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) text.append(buf, n);
    fclose(f);
    std::string out;
    bool keep = false;
    size_t at = 0;
    while (at <= text.size()) {
        size_t end = text.find('\n', at);
        if (end == std::string::npos) end = text.size();
        std::string line = text.substr(at, end - at);
        while (!line.empty() && (line[line.size() - 1] == '\r')) line.erase(line.size() - 1);
        std::string t = Trim(line);
        if (!t.empty() && t[0] == '[') {
            keep = _stricmp(t.c_str(), "[controls]") != 0 && _stricmp(t.c_str(), "[audio]") != 0 &&
                   _stricmp(t.c_str(), "[graphics]") != 0;
            if (keep) out += "\n" + t + "\n";
        } else if (keep && !t.empty()) {
            out += line + "\n";
        }
        if (end == text.size()) break;
        at = end + 1;
    }
    return out;
}

bool SaveSettings(const std::string& path, const Settings& st) {
    std::string text = FormatSettings(st) + OtherSections(path);
    std::string tmp = path + ".tmp";
    FILE* f = fopen(tmp.c_str(), "wb");
    if (!f) return false;
    bool ok = fwrite(text.data(), 1, text.size(), f) == text.size();
    ok = fclose(f) == 0 && ok;
    return ok && MoveFileExA(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING);
}

// ---------------------------------------------------------------------------------------------------------------
// input snapshot and actions
// ---------------------------------------------------------------------------------------------------------------
struct RawInput {
    uint8_t  keys[256];         // physical keys and mouse buttons (0x80 = down)
    int      slot;              // the first controller's slot (controller id 0), -1 = none
    int      devType;
    uint32_t padRaw;            // its raw button word this frame
    float    stick[2][2];       // left / right stick (x, y), up and right positive, dead zone applied
    bool     active;            // a game window has the focus
    float    mouse[2];          // this frame's mouse movement in heights of the view (x right, y down)
    uint8_t  wheel;             // this frame: 1 a notch up, 2 a notch down
    bool     wiggle[4];         // a fast mouse wiggle along x / y, circles clockwise / counter-clockwise (TrackMouse)
    double   wiggleHz[4];       // its rhythm, 0 = not known yet
    uint8_t  wheelActive;       // a notch within the last WHEEL_HOLD seconds: 1 the last one up, 2 down
    double   wheelHz;
};

// a fast mouse wiggle on one axis: turns of the mouse ending strokes of at least WIGGLE_TRAVEL at a peak of
// WIGGLE_SPEED; two such turns in a row start it, it stops when no turn comes for twice the time between turns
// (WIGGLE_GAP at most)
const float  WIGGLE_TRAVEL = 0.04f;     // heights of the view
const float  WIGGLE_SPEED  = 0.8f;      // heights of the view per second
const float  WIGGLE_JITTER = 0.002f;    // smaller movements do not turn a stroke
const double WIGGLE_GAP    = 0.45;
const double WHEEL_HOLD    = 0.3;
// the mouse moved in circles: the direction of movement keeps turning the same way (a wiggle reverses it)
const float  CIRCLE_SPEED  = 0.5f;      // heights of the view per second: slower movement is not part of a circle
const double CIRCLE_WINDOW = 0.9;       // seconds of movement looked at
const float  CIRCLE_TURN   = 0.85f;     // of a full turn within the window starts it
const float  CIRCLE_REVERSE = 2.0f;     // radians: a bigger change of direction in one frame is a reversal
const float  CIRCLE_ASPECT = 0.45f;     // the path's smaller extent over the larger: flatter is a wiggle
const double CIRCLE_IDLE   = 0.12;      // seconds without movement end it
enum { CIRCLE_STEPS = 96 };

struct Wiggle {
    int    dir;                 // of the current stroke, 0 = none yet
    float  travel, peak;
    int    turns;               // turns in a row that ended a fast stroke
    double lastTurn, halfPeriod;
};

struct CircleStep { double t; float turn, x, y; };
struct Circle {
    CircleStep step[CIRCLE_STEPS];
    int    first, count;
    bool   haveAngle;
    float  angle, x, y;         // the last direction of movement; the path's running position
    double lastMove;
    int    dir;                 // 1 clockwise on the screen, -1, 0 = none
    bool   pending;             // a round path turning one way by 0.4 of a turn: its strokes are not wiggles
    double hz;
};

struct MouseTrack {
    Wiggle wiggle[2];
    Circle circle;
    double last;                // time of the previous frame
    double lastNotch, notchGap;
    uint8_t lastDir;
};

MouseTrack s_mouse;

void WiggleStep(Wiggle& w, float move, double dt, double t) {
    int dir = move >= WIGGLE_JITTER ? 1 : move <= -WIGGLE_JITTER ? -1 : 0;
    if (dir != 0 && w.dir != 0 && dir != w.dir) {
        bool fast = w.travel >= WIGGLE_TRAVEL && w.peak >= WIGGLE_SPEED;
        double gap = t - w.lastTurn;
        if (fast && w.turns > 0 && gap <= WIGGLE_GAP) {
            w.halfPeriod = w.turns > 1 ? 0.5 * w.halfPeriod + 0.5 * gap : gap;
            ++w.turns;
        } else {
            w.turns = fast ? 1 : 0;
        }
        if (fast) w.lastTurn = t;
        w.travel = w.peak = 0.0f;
    }
    if (dir != 0) {
        w.dir = dir;
        w.travel += (float)fabs(move);
        float speed = dt > 0.0 ? (float)(fabs(move) / dt) : 0.0f;
        if (speed > w.peak) w.peak = speed;
    }
    double limit = w.turns >= 2 ? 2.0 * w.halfPeriod : WIGGLE_GAP;
    if (limit < 0.2) limit = 0.2;
    if (limit > WIGGLE_GAP) limit = WIGGLE_GAP;
    if (w.turns > 0 && t - w.lastTurn > limit) w.turns = 0;
}

void CircleReset(Circle& c) {
    c.first = c.count = 0;
    c.haveAngle = false;
    c.dir = 0;
    c.hz = 0.0;
    c.pending = false;
}

// the mouse's movement this frame (x right, y down: a growing angle is clockwise on the screen)
void CircleStepMouse(Circle& c, float mx, float my, double dt, double t) {
    float mag = (float)sqrt(mx * mx + my * my);
    if (dt <= 0.0 || mag < WIGGLE_JITTER || mag / dt < CIRCLE_SPEED) {
        if (c.lastMove > 0.0 && t - c.lastMove > CIRCLE_IDLE) CircleReset(c);
        return;
    }
    c.lastMove = t;
    float angle = (float)atan2(my, mx);
    c.x += mx;
    c.y += my;
    if (c.haveAngle) {
        float turn = angle - c.angle;
        if (turn > 3.14159265f) turn -= 6.2831853f;
        if (turn < -3.14159265f) turn += 6.2831853f;
        if (fabs(turn) > CIRCLE_REVERSE) {
            CircleReset(c);
        } else {
            if (c.count == CIRCLE_STEPS) c.first = (c.first + 1) % CIRCLE_STEPS, --c.count;
            CircleStep& st = c.step[(c.first + c.count) % CIRCLE_STEPS];
            st.t = t, st.turn = turn, st.x = c.x, st.y = c.y;
            ++c.count;
        }
    }
    c.angle = angle;
    c.haveAngle = true;
    while (c.count > 0 && t - c.step[c.first].t > CIRCLE_WINDOW) c.first = (c.first + 1) % CIRCLE_STEPS, --c.count;
    float sum = 0.0f, x0 = 0.0f, x1 = 0.0f, y0 = 0.0f, y1 = 0.0f;
    for (int i = 0; i < c.count; ++i) {
        const CircleStep& st = c.step[(c.first + i) % CIRCLE_STEPS];
        sum += st.turn;
        if (i == 0 || st.x < x0) x0 = st.x;
        if (i == 0 || st.x > x1) x1 = st.x;
        if (i == 0 || st.y < y0) y0 = st.y;
        if (i == 0 || st.y > y1) y1 = st.y;
    }
    float w = x1 - x0, h = y1 - y0, big = w > h ? w : h, small_ = w > h ? h : w;
    double span = c.count > 1 ? t - c.step[c.first].t : 0.0;
    bool round = big > 0.0f && small_ / big >= CIRCLE_ASPECT;
    c.pending = round && fabs(sum) >= 0.4f * 6.2831853f;
    if (fabs(sum) >= CIRCLE_TURN * 6.2831853f && round && span > 0.0) {
        c.dir = sum > 0.0f ? 1 : -1;
        c.hz = fabs(sum) / 6.2831853 / span;
    } else if (c.dir != 0 && (!round || fabs(sum) < 0.5f * 6.2831853f || (sum > 0.0f) != (c.dir > 0))) {
        c.dir = 0;
        c.hz = 0.0;
    }
}

// once per frame after the snapshot: the wiggles, the circles and the wheel's activity
void TrackMouse(MouseTrack& m, RawInput& in, double t) {
    double dt = m.last > 0.0 && t > m.last && t - m.last < 0.25 ? t - m.last : 0.0;
    m.last = t;
    for (int a = 0; a < 2; ++a) {
        Wiggle& w = m.wiggle[a];
        WiggleStep(w, in.mouse[a], dt, t);
        in.wiggle[a] = w.turns >= 2;
        in.wiggleHz[a] = in.wiggle[a] && w.halfPeriod > 0.0 ? 0.5 / w.halfPeriod : 0.0;
    }
    CircleStepMouse(m.circle, in.mouse[0], in.mouse[1], dt, t);
    in.wiggle[2] = m.circle.dir > 0;
    in.wiggle[3] = m.circle.dir < 0;
    in.wiggleHz[2] = in.wiggle[2] ? m.circle.hz : 0.0;
    in.wiggleHz[3] = in.wiggle[3] ? m.circle.hz : 0.0;
    if (m.circle.dir != 0 || m.circle.pending)      // a circle's x and y strokes are not wiggles: two new turns
        for (int a = 0; a < 2; ++a) {               // after it start one
            in.wiggle[a] = false, in.wiggleHz[a] = 0.0;
            m.wiggle[a].turns = 0;
        }
    if (in.wheel) {
        double gap = t - m.lastNotch;
        if (gap < WHEEL_HOLD) m.notchGap = m.notchGap > 0.0 ? 0.5 * m.notchGap + 0.5 * gap : gap;
        else m.notchGap = 0.0;
        m.lastNotch = t;
        m.lastDir = (in.wheel & 1) ? 1 : 2;
    }
    bool held = m.lastNotch > 0.0 && t - m.lastNotch <= WHEEL_HOLD;
    in.wheelActive = held ? m.lastDir : 0;
    in.wheelHz = held && m.notchGap > 0.0 ? 0.5 / m.notchGap : 0.0;
}

uint32_t s_padRaw[10];
uint32_t s_padFrame[10];
uint32_t s_frame;

typedef uint32_t (__cdecl* RawFn)(int slot);
typedef void (__cdecl* StickFn)(int slot, int stick, float* out);

uint32_t Capture(int slot, uint32_t v) {
    if ((unsigned)slot < 10) {
        s_padRaw[slot] = v;
        s_padFrame[slot] = s_frame;
    }
    return v;
}

uint32_t __cdecl RawDInputHook(int slot) { return Capture(slot, ((RawFn)(uintptr_t)RAW_DINPUT)(slot)); }
uint32_t __cdecl RawKpadHook(int slot) { return Capture(slot, ((RawFn)(uintptr_t)RAW_KPAD)(slot)); }
uint32_t __cdecl RawXInputHook(int slot) { return Capture(slot, ((RawFn)(uintptr_t)RAW_XINPUT)(slot)); }

bool s_realEngine = true;       // false in the self-test: no engine functions are called

void ReadStick(const OptCtx& c, int slot, int stick, float out[2]) {
    out[0] = out[1] = 0.0f;
    int type = c.devType[slot];
    if (type == 3 && s_realEngine) {
        float v[3] = { 0, 0, 0 };
        ((StickFn)(uintptr_t)XINPUT_STICK)(slot, stick, v);
        ((StickFn)(uintptr_t)STICK_NORMALISE)(slot, stick, v);
        out[0] = v[0];
        out[1] = v[1];
    } else if (c.stickEnabled[slot * 3 + stick]) {              // other devices: the engine's values when enabled
        out[0] = c.sticks[(slot * 3 + stick) * 3];
        out[1] = c.sticks[(slot * 3 + stick) * 3 + 1];
    }
}

void Snapshot(const OptCtx& c, RawInput& in) {
    memset(&in, 0, sizeof(in));
    in.slot = -1;
    in.active = *c.windowActive != 0;
    uint8_t* kb = *c.kbCur;
    if (kb) memcpy(in.keys, kb, 256);
    for (int i = 0x88; i <= 0x8F; ++i) in.keys[i] = 0;      // the virtual keys are this layer's, not physical keys
    for (int i = 0x97; i <= 0x9F; ++i) in.keys[i] = 0;
    in.keys[VK_LAYER_ON] = 0;
    if (!in.active) {
        memset(in.keys, 0, sizeof(in.keys));
        return;
    }
    float h = c.viewport[0] > 0 ? (float)c.viewport[0] : 720.0f, scale = *c.mouseScale;
    if (scale > 0.0f && scale < 1e6f)
        for (int a = 0; a < 2; ++a) {
            float d = c.mouseMove[a] * scale / h;
            in.mouse[a] = d == d && d > -100.0f && d < 100.0f ? d : 0.0f;
        }
    in.wheel = (uint8_t)(((*c.mouseCur & 0x08) ? 1 : 0) | ((*c.mouseCur & 0x10) ? 2 : 0));
    int slot = c.slotOfId[0];
    if ((unsigned)slot < 10 && c.connected[slot]) {
        in.slot = slot;
        in.devType = c.devType[slot];
        in.padRaw = s_padFrame[slot] == s_frame ? s_padRaw[slot] : 0;
        ReadStick(c, slot, 0, in.stick[0]);
        ReadStick(c, slot, 1, in.stick[1]);
    }
}

// motion: the value of a motion row (wm_motion.cpp): the wheel is held while its notches keep coming
float BindValue(const Bind& b, const RawInput& in, bool motion = false) {
    switch (b.kind) {
    case B_MOUSEMOVE:
        return b.code < N_MOUSEMOVES && in.wiggle[b.code] ? 1.0f : 0.0f;
    case B_WHEEL:
        if (motion) return in.wheelActive && (b.code == 0 || b.code == in.wheelActive) ? 1.0f : 0.0f;
        return (b.code == 0 ? in.wheel != 0 : (in.wheel & b.code) != 0) ? 1.0f : 0.0f;
    case B_KEY:
        return (in.keys[b.code] & 0x80) ? 1.0f : 0.0f;
    case B_MOUSE:
        return b.code <= 5 && (in.keys[kMouseVk[b.code]] & 0x80) ? 1.0f : 0.0f;
    case B_PAD:
        return in.slot >= 0 && b.code < 32 && ((in.padRaw >> b.code) & 1) ? 1.0f : 0.0f;
    case B_AXIS: {
        if (in.slot < 0 || b.code >= 8) return 0.0f;
        const float* s = in.stick[b.code / 4];
        float v = 0.0f;
        switch (b.code % 4) {
        case 0: v = s[1]; break;
        case 1: v = -s[1]; break;
        case 2: v = -s[0]; break;
        default: v = s[0]; break;
        }
        return v > 0.0f ? (v > 1.0f ? 1.0f : v) : 0.0f;
    }
    default:
        return 0.0f;
    }
}

struct ActionState {
    float value[N_ACTIONS];     // strongest binding, 0..1
    bool  down[N_ACTIONS];      // value > 0.5
    bool  runPad;               // RUN held on a pad binding
    bool  runOther;             // RUN held on a keyboard / mouse binding
};

void Evaluate(const Settings& st, const RawInput& in, ActionState& out) {
    memset(&out, 0, sizeof(out));
    for (int a = 0; a < N_ACTIONS; ++a) {
        for (int s = 0; s < N_SLOTS; ++s) {
            const Bind& b = st.bind[a][s];
            float v = BindValue(b, in, kActions[a].vk == 0);
            if (v > out.value[a]) out.value[a] = v;
            if (a == 5 && v > 0.5f) (b.kind == B_PAD || b.kind == B_AXIS ? out.runPad : out.runOther) = true;
        }
        out.down[a] = out.value[a] > 0.5f;
    }
}

// the scripts' view: virtual keys, and the first pad's left stick = the movement actions
void WriteGame(const OptCtx& c, const ActionState& as, const RawInput& in) {
    uint8_t* kb = *c.kbCur;
    if (!kb) return;
    kb[VK_LAYER_ON] = 0x80;
    for (int a = 0; a < N_ACTIONS; ++a) {
        const ActionDef& d = kActions[a];
        if (!d.vk) continue;                                        // the remote's motion (FeedMotion)
        if (d.vkPad) {
            kb[d.vk] = as.runOther ? 0x80 : 0;
            kb[d.vkPad] = as.runPad ? 0x80 : 0;
        } else {
            kb[d.vk] = as.down[a] ? 0x80 : 0;
        }
    }
    if (in.slot >= 0 && c.stickEnabled[in.slot * 3]) {
        float* s = &c.sticks[in.slot * 3 * 3];
        s[0] = as.value[A_RIGHT] - as.value[A_LEFT];
        s[1] = as.value[A_FORWARD] - as.value[A_BACK];
        s[2] = 0.0f;
    }
}

// A mouse button bound to a tilt row also clicks and drags (Inside Zee Wiimote: left click = A, right click = B, the
// pointer grabs and drags with A held): it tilts the remote only when it is held with the mouse still - TILT_STILL_SECS
// with less than TILT_STILL_TRAVEL of movement - and from then on until it is released, so a tilted remote can be
// shaken with the mouse.  Keys and pad buttons tilt at once.
const double TILT_STILL_SECS   = 0.25;
const float  TILT_STILL_TRAVEL = 0.03f;     // heights of the view

struct TiltHold { bool down, engaged, rejected; double since; float travel; };
TiltHold s_tiltHold[2][N_SLOTS];

bool TiltDown(const Settings& st, int action, TiltHold hold[N_SLOTS], const RawInput& in, double t) {
    bool down = false;
    for (int s = 0; s < N_SLOTS; ++s) {
        const Bind& b = st.bind[action][s];
        TiltHold& h = hold[s];
        bool held = BindValue(b, in, true) > 0.5f;
        if (b.kind != B_MOUSE || !held) {
            memset(&h, 0, sizeof(h));
            down = down || held;
            continue;
        }
        if (!h.down) {
            memset(&h, 0, sizeof(h));
            h.down = true;
            h.since = t;
        }
        if (!h.engaged && !h.rejected) {
            h.travel += (float)(fabs(in.mouse[0]) + fabs(in.mouse[1]));
            if (h.travel > TILT_STILL_TRAVEL) h.rejected = true;
            else if (t - h.since >= TILT_STILL_SECS) h.engaged = true;
        }
        down = down || h.engaged;
    }
    return down;
}

// the remote's motion from the actions: SCREAM shakes it and the Nunchuk, the motion rows shake and tilt it; a shake
// driven by a mouse wiggle or the wheel takes its rhythm.  on = false: the page has the input, the remote settles.
void FeedMotion(const Settings& st, const ActionState& as, const RawInput& in, bool on, double t) {
    MotionInput m;
    memset(&m, 0, sizeof(m));
    m.t = t;
    m.on = on;
    if (on) {
        m.attack = as.down[A_SCREAM];
        for (int k = 0; k < MOTION_AXES; ++k) {
            int a = A_SHAKE_X + k;
            m.shake[k] = as.down[a];
            for (int s = 0; s < N_SLOTS && m.shake[k]; ++s) {
                const Bind& b = st.bind[a][s];
                if (b.kind == B_MOUSEMOVE && b.code < N_MOUSEMOVES && in.wiggle[b.code] && in.wiggleHz[b.code] > 0.0) {
                    m.hz[k] = in.wiggleHz[b.code];
                    break;
                }
                if (b.kind == B_WHEEL && BindValue(b, in, true) > 0.5f && in.wheelHz > 0.0) {
                    m.hz[k] = in.wheelHz;
                    break;
                }
            }
        }
        m.tiltLeft = TiltDown(st, A_TILT_LEFT, s_tiltHold[0], in, t);
        m.tiltRight = TiltDown(st, A_TILT_RIGHT, s_tiltHold[1], in, t);
        m.noPad = in.slot < 0;
        m.move[0] = as.value[A_RIGHT] - as.value[A_LEFT];
        m.move[1] = as.value[A_FORWARD] - as.value[A_BACK];
        bool cw = as.down[A_ROTATE_CW], ccw = as.down[A_ROTATE_CCW];
        m.rotate = cw && !ccw ? 1 : ccw && !cw ? -1 : 0;
        for (int s = 0; s < N_SLOTS && m.rotate != 0; ++s) {
            const Bind& b = st.bind[m.rotate > 0 ? A_ROTATE_CW : A_ROTATE_CCW][s];
            if (b.kind == B_MOUSEMOVE && b.code < N_MOUSEMOVES && in.wiggle[b.code] && in.wiggleHz[b.code] > 0.0) {
                m.rotateHz = in.wiggleHz[b.code];
                break;
            }
        }
    } else {
        memset(s_tiltHold, 0, sizeof(s_tiltHold));
    }
    // the log names each motion input as it starts and stops (bit per input; the rhythm when one drives it)
    static unsigned s_logged;
    unsigned now = (m.attack ? 1u : 0u) | (m.shake[0] ? 2u : 0u) | (m.shake[1] ? 4u : 0u) | (m.shake[2] ? 8u : 0u) |
                   (m.tiltLeft ? 16u : 0u) | (m.tiltRight ? 32u : 0u) | (m.rotate > 0 ? 64u : 0u) |
                   (m.rotate < 0 ? 128u : 0u);
    if (now != s_logged && s_realEngine) {
        const char* const names[8] = { "SCREAM shake", "SHAKE LEFT/RIGHT", "SHAKE UP/DOWN", "MARACA SHAKE", "TILT LEFT",
                                       "TILT RIGHT", "SHAKE CLOCKWISE", "SHAKE COUNTER-CW" };
        for (int i = 0; i < 8; ++i) {
            unsigned bit = 1u << i;
            if ((now ^ s_logged) & bit) {
                double hz = i >= 1 && i <= 3 ? m.hz[i - 1] : i >= 6 ? m.rotateHz : 0.0;
                if ((now & bit) && hz > 0.0) OptLog("motion: %s on (input rhythm %.1f Hz)", names[i], hz);
                else OptLog("motion: %s %s", names[i], (now & bit) ? "on" : "off");
            }
        }
        s_logged = now;
    }
    MotionFrame(m);
}

// nothing for the game: keys (the layer flag stays on), mouse buttons, pad buttons and sticks
void MaskGame(const OptCtx& c) {
    uint8_t* kb = *c.kbCur;
    if (kb) {
        memset(kb, 0, 256);
        kb[VK_LAYER_ON] = 0x80;
    }
    *c.mouseCur = 0;
    for (int slot = 0; slot < 10; ++slot) {
        if (!c.connected[slot]) continue;
        c.joyMask[slot * 0xA4 / 4] = 0;
        for (int i = 0; i < 9; ++i) c.sticks[slot * 9 + i] = 0.0f;
    }
}

bool AnyDown(const RawInput& in) {
    for (int i = 1; i < 256; ++i)
        if (in.keys[i] & 0x80) return true;
    if (in.slot >= 0) {
        if (in.padRaw) return true;
        for (int s = 0; s < 2; ++s)
            if (fabs(in.stick[s][0]) > 0.5f || fabs(in.stick[s][1]) > 0.5f) return true;
    }
    return false;
}

// ---------------------------------------------------------------------------------------------------------------
// volumes
// ---------------------------------------------------------------------------------------------------------------
enum { SND_MANAGER_OFF = 0xA28, SND_OFFSETS = 0x1000, SND_DEFS = 0x1100, SND_DEF_SIZE = 0x2C, SND_GROUPS = 64 };
enum { G_MASTER = 0, G_USER_SFX = 1, G_USER_MUSIC = 2, G_SFX = 10, G_AMB = 11, G_MUSIC = 12, G_DIALOG = 13,
       G_HUD = 14 };

// percent -> millibels: 40 log10(percent / 100) dB x 100 (the gain squared, so the slider sounds even); 0 % = silence
int VolumeToMb(int percent) {
    if (percent <= 0) return -10000;
    if (percent >= 100) return 0;
    double mb = 4000.0 * log10((double)percent / 100.0);
    return mb < -10000.0 ? -10000 : (int)floor(mb + 0.5);
}

struct SoundGroups {
    int list[N_VOLUMES][4];     // groups per slider, -1 terminated
    bool resolved;
};

SoundGroups s_groups;
int s_appliedMb[SND_GROUPS];
bool s_appliedKnown[SND_GROUPS];

uint32_t SoundManager(const OptCtx& c) {
    uint32_t g = *c.sndGlobal;
    return g ? *(uint32_t*)(uintptr_t)(g + SND_MANAGER_OFF) : 0;
}

bool IsBelow(uint32_t defs, int group, int ancestor) {
    for (int i = 0, g = group; i < SND_GROUPS && g >= 0 && g < SND_GROUPS; ++i) {
        int parent = *(int32_t*)(uintptr_t)(defs + g * SND_DEF_SIZE + 4);
        if (parent == ancestor) return true;
        if (parent == g) return false;
        g = parent;
    }
    return false;
}

void ResolveGroups(uint32_t mgr) {
    uint32_t defs = *(uint32_t*)(uintptr_t)(mgr + SND_DEFS);
    if (!defs) return;
    std::string tree;
    char buf[40];
    for (int g = 0; g < SND_GROUPS; ++g) {
        int parent = *(int32_t*)(uintptr_t)(defs + g * SND_DEF_SIZE + 4);
        int base = *(int32_t*)(uintptr_t)(defs + g * SND_DEF_SIZE + 8);
        if (parent == g && base == 0 && g > 0) continue;
        sprintf(buf, " %d<-%d(%d)", g, parent, base);
        tree += buf;
    }
    OptLog("sound groups (group<-parent(base mB)):%s", tree.c_str());
    const int master[] = { G_MASTER, -1 };
    const int music[] = { IsBelow(defs, G_MUSIC, G_USER_MUSIC) ? G_USER_MUSIC : G_MUSIC, -1 };
    const int fxUser[] = { G_USER_SFX, -1 };
    const int fxEach[] = { G_SFX, G_AMB, G_HUD, -1 };
    const int voices[] = { G_DIALOG, -1 };
    bool userSfx = IsBelow(defs, G_SFX, G_USER_SFX) && !IsBelow(defs, G_DIALOG, G_USER_SFX);
    const int* picks[N_VOLUMES] = { master, music, userSfx ? fxUser : fxEach, voices };
    for (int v = 0; v < N_VOLUMES; ++v)
        for (int i = 0; i < 4; ++i) {
            s_groups.list[v][i] = picks[v][i];
            if (picks[v][i] < 0) break;
        }
    s_groups.resolved = true;
    std::string pick;
    for (int v = 0; v < N_VOLUMES; ++v) {
        sprintf(buf, " %s:", kVolumeKeys[v]);
        pick += buf;
        for (int i = 0; i < 4 && s_groups.list[v][i] >= 0; ++i) {
            sprintf(buf, "%s%d", i ? "," : "", s_groups.list[v][i]);
            pick += buf;
        }
    }
    OptLog("volume groups:%s", pick.c_str());
}

typedef void (__cdecl* GroupSetFn)(int group, float db);

bool s_musicSilenced;                           // wm_sm64.cpp: SM64's music is playing instead
bool s_effectsSilenced;                         // and its effects, while Mario has the level
bool s_voicesSilenced;                          // and the rabbids' own voices, which are the dialogue group
int s_extraSilent[8];                           // groups silenced outright (the rabbids' own, while Mario plays)
int s_extraSilentN;

void ApplyVolumes(const OptCtx& c, const Settings& st, bool force) {
    uint32_t mgr = SoundManager(c);
    if (!mgr) return;
    if (!s_groups.resolved) ResolveGroups(mgr);
    if (!s_groups.resolved) return;
    for (int v = 0; v < N_VOLUMES; ++v) {
        bool off = (v == 1 && s_musicSilenced) || (v == 2 && s_effectsSilenced) || (v == 3 && s_voicesSilenced);
        int mb = VolumeToMb(off ? 0 : st.volume[v]);
        for (int i = 0; i < 4 && s_groups.list[v][i] >= 0; ++i) {
            int g = s_groups.list[v][i];
            int cur = *(int32_t*)(uintptr_t)(mgr + SND_OFFSETS + g * 4);
            if (!force && s_appliedKnown[g] && s_appliedMb[g] == mb && cur == mb) continue;
            ((GroupSetFn)(uintptr_t)SND_GROUP_SET)(g, (float)mb / 100.0f);
            s_appliedMb[g] = mb;
            s_appliedKnown[g] = true;
        }
    }
    // Groups silenced outright, whatever slider they belong to: the rabbids' own noises while Mario is the one
    // being played.  Their sounds are not the dialogue group - that is the game's speech - but the mix's own
    // Characters group (24) under effects, so silencing it leaves the level's noises alone.
    for (int i = 0; i < s_extraSilentN; ++i) {
        int g = s_extraSilent[i];
        if (g < 0 || g >= SND_GROUPS) continue;
        ((GroupSetFn)(uintptr_t)SND_GROUP_SET)(g, (float)VolumeToMb(0) / 100.0f);
        s_appliedKnown[g] = false;               // the slider pass puts it back when the list is cleared
    }
}

// ---------------------------------------------------------------------------------------------------------------
// the page (element names and layout: rghport/convert/menus.py)
// ---------------------------------------------------------------------------------------------------------------
const char* const PAGE_NAME = "PCOPT_P_Options";
enum { ROWS = 8, AUDIO_ROWS = N_VOLUMES };
enum { P_STRING = 21, P_POSITION = 32, P_SIZE = 33, P_SCALE = 34, P_FRAME = 77, P_VISIBLE = 115 };
enum { F_SELECTED = 0, F_NORMAL = 10, F_GREY_SELECTED = 40, F_GREY = 50 };

enum { N_TABS = 3 };
const double TAB_SCALE = 0.8, ROW_SCALE = 0.6, AUDIO_SCALE = 0.8, FOOTER_SCALE = 0.75, GFX_NAME_SCALE = 0.75,
             GFX_VALUE_SCALE = 0.85;
const int TAB_CX[N_TABS] = { 200, 427, 654 }, TAB_CY = 42;
const char* const kTabNames[N_TABS] = { "TabControls", "TabAudio", "TabGraphics" };
const int ROW_Y0 = 102, ROW_DY = 37, SLOT_X0 = 420, SLOT_DX = 132;
const int AUDIO_Y0 = 112, AUDIO_DY = 62, AUDIO_LABEL_CX = 230, BAR_X0 = 360, BAR_X1 = 680;
const int GFX_Y0 = 100, GFX_DY = 44, GFX_NAME_CX = 250, GFX_VALUE_CX = 575;
const int FOOTER_POS[2][2] = { { 249, 412 }, { 503, 412 } };

enum { GR_DISPLAY, GR_RES, GR_VSYNC, GR_FPS, GR_DETAIL, GR_FILTER, GR_AFX, GFX_ROWS };
const wchar_t* const kGfxLabels[GFX_ROWS] = { L"DISPLAY", L"RESOLUTION", L"VSYNC", L"FRAME RATE", L"HIGH DETAIL",
                                              L"TEXTURES",
                                              L"WII EFFECTS" };
const int kHeights[] = { 0, 360, 480, 540, 720, 900, 1080, 1440, 2160 };   // 0 = the window's size
enum { N_HEIGHTS = sizeof(kHeights) / sizeof(kHeights[0]) };
const int kAniso[] = { 0, 2, 4, 8, 16 };
const int kRates[] = { 30, 60, 120, 144, 165, 240, 0 };                      // Hz; 0 = uncapped
enum { N_RATES = sizeof(kRates) / sizeof(kRates[0]) };

uint32_t Crc(const char* s) { return Crc32((const uint8_t*)s, strlen(s)); }

uint32_t Gid(const char* fmt, ...) {
    char name[64] = "PCOPT_G_";
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf(name + 8, sizeof(name) - 9, fmt, ap);
    va_end(ap);
    return Crc(name);
}

struct Rect { double x0, x1, y0, y1; };

bool Inside(const Rect& r, double x, double y) { return x >= r.x0 && x <= r.x1 && y >= r.y0 && y <= r.y1; }

void LabelPos(double cx, double cy, double s, int& px, int& py) {
    px = (int)floor(cx - 68.0 * s + 0.5);
    py = (int)floor(cy - 24.0 * s + 0.5);
}

Rect LabelRect(int px, int py, double s) { Rect r = { px - 30 * s, px + 166 * s, py - 2 * s, py + 50 * s }; return r; }

// Magma calls, with the last value per property kept so unchanged values are not sent again (a frame index sent
// again would restart the label's animation)
typedef void (__cdecl* SetIntFn)(long, long, long, long, long);
typedef void (__cdecl* SetFloatFn)(long, long, long, float, long);
typedef void (__cdecl* SetWStrFn)(long, long, long, const wchar_t*, long);
typedef long (__cdecl* GetIntFn)(long, long, long);
typedef void (__cdecl* PopPageFn)(long, long, long);
typedef void (__cdecl* PopCursorFn)(const char*);
typedef long (__cdecl* TopPageFn)();

std::map<uint64_t, double> s_numCache;
std::map<uint32_t, std::wstring> s_textCache;
std::vector<std::string>* s_testCalls;   // self-test: the Magma calls made

uint64_t CacheKey(uint32_t id, int prop, int idx) {
    return ((uint64_t)id << 32) | ((uint32_t)prop << 8) | (uint32_t)idx;
}

void SetInt(uint32_t id, int prop, int idx, int v, bool always = false) {
    uint64_t k = CacheKey(id, prop, idx);
    std::map<uint64_t, double>::iterator it = s_numCache.find(k);
    if (!always && it != s_numCache.end() && it->second == (double)v) return;
    s_numCache[k] = v;
    if (s_testCalls) {
        char buf[64];
        sprintf(buf, "int %08X %d %d %d", id, prop, idx, v);
        s_testCalls->push_back(buf);
        return;
    }
    ((SetIntFn)(uintptr_t)MGM_SET_INT)((long)id, prop, idx, v, 1);
}

void SetFloat(uint32_t id, int prop, int idx, float v) {
    uint64_t k = CacheKey(id, prop, idx);
    std::map<uint64_t, double>::iterator it = s_numCache.find(k);
    if (it != s_numCache.end() && it->second == (double)v) return;
    s_numCache[k] = v;
    if (s_testCalls) {
        char buf[64];
        sprintf(buf, "float %08X %d %d %.3f", id, prop, idx, v);
        s_testCalls->push_back(buf);
        return;
    }
    ((SetFloatFn)(uintptr_t)MGM_SET_FLOAT)((long)id, prop, idx, v, 1);
}

void SetText(uint32_t id, const std::wstring& text) {
    std::map<uint32_t, std::wstring>::iterator it = s_textCache.find(id);
    if (it != s_textCache.end() && it->second == text) return;
    s_textCache[id] = text;
    if (s_testCalls) {
        char buf[96];
        sprintf(buf, "text %08X %ls", id, text.c_str());
        s_testCalls->push_back(buf);
        return;
    }
    ((SetWStrFn)(uintptr_t)MGM_SET_WSTRING)((long)id, P_STRING, 0, text.c_str(), 1);
}

void SetVisible(uint32_t id, bool on) { SetInt(id, P_VISIBLE, 0, on ? 1 : 0); }

// ---------------------------------------------------------------------------------------------------------------
// the page's state and behaviour
// ---------------------------------------------------------------------------------------------------------------
enum Zone { Z_TABS, Z_LIST, Z_FOOTER };
enum { TAB_CONTROLS, TAB_AUDIO, TAB_GRAPHICS };

struct UiInput {                // this frame's page commands
    bool up, down, left, right; // with repeat
    bool ok, back, clear, tabPrev, tabNext;
    bool mouseMoved, click;
    double mx, my;              // pointer in page units (854 x 480); < 0 = outside
};

struct Ui {
    bool   open;
    bool   armed;               // every input was released since the page opened
    bool   capturing, captureArmed;
    int    capAction, capSlot;
    double capStart;
    int    tab, zone, col, row, scroll;
    bool   dirty;
    double holdStart, lastRepeat;
    int    holdDir;             // 0 none, 1 up, 2 down, 3 left, 4 right
    double lastMx, lastMy;
    bool   dragBar;
    bool   pcControls;          // [controls] mode=pc: the controls tab edits bindings
};

Ui s_ui;
Settings s_editStart;           // the settings when the page opened (saved on close when they differ)
bool s_swallow;                 // after the page: no input for the game until everything is released
double s_swallowStart;
RawInput s_in, s_prevIn;
ActionState s_actions;
bool s_layerOn;                 // input layer installed ([controls] mode=pc, verified)
bool s_pageOn;                  // page driver installed
uint32_t s_pageId;

bool Pressed(const RawInput& now, const RawInput& before, int vk) {
    return (now.keys[vk] & 0x80) && !(before.keys[vk] & 0x80);
}

bool PadPressed(const RawInput& now, const RawInput& before, int bit) {
    return now.slot >= 0 && ((now.padRaw >> bit) & 1) && !(before.slot >= 0 && ((before.padRaw >> bit) & 1));
}

int StickDir(const RawInput& in) {
    if (in.slot < 0) return 0;
    float x = in.stick[0][0], y = in.stick[0][1];
    if (fabs(x) < 0.55f && fabs(y) < 0.55f) return 0;
    if (fabs(y) >= fabs(x)) return y > 0 ? 1 : 2;
    return x < 0 ? 3 : 4;
}

int HeldDir(const RawInput& in) {
    if (in.keys[VK_UP] & 0x80) return 1;
    if (in.keys[VK_DOWN] & 0x80) return 2;
    if (in.keys[VK_LEFT] & 0x80) return 3;
    if (in.keys[VK_RIGHT] & 0x80) return 4;
    if (in.slot >= 0) {
        if (in.padRaw & 1) return 1;
        if (in.padRaw & 2) return 2;
        if (in.padRaw & 4) return 3;
        if (in.padRaw & 8) return 4;
    }
    return StickDir(in);
}

bool PagePointer(const OptCtx& c, double& mx, double& my) {
    int32_t h = c.viewport[0], w = c.viewport[1];
    if (w <= 0 || h <= 0) return false;
    mx = (double)(c.cursor[0] - c.viewport[3]) * 854.0 / (double)w;
    my = (double)(c.cursor[1] - c.viewport[2]) * 480.0 / (double)h;
    return mx >= 0 && mx <= 854 && my >= 0 && my <= 480;
}

void ReadUi(const OptCtx& c, const RawInput& now, const RawInput& before, double t, UiInput& u) {
    memset(&u, 0, sizeof(u));
    int dir = HeldDir(now);
    if (dir != s_ui.holdDir) {
        s_ui.holdDir = dir;
        s_ui.holdStart = s_ui.lastRepeat = t;
        if (dir == 1) u.up = true;
        if (dir == 2) u.down = true;
        if (dir == 3) u.left = true;
        if (dir == 4) u.right = true;
    } else if (dir && t - s_ui.holdStart > 0.4 && t - s_ui.lastRepeat > 0.09) {
        s_ui.lastRepeat = t;
        if (dir == 1) u.up = true;
        if (dir == 2) u.down = true;
        if (dir == 3) u.left = true;
        if (dir == 4) u.right = true;
    }
    u.ok = Pressed(now, before, VK_RETURN) || Pressed(now, before, VK_SPACE) || PadPressed(now, before, 12);
    u.back = Pressed(now, before, VK_ESCAPE) || Pressed(now, before, VK_BACK) || PadPressed(now, before, 13) ||
             PadPressed(now, before, 4) || Pressed(now, before, VK_RBUTTON);
    u.clear = Pressed(now, before, VK_DELETE) || PadPressed(now, before, 14);
    bool shift = (now.keys[VK_SHIFT] & 0x80) || (now.keys[VK_LSHIFT] & 0x80) || (now.keys[VK_RSHIFT] & 0x80);
    u.tabNext = (Pressed(now, before, VK_TAB) && !shift) || PadPressed(now, before, 9);
    u.tabPrev = (Pressed(now, before, VK_TAB) && shift) || PadPressed(now, before, 8);
    u.click = Pressed(now, before, VK_LBUTTON);
    u.mx = u.my = -1;
    double mx, my;
    if (PagePointer(c, mx, my)) {
        u.mx = mx;
        u.my = my;
        u.mouseMoved = fabs(mx - s_ui.lastMx) > 0.5 || fabs(my - s_ui.lastMy) > 0.5;
        s_ui.lastMx = mx;
        s_ui.lastMy = my;
    }
}

int ListRows() { return s_ui.tab == TAB_CONTROLS ? N_ACTIONS : s_ui.tab == TAB_AUDIO ? AUDIO_ROWS : GFX_ROWS; }

// ---------------------------------------------------------------------------------------------------------------
// graphics settings: values shown and changes (wm_gfx.cpp applies them)
// ---------------------------------------------------------------------------------------------------------------
int CurrentVsync() { return s_set.gfx.vsync >= 0 ? s_set.gfx.vsync : GfxVsync(); }
double CurrentRate() { return s_set.gfx.frameRate >= 0 ? (double)s_set.gfx.frameRate : TimingIniCap(); }
bool AfxOn() { return s_set.gfx.wiiEffects >= 0 ? s_set.gfx.wiiEffects != 0 : AfxUserEnabled(); }

bool GfxRowAvailable(int row) {
    if (row == GR_FPS) return TimingInstalled();
    if (!GfxInstalled() && row != GR_AFX) return false;
    if ((row == GR_DISPLAY || row == GR_RES) && !GfxWindowed()) return false;
    if (row == GR_RES) return GfxResolutionInstalled();
    if (row == GR_AFX) return AfxInstalled();
    return true;
}

std::wstring GfxValue(int row) {
    wchar_t buf[40];
    const GfxOptions& g = s_set.gfx;
    switch (row) {
    case GR_DISPLAY:
        return g.display ? L"BORDERLESS" : L"WINDOW";
    case GR_RES: {
        if (!g.resolution) return L"NATIVE";
        int w, h;
        if (GfxRenderSize(g.resolution, w, h)) swprintf(buf, 40, L"%d X %d", w, h);
        else swprintf(buf, 40, L"%dP", g.resolution);
        return buf;
    }
    case GR_VSYNC:
        return CurrentVsync() ? L"ON" : L"OFF";
    case GR_FPS:
        if (!(CurrentRate() > 0.0)) return L"UNCAPPED";
        swprintf(buf, 40, L"%d FPS", (int)floor(CurrentRate() + 0.5));
        return buf;
    case GR_DETAIL:
        return g.highDetail ? L"ON" : L"OFF";
    case GR_FILTER:
        if (!g.aniso) return L"BILINEAR";
        swprintf(buf, 40, L"ANISO %dX", g.aniso);
        return buf;
    default:
        return AfxInstalled() ? (AfxOn() ? L"ON" : L"OFF") : L"-";
    }
}

void GfxChange(int row, int dir) {
    GfxOptions& g = s_set.gfx;
    switch (row) {
    case GR_DISPLAY:
        g.display = !g.display;
        GfxSetBorderless(g.display != 0);
        break;
    case GR_RES: {
        int i = 0;                                  // the preset at or just below the current height
        for (int k = 0; k < N_HEIGHTS; ++k)
            if (kHeights[k] <= g.resolution) i = k;
        i = (i + (dir > 0 ? 1 : N_HEIGHTS - 1)) % N_HEIGHTS;
        g.resolution = kHeights[i];
        GfxSetResolution(g.resolution);
        break;
    }
    case GR_VSYNC:
        g.vsync = CurrentVsync() ? 0 : 1;
        GfxSetVsync(g.vsync);
        break;
    case GR_FPS: {
        double cur = CurrentRate();
        int i = N_RATES - 1;                        // uncapped, or the preset at or just below the current rate
        if (cur > 0.0)
            for (int k = 0; k < N_RATES - 1; ++k)
                if (kRates[k] <= cur + 0.5) i = k;
        if (cur > 0.0 && i == N_RATES - 1) i = 0;
        i = (i + (dir > 0 ? 1 : N_RATES - 1)) % N_RATES;
        g.frameRate = kRates[i];
        TimingSetCap(g.frameRate);
        break;
    }
    case GR_DETAIL:
        g.highDetail = !g.highDetail;
        GfxSetHighDetail(g.highDetail != 0);
        break;
    case GR_FILTER: {
        int i = 0;
        while (i < 4 && kAniso[i] != g.aniso) ++i;
        i = (i + (dir > 0 ? 1 : 4)) % 5;
        g.aniso = kAniso[i];
        GfxSetAniso(g.aniso);
        break;
    }
    default:
        g.wiiEffects = AfxOn() ? 0 : 1;
        AfxSetUserEnabled(g.wiiEffects != 0);
        break;
    }
}

// the saved graphics settings, once the game window and the renderer exist
bool s_gfxApplied;

void ApplyStartupGraphics() {
    const GfxOptions& g = s_set.gfx;
    GfxSetHighDetail(g.highDetail != 0);
    GfxSetAniso(g.aniso);
    if (g.wiiEffects >= 0) AfxSetUserEnabled(g.wiiEffects != 0);
    if (g.display) GfxSetBorderless(true);
    GfxSetResolution(g.resolution);
    if (g.frameRate >= 0) TimingSetCap(g.frameRate);
    if (g.vsync >= 0) GfxSetVsync(g.vsync);
    s_gfxApplied = true;
}

void KeepVisible() {
    if (s_ui.tab != TAB_CONTROLS) {
        s_ui.scroll = 0;
        return;
    }
    if (s_ui.row < s_ui.scroll) s_ui.scroll = s_ui.row;
    if (s_ui.row >= s_ui.scroll + ROWS) s_ui.scroll = s_ui.row - ROWS + 1;
    if (s_ui.scroll < 0) s_ui.scroll = 0;
    if (s_ui.scroll > N_ACTIONS - ROWS) s_ui.scroll = N_ACTIONS - ROWS > 0 ? N_ACTIONS - ROWS : 0;
}

void SetTab(int tab) {
    if (tab == s_ui.tab) return;
    s_ui.tab = tab;
    s_ui.row = 0;
    s_ui.col = s_ui.zone == Z_TABS ? tab : 0;
    s_ui.scroll = 0;
    s_ui.dirty = true;
}

void Move(int dir) {
    s_ui.dirty = true;
    int rows = ListRows();
    if (s_ui.tab == TAB_CONTROLS && !s_ui.pcControls) rows = 0;
    switch (s_ui.zone) {
    case Z_TABS:
        if (dir == 3 || dir == 4) {
            SetTab((s_ui.tab + (dir == 3 ? N_TABS - 1 : 1)) % N_TABS);
            s_ui.col = s_ui.tab;
        } else if (dir == 2) {
            s_ui.zone = rows ? Z_LIST : Z_FOOTER;
            s_ui.row = 0;
            s_ui.col = rows ? 0 : 1;
        }
        break;
    case Z_LIST:
        if (dir == 1) {
            if (s_ui.row > 0) {
                --s_ui.row;
            } else {
                s_ui.zone = Z_TABS;
                s_ui.col = s_ui.tab;
            }
        } else if (dir == 2) {
            if (s_ui.row < rows - 1) {
                ++s_ui.row;
            } else {
                s_ui.zone = Z_FOOTER;
                s_ui.col = 1;
            }
        } else if (s_ui.tab == TAB_CONTROLS) {
            if (dir == 3 && s_ui.col > 0) --s_ui.col;
            if (dir == 4 && s_ui.col < N_SLOTS - 1) ++s_ui.col;
        } else if (s_ui.tab == TAB_AUDIO) {
            int& v = s_set.volume[s_ui.row];
            v += dir == 4 ? 5 : -5;
            v = v < 0 ? 0 : v > 100 ? 100 : v;
        } else if (GfxRowAvailable(s_ui.row)) {
            GfxChange(s_ui.row, dir == 4 ? 1 : -1);
        }
        break;
    case Z_FOOTER:
        if (dir == 3 || dir == 4) {
            s_ui.col = dir == 3 ? 0 : 1;
        } else if (dir == 1) {
            s_ui.zone = rows ? Z_LIST : Z_TABS;
            s_ui.row = rows ? rows - 1 : 0;
            s_ui.col = rows ? 0 : s_ui.tab;
        }
        break;
    }
    KeepVisible();
}

void StartCapture(int action, int slot, double t) {
    s_ui.capturing = true;
    s_ui.captureArmed = false;
    s_ui.capAction = action;
    s_ui.capSlot = slot;
    s_ui.capStart = t;
    s_ui.dirty = true;
}

// the first new input while capturing (keys, mouse buttons, the wheel, a mouse wiggle, pad buttons, stick directions);
// motion: a motion row (the wheel either way)
bool CaptureInput(const RawInput& now, const RawInput& before, Bind& out, bool motion = false) {
    if (Pressed(now, before, VK_MENU)) {             // Alt (AltGr also presses a Ctrl)
        out.kind = B_KEY;
        out.code = VK_MENU;
        return true;
    }
    for (int vk = 1; vk < 256; ++vk) {
        if ((vk >= 0xA0 && vk <= 0xA5) || vk == VK_ESCAPE) continue;   // Shift / Ctrl / Alt: the combined codes
        if (!Pressed(now, before, vk)) continue;
        for (int m = 1; m <= 5; ++m)
            if (vk == kMouseVk[m]) { out.kind = B_MOUSE; out.code = (uint8_t)m; return true; }
        if (vk == 0x03 || vk == 0x07 || (vk >= 0x88 && vk <= 0x8F) || (vk >= 0x97 && vk <= 0x9F)) continue;
        out.kind = B_KEY;
        out.code = (uint8_t)vk;
        return true;
    }
    if (now.wheel) {
        out.kind = B_WHEEL;
        out.code = (uint8_t)(motion ? 0 : (now.wheel & 1) ? 1 : 2);
        return true;
    }
    for (int a = N_MOUSEMOVES - 1; a >= 0; --a)
        if (now.wiggle[a] && !before.wiggle[a]) {
            out.kind = B_MOUSEMOVE;
            out.code = (uint8_t)a;
            return true;
        }
    if (now.slot >= 0) {
        for (int bit = 0; bit < 32; ++bit)
            if (PadPressed(now, before, bit)) { out.kind = B_PAD; out.code = (uint8_t)bit; return true; }
        for (int s = 0; s < 2; ++s)
            for (int d = 0; d < 4; ++d) {
                Bind b = { B_AXIS, (uint8_t)(s * 4 + d) };
                if (BindValue(b, now) > 0.6f && BindValue(b, before) <= 0.6f) { out = b; return true; }
            }
    }
    return false;
}

void CloseCapture() {
    s_ui.capturing = false;
    s_ui.armed = false;         // the key that ended the capture must not act on the page
    s_ui.dirty = true;
}

void Activate(double t) {
    s_ui.dirty = true;
    switch (s_ui.zone) {
    case Z_TABS:
        SetTab(s_ui.col);
        break;
    case Z_LIST:
        if (s_ui.tab == TAB_CONTROLS && s_ui.pcControls) StartCapture(s_ui.row, s_ui.col, t);
        else if (s_ui.tab == TAB_GRAPHICS && GfxRowAvailable(s_ui.row)) GfxChange(s_ui.row, 1);
        break;
    case Z_FOOTER:
        break;                  // handled by the caller (defaults / back)
    }
}

// pointer: what is under it -> zone / row / col; returns false when nothing
bool HitTest(double mx, double my, int& zone, int& row, int& col, double* barFrac) {
    for (int k = 0; k < N_TABS; ++k) {
        int px, py;
        LabelPos(TAB_CX[k], TAB_CY, TAB_SCALE, px, py);
        if (Inside(LabelRect(px, py, TAB_SCALE), mx, my)) {
            zone = Z_TABS; col = k; row = 0; return true;
        }
    }
    for (int k = 0; k < 2; ++k)
        if (Inside(LabelRect(FOOTER_POS[k][0], FOOTER_POS[k][1], FOOTER_SCALE), mx, my)) {
            zone = Z_FOOTER; col = k; row = 0; return true;
        }
    if (s_ui.tab == TAB_CONTROLS && s_ui.pcControls) {
        for (int r = 0; r < ROWS && s_ui.scroll + r < N_ACTIONS; ++r)
            for (int k = 0; k < N_SLOTS; ++k) {
                int px, py;
                LabelPos(SLOT_X0 + k * SLOT_DX, ROW_Y0 + r * ROW_DY, ROW_SCALE, px, py);
                if (Inside(LabelRect(px, py, ROW_SCALE), mx, my)) {
                    zone = Z_LIST; row = s_ui.scroll + r; col = k; return true;
                }
            }
    } else if (s_ui.tab == TAB_AUDIO) {
        for (int a = 0; a < AUDIO_ROWS; ++a) {
            int cy = AUDIO_Y0 + a * AUDIO_DY, px, py;
            LabelPos(AUDIO_LABEL_CX, cy, AUDIO_SCALE, px, py);
            Rect bar = { BAR_X0 - 6.0, BAR_X1 + 6.0, cy - 16.0, cy + 16.0 };
            bool onBar = Inside(bar, mx, my);
            if (onBar || Inside(LabelRect(px, py, AUDIO_SCALE), mx, my)) {
                zone = Z_LIST; row = a; col = 0;
                if (barFrac) *barFrac = onBar ? (mx - BAR_X0) / (double)(BAR_X1 - BAR_X0) : -1.0;
                return true;
            }
        }
    } else if (s_ui.tab == TAB_GRAPHICS) {
        for (int r = 0; r < GFX_ROWS; ++r) {
            int cy = GFX_Y0 + r * GFX_DY, nx, ny, vx, vy;
            LabelPos(GFX_NAME_CX, cy, GFX_NAME_SCALE, nx, ny);
            LabelPos(GFX_VALUE_CX, cy, GFX_VALUE_SCALE, vx, vy);
            bool onValue = Inside(LabelRect(vx, vy, GFX_VALUE_SCALE), mx, my);
            if (onValue || Inside(LabelRect(nx, ny, GFX_NAME_SCALE), mx, my)) {
                zone = Z_LIST; row = r; col = onValue ? 1 : 0;
                return true;
            }
        }
    }
    return false;
}

// ---------------------------------------------------------------------------------------------------------------
// drawing: everything the page shows, from s_ui and s_set (unchanged values are not sent again)
// ---------------------------------------------------------------------------------------------------------------
int DevTypeShown() { return s_in.slot >= 0 ? s_in.devType : 3; }

void DrawArrows(int px, int py, double s) {
    uint32_t id = Gid("Arrows");
    SetInt(id, P_POSITION, 11, (int)floor(px - 99.0 * s + 0.5));
    SetInt(id, P_POSITION, 12, (int)floor(py - 23.0 * s + 0.5));
    SetFloat(id, P_SCALE, 11, (float)(1.09 * s));
    SetFloat(id, P_SCALE, 12, (float)(1.09 * s));
}

void Draw() {
    bool controls = s_ui.tab == TAB_CONTROLS, audio = s_ui.tab == TAB_AUDIO, graphics = s_ui.tab == TAB_GRAPHICS;
    bool list = controls ? s_ui.pcControls : true;
    for (int k = 0; k < N_TABS; ++k) {
        bool sel = s_ui.zone == Z_TABS && s_ui.col == k;
        SetInt(Gid(kTabNames[k]), P_FRAME, 0,
               s_ui.tab == k ? (sel ? F_SELECTED : F_NORMAL) : (sel ? F_GREY_SELECTED : F_GREY));
    }
    // graphics rows
    for (int r = 0; r < GFX_ROWS; ++r) {
        uint32_t name = Gid("Gfx%dName", r), value = Gid("Gfx%dValue", r);
        SetVisible(name, graphics);
        SetVisible(value, graphics);
        if (!graphics) continue;
        bool sel = s_ui.zone == Z_LIST && s_ui.row == r, on = GfxRowAvailable(r);
        SetText(name, kGfxLabels[r]);
        SetText(value, GfxValue(r));
        SetInt(name, P_FRAME, 0, sel ? F_SELECTED : F_NORMAL);
        SetInt(value, P_FRAME, 0, on ? (sel ? F_SELECTED : F_NORMAL) : (sel ? F_GREY_SELECTED : F_GREY));
    }
    // controls rows
    for (int r = 0; r < ROWS; ++r) {
        int a = s_ui.scroll + r;
        bool show = controls && list && a < N_ACTIONS;
        SetVisible(Gid("Row%dName", r), show);
        if (show) SetText(Gid("Row%dName", r), kActions[a].label);
        for (int k = 0; k < N_SLOTS; ++k) {
            uint32_t id = Gid("Row%dSlot%d", r, k);
            SetVisible(id, show);
            if (!show) continue;
            const Bind& b = s_set.bind[a][k];
            bool sel = s_ui.zone == Z_LIST && s_ui.row == a && s_ui.col == k;
            SetText(id, ShownName(b, DevTypeShown()));
            SetInt(id, P_FRAME, 0, b.kind != B_NONE ? (sel ? F_SELECTED : F_NORMAL) : (sel ? F_GREY_SELECTED : F_GREY));
        }
    }
    wchar_t buf[96];
    if (controls && list && N_ACTIONS > ROWS) {
        swprintf(buf, 96, L"%d-%d OF %d", s_ui.scroll + 1, s_ui.scroll + ROWS, (int)N_ACTIONS);
        SetText(Gid("Scroll"), buf);
        SetVisible(Gid("Scroll"), true);
    } else {
        SetVisible(Gid("Scroll"), false);
    }
    SetVisible(Gid("Note"), controls && !list);
    if (controls && !list) SetText(Gid("Note"), L"THE WII REMOTE CONTROLS ARE SET IN WIIMOTE.INI");
    // audio rows
    for (int v = 0; v < AUDIO_ROWS; ++v) {
        bool sel = s_ui.zone == Z_LIST && s_ui.row == v;
        uint32_t name = Gid("Audio%dName", v), track = Gid("Audio%dTrack", v), fill = Gid("Audio%dFill", v),
                 value = Gid("Audio%dValue", v);
        SetVisible(name, audio);
        SetVisible(track, audio);
        SetVisible(value, audio);
        SetVisible(fill, audio && s_set.volume[v] > 0);
        if (!audio) continue;
        SetText(name, kVolumeLabels[v]);
        SetInt(name, P_FRAME, 0, sel ? F_SELECTED : F_NORMAL);
        int width = (int)floor((BAR_X1 - BAR_X0 - 6) * s_set.volume[v] / 100.0 + 0.5);
        if (width > 0) SetInt(fill, P_SIZE, 11, width);
        swprintf(buf, 96, L"%d%%", s_set.volume[v]);
        SetText(value, buf);
    }
    // footer
    for (int k = 0; k < 2; ++k)
        SetInt(Gid(k ? "Back" : "Reset"), P_FRAME, 0, s_ui.zone == Z_FOOTER && s_ui.col == k ? F_SELECTED : F_NORMAL);
    const wchar_t* hint;
    bool pad = s_in.slot >= 0;
    if (controls && list)
        hint = pad ? L"A: CHANGE    X: CLEAR    LB/RB: TABS    B: BACK"
                   : L"ENTER: CHANGE    DELETE: CLEAR    TAB: NEXT TAB    ESC: BACK";
    else if (audio)
        hint = pad ? L"LEFT/RIGHT: VOLUME    LB/RB: TABS    B: BACK"
                   : L"LEFT/RIGHT: VOLUME    TAB: NEXT TAB    ESC: BACK";
    else if (graphics)
        hint = pad ? L"LEFT/RIGHT: CHANGE    LB/RB: TABS    B: BACK"
                   : L"LEFT/RIGHT: CHANGE    TAB: NEXT TAB    ESC: BACK";
    else
        hint = pad ? L"LB/RB: TABS    B: BACK" : L"TAB: NEXT TAB    ESC: BACK";
    SetText(Gid("Hint"), hint);
    // arrows
    int px, py;
    switch (s_ui.zone) {
    case Z_TABS:
        LabelPos(TAB_CX[s_ui.col], TAB_CY, TAB_SCALE, px, py);
        DrawArrows(px, py, TAB_SCALE);
        break;
    case Z_LIST:
        if (controls) {
            LabelPos(SLOT_X0 + s_ui.col * SLOT_DX, ROW_Y0 + (s_ui.row - s_ui.scroll) * ROW_DY, ROW_SCALE, px, py);
            DrawArrows(px, py, ROW_SCALE);
        } else if (audio) {
            LabelPos(AUDIO_LABEL_CX, AUDIO_Y0 + s_ui.row * AUDIO_DY, AUDIO_SCALE, px, py);
            DrawArrows(px, py, AUDIO_SCALE);
        } else {
            LabelPos(GFX_VALUE_CX, GFX_Y0 + s_ui.row * GFX_DY, GFX_VALUE_SCALE, px, py);
            DrawArrows(px, py, GFX_VALUE_SCALE);
        }
        break;
    default:
        DrawArrows(FOOTER_POS[s_ui.col][0], FOOTER_POS[s_ui.col][1], FOOTER_SCALE);
        break;
    }
    // capture overlay
    SetVisible(Gid("CaptureDim"), s_ui.capturing);
    SetVisible(Gid("CapturePanel"), s_ui.capturing);
    SetVisible(Gid("CaptureText"), s_ui.capturing);
    if (s_ui.capturing) {
        if (kActions[s_ui.capAction].vk)
            swprintf(buf, 96, L"PRESS A KEY OR BUTTON FOR %ls\nESC: CANCEL", kActions[s_ui.capAction].label);
        else
            swprintf(buf, 96, L"PRESS A KEY OR BUTTON, SCROLL OR SHAKE THE MOUSE FOR %ls\nESC: CANCEL",
                     kActions[s_ui.capAction].label);
        SetText(Gid("CaptureText"), buf);
    }
}

// ---------------------------------------------------------------------------------------------------------------
// open / close / per frame
// ---------------------------------------------------------------------------------------------------------------
void OpenPage(double t) {
    s_numCache.clear();
    s_textCache.clear();
    memset(&s_ui, 0, sizeof(s_ui));
    s_ui.open = true;
    s_ui.pcControls = s_layerOn;
    s_ui.zone = Z_TABS;
    s_ui.tab = s_ui.col = TAB_CONTROLS;
    s_ui.lastMx = s_ui.lastMy = -1;
    s_ui.holdDir = HeldDir(s_in);
    s_ui.holdStart = s_ui.lastRepeat = t;
    s_editStart = s_set;
    SetInt(Gid("Arrows"), P_FRAME, 0, 0);
    Draw();
    OptLog("Options page opened");
}

// member by member: the bindings (2 bytes each) leave padding before the volumes
bool SameSettings(const Settings& a, const Settings& b) {
    return memcmp(a.bind, b.bind, sizeof(a.bind)) == 0 && memcmp(a.volume, b.volume, sizeof(a.volume)) == 0 &&
           memcmp(&a.gfx, &b.gfx, sizeof(a.gfx)) == 0;
}

void ClosePage(bool popIt) {
    if (popIt && !s_testCalls) {
        ((PopCursorFn)(uintptr_t)MGM_POP_CURSOR)(PAGE_NAME);
        ((PopPageFn)(uintptr_t)MGM_POP_PAGE)(0, 0, 0);
    }
    s_ui.open = false;
    s_ui.capturing = false;
    s_swallow = true;
    s_swallowStart = NowSeconds();
    if (!SameSettings(s_set, s_editStart) && !s_path.empty()) {
        bool ok = SaveSettings(s_path, s_set);
        OptLog("settings %s %s", ok ? "saved to" : "NOT saved to", s_path.c_str());
    }
    OptLog("Options page closed%s", popIt ? "" : " (by the game)");
}

// one frame of the page (the page is on top); returns false when it closed
bool PageFrame(const OptCtx& c, double t) {
    UiInput u;
    ReadUi(c, s_in, s_prevIn, t, u);
    if (!s_ui.armed) {
        if (AnyDown(s_in)) return true;
        s_ui.armed = true;
        memset(&u, 0, sizeof(u));
    }
    if (s_ui.capturing) {
        if (!s_ui.captureArmed) {
            if (!AnyDown(s_in)) s_ui.captureArmed = true;
            return true;
        }
        Bind b;
        if (Pressed(s_in, s_prevIn, VK_ESCAPE) || t - s_ui.capStart > 8.0) {
            CloseCapture();
        } else if (CaptureInput(s_in, s_prevIn, b, kActions[s_ui.capAction].vk == 0)) {
            Bind* row = s_set.bind[s_ui.capAction];
            for (int k = 0; k < N_SLOTS; ++k)              // the same input twice in one action: keep one
                if (k != s_ui.capSlot && row[k].kind == b.kind && row[k].code == b.code) row[k].kind = row[k].code = 0;
            row[s_ui.capSlot] = b;
            OptLog("%s slot %d = %s", kActions[s_ui.capAction].key, s_ui.capSlot + 1, IniName(b).c_str());
            CloseCapture();
        }
        if (s_ui.dirty) {
            Draw();
            s_ui.dirty = false;
        }
        return true;
    }
    if (u.mx >= 0) {
        int zone, row, col;
        double frac = -1;
        bool hit = HitTest(u.mx, u.my, zone, row, col, &frac);
        if (hit && (u.mouseMoved || u.click)) {
            if (zone != s_ui.zone || row != s_ui.row || col != s_ui.col) s_ui.dirty = true;
            s_ui.zone = zone;
            s_ui.row = row;
            s_ui.col = col;
        }
        bool leftDown = (s_in.keys[VK_LBUTTON] & 0x80) != 0;
        if (u.click && hit && s_ui.tab == TAB_AUDIO && zone == Z_LIST && frac >= 0) s_ui.dragBar = true;
        if (!leftDown) s_ui.dragBar = false;
        if (s_ui.dragBar && s_ui.zone == Z_LIST && s_ui.tab == TAB_AUDIO) {
            double f = (u.mx - BAR_X0) / (double)(BAR_X1 - BAR_X0);
            int v = (int)floor((f < 0 ? 0 : f > 1 ? 1 : f) * 100.0 / 5.0 + 0.5) * 5;
            if (v != s_set.volume[s_ui.row]) {
                s_set.volume[s_ui.row] = v;
                s_ui.dirty = true;
            }
        }
        if (u.click && hit && !(s_ui.tab == TAB_AUDIO && zone == Z_LIST) &&
            !(s_ui.tab == TAB_GRAPHICS && zone == Z_LIST && col == 0)) u.ok = true;
    }
    if (u.back) {
        ClosePage(true);
        return false;
    }
    if (u.tabNext || u.tabPrev) {
        SetTab((s_ui.tab + (u.tabPrev ? N_TABS - 1 : 1)) % N_TABS);
        s_ui.zone = Z_TABS;
        s_ui.col = s_ui.tab;
    }
    if (u.up) Move(1);
    if (u.down) Move(2);
    if (u.left) Move(3);
    if (u.right) Move(4);
    if (u.clear && s_ui.zone == Z_LIST && s_ui.tab == TAB_CONTROLS && s_ui.pcControls) {
        s_set.bind[s_ui.row][s_ui.col].kind = s_set.bind[s_ui.row][s_ui.col].code = 0;
        s_ui.dirty = true;
    }
    if (u.ok) {
        if (s_ui.zone == Z_FOOTER && s_ui.col == 1) {
            ClosePage(true);
            return false;
        }
        if (s_ui.zone == Z_FOOTER && s_ui.col == 0) {
            if (s_ui.tab == TAB_CONTROLS) {
                Defaults(s_set, s_ui.pcControls, false);
            } else if (s_ui.tab == TAB_AUDIO) {
                Defaults(s_set, false, true);
            } else {
                Defaults(s_set, false, false, true);
                GfxSetBorderless(false);
                GfxSetResolution(0);
                TimingSetCap(TimingIniCap());
                GfxSetVsync(GfxGameVsync());
                GfxSetHighDetail(false);
                GfxSetAniso(0);
                AfxSetUserEnabled(AfxDefaultEnabled());
            }
            OptLog("%s back to the defaults", s_ui.tab == TAB_CONTROLS ? "controls" : s_ui.tab == TAB_AUDIO ?
                   "volumes" : "graphics");
            s_ui.dirty = true;
        } else {
            Activate(t);
        }
    }
    if (s_ui.dirty) {
        Draw();
        s_ui.dirty = false;
    }
    return true;
}

// ---------------------------------------------------------------------------------------------------------------
// hooks
// ---------------------------------------------------------------------------------------------------------------
typedef void (__cdecl* VoidFn)();
typedef void* (__cdecl* ManagerFn)();

void OnInputPolled(const OptCtx& c) {
    if (s_realEngine) HangWatchFrame();
    ++s_frame;
    double t = NowSeconds();
    s_prevIn = s_in;
    Snapshot(c, s_in);
    TrackMouse(s_mouse, s_in, t);                   // also while the page captures a binding
    if (!s_layerOn) return;
    if (s_ui.open) {
        MaskGame(c);
        FeedMotion(s_set, s_actions, s_in, false, t);
        return;
    }
    if (MediaInputBlocked()) {      // a mod has the controls (wc.game.block_input): a remote nobody is touching
        MaskGame(c);
        FeedMotion(s_set, s_actions, s_in, false, t);
        return;
    }
    if (s_swallow) {
        if (AnyDown(s_in) && t - s_swallowStart < 2.0) {
            MaskGame(c);
            FeedMotion(s_set, s_actions, s_in, false, t);
            return;
        }
        s_swallow = false;
    }
    Evaluate(s_set, s_in, s_actions);
    WriteGame(c, s_actions, s_in);
    FeedMotion(s_set, s_actions, s_in, true, t);
}

void __cdecl InputPollHook() {
    ((VoidFn)(uintptr_t)INPUT_POLL)();
    OnInputPolled(s_game);
}

DWORD s_volumeTick;
uint32_t s_updates;

void* __cdecl MagmaUpdateHook() {
    double t = NowSeconds();
    int cw, ch;
    if (!s_gfxApplied && ++s_updates > 30 && GfxClientSize(cw, ch)) ApplyStartupGraphics();
    GfxFrame();
    Sm64Frame();
    LuaFrame();
    if (s_pageOn) {
        long top = ((TopPageFn)(uintptr_t)MGM_TOP_PAGE)();
        bool onTop = (uint32_t)top == s_pageId;
        if (onTop && !s_ui.open) OpenPage(t);
        else if (!onTop && s_ui.open) ClosePage(false);
        if (s_ui.open) PageFrame(s_game, t);
    }
    DWORD now = GetTickCount();
    if (s_ui.open || now - s_volumeTick > 1000) {        // live while editing, else once a second (world loads)
        s_volumeTick = now;
        ApplyVolumes(s_game, s_set, false);
    }
    return ((ManagerFn)(uintptr_t)MGM_GET_MANAGER)();
}

// ---------------------------------------------------------------------------------------------------------------
// verification and installation
// ---------------------------------------------------------------------------------------------------------------
bool CheckCall(const Image& img, uint32_t va, uint32_t target) {
    uint8_t b[5];
    if (!img.Read(va, b, 5) || b[0] != 0xE8) return false;
    int32_t rel;
    memcpy(&rel, b + 1, 4);
    return va + 5 + (uint32_t)rel == target;
}

int Verify(const Image& img, std::string& rep) {
    int bad = 0;
    uint32_t got = 0;
    for (size_t i = 0; i < sizeof(kSites) / sizeof(kSites[0]); ++i) {
        const CallSite& s = kSites[i];
        bool call = CheckCall(img, s.va, s.target);
        bool crc = CheckCrc(img, s.va - 0x10, 0x20, s.crc, &got);
        Report(rep, call && crc, "%-56s call at %08X -> %08X%s, crc %08X (want %08X)", s.what, s.va, s.target,
               call ? "" : " (differs)", got, s.crc);
        if (!(call && crc)) ++bad;
    }
    for (size_t i = 0; i < sizeof(kCode) / sizeof(kCode[0]); ++i) {
        const Code& k = kCode[i];
        bool ok = CheckCrc(img, k.va, k.len, k.crc, &got);
        Report(rep, ok, "%-56s %08X len 0x%03X crc %08X (want %08X)", k.what, k.va, k.len, got, k.crc);
        if (!ok) ++bad;
    }
    return bad;
}

bool PatchCall(uint32_t va, void* fn) {
    int32_t rel = (int32_t)((uint32_t)(uintptr_t)fn - (va + 5));
    return WriteCode(va + 1, &rel, 4);
}

void CopyOut(const std::string& rep, char* out, int outSize) {
    if (!out || outSize <= 0) return;
    size_t n = rep.size() < (size_t)outSize - 1 ? rep.size() : (size_t)outSize - 1;
    memcpy(out, rep.c_str(), n);
    out[n] = 0;
}

// ---------------------------------------------------------------------------------------------------------------
// self-test (wmtest options)
// ---------------------------------------------------------------------------------------------------------------
#define OPT_EXPECT(cond, ...) do { bool ok_ = (cond); Report(rep, ok_, __VA_ARGS__); if (!ok_) ++fails; } while (0)

struct FakeEngine {
    uint8_t   kb[256];
    uint8_t*  kbPtr;
    int32_t   active;
    int32_t   slotOfId[50];
    int32_t   connected[10];
    int32_t   devType[10];
    uint32_t  joyMask[10 * 0xA4 / 4 + 1];
    float     sticks[10 * 3 * 3];
    int32_t   stickEnabled[30];
    uint32_t  mouse;
    int32_t   cursor[2];
    int32_t   viewport[4];
    uint32_t  snd;
    float     mouseMove[2];
    float     mouseScale;
};

// frames at fps of a mouse moving pos(t) (heights of the view) along x: whether a wiggle was on at the end, when it
// started (-1 = never) and its rhythm
struct WiggleRun { bool on; double startedAt, hz; };

WiggleRun RunWiggle(double fps, double secs, double (*pos)(double), double stopAt = 1e9) {
    MouseTrack m;
    memset(&m, 0, sizeof(m));
    WiggleRun r = { false, -1.0, 0.0 };
    double prev = pos(0.0);
    for (int f = 1; f <= (int)(secs * fps + 0.5); ++f) {
        double t = f / fps;
        double p = pos(t < stopAt ? t : stopAt);
        RawInput in;
        memset(&in, 0, sizeof(in));
        in.mouse[0] = (float)(p - prev);
        prev = p;
        TrackMouse(m, in, 100.0 + t);
        if (in.wiggle[0] && r.startedAt < 0.0) r.startedAt = t;
        r.on = in.wiggle[0];
        if (in.wiggle[0]) r.hz = in.wiggleHz[0];
    }
    return r;
}

double Tri(double t, double hz, double amp) {             // a triangle wave: amp peak to peak
    double ph = fmod(t * hz, 1.0);
    return amp * (ph < 0.5 ? ph * 2.0 : 2.0 - ph * 2.0);
}
double Wiggle4Hz(double t) { return Tri(t, 4.0, 0.15); }
double Wiggle2Hz(double t) { return Tri(t, 2.0, 0.3); }
double SlowWave(double t) { return Tri(t, 1.0, 0.15); }  // 0.3 heights a second
double Flick(double t) { return t < 0.1 ? t * 2.0 : t < 0.2 ? 0.2 - (t - 0.1) * 2.0 : 0.0; }

// the same for a path (x, y): circles
struct PathRun { bool cw, ccw, wx, wy, wiggleWhileCircle; double startedAt, hz; };

PathRun RunPath(double fps, double secs, void (*pos)(double, double&, double&), double stopAt = 1e9) {
    MouseTrack m;
    memset(&m, 0, sizeof(m));
    PathRun r = { false, false, false, false, false, -1.0, 0.0 };
    double px, py;
    pos(0.0, px, py);
    for (int f = 1; f <= (int)(secs * fps + 0.5); ++f) {
        double t = f / fps, x, y;
        pos(t < stopAt ? t : stopAt, x, y);
        RawInput in;
        memset(&in, 0, sizeof(in));
        in.mouse[0] = (float)(x - px);
        in.mouse[1] = (float)(y - py);
        px = x, py = y;
        TrackMouse(m, in, 300.0 + t);
        bool circle = in.wiggle[2] || in.wiggle[3];
        if (circle && r.startedAt < 0.0) r.startedAt = t;
        if (circle && (in.wiggle[0] || in.wiggle[1])) r.wiggleWhileCircle = true;
        r.cw = in.wiggle[2], r.ccw = in.wiggle[3], r.wx = in.wiggle[0], r.wy = in.wiggle[1];
        if (circle) r.hz = in.wiggleHz[in.wiggle[2] ? 2 : 3];
    }
    return r;
}

void CircleCw(double t, double& x, double& y) { x = 0.08 * cos(18.8495559 * t), y = 0.08 * sin(18.8495559 * t); }   // 3 Hz
void CircleCcw(double t, double& x, double& y) { x = 0.08 * cos(18.8495559 * t), y = -0.08 * sin(18.8495559 * t); }
void FlatEllipse(double t, double& x, double& y) { x = 0.075 * cos(25.1327412 * t), y = 0.01 * sin(25.1327412 * t); } // 4 Hz
void SlowCircle(double t, double& x, double& y) { x = 0.08 * cos(3.14159265 * t), y = 0.08 * sin(3.14159265 * t); }  // 0.5 Hz

int RunSelfTest(std::string& rep) {
    int fails = 0;
    FakeEngine* f = new FakeEngine();
    memset(f, 0, sizeof(*f));
    f->kbPtr = f->kb;
    f->active = 1;
    for (int i = 0; i < 50; ++i) f->slotOfId[i] = -1;
    OptCtx ctx = { &f->kbPtr, &f->active, f->slotOfId, f->connected, f->devType, f->joyMask, f->sticks, f->stickEnabled,
                   &f->mouse, f->cursor, f->viewport, &f->snd, f->mouseMove, &f->mouseScale };
    Settings saved = s_set;
    std::string* oldLog = s_testLog;
    s_testLog = &rep;
    s_realEngine = false;

    // names
    Bind b;
    bool p1 = ParseBind("rshift", b) && b.kind == B_KEY && b.code == VK_SHIFT && IniName(b) == "SHIFT" &&
              ShownName(b, 3) == L"SHIFT" && ParseBind("0xA5", b) && b.code == VK_MENU && IniName(b) == "ALT";
    bool p2 = ParseBind("MOUSE2", b) && b.kind == B_MOUSE && b.code == 2 && ShownName(b, 3) == L"RIGHT CLICK";
    bool p3 = ParseBind("PAD_LT", b) && b.kind == B_PAD && b.code == 16 && IniName(b) == "PAD_LT";
    bool p4 = ParseBind("pad_rs_left", b) && b.kind == B_AXIS && b.code == 6 && ShownName(b, 3) == L"R STICK LEFT";
    bool p5 = ParseBind("0x41", b) && b.kind == B_KEY && b.code == 'A' && IniName(b) == "A";
    bool p6 = ParseBind("LBUTTON", b) && b.kind == B_MOUSE && b.code == 1;
    bool p7 = !ParseBind("PAD_Q", b) && ParseBind("F12", b) && b.code == 0x7B && IniName(b) == "F12";
    bool p8 = ParseBind("PAD_BIT20", b) && b.kind == B_PAD && b.code == 20 && ShownName(b, 1) == L"BUTTON 21";
    bool p9 = ParseBind("mouse_shake_y", b) && b.kind == B_MOUSEMOVE && b.code == 1 && IniName(b) == "MOUSE_SHAKE_Y" &&
              ShownName(b, 3) == L"MOUSE SHAKE Y" && ParseBind("WHEEL", b) && b.kind == B_WHEEL && b.code == 0 &&
              ShownName(b, 3) == L"SCROLL WHEEL" && ParseBind("WHEEL_DOWN", b) && b.code == 2 &&
              IniName(b) == "WHEEL_DOWN";
    OPT_EXPECT(p1 && p2 && p3 && p4 && p5 && p6 && p7 && p8 && p9,
               "input names: RSHIFT = SHIFT and 0xA5 = ALT %d, MOUSE2 %d, PAD_LT %d, PAD_RS_LEFT %d, 0x41 %d, "
               "LBUTTON %d, F12 / bad name %d, "
               "PAD_BIT20 %d, MOUSE_SHAKE_Y / WHEEL / WHEEL_DOWN %d", p1, p2, p3, p4, p5, p6, p7, p8, p9);

    // defaults and the settings file round trip
    Settings d;
    Defaults(d, true, true, true);
    bool defs = d.bind[5][0].kind == B_KEY && d.bind[5][0].code == VK_SHIFT && d.bind[5][1].kind == B_PAD &&
                d.bind[5][1].code == 12 && d.bind[5][2].kind == B_NONE && d.bind[7][0].kind == B_MOUSE &&
                d.bind[0][2].kind == B_AXIS && d.bind[0][2].code == 0 && d.volume[2] == 100;
    OPT_EXPECT(defs, "defaults: RUN = SHIFT, PAD A, (none); THROW = MOUSE1; MOVE FORWARD slot 3 = left stick up");
    bool motionDefs = d.bind[A_SHAKE_X][0].kind == B_MOUSEMOVE && d.bind[A_SHAKE_X][0].code == 0 &&
                      d.bind[A_SHAKE_Y][0].kind == B_MOUSEMOVE && d.bind[A_SHAKE_Y][0].code == 1 &&
                      d.bind[A_MARACA][0].kind == B_WHEEL && d.bind[A_MARACA][0].code == 0 &&
                      d.bind[A_TILT_LEFT][0].kind == B_MOUSE && d.bind[A_TILT_LEFT][0].code == 1 &&
                      d.bind[A_TILT_RIGHT][0].kind == B_MOUSE && d.bind[A_TILT_RIGHT][0].code == 2 &&
                      kActions[A_SCREAM].vk == 0x8F && !kActions[A_SHAKE_X].vk && !kActions[A_TILT_RIGHT].vk &&
                      d.bind[A_ROTATE_CW][0].kind == B_MOUSEMOVE && d.bind[A_ROTATE_CW][0].code == 2 &&
                      d.bind[A_ROTATE_CCW][0].kind == B_MOUSEMOVE && d.bind[A_ROTATE_CCW][0].code == 3 &&
                      N_ACTIONS == A_ROTATE_CCW + 1;
    OPT_EXPECT(motionDefs, "motion rows: SHAKE LEFT/RIGHT = MOUSE_SHAKE_X, SHAKE UP/DOWN = MOUSE_SHAKE_Y, MARACA = WHEEL, "
               "TILT LEFT / RIGHT = MOUSE1 / MOUSE2, SHAKE CLOCKWISE / COUNTER-CW = MOUSE_CIRCLE_CW / _CCW, no virtual "
               "keys; SCREAM is action %d", (int)A_SCREAM);

    // mouse wiggles and the wheel
    WiggleRun w4 = RunWiggle(60.0, 1.0, Wiggle4Hz), w4f = RunWiggle(240.0, 1.0, Wiggle4Hz);
    WiggleRun w4s = RunWiggle(60.0, 1.5, Wiggle4Hz, 1.0), w2 = RunWiggle(60.0, 2.0, Wiggle2Hz);
    WiggleRun slow = RunWiggle(60.0, 3.0, SlowWave), flick = RunWiggle(60.0, 1.0, Flick);
    OPT_EXPECT(w4.on && w4.startedAt > 0.2 && w4.startedAt < 0.45 && fabs(w4.hz - 4.0) < 0.5 && w4f.on &&
               fabs(w4f.hz - 4.0) < 0.3 && !w4s.on && w2.on && fabs(w2.hz - 2.0) < 0.3 && !slow.on &&
               slow.startedAt < 0.0 && !flick.on && flick.startedAt < 0.0,
               "mouse wiggles: 4 Hz 15 %% of the height at 60 fps on after %.2f s at %.2f Hz, at 240 fps %.2f Hz; "
               "stopped 0.5 s after the mouse stops %d; 2 Hz 30 %% %d (%.2f Hz); 0.3 heights a second never "
               "(%d); one flick never (%d)", w4.startedAt, w4.hz, w4f.hz, !w4s.on, w2.on, w2.hz, !slow.on, !flick.on);
    PathRun ccw60 = RunPath(60.0, 1.5, CircleCcw), cw60 = RunPath(60.0, 1.5, CircleCw), cw240 = RunPath(240.0, 1.5, CircleCw);
    PathRun cwStop = RunPath(60.0, 1.5, CircleCw, 1.0), flat = RunPath(60.0, 1.5, FlatEllipse), lazy = RunPath(60.0, 3.0, SlowCircle);
    OPT_EXPECT(cw60.cw && !cw60.ccw && cw60.startedAt > 0.15 && cw60.startedAt < 0.45 && fabs(cw60.hz - 3.0) < 0.4 &&
               !cw60.wiggleWhileCircle && cw240.cw && fabs(cw240.hz - 3.0) < 0.3 && ccw60.ccw && !ccw60.cw &&
               fabs(ccw60.hz - 3.0) < 0.4 && !cwStop.cw && !cwStop.ccw && flat.wx && !flat.cw && !flat.ccw &&
               flat.startedAt < 0.0 && !lazy.cw && lazy.startedAt < 0.0,
               "mouse circles: 3 turns a second, 8 %% of the height, clockwise at 60 fps on after %.2f s at %.2f Hz, "
               "at 240 fps %.2f Hz, no x / y wiggle with it %d; the other way round %d (%.2f Hz); stopped 0.5 s "
               "after the mouse stops %d; a flat ellipse is a wiggle, not a circle %d; half a turn a second never %d",
               cw60.startedAt, cw60.hz, cw240.hz, !cw60.wiggleWhileCircle, ccw60.ccw, ccw60.hz, !cwStop.cw,
               flat.wx && !flat.cw, !lazy.cw);
    MouseTrack mt;
    memset(&mt, 0, sizeof(mt));
    RawInput wi;
    bool wheelOn = true, wheelOff = false;
    double wheelHz = 0.0;
    for (int fr = 1; fr <= 120; ++fr) {
        memset(&wi, 0, sizeof(wi));
        if (fr <= 60 && fr % 6 == 0) wi.wheel = 2;                   // a notch down every 0.1 s for a second
        TrackMouse(mt, wi, 200.0 + fr / 60.0);
        if (fr >= 12 && fr <= 60) wheelOn = wheelOn && wi.wheelActive == 2, wheelHz = wi.wheelHz;
        if (fr == 90) wheelOff = wi.wheelActive == 0;
    }
    Bind wheelAny = { B_WHEEL, 0 }, wheelUp = { B_WHEEL, 1 };
    memset(&wi, 0, sizeof(wi));
    wi.wheel = 1;
    wi.wheelActive = 1;
    bool once = BindValue(wheelAny, wi) == 1.0f && BindValue(wheelUp, wi) == 1.0f && BindValue(wheelUp, wi, true) == 1.0f;
    wi.wheel = 0;
    once = once && BindValue(wheelAny, wi) == 0.0f && BindValue(wheelAny, wi, true) == 1.0f;
    OPT_EXPECT(wheelOn && wheelOff && fabs(wheelHz - 5.0) < 0.1 && once,
               "wheel: a notch every 0.1 s holds a motion row (rhythm %.2f Hz), 0.5 s later released %d; an action "
               "sees one frame per notch, a motion row the whole time %d", wheelHz, wheelOff, once);
    char tmp[MAX_PATH];
    GetTempPathA(MAX_PATH, tmp);
    std::string path = std::string(tmp) + "wmtest_options.ini";
    Settings e = d;
    e.bind[4][1].kind = B_NONE, e.bind[4][1].code = 0;
    e.bind[9][2].kind = B_AXIS, e.bind[9][2].code = 7;
    e.volume[1] = 35;
    e.gfx.display = 1;
    e.gfx.resolution = 720;
    e.gfx.vsync = 0;
    e.gfx.highDetail = 1;
    e.gfx.aniso = 8;
    e.gfx.wiiEffects = 0;
    e.gfx.frameRate = 0;
    FILE* pre = fopen(path.c_str(), "wb");                  // what the launcher keeps there, which is not ours to lose
    if (pre) {
        fputs("[graphics]\ndisplay=window\n\n[mods]\nenabled=It's a me!|Original soundtrack\n\n"
              "[launch]\nswitches=/binload/fe /lang/en\n", pre);
        fclose(pre);
    }
    bool saved1 = SaveSettings(path, e);
    Settings l;
    std::vector<std::string> problems;
    LoadSettings(path, l, problems);
    std::string after;
    FILE* rd = fopen(path.c_str(), "rb");
    if (rd) {
        char rb[1024];
        size_t got;
        while ((got = fread(rb, 1, sizeof(rb), rd)) > 0) after.append(rb, got);
        fclose(rd);
    }
    bool kept = after.find("enabled=It's a me!|Original soundtrack") != std::string::npos &&
                after.find("switches=/binload/fe /lang/en") != std::string::npos &&
                after.find("[graphics]") != std::string::npos;
    DeleteFileA(path.c_str());
    OPT_EXPECT(saved1 && SameSettings(e, l) && problems.empty() && kept,
               "options.ini round trip (a cleared slot, a stick binding, music 35%%, borderless, 720 lines, VSync off, "
               "high detail, 8x filtering, Wii effects off, uncapped): saved %d, equal %d, %d problem(s), the "
               "launcher's own sections kept %d", saved1, SameSettings(e, l), (int)problems.size(), kept);
    ParseList("SPACE, NOPE ,PAD_A,W,S", l.bind[0], "test", &problems);
    OPT_EXPECT(problems.size() == 1 && l.bind[0][0].code == 0x20 && l.bind[0][1].code == 12 && l.bind[0][2].code == 'W',
               "a list with an unknown name and four inputs: %d problem(s), first three kept", (int)problems.size());

    // actions from inputs, written as virtual keys; the pad stick = movement
    f->kb[0x20] = 0x80;                     // SPACE
    f->kb[0xA0] = 0x80;                     // left Shift: GetKeyboardState sets the combined VK_SHIFT as well
    f->kb[VK_SHIFT] = 0x80;
    f->kb[0x01] = 0x80;                     // left mouse button
    f->kb[0x8C] = 0x80;                     // stale virtual key from the previous frame
    f->slotOfId[0] = 2;
    f->connected[2] = 1;
    f->devType[2] = 3;
    f->stickEnabled[2 * 3] = f->stickEnabled[2 * 3 + 1] = 1;
    f->sticks[2 * 3 * 3 + 0] = -0.6f;       // the engine's left stick: left 0.6, up 0.8
    f->sticks[2 * 3 * 3 + 1] = 0.8f;
    f->devType[2] = 1;                      // DirectInput path: the engine's stick values
    s_frame = 7;
    s_padFrame[2] = 7;
    s_padRaw[2] = (1 << 13);                // PAD B
    RawInput in;
    Snapshot(ctx, in);
    ActionState as;
    Evaluate(d, in, as);
    WriteGame(ctx, as, in);
    bool keys = f->kb[VK_LAYER_ON] == 0x80 && f->kb[0x8C] == 0x80 && f->kb[0x8F] == 0x80 && f->kb[0x8D] == 0x80 &&
                f->kb[0x8E] == 0 && f->kb[0x97] == 0x80 && f->kb[0x99] == 0x80 && f->kb[0x98] == 0 &&
                f->kb[0x88] == 0x80 && f->kb[0x8A] == 0x80 && f->kb[0x20] == 0x80;
    float sx = f->sticks[18], sy = f->sticks[19];
    OPT_EXPECT(keys && fabs(sx + 0.6f) < 1e-5f && fabs(sy - 0.8f) < 1e-5f,
               "Space + L Shift + left click + pad B + stick up-left: DASH SCREAM RUN(keyboard) THROW BOOST "
               "FORWARD LEFT on, GRAB off, physical Space kept, pad stick (%.2f, %.2f)", sx, sy);
    d.bind[4][0].kind = B_NONE;             // DASH without SPACE
    d.bind[4][1].kind = B_KEY, d.bind[4][1].code = 'Q';
    Evaluate(d, in, as);
    WriteGame(ctx, as, in);
    OPT_EXPECT(f->kb[0x8C] == 0 && f->kb[0x8F] == 0x80,
               "DASH rebound to Q: Space no longer dashes (virtual key %02X), SCREAM still on Space", f->kb[0x8C]);
    bool motionActs = as.down[A_TILT_LEFT] && !as.down[A_TILT_RIGHT] && !as.down[A_SHAKE_X] && as.down[A_SCREAM] &&
                      f->kb[0] == 0;
    in.wiggle[1] = true;
    in.wheelActive = 2;
    Evaluate(d, in, as);
    motionActs = motionActs && as.down[A_SHAKE_Y] && as.down[A_MARACA] && !as.down[A_SHAKE_X];
    in.wiggle[1] = false;
    in.wheelActive = 0;
    OPT_EXPECT(motionActs, "left click = TILT LEFT (and THROW), Space = SCREAM; an up-down wiggle = SHAKE UP/DOWN, the "
               "wheel = MARACA SHAKE; no key written for the motion rows");
    {   // a mouse button tilts only when held still; a key tilts at once
        TiltHold hold[N_SLOTS];
        memset(hold, 0, sizeof(hold));
        RawInput ti;
        memset(&ti, 0, sizeof(ti));
        ti.slot = -1;
        ti.keys[0x01] = 0x80;
        bool early = TiltDown(d, A_TILT_LEFT, hold, ti, 10.00);
        bool mid = TiltDown(d, A_TILT_LEFT, hold, ti, 10.20);
        bool still = TiltDown(d, A_TILT_LEFT, hold, ti, 10.26);
        ti.mouse[0] = 0.2f;                                         // shaken while tilted: stays
        bool kept = TiltDown(d, A_TILT_LEFT, hold, ti, 10.40);
        ti.keys[0x01] = 0;
        bool released = TiltDown(d, A_TILT_LEFT, hold, ti, 10.50);
        ti.keys[0x01] = 0x80;                                       // a drag: moving during the first quarter second
        ti.mouse[0] = 0.02f;
        TiltDown(d, A_TILT_LEFT, hold, ti, 11.00);
        TiltDown(d, A_TILT_LEFT, hold, ti, 11.10);
        bool drag = TiltDown(d, A_TILT_LEFT, hold, ti, 11.60);
        Settings k = d;
        k.bind[A_TILT_LEFT][0].kind = B_KEY, k.bind[A_TILT_LEFT][0].code = 'Q';
        memset(hold, 0, sizeof(hold));
        memset(&ti, 0, sizeof(ti));
        ti.slot = -1;
        ti.keys['Q'] = 0x80;
        bool key = TiltDown(k, A_TILT_LEFT, hold, ti, 12.00);
        OPT_EXPECT(!early && !mid && still && kept && !released && !drag && key,
                   "tilt on a mouse button: not at once %d, not after 0.2 s %d, after 0.26 s held still %d, kept while "
                   "the mouse shakes %d, off on release %d, never while dragging %d; on a key at once %d", !early, !mid,
                   still, kept, !released, !drag, key);
    }
    f->mouseScale = 2.0f;
    f->mouseMove[0] = 36.0f;                // 72 pixels of a 720-line view
    f->mouseMove[1] = -18.0f;
    f->viewport[0] = 720;
    f->mouse = 0x10 | 1;
    Snapshot(ctx, in);
    OPT_EXPECT(fabs(in.mouse[0] - 0.1f) < 1e-6f && fabs(in.mouse[1] + 0.05f) < 1e-6f && in.wheel == 2,
               "snapshot: movement x %.3f y %.3f heights (want 0.1, -0.05), wheel %d (want 2, down)", in.mouse[0],
               in.mouse[1], in.wheel);
    f->mouse = 0;
    f->mouseMove[0] = f->mouseMove[1] = 0.0f;
    f->viewport[0] = 0;
    Snapshot(ctx, in);
    Evaluate(d, in, as);
    f->stickEnabled[2 * 3] = 0;
    f->sticks[18] = f->sticks[19] = 0;
    Snapshot(ctx, in);
    Evaluate(d, in, as);
    WriteGame(ctx, as, in);
    OPT_EXPECT(f->sticks[18] == 0 && f->sticks[19] == 0 && !as.down[A_FORWARD],
               "stick 0 switched off by the scripts: not written, no stick movement");
    f->active = 0;
    Snapshot(ctx, in);
    Evaluate(d, in, as);
    WriteGame(ctx, as, in);
    OPT_EXPECT(!AnyDown(in) && f->kb[0x8F] == 0 && f->kb[VK_LAYER_ON] == 0x80, "no focus: no action is down");
    f->active = 1;
    MaskGame(ctx);
    OPT_EXPECT(f->kb[0x20] == 0 && f->kb[VK_LAYER_ON] == 0x80 && f->mouse == 0 && f->joyMask[2 * 0xA4 / 4] == 0,
               "masked: keys, mouse and pad cleared, the layer flag kept");

    // volumes
    OPT_EXPECT(VolumeToMb(100) == 0 && VolumeToMb(50) == -1204 && VolumeToMb(10) == -4000 && VolumeToMb(1) == -8000 &&
               VolumeToMb(0) == -10000, "volume -> mB: 100%% %d, 50%% %d, 10%% %d, 1%% %d, 0%% %d", VolumeToMb(100),
               VolumeToMb(50), VolumeToMb(10), VolumeToMb(1), VolumeToMb(0));

    // the page: navigation, capture, drawing calls
    std::vector<std::string> calls;
    s_testCalls = &calls;
    s_set = d;
    s_layerOn = true;
    memset(f->kb, 0, sizeof(f->kb));
    s_prevIn = s_in = in;
    memset(&s_in, 0, sizeof(s_in));
    s_in.slot = -1;
    s_prevIn = s_in;
    OpenPage(0.0);
    bool drew = false;
    for (size_t i = 0; i < calls.size(); ++i)
        if (calls[i].find("text") == 0 && calls[i].find("MOVE FORWARD") != std::string::npos) drew = true;
    OPT_EXPECT(s_ui.open && drew && calls.size() > 40, "open: %d Magma calls, row names written", (int)calls.size());
    size_t before = calls.size();
    PageFrame(ctx, 0.1);                    // nothing held: armed
    OPT_EXPECT(calls.size() == before, "an idle frame sends nothing (%d calls)", (int)(calls.size() - before));
    RawInput idle = s_in;
    s_prevIn = idle;
    s_in.keys[VK_DOWN] = 0x80;
    PageFrame(ctx, 0.2);
    s_prevIn = s_in;
    s_in = idle;
    PageFrame(ctx, 0.3);
    s_prevIn = s_in;
    s_in.keys[VK_RIGHT] = 0x80;
    PageFrame(ctx, 0.4);
    s_prevIn = s_in;
    s_in = idle;
    PageFrame(ctx, 0.5);
    OPT_EXPECT(s_ui.zone == Z_LIST && s_ui.row == 0 && s_ui.col == 1,
               "down, right: list row 0 slot 2 (zone %d row %d col %d)",
               s_ui.zone, s_ui.row, s_ui.col);
    s_prevIn = s_in;
    s_in.keys[VK_RETURN] = 0x80;
    PageFrame(ctx, 0.6);
    OPT_EXPECT(s_ui.capturing, "Enter on a slot: capturing");
    s_prevIn = s_in;
    s_in = idle;
    PageFrame(ctx, 0.7);                    // released: armed
    s_prevIn = s_in;
    s_in.keys['K'] = 0x80;
    PageFrame(ctx, 0.8);
    OPT_EXPECT(!s_ui.capturing && s_set.bind[0][1].kind == B_KEY && s_set.bind[0][1].code == 'K',
               "K pressed: MOVE FORWARD slot 2 = K");
    s_prevIn = s_in;
    s_in = idle;
    PageFrame(ctx, 0.9);
    for (int i = 0; i < 12; ++i) {          // down to the last row and past it: the footer
        s_prevIn = s_in;
        s_in.keys[VK_DOWN] = (i % 2) ? 0 : 0x80;
        PageFrame(ctx, 1.0 + i * 0.05);
    }
    s_prevIn = s_in;
    s_in = idle;
    PageFrame(ctx, 2.0);
    OPT_EXPECT(s_ui.row == 6 && s_ui.scroll == 0, "six rows down: row %d, scroll %d", s_ui.row, s_ui.scroll);
    for (int i = 0; i < 8; ++i) {
        s_prevIn = s_in;
        s_in.keys[VK_DOWN] = (i % 2) ? 0 : 0x80;
        PageFrame(ctx, 2.1 + i * 0.05);
    }
    OPT_EXPECT(s_ui.row == 10 && s_ui.scroll == 3, "four more: row %d, scroll %d (the list scrolls)", s_ui.row,
               s_ui.scroll);
    s_ui.row = A_MARACA;                    // capture on a motion row: the wheel either way
    s_ui.col = 1;
    KeepVisible();
    s_prevIn = s_in;
    s_in = idle;
    s_in.keys[VK_RETURN] = 0x80;
    PageFrame(ctx, 2.6);
    s_prevIn = s_in;
    s_in = idle;
    PageFrame(ctx, 2.65);
    s_prevIn = s_in;
    s_in = idle;
    s_in.wheel = 1;
    PageFrame(ctx, 2.7);
    bool capWheel = !s_ui.capturing && s_set.bind[A_MARACA][1].kind == B_WHEEL && s_set.bind[A_MARACA][1].code == 0;
    s_ui.row = A_ROTATE_CCW;                // the last row
    s_ui.col = 2;
    KeepVisible();
    s_prevIn = s_in;
    s_in = idle;
    PageFrame(ctx, 2.72);                   // the page arms again after a capture
    s_prevIn = s_in;
    s_in = idle;
    s_in.keys[VK_RETURN] = 0x80;
    PageFrame(ctx, 2.75);
    s_prevIn = s_in;
    s_in = idle;
    PageFrame(ctx, 2.8);
    s_prevIn = s_in;
    s_in = idle;
    s_in.wiggle[2] = true;
    PageFrame(ctx, 2.85);
    bool capWiggle = !s_ui.capturing && s_set.bind[A_ROTATE_CCW][2].kind == B_MOUSEMOVE &&
                     s_set.bind[A_ROTATE_CCW][2].code == 2;
    OPT_EXPECT(capWheel && capWiggle && s_ui.scroll == N_ACTIONS - ROWS,
               "capture: the wheel on MARACA SHAKE slot 2 = WHEEL %d, clockwise mouse circles on SHAKE COUNTER-CW slot 3 "
               "= MOUSE_CIRCLE_CW %d, scrolled to the end (%d)", capWheel, capWiggle, s_ui.scroll);
    s_prevIn = s_in;
    s_in = idle;
    PageFrame(ctx, 2.9);
    s_ui.row = 10;
    s_ui.col = 0;
    s_ui.scroll = 3;
    s_prevIn = s_in;
    s_in = idle;
    s_in.keys[VK_TAB] = 0x80;
    PageFrame(ctx, 3.0);
    s_prevIn = s_in;
    s_in = idle;
    PageFrame(ctx, 3.1);
    s_prevIn = s_in;
    s_in.keys[VK_DOWN] = 0x80;
    PageFrame(ctx, 3.2);
    s_prevIn = s_in;
    s_in = idle;
    PageFrame(ctx, 3.3);
    s_prevIn = s_in;
    s_in.keys[VK_LEFT] = 0x80;
    PageFrame(ctx, 3.4);
    OPT_EXPECT(s_ui.tab == TAB_AUDIO && s_ui.zone == Z_LIST && s_ui.row == 0 && s_set.volume[0] == 95,
               "Tab, down, left: audio tab, master 95%% (tab %d zone %d volume %d)", s_ui.tab, s_ui.zone,
               s_set.volume[0]);
    char want[48];
    sprintf(want, "int %08X 115 0 0", Gid("Row0Name"));
    bool hidden = false;
    for (size_t i = 0; i < calls.size(); ++i)
        if (calls[i] == want) hidden = true;
    OPT_EXPECT(hidden, "audio tab: the controls rows are hidden");
    f->viewport[0] = 480, f->viewport[1] = 854, f->viewport[2] = 0, f->viewport[3] = 0;
    f->cursor[0] = (BAR_X0 + BAR_X1) / 2;
    f->cursor[1] = AUDIO_Y0 + AUDIO_DY * 2;
    s_prevIn = s_in;
    s_in = idle;
    s_in.keys[VK_LBUTTON] = 0x80;
    PageFrame(ctx, 3.6);
    OPT_EXPECT(s_ui.row == 2 && s_set.volume[2] == 50, "click in the middle of the effects bar: row %d, effects %d%%",
               s_ui.row, s_set.volume[2]);
    // graphics tab (nothing installed in the test: every row but the Wii effects reads unavailable)
    s_prevIn = s_in;
    s_in = idle;
    s_in.keys[VK_TAB] = 0x80;
    PageFrame(ctx, 3.62);
    s_prevIn = s_in;
    s_in = idle;
    PageFrame(ctx, 3.64);
    sprintf(want, "int %08X 115 0 1", Gid("Gfx0Name"));
    char wantText[64];
    sprintf(wantText, "text %08X WINDOW", Gid("Gfx0Value"));
    bool gfxShown = false, gfxText = false;
    for (size_t i = 0; i < calls.size(); ++i) {
        if (calls[i] == want) gfxShown = true;
        if (calls[i] == wantText) gfxText = true;
    }
    OPT_EXPECT(s_ui.tab == TAB_GRAPHICS && gfxShown && gfxText,
               "Tab: graphics tab, rows shown, DISPLAY = WINDOW (tab %d)", s_ui.tab);
    s_prevIn = s_in;
    s_in.keys[VK_DOWN] = 0x80;
    PageFrame(ctx, 3.66);
    s_prevIn = s_in;
    s_in = idle;
    PageFrame(ctx, 3.67);
    GfxOptions g0 = s_set.gfx;
    s_prevIn = s_in;
    s_in.keys[VK_RIGHT] = 0x80;
    PageFrame(ctx, 3.68);
    s_prevIn = s_in;
    s_in = idle;
    PageFrame(ctx, 3.69);
    OPT_EXPECT(s_ui.zone == Z_LIST && s_ui.row == GR_DISPLAY && !memcmp(&g0, &s_set.gfx, sizeof(g0)),
               "right on DISPLAY without the graphics module: unchanged");
    GfxChange(GR_FILTER, 1);
    int f1 = s_set.gfx.aniso;
    GfxChange(GR_FILTER, -1);
    GfxChange(GR_FILTER, -1);
    int f2 = s_set.gfx.aniso;
    GfxChange(GR_DETAIL, 1);
    GfxChange(GR_RES, 1);
    int r1 = s_set.gfx.resolution;
    GfxChange(GR_RES, 1);
    GfxChange(GR_RES, -1);
    GfxChange(GR_RES, -1);
    int r2 = s_set.gfx.resolution;
    GfxChange(GR_RES, -1);
    int r3 = s_set.gfx.resolution;
    OPT_EXPECT(f1 == 2 && f2 == 16 && s_set.gfx.highDetail == 1 && r1 == 360 && r2 == 0 && r3 == 2160,
               "values: filtering 0 -> 2, back twice -> %d; high detail on; resolution native -> %d, 480, 360, "
               "native (%d), then below native wraps to %d", f2, r1, r2, r3);
    s_set.gfx.frameRate = 60;
    GfxChange(GR_FPS, 1);
    int q1 = s_set.gfx.frameRate;
    GfxChange(GR_FPS, -1);
    GfxChange(GR_FPS, -1);
    int q2 = s_set.gfx.frameRate;
    GfxChange(GR_FPS, -1);
    int q3 = s_set.gfx.frameRate;
    GfxChange(GR_FPS, 1);
    int q4 = s_set.gfx.frameRate;
    TimingSetCap(TimingIniCap());
    OPT_EXPECT(q1 == 120 && q2 == 30 && q3 == 0 && q4 == 30,
               "frame rate: 60 -> %d, back twice -> %d, below 30 -> uncapped (%d), then %d", q1, q2, q3, q4);
    Bind pair[N_SLOTS];
    ParseList("LALT,RALT,PAD_B,RSHIFT", pair, "test", NULL);
    OPT_EXPECT(pair[0].code == VK_MENU && pair[1].kind == B_PAD && pair[2].code == VK_SHIFT,
               "LALT,RALT,PAD_B,RSHIFT: one ALT, PAD B, SHIFT (the slot the second Alt took is free)");
    s_prevIn = s_in;
    s_in = idle;
    s_in.keys[VK_TAB] = 0x80;
    PageFrame(ctx, 3.72);
    s_prevIn = s_in;
    s_in = idle;
    PageFrame(ctx, 3.74);
    OPT_EXPECT(s_ui.tab == TAB_CONTROLS, "Tab on the last tab: back to the controls tab (tab %d)", s_ui.tab);

    s_prevIn = s_in;
    s_in = idle;
    s_in.keys[VK_ESCAPE] = 0x80;
    s_path.clear();
    bool still = PageFrame(ctx, 3.8);
    OPT_EXPECT(!still && !s_ui.open && s_swallow, "Esc: page closed, input held back until released");

    s_testCalls = NULL;
    s_set = saved;
    s_testLog = oldLog;
    s_realEngine = true;
    s_swallow = false;
    memset(&s_ui, 0, sizeof(s_ui));
    delete f;
    return fails;
}

}  // namespace

// ---------------------------------------------------------------------------------------------------------------
// entry points
// ---------------------------------------------------------------------------------------------------------------
void OptionsAttach(const std::string& iniPath) {
    s_inAttach = true;
    std::string v;
    bool enabled = !(ReadIniKey(iniPath, "options", "enabled", v) && (Trim(v) == "0"));
    s_path = g_dllDir + "options.ini";
    std::vector<std::string> problems;
    LoadSettings(s_path, s_set, problems);
    for (size_t i = 0; i < problems.size(); ++i) OptLog("options.ini: %s", problems[i].c_str());
    if (!enabled) {
        OptLog("[options] enabled=0: no Options page, no control bindings, no volumes");
        s_inAttach = false;
        return;
    }
    ProcessImage img;
    uint8_t probe[5];
    if (!img.Read(CALL_INPUT_POLL, probe, 5)) {
        OptLog("host process is not the RGH PC executable (no code at %08X): Options not installed", CALL_INPUT_POLL);
        s_inAttach = false;
        return;
    }
    std::string rep;
    int bad = Verify(img, rep);
    if (bad) {
        OptLog("the PC executable differs from the expected 2010 build in %d place(s): Options NOT installed (with "
               "[controls] mode=pc the port's scripts then read no controls)", bad);
        size_t pos = 0;
        while (pos < rep.size()) {
            size_t end = rep.find('\n', pos);
            std::string line = rep.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
            if (line.find("FAIL") != std::string::npos) OptLog("%s", line.c_str());
            pos = end == std::string::npos ? rep.size() : end + 1;
        }
        s_inAttach = false;
        return;
    }
    s_pageId = Crc(PAGE_NAME);
    bool raw = PatchCall(CALL_RAW_DINPUT, (void*)&RawDInputHook) && PatchCall(CALL_RAW_KPAD, (void*)&RawKpadHook) &&
               PatchCall(CALL_RAW_XINPUT, (void*)&RawXInputHook);
    bool poll = raw && PatchCall(CALL_INPUT_POLL, (void*)&InputPollHook);
    s_pageOn = PatchCall(CALL_MGM_UPDATE, (void*)&MagmaUpdateHook);
    std::string gv;
    if (ReadIniKey(iniPath, "options", "graphics", gv) && Trim(gv) == "0") {
        OptLog("[options] graphics=0: graphics settings not installed");
    } else {
        GfxAttach();
        GfxStartupVsync(s_set.gfx.vsync);
    }
    bool pc = CtlPcMode();                          // CtlAttach ran first
    s_layerOn = poll && pc;
    OptLog("PC executable verified: Options page driver %s (page %s = %08X), control bindings %s, volumes "
           "master %d%% music %d%% effects %d%% voices %d%% (%s)", s_pageOn ? "installed" : "NOT installed", PAGE_NAME,
           s_pageId, s_layerOn ? "on (virtual keys 88-8F, 97-9B, layer flag 07)" :
           pc ? "NOT installed" : "off ([controls] mode is not pc)", s_set.volume[0], s_set.volume[1],
           s_set.volume[2], s_set.volume[3], s_path.c_str());
    s_inAttach = false;
}

// wm_sm64.cpp: the game's music off (its own groups only - effects, ambience and dialogue keep their volume)
// while SM64's plays, and back to the player's setting afterwards
void OptionsSilenceMusic(bool on) {
    if (s_musicSilenced == on) return;
    s_musicSilenced = on;
    ApplyVolumes(s_game, s_set, true);
}

// the rabbids' own effects, while Mario is the one being played.  Dialogue keeps its volume: it is the voices
// slider, a different group again.
void OptionsSilenceEffects(bool on) {
    if (s_effectsSilenced == on) return;
    s_effectsSilenced = on;
    ApplyVolumes(s_game, s_set, true);
}

// The rabbids themselves, while Mario is the one being played: their voices are the dialogue group, so this takes
// the screaming away and leaves the level's own noises - things breaking, things going in the cart - alone.
void OptionsSilenceVoices(bool on) {
    if (s_voicesSilenced == on) return;
    s_voicesSilenced = on;
    ApplyVolumes(s_game, s_set, true);
}

// while Mario has the level: silence these groups outright (empty list = back to normal)
void OptionsSilenceExtra(const int* groups, int n) {
    if (n > 8) n = 8;
    bool same = n == s_extraSilentN;
    for (int i = 0; same && i < n; ++i) same = s_extraSilent[i] == groups[i];
    if (same) return;
    int was[8], wasN = s_extraSilentN;
    for (int i = 0; i < wasN; ++i) was[i] = s_extraSilent[i];
    s_extraSilentN = n;
    for (int i = 0; i < n; ++i) s_extraSilent[i] = groups[i];
    for (int i = 0; i < wasN; ++i) s_appliedKnown[was[i]] = false;   // so the slider pass writes them again
    ApplyVolumes(s_game, s_set, true);
}

void OptionsAfterConfig() {
    for (size_t i = 0; i < s_pending.size(); ++i) Log("OPTIONS: %s", s_pending[i].c_str());
    s_pending.clear();
    GfxAfterConfig();
}

extern "C" {

// wmtest only: verify the Options hook sites against an exe file.  Returns the number of differences (-1: unreadable).
int __cdecl WiimoteOptionsCheckExe(const char* exePath, char* out, int outSize) {
    FileImage img;
    std::string rep;
    int bad = -1;
    if (!img.Load(exePath)) rep = "  FAIL  cannot read " + std::string(exePath ? exePath : "(null)") + "\n";
    else bad = Verify(img, rep) + GfxVerify(img, rep);
    CopyOut(rep, out, outSize);
    return bad;
}

// wmtest only: names, settings file, actions on a fake engine, the page's navigation.  Returns the number of failures.
int __cdecl WiimoteOptionsSelfTest(char* out, int outSize) {
    std::string rep;
    int fails = RunSelfTest(rep);
    CopyOut(rep, out, outSize);
    return fails;
}

}  // extern "C"
