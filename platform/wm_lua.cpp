// wm_lua.cpp - Lua 5.4 scripts of the enabled mods ([mods] lua=1): mods\<mod>\main.lua runs once at start (on the
// game's main thread, at the first frame the DLL sees), with the table `wc` as its way into the game.  Every mod has
// its own Lua state; errors go to wiimote_log.txt and never stop the game.
//
//   wc.log(...)                      a line in wiimote_log.txt ("LUA <mod>: ...");  print() does the same
//   wc.on("frame", fn)               fn(dt) every frame (dt seconds);  wc.on("shutdown", fn) not yet
//   wc.every(seconds, fn)            fn() at that period (from the frame callback)
//   wc.time()                        seconds since the DLL started
//   wc.key(vk)  wc.pressed(vk)       the keyboard while the game has the focus: held / went down this frame;
//                                    wc.vk.SPACE, wc.vk.F5, wc.vk.A ... hold the virtual-key codes
//   wc.text(id, x, y, str [, argb])  a line of text drawn over the picture until wc.text(id) clears it (x, y in
//                                    pixels of the picture, see wc.screen(); id any string)
//   wc.screen()                      the picture's width and height in pixels
//   wc.screenshot(path)              saves the picture (with the texts) as .png / .jpg / .bmp at the next frame
//   wc.mem.u8/u16/u32/i32/f32(addr)  read the process memory (nil when the address is not readable)
//   wc.mem.write_u8/.../f32(addr, v) write it (false when not writable);  wc.mem.patch(addr, bytes) writes code
//   wc.mem.string(addr [, max])      a C string
//   wc.game.fps_cap([hz])            the frame rate cap (get / set);  wc.game.afx([on]);  wc.game.high_detail(on)
//   wc.natives[name]                 the word of an engine script native (2168 names from the Wii executable's tables)
//   wc.native(name, fn)              fn(vm) runs in place of the native each time a script calls it.  vm:arg_int(i) /
//                                    vm:arg_float(i) look at argument i (0 = first) without taking it;
//                                    vm:set_arg_int(i, v) rewrites it in place (before vm:original()); vm:pop_int()
//                                    ... take the arguments (last first, as the engine does); vm:push_int(v) /
//                                    vm:push_float(v) / vm:push_string(s) give the result; vm:original() runs the
//                                    engine's own handler (with the arguments still on the stack) and returns
//   wc.call(name, ...)               calls an engine script native from Lua (4-byte arguments: numbers, or strings
//                                    for pointer arguments); returns the 4-byte result as an integer, nil for none.
//                                    wc.callf(name, ...) returns it as a float
//   wc.mod                           this mod's name;  wc.dir  its folder;  require() searches that folder
//
// Natives: SCR::ApplyCommon (00516030) calls the handler of the TOOsarray element whose rank the bytecode holds,
// `void* handler(void* ip)` returning ip + 4 (wm_sav.cpp §12.3): the hook is one handler for every hooked word,
// which finds the word from the rank at ip.  A call from Lua pushes the arguments on the VM stack and calls the
// handler with a two-word node {rank, 0}.
#include "wiimote.h"

#include <d3d9.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <map>

extern "C" {
#include "lua/lua.h"
#include "lua/lualib.h"
#include "lua/lauxlib.h"
}

using namespace wmpatch;

namespace {

const uint32_t VM_INDEX  = 0x00A7E188;   // entry index
const uint32_t VM_OFFSET = 0x00A7E18C;   // data offset
const uint32_t VM_DATA   = 0x00A7E198;   // cell: data buffer base
const uint32_t VM_PTRS   = 0x00A7E19C;   // cell: entry pointer array
const uint32_t NATIVES   = 0x00A718AC;   // TOOsarray {data, element size, capacity, count}

struct Code { uint32_t va, len, crc; const char* what; };
const Code kCode[] = {
    { 0x00516030, 0x030, 0xC48AACD7, "SCR::ApplyCommon (the native call)" },
};

struct Timer { double period, next; int ref; };

struct Script {
    std::string mod, dir;
    lua_State* L = NULL;
    int frameRef = LUA_NOREF;
    int drawRef = LUA_NOREF;                  // wc.on("draw"): the mod puts its own picture up
    std::vector<Timer> timers;
    int errors = 0;
    bool dead = false;
    uint8_t keys[256];                       // last frame's keyboard, for wc.pressed
};

struct Hook { uint32_t word; Script* script; int ref; void* (*orig)(void*); };
struct Text { std::string s; int x, y; uint32_t color; };

std::vector<std::string> s_pending;
bool s_inAttach, s_enabled = true, s_log = true, s_started, s_vmOk, s_applyOk;
std::vector<Script*> s_scripts;
std::map<uint32_t, Hook> s_hooks;              // by word
std::map<std::string, Text> s_texts;
CRITICAL_SECTION s_cs;
bool s_csInit;
double s_lastFrame;
Script* s_current;                             // the script whose code runs (for wc.native handlers)
void* s_fontD3dx;                              // ID3DXFont
bool s_fontFailed;
std::string s_shotPath;                        // a screenshot to save at the next draw (wc.screenshot)
uint8_t s_keysNow[256];

void LuaLog(const char* fmt, ...) {
    char buf[900];
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf(buf, sizeof(buf) - 1, fmt, ap);
    buf[sizeof(buf) - 1] = 0;
    va_end(ap);
    if (s_inAttach) s_pending.push_back(buf);
    else if (s_log) Log("LUA: %s", buf);
}

struct Lock {
    Lock() { EnterCriticalSection(&s_cs); }
    ~Lock() { LeaveCriticalSection(&s_cs); }
};

// ---------------------------------------------------------------------------------------------- memory access
bool Readable(uint32_t va, uint32_t n) {
    MEMORY_BASIC_INFORMATION mbi;
    if (!VirtualQuery((LPCVOID)(uintptr_t)va, &mbi, sizeof(mbi))) return false;
    if (mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD))) return false;
    uintptr_t end = (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
    return (uintptr_t)va + n <= end;
}

bool Writable(uint32_t va, uint32_t n) {
    MEMORY_BASIC_INFORMATION mbi;
    if (!VirtualQuery((LPCVOID)(uintptr_t)va, &mbi, sizeof(mbi))) return false;
    if (mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD))) return false;
    if (!(mbi.Protect & (PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY))) return false;
    uintptr_t end = (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
    return (uintptr_t)va + n <= end;
}

// ---------------------------------------------------------------------------------------------- the script VM
inline uint32_t* VmPtrs() { return (uint32_t*)(uintptr_t)*(uint32_t*)(uintptr_t)VM_PTRS; }
inline uint32_t& VmIndex() { return *(uint32_t*)(uintptr_t)VM_INDEX; }
inline uint32_t& VmOffset() { return *(uint32_t*)(uintptr_t)VM_OFFSET; }
inline uint8_t* VmData() { return (uint8_t*)(uintptr_t)*(uint32_t*)(uintptr_t)VM_DATA; }

uint32_t VmPop() {
    uint32_t idx = VmIndex() - 1;
    VmIndex() = idx;
    VmOffset() -= 4;
    return *(uint32_t*)(uintptr_t)VmPtrs()[idx];
}

uint32_t VmPeek(int i) {                       // the i-th entry from the top (0 = the last pushed)
    uint32_t idx = VmIndex();
    if ((uint32_t)i >= idx) return 0;
    return *(uint32_t*)(uintptr_t)VmPtrs()[idx - 1 - i];
}

void VmPushZero(uint32_t size) {               // a zero result of `size` bytes (a structure)
    uint8_t* data = VmData();
    uint32_t off = VmOffset();
    memset(data + off, 0, size);
    VmPtrs()[VmIndex()] = (uint32_t)(uintptr_t)(data + off);
    VmIndex() += 1;
    VmOffset() += size;
}

void VmPush(uint32_t v) {
    uint8_t* data = VmData();
    uint32_t off = VmOffset();
    memcpy(data + off, &v, 4);
    VmPtrs()[VmIndex()] = (uint32_t)(uintptr_t)(data + off);
    VmIndex() += 1;
    VmOffset() += 4;
}
void VmPushBytes(const void* p, uint32_t size) {   // one entry of `size` bytes (a vector, a structure)
    uint8_t* data = VmData();
    uint32_t off = VmOffset();
    memcpy(data + off, p, size);
    VmPtrs()[VmIndex()] = (uint32_t)(uintptr_t)(data + off);
    VmIndex() += 1;
    VmOffset() += size;
}

uint32_t WordOfIp(void* ip) {
    uint32_t* n = (uint32_t*)(uintptr_t)NATIVES;
    if (!ip || !n[0] || n[1] < 8) return 0;
    uint32_t rank = *(uint32_t*)ip;
    if (rank >= n[3]) return 0;
    return *(uint32_t*)(uintptr_t)(n[0] + rank * n[1]);
}

// the rank and handler of a word in the executable's table
bool VmReady() {                               // the native table exists (built by ViD::b_Create, before any script)
    uint32_t* n = (uint32_t*)(uintptr_t)NATIVES;
    s_vmOk = n[0] && n[1] >= 8 && n[1] <= 64 && n[3] > 100 && n[3] < 10000;
    static bool logged = false;
    if (!s_vmOk && !logged) {
        logged = true;
        LuaLog("native table at %08X reads {%08X, %u, %u, %u}: not usable", NATIVES, n[0], n[1], n[2], n[3]);
    }
    return s_vmOk;
}

bool NativeEntry(uint32_t word, uint32_t* rank, void** handler) {
    uint32_t* n = (uint32_t*)(uintptr_t)NATIVES;
    if (!VmReady()) return false;
    for (uint32_t i = 0; i < n[3]; ++i) {
        uint32_t* e = (uint32_t*)(uintptr_t)(n[0] + i * n[1]);
        if (e[0] == word) {
            *rank = i;
            *handler = (void*)(uintptr_t)e[1];
            return true;
        }
    }
    return false;
}

std::vector<wmnatives::Native> s_extraNatives;   // natives the platform layer adds to the engine (wm_sm64)

const wmnatives::Native* FindNative(const char* name) {
    for (int i = 0; i < wmnatives::kCount; ++i)
        if (strcmp(wmnatives::kTable[i].name, name) == 0) return &wmnatives::kTable[i];
    for (size_t i = 0; i < s_extraNatives.size(); ++i)
        if (strcmp(s_extraNatives[i].name, name) == 0) return &s_extraNatives[i];
    return NULL;
}

const wmnatives::Native* FindNativeWord(uint32_t word) {
    for (int i = 0; i < wmnatives::kCount; ++i)
        if (wmnatives::kTable[i].word == word) return &wmnatives::kTable[i];
    for (size_t i = 0; i < s_extraNatives.size(); ++i)
        if (s_extraNatives[i].word == word) return &s_extraNatives[i];
    return NULL;
}

// ---------------------------------------------------------------------------------------------- Lua helpers
Script* ScriptOf(lua_State* L) {
    lua_getfield(L, LUA_REGISTRYINDEX, "wc_script");
    Script* s = (Script*)lua_touserdata(L, -1);
    lua_pop(L, 1);
    return s;
}

void ReportError(Script* s, const char* what, lua_State* L) {
    const char* msg = lua_tostring(L, -1);
    if (s->errors < 20 || (s->errors % 100) == 0)
        LuaLog("%s: %s: %s", s->mod.c_str(), what, msg ? msg : "(no message)");
    ++s->errors;
    lua_pop(L, 1);
}

int l_log(lua_State* L) {
    Script* s = ScriptOf(L);
    std::string line;
    int n = lua_gettop(L);
    for (int i = 1; i <= n; ++i) {
        size_t len;
        const char* str = luaL_tolstring(L, i, &len);
        if (i > 1) line += " ";
        line.append(str, len);
        lua_pop(L, 1);
    }
    if (s_log) Log("LUA %s: %s", s ? s->mod.c_str() : "?", line.c_str());
    return 0;
}

int l_on(lua_State* L) {
    Script* s = ScriptOf(L);
    const char* ev = luaL_checkstring(L, 1);
    luaL_checktype(L, 2, LUA_TFUNCTION);
    if (strcmp(ev, "frame") == 0) {
        if (s->frameRef != LUA_NOREF) luaL_unref(L, LUA_REGISTRYINDEX, s->frameRef);
        lua_pushvalue(L, 2);
        s->frameRef = luaL_ref(L, LUA_REGISTRYINDEX);
        return 0;
    }
    if (strcmp(ev, "draw") == 0) {
        if (s->drawRef != LUA_NOREF) luaL_unref(L, LUA_REGISTRYINDEX, s->drawRef);
        lua_pushvalue(L, 2);
        s->drawRef = luaL_ref(L, LUA_REGISTRYINDEX);
        return 0;
    }
    return luaL_error(L, "wc.on: unknown event '%s' (frame, draw)", ev);
}

int l_every(lua_State* L) {
    Script* s = ScriptOf(L);
    double period = luaL_checknumber(L, 1);
    luaL_checktype(L, 2, LUA_TFUNCTION);
    if (period < 0.001) period = 0.001;
    Timer t;
    t.period = period;
    t.next = NowSeconds() + period;
    lua_pushvalue(L, 2);
    t.ref = luaL_ref(L, LUA_REGISTRYINDEX);
    s->timers.push_back(t);
    return 0;
}

int l_time(lua_State* L) {
    lua_pushnumber(L, NowSeconds());
    return 1;
}

int l_screenshot(lua_State* L) {               // the presented picture, with the overlay, saved at the next frame
    const char* path = luaL_checkstring(L, 1);
    Lock lock;
    s_shotPath = path;
    return 0;
}

int l_screen(lua_State* L) {                   // the picture's size in pixels (what wc.text coordinates refer to)
    RECT r = GfxPicture();
    lua_pushinteger(L, r.right - r.left);
    lua_pushinteger(L, r.bottom - r.top);
    return 2;
}

bool GameHasFocus() {
    HWND w = GetForegroundWindow();
    if (!w) return false;
    DWORD pid = 0;
    GetWindowThreadProcessId(w, &pid);
    return pid == GetCurrentProcessId();
}

int l_key(lua_State* L) {
    int vk = (int)luaL_checkinteger(L, 1);
    lua_pushboolean(L, vk > 0 && vk < 256 && s_keysNow[vk]);
    return 1;
}

int l_pressed(lua_State* L) {
    Script* s = ScriptOf(L);
    int vk = (int)luaL_checkinteger(L, 1);
    lua_pushboolean(L, vk > 0 && vk < 256 && s_keysNow[vk] && !s->keys[vk]);
    return 1;
}

int l_text(lua_State* L) {
    Script* s = ScriptOf(L);
    const char* id = luaL_checkstring(L, 1);
    std::string key = s->mod + "/" + id;
    Lock lock;
    if (lua_gettop(L) < 4 || lua_isnil(L, 4)) {
        s_texts.erase(key);
        return 0;
    }
    Text t;
    t.x = (int)luaL_checkinteger(L, 2);
    t.y = (int)luaL_checkinteger(L, 3);
    t.s = luaL_checkstring(L, 4);
    t.color = lua_gettop(L) >= 5 ? (uint32_t)luaL_checkinteger(L, 5) : 0xFFFFFFFFu;
    s_texts[key] = t;
    return 0;
}

// ---------------------------------------------------------------------------------------------------------------
// wc.draw - what a mod uses to put its own picture up, inside its wc.on("draw") handler.  Everything is in the
// pixels of the presented picture; wc.draw.picture() says where that is and how big, and a mod that wants its own
// grid (a 640x480 one, say) does that arithmetic itself.
// ---------------------------------------------------------------------------------------------------------------
int l_picture(lua_State* L) {
    RECT r = MediaPicture();
    lua_pushinteger(L, r.left);
    lua_pushinteger(L, r.top);
    lua_pushinteger(L, r.right - r.left);
    lua_pushinteger(L, r.bottom - r.top);
    return 4;
}

int l_rect(lua_State* L) {
    if (!MediaDrawing()) return luaL_error(L, "wc.draw.rect: only inside a draw handler");
    MediaRect((float)luaL_checknumber(L, 1), (float)luaL_checknumber(L, 2), (float)luaL_checknumber(L, 3),
              (float)luaL_checknumber(L, 4), (uint32_t)luaL_optinteger(L, 5, 0xFFFFFFFF));
    return 0;
}

// wc.draw.image(bytes | path) -> id.  A string that names a file that exists is read; anything else is taken as
// the picture itself, so a mod can pull one out of an archive of its own and hand the bytes straight over.
int l_image(lua_State* L) {
    size_t n = 0;
    const char* data = luaL_checklstring(L, 1, &n);
    std::vector<uint8_t> file;
    if (n < MAX_PATH) {
        FILE* f = fopen(data, "rb");
        if (f) {
            fseek(f, 0, SEEK_END);
            long len = ftell(f);
            fseek(f, 0, SEEK_SET);
            file.resize((size_t)(len > 0 ? len : 0));
            if (!file.empty()) fread(&file[0], 1, file.size(), f);
            fclose(f);
        }
    }
    int id = file.empty() ? MediaImage(data, n) : MediaImage(&file[0], file.size());
    if (!id) {
        lua_pushnil(L);
        return 1;
    }
    int w = 0, h = 0;
    MediaImageSize(id, w, h);
    lua_pushinteger(L, id);
    lua_pushinteger(L, w);
    lua_pushinteger(L, h);
    return 3;
}

// wc.draw.image_at(id, x, y, w, h [, u0, v0, u1, v1] [, tint])
int l_image_at(lua_State* L) {
    if (!MediaDrawing()) return luaL_error(L, "wc.draw.image_at: only inside a draw handler");
    int id = (int)luaL_checkinteger(L, 1);
    float x = (float)luaL_checknumber(L, 2), y = (float)luaL_checknumber(L, 3);
    float w = (float)luaL_checknumber(L, 4), h = (float)luaL_checknumber(L, 5);
    int iw = 0, ih = 0;
    if (!MediaImageSize(id, iw, ih)) return 0;
    float u0 = (float)luaL_optnumber(L, 6, 0.0), v0 = (float)luaL_optnumber(L, 7, 0.0);
    float u1 = (float)luaL_optnumber(L, 8, iw), v1 = (float)luaL_optnumber(L, 9, ih);
    MediaDrawImage(id, x, y, w, h, u0, v0, u1, v1, (uint32_t)luaL_optinteger(L, 10, 0xFFFFFFFF));
    return 0;
}

int l_image_free(lua_State* L) {
    MediaImageFree((int)luaL_checkinteger(L, 1));
    return 0;
}

// ---------------------------------------------------------------------------------------------------------------
// wc.sound - a wav or an ogg, from a file or from bytes a mod has in hand; and one piece of music, streamed
// ---------------------------------------------------------------------------------------------------------------
int l_sound(lua_State* L) {
    size_t n = 0;
    const char* data = luaL_checklstring(L, 1, &n);
    int volume = (int)luaL_optinteger(L, 2, 100);
    std::vector<uint8_t> file;
    if (n < MAX_PATH) {
        FILE* f = fopen(data, "rb");
        if (f) {
            fseek(f, 0, SEEK_END);
            long len = ftell(f);
            fseek(f, 0, SEEK_SET);
            file.resize((size_t)(len > 0 ? len : 0));
            if (!file.empty()) fread(&file[0], 1, file.size(), f);
            fclose(f);
        }
    }
    bool ok = file.empty() ? MediaSound(data, n, volume) : MediaSound(&file[0], file.size(), volume);
    lua_pushboolean(L, ok);
    return 1;
}

int l_music(lua_State* L) {
    if (lua_isnoneornil(L, 1)) {
        MediaMusicStop();
        return 0;
    }
    const char* path = luaL_checkstring(L, 1);
    bool ok = MediaMusic(path, (int)luaL_optinteger(L, 2, 100), lua_isnoneornil(L, 3) ? true : lua_toboolean(L, 3) != 0);
    lua_pushboolean(L, ok);
    return 1;
}

// ---------------------------------------------------------------------------------------------------------------
// wc.game - letting a mod take the screen: the world's clock, the game's controller, the game's own noise
// ---------------------------------------------------------------------------------------------------------------
int l_freeze(lua_State* L) {
    MediaFreeze(lua_toboolean(L, 1) != 0);
    return 0;
}

int l_block_input(lua_State* L) {
    MediaBlockInput(lua_toboolean(L, 1) != 0);
    return 0;
}

int l_mute(lua_State* L) {
    MediaMuteGame(lua_toboolean(L, 1) != 0);
    return 0;
}

// wc.mem
template <typename T> int l_read(lua_State* L) {
    uint32_t va = (uint32_t)luaL_checkinteger(L, 1);
    if (!Readable(va, sizeof(T))) { lua_pushnil(L); return 1; }
    T v;
    memcpy(&v, (const void*)(uintptr_t)va, sizeof(T));
    lua_pushinteger(L, (lua_Integer)v);
    return 1;
}

int l_read_f32(lua_State* L) {
    uint32_t va = (uint32_t)luaL_checkinteger(L, 1);
    if (!Readable(va, 4)) { lua_pushnil(L); return 1; }
    float v;
    memcpy(&v, (const void*)(uintptr_t)va, 4);
    lua_pushnumber(L, v);
    return 1;
}

template <typename T> int l_write(lua_State* L) {
    uint32_t va = (uint32_t)luaL_checkinteger(L, 1);
    T v = (T)luaL_checkinteger(L, 2);
    if (!Writable(va, sizeof(T))) { lua_pushboolean(L, 0); return 1; }
    memcpy((void*)(uintptr_t)va, &v, sizeof(T));
    lua_pushboolean(L, 1);
    return 1;
}

int l_write_f32(lua_State* L) {
    uint32_t va = (uint32_t)luaL_checkinteger(L, 1);
    float v = (float)luaL_checknumber(L, 2);
    if (!Writable(va, 4)) { lua_pushboolean(L, 0); return 1; }
    memcpy((void*)(uintptr_t)va, &v, 4);
    lua_pushboolean(L, 1);
    return 1;
}

int l_patch(lua_State* L) {
    uint32_t va = (uint32_t)luaL_checkinteger(L, 1);
    size_t n;
    const char* b = luaL_checklstring(L, 2, &n);
    if (!n || !Readable(va, (uint32_t)n)) { lua_pushboolean(L, 0); return 1; }
    lua_pushboolean(L, WriteCode(va, b, (uint32_t)n));
    return 1;
}

int l_string(lua_State* L) {
    uint32_t va = (uint32_t)luaL_checkinteger(L, 1);
    int max = (int)luaL_optinteger(L, 2, 4096);
    std::string out;
    for (int i = 0; i < max; ++i) {
        if (!Readable(va + i, 1)) { if (i == 0) { lua_pushnil(L); return 1; } break; }
        char c = *(const char*)(uintptr_t)(va + i);
        if (!c) break;
        out += c;
    }
    lua_pushlstring(L, out.data(), out.size());
    return 1;
}

// wc.game
int l_fps_cap(lua_State* L) {
    if (lua_gettop(L) >= 1 && !lua_isnil(L, 1)) TimingSetCap(luaL_checknumber(L, 1));
    lua_pushnumber(L, TimingCap());
    return 1;
}

int l_afx(lua_State* L) {
    if (lua_gettop(L) >= 1 && !lua_isnil(L, 1)) AfxSetUserEnabled(lua_toboolean(L, 1) != 0);
    lua_pushboolean(L, AfxUserEnabled());
    return 1;
}

int l_high_detail(lua_State* L) {
    GfxSetHighDetail(lua_toboolean(L, 1) != 0);
    return 0;
}

// the vm object of a native hook
struct VmCall {
    void* ip;
    const Hook* hook;
    const wmnatives::Native* native;
    std::vector<std::string> strings;
    bool originalRan;
};

VmCall* CheckVm(lua_State* L) { return *(VmCall**)luaL_checkudata(L, 1, "wc.vm"); }

int vm_arg_int(lua_State* L) {
    VmCall* c = CheckVm(L);
    int i = (int)luaL_checkinteger(L, 2);
    int n = c->native ? c->native->nargs : 0;
    if (!c->native || i < 0 || i >= n) return luaL_error(L, "arg %d: the native has %d argument(s)", i, n);
    lua_pushinteger(L, (int32_t)VmPeek(n - 1 - i));
    return 1;
}

int vm_arg_float(lua_State* L) {
    VmCall* c = CheckVm(L);
    int i = (int)luaL_checkinteger(L, 2);
    int n = c->native ? c->native->nargs : 0;
    if (!c->native || i < 0 || i >= n) return luaL_error(L, "arg %d: the native has %d argument(s)", i, n);
    uint32_t v = VmPeek(n - 1 - i);
    float f;
    memcpy(&f, &v, 4);
    lua_pushnumber(L, f);
    return 1;
}

int vm_arg_string(lua_State* L) {
    VmCall* c = CheckVm(L);
    int i = (int)luaL_checkinteger(L, 2);
    int n = c->native ? c->native->nargs : 0;
    if (!c->native || i < 0 || i >= n) return luaL_error(L, "arg %d: the native has %d argument(s)", i, n);
    uint32_t p = VmPeek(n - 1 - i);
    if (!Readable(p, 1)) { lua_pushnil(L); return 1; }
    lua_pushstring(L, (const char*)(uintptr_t)p);
    return 1;
}

int vm_set_arg_int(lua_State* L) {             // rewrite argument i in place (before vm:original())
    VmCall* c = CheckVm(L);
    int i = (int)luaL_checkinteger(L, 2);
    int n = c->native ? c->native->nargs : 0;
    if (!c->native || i < 0 || i >= n) return luaL_error(L, "arg %d: the native has %d argument(s)", i, n);
    uint32_t idx = VmIndex();
    if ((uint32_t)(n - 1 - i) >= idx) return 0;
    *(uint32_t*)(uintptr_t)VmPtrs()[idx - 1 - (n - 1 - i)] = (uint32_t)(int32_t)luaL_checkinteger(L, 3);
    return 0;
}

int vm_pop_int(lua_State* L) { CheckVm(L); lua_pushinteger(L, (int32_t)VmPop()); return 1; }
int vm_pop_float(lua_State* L) { CheckVm(L); uint32_t v = VmPop(); float f; memcpy(&f, &v, 4); lua_pushnumber(L, f); return 1; }
int vm_push_int(lua_State* L) { CheckVm(L); VmPush((uint32_t)(int32_t)luaL_checkinteger(L, 2)); return 0; }
int vm_push_float(lua_State* L) { CheckVm(L); float f = (float)luaL_checknumber(L, 2); uint32_t v; memcpy(&v, &f, 4); VmPush(v); return 0; }
int vm_push_string(lua_State* L) {
    VmCall* c = CheckVm(L);
    c->strings.push_back(luaL_checkstring(L, 2));
    VmPush((uint32_t)(uintptr_t)c->strings.back().c_str());
    return 0;
}
int vm_original(lua_State* L) {
    VmCall* c = CheckVm(L);
    if (c->hook && c->hook->orig && !c->originalRan) {
        c->originalRan = true;
        c->hook->orig(c->ip);
    }
    return 0;
}
int vm_word(lua_State* L) { VmCall* c = CheckVm(L); lua_pushinteger(L, c->hook ? c->hook->word : 0); return 1; }
int vm_name(lua_State* L) { VmCall* c = CheckVm(L); lua_pushstring(L, c->native ? c->native->name : ""); return 1; }

const luaL_Reg kVmMethods[] = {
    { "arg_int", vm_arg_int }, { "arg_float", vm_arg_float }, { "arg_string", vm_arg_string }, { "set_arg_int", vm_set_arg_int },
    { "pop_int", vm_pop_int }, { "pop_float", vm_pop_float }, { "push_int", vm_push_int },
    { "push_float", vm_push_float }, { "push_string", vm_push_string }, { "original", vm_original },
    { "word", vm_word }, { "name", vm_name }, { NULL, NULL }
};

void* __cdecl NativeHook(void* ip) {
    uint32_t word = WordOfIp(ip);
    std::map<uint32_t, Hook>::iterator it = s_hooks.find(word);
    if (it == s_hooks.end()) return (uint8_t*)ip + 4;          // not ours: nothing to run (should not happen)
    Hook& h = it->second;
    Script* s = h.script;
    if (s->dead) return h.orig ? h.orig(ip) : (uint8_t*)ip + 4;
    lua_State* L = s->L;
    VmCall call;
    call.ip = ip;
    call.hook = &h;
    call.native = FindNativeWord(word);
    call.originalRan = false;
    // what the stack must look like afterwards: the arguments taken, the result (if any) left
    uint32_t idx0 = VmIndex(), off0 = VmOffset();
    uint32_t argBytes = 0;
    int nargs = call.native ? call.native->nargs : 0;
    for (int i = 0; i < nargs; ++i) argBytes += call.native->argsizes[i];
    uint32_t ret = call.native ? call.native->retsize : 0;
    Script* was = s_current;
    s_current = s;
    lua_rawgeti(L, LUA_REGISTRYINDEX, h.ref);
    VmCall** ud = (VmCall**)lua_newuserdatauv(L, sizeof(VmCall*), 0);
    *ud = &call;
    luaL_setmetatable(L, "wc.vm");
    if (lua_pcall(L, 1, 0, 0) != LUA_OK) ReportError(s, (std::string("native ") + (call.native ? call.native->name : "?")).c_str(), L);
    *ud = NULL;
    s_current = was;
    if (!call.originalRan && call.native) {
        uint32_t wantIdx = idx0 - nargs + (ret ? 1 : 0), wantOff = off0 - argBytes + ret;
        if (VmIndex() == idx0 && VmOffset() == off0) {          // the script looked only: the call is a no-op
            for (int i = 0; i < nargs; ++i) VmPop();
            if (ret == 4) VmPush(0);
            else if (ret) VmPushZero(ret);
        } else if (VmIndex() != wantIdx || VmOffset() != wantOff) {
            if (s->errors++ < 20)
                LuaLog("%s: native %s: the stack is off after the hook (index %u, want %u): reset", s->mod.c_str(),
                       call.native->name, VmIndex(), wantIdx);
            VmIndex() = wantIdx;
            VmOffset() = wantOff;
        }
    }
    return (uint8_t*)ip + 4;
}

int l_native(lua_State* L) {
    Script* s = ScriptOf(L);
    const wmnatives::Native* n = NULL;
    uint32_t word = 0;
    if (lua_type(L, 1) == LUA_TNUMBER) word = (uint32_t)lua_tointeger(L, 1);
    else {
        n = FindNative(luaL_checkstring(L, 1));
        if (!n) return luaL_error(L, "wc.native: unknown native '%s'", lua_tostring(L, 1));
        word = n->word;
    }
    luaL_checktype(L, 2, LUA_TFUNCTION);
    if (!s_applyOk) return luaL_error(L, "wc.native: the native table is not the expected one");
    uint32_t rank;
    void* handler;
    if (!NativeEntry(word, &rank, &handler)) {
        char hex[16];
        _snprintf(hex, sizeof(hex), "%08X", word);
        return luaL_error(L, "wc.native: word %s is not in the executable's table", hex);
    }
    std::map<uint32_t, Hook>::iterator it = s_hooks.find(word);
    if (it != s_hooks.end()) {
        luaL_unref(it->second.script->L, LUA_REGISTRYINDEX, it->second.ref);
        it->second.script = s;
        lua_pushvalue(L, 2);
        it->second.ref = luaL_ref(L, LUA_REGISTRYINDEX);
        return 0;
    }
    Hook h;
    h.word = word;
    h.script = s;
    lua_pushvalue(L, 2);
    h.ref = luaL_ref(L, LUA_REGISTRYINDEX);
    h.orig = (void* (*)(void*))handler;
    if (handler == (void*)&NativeHook) h.orig = NULL;
    s_hooks[word] = h;
    std::string notes;
    int r = PatchNativeTable(word, 0xFFFFFFFF, (uint32_t)(uintptr_t)handler, (uint32_t)(uintptr_t)&NativeHook, notes);
    if (r != 1) {
        s_hooks.erase(word);
        return luaL_error(L, "wc.native: the table entry could not be replaced (%s)", notes.c_str());
    }
    LuaLog("%s hooks %s (word %08X, engine handler %p)", s->mod.c_str(), n ? n->name : "?", word, handler);
    return 0;
}

int CallNative(lua_State* L, bool asFloat) {
    // A word instead of a name: a native the game's own table never had, put there by something else in the
    // platform or by another mod.  Nothing is known about its shape, so this form is the simple one - no
    // arguments, one 4-byte answer, which is what a getter looks like.
    wmnatives::Native adhoc;
    const wmnatives::Native* n;
    const char* name;
    char hex[16];
    if (lua_type(L, 1) == LUA_TNUMBER) {
        memset(&adhoc, 0, sizeof(adhoc));
        adhoc.word = (uint32_t)lua_tointeger(L, 1);
        adhoc.nargs = 0;
        adhoc.retsize = 4;
        _snprintf(hex, sizeof(hex), "%08X", adhoc.word);
        adhoc.name = hex;
        n = &adhoc;
        name = hex;
    } else {
        name = luaL_checkstring(L, 1);
        n = FindNative(name);
        if (!n) return luaL_error(L, "wc.call: unknown native '%s'", name);
    }
    if (!s_applyOk || !VmReady()) return luaL_error(L, "wc.call: the script VM is not the expected one");
    uint32_t rank;
    void* handler;
    if (!NativeEntry(n->word, &rank, &handler)) return luaL_error(L, "wc.call: %s is not in the executable's table", name);
    int given = lua_gettop(L) - 1;
    if (given != n->nargs) return luaL_error(L, "wc.call: %s takes %d argument(s), %d given", name, n->nargs, given);
    // The table lists the sizes in the order the engine's wrapper pops them (the last argument first); the script
    // pushes the arguments in the C signature's order, and so does this: Lua argument i has the size argsizes[n-1-i].
    std::vector<std::string> strings;
    strings.reserve(n->nargs);
    for (int i = 0; i < n->nargs; ++i) {
        int size = n->argsizes[n->nargs - 1 - i];
        int t = lua_type(L, 2 + i);
        if (size == 12) {                        // a vector argument: {x, y, z}
            if (t != LUA_TTABLE) return luaL_error(L, "wc.call: %s: argument %d is a vector, give {x, y, z}", name, i);
            float v[3];
            for (int k = 0; k < 3; ++k) {
                lua_rawgeti(L, 2 + i, k + 1);
                v[k] = (float)luaL_checknumber(L, -1);
                lua_pop(L, 1);
            }
            VmPushBytes(v, 12);
            continue;
        }
        if (size != 4) return luaL_error(L, "wc.call: %s: argument %d is %d bytes (only 4-byte and vector arguments can be given)", name, i, size);
        if (t == LUA_TSTRING) {
            strings.push_back(lua_tostring(L, 2 + i));
            VmPush((uint32_t)(uintptr_t)strings.back().c_str());
        } else if (t == LUA_TBOOLEAN) {
            VmPush(lua_toboolean(L, 2 + i) ? 1 : 0);
        } else if (lua_isinteger(L, 2 + i)) {
            VmPush((uint32_t)(int32_t)lua_tointeger(L, 2 + i));
        } else {
            float f = (float)luaL_checknumber(L, 2 + i);
            uint32_t v;
            memcpy(&v, &f, 4);
            VmPush(v);
        }
    }
    // the handler of the word, with a node holding the rank (the original handler when this word is hooked)
    std::map<uint32_t, Hook>::iterator hk = s_hooks.find(n->word);
    void* (*fn)(void*) = (hk != s_hooks.end() && hk->second.orig) ? hk->second.orig : (void* (*)(void*))handler;
    uint32_t node[4] = { rank, 0, 0, 0 };
    fn(node);
    if (n->retsize == 4) {
        uint32_t v = VmPop();
        if (asFloat) { float f; memcpy(&f, &v, 4); lua_pushnumber(L, f); }
        else lua_pushinteger(L, (int32_t)v);
        return 1;
    }
    if (n->retsize == 12) {                     // a vector result: x, y, z
        uint32_t idx = VmIndex() - 1;
        const float* v = (const float*)(uintptr_t)VmPtrs()[idx];
        float x = v[0], y = v[1], z = v[2];
        VmIndex() = idx;
        VmOffset() -= 12;
        lua_pushnumber(L, x);
        lua_pushnumber(L, y);
        lua_pushnumber(L, z);
        return 3;
    }
    if (n->retsize > 4) {                       // another structure: taken off the stack, not returned
        VmIndex() -= 1;
        VmOffset() -= n->retsize;
    }
    return 0;
}

int l_call(lua_State* L) { return CallNative(L, false); }
int l_callf(lua_State* L) { return CallNative(L, true); }

// require() from the mod's folder
int l_searcher(lua_State* L) {
    Script* s = ScriptOf(L);
    std::string name = luaL_checkstring(L, 1);
    for (size_t i = 0; i < name.size(); ++i) if (name[i] == '.') name[i] = '\\';
    std::string path = s->dir + "\\" + name + ".lua";
    if (GetFileAttributesA(path.c_str()) == INVALID_FILE_ATTRIBUTES) {
        lua_pushfstring(L, "\n\tno file '%s'", path.c_str());
        return 1;
    }
    if (luaL_loadfile(L, path.c_str()) != LUA_OK) return lua_error(L);
    lua_pushstring(L, path.c_str());
    return 2;
}

const luaL_Reg kWc[] = {
    { "log", l_log }, { "on", l_on }, { "every", l_every }, { "time", l_time }, { "screen", l_screen },
    { "screenshot", l_screenshot }, { "key", l_key },
    { "pressed", l_pressed }, { "text", l_text }, { "native", l_native }, { "call", l_call }, { "callf", l_callf },
    { NULL, NULL }
};
const luaL_Reg kMem[] = {
    { "u8", l_read<uint8_t> }, { "u16", l_read<uint16_t> }, { "u32", l_read<uint32_t> }, { "i32", l_read<int32_t> },
    { "f32", l_read_f32 }, { "write_u8", l_write<uint8_t> }, { "write_u16", l_write<uint16_t> },
    { "write_u32", l_write<uint32_t> }, { "write_i32", l_write<int32_t> }, { "write_f32", l_write_f32 },
    { "patch", l_patch }, { "string", l_string }, { NULL, NULL }
};
const luaL_Reg kGame[] = {
    { "fps_cap", l_fps_cap }, { "afx", l_afx }, { "high_detail", l_high_detail },
    { "freeze", l_freeze }, { "block_input", l_block_input }, { "mute", l_mute }, { NULL, NULL }
};
const luaL_Reg kDraw[] = {
    { "picture", l_picture }, { "rect", l_rect }, { "image", l_image }, { "image_at", l_image_at },
    { "image_free", l_image_free }, { NULL, NULL }
};
const luaL_Reg kAudio[] = {
    { "sound", l_sound }, { "music", l_music }, { NULL, NULL }
};

struct Vk { const char* name; int vk; };
const Vk kVk[] = {
    { "SPACE", VK_SPACE }, { "ENTER", VK_RETURN }, { "ESCAPE", VK_ESCAPE }, { "TAB", VK_TAB }, { "SHIFT", VK_SHIFT },
    { "CTRL", VK_CONTROL }, { "ALT", VK_MENU }, { "UP", VK_UP }, { "DOWN", VK_DOWN }, { "LEFT", VK_LEFT },
    { "RIGHT", VK_RIGHT }, { "BACKSPACE", VK_BACK }, { "DELETE", VK_DELETE }, { "INSERT", VK_INSERT },
    { "HOME", VK_HOME }, { "END", VK_END }, { "PAGEUP", VK_PRIOR }, { "PAGEDOWN", VK_NEXT },
    { "LBUTTON", VK_LBUTTON }, { "RBUTTON", VK_RBUTTON }, { "MBUTTON", VK_MBUTTON },
};

void OpenWc(Script* s) {
    lua_State* L = s->L;
    lua_pushlightuserdata(L, s);
    lua_setfield(L, LUA_REGISTRYINDEX, "wc_script");
    luaL_newmetatable(L, "wc.vm");
    lua_pushvalue(L, -1);
    lua_setfield(L, -2, "__index");
    luaL_setfuncs(L, kVmMethods, 0);
    lua_pop(L, 1);

    luaL_newlib(L, kWc);
    luaL_newlib(L, kMem);
    lua_setfield(L, -2, "mem");
    luaL_newlib(L, kGame);
    lua_setfield(L, -2, "game");
    luaL_newlib(L, kDraw);
    lua_setfield(L, -2, "draw");
    luaL_newlib(L, kAudio);
    lua_setfield(L, -2, "audio");
    lua_newtable(L);
    for (size_t i = 0; i < sizeof(kVk) / sizeof(kVk[0]); ++i) {
        lua_pushinteger(L, kVk[i].vk);
        lua_setfield(L, -2, kVk[i].name);
    }
    for (char c = 'A'; c <= 'Z'; ++c) { char n[2] = { c, 0 }; lua_pushinteger(L, c); lua_setfield(L, -2, n); }
    for (char c = '0'; c <= '9'; ++c) { char n[3] = { 'N', c, 0 }; lua_pushinteger(L, c); lua_setfield(L, -2, n); }
    for (int f = 1; f <= 12; ++f) { char n[4]; _snprintf(n, 4, "F%d", f); n[3] = 0; lua_pushinteger(L, VK_F1 + f - 1); lua_setfield(L, -2, n); }
    lua_setfield(L, -2, "vk");
    lua_newtable(L);
    for (int i = 0; i < wmnatives::kCount; ++i) {
        lua_pushinteger(L, wmnatives::kTable[i].word);
        lua_setfield(L, -2, wmnatives::kTable[i].name);
    }
    lua_setfield(L, -2, "natives");
    lua_pushstring(L, s->mod.c_str());
    lua_setfield(L, -2, "mod");
    lua_pushstring(L, s->dir.c_str());
    lua_setfield(L, -2, "dir");
    lua_setglobal(L, "wc");
    // print -> wc.log; require from the mod folder
    lua_pushcfunction(L, l_log);
    lua_setglobal(L, "print");
    lua_getglobal(L, "package");
    if (lua_istable(L, -1)) {
        lua_getfield(L, -1, "searchers");
        if (lua_istable(L, -1)) {
            lua_pushcfunction(L, l_searcher);
            lua_rawseti(L, -2, 2);                     // after the preload searcher, before the path ones
        }
        lua_pop(L, 1);
    }
    lua_pop(L, 1);
}

void StartScripts() {
    const std::vector<std::string>& mods = ModsEnabled();
    for (size_t i = 0; i < mods.size(); ++i) {
        std::string dir = g_dllDir + "mods\\" + mods[i];
        std::string main = dir + "\\main.lua";
        if (GetFileAttributesA(main.c_str()) == INVALID_FILE_ATTRIBUTES) continue;
        Script* s = new Script;
        s->mod = mods[i];
        s->dir = dir;
        memset(s->keys, 0, sizeof(s->keys));
        s->L = luaL_newstate();
        luaL_openlibs(s->L);
        OpenWc(s);
        s_scripts.push_back(s);
        s_current = s;
        if (luaL_loadfile(s->L, main.c_str()) != LUA_OK || lua_pcall(s->L, 0, 0, 0) != LUA_OK) {
            ReportError(s, "main.lua", s->L);
            s->dead = s->frameRef == LUA_NOREF && s->drawRef == LUA_NOREF && s->timers.empty();
        } else {
            LuaLog("%s: main.lua ran", s->mod.c_str());
        }
        s_current = NULL;
    }
    LuaLog("%u script%s running", (unsigned)s_scripts.size(), s_scripts.size() == 1 ? "" : "s");
}

// ---------------------------------------------------------------------------------------------- the overlay
typedef HRESULT (WINAPI* CreateFontFn)(IDirect3DDevice9*, INT, UINT, UINT, UINT, BOOL, DWORD, DWORD, DWORD, DWORD,
                                       LPCSTR, void**);
struct ID3DXFontLite : IUnknown {           // the ID3DXFont methods used, at their vtable positions
    virtual HRESULT STDMETHODCALLTYPE GetDevice(IDirect3DDevice9**) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetDescA(void*) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetDescW(void*) = 0;
    virtual BOOL STDMETHODCALLTYPE GetTextMetricsA(void*) = 0;
    virtual BOOL STDMETHODCALLTYPE GetTextMetricsW(void*) = 0;
    virtual HDC STDMETHODCALLTYPE GetDC() = 0;
    virtual HRESULT STDMETHODCALLTYPE GetGlyphData(UINT, void**, RECT*, POINT*) = 0;
    virtual HRESULT STDMETHODCALLTYPE PreloadCharacters(UINT, UINT) = 0;
    virtual HRESULT STDMETHODCALLTYPE PreloadGlyphs(UINT, UINT) = 0;
    virtual HRESULT STDMETHODCALLTYPE PreloadTextA(LPCSTR, INT) = 0;
    virtual HRESULT STDMETHODCALLTYPE PreloadTextW(LPCWSTR, INT) = 0;
    virtual INT STDMETHODCALLTYPE DrawTextA(void* sprite, LPCSTR text, INT count, LPRECT rect, DWORD format, D3DCOLOR color) = 0;
    virtual INT STDMETHODCALLTYPE DrawTextW(void* sprite, LPCWSTR text, INT count, LPRECT rect, DWORD format, D3DCOLOR color) = 0;
    virtual HRESULT STDMETHODCALLTYPE OnLostDevice() = 0;
    virtual HRESULT STDMETHODCALLTYPE OnResetDevice() = 0;
};

typedef HRESULT (WINAPI* SaveSurfaceFn)(LPCSTR, DWORD, IDirect3DSurface9*, const PALETTEENTRY*, const RECT*);

void SaveShot(IDirect3DDevice9* dev, const RECT& picture) {
    std::string path = s_shotPath;
    s_shotPath.clear();
    HMODULE d3dx = GetModuleHandleA("d3dx9_37.dll");
    SaveSurfaceFn save = d3dx ? (SaveSurfaceFn)GetProcAddress(d3dx, "D3DXSaveSurfaceToFileA") : NULL;
    IDirect3DSurface9* back = NULL;
    if (!save || FAILED(dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &back)) || !back) {
        LuaLog("screenshot: no D3DX / back buffer");
        return;
    }
    size_t dot = path.find_last_of('.');
    std::string ext = dot == std::string::npos ? "" : path.substr(dot + 1);
    for (size_t i = 0; i < ext.size(); ++i) ext[i] = (char)tolower((unsigned char)ext[i]);
    DWORD fmt = ext == "png" ? 3 : ext == "jpg" || ext == "jpeg" ? 1 : ext == "dds" ? 4 : 0;   // D3DXIMAGE_FILEFORMAT
    HRESULT hr = save(path.c_str(), fmt, back, NULL, &picture);
    back->Release();
    LuaLog("screenshot %s: %s", path.c_str(), SUCCEEDED(hr) ? "saved" : "FAILED");
}

// The mods' own drawing: wc.on("draw") handlers run here, with wc.draw.* legal until they return.  This is the
// render thread's call into Lua, so it takes the same lock as everything else.
void DrawHandlers(IDirect3DDevice9* dev, const RECT& picture) {
    bool any = false;
    for (size_t i = 0; i < s_scripts.size(); ++i) if (s_scripts[i]->drawRef != LUA_NOREF) any = true;
    if (!any) return;
    MediaDrawBegin(dev, picture);
    for (size_t i = 0; i < s_scripts.size(); ++i) {
        Script* s = s_scripts[i];
        if (s->dead || s->drawRef == LUA_NOREF) continue;
        lua_State* L = s->L;
        lua_rawgeti(L, LUA_REGISTRYINDEX, s->drawRef);
        lua_pushinteger(L, picture.right - picture.left);
        lua_pushinteger(L, picture.bottom - picture.top);
        s_current = s;
        if (lua_pcall(L, 2, 0, 0) != LUA_OK) ReportError(s, "draw", L);
        s_current = NULL;
    }
    MediaDrawEnd();
}

void OverlayDraw(IDirect3DDevice9* dev, const RECT& picture) {
    Lock lock;
    DrawHandlers(dev, picture);
    if (!s_shotPath.empty() && (s_texts.empty() || s_fontFailed)) SaveShot(dev, picture);
    if (s_texts.empty() || s_fontFailed) return;
    if (!s_fontD3dx) {
        HMODULE d3dx = GetModuleHandleA("d3dx9_37.dll");
        CreateFontFn create = d3dx ? (CreateFontFn)GetProcAddress(d3dx, "D3DXCreateFontA") : NULL;
        if (!create || FAILED(create(dev, 22, 0, 700, 1, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, DEFAULT_QUALITY,
                                     DEFAULT_PITCH | FF_DONTCARE, "Arial", &s_fontD3dx))) {
            s_fontFailed = true;
            LuaLog("no D3DX font: wc.text draws nothing");
            return;
        }
    }
    ID3DXFontLite* font = (ID3DXFontLite*)s_fontD3dx;
    for (std::map<std::string, Text>::const_iterator it = s_texts.begin(); it != s_texts.end(); ++it) {
        const Text& t = it->second;
        int x = picture.left + t.x, y = picture.top + t.y;              // wc.text places in the picture's pixels
        RECT r = { x + 1, y + 1, x + 1, y + 1 };
        font->DrawTextA(NULL, t.s.c_str(), -1, &r, DT_NOCLIP, 0xC0000000u);              // a shadow
        RECT r2 = { x, y, x, y };
        font->DrawTextA(NULL, t.s.c_str(), -1, &r2, DT_NOCLIP, t.color);
    }
    if (!s_shotPath.empty()) SaveShot(dev, picture);
}

void OverlayLost() {
    Lock lock;
    if (s_fontD3dx) ((ID3DXFontLite*)s_fontD3dx)->OnLostDevice();
}

void OverlayRestored() {
    Lock lock;
    if (s_fontD3dx) ((ID3DXFontLite*)s_fontD3dx)->OnResetDevice();
}

}  // namespace

void LuaAddNative(const wmnatives::Native& n) { s_extraNatives.push_back(n); }

void LuaAttach(const std::string& iniPath) {
    s_inAttach = true;
    if (!s_csInit) { InitializeCriticalSection(&s_cs); s_csInit = true; }
    std::string v;
    if (ReadIniKey(iniPath, "mods", "lua", v)) s_enabled = atoi(v.c_str()) != 0;
    if (ReadIniKey(iniPath, "mods", "log", v)) s_log = atoi(v.c_str()) != 0;
    ProcessImage img;
    uint8_t probe[4];
    if (!s_enabled || !img.Read(NATIVES, probe, 4)) {
        if (!s_enabled) LuaLog("off ([mods] lua=0)");
        s_inAttach = false;
        return;
    }
    std::string rep;
    uint32_t got = 0;
    s_applyOk = CheckCrc(img, kCode[0].va, kCode[0].len, kCode[0].crc, &got);
    if (!s_applyOk) LuaLog("PC executable differs (native call site): wc.native / wc.call are off");
    GfxOverlay ov;
    ov.draw = OverlayDraw;
    ov.lost = OverlayLost;
    ov.restored = OverlayRestored;
    GfxSetOverlay(ov);
    s_inAttach = false;
}

void LuaAfterConfig() {
    for (size_t i = 0; i < s_pending.size(); ++i) if (s_log) Log("LUA: %s", s_pending[i].c_str());
    s_pending.clear();
}

void LuaFrame() {
    if (!s_enabled) return;
    if (!s_started) {
        s_started = true;
        s_lastFrame = NowSeconds();
        StartScripts();
    }
    if (s_scripts.empty()) return;
    double now = NowSeconds();
    double dt = now - s_lastFrame;
    s_lastFrame = now;
    if (dt < 0 || dt > 1.0) dt = 0;
    bool focus = GameHasFocus();
    for (int k = 1; k < 256; ++k) s_keysNow[k] = focus && (GetAsyncKeyState(k) & 0x8000) ? 1 : 0;
    for (size_t i = 0; i < s_scripts.size(); ++i) {
        Script* s = s_scripts[i];
        if (s->dead) continue;
        s_current = s;
        lua_State* L = s->L;
        if (s->frameRef != LUA_NOREF) {
            lua_rawgeti(L, LUA_REGISTRYINDEX, s->frameRef);
            lua_pushnumber(L, dt);
            if (lua_pcall(L, 1, 0, 0) != LUA_OK) ReportError(s, "frame", L);
        }
        for (size_t t = 0; t < s->timers.size(); ++t) {
            Timer& tm = s->timers[t];
            if (now < tm.next) continue;
            tm.next = now + tm.period;
            lua_rawgeti(L, LUA_REGISTRYINDEX, tm.ref);
            if (lua_pcall(L, 0, 0, 0) != LUA_OK) ReportError(s, "timer", L);
        }
        memcpy(s->keys, s_keysNow, sizeof(s_keysNow));
        if (s->errors > 1000) { s->dead = true; LuaLog("%s: too many errors, stopped", s->mod.c_str()); }
        s_current = NULL;
    }
}
