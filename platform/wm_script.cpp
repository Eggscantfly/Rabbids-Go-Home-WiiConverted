// wm_script.cpp - script natives the PC executable answers for the PC or leaves as empty stubs, answered like the Wii
// executable does ([script] in wiimote.ini).  Each native is replaced per word in the script native table once its
// engine handler (CRC32) and its ViD::RegisterScript entry are verified: all wanted words or none.
//
// platform=auto|wii|pc   ViD_PlatformCurrentGet (word 0770FFFF, handler 00545E40 calls CFG_e_GetCurrentPlatform
//                        005EC610 = "xor eax, eax": 0).  The Wii executable answers 1 (ViD_PlatformCurrentGet_C calls
//                        80080CF8).  The scripts both releases share branch on it: PJ_BunniesGroupObject sets
//                        ai_control_type[0..1] = 2 (remote + Nunchuk) on 1, GST_MapManager_Bunnies and GST_Global_Bunnies
//                        pick the rabbid groups' controller ids (0 on the Wii), the Wii-only CUSTO_LIB and MUSICMAKER_LIB
//                        take their Wii paths.  auto = 1 with the virtual Wii remote ([controls] mode=wii), 0 with
//                        PC-native controls (mode=pc: the scripts' own PC control types).
// pointer_state=1        IO_JoystickPointerStateSet(id, state) (word 00A3FFFF; handler 005EE5E0 pops both arguments and
//                        calls an empty function).  The Wii scripts switch the pointer off while a video or cinematic
//                        runs (GST_Global_Bunnies) and on again (activator_event, GST_Photo).  The state is kept per
//                        controller; while controller n's pointer is off its IR pointer reads as not valid (the virtual
//                        remote's samples for channel n in mode=wii, the pointer natives in mode=pc).
// return_to_menu=quit    WII_ReturnToMenu (word 0915FFFF, the shared no-op handler 00560580).  The Wii leaves the game
//                        for the system menu (80044D6C8); quit closes the game window (the engine ends the process on
//                        WM_CLOSE), none leaves the stub.
// (always)               the file natives IO_FileOpenRead .. IO_FileOpen (words 00B4FFFF..00BEFFFF, handlers
//                        005EE6A0..005EEB00).  The Wii's never reach a disk: IO_FileOpenRead_C 800C4700 and
//                        IO_FileGetI_C 800C4B80 answer -1, the GetC / GetF / EOF wrappers -1 / -1.0 / 1, a read
//                        IO_FileOpen -1, and the writes go to the development kit's host over HIO2
//                        (LogFileHandlerRequest 8001F82C, LogFileWrite 8001F9A4), nothing on a retail console; Put* and
//                        Close skip handle -1.  The PC handlers are fopen / fgetc / fwrite / fclose on real files and pass
//                        a failed fopen's NULL on to the CRT, whose invalid-parameter check ends the game
//                        (c000000d in fwrite).  CUSTO_LIB_SetJpeg (PNJ_CannonBall's CUSTO_LIB: leaving the paint mode,
//                        applying a texture, saving a figurine in the Wii remote) writes the rabbid's JPEG to
//                        "C:\Documents and Settings\All Users\Bureau\Custo.jpg" whenever ViD_PlatformCurrentGet() is not
//                        1, which it is not with platform=pc.  Here every open answers -1 and the other file natives
//                        read -1 (-1.0, EOF 1) or do nothing.
// mix_guard=1            the ACT module's mix layer natives ACT_ItemMixFrameSet, ACT_ItemMixTimeRatioSet, ACT_MixFinished
//                        and ACT_MixOff (handlers 0058D5B0, 0058D800, 0058DA40, 0058CE90) index the actor's mix layers
//                        (+0x30 + 4 x layer) without a range check; ACT_MixValueSet / Get check it (< 5).  ACT_MixAdd
//                        (0058CA20 -> 00553060) answers -1 when the actor has no free layer (1..4) or no such action,
//                        and the Wii's CR_STATE_TATA (the rabbid in the remote; Rabbid Do-What-I-Do from its second
//                        level) hands that -1 to ACT_ItemMixFrameSet: slot -1 is NULL, access violation at 00552760.
//                        A layer outside 0..5 is skipped (ACT_MixFinished answers 1: there is nothing left to play);
//                        ACT_MixAdd's -1 is logged with its action.
// enabled=1              0 leaves all of these natives to the engine.
#include "wiimote.h"

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <vector>

using namespace wmpatch;

namespace {

typedef void* (__cdecl* NativeFn)(void* ip);

// script VM stack of the PC executable (the self-test points these at a fake stack)
struct VmCtx {
    uint32_t* vmIndex;      // 0x00A7E188 number of entries
    uint32_t* vmOffset;     // 0x00A7E18C bytes used in the value area
    uint32_t* vmData;       // 0x00A7E198 value area
    uint32_t* vmPtrs;       // 0x00A7E19C entry pointers
    uint32_t* scrCtx;       // 0x00A72530 the script context (+0x2380C the object the next native acts on)
};
VmCtx s_game = { (uint32_t*)0x00A7E188, (uint32_t*)0x00A7E18C, (uint32_t*)0x00A7E198, (uint32_t*)0x00A7E19C,
                 (uint32_t*)0x00A72530 };
VmCtx* s_vm = &s_game;

// the script context: a native reads the object it acts on (+0x2380C, an "obj." prefix) and puts the default back
const uint32_t CTX_OBJECT = 0x2380C, CTX_DEFAULT = 0x23810;
const int32_t  MIX_LAYERS = 6;

enum { PLATFORM_PC = 0, PLATFORM_WII = 1 };
enum { F_PLATFORM, F_POINTER, F_RETURN, F_FILE, F_MIX };

// the engine handlers the mix guard passes valid calls to (the self-test points these at fakes)
enum { E_MIXADD, E_FRAMESET, E_TIMERATIOSET, E_FINISHED, E_MIXOFF, N_ENGINE };
NativeFn s_engine[N_ENGINE] = {
    (NativeFn)(uintptr_t)0x0058CA20, (NativeFn)(uintptr_t)0x0058D5B0, (NativeFn)(uintptr_t)0x0058D800,
    (NativeFn)(uintptr_t)0x0058DA40, (NativeFn)(uintptr_t)0x0058CE90,
};

bool         s_enabled = true;
int          s_platform = PLATFORM_WII;
bool         s_pointerFeature = true;
bool         s_returnQuit = true;
bool         s_mixGuard = true;
LONG         s_mixLogs;                 // mix layer calls skipped and ACT_MixAdd failures logged so far (the first 16)
LONG         s_pointerOn[4] = { 1, 1, 1, 1 };
bool         s_inAttach;
bool         s_verified;
bool         s_testNoQuit;              // self-test: WII_ReturnToMenu closes nothing
std::vector<std::string> s_pending;
std::string* s_testLog;
LONG         s_seen[3];
LONG         s_fileLogs;                // file native calls logged so far (the first 8)

void ScrLog(const char* fmt, ...) {
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
        Log("SCRIPT: %s", buf);
    }
}

bool IsOn(const std::string& v) {
    return v == "1" || _stricmp(v.c_str(), "on") == 0 || _stricmp(v.c_str(), "true") == 0 ||
           _stricmp(v.c_str(), "yes") == 0;
}

// ---------------------------------------------------------------------------------------------------------------
// script VM protocol: arguments are popped last first, one result is pushed, the handler returns ip + 4
// ---------------------------------------------------------------------------------------------------------------
inline uint32_t* Ptrs() { return (uint32_t*)(uintptr_t)*s_vm->vmPtrs; }

int32_t Arg(int n, int i) { return *(int32_t*)(uintptr_t)Ptrs()[*s_vm->vmIndex - n + i]; }

void PopInts(int n) {
    *s_vm->vmIndex -= n;
    *s_vm->vmOffset -= 4 * n;
}

void PushInt(int32_t v) {
    uint8_t* at = (uint8_t*)(uintptr_t)(*s_vm->vmData + *s_vm->vmOffset);
    Ptrs()[*s_vm->vmIndex] = (uint32_t)(uintptr_t)at;
    *s_vm->vmIndex += 1;
    *s_vm->vmOffset += 4;
    memcpy(at, &v, 4);
}

// ---------------------------------------------------------------------------------------------------------------
// handlers
// ---------------------------------------------------------------------------------------------------------------
void* __cdecl H_PlatformCurrentGet(void* ip) {  // ViD_PlatformCurrentGet() -> 1 Wii, 0 PC
    if (InterlockedExchange(&s_seen[F_PLATFORM], 1) == 0)
        ScrLog("first call: ViD_PlatformCurrentGet() -> %d (%s)", s_platform, s_platform == PLATFORM_WII ? "Wii" : "PC");
    PushInt(s_platform);
    return (uint8_t*)ip + 4;
}

void* __cdecl H_PointerStateSet(void* ip) {     // IO_JoystickPointerStateSet(id, state)
    int32_t id = Arg(2, 0), state = Arg(2, 1);
    PopInts(2);
    if (id >= 0 && id < 4) {
        LONG on = state != 0 ? 1 : 0;
        if (InterlockedExchange(&s_pointerOn[id], on) != on)
            ScrLog("IO_JoystickPointerStateSet(%d, %d): pointer %s", id, state, on ? "on" : "off");
    }
    return (uint8_t*)ip + 4;
}

BOOL CALLBACK CloseGameWindow(HWND h, LPARAM) {  // the game's main window: visible, top-level, no owner
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    if (pid == GetCurrentProcessId() && IsWindowVisible(h) && GetWindow(h, GW_OWNER) == NULL)
        PostMessageA(h, WM_CLOSE, 0, 0);
    return TRUE;
}

void* __cdecl H_ReturnToMenu(void* ip) {        // WII_ReturnToMenu()
    ScrLog("WII_ReturnToMenu(): %s", s_testNoQuit ? "test, nothing closed" : "closing the game window");
    if (!s_testNoQuit) EnumWindows(CloseGameWindow, 0);
    return (uint8_t*)ip + 4;
}

// the file natives: no disk access, the Wii's answers (see the top of the file)
void CopyScriptString(uint32_t p, char* out, size_t n) {
    out[0] = 0;
    if (!p) return;
    __try {
        const char* s = (const char*)(uintptr_t)p;
        size_t i = 0;
        for (; i + 1 < n && s[i]; ++i) out[i] = s[i];
        out[i] = 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        out[0] = 0;
    }
}

void FileOpenLog(const char* name, uint32_t path) {     // the first 8 opens
    if (InterlockedIncrement(&s_fileLogs) > 8) return;
    char p[260];
    CopyScriptString(path, p, sizeof(p));
    ScrLog("%s(\"%s\") -> -1: the Wii's file natives open no file", name, p);
}

void* __cdecl H_FileOpenRead(void* ip) {        // IO_FileOpenRead(path) -> -1
    FileOpenLog("IO_FileOpenRead", (uint32_t)Arg(1, 0));
    PopInts(1);
    PushInt(-1);
    return (uint8_t*)ip + 4;
}

void* __cdecl H_FileOpenWrite(void* ip) {       // IO_FileOpenWrite(path) -> -1
    FileOpenLog("IO_FileOpenWrite", (uint32_t)Arg(1, 0));
    PopInts(1);
    PushInt(-1);
    return (uint8_t*)ip + 4;
}

void* __cdecl H_FileOpen(void* ip) {            // IO_FileOpen(path, read, append, binary) -> -1
    FileOpenLog("IO_FileOpen", (uint32_t)Arg(4, 0));
    PopInts(4);
    PushInt(-1);
    return (uint8_t*)ip + 4;
}

void* __cdecl H_FileClose(void* ip) {           // IO_FileClose(handle)
    PopInts(1);
    return (uint8_t*)ip + 4;
}

void* __cdecl H_FilePut(void* ip) {             // IO_FilePutC / IO_FilePutI / IO_FilePutF(handle, value)
    PopInts(2);
    return (uint8_t*)ip + 4;
}

void* __cdecl H_FileGetInt(void* ip) {          // IO_FileGetC / IO_FileGetI(handle) -> -1
    PopInts(1);
    PushInt(-1);
    return (uint8_t*)ip + 4;
}

void* __cdecl H_FileGetFloat(void* ip) {        // IO_FileGetF(handle) -> -1.0
    const float v = -1.0f;
    int32_t bits;
    memcpy(&bits, &v, 4);
    PopInts(1);
    PushInt(bits);
    return (uint8_t*)ip + 4;
}

void* __cdecl H_FileEOF(void* ip) {             // IO_FileEOF(handle) -> 1
    PopInts(1);
    PushInt(1);
    return (uint8_t*)ip + 4;
}

// the mix layer guard (see the top of the file)
inline bool MixLayerValid(int32_t layer) { return layer >= 0 && layer < MIX_LAYERS; }

void MixLog(const char* fmt, ...) {
    if (InterlockedIncrement(&s_mixLogs) > 16) return;
    char buf[300];
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf(buf, sizeof(buf) - 1, fmt, ap);
    buf[sizeof(buf) - 1] = 0;
    va_end(ap);
    ScrLog("%s", buf);
}

// what the skipped engine handler does besides its work: the next native acts on the default object again
void DropObjectPrefix() {
    uint8_t* ctx = (uint8_t*)(uintptr_t)*s_vm->scrCtx;
    if (ctx) *(uint32_t*)(ctx + CTX_OBJECT) = *(uint32_t*)(ctx + CTX_DEFAULT);
}

void* MixSkip(void* ip, const char* name, int32_t layer, int args) {
    MixLog("%s(%d): no such mix layer (an ACT_MixAdd that answered -1): skipped", name, layer);
    DropObjectPrefix();
    PopInts(args);
    return (uint8_t*)ip + 4;
}

void* __cdecl H_MixAdd(void* ip) {              // ACT_MixAdd(action, flags, blend, a4, a5) -> layer or -1
    int32_t action = Arg(5, 0), flags = Arg(5, 1);
    void* r = s_engine[E_MIXADD](ip);
    if (*s_vm->vmIndex && Arg(1, 0) == -1)
        MixLog("ACT_MixAdd(action %d, flags %d) -> -1: no free mix layer or no such action", action, flags);
    return r;
}

void* __cdecl H_ItemMixFrameSet(void* ip) {     // ACT_ItemMixFrameSet(layer, frame)
    int32_t layer = Arg(2, 0);
    if (MixLayerValid(layer)) return s_engine[E_FRAMESET](ip);
    return MixSkip(ip, "ACT_ItemMixFrameSet", layer, 2);
}

void* __cdecl H_ItemMixTimeRatioSet(void* ip) { // ACT_ItemMixTimeRatioSet(layer, ratio)
    int32_t layer = Arg(2, 0);
    if (MixLayerValid(layer)) return s_engine[E_TIMERATIOSET](ip);
    return MixSkip(ip, "ACT_ItemMixTimeRatioSet", layer, 2);
}

void* __cdecl H_MixFinished(void* ip) {         // ACT_MixFinished(layer) -> 0 / 1
    int32_t layer = Arg(1, 0);
    if (MixLayerValid(layer)) return s_engine[E_FINISHED](ip);
    void* r = MixSkip(ip, "ACT_MixFinished", layer, 1);
    PushInt(1);
    return r;
}

void* __cdecl H_MixOff(void* ip) {              // ACT_MixOff(layer)
    int32_t layer = Arg(1, 0);
    if (MixLayerValid(layer)) return s_engine[E_MIXOFF](ip);
    return MixSkip(ip, "ACT_MixOff", layer, 1);
}

// ---------------------------------------------------------------------------------------------------------------
// the PC executable (2010 build): what is verified and what is replaced
// ---------------------------------------------------------------------------------------------------------------
struct ScrWord {
    uint32_t key, oldFn, len, crc;
    uint32_t reg;           // imm32 of "mov [esp+x], handler" in ViD::RegisterScript (or the module's registrar)
    bool     reg32;         // C7 84 24 disp32 imm32 (else C7 44 24 disp8 imm32)
    NativeFn fn;
    const char* name;
    int      feature;
    bool     module;        // a module word (its key's low half is assigned at registration): matched by its handler
};

const ScrWord kWords[] = {
    { 0x0770FFFF, 0x00545E40, 0x045, 0x6AFB1F4F, 0x00544630, true,  H_PlatformCurrentGet, "ViD_PlatformCurrentGet", F_PLATFORM },
    { 0x00A3FFFF, 0x005EE5E0, 0x045, 0x5E09E42C, 0x005432F1, false, H_PointerStateSet, "IO_JoystickPointerStateSet", F_POINTER },
    { 0x0915FFFF, 0x00560580, 0x008, 0xBAC82FF4, 0x005458F3, true,  H_ReturnToMenu, "WII_ReturnToMenu", F_RETURN },
    { 0x00B4FFFF, 0x005EE6A0, 0x06E, 0xB59404E7, 0x0054349B, false, H_FileOpenRead, "IO_FileOpenRead", F_FILE },
    { 0x00B5FFFF, 0x005EE830, 0x031, 0x1079FADB, 0x005434CF, false, H_FileClose, "IO_FileClose", F_FILE },
    { 0x00B6FFFF, 0x005EE870, 0x073, 0x33A759A2, 0x00543503, false, H_FileGetInt, "IO_FileGetC", F_FILE },
    { 0x00B7FFFF, 0x005EE710, 0x06E, 0x2AE878D5, 0x00543537, false, H_FileOpenWrite, "IO_FileOpenWrite", F_FILE },
    { 0x00B8FFFF, 0x005EE8F0, 0x046, 0x61F0DEC5, 0x0054356B, false, H_FilePut, "IO_FilePutC", F_FILE },
    { 0x00B9FFFF, 0x005EE9C0, 0x053, 0xAFF635E9, 0x0054359F, false, H_FilePut, "IO_FilePutI", F_FILE },
    { 0x00BAFFFF, 0x005EE940, 0x077, 0xDE6EC238, 0x005435D3, false, H_FileGetInt, "IO_FileGetI", F_FILE },
    { 0x00BBFFFF, 0x005EEAA0, 0x060, 0x8DC4F56A, 0x00543607, false, H_FilePut, "IO_FilePutF", F_FILE },
    { 0x00BCFFFF, 0x005EEA20, 0x077, 0xC39DB3FC, 0x0054363B, false, H_FileGetFloat, "IO_FileGetF", F_FILE },
    { 0x00BDFFFF, 0x005EEB00, 0x082, 0xFF7970C5, 0x0054366F, false, H_FileEOF, "IO_FileEOF", F_FILE },
    { 0x00BEFFFF, 0x005EE780, 0x0AC, 0xCFCFE39A, 0x005436A3, false, H_FileOpen, "IO_FileOpen", F_FILE },
    { 0x012C000F, 0x0058CA20, 0x16F, 0x24D9D5DA, 0x0058C167, false, H_MixAdd, "ACT_MixAdd", F_MIX, true },
    { 0x012D000F, 0x0058CE90, 0x0CB, 0xA4CD1CD9, 0x0058C19D, false, H_MixOff, "ACT_MixOff", F_MIX, true },
    { 0x0131000F, 0x0058DA40, 0x104, 0x8B84D422, 0x0058C275, false, H_MixFinished, "ACT_MixFinished", F_MIX, true },
    { 0x0142000F, 0x0058D800, 0x112, 0x6D778A9D, 0x0058C425, false, H_ItemMixTimeRatioSet, "ACT_ItemMixTimeRatioSet", F_MIX, true },
    { 0x0144000F, 0x0058D5B0, 0x105, 0x510BF420, 0x0058C47D, false, H_ItemMixFrameSet, "ACT_ItemMixFrameSet", F_MIX, true },
};
const size_t N_WORDS = sizeof(kWords) / sizeof(kWords[0]);
bool s_wordDone[sizeof(kWords) / sizeof(kWords[0])];

bool Wanted(const ScrWord& w) {
    return w.feature == F_PLATFORM || w.feature == F_FILE || (w.feature == F_POINTER && s_pointerFeature) ||
           (w.feature == F_RETURN && s_returnQuit) || (w.feature == F_MIX && s_mixGuard);
}

int CountWanted() {
    int n = 0;
    for (size_t i = 0; i < N_WORDS; ++i)
        if (Wanted(kWords[i])) ++n;
    return n;
}

// "mov dword ptr [esp + disp32], handler" (C7 84 24 d32 imm32) with "push key" (68 imm32) within 0x30 bytes
bool CheckReg32(const Image& img, uint32_t reg, uint32_t handler, uint32_t key) {
    const int at = 0x30;
    uint32_t v = 0;
    uint8_t win[0x60];
    if (!img.Read(reg, &v, 4) || v != handler || !img.Read(reg - at, win, sizeof(win))) return false;
    if (!(win[at - 7] == 0xC7 && win[at - 6] == 0x84 && win[at - 5] == 0x24)) return false;
    for (int i = 0; i + 5 <= (int)sizeof(win); ++i)
        if (win[i] == 0x68 && memcmp(&win[i + 1], &key, 4) == 0) return true;
    return false;
}

// A module registrar element: "mov [esp+x], handler" (C7 44 24 xx) with "movzx eax, word [esi+0Ah]" and this word's
// "add eax, n << 16" (05 imm32) within 0x24 bytes on either side.  (CheckModuleEntry takes any add with n < 0x100;
// the ACT mix words are 0x12C and up.)
bool CheckModuleWord(const Image& img, uint32_t reg, uint32_t handler, uint32_t key) {
    const int at = 0x24;
    uint32_t v = 0;
    uint8_t win[0x48];
    if (!img.Read(reg, &v, 4) || v != handler || !img.Read(reg - at, win, sizeof(win))) return false;
    if (!(win[at - 4] == 0xC7 && win[at - 3] == 0x44 && win[at - 2] == 0x24)) return false;
    const uint32_t word = key & 0xFFFF0000;
    bool movzx = false, add = false;
    for (int i = 0; i + 5 <= (int)sizeof(win); ++i) {
        if (i >= at - 4 && i < at + 4) continue;    // the mov itself
        if (win[i] == 0x0F && win[i + 1] == 0xB7 && win[i + 2] == 0x46 && win[i + 3] == 0x0A) movzx = true;
        if (win[i] == 0x05 && memcmp(&win[i + 1], &word, 4) == 0) add = true;
    }
    return movzx && add;
}

int ScrVerify(const Image& img, std::string& rep, bool all) {
    int bad = 0;
    uint32_t got = 0;
    for (size_t i = 0; i < N_WORDS; ++i) {
        const ScrWord& w = kWords[i];
        if (!all && !Wanted(w)) continue;
        bool crcOk = CheckCrc(img, w.oldFn, w.len, w.crc, &got);
        bool regOk = w.module ? CheckModuleWord(img, w.reg, w.oldFn, w.key)
                   : w.reg32  ? CheckReg32(img, w.reg, w.oldFn, w.key)
                              : CheckRegisterScriptEntry(img, w.reg, w.oldFn, w.key);
        Report(rep, crcOk && regOk, "%-28s word %08X handler %08X len 0x%03X crc %08X (want %08X), registered at %08X%s",
               w.name, w.key, w.oldFn, w.len, got, w.crc, w.reg, regOk ? "" : " (registration differs)");
        if (!(crcOk && regOk)) ++bad;
    }
    return bad;
}

int InstallWords() {
    int done = 0;
    for (size_t i = 0; i < N_WORDS; ++i) {
        const ScrWord& w = kWords[i];
        if (!Wanted(w)) continue;
        if (!s_wordDone[i]) {
            std::string notes;
            int r = w.module ? (PatchNativeHandler(w.oldFn, (uint32_t)(uintptr_t)w.fn, notes) > 0 ? 1 : 0)
                             : PatchNativeTable(w.key, 0xFFFFFFFF, w.oldFn, (uint32_t)(uintptr_t)w.fn, notes);
            size_t pos = 0;
            while (pos < notes.size()) {
                size_t end = notes.find('\n', pos);
                ScrLog("%s: %s", w.name, notes.substr(pos, end == std::string::npos ? std::string::npos : end - pos).c_str());
                pos = end == std::string::npos ? notes.size() : end + 1;
            }
            s_wordDone[i] = r == 1;
        }
        if (s_wordDone[i]) ++done;
    }
    return done;
}

void CopyOut(const std::string& rep, char* out, int outSize) {
    if (!out || outSize <= 0) return;
    size_t n = rep.size() < (size_t)(outSize - 1) ? rep.size() : (size_t)(outSize - 1);
    memcpy(out, rep.data(), n);
    out[n] = 0;
}

// ---------------------------------------------------------------------------------------------------------------
// self-test: the handlers on a fake script stack
// ---------------------------------------------------------------------------------------------------------------
int RunSelfTest(std::string& rep) {
    int fails = 0;
    uint32_t idx = 0, off = 0;
    uint32_t ptrs[16] = {};
    uint8_t data[256] = {};
    uint32_t dataBase = (uint32_t)(uintptr_t)data, ptrBase = (uint32_t)(uintptr_t)ptrs;
    std::vector<uint8_t> scr(CTX_DEFAULT + 4, 0);
    uint32_t scrBase = (uint32_t)(uintptr_t)&scr[0];
    VmCtx fake = { &idx, &off, &dataBase, &ptrBase, &scrBase };
    VmCtx* savedVm = s_vm;
    std::string* savedLog = s_testLog;
    std::string log;
    int savedPlatform = s_platform;
    bool savedVerified = s_verified, savedPointer = s_pointerFeature, savedNoQuit = s_testNoQuit;
    s_vm = &fake;
    s_testLog = &log;
    s_verified = true;
    s_pointerFeature = true;
    s_testNoQuit = true;
    uint8_t code[8] = {};
    void* ip = code;
    char line[200];

    // 1-2: ViD_PlatformCurrentGet pushes one int, 1 in wii, 0 in pc
    for (int k = 0; k < 2; ++k) {
        idx = off = 0;
        s_platform = k == 0 ? PLATFORM_WII : PLATFORM_PC;
        void* r = H_PlatformCurrentGet(ip);
        int32_t v = idx == 1 ? *(int32_t*)(uintptr_t)ptrs[0] : -99;
        bool ok = r == (void*)((uint8_t*)ip + 4) && idx == 1 && off == 4 && v == s_platform;
        _snprintf(line, sizeof(line) - 1, "ViD_PlatformCurrentGet (platform=%s) pushes %d: entries %u, bytes %u, value %d",
                  k == 0 ? "wii" : "pc", s_platform, idx, off, v);
        line[sizeof(line) - 1] = 0;
        Report(rep, ok, "%s", line);
        if (!ok) ++fails;
    }

    // 3-5: IO_JoystickPointerStateSet(0, 0) turns controller 0's pointer off, (0, 1) on; ids outside 0..3 are ignored
    const int32_t calls[3][2] = { { 0, 0 }, { 0, 1 }, { 7, 0 } };
    const bool expectOn[3] = { false, true, true };
    for (int k = 0; k < 3; ++k) {
        idx = 2;
        off = 8;
        memcpy(data, &calls[k][0], 4);
        memcpy(data + 4, &calls[k][1], 4);
        ptrs[0] = dataBase;
        ptrs[1] = dataBase + 4;
        void* r = H_PointerStateSet(ip);
        bool on = ScriptPointerOn(0);
        bool ok = r == (void*)((uint8_t*)ip + 4) && idx == 0 && off == 0 && on == expectOn[k];
        Report(rep, ok, "IO_JoystickPointerStateSet(%d, %d): stack popped (entries %u, bytes %u), controller 0 pointer %s",
               calls[k][0], calls[k][1], idx, off, on ? "on" : "off");
        if (!ok) ++fails;
    }

    // 6: with pointer_state=0 the state is ignored
    s_pointerOn[0] = 0;
    s_pointerFeature = false;
    bool ignored = ScriptPointerOn(0);
    Report(rep, ignored, "pointer_state=0: ScriptPointerOn(0) stays on");
    if (!ignored) ++fails;
    s_pointerOn[0] = 1;
    s_pointerFeature = true;

    // 7: WII_ReturnToMenu leaves the stack alone (test: nothing is closed)
    idx = off = 0;
    void* r = H_ReturnToMenu(ip);
    bool ok = r == (void*)((uint8_t*)ip + 4) && idx == 0 && off == 0;
    Report(rep, ok, "WII_ReturnToMenu: no stack effect, returns ip + 4");
    if (!ok) ++fails;

    // 8: the disp32 registration check on a synthetic ViD::RegisterScript fragment
    struct FakeImage : Image {
        uint32_t base;
        std::vector<uint8_t> b;
        bool Read(uint32_t va, void* out, uint32_t n) const {
            if (va < base || va + n > base + b.size()) return false;
            memcpy(out, &b[va - base], n);
            return true;
        }
    } img;
    img.base = 0x00400000;
    img.b.assign(0x80, 0x90);
    const uint8_t frag[] = { 0x68, 0xFF, 0xFF, 0x70, 0x07,                        // push 0770FFFF
                             0xB9, 0xAC, 0x18, 0xA7, 0x00,                        // mov ecx, 00A718AC
                             0xC7, 0x84, 0x24, 0x68, 0x03, 0x00, 0x00,            // mov [esp+368h], ...
                             0x40, 0x5E, 0x54, 0x00 };                            // ... 00545E40
    memcpy(&img.b[0x30], frag, sizeof(frag));
    uint32_t reg = img.base + 0x30 + 17;
    bool good = CheckReg32(img, reg, 0x00545E40, 0x0770FFFF);
    bool wrongKey = !CheckReg32(img, reg, 0x00545E40, 0x0771FFFF);
    bool wrongFn = !CheckReg32(img, reg, 0x00545E41, 0x0770FFFF);
    Report(rep, good && wrongKey && wrongFn, "disp32 registration check: match %d, other word rejected %d, other handler "
           "rejected %d", good, wrongKey, wrongFn);
    if (!(good && wrongKey && wrongFn)) ++fails;

    // 9-15: the file natives pop their arguments and push the Wii's answer (opens -1, gets -1 / -1.0, EOF 1)
    static const char kPath[] = "C:\\Documents and Settings\\All Users\\Bureau\\Custo.jpg";
    const float minusOne = -1.0f;
    int32_t minusOneBits;
    memcpy(&minusOneBits, &minusOne, 4);
    struct FileCase { NativeFn fn; const char* name; int args; bool pushes; int32_t result; };
    const FileCase files[] = {
        { H_FileOpenRead, "IO_FileOpenRead(path)", 1, true, -1 },
        { H_FileOpenWrite, "IO_FileOpenWrite(path)", 1, true, -1 },
        { H_FileOpen, "IO_FileOpen(path, 0, 0, 1)", 4, true, -1 },
        { H_FilePut, "IO_FilePutI(-1, 0x12345678)", 2, false, 0 },
        { H_FileClose, "IO_FileClose(-1)", 1, false, 0 },
        { H_FileGetInt, "IO_FileGetI(-1)", 1, true, -1 },
        { H_FileGetFloat, "IO_FileGetF(-1)", 1, true, minusOneBits },
        { H_FileEOF, "IO_FileEOF(-1)", 1, true, 1 },
    };
    for (size_t k = 0; k < sizeof(files) / sizeof(files[0]); ++k) {
        const FileCase& fc = files[k];
        const int32_t path = (int32_t)(uintptr_t)kPath;
        int32_t args[4] = { -1, 0x12345678, 0, 0 };
        if (fc.fn == H_FileOpenRead || fc.fn == H_FileOpenWrite || fc.fn == H_FileOpen) {
            args[0] = path;
            args[1] = 0;
            args[3] = 1;
        }
        memset(data, 0, sizeof(data));
        idx = (uint32_t)fc.args;
        off = 4 * (uint32_t)fc.args;
        for (int a = 0; a < fc.args; ++a) {
            memcpy(data + 4 * a, &args[a], 4);
            ptrs[a] = dataBase + 4 * a;
        }
        void* fr = fc.fn(ip);
        uint32_t wantIdx = fc.pushes ? 1 : 0;
        int32_t got = fc.pushes && idx == 1 ? *(int32_t*)(uintptr_t)ptrs[0] : 0;
        bool fok = fr == (void*)((uint8_t*)ip + 4) && idx == wantIdx && off == 4 * wantIdx && got == fc.result;
        Report(rep, fok, "%s: %d argument(s) popped, %s (entries %u, bytes %u, value %08X)", fc.name, fc.args,
               fc.pushes ? "one result pushed" : "nothing pushed", idx, off, (uint32_t)got);
        if (!fok) ++fails;
    }
    s_fileLogs = 0;

    // 16-25: the mix layer guard.  A layer outside 0..5 pops the arguments, puts the default object back and answers
    // ACT_MixFinished with 1; a valid one goes to the engine handler (a fake here).  ACT_MixAdd's -1 is passed on.
    NativeFn savedEngine[N_ENGINE];
    memcpy(savedEngine, s_engine, sizeof(savedEngine));
    struct FakeEngine {
        static int& Calls() { static int n; return n; }
        static void* __cdecl Pass(void* p) { ++Calls(); return (uint8_t*)p + 4; }
        static void* __cdecl MixAddFails(void* p) { ++Calls(); PopInts(5); PushInt(-1); return (uint8_t*)p + 4; }
    };
    for (int e = 0; e < N_ENGINE; ++e) s_engine[e] = FakeEngine::Pass;
    s_engine[E_MIXADD] = FakeEngine::MixAddFails;
    struct MixCase { NativeFn fn; const char* name; int args; int32_t layer; bool engine; bool pushes; int32_t result; };
    const MixCase mixes[] = {
        { H_ItemMixFrameSet, "ACT_ItemMixFrameSet(-1, 0)", 2, -1, false, false, 0 },
        { H_ItemMixFrameSet, "ACT_ItemMixFrameSet(6, 0)", 2, 6, false, false, 0 },
        { H_ItemMixFrameSet, "ACT_ItemMixFrameSet(2, 0)", 2, 2, true, false, 0 },
        { H_ItemMixTimeRatioSet, "ACT_ItemMixTimeRatioSet(-1, 1.0)", 2, -1, false, false, 0 },
        { H_ItemMixTimeRatioSet, "ACT_ItemMixTimeRatioSet(0, 1.0)", 2, 0, true, false, 0 },
        { H_MixFinished, "ACT_MixFinished(-1)", 1, -1, false, true, 1 },
        { H_MixFinished, "ACT_MixFinished(5)", 1, 5, true, false, 0 },
        { H_MixOff, "ACT_MixOff(-1)", 1, -1, false, false, 0 },
        { H_MixOff, "ACT_MixOff(1)", 1, 1, true, false, 0 },
    };
    const uint32_t objPrefix = 0x11111111, objDefault = 0x22222222;
    for (size_t k = 0; k < sizeof(mixes) / sizeof(mixes[0]); ++k) {
        const MixCase& mc = mixes[k];
        memset(data, 0, sizeof(data));
        idx = (uint32_t)mc.args;
        off = 4 * (uint32_t)mc.args;
        int32_t args[2] = { mc.layer, 0x3F800000 };
        for (int a = 0; a < mc.args; ++a) {
            memcpy(data + 4 * a, &args[a], 4);
            ptrs[a] = dataBase + 4 * a;
        }
        memcpy(&scr[CTX_OBJECT], &objPrefix, 4);
        memcpy(&scr[CTX_DEFAULT], &objDefault, 4);
        FakeEngine::Calls() = 0;
        void* mr = mc.fn(ip);
        uint32_t obj;
        memcpy(&obj, &scr[CTX_OBJECT], 4);
        bool mok;
        if (mc.engine) {
            mok = mr == (void*)((uint8_t*)ip + 4) && FakeEngine::Calls() == 1 && idx == (uint32_t)mc.args &&
                  obj == objPrefix;
        } else {
            uint32_t wantIdx = mc.pushes ? 1 : 0;
            int32_t got = mc.pushes && idx == 1 ? *(int32_t*)(uintptr_t)ptrs[0] : 0;
            mok = mr == (void*)((uint8_t*)ip + 4) && FakeEngine::Calls() == 0 && idx == wantIdx &&
                  off == 4 * wantIdx && got == mc.result && obj == objDefault;
        }
        Report(rep, mok, "%s: %s (engine calls %d, entries %u, bytes %u, object %08X)", mc.name,
               mc.engine ? "passed to the engine" : mc.pushes ? "skipped, answers 1" : "skipped",
               FakeEngine::Calls(), idx, off, obj);
        if (!mok) ++fails;
    }
    {
        idx = 5;
        off = 20;
        const int32_t addArgs[5] = { 42, 0, 0, 0, 0 };
        for (int a = 0; a < 5; ++a) {
            memcpy(data + 4 * a, &addArgs[a], 4);
            ptrs[a] = dataBase + 4 * a;
        }
        FakeEngine::Calls() = 0;
        size_t before = log.size();
        void* ar = H_MixAdd(ip);
        int32_t got = idx == 1 ? *(int32_t*)(uintptr_t)ptrs[0] : 0;
        bool logged = log.find("ACT_MixAdd(action 42", before) != std::string::npos;
        bool aok = ar == (void*)((uint8_t*)ip + 4) && FakeEngine::Calls() == 1 && idx == 1 && got == -1 && logged;
        Report(rep, aok, "ACT_MixAdd(42, ...) -> -1 from the engine: passed on and logged with its action (entries %u, "
               "value %d, logged %d)", idx, got, logged);
        if (!aok) ++fails;
    }
    memcpy(s_engine, savedEngine, sizeof(savedEngine));
    s_mixLogs = 0;

    rep += log;
    s_vm = savedVm;
    s_testLog = savedLog;
    s_platform = savedPlatform;
    s_verified = savedVerified;
    s_pointerFeature = savedPointer;
    s_testNoQuit = savedNoQuit;
    return fails;
}

}  // namespace

bool ScriptPointerOn(int id) {
    if (!s_pointerFeature || !s_verified || id < 0 || id >= 4) return true;
    return s_pointerOn[id] != 0;
}

void ScriptAttach(const std::string& iniPath) {
    s_inAttach = true;
    std::string v;
    if (ReadIniKey(iniPath, "script", "enabled", v)) s_enabled = IsOn(v);
    int platform = -1;                          // auto
    if (ReadIniKey(iniPath, "script", "platform", v)) {
        if (_stricmp(v.c_str(), "wii") == 0 || v == "1") platform = PLATFORM_WII;
        else if (_stricmp(v.c_str(), "pc") == 0 || v == "0") platform = PLATFORM_PC;
    }
    s_platform = platform >= 0 ? platform : (CtlPcMode() ? PLATFORM_PC : PLATFORM_WII);
    if (ReadIniKey(iniPath, "script", "pointer_state", v)) s_pointerFeature = IsOn(v);
    if (ReadIniKey(iniPath, "script", "return_to_menu", v)) s_returnQuit = _stricmp(v.c_str(), "quit") == 0;
    if (ReadIniKey(iniPath, "script", "mix_guard", v)) s_mixGuard = IsOn(v);
    if (!s_enabled) {
        ScrLog("[script] enabled=0: ViD_PlatformCurrentGet, IO_JoystickPointerStateSet, WII_ReturnToMenu, the IO_File "
               "natives and the ACT mix layer natives are left to the engine");
        return;
    }
    ProcessImage img;
    uint8_t probe[4];
    if (!img.Read(kWords[0].oldFn, probe, sizeof(probe))) {
        ScrLog("host process is not the RGH PC executable (no code at %08X): script natives not installed",
               kWords[0].oldFn);
        return;
    }
    std::string rep;
    int bad = ScrVerify(img, rep, false);
    if (bad) {
        ScrLog("the PC executable differs from the expected 2010 build in %d place(s): script natives NOT installed", bad);
        size_t pos = 0;
        while (pos < rep.size()) {
            size_t end = rep.find('\n', pos);
            std::string ln = rep.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
            if (ln.find("FAIL") != std::string::npos) ScrLog("%s", ln.c_str());
            pos = end == std::string::npos ? rep.size() : end + 1;
        }
        return;
    }
    int reg = 0;
    for (size_t i = 0; i < N_WORDS; ++i) {       // in case ViD::RegisterScript runs (again) later
        if (!Wanted(kWords[i])) continue;
        uint32_t fn = (uint32_t)(uintptr_t)kWords[i].fn;
        if (WriteCode(kWords[i].reg, &fn, 4)) ++reg;
    }
    s_verified = true;
    int done = InstallWords();
    ScrLog("PC executable verified: platform %d (%s%s), pointer_state=%d, return_to_menu=%s, mix_guard=%d; %d/%d words "
           "in the native table, %d registration entries", s_platform, s_platform == PLATFORM_WII ? "Wii" : "PC",
           platform < 0 ? ", auto" : "", s_pointerFeature ? 1 : 0, s_returnQuit ? "quit" : "none", s_mixGuard ? 1 : 0,
           done, CountWanted(), reg);
}

void ScriptAfterConfig() {
    s_inAttach = false;
    for (size_t i = 0; i < s_pending.size(); ++i) Log("SCRIPT: %s", s_pending[i].c_str());
    s_pending.clear();
    if (s_verified) {
        int before = 0;
        for (size_t i = 0; i < N_WORDS; ++i)
            if (Wanted(kWords[i]) && s_wordDone[i]) ++before;
        if (before < CountWanted()) {
            int done = InstallWords();
            Log("SCRIPT: %d/%d words in the native table after start-up", done, CountWanted());
        }
    }
}

extern "C" {

// wmtest only: the patch sites in the PC executable file. Returns the number of differences (-1 unreadable).
int __cdecl WiimoteScriptCheckExe(const char* exePath, char* out, int outSize) {
    FileImage img;
    std::string rep;
    int bad = -1;
    if (!img.Load(exePath)) rep = "  FAIL  cannot read " + std::string(exePath ? exePath : "(null)") + "\n";
    else bad = ScrVerify(img, rep, true);
    CopyOut(rep, out, outSize);
    return bad;
}

// wmtest only: the handlers on a fake script stack. Returns the number of failures.
int __cdecl WiimoteScriptSelfTest(char* out, int outSize) {
    std::string rep;
    int fails = RunSelfTest(rep);
    CopyOut(rep, out, outSize);
    return fails;
}

}  // extern "C"
