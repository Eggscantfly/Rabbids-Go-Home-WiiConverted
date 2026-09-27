// wm_timing.cpp - frame pacing ([timing] fps_cap in wiimote.ini, default 60 Hz; 0 = off).
//
// Why: the PC executable's main loop (00409370) calls ViD::OneFrame back to back, with no limiter and no Sleep. The
// port therefore runs at hundreds or thousands of frames per second (engine DT ~1 ms at the title screen), and vsync
// alone does not bring it to the Wii's 60 Hz. Script code written for a 60 Hz Wii (per-frame terms not scaled by DT)
// then runs many times too often.
//
// Where: the engine measures DT inside ViD::OneFrame. TIM_UpdateBeforeFrame (006D5AB0, its only call at 00503781,
// right after the input poll) stamps the frame start (QueryPerformanceCounter -> 00A92898). TIM_UpdateAfterFrame
// (006D5B80, its only call at 00503A15, the last call of OneFrame) takes now - start - paused time (00A928B4), clamps
// it and stores it as the DT of the next frame (00A928C4, copied to 00A928BC by the next TIM_UpdateBeforeFrame).
// The pacer replaces the call at 00503A15: it waits until start + paused + 1/fps_cap, then runs TIM_UpdateAfterFrame.
// The wait is inside the measured window, so the engine's DT is the capped period, with or without vsync. Each
// deadline is relative to that frame's own start: a long frame is not waited on and is not followed by a burst of
// short frames.
#include "wiimote.h"

#include <mmsystem.h>

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>

using namespace wmpatch;

namespace {

// ---------------------------------------------------------------------------------------------------------------
// the PC executable (2010 build)
// ---------------------------------------------------------------------------------------------------------------
const uint32_t CALL_AFTER_FRAME = 0x00503A15;       // ViD::OneFrame: "call TIM_UpdateAfterFrame" (hooked)
const uint32_t TIM_AFTER_FRAME  = 0x006D5B80;
const uint32_t TIM_NESTING      = 0x00A928B0;       // int: TIM_UpdateBeforeFrame depth (1 inside the outer frame)
const uint32_t TIM_FRAME_START  = 0x00A92898;       // int64: QueryPerformanceCounter at TIM_UpdateBeforeFrame
const uint32_t TIM_PAUSED       = 0x00A928B4;       // float: seconds excluded from this frame's DT
const uint32_t TIM_FRAME_DT     = 0x00A928C0;       // float: this frame's DT after the clamps, before the time factor
const uint32_t TIM_NEXT_DT      = 0x00A928C4;       // float: what TIM_UpdateAfterFrame leaves as the next frame's DT
// What the world's time is set to while a mod has asked for it to stand still (wc.game.freeze): not zero,
// because code that divides by DT would come apart, but small enough that nothing moves.
const float FROZEN_DT = 0.00001f;

struct CallSite { uint32_t va, target, crc; const char* what; };   // E8 rel32 and CRC32 of [va - 0x10, va + 0x10)

const CallSite kSites[] = {
    { 0x00503A15, 0x006D5B80, 0xA5986EEF, "ViD::OneFrame -> TIM_UpdateAfterFrame" },
    { 0x00503781, 0x006D5AB0, 0xF0629F2A, "ViD::OneFrame -> TIM_UpdateBeforeFrame" },
    { 0x00409418, 0x00503750, 0x228B5331, "main loop -> ViD::OneFrame" },
};

struct Code { uint32_t va, len, crc; const char* what; };

const Code kCode[] = {
    { 0x006D5AB0, 0x0CF, 0x420220FB, "TIM_UpdateBeforeFrame" },
    { 0x006D5B80, 0x13B, 0x77868446, "TIM_UpdateAfterFrame" },
};

// ---------------------------------------------------------------------------------------------------------------
// pacing math (pure, self-tested)
// ---------------------------------------------------------------------------------------------------------------
// Seconds to wait before the frame may end so that its DT (elapsed - paused) reaches the period. Never negative,
// never more than one period (a stale or inconsistent frame start cannot stall the game).
double WaitSeconds(double elapsed, double paused, double period) {
    if (!(period > 0.0)) return 0.0;
    double w = period + (paused > 0.0 ? paused : 0.0) - elapsed;
    if (!(w > 0.0)) return 0.0;
    return w > period ? period : w;
}

// Milliseconds to Sleep with `left` ticks remaining: everything but the last `spin` ticks, in whole milliseconds
// (timeBeginPeriod(1) is active); 0 = spin now.
DWORD SleepMs(int64_t left, int64_t spin, int64_t freq) {
    if (left <= spin || freq <= 0) return 0;
    return (DWORD)(((left - spin) * 1000) / freq);
}

float Median(std::vector<float> v) {
    if (v.empty()) return 0.0f;
    size_t m = v.size() / 2;
    std::nth_element(v.begin(), v.begin() + m, v.end());
    return v[m];
}

// ---------------------------------------------------------------------------------------------------------------
// state
// ---------------------------------------------------------------------------------------------------------------
double   s_cap;                                    // Hz, 0 = measure only
double   s_iniCap;                                 // [timing] fps_cap (the Options screen's default)
double   s_period;                                 // seconds, 0 = no waiting
bool     s_installed;
bool     s_inAttach;
bool     s_timerRes;
int64_t  s_freq = 1;
double   s_invFreq = 1.0;
std::vector<std::string> s_pending;
std::string* s_testLog;

struct Window {
    double   start, last;
    unsigned frames, late;
    double   waited;
    std::vector<float> dt, period;
};
Window s_win;

void TimLog(const char* fmt, ...) {
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
        Log("TIMING: %s", buf);
    }
}

void WaitUntil(int64_t deadline) {
    const int64_t spin = (int64_t)(0.0015 * (double)s_freq);
    for (;;) {
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        int64_t left = deadline - now.QuadPart;
        if (left <= 0) return;
        DWORD ms = SleepMs(left, spin, s_freq);
        if (ms) Sleep(ms);
        else YieldProcessor();
    }
}

void LogWindow(double now) {
    double secs = now - s_win.start;
    float dtMin = 0.0f, dtMax = 0.0f;
    if (!s_win.dt.empty()) {
        dtMin = *std::min_element(s_win.dt.begin(), s_win.dt.end());
        dtMax = *std::max_element(s_win.dt.begin(), s_win.dt.end());
    }
    char cap[32];
    if (s_cap > 0.0) _snprintf(cap, sizeof(cap), "fps_cap %.2f", s_cap);
    else strcpy(cap, "fps_cap off");
    TimLog("%s: %u frames in %.2f s = %.2f fps, frame period median %.2f ms, engine DT median %.2f ms (min %.2f, "
           "max %.2f), average wait %.2f ms, %u frames over budget", cap, s_win.frames, secs,
           secs > 0.0 ? s_win.frames / secs : 0.0, Median(s_win.period) * 1000.0f, Median(s_win.dt) * 1000.0f,
           dtMin * 1000.0f, dtMax * 1000.0f, s_win.frames ? s_win.waited * 1000.0 / s_win.frames : 0.0, s_win.late);
}

void Account(double waited, bool late) {
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    double now = (double)t.QuadPart * s_invFreq;
    if (s_win.start == 0.0) {
        s_win.start = s_win.last = now;
        return;
    }
    if (s_win.dt.size() < 32768) {
        s_win.dt.push_back(*(float*)(uintptr_t)TIM_FRAME_DT);
        s_win.period.push_back((float)(now - s_win.last));
    }
    ++s_win.frames;
    if (late) ++s_win.late;
    s_win.waited += waited;
    s_win.last = now;
    if (now - s_win.start >= 10.0) {
        if (g_cfg.log) LogWindow(now);
        s_win.start = now;
        s_win.frames = s_win.late = 0;
        s_win.waited = 0.0;
        s_win.dt.clear();
        s_win.period.clear();
    }
}

// Replaces "call TIM_UpdateAfterFrame" at the end of ViD::OneFrame (same thread, no arguments, cdecl).
void __cdecl PacedAfterFrame() {
    bool outer = *(int32_t*)(uintptr_t)TIM_NESTING == 1;
    double wait = 0.0;
    bool late = false;
    if (outer && s_period > 0.0) {
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        int64_t start = *(int64_t*)(uintptr_t)TIM_FRAME_START;
        double elapsed = (double)(now.QuadPart - start) * s_invFreq;
        float paused = *(float*)(uintptr_t)TIM_PAUSED;
        late = elapsed - paused > s_period + 0.001;
        wait = WaitSeconds(elapsed, paused, s_period);
        if (wait > 0.0) {
            if (!s_timerRes) {
                timeBeginPeriod(1);
                s_timerRes = true;
            }
            WaitUntil(now.QuadPart + (int64_t)(wait * (double)s_freq));
        }
    }
    ((void (__cdecl*)())(uintptr_t)TIM_AFTER_FRAME)();
    if (outer && MediaFrozen()) {                // a mod has asked for the world to stand still (wc.game.freeze)
        *(float*)(uintptr_t)TIM_NEXT_DT = FROZEN_DT;
        *(float*)(uintptr_t)TIM_FRAME_DT = FROZEN_DT;
    }
    if (outer) {
        Account(wait, late);
        VideoTick();
    }
}

int Verify(const Image& img, std::string& rep, const FileImage* file) {
    int bad = 0;
    for (size_t i = 0; i < sizeof(kSites) / sizeof(kSites[0]); ++i) {
        const CallSite& s = kSites[i];
        uint8_t b[5] = { 0 };
        int32_t rel = 0;
        uint32_t got = 0;
        bool ok = img.Read(s.va, b, 5) && b[0] == 0xE8;
        memcpy(&rel, b + 1, 4);
        ok = ok && s.va + 5 + (uint32_t)rel == s.target;
        bool crcOk = CheckCrc(img, s.va - 0x10, 0x20, s.crc, &got);
        Report(rep, ok && crcOk, "%-40s call at %08X -> %08X, bytes around it crc %08X (want %08X)", s.what, s.va,
               s.target, got, s.crc);
        if (!(ok && crcOk)) ++bad;
    }
    for (size_t i = 0; i < sizeof(kCode) / sizeof(kCode[0]); ++i) {
        const Code& c = kCode[i];
        uint32_t got = 0;
        bool ok = CheckCrc(img, c.va, c.len, c.crc, &got);
        Report(rep, ok, "%-40s %08X len 0x%03X crc %08X (want %08X)", c.what, c.va, c.len, got, c.crc);
        if (!ok) ++bad;
    }
    if (file) {                                      // the hooked call is the only way the frame window closes
        const size_t n = sizeof(kSites) / sizeof(kSites[0]);
        int count[3] = { 0, 0, 0 };
        for (size_t s = 0; s < file->secs.size(); ++s) {
            const FileImage::Sec& sec = file->secs[s];
            if (!sec.code || sec.rptr + sec.rsize > file->data.size() || sec.rsize < 5) continue;
            const uint8_t* p = &file->data[sec.rptr];
            for (uint32_t k = 0; k + 5 <= sec.rsize; ++k) {
                if (p[k] != 0xE8 && p[k] != 0xE9) continue;
                int32_t rel;
                memcpy(&rel, p + k + 1, 4);
                uint32_t dst = sec.va + k + 5 + (uint32_t)rel;
                for (size_t i = 0; i < n; ++i)
                    if (dst == kSites[i].target) ++count[i];
            }
        }
        for (size_t i = 0; i < n; ++i) {
            bool ok = count[i] == 1;
            Report(rep, ok, "%-40s %08X has %d call/jmp reference(s) in code (want 1)", kSites[i].what,
                   kSites[i].target, count[i]);
            if (!ok) ++bad;
        }
    }
    return bad;
}

bool IsOn(const std::string& v) {
    return _stricmp(v.c_str(), "1") == 0 || _stricmp(v.c_str(), "true") == 0 || _stricmp(v.c_str(), "yes") == 0 ||
           _stricmp(v.c_str(), "on") == 0;
}

// ---------------------------------------------------------------------------------------------------------------
// self-test (wmtest timing)
// ---------------------------------------------------------------------------------------------------------------
#define TIM_EXPECT(cond, ...) do { bool ok_ = (cond); Report(rep, ok_, __VA_ARGS__); if (!ok_) ++fails; } while (0)

inline bool Near(double a, double b, double eps) { return fabs(a - b) <= eps; }

// The engine's frame loop on a virtual clock: input poll/message pump (outside the window), work, the pacer, then
// TIM_UpdateAfterFrame's DT = clamp(end - start - paused, 0.001, 0.48).
struct SimResult { std::vector<float> dt; double seconds; };

SimResult Simulate(double period, const std::vector<double>& work, double outside, double paused) {
    SimResult r;
    double t = 0.0;
    for (size_t i = 0; i < work.size(); ++i) {
        t += outside;
        double start = t;
        t += work[i] + paused;
        t += WaitSeconds(t - start, paused, period);
        double dt = (t - start) - paused;
        if (dt < 0.001) dt = 0.001;
        if (dt > 0.48) dt = 0.48;
        r.dt.push_back((float)dt);
    }
    r.seconds = t;
    return r;
}

int RunTimingSelfTest(std::string& rep) {
    int fails = 0;
    const double T = 1.0 / 60.0;
    s_testLog = &rep;

    TIM_EXPECT(Near(WaitSeconds(0.001, 0.0, T), T - 0.001, 1e-12), "1 ms of work at 60 Hz waits %.4f ms (want %.4f)",
               WaitSeconds(0.001, 0.0, T) * 1000.0, (T - 0.001) * 1000.0);
    TIM_EXPECT(WaitSeconds(0.020, 0.0, T) == 0.0 && WaitSeconds(T, 0.0, T) == 0.0,
               "a frame at or over the period waits 0");
    TIM_EXPECT(Near(WaitSeconds(0.010, 0.005, T), T - 0.005, 1e-12),
               "paused time is not counted as DT: 10 ms elapsed with 5 ms paused waits %.4f ms",
               WaitSeconds(0.010, 0.005, T) * 1000.0);
    TIM_EXPECT(WaitSeconds(0.0, 5.0, T) == T && WaitSeconds(-1.0, 0.0, T) == T && WaitSeconds(NAN, 0.0, T) == 0.0 &&
               WaitSeconds(0.001, 0.0, 0.0) == 0.0,
               "the wait is bounded: never above one period (stale start, huge pause), 0 for NaN or no cap");

    const int64_t f = 10000000;                      // 100 ns ticks
    TIM_EXPECT(SleepMs((int64_t)(0.01567 * f), (int64_t)(0.0015 * f), f) == 14 &&
               SleepMs((int64_t)(0.0014 * f), (int64_t)(0.0015 * f), f) == 0 &&
               SleepMs((int64_t)(0.0024 * f), (int64_t)(0.0015 * f), f) == 0,
               "sleep plan: 15.67 ms left -> Sleep(14) then spin; under 2.5 ms left -> spin only");

    std::vector<float> m;
    m.push_back(3.0f);
    m.push_back(1.0f);
    m.push_back(2.0f);
    std::vector<float> m2 = m;
    m2.push_back(10.0f);
    TIM_EXPECT(Median(m) == 2.0f && Median(m2) == 3.0f && Median(std::vector<float>()) == 0.0f,
               "median helper: {3,1,2} -> 2, {3,1,2,10} -> 3, {} -> 0");

    std::vector<double> work(600, 0.001);
    work[300] = 0.050;                               // one 50 ms stall
    SimResult sim = Simulate(T, work, 0.0003, 0.0);
    float med = Median(sim.dt);
    bool burst = false;
    for (size_t i = 301; i < sim.dt.size(); ++i)
        if (sim.dt[i] < T - 1e-6) burst = true;
    TIM_EXPECT(Near(med, T, 1e-6) && Near(sim.dt[300], 0.050, 1e-6) && Near(sim.dt[301], T, 1e-6) && !burst,
               "simulated 600 frames (1 ms work, 0.3 ms outside the window, one 50 ms stall): DT median %.4f ms, "
               "stall frame %.2f ms, next frame %.4f ms, no short frames after it", med * 1000.0f,
               sim.dt[300] * 1000.0f, sim.dt[301] * 1000.0f);
    double rate = 600.0 / sim.seconds;
    TIM_EXPECT(rate > 58.0 && rate < 60.0, "simulated wall rate %.2f fps (period + the 0.3 ms outside the window)",
               rate);
    SimResult vsync = Simulate(T, std::vector<double>(120, 0.005), 0.0002, 0.002);
    TIM_EXPECT(Near(Median(vsync.dt), T, 1e-6),
               "5 ms frames (vsync at 200 Hz) with 2 ms paused per frame: DT median %.4f ms",
               Median(vsync.dt) * 1000.0f);
    SimResult slow = Simulate(T, std::vector<double>(60, 0.025), 0.0003, 0.0);
    TIM_EXPECT(Near(Median(slow.dt), 0.025, 1e-6), "25 ms frames are not slowed further: DT median %.2f ms",
               Median(slow.dt) * 1000.0f);

    // real clock: 30 paced frames with no work
    LARGE_INTEGER qf;
    QueryPerformanceFrequency(&qf);
    int64_t oldFreq = s_freq;
    s_freq = qf.QuadPart;
    timeBeginPeriod(1);
    std::vector<float> periods;
    LARGE_INTEGER a, b;
    QueryPerformanceCounter(&a);
    for (int i = 0; i < 30; ++i) {
        QueryPerformanceCounter(&b);
        double elapsed = (double)(b.QuadPart - a.QuadPart) / (double)s_freq;
        WaitUntil(b.QuadPart + (int64_t)(WaitSeconds(elapsed, 0.0, T) * (double)s_freq));
        LARGE_INTEGER c;
        QueryPerformanceCounter(&c);
        periods.push_back((float)((double)(c.QuadPart - a.QuadPart) / (double)s_freq));
        a = c;
    }
    timeEndPeriod(1);
    s_freq = oldFreq;
    float pm = Median(periods);
    float pmax = *std::max_element(periods.begin(), periods.end());
    TIM_EXPECT(fabs(pm - T) < 0.0005 && pmax < T + 0.004,
               "real clock, 30 paced frames: period median %.3f ms (want 16.667 +- 0.5), max %.3f ms", pm * 1000.0f,
               pmax * 1000.0f);

    s_testLog = NULL;
    return fails;
}

void CopyOut(const std::string& rep, char* out, int outSize) {
    if (!out || outSize <= 0) return;
    size_t n = rep.size() < (size_t)outSize - 1 ? rep.size() : (size_t)outSize - 1;
    memcpy(out, rep.c_str(), n);
    out[n] = 0;
}

}  // namespace

// ---------------------------------------------------------------------------------------------------------------
// entry points
// ---------------------------------------------------------------------------------------------------------------
void TimingAttach(const std::string& iniPath) {
    s_inAttach = true;
    std::string v;
    double cap = 60.0;
    if (ReadIniKey(iniPath, "timing", "fps_cap", v) && !v.empty()) cap = atof(v.c_str());
    if (!(cap >= 10.0)) cap = 0.0;
    if (cap > 1000.0) cap = 1000.0;
    bool log = ReadIniKey(iniPath, "general", "log", v) && IsOn(v);
    s_cap = s_iniCap = cap;
    s_period = cap > 0.0 ? 1.0 / cap : 0.0;
    if (cap == 0.0 && !log) {
        TimLog("[timing] fps_cap=0: frame pacing off, the engine is left untouched");
        s_inAttach = false;
        return;
    }
    ProcessImage img;
    uint8_t probe[5];
    if (!img.Read(CALL_AFTER_FRAME, probe, 5)) {
        TimLog("host process is not the RGH PC executable (no code at %08X): frame pacing not installed",
               CALL_AFTER_FRAME);
        s_inAttach = false;
        return;
    }
    std::string rep;
    int bad = Verify(img, rep, NULL);
    if (bad) {
        TimLog("the PC executable differs from the expected 2010 build in %d place(s): frame pacing NOT installed", bad);
        size_t pos = 0;
        while (pos < rep.size()) {
            size_t end = rep.find('\n', pos);
            std::string line = rep.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
            if (line.find("FAIL") != std::string::npos) TimLog("%s", line.c_str());
            pos = end == std::string::npos ? rep.size() : end + 1;
        }
        s_inAttach = false;
        return;
    }
    LARGE_INTEGER qf;
    QueryPerformanceFrequency(&qf);
    s_freq = qf.QuadPart > 0 ? qf.QuadPart : 1;
    s_invFreq = 1.0 / (double)s_freq;
    int32_t rel = (int32_t)((uint32_t)(uintptr_t)&PacedAfterFrame - (CALL_AFTER_FRAME + 5));
    s_installed = WriteCode(CALL_AFTER_FRAME + 1, &rel, 4);
    if (!s_installed) TimLog("cannot patch %08X (error %lu): frame pacing not installed", CALL_AFTER_FRAME, GetLastError());
    else if (cap > 0.0)
        TimLog("PC executable verified: the call to TIM_UpdateAfterFrame at %08X paces frames to %.2f Hz (period %.3f "
               "ms, waited inside the engine's DT window)", CALL_AFTER_FRAME, cap, s_period * 1000.0);
    else
        TimLog("PC executable verified: fps_cap=0, the call at %08X only measures frames (log=1)", CALL_AFTER_FRAME);
    s_inAttach = false;
}

void TimingAfterConfig() {
    for (size_t i = 0; i < s_pending.size(); ++i) Log("TIMING: %s", s_pending[i].c_str());
    s_pending.clear();
}

bool TimingInstalled() { return s_installed; }
double TimingCap() { return s_cap; }
double TimingIniCap() { return s_iniCap; }

// the Options screen's FRAME RATE: the pacer reads the period at every frame (0 = uncapped)
void TimingSetCap(double hz) {
    if (!(hz >= 10.0)) hz = 0.0;
    if (hz > 1000.0) hz = 1000.0;
    if (hz == s_cap) return;
    s_cap = hz;
    s_period = hz > 0.0 ? 1.0 / hz : 0.0;
    if (hz > 0.0) TimLog("frame rate cap %.2f Hz (Options screen)", hz);
    else TimLog("frame rate uncapped (Options screen): the engine's DT follows the real frame time");
}

extern "C" {

// wmtest only: verify the frame pacing hook site against an exe file. Returns the number of differences (-1: unreadable).
int __cdecl WiimoteTimingCheckExe(const char* exePath, char* out, int outSize) {
    FileImage img;
    std::string rep;
    int bad = -1;
    if (!img.Load(exePath)) rep = "  FAIL  cannot read " + std::string(exePath ? exePath : "(null)") + "\n";
    else bad = Verify(img, rep, &img);
    CopyOut(rep, out, outSize);
    return bad;
}

// wmtest only: the pacing math, a simulated engine loop and a short real-clock run. Returns the number of failures.
int __cdecl WiimoteTimingSelfTest(char* out, int outSize) {
    std::string rep;
    int fails = RunTimingSelfTest(rep);
    CopyOut(rep, out, outSize);
    return fails;
}

}  // extern "C"
