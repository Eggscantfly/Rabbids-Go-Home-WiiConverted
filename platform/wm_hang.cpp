// wm_hang.cpp - hang watch ([general] hang_watch=1, with log=1): when the game's main thread completes no frame for
// 4 s, the log gets where that thread is (its registers and the return addresses found on its stack, as module +
// address), again every 2 s while it stays stuck (at most 8 samples per stall), and a line when frames resume.  A
// freeze can then be located from the log alone.
//
// The frame tick comes from the Options module's call after the engine's input poll (ViD::OneFrame), which also names
// the main thread.  A sample suspends the thread only to read its context and copy its stack into a static buffer;
// everything that could take a lock the suspended thread might hold (the log, the loader lock of GetModuleHandleEx,
// the heap) runs after it is resumed.
//
// Crash log ([general] crash_log=1, the default with log=1): a vectored exception handler sees every exception
// before any handler, the engine's own unhandled-exception filter included - that filter spins for five seconds
// trying to write a report into a LyNcrash folder the port does not have, then dies in its own error path, so a
// crash used to leave nothing but a hang sample of the reporter.  A fatal exception (an access violation, an illegal
// instruction, a stack overflow: the error class) is logged from the faulting thread with where it happened, the
// registers, and the return addresses found on its stack from the faulting frame up, as module + address, and a
// minidump (crash_<time>.dmp beside the log) is written when dbghelp.dll can be had.  The exception then goes on
// exactly as before.  Nothing is allocated on the way: a crash from a broken heap must still get logged.
#include "wiimote.h"

#include <dbghelp.h>
#include <intrin.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

using namespace wmpatch;

namespace {

const double STALL_SECONDS = 4.0;
const double SAMPLE_EVERY = 2.0;
const int    MAX_SAMPLES = 8;
const int    MAX_FRAMES_SHOWN = 48;
enum { STACK_WORDS = 8192 };

volatile LONG s_frames;
DWORD  s_mainId;
HANDLE s_main;
HANDLE s_watch;
bool   s_started, s_off;
uint32_t s_stack[STACK_WORDS];

bool Readable(uintptr_t addr, size_t n) {
    MEMORY_BASIC_INFORMATION mbi;
    if (!VirtualQuery((LPCVOID)addr, &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT) return false;
    if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return false;
    return addr + n <= (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
}

bool Executable(uintptr_t addr) {
    MEMORY_BASIC_INFORMATION mbi;
    if (!VirtualQuery((LPCVOID)addr, &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT) return false;
    return (mbi.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) != 0;
}

// a return address follows a call: E8 rel32, FF /2 with a register, [reg], [reg+disp8], [reg+disp32] or [abs32]
bool AfterCall(uintptr_t ret) {
    if (!Executable(ret) || !Readable(ret - 7, 7)) return false;
    const uint8_t* p = (const uint8_t*)(ret - 7);
    if (p[2] == 0xE8) return true;                                          // call rel32
    if (p[5] == 0xFF && (p[6] & 0x38) == 0x10 && (p[6] >> 6) == 3) return true;   // call reg
    if (p[5] == 0xFF && (p[6] & 0x38) == 0x10 && (p[6] >> 6) == 0) return true;   // call [reg]
    if (p[4] == 0xFF && (p[5] & 0x38) == 0x10 && (p[5] >> 6) == 1) return true;   // call [reg+disp8]
    if (p[3] == 0xFF && (p[4] & 0x38) == 0x10 && (p[4] >> 6) == 1) return true;   // call [reg+reg*s+disp8]
    if (p[1] == 0xFF && (p[2] & 0x38) == 0x10) return true;                 // call [reg+disp32] / [abs32]
    if (p[0] == 0xFF && (p[1] & 0x38) == 0x10) return true;                 // call [reg+reg*s+disp32]
    return false;
}

std::string Where(uintptr_t addr) {
    HMODULE m = NULL;
    char name[MAX_PATH] = "?";
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCSTR)addr, &m) && m) {
        char path[MAX_PATH];
        DWORD n = GetModuleFileNameA(m, path, MAX_PATH);
        if (n) {
            std::string p(path, n);
            size_t slash = p.find_last_of("\\/");
            _snprintf(name, sizeof(name) - 1, "%s", p.substr(slash == std::string::npos ? 0 : slash + 1).c_str());
            name[sizeof(name) - 1] = 0;
        }
    }
    char buf[MAX_PATH + 32];
    _snprintf(buf, sizeof(buf) - 1, "%s!%08X", name, (unsigned)addr);
    buf[sizeof(buf) - 1] = 0;
    return buf;
}

void Sample(double stalled, int index) {
    CONTEXT ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
    size_t words = 0;
    if (SuspendThread(s_main) == (DWORD)-1) return;
    BOOL ok = GetThreadContext(s_main, &ctx);
    if (ok) {
        MEMORY_BASIC_INFORMATION mbi;
        if (VirtualQuery((LPCVOID)(uintptr_t)ctx.Esp, &mbi, sizeof(mbi)) && mbi.State == MEM_COMMIT) {
            size_t avail = ((uintptr_t)mbi.BaseAddress + mbi.RegionSize - ctx.Esp) / 4;
            words = avail < STACK_WORDS ? avail : STACK_WORDS;
            memcpy(s_stack, (void*)(uintptr_t)ctx.Esp, words * 4);
        }
    }
    ResumeThread(s_main);
    if (!ok) {
        Log("HANG: no frame for %.1f s; the main thread's context could not be read (error %lu)", stalled,
            GetLastError());
        return;
    }
    std::vector<std::pair<uintptr_t, size_t> > rets;     // (return address, stack offset)
    uintptr_t prev = 0;
    for (size_t i = 0; i < words && (int)rets.size() < MAX_FRAMES_SHOWN; ++i) {
        uintptr_t v = s_stack[i];
        if (v < 0x10000 || v == prev || !AfterCall(v)) continue;
        rets.push_back(std::make_pair(v, i * 4));
        prev = v;
    }
    // raw addresses first (module lookups take the loader lock, which the stuck thread may hold), then module names
    std::string raw;
    char buf[48];
    for (size_t i = 0; i < rets.size(); ++i) {
        _snprintf(buf, sizeof(buf) - 1, " %08X", (unsigned)rets[i].first);
        buf[sizeof(buf) - 1] = 0;
        raw += buf;
    }
    Log("HANG: no frame for %.1f s (sample %d): eip %08X esp %08X ebp %08X eax %08X ebx %08X ecx %08X edx %08X "
        "esi %08X edi %08X; return addresses (%u stack words read):%s", stalled, index + 1, (unsigned)ctx.Eip,
        (unsigned)ctx.Esp, (unsigned)ctx.Ebp, (unsigned)ctx.Eax, (unsigned)ctx.Ebx, (unsigned)ctx.Ecx,
        (unsigned)ctx.Edx, (unsigned)ctx.Esi, (unsigned)ctx.Edi, (unsigned)words, raw.empty() ? " none" : raw.c_str());
    std::string named = "eip " + Where(ctx.Eip);
    for (size_t i = 0; i < rets.size(); ++i) {
        _snprintf(buf, sizeof(buf) - 1, " [esp+%X]", (unsigned)rets[i].second);
        buf[sizeof(buf) - 1] = 0;
        named += "\n      " + Where(rets[i].first) + buf;
    }
    Log("HANG: sample %d by module: %s", index + 1, named.c_str());
}

// ---------------------------------------------------------------------------------------------------------------
// crash log
// ---------------------------------------------------------------------------------------------------------------
const int MAX_CRASHES_LOGGED = 4;               // a fault that is caught and repeats does not fill the log
const int MAX_CRASH_FRAMES = 64;
volatile LONG s_crashes;
volatile LONG s_inCrash;
uint32_t s_crashStack[STACK_WORDS];

// the module a code address is in, without the heap: into the caller's buffer
void WhereInto(uintptr_t addr, char* out, size_t n) {
    HMODULE m = NULL;
    const char* name = "?";
    char path[MAX_PATH];
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCSTR)addr, &m) && m) {
        DWORD len = GetModuleFileNameA(m, path, MAX_PATH);
        if (len) {
            path[len < MAX_PATH ? len : MAX_PATH - 1] = 0;
            const char* slash = strrchr(path, '\\');
            const char* fslash = strrchr(path, '/');
            if (fslash > slash) slash = fslash;
            name = slash ? slash + 1 : path;
        }
    }
    _snprintf(out, n - 1, "%s!%08X", name, (unsigned)addr);
    out[n - 1] = 0;
}

typedef BOOL (WINAPI* MiniDumpWriteDumpFn)(HANDLE, DWORD, HANDLE, MINIDUMP_TYPE, PMINIDUMP_EXCEPTION_INFORMATION,
                                           PMINIDUMP_USER_STREAM_INFORMATION, PMINIDUMP_CALLBACK_INFORMATION);

void CrashDump(EXCEPTION_POINTERS* ep, const char* stamp) {
    HMODULE dbg = LoadLibraryA("dbghelp.dll");
    MiniDumpWriteDumpFn write = dbg ? (MiniDumpWriteDumpFn)GetProcAddress(dbg, "MiniDumpWriteDump") : NULL;
    if (!write) { Log("CRASH: no minidump: dbghelp.dll is not usable"); return; }
    char path[MAX_PATH + 48];
    _snprintf(path, sizeof(path) - 1, "%scrash_%s.dmp", g_dllDir.c_str(), stamp);
    path[sizeof(path) - 1] = 0;
    HANDLE f = CreateFileA(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) { Log("CRASH: no minidump: %s cannot be written (error %lu)", path, GetLastError()); return; }
    MINIDUMP_EXCEPTION_INFORMATION info;
    info.ThreadId = GetCurrentThreadId();
    info.ExceptionPointers = ep;
    info.ClientPointers = FALSE;
    // the threads' stacks and registers, the modules, the data segments, and what the stacks point at
    MINIDUMP_TYPE type = (MINIDUMP_TYPE)(MiniDumpWithDataSegs | MiniDumpWithIndirectlyReferencedMemory);
    BOOL ok = write(GetCurrentProcess(), GetCurrentProcessId(), f, type, &info, NULL, NULL);
    DWORD err = GetLastError();
    CloseHandle(f);
    if (ok) Log("CRASH: minidump written: %s", path);
    else Log("CRASH: minidump NOT written to %s (error %08lX)", path, err);
}

LONG WINAPI CrashHandler(EXCEPTION_POINTERS* ep) {
    if (!ep || !ep->ExceptionRecord || !ep->ContextRecord) return EXCEPTION_CONTINUE_SEARCH;
    DWORD code = ep->ExceptionRecord->ExceptionCode;
    if ((code & 0xF0000000u) != 0xC0000000u) return EXCEPTION_CONTINUE_SEARCH;   // the error class only: not a C++
                                                                               // throw (E06D7363), a breakpoint, a
                                                                               // guard page, a debugger note
    if (InterlockedCompareExchange(&s_inCrash, 1, 0) != 0) return EXCEPTION_CONTINUE_SEARCH;   // a fault in here
    if (InterlockedIncrement(&s_crashes) <= MAX_CRASHES_LOGGED) {
        const EXCEPTION_RECORD& r = *ep->ExceptionRecord;
        const CONTEXT& c = *ep->ContextRecord;
        char what[64] = "";
        if (code == EXCEPTION_ACCESS_VIOLATION && r.NumberParameters >= 2)
            _snprintf(what, sizeof(what) - 1, " (%s %08X)",
                      r.ExceptionInformation[0] == 0 ? "reading" : r.ExceptionInformation[0] == 1 ? "writing" : "executing",
                      (unsigned)r.ExceptionInformation[1]);
        what[sizeof(what) - 1] = 0;
        SYSTEMTIME t;
        GetLocalTime(&t);
        char stamp[32];
        _snprintf(stamp, sizeof(stamp) - 1, "%04u%02u%02u_%02u%02u%02u", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
        stamp[sizeof(stamp) - 1] = 0;
        // the stack from the faulting frame up: the return addresses on it, raw first, then by module
        size_t words = 0;
        MEMORY_BASIC_INFORMATION mbi;
        if (VirtualQuery((LPCVOID)(uintptr_t)c.Esp, &mbi, sizeof(mbi)) && mbi.State == MEM_COMMIT) {
            size_t avail = ((uintptr_t)mbi.BaseAddress + mbi.RegionSize - c.Esp) / 4;
            words = avail < STACK_WORDS ? avail : STACK_WORDS;
            memcpy(s_crashStack, (void*)(uintptr_t)c.Esp, words * 4);
        }
        uintptr_t rets[MAX_CRASH_FRAMES];
        size_t offs[MAX_CRASH_FRAMES];
        int n = 0;
        uintptr_t prev = 0;
        for (size_t i = 0; i < words && n < MAX_CRASH_FRAMES; ++i) {
            uintptr_t v = s_crashStack[i];
            if (v < 0x10000 || v == prev || !AfterCall(v)) continue;
            rets[n] = v;
            offs[n] = i * 4;
            ++n;
            prev = v;
        }
        char raw[MAX_CRASH_FRAMES * 10 + 8] = "";
        size_t at = 0;
        for (int i = 0; i < n && at + 10 < sizeof(raw); ++i)
            at += _snprintf(raw + at, sizeof(raw) - at - 1, " %08X", (unsigned)rets[i]);
        DWORD tid = GetCurrentThreadId();
        char where[MAX_PATH + 16];
        WhereInto(c.Eip, where, sizeof(where));
        Log("CRASH: exception %08X%s at %s on thread %lu%s; eax %08X ebx %08X ecx %08X edx %08X esi %08X edi %08X "
            "ebp %08X esp %08X; return addresses (%u stack words read):%s", (unsigned)code, what, where, tid,
            tid == s_mainId ? " (the main thread)" : "", (unsigned)c.Eax, (unsigned)c.Ebx, (unsigned)c.Ecx,
            (unsigned)c.Edx, (unsigned)c.Esi, (unsigned)c.Edi, (unsigned)c.Ebp, (unsigned)c.Esp, (unsigned)words,
            n ? raw : " none");
        static char named[MAX_CRASH_FRAMES * (MAX_PATH + 40) + 64];   // one crash at a time: s_inCrash
        at = 0;
        for (int i = 0; i < n && at + MAX_PATH + 40 < sizeof(named); ++i) {
            WhereInto(rets[i], where, sizeof(where));
            at += _snprintf(named + at, sizeof(named) - at - 1, "\n      %s [esp+%X]", where, (unsigned)offs[i]);
        }
        named[at < sizeof(named) ? at : sizeof(named) - 1] = 0;
        Log("CRASH: by module (logged as raised: a handler of the game's may still catch it):%s", n ? named : " none");
        CrashDump(ep, stamp);
    }
    InterlockedExchange(&s_inCrash, 0);
    return EXCEPTION_CONTINUE_SEARCH;
}

void CrashLogInstall() {
    std::string v;
    if (ReadIniKey(g_dllDir + "wiimote.ini", "general", "crash_log", v) && atoi(v.c_str()) == 0) return;
    void* h = AddVectoredExceptionHandler(1, CrashHandler);
    Log("CRASH: log %s: a fatal exception is logged with its place, registers and return addresses, and a minidump "
        "is written beside this log", h ? "on" : "NOT installed");
}

DWORD WINAPI WatchProc(LPVOID) {
    LONG last = s_frames;
    double changed = NowSeconds(), lastSample = 0;
    int samples = 0;
    for (;;) {
        Sleep(250);
        LONG f = s_frames;
        double t = NowSeconds();
        if (f != last) {
            if (samples) Log("HANG: frames again after %.1f s", t - changed);
            last = f;
            changed = t;
            samples = 0;
            continue;
        }
        if (t - changed >= STALL_SECONDS && samples < MAX_SAMPLES && t - lastSample >= SAMPLE_EVERY) {
            Sample(t - changed, samples);
            ++samples;
            lastSample = t;
        }
    }
}

// ---------------------------------------------------------------------------------------------------------------
// shader compile log ([general] shader_log=1): the executable's imports of D3DXCompileShader and D3DXCreateEffect
// (IAT 0089D664, 0089D65C) are wrapped to log each call (entry point, profile, defines, duration, thread, caller)
// ---------------------------------------------------------------------------------------------------------------
const uint32_t IAT_COMPILE_SHADER = 0x0089D664;
const uint32_t IAT_CREATE_EFFECT = 0x0089D65C;

typedef HRESULT (WINAPI* CompileShaderFn)(const char*, UINT, const void*, void*, const char*, const char*, DWORD,
                                          void**, void**, void**);
typedef HRESULT (WINAPI* CreateEffectFn)(void*, const void*, UINT, const void*, void*, DWORD, void*, void**, void**);
CompileShaderFn s_compile;
CreateEffectFn s_createEffect;
volatile LONG s_compiles;
double s_compileMs;

std::string Defines(const void* defines) {
    std::string out;
    const char* const* d = (const char* const*)defines;
    for (int i = 0; d && d[2 * i] && i < 64; ++i) {
        out += " ";
        out += d[2 * i];
        if (d[2 * i + 1] && *d[2 * i + 1]) {
            out += "=";
            out += d[2 * i + 1];
        }
    }
    return out;
}

HRESULT WINAPI CompileShaderHook(const char* src, UINT len, const void* defines, void* include, const char* fn,
                                 const char* profile, DWORD flags, void** shader, void** errors, void** table) {
    double t0 = NowSeconds();
    HRESULT hr = s_compile(src, len, defines, include, fn, profile, flags, shader, errors, table);
    double ms = (NowSeconds() - t0) * 1000.0;
    LONG n = InterlockedIncrement(&s_compiles);
    s_compileMs += ms;
    Log("SHADER: #%ld D3DXCompileShader %s %s flags %lX (%u bytes source) defines:%s -> %08lX in %.0f ms "
        "(total %.0f ms), thread %lu, caller %08X", n, fn ? fn : "?", profile ? profile : "?", flags, len,
        Defines(defines).c_str(), hr, ms, s_compileMs, GetCurrentThreadId(), (unsigned)(uintptr_t)_ReturnAddress());
    return hr;
}

HRESULT WINAPI CreateEffectHook(void* dev, const void* src, UINT len, const void* defines, void* include, DWORD flags,
                                void* pool, void** effect, void** errors) {
    double t0 = NowSeconds();
    HRESULT hr = s_createEffect(dev, src, len, defines, include, flags, pool, effect, errors);
    double ms = (NowSeconds() - t0) * 1000.0;
    LONG n = InterlockedIncrement(&s_compiles);
    s_compileMs += ms;
    Log("SHADER: #%ld D3DXCreateEffect flags %lX (%u bytes source) defines:%s -> %08lX in %.0f ms (total %.0f ms), "
        "thread %lu, caller %08X", n, flags, len, Defines(defines).c_str(), hr, ms, s_compileMs, GetCurrentThreadId(),
        (unsigned)(uintptr_t)_ReturnAddress());
    return hr;
}

void ShaderLogInstall() {
    HMODULE d3dx = GetModuleHandleA("d3dx9_37.dll");
    void* realCompile = d3dx ? (void*)GetProcAddress(d3dx, "D3DXCompileShader") : NULL;
    void* realEffect = d3dx ? (void*)GetProcAddress(d3dx, "D3DXCreateEffect") : NULL;
    void* slotCompile = *(void**)(uintptr_t)IAT_COMPILE_SHADER;
    void* slotEffect = *(void**)(uintptr_t)IAT_CREATE_EFFECT;
    bool okCompile = realCompile && slotCompile == realCompile;
    bool okEffect = realEffect && slotEffect == realEffect;
    if (okCompile) {
        s_compile = (CompileShaderFn)realCompile;
        void* hook = (void*)&CompileShaderHook;
        okCompile = WriteCode(IAT_COMPILE_SHADER, &hook, 4);
    }
    if (okEffect) {
        s_createEffect = (CreateEffectFn)realEffect;
        void* hook = (void*)&CreateEffectHook;
        okEffect = WriteCode(IAT_CREATE_EFFECT, &hook, 4);
    }
    Log("SHADER: compile log: D3DXCompileShader %s, D3DXCreateEffect %s", okCompile ? "logged" : "NOT logged",
        okEffect ? "logged" : "NOT logged");
}

}  // namespace

void HangWatchFrame() {
    InterlockedIncrement(&s_frames);
    if (s_started || s_off) return;
    s_started = true;
    std::string v;
    if (g_cfg.log) CrashLogInstall();
    if (g_cfg.log && ReadIniKey(g_dllDir + "wiimote.ini", "general", "shader_log", v) && atoi(v.c_str()) == 1)
        ShaderLogInstall();
    if (!g_cfg.log || (ReadIniKey(g_dllDir + "wiimote.ini", "general", "hang_watch", v) && atoi(v.c_str()) == 0)) {
        s_off = true;
        return;
    }
    s_mainId = GetCurrentThreadId();
    s_main = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, s_mainId);
    if (s_main) s_watch = CreateThread(NULL, 0, WatchProc, NULL, 0, NULL);
    Log("HANG: watch %s (main thread %lu): a frame that takes more than %.0f s logs where the thread is",
        s_watch ? "on" : "NOT started", s_mainId, STALL_SECONDS);
}
