// wm_audio.cpp - the sequence-set sound fix ([audio] keep_pcm=1, on by default), sound diagnostics and an optional
// Doppler speed filter ([audio] doppler=engine|wii, doppler_rate=60, doppler_log=0).
//
// keep_pcm: a level's sounds come from its sound bins through the load stream (binary loading).  A RAM sample's
// header is read with its file in state 2 (its data is in the stream, 004A5C60); the sample's data block (004A7000 ->
// 0049A860 -> SLib_file_S slot 10, 004BBD10) decodes it (004A6630: the PCM at file +6Ch), fills its static
// DirectSound buffer and frees the PCM (004A5890, called at 004BC042).  After the stream is closed the sets are
// resolved (004996B0 -> SLib_set slot 7, 004C8910 -> 004C8700), and a set of type 2, 3 or 8 - samples played one
// after the other through a 48 kHz ring, like the taxi in hub 3 (horn, horn, engine start, engine loop) - asks each
// of its samples for its PCM again.  The PCM is gone, so the sample is decoded again, and its reads go through a read
// handle that a state-2 file never opened: BIG_S_P4::b_ReadDynAccess reads the start of the first shadow BF
// (RGH_WC.wii.sns.bf: its header and file table).  Measured in hub 3 on 2026-09-27: every such decode equals the
// ADPCM decode of that file's first bytes - short samples came out silent (the taxi's horn), longer ones full-scale
// noise (the buzz near the taxi, the helicopter, the bombs).  The samples exist only in the bins, so there is nothing
// to read them from again.  The fix: at 004BC042 the PCM is kept when a set of type 2, 3 or 8 (the type the resolve
// will give it: its template's unless its override bit 7 is set) has the sample as a child, or when no static buffer
// could be made (the ring plays it then).  The resolve finds the PCM decoded from the stream and the sets play what
// they play on the Wii.  A decode that would still read through an unopened handle is answered with silence and
// logged.  keep_pcm=0 leaves the engine alone.
//
// SLib measures each sound source's velocity once per game frame, (position - previous position) / DT, in the source
// update (004ACE90, the block 004ACF74..004AD041); the Doppler insert turns it into a pitch factor (004C03B0:
// 1 + scale x approach speed / (300 - listener speed), clamped to the insert's range; 004C0620 multiplies the voice's
// pitch by it) and "user" inserts with input 2 read its length.  The listener's velocity gets an acceleration limit
// (0049D2E0, manager +0x9EC); a source's does not.  On the Wii this runs at 60 Hz; uncapped on the PC at hundreds of
// frames per second with a DT of 0.5-3 ms.  Measured in hub 3 on 2026-09-26 at 220-250 fps: the Doppler factors move
// by 0.1-0.7 % from one frame to the next (not audible), so the engine is left alone by default.
//
// doppler=wii measures the velocity over at least one Wii frame (1 / doppler_rate s of game time): the previous
// position is kept until that much DT has passed, then velocity = (position - kept position) / time; at 60 Hz or less
// every frame is a measurement and the result is the engine's own.  A measurement faster than JUMP_SPEED is a jump (a
// new source's first position is the world origin, the next one its object's), not motion: velocity 0.
//
// doppler_log=1 (diagnostics) logs once per second: the sources started per sound set and the ones near the listener,
// the Doppler sources (speed, factor range, frame-to-frame steps; factors over 1.25 or under 0.8 with the source and
// listener state), the ngcadpcm decode calls (alignment of the PCM offsets and lengths), and the DirectSound calls
// SLib makes (SetVolume / SetFrequency / SetPan / SetCurrentPosition / Play / Stop per buffer, the buffer's sound set,
// frequencies of 60 kHz and more with the buffer's format).
#include "wiimote.h"

#include <mmsystem.h>
#include <mmreg.h>

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <unordered_map>
#include <unordered_set>

using namespace wmpatch;

namespace {

bool IsOn(const std::string& v) {
    return v == "1" || _stricmp(v.c_str(), "on") == 0 || _stricmp(v.c_str(), "true") == 0 ||
           _stricmp(v.c_str(), "yes") == 0;
}

// ---------------------------------------------------------------------------------------------------------------
// the PC executable (2010 build)
// ---------------------------------------------------------------------------------------------------------------
const uint32_t SRC_UPDATE      = 0x004ACE90;       // source update (thiscall, arg: the sound manager)
const uint32_t VEL_BLOCK       = 0x004ACF74;       // mov eax, [esi+8] ... the velocity block (replaced)
const uint32_t VEL_BLOCK_END   = 0x004AD041;       // cmp [esi+220h], edx: the voices' loop
const uint32_t DOPPLER_CALL    = 0x004C06B0;       // Doppler insert update: call DopplerFactor (hooked for the log)
const uint32_t DOPPLER_FACTOR  = 0x004C03B0;       // float thiscall(source, listener, float* 1/dist, float* speed)
const uint32_t SRC_FLAGS       = 0x008;            // source: flags, 0x2000000 = has a previous position
const uint32_t SRC_POS         = 0x0D8;            // source: position (3 floats)
const uint32_t SRC_PREV        = 0x124;            // source: previous position
const uint32_t SRC_VEL         = 0x130;            // source: velocity (what the Doppler insert reads)
const uint32_t SRC_MANAGER     = 0x004;            // source: the sound manager
const uint32_t MGR_DT          = 0x9D0;            // manager: this frame's DT (TIM, copied at the manager update)
const uint32_t MGR_DOPPLER     = 0x9E0;            // manager: global Doppler scale
const uint32_t FLAG_HAS_PREV   = 0x2000000;
const float    JUMP_SPEED      = 150.0f;           // m/s: faster is a jump (half the speed of sound the insert uses)
const uint32_t ADPCM_VTABLE    = 0x008A7108;       // SLib_codec_ngcadpcm_S vtable
const uint32_t ADPCM_DECODE    = 0x004C7F30;       // slot 4: uint thiscall(pcm offset, dest, pcm bytes, interleave)

// keep_pcm (see the top)
const uint32_t G_SLIB_MGR      = 0x00A6E88C;       // SLib manager*
const uint32_t MGR_OBJ_DATA    = 0x9AC;            // manager: the loaded SLib objects (element +4: the object)
const uint32_t MGR_OBJ_STRIDE  = 0x9B0;
const uint32_t MGR_OBJ_COUNT   = 0x9B8;
const uint32_t SET_VTABLE      = 0x008A68FC;       // SLib_set
const uint32_t OBJ_KEY         = 0x004;
const uint32_t OBJ_KIND        = 0x008;            // SLib object: u16, 1 = file (a RAM sample)
const uint32_t OBJ_FLAGS       = 0x00A;            // u16; a set: 1 = resolved
const uint32_t OBJ_FILE        = 0x00C;            // SLib_file_S: its file
const uint32_t OBJ_BUF_BYTES   = 0x014;            // SLib_file_S: bytes in its static buffer (0: none could be made)
const uint32_t SET_TYPE        = 0x010;            // 2, 3, 8: the samples play one after the other (a 48 kHz ring)
const uint32_t SET_COUNT       = 0x014;
const uint32_t SET_CHILDREN    = 0x018;
const uint32_t SET_TEMPLATE    = 0x0F4;
const uint32_t SET_OVERRIDE    = 0x0F8;            // bit 7 set: its own type, else its template's
const uint32_t FILE_KEY        = 0x004;
const uint32_t FILE_HANDLE     = 0x008;            // BIG_tt_DynAccess: +0 position (64 bit), +0C shadow BF index
const uint32_t FILE_PREFETCH   = 0x014;
const uint32_t FILE_STATE      = 0x01C;            // 1 open, 2 data in the load stream, 3 closed after a read
const uint32_t FILE_PCM_SIZE   = 0x068;
const uint32_t FILE_PCM        = 0x06C;
const uint32_t G_BINARY_MODE   = 0x00A9221C;       // ENG_Binary_Mode(): a load stream is open
const uint32_t FREE_PCM        = 0x004A5890;       // thiscall(file): free the decoded sound
const uint32_t FREE_PCM_CALL   = 0x004BC042;       // SLib_file_S slot 10: mov ecx, [esi+0Ch]; call FREE_PCM
const uint32_t RESOLVE_ALL     = 0x004996B0;       // thiscall(manager): resolve the loaded objects (sets: slot 7)
const uint32_t RESOLVE_CALLS[] = { 0x004EBA74, 0x004EC93C, 0x004ED2DA };   // LoadSounds, LoadList_End, LoadList_Thread

struct Code { uint32_t va, len, crc; const char* what; };
const Code kFix[] = {
    { 0x004BBD10, 0x361, 0x58C184D2, "SLib_file_S static buffer: decode, fill, free the sound at 004BC042" },
    { 0x004A5890, 0x01F, 0x81259DB5, "free a file's decoded sound (+6Ch)" },
    { 0x004A6630, 0x0B0, 0x5F1A2EC6, "a file's decoded sound: decodes all of it when +6Ch is 0" },
    { 0x004A6790, 0x100, 0xE6CD3AB2, "file read: the load stream, or the file's read handle" },
    { 0x004A5C60, 0x9D0, 0xB6BE6C9E, "file header load: state 2 = data in the load stream" },
    { 0x004C8700, 0x0F0, 0x1F305784, "set prepare: types 2, 3, 8 get their samples' decoded sound" },
    { 0x004C8910, 0x350, 0x287E3FF4, "set resolve: the template's type unless override bit 7" },
    { 0x004996B0, 0x050, 0xFEB21021, "resolve the loaded objects (manager +9ACh)" },
    { 0x008A68FC, 0x02C, 0x6754ECFB, "SLib_set vtable" },
    { 0x008A6960, 0x02C, 0x8466784B, "SLib_file_S vtable" },
    { 0x006CF0C0, 0x00B, 0x63E1BE0F, "ENG_Binary_Mode (00A9221C)" },
    { 0x004C7F30, 0x250, 0x1DD2C57C, "ngcadpcm decode" },
    { 0x008A7108, 0x02C, 0x290B5375, "SLib_codec_ngcadpcm_S vtable" },
};
const Code kCode[] = {
    { 0x004ACE90, 0x2C3, 0x6293196E, "source update (position, velocity, voices)" },
    { 0x004ACF74, 0x0CD, 0x97077A5B, "source update: velocity block 004ACF74..004AD041" },
    { 0x004C0620, 0x1B0, 0xB5A4BEF9, "Doppler insert update (call DopplerFactor at 004C06B0)" },
    { 0x004C03B0, 0x267, 0x6FEFCAB8, "Doppler factor (source velocity at +130h, speed of sound 300)" },
    { 0x004C7F30, 0x250, 0x1DD2C57C, "ngcadpcm decode (PCM offset / length in 28-byte units per channel)" },
    { 0x008A7108, 0x02C, 0x290B5375, "SLib_codec_ngcadpcm_S vtable" },
};

// ---------------------------------------------------------------------------------------------------------------
// velocity math (pure, self-tested)
// ---------------------------------------------------------------------------------------------------------------
// The engine: velocity = (pos - prev) / dt (dt 0: the difference itself), prev = pos.
void VelocityEngine(const float pos[3], float prev[3], float vel[3], float dt) {
    float inv = dt != 0.0f ? 1.0f / dt : 1.0f;
    for (int i = 0; i < 3; ++i) {
        vel[i] = (pos[i] - prev[i]) * inv;
        prev[i] = pos[i];
    }
}

// Measured over at least `window` seconds of game time: `acc` is the time since the kept position (prev).  A frame
// of `window` or more (the Wii's rate or slower) measures every frame, exactly like the engine.
void VelocityWindow(const float pos[3], float prev[3], float vel[3], float dt, float& acc, float window) {
    if (!(dt > 0.0f)) {                            // no time passed (paused): the engine's result, restart the window
        VelocityEngine(pos, prev, vel, dt);
        acc = 0.0f;
        return;
    }
    acc += dt;
    if (acc < window) return;                      // velocity and kept position stay
    float inv = 1.0f / acc, v[3];
    for (int i = 0; i < 3; ++i) {
        v[i] = (pos[i] - prev[i]) * inv;
        prev[i] = pos[i];
    }
    bool jump = v[0] * v[0] + v[1] * v[1] + v[2] * v[2] > JUMP_SPEED * JUMP_SPEED;
    for (int i = 0; i < 3; ++i) vel[i] = jump ? 0.0f : v[i];
    acc = 0.0f;
}

// ---------------------------------------------------------------------------------------------------------------
// state
// ---------------------------------------------------------------------------------------------------------------
enum Mode { MODE_ENGINE, MODE_WII };
Mode     s_mode = MODE_ENGINE;
float    s_window = 1.0f / 60.0f;                   // doppler_rate: the threshold is 90% of a period (see Attach)
float    s_rate = 60.0f;
bool     s_velInstalled, s_logInstalled, s_log;
bool     s_keepPcm = true, s_fixInstalled;
bool     s_inAttach;
std::vector<std::string> s_pending;
CRITICAL_SECTION s_cs;
std::unordered_map<uintptr_t, float> s_acc;         // source -> game time since its kept position

void AudLog(const char* fmt, ...) {
    char buf[700];
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf(buf, sizeof(buf) - 1, fmt, ap);
    buf[sizeof(buf) - 1] = 0;
    va_end(ap);
    if (s_inAttach) s_pending.push_back(buf);
    else Log("AUDIO: %s", buf);
}

// ---------------------------------------------------------------------------------------------------------------
// doppler_log: per source, per second
// ---------------------------------------------------------------------------------------------------------------
struct SrcStats {
    uintptr_t src;
    unsigned  upd, still;                          // velocity updates; updates whose position had not moved
    float     dtMin, dtMax;
    double    speedSum;
    float     speedMax;                            // |velocity| as the Doppler insert reads it
    unsigned  nf;
    double    fSum, stepSq;
    float     fMin, fMax, fLast;
    float     dist, pos[3];
};
std::unordered_map<uintptr_t, SrcStats> s_stats;
std::unordered_set<uintptr_t> s_described;
double   s_winStart;
unsigned s_windows;

struct Seen {                                      // every source updated in the window (doppler_log)
    uint32_t set, flags;
    float    pos[3], pitch, cutoff;
    unsigned upd;
};
struct Starts { unsigned n; float pos[3]; };
std::unordered_map<uintptr_t, Seen> s_seen;
std::unordered_map<uint32_t, Starts> s_starts;     // set key -> sources started in the window
uint32_t s_mgr;

bool SafeRead(uintptr_t p, void* out, size_t n) {
    __try {
        memcpy(out, (const void*)p, n);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

uint32_t SetKey(uintptr_t src) {                   // source +0C -> the sound set, +4: its key
    uint32_t set = 0, key = 0;
    if (SafeRead(src + 0xC, &set, 4) && set >= 0x10000) SafeRead(set + 4, &key, 4);
    return key;
}

bool KeyLike(uint32_t v) {                         // BIG keys of the game's sounds look like 85004931, 8E001CB4
    uint32_t hi = v >> 24;
    return hi >= 0x80 && hi <= 0x9F && (v & 0x00F00000) == 0 && (v & 0x000FFFFF) != 0;
}

// Key-like values in the source and in what its first pointers point at: which sound set / object it plays.
std::string KeysNear(uintptr_t src) {
    std::string out;
    uint32_t words[0x240 / 4];
    if (!SafeRead(src, words, sizeof(words))) return "(unreadable)";
    char item[48];
    int found = 0;
    for (int i = 0; i < 0x240 / 4 && found < 12; ++i) {
        uint32_t v = words[i];
        if (KeyLike(v)) {
            _snprintf(item, sizeof(item) - 1, " +%X=%08X", i * 4, v);
            item[sizeof(item) - 1] = 0;
            out += item;
            ++found;
            continue;
        }
        if (v < 0x10000 || (v & 3)) continue;
        uint32_t sub[0x100 / 4];
        if (!SafeRead(v, sub, sizeof(sub))) continue;
        for (int j = 0; j < 0x100 / 4 && found < 12; ++j) {
            if (!KeyLike(sub[j])) continue;
            _snprintf(item, sizeof(item) - 1, " +%X>+%X=%08X", i * 4, j * 4, sub[j]);
            item[sizeof(item) - 1] = 0;
            out += item;
            ++found;
        }
    }
    return out.empty() ? " (none)" : out;
}

void FlushSources(double secs) {
    float lp[3] = { 0, 0, 0 };
    uint32_t lst = 0;
    bool haveL = s_mgr && SafeRead(s_mgr + 0xA28, &lst, 4) && lst && SafeRead(lst + 0x1728, lp, sizeof(lp));
    std::vector<std::pair<uint32_t, Starts> > st(s_starts.begin(), s_starts.end());
    std::sort(st.begin(), st.end(), [](const std::pair<uint32_t, Starts>& a, const std::pair<uint32_t, Starts>& b) {
        return a.second.n > b.second.n;
    });
    std::string line;
    char item[96];
    for (size_t i = 0; i < st.size() && i < 12; ++i) {
        const float* p = st[i].second.pos;
        float d = sqrtf((p[0] - lp[0]) * (p[0] - lp[0]) + (p[1] - lp[1]) * (p[1] - lp[1]) + (p[2] - lp[2]) * (p[2] - lp[2]));
        _snprintf(item, sizeof(item) - 1, " %08X x%u (%.0f m)", st[i].first, st[i].second.n, d);
        item[sizeof(item) - 1] = 0;
        line += item;
    }
    AudLog("sources: %.2f s, %u updated, %u started; listener %s(%.1f, %.1f, %.1f); starts per set:%s", secs,
           (unsigned)s_seen.size(), (unsigned)[&] { unsigned n = 0; for (auto& x : st) n += x.second.n; return n; }(),
           haveL ? "" : "? ", lp[0], lp[1], lp[2], line.empty() ? " none" : line.c_str());
    std::vector<std::pair<float, std::pair<uintptr_t, Seen> > > nearby;
    for (auto& kv : s_seen) {
        const float* p = kv.second.pos;
        float d = sqrtf((p[0] - lp[0]) * (p[0] - lp[0]) + (p[1] - lp[1]) * (p[1] - lp[1]) + (p[2] - lp[2]) * (p[2] - lp[2]));
        if (d < 25.0f) nearby.push_back(std::make_pair(d, kv));
    }
    std::sort(nearby.begin(), nearby.end(), [](const std::pair<float, std::pair<uintptr_t, Seen> >& a,
                                           const std::pair<float, std::pair<uintptr_t, Seen> >& b) { return a.first < b.first; });
    for (size_t i = 0; i < nearby.size() && i < 16; ++i) {
        const Seen& z = nearby[i].second.second;
        AudLog("  near: src %08X set %08X %.1f m at (%.1f, %.1f, %.1f): %u updates, flags %08X, pitch %.3f, cutoff %.0f",
               (unsigned)nearby[i].second.first, z.set, nearby[i].first, z.pos[0], z.pos[1], z.pos[2], z.upd, z.flags,
               z.pitch, z.cutoff);
    }
    s_seen.clear();
    s_starts.clear();
}

void FlushDecodes();
void FlushDs();

void FlushWindow(double now) {
    double secs = now - s_winStart;
    s_winStart = now;
    FlushSources(secs);
    FlushDecodes();
    FlushDs();
    if (s_stats.empty()) return;
    std::vector<const SrcStats*> v;
    for (auto& kv : s_stats) v.push_back(&kv.second);
    auto rms = [](const SrcStats* s) { return s->nf > 1 ? sqrt(s->stepSq / (s->nf - 1)) : 0.0; };
    std::sort(v.begin(), v.end(), [&](const SrcStats* a, const SrcStats* b) { return rms(a) > rms(b); });
    ++s_windows;
    AudLog("doppler: %.2f s, %u source(s) with a Doppler insert, largest frame-to-frame factor step (rms) %.5f",
           secs, (unsigned)v.size(), rms(v[0]));
    for (size_t i = 0; i < v.size() && i < 4; ++i) {
        const SrcStats* s = v[i];
        AudLog("  src %08X at (%.1f, %.1f, %.1f) %.1f m away: %u updates, %u still, dt %.2f..%.2f ms, speed mean "
               "%.2f max %.2f m/s; factor %u x mean %.4f range %.4f..%.4f step rms %.5f",
               (unsigned)s->src, s->pos[0], s->pos[1], s->pos[2], s->dist, s->upd, s->still, s->dtMin * 1000.0f,
               s->dtMax * 1000.0f, s->upd ? s->speedSum / s->upd : 0.0, s->speedMax, s->nf,
               s->nf ? s->fSum / s->nf : 0.0, s->fMin, s->fMax, rms(s));
    }
    s_stats.clear();
}

void MaybeFlush() {                               // inside s_cs
    LARGE_INTEGER qc, qf;
    QueryPerformanceCounter(&qc);
    QueryPerformanceFrequency(&qf);
    double now = (double)qc.QuadPart / (double)qf.QuadPart;
    if (s_winStart == 0.0) s_winStart = now;
    else if (now - s_winStart >= 1.0) FlushWindow(now);
}

SrcStats& Stats(uintptr_t src) {
    auto it = s_stats.find(src);
    if (it != s_stats.end()) return it->second;
    SrcStats s;
    memset(&s, 0, sizeof(s));
    s.src = src;
    s.dtMin = 1e9f;
    s.fMin = 1e9f;
    s.fMax = -1e9f;
    return s_stats.emplace(src, s).first->second;
}

// ---------------------------------------------------------------------------------------------------------------
// keep_pcm: the samples of sets that play them one after the other keep their decoded sound (see the top)
// ---------------------------------------------------------------------------------------------------------------
unsigned      s_kept, s_keptNoBuffer, s_silenced;
uint64_t      s_keptBytes;
volatile LONG s_resolving, s_resolveDecodes;
std::unordered_set<uint32_t> s_silencedFiles;       // logged once each

// The type a set has once it is resolved (004C8910): its own when resolved already or when its override bit 7 is
// set, else its template's.  Reads without checks: the callers catch the exceptions.
int SetTypeResolved(uint32_t set) {
    for (int depth = 0; set && depth < 8; ++depth) {
        uint32_t type = *(const uint32_t*)(uintptr_t)(set + SET_TYPE);
        if (*(const uint16_t*)(uintptr_t)(set + OBJ_FLAGS) & 1) return (int)type;
        uint32_t tpl = *(const uint32_t*)(uintptr_t)(set + SET_TEMPLATE);
        if (!tpl || (*(const uint32_t*)(uintptr_t)(set + SET_OVERRIDE) & 0x80) ||
            *(const uint32_t*)(uintptr_t)tpl != SET_VTABLE)
            return (int)type;
        set = tpl;
    }
    return -1;
}

bool SequenceType(int t) { return t == 2 || t == 3 || t == 8; }

// Is `file` the file of a RAM sample (SLib_file_S) that a set of type 2, 3 or 8 in the manager's list plays?
bool SequenceChild(uint32_t mgr, uint32_t file, uint32_t* setKey, int* setType) {
    __try {
        uint32_t data = *(const uint32_t*)(uintptr_t)(mgr + MGR_OBJ_DATA);
        uint32_t stride = *(const uint32_t*)(uintptr_t)(mgr + MGR_OBJ_STRIDE);
        uint32_t count = *(const uint32_t*)(uintptr_t)(mgr + MGR_OBJ_COUNT);
        if (!data || stride < 8 || count > 200000) return false;
        for (uint32_t i = 0; i < count; ++i) {
            uint32_t set = *(const uint32_t*)(uintptr_t)(data + stride * i + 4);
            if (!set || *(const uint32_t*)(uintptr_t)set != SET_VTABLE) continue;
            int type = SetTypeResolved(set);
            if (!SequenceType(type)) continue;
            uint32_t n = *(const uint32_t*)(uintptr_t)(set + SET_COUNT);
            uint32_t kids = *(const uint32_t*)(uintptr_t)(set + SET_CHILDREN);
            if (!kids || n > 4096) continue;
            for (uint32_t k = 0; k < n; ++k) {
                uint32_t obj = *(const uint32_t*)(uintptr_t)(kids + 4 * k);
                if (obj && *(const uint16_t*)(uintptr_t)(obj + OBJ_KIND) == 1 &&
                    *(const uint32_t*)(uintptr_t)(obj + OBJ_FILE) == file) {
                    *setKey = *(const uint32_t*)(uintptr_t)(set + OBJ_KEY);
                    *setType = type;
                    return true;
                }
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return false;
}

// The sets of type 2, 3 or 8 in the manager's list and their RAM samples (for the log).
void CountSequenceSets(uint32_t mgr, unsigned* sets, unsigned* samples) {
    __try {
        uint32_t data = *(const uint32_t*)(uintptr_t)(mgr + MGR_OBJ_DATA);
        uint32_t stride = *(const uint32_t*)(uintptr_t)(mgr + MGR_OBJ_STRIDE);
        uint32_t count = *(const uint32_t*)(uintptr_t)(mgr + MGR_OBJ_COUNT);
        if (!data || stride < 8 || count > 200000) return;
        for (uint32_t i = 0; i < count; ++i) {
            uint32_t set = *(const uint32_t*)(uintptr_t)(data + stride * i + 4);
            if (!set || *(const uint32_t*)(uintptr_t)set != SET_VTABLE || !SequenceType(SetTypeResolved(set)))
                continue;
            ++*sets;
            uint32_t n = *(const uint32_t*)(uintptr_t)(set + SET_COUNT);
            uint32_t kids = *(const uint32_t*)(uintptr_t)(set + SET_CHILDREN);
            for (uint32_t k = 0; kids && n <= 4096 && k < n; ++k) {
                uint32_t obj = *(const uint32_t*)(uintptr_t)(kids + 4 * k);
                if (obj && *(const uint16_t*)(uintptr_t)(obj + OBJ_KIND) == 1 &&
                    *(const uint32_t*)(uintptr_t)(obj + OBJ_FILE))
                    ++*samples;
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

// A file whose data came through the load stream (state 2) never opened its read handle: once the stream is closed
// (binaryFlag 0), its reads come from the start of the first shadow BF.
bool StaleStreamFile(uint32_t file, uint32_t binaryFlag) {
    uint32_t state = 0, binary = 1, prefetch = 1, pos[2] = { 1, 1 };
    return SafeRead(file + FILE_STATE, &state, 4) && state == 2 && SafeRead(binaryFlag, &binary, 4) && !binary &&
           SafeRead(file + FILE_PREFETCH, &prefetch, 4) && !prefetch && SafeRead(file + FILE_HANDLE, pos, 8) &&
           !pos[0] && !pos[1];
}

typedef void(__fastcall* ThisNoArgsFn)(uint32_t self);    // a thiscall function without arguments

// Replaces the free at 004BC042 (through FreeThunk): the file's decoded sound stays when a sequence set plays the
// sample or when no static buffer could be made; everything else is freed as before.
void __stdcall FreeAfterStatic(uint32_t file, uint32_t obj) {
    uint32_t pcm = 0, bytes = 1, mgr = 0, setKey = 0;
    int setType = 0;
    SafeRead(file + FILE_PCM, &pcm, 4);
    SafeRead(obj + OBJ_BUF_BYTES, &bytes, 4);
    SafeRead(G_SLIB_MGR, &mgr, 4);
    bool seq = pcm && mgr && SequenceChild(mgr, file, &setKey, &setType);
    if (!pcm || (!seq && bytes)) {
        ((ThisNoArgsFn)(uintptr_t)FREE_PCM)(file);
        return;
    }
    uint32_t size = 0, key = 0;
    SafeRead(file + FILE_PCM_SIZE, &size, 4);
    SafeRead(file + FILE_KEY, &key, 4);
    EnterCriticalSection(&s_cs);
    ++s_kept;
    if (!seq) ++s_keptNoBuffer;
    s_keptBytes += size;
    LeaveCriticalSection(&s_cs);
    if (!s_log) return;
    if (seq) AudLog("kept the decoded sound of sample %08X (file %08X, %u bytes): set %08X (type %d) plays it", key, file,
                    size, setKey, setType);
    else AudLog("kept the decoded sound of sample %08X (file %08X, %u bytes): no static buffer could be made", key, file,
                size);
}

__declspec(naked) void FreeThunk() {              // at 004BC042: ecx = the file, esi = its SLib_file_S
    __asm {
        push esi
        push ecx
        call FreeAfterStatic
        ret
    }
}

// A decode that would read through an unopened handle (see StaleStreamFile) gets silence instead of the BF's header.
uint32_t Silence(uint32_t file, void* dest, uint32_t len) {
    memset(dest, 0, len);
    uint32_t key = 0;
    SafeRead(file + FILE_KEY, &key, 4);
    EnterCriticalSection(&s_cs);
    ++s_silenced;
    bool first = s_silencedFiles.insert(file).second;
    LeaveCriticalSection(&s_cs);
    if (first)
        AudLog("sample %08X (file %08X) was asked for its sound after the level's sound data was read: silence (the "
               "engine would decode the start of the sound BF: noise)", key, file);
    return len;
}

// After each resolve pass (the calls at RESOLVE_CALLS): one line about the sequence sets.
void __fastcall ResolveHook(uint32_t mgr) {
    InterlockedIncrement(&s_resolving);
    ((ThisNoArgsFn)(uintptr_t)RESOLVE_ALL)(mgr);
    InterlockedDecrement(&s_resolving);
    unsigned sets = 0, samples = 0;
    CountSequenceSets(mgr, &sets, &samples);
    EnterCriticalSection(&s_cs);
    unsigned kept = s_kept, noBuffer = s_keptNoBuffer, silenced = s_silenced;
    unsigned again = (unsigned)InterlockedExchange(&s_resolveDecodes, 0);
    double kb = (double)s_keptBytes / 1024.0;
    s_kept = s_keptNoBuffer = s_silenced = 0;
    s_keptBytes = 0;
    s_silencedFiles.clear();
    LeaveCriticalSection(&s_cs);
    if (!kept && !silenced && !again && !s_log) return;
    AudLog("sound load: %u sets play their samples one after the other (types 2, 3, 8; %u samples loaded): %u samples "
           "kept their decoded sound from the load (%.0f KB, %u of them without a static buffer), %u decoded again by "
           "the resolve, %u answered with silence", sets, samples, kept, kb, noBuffer, again, silenced);
}

struct DecStats {                                  // one ngcadpcm codec (one playing sample or stream)
    unsigned calls, misOff, misLen, overshoot, shortRet, gaps;
    uint32_t ch, dataSize, lastEnd, minLen, maxLen, flag;
    uint64_t bytes;
};
std::unordered_map<uintptr_t, DecStats> s_dec;

typedef uint32_t(__thiscall* DecodeFn)(void* self, uint32_t off, void* dest, uint32_t len, int flag);

void DumpDecode(uint32_t codec, uint32_t file, uint32_t ch, uint32_t dataSize, uint32_t off, const void* dest,
                uint32_t len, uint32_t got, int flag);

uint32_t __fastcall DecodeHook(void* self, void*, uint32_t off, void* dest, uint32_t len, int flag) {
    uint32_t file = 0;
    SafeRead((uintptr_t)self + 4, &file, 4);
    if (s_fixInstalled && file && dest && StaleStreamFile(file, G_BINARY_MODE)) return Silence(file, dest, len);
    uint32_t got = ((DecodeFn)(uintptr_t)ADPCM_DECODE)(self, off, dest, len, flag);
    if (s_resolving && !flag && !off) InterlockedIncrement(&s_resolveDecodes);
    if (!s_log) return got;
    uint32_t ch = 1, dataSize = 0;
    uint16_t c16 = 0;
    if (SafeRead(file + 0x42, &c16, 2) && c16) ch = c16;
    SafeRead(file + 0x58, &dataSize, 4);
    DumpDecode((uint32_t)(uintptr_t)self, file, ch, dataSize, off, dest, len, got, flag);
    EnterCriticalSection(&s_cs);
    DecStats& d = s_dec[(uintptr_t)self];
    if (!d.calls) { d.minLen = 0xFFFFFFFF; d.ch = ch; d.dataSize = dataSize; d.flag = (uint32_t)flag; }
    uint32_t unit = 28 * ch;
    if (d.calls && off != d.lastEnd) ++d.gaps;
    ++d.calls;
    if (off % unit) ++d.misOff;
    if (len % unit) ++d.misLen;
    if (got > len) ++d.overshoot;
    if (got < len) ++d.shortRet;
    d.lastEnd = off + got;
    d.minLen = std::min(d.minLen, len);
    d.maxLen = std::max(d.maxLen, len);
    d.bytes += got;
    LeaveCriticalSection(&s_cs);
    return got;
}

void FlushDecodes() {                              // inside s_cs
    for (auto& kv : s_dec) {
        const DecStats& d = kv.second;
        if (d.calls < 2 && !d.misOff && !d.misLen && !d.overshoot) continue;   // decode-all of a RAM sample
        AudLog("  adpcm codec %08X: %u ch, %u bytes of data, interleave %u: %u calls (len %u..%u), %llu PCM bytes; "
               "offset not a multiple of %u: %u, length not: %u, returned more than asked: %u, less: %u, "
               "offset not where the last call ended: %u", (unsigned)kv.first, d.ch, d.dataSize, d.flag, d.calls,
               d.minLen, d.maxLen, (unsigned long long)d.bytes, 28 * d.ch, d.misOff, d.misLen, d.overshoot,
               d.shortRet, d.gaps);
    }
    s_dec.clear();
}

// ---------------------------------------------------------------------------------------------------------------
// doppler_log: what SLib asks of DirectSound (the secondary buffers' vtable, patched once the game created its device)
// ---------------------------------------------------------------------------------------------------------------
const uint32_t G_DSOUND = 0x00A6E920;              // IDirectSound8* (SLib's device)
struct DsDesc { DWORD size, flags, bytes, reserved; WAVEFORMATEX* fmt; GUID alg; };
typedef HRESULT(__stdcall* DsCreateFn)(void* ds, const DsDesc* d, void** buf, void* outer);
typedef ULONG(__stdcall* DsReleaseFn)(void* self);
typedef HRESULT(__stdcall* DsPosFn)(void* self, DWORD* play, DWORD* write);
typedef HRESULT(__stdcall* DsU32Fn)(void* self, DWORD v);
typedef HRESULT(__stdcall* DsPlayFn)(void* self, DWORD r, DWORD prio, DWORD flags);
typedef HRESULT(__stdcall* DsStopFn)(void* self);
enum { DS_GETPOS = 4, DS_LOCK = 11, DS_PLAY = 12, DS_SETPOS = 13, DS_SETVOL = 15, DS_SETPAN = 16, DS_SETFREQ = 17,
       DS_STOP = 18, DS_UNLOCK = 19 };
void* s_dsOrig[24];
bool s_dsHooked;
struct DsStats { unsigned vol, pan, freq, setpos, play, stop; int volMin, volMax, lastVol, maxVolStep; DWORD fMin, fMax; };
std::unordered_map<uintptr_t, DsStats> s_ds;
unsigned s_setposLogs, s_bigLogs;
struct BufOwner { uint32_t set; uintptr_t src; };
std::unordered_map<uintptr_t, BufOwner> s_bufOwner;   // DirectSound buffer -> the source / set playing it

void NoteVoices(uintptr_t src, uint32_t set) {     // source +234h: voices (0x1C8 each), voice +0 -> +4: the buffer
    uint32_t voices = 0, n = 0;
    if (!SafeRead(src + 0x234, &voices, 4) || !voices) return;
    SafeRead(src + 0x1E0, &n, 2);                  // voice count (u16 at +1E0, as FUN_004AB5B0 reads param+0x78*4)
    n &= 0xFFFF;
    if (n == 0 || n > 8) n = 1;
    for (uint32_t i = 0; i < n; ++i) {
        uint32_t plat = 0, buf = 0;
        if (SafeRead(voices + i * 0x1C8, &plat, 4) && plat && SafeRead(plat + 4, &buf, 4) && buf) {
            if (s_bufOwner.size() > 4096) s_bufOwner.clear();
            s_bufOwner[buf] = BufOwner{ set, src };
        }
    }
}

DsStats& Ds(void* b) {
    auto it = s_ds.find((uintptr_t)b);
    if (it != s_ds.end()) return it->second;
    DsStats z;
    memset(&z, 0, sizeof(z));
    z.volMin = 100000; z.volMax = -100000; z.lastVol = 1; z.fMin = 0xFFFFFFFF;
    return s_ds.emplace((uintptr_t)b, z).first->second;
}
void DumpEvent(const char* what, void* b, DWORD a, DWORD c);
HRESULT __stdcall DsSetVolume(void* b, DWORD v) {
    EnterCriticalSection(&s_cs);
    DsStats& d = Ds(b);
    ++d.vol;
    int iv = (int)v;
    if (d.lastVol != 1) d.maxVolStep = std::max(d.maxVolStep, abs(iv - d.lastVol));
    d.lastVol = iv; d.volMin = std::min(d.volMin, iv); d.volMax = std::max(d.volMax, iv);
    LeaveCriticalSection(&s_cs);
    DumpEvent("vol", b, v, 0);
    return ((DsU32Fn)s_dsOrig[DS_SETVOL])(b, v);
}
HRESULT __stdcall DsSetPan(void* b, DWORD v) {
    EnterCriticalSection(&s_cs); ++Ds(b).pan; LeaveCriticalSection(&s_cs);
    return ((DsU32Fn)s_dsOrig[DS_SETPAN])(b, v);
}
typedef HRESULT(__stdcall* DsStatusFn)(void* self, DWORD* st);
void DumpEvent(const char* what, void* b, DWORD a, DWORD c);
unsigned s_fastLogs;
HRESULT __stdcall DsSetFrequency(void* b, DWORD v) {
    DWORD st = 0, play = 0, write = 0;
    ((DsStatusFn)s_dsOrig[9])(b, &st);
    ((DsPosFn)s_dsOrig[DS_GETPOS])(b, &play, &write);
    EnterCriticalSection(&s_cs);
    DsStats& d = Ds(b); ++d.freq; d.fMin = std::min(d.fMin, v); d.fMax = std::max(d.fMax, v);
    if (v >= 60000 && s_fastLogs < 40) {
        ++s_fastLogs;
        auto ow = s_bufOwner.find((uintptr_t)b);
        WAVEFORMATEX wf;
        memset(&wf, 0, sizeof(wf));
        DWORD got = 0;
        typedef HRESULT(__stdcall* DsFmtFn)(void*, WAVEFORMATEX*, DWORD, DWORD*);
        ((DsFmtFn)s_dsOrig[5])(b, &wf, sizeof(wf), &got);
        struct { DWORD size, flags, bytes, unlockRate, cpu; } caps = { sizeof(caps), 0, 0, 0, 0 };
        typedef HRESULT(__stdcall* DsCapsFn)(void*, void*);
        ((DsCapsFn)s_dsOrig[3])(b, &caps);
        float pitch = 0.0f;
        if (ow != s_bufOwner.end()) SafeRead(ow->second.src + 0xA4, &pitch, 4);
        AudLog("  fast: buffer %08X (set %08X, source pitch %.3f) SetFrequency(%lu): buffer format %u Hz %u ch %u bit, "
               "%lu bytes, flags %lX; status %lX, play cursor %lu, last volume %d mB", (unsigned)(uintptr_t)b,
               ow != s_bufOwner.end() ? ow->second.set : 0u, pitch, (unsigned long)v, (unsigned)wf.nSamplesPerSec,
               wf.nChannels, wf.wBitsPerSample, (unsigned long)caps.bytes, (unsigned long)caps.flags,
               (unsigned long)st, (unsigned long)play, d.lastVol == 1 ? 99999 : d.lastVol);
    }
    LeaveCriticalSection(&s_cs);
    DumpEvent("freq", b, v, st);
    return ((DsU32Fn)s_dsOrig[DS_SETFREQ])(b, v);
}
HRESULT __stdcall DsSetCurrentPosition(void* b, DWORD v) {
    DWORD play = 0xFFFFFFFF, write = 0xFFFFFFFF;
    ((DsPosFn)s_dsOrig[DS_GETPOS])(b, &play, &write);
    EnterCriticalSection(&s_cs);
    ++Ds(b).setpos;
    if (v != 0 && s_setposLogs < 50) {
        ++s_setposLogs;
        AudLog("  dsound buffer %08X: SetCurrentPosition(%lu) while the play cursor is at %ld", (unsigned)(uintptr_t)b,
               (unsigned long)v, (long)play);
    }
    LeaveCriticalSection(&s_cs);
    DumpEvent("setpos", b, v, play);
    return ((DsU32Fn)s_dsOrig[DS_SETPOS])(b, v);
}
void DumpStatic(void* b, DWORD flags);
void DumpEvent(const char* what, void* b, DWORD a, DWORD c);
HRESULT __stdcall DsPlay(void* b, DWORD r, DWORD prio, DWORD flags) {
    EnterCriticalSection(&s_cs); ++Ds(b).play; LeaveCriticalSection(&s_cs);
    DumpStatic(b, flags);
    DumpEvent("play", b, flags, 0);
    return ((DsPlayFn)s_dsOrig[DS_PLAY])(b, r, prio, flags);
}
HRESULT __stdcall DsStop(void* b) {
    EnterCriticalSection(&s_cs); ++Ds(b).stop; LeaveCriticalSection(&s_cs);
    DumpEvent("stop", b, 0, 0);
    return ((DsStopFn)s_dsOrig[DS_STOP])(b);
}

// doppler_log=2: every Unlock's data, per buffer, into dsdump\ next to the DLL (index.csv: time, buffer, set, lock
// offset, bytes), at most 400 MB.
int s_logLevel;
std::string s_dumpDir;                             // [audio] dump_dir (default <DLL folder>\dsdump)
FILE* s_dumpIdx;
FILE* s_dumpDat;
uint64_t s_dumpBytes;
std::unordered_map<uintptr_t, DWORD> s_lockOff;
std::unordered_set<uintptr_t> s_rings;              // buffers SLib writes while they play (stream rings)
typedef HRESULT(__stdcall* DsLockFn)(void*, DWORD, DWORD, void**, DWORD*, void**, DWORD*, DWORD);
typedef HRESULT(__stdcall* DsUnlockFn)(void*, void*, DWORD, void*, DWORD);
HRESULT __stdcall DsLock(void* b, DWORD off, DWORD bytes, void** p1, DWORD* n1, void** p2, DWORD* n2, DWORD flags) {
    HRESULT hr = ((DsLockFn)s_dsOrig[DS_LOCK])(b, off, bytes, p1, n1, p2, n2, flags);
    EnterCriticalSection(&s_cs);
    s_lockOff[(uintptr_t)b] = (flags & 2) ? 0 : off;      // DSBLOCK_ENTIREBUFFER
    LeaveCriticalSection(&s_cs);
    return hr;
}
HRESULT __stdcall DsUnlock(void* b, void* p1, DWORD n1, void* p2, DWORD n2) {
    EnterCriticalSection(&s_cs);
    if (s_dumpIdx && s_dumpDat && s_dumpBytes < (400ull << 20)) {
        auto ow = s_bufOwner.find((uintptr_t)b);
        LARGE_INTEGER qc, qf;
        QueryPerformanceCounter(&qc);
        QueryPerformanceFrequency(&qf);
        long long at = _ftelli64(s_dumpDat);
        if (p1 && n1) fwrite(p1, 1, n1, s_dumpDat);
        if (p2 && n2) fwrite(p2, 1, n2, s_dumpDat);
        fprintf(s_dumpIdx, "%.6f,%08X,%08X,%lu,%lu,%lu,%lld\n", (double)qc.QuadPart / (double)qf.QuadPart,
                (unsigned)(uintptr_t)b, ow != s_bufOwner.end() ? ow->second.set : 0u,
                (unsigned long)s_lockOff[(uintptr_t)b], (unsigned long)n1, (unsigned long)n2, at);
        s_dumpBytes += n1 + n2;
        s_rings.insert((uintptr_t)b);
    }
    LeaveCriticalSection(&s_cs);
    return ((DsUnlockFn)s_dsOrig[DS_UNLOCK])(b, p1, n1, p2, n2);
}

// doppler_log=2: Play / Stop / SetCurrentPosition per buffer ("evt" lines) and, once per second, the status and play
// cursor of every buffer SLib has written to while playing ("state" lines: status 1 playing, 4 looping).
void DumpEvent(const char* what, void* b, DWORD a, DWORD c) {
    if (s_logLevel < 2) return;
    LARGE_INTEGER qc, qf;
    QueryPerformanceCounter(&qc);
    QueryPerformanceFrequency(&qf);
    EnterCriticalSection(&s_cs);
    if (s_dumpIdx) fprintf(s_dumpIdx, "evt,%.6f,%08X,%s,%lu,%lu\n", (double)qc.QuadPart / (double)qf.QuadPart,
                           (unsigned)(uintptr_t)b, what, (unsigned long)a, (unsigned long)c);
    LeaveCriticalSection(&s_cs);
}

void DumpRingStates() {                            // inside s_cs
    if (s_logLevel < 2 || !s_dumpIdx) return;
    LARGE_INTEGER qc, qf;
    QueryPerformanceCounter(&qc);
    QueryPerformanceFrequency(&qf);
    for (uintptr_t b : s_rings) {
        DWORD st = 0, play = 0, write = 0;
        if (FAILED(((DsStatusFn)s_dsOrig[9])((void*)b, &st))) continue;
        ((DsPosFn)s_dsOrig[DS_GETPOS])((void*)b, &play, &write);
        fprintf(s_dumpIdx, "state,%.6f,%08X,%lu,%lu,%lu\n", (double)qc.QuadPart / (double)qf.QuadPart, (unsigned)b,
                (unsigned long)st, (unsigned long)play, (unsigned long)write);
    }
}

// doppler_log=2: each ngcadpcm decode call (codec, file, channels, rate, data size, PCM offset / length / returned,
// the file's first key-like word) and its output: "decode" lines in index.csv.
void DumpDecode(uint32_t codec, uint32_t file, uint32_t ch, uint32_t dataSize, uint32_t off, const void* dest,
                uint32_t len, uint32_t got, int flag) {
    if (s_logLevel < 2 || !dest) return;
    uint32_t rate = 0, words[0x80 / 4], key = 0;
    SafeRead(file + 0x44, &rate, 4);
    if (SafeRead(file, words, sizeof(words)))
        for (int i = 0; i < 0x80 / 4 && !key; ++i) if (KeyLike(words[i])) key = words[i];
    LARGE_INTEGER qc, qf;
    QueryPerformanceCounter(&qc);
    QueryPerformanceFrequency(&qf);
    EnterCriticalSection(&s_cs);
    if (s_dumpIdx && s_dumpDat && s_dumpBytes < (400ull << 20)) {
        long long at = _ftelli64(s_dumpDat);
        fwrite(dest, 1, got, s_dumpDat);
        fprintf(s_dumpIdx, "decode,%.6f,%08X,%08X,%u,%u,%u,%u,%u,%u,%d,%08X,%lld\n",
                (double)qc.QuadPart / (double)qf.QuadPart, codec, file, ch, rate, dataSize, off, len, got, flag, key, at);
        s_dumpBytes += got;
    }
    LeaveCriticalSection(&s_cs);
}

// doppler_log=2: the whole content of each buffer that starts playing, once per distinct content (FNV-1a), with its
// format, the Play flags (1 = looping) and the sound set: "static" lines in index.csv.
std::unordered_set<uint64_t> s_dumpedContent;
void DumpStatic(void* b, DWORD playFlags) {
    if (s_logLevel < 2) return;
    struct { DWORD size, flags, bytes, unlockRate, cpu; } caps = { sizeof(caps), 0, 0, 0, 0 };
    typedef HRESULT(__stdcall* DsCapsFn)(void*, void*);
    if (FAILED(((DsCapsFn)s_dsOrig[3])(b, &caps)) || !caps.bytes) return;
    void *p1 = NULL, *p2 = NULL;
    DWORD n1 = 0, n2 = 0;
    if (FAILED(((DsLockFn)s_dsOrig[DS_LOCK])(b, 0, 0, &p1, &n1, &p2, &n2, 2))) return;
    uint64_t h = 1469598103934665603ull ^ n1;
    for (DWORD i = 0; i < n1; ++i) h = (h ^ ((const uint8_t*)p1)[i]) * 1099511628211ull;
    WAVEFORMATEX wf;
    memset(&wf, 0, sizeof(wf));
    DWORD got = 0;
    typedef HRESULT(__stdcall* DsFmtFn)(void*, WAVEFORMATEX*, DWORD, DWORD*);
    ((DsFmtFn)s_dsOrig[5])(b, &wf, sizeof(wf), &got);
    EnterCriticalSection(&s_cs);
    if (s_dumpIdx) {                               // every play: which content it starts with
        LARGE_INTEGER qc, qf;
        QueryPerformanceCounter(&qc);
        QueryPerformanceFrequency(&qf);
        fprintf(s_dumpIdx, "playcontent,%.6f,%08X,%016llX,%lu,%lu\n", (double)qc.QuadPart / (double)qf.QuadPart,
                (unsigned)(uintptr_t)b, (unsigned long long)h, (unsigned long)n1, (unsigned long)playFlags);
    }
    if (s_dumpIdx && s_dumpDat && s_dumpBytes < (400ull << 20) && s_dumpedContent.insert(h).second) {
        auto ow = s_bufOwner.find((uintptr_t)b);
        long long at = _ftelli64(s_dumpDat);
        fwrite(p1, 1, n1, s_dumpDat);
        fprintf(s_dumpIdx, "static,%08X,%08X,%lu,%u,%u,%lld,%016llX,%lu\n", (unsigned)(uintptr_t)b,
                ow != s_bufOwner.end() ? ow->second.set : 0u, (unsigned long)n1, (unsigned)wf.nSamplesPerSec,
                wf.nChannels, at, (unsigned long long)h, (unsigned long)playFlags);
        s_dumpBytes += n1;
    }
    LeaveCriticalSection(&s_cs);
    ((DsUnlockFn)s_dsOrig[DS_UNLOCK])(b, p1, n1, p2, n2);
}

void DsHookOnce() {                                // inside s_cs, from a source update (SLib's device exists)
    if (s_dsHooked) return;
    void* ds = *(void**)(uintptr_t)G_DSOUND;
    if (!ds) return;
    s_dsHooked = true;
    WAVEFORMATEX wf = { WAVE_FORMAT_PCM, 1, 22050, 44100, 2, 16, 0 };
    DsDesc d = { sizeof(DsDesc), 0xE0, 4096, 0, &wf, GUID() };
    void* buf = NULL;
    void** vt = *(void***)ds;
    HRESULT hr = ((DsCreateFn)vt[3])(ds, &d, &buf, NULL);
    if (FAILED(hr) || !buf) { AudLog("dsound log: CreateSoundBuffer failed (%08lX)", (unsigned long)hr); return; }
    void** bvt = *(void***)buf;
    memcpy(s_dsOrig, bvt, sizeof(s_dsOrig));
    void* repl[][2] = { { (void*)DS_SETVOL, (void*)&DsSetVolume }, { (void*)DS_SETPAN, (void*)&DsSetPan },
                        { (void*)DS_SETFREQ, (void*)&DsSetFrequency }, { (void*)DS_SETPOS, (void*)&DsSetCurrentPosition },
                        { (void*)DS_PLAY, (void*)&DsPlay }, { (void*)DS_STOP, (void*)&DsStop },
                        { (void*)DS_LOCK, (void*)&DsLock }, { (void*)DS_UNLOCK, (void*)&DsUnlock } };
    if (s_logLevel >= 2) {
        std::string dir = s_dumpDir.empty() ? g_dllDir + "dsdump" : s_dumpDir;
        CreateDirectoryA(dir.c_str(), NULL);
        s_dumpIdx = fopen((dir + "\\index.csv").c_str(), "w");
        s_dumpDat = fopen((dir + "\\data.bin").c_str(), "wb");
        if (s_dumpIdx) fprintf(s_dumpIdx, "t,buffer,set,lock_offset,n1,n2,data_at\n");
        AudLog("dsound dump: %s (%s)", dir.c_str(), s_dumpIdx && s_dumpDat ? "open" : "cannot write");
    }
    bool ok = true;
    for (size_t i = 0; i < sizeof(repl) / sizeof(repl[0]); ++i) {
        uint32_t slot = (uint32_t)(uintptr_t)&bvt[(uintptr_t)repl[i][0]];
        ok = WriteCode(slot, &repl[i][1], 4) && ok;
    }
    ((DsReleaseFn)bvt[2])(buf);
    AudLog("dsound log: secondary buffer methods hooked (vtable %08X)%s", (unsigned)(uintptr_t)bvt, ok ? "" : " (a write failed)");
}

void FlushDs() {                                   // inside s_cs
    DumpRingStates();
    if (s_ds.empty()) return;
    unsigned vol = 0, pan = 0, freq = 0, setpos = 0, play = 0, stop = 0;
    std::vector<std::pair<unsigned, uintptr_t> > busy;
    for (auto& kv : s_ds) {
        const DsStats& d = kv.second;
        vol += d.vol; pan += d.pan; freq += d.freq; setpos += d.setpos; play += d.play; stop += d.stop;
        busy.push_back(std::make_pair((unsigned)d.fMax * (d.freq ? 1u : 0u), kv.first));
    }
    std::sort(busy.rbegin(), busy.rend());
    AudLog("dsound: %u buffers: SetVolume %u, SetPan %u, SetFrequency %u, SetCurrentPosition %u, Play %u, Stop %u",
           (unsigned)s_ds.size(), vol, pan, freq, setpos, play, stop);
    for (size_t i = 0; i < busy.size() && i < 6; ++i) {
        const DsStats& d = s_ds[busy[i].second];
        auto ow = s_bufOwner.find(busy[i].second);
        AudLog("  buffer %08X (set %08X): SetVolume x%u (%d..%d mB, largest step %d), SetFrequency x%u (%lu..%lu Hz), "
               "SetPan x%u, SetCurrentPosition x%u, Play x%u, Stop x%u", (unsigned)busy[i].second,
               ow != s_bufOwner.end() ? ow->second.set : 0u, d.vol, d.vol ? d.volMin : 0,
               d.vol ? d.volMax : 0, d.maxVolStep, d.freq, (unsigned long)(d.freq ? d.fMin : 0), (unsigned long)d.fMax,
               d.pan, d.setpos, d.play, d.stop);
    }
    s_ds.clear();
    s_setposLogs = 0;
    s_fastLogs = 0;
    s_bigLogs = 0;
}

// ---------------------------------------------------------------------------------------------------------------
// hooks
// ---------------------------------------------------------------------------------------------------------------
// Replaces the velocity block of the source update (see VelocityCode): the engine's first-frame rule, then the
// velocity per mode, then previous position.
void __stdcall SourceVelocity(uint8_t* src, uint8_t* mgr) {
    uint32_t& flags = *(uint32_t*)(src + SRC_FLAGS);
    float* pos = (float*)(src + SRC_POS);
    float* prev = (float*)(src + SRC_PREV);
    float* vel = (float*)(src + SRC_VEL);
    float dt = *(float*)(mgr + MGR_DT);
    EnterCriticalSection(&s_cs);
    if (s_log) {
        DsHookOnce();
        s_mgr = *(uint32_t*)(src + SRC_MANAGER);
        Seen& z = s_seen[(uintptr_t)src];
        if (!z.upd) z.set = SetKey((uintptr_t)src);
        NoteVoices((uintptr_t)src, z.set);
        ++z.upd;
        z.flags = flags;
        memcpy(z.pos, pos, sizeof(z.pos));
        z.pitch = *(float*)(src + 0xA4);
        z.cutoff = *(float*)(src + 0x7C);
        if (!(flags & FLAG_HAS_PREV)) {
            Starts& t = s_starts[z.set];
            ++t.n;
            memcpy(t.pos, pos, sizeof(t.pos));
        }
    }
    if (!(flags & FLAG_HAS_PREV)) {                // the first update: no velocity yet
        flags |= FLAG_HAS_PREV;
        vel[0] = vel[1] = vel[2] = 0.0f;
        for (int i = 0; i < 3; ++i) prev[i] = pos[i];
        if (s_mode == MODE_WII) {
            if (s_acc.size() > 8192) s_acc.clear();   // sources that are gone
            s_acc[(uintptr_t)src] = 0.0f;
        }
    } else {
        bool still = pos[0] == prev[0] && pos[1] == prev[1] && pos[2] == prev[2];
        if (s_mode == MODE_WII) VelocityWindow(pos, prev, vel, dt, s_acc[(uintptr_t)src], s_window);
        else VelocityEngine(pos, prev, vel, dt);
        if (s_log) {
            SrcStats& s = Stats((uintptr_t)src);
            ++s.upd;
            if (still) ++s.still;
            s.dtMin = std::min(s.dtMin, dt);
            s.dtMax = std::max(s.dtMax, dt);
            float sp = sqrtf(vel[0] * vel[0] + vel[1] * vel[1] + vel[2] * vel[2]);
            s.speedSum += sp;
            s.speedMax = std::max(s.speedMax, sp);
            memcpy(s.pos, pos, sizeof(s.pos));
        }
    }
    if (s_log) MaybeFlush();
    LeaveCriticalSection(&s_cs);
}

typedef float(__thiscall* DopplerFactorFn)(void* self, uint8_t* src, uint32_t listener, float* invDist, float* speed);

float __fastcall DopplerFactorHook(void* self, void*, uint8_t* src, uint32_t listener, float* invDist, float* speed) {
    float f = ((DopplerFactorFn)(uintptr_t)DOPPLER_FACTOR)(self, src, listener, invDist, speed);
    if (!s_log) return f;
    EnterCriticalSection(&s_cs);
    if ((f > 1.25f || f < 0.8f) && s_bigLogs < 40) {
        ++s_bigLogs;
        uint32_t mgr = *(uint32_t*)(src + SRC_MANAGER), lst = 0;
        float lp[3] = { 0, 0, 0 }, lv[3] = { 0, 0, 0 };
        if (SafeRead(mgr + 0xA28, &lst, 4) && lst) {
            SafeRead(lst + 0x1728 + listener * 0xEC, lp, sizeof(lp));
            SafeRead(lst + 0x1774 + listener * 0xEC, lv, sizeof(lv));
        }
        const float* p = (const float*)(src + SRC_POS);
        const float* v = (const float*)(src + SRC_VEL);
        const float* q = (const float*)(src + SRC_PREV);
        AudLog("  doppler %.3f: src %08X set %08X pos (%.2f, %.2f, %.2f) prev (%.2f, %.2f, %.2f) vel (%.1f, %.1f, %.1f); "
               "listener %u pos (%.2f, %.2f, %.2f) vel (%.1f, %.1f, %.1f); dt %.2f ms", f, (unsigned)(uintptr_t)src,
               SetKey((uintptr_t)src), p[0], p[1], p[2], q[0], q[1], q[2], v[0], v[1], v[2], listener, lp[0], lp[1],
               lp[2], lv[0], lv[1], lv[2], *(float*)(mgr + MGR_DT) * 1000.0f);
    }
    SrcStats& s = Stats((uintptr_t)src);
    if (s.nf) s.stepSq += (double)(f - s.fLast) * (f - s.fLast);
    ++s.nf;
    s.fSum += f;
    s.fMin = std::min(s.fMin, f);
    s.fMax = std::max(s.fMax, f);
    s.fLast = f;
    s.dist = *invDist > 0.0f ? 1.0f / *invDist : 0.0f;
    if (s_described.size() < 4096 && s_described.insert((uintptr_t)src).second) {
        const float* par = NULL;
        uint32_t parPtr = 0;
        float pv[5] = { 0, 0, 0, 0, 0 };
        if (SafeRead((uintptr_t)self + 8, &parPtr, 4) && SafeRead(parPtr + 0xC, pv, sizeof(pv))) par = pv;
        uint32_t mgr = *(uint32_t*)(src + SRC_MANAGER);
        float global = 0.0f;
        SafeRead(mgr + MGR_DOPPLER, &global, 4);
        std::string keys = KeysNear((uintptr_t)src);
        AudLog("doppler source %08X: insert scale %.3f range %.3f..%.3f curves %08X/%08X, global scale %.3f; keys%s",
               (unsigned)(uintptr_t)src, par ? par[0] : 0.0f, par ? par[1] : 0.0f, par ? par[2] : 0.0f,
               par ? *(const uint32_t*)&par[3] : 0u, par ? *(const uint32_t*)&par[4] : 0u, global, keys.c_str());
    }
    MaybeFlush();
    LeaveCriticalSection(&s_cs);
    return f;
}

// The replacement of 004ACF74..004AD041: SourceVelocity(esi = source, edi = manager), then the x87 stack the
// voices' loop expects (st0 = 0.0, st1 = 1.0, as the engine's block leaves it) and edx = 0 (its counter).
void VelocityCode(uint8_t* code, uint32_t n) {
    memset(code, 0xCC, n);
    uint8_t* p = code;
    *p++ = 0x57;                                   // push edi
    *p++ = 0x56;                                   // push esi
    *p++ = 0xE8;                                   // call SourceVelocity
    int32_t rel = (int32_t)((uint32_t)(uintptr_t)&SourceVelocity - (VEL_BLOCK + (uint32_t)(p + 4 - code)));
    memcpy(p, &rel, 4);
    p += 4;
    *p++ = 0xD9; *p++ = 0xE8;                      // fld1
    *p++ = 0xD9; *p++ = 0xEE;                      // fldz
    *p++ = 0x33; *p++ = 0xD2;                      // xor edx, edx
    *p++ = 0xE9;                                   // jmp 004AD041
    rel = (int32_t)(VEL_BLOCK_END - (VEL_BLOCK + (uint32_t)(p + 4 - code)));
    memcpy(p, &rel, 4);
}

int Verify(const Image& img, std::string& rep) {
    int bad = 0;
    uint32_t got = 0;
    for (size_t i = 0; i < sizeof(kCode) / sizeof(kCode[0]); ++i) {
        bool ok = CheckCrc(img, kCode[i].va, kCode[i].len, kCode[i].crc, &got);
        Report(rep, ok, "%-62s %08X len 0x%03X crc %08X (want %08X)", kCode[i].what, kCode[i].va, kCode[i].len, got,
               kCode[i].crc);
        if (!ok) ++bad;
    }
    uint8_t call[5];
    bool callOk = img.Read(DOPPLER_CALL, call, 5) && call[0] == 0xE8 &&
                  DOPPLER_CALL + 5 + *(int32_t*)&call[1] == DOPPLER_FACTOR;
    Report(rep, callOk, "Doppler insert update: call DopplerFactor at %08X", DOPPLER_CALL);
    if (!callOk) ++bad;
    return bad;
}

int VerifyFix(const Image& img, std::string& rep) {
    int bad = 0;
    uint32_t got = 0;
    for (size_t i = 0; i < sizeof(kFix) / sizeof(kFix[0]); ++i) {
        bool ok = CheckCrc(img, kFix[i].va, kFix[i].len, kFix[i].crc, &got);
        Report(rep, ok, "%-62s %08X len 0x%03X crc %08X (want %08X)", kFix[i].what, kFix[i].va, kFix[i].len, got,
               kFix[i].crc);
        if (!ok) ++bad;
    }
    uint8_t b[8];
    bool ok = img.Read(FREE_PCM_CALL - 3, b, 8) && b[0] == 0x8B && b[1] == 0x4E && b[2] == 0x0C && b[3] == 0xE8 &&
              FREE_PCM_CALL + 5 + *(int32_t*)&b[4] == FREE_PCM;
    Report(rep, ok, "static buffer: mov ecx, [esi+0Ch]; call %08X at %08X", FREE_PCM, FREE_PCM_CALL);
    if (!ok) ++bad;
    for (size_t i = 0; i < sizeof(RESOLVE_CALLS) / sizeof(RESOLVE_CALLS[0]); ++i) {
        uint8_t c[11];
        static const uint8_t movEcx[6] = { 0x8B, 0x0D, 0x8C, 0xE8, 0xA6, 0x00 };   // mov ecx, [00A6E88C]
        ok = img.Read(RESOLVE_CALLS[i] - 6, c, 11) && !memcmp(c, movEcx, 6) && c[6] == 0xE8 &&
             RESOLVE_CALLS[i] + 5 + *(int32_t*)&c[7] == RESOLVE_ALL;
        Report(rep, ok, "mov ecx, [%08X]; call %08X (resolve) at %08X", G_SLIB_MGR, RESOLVE_ALL, RESOLVE_CALLS[i]);
        if (!ok) ++bad;
    }
    return bad;
}

bool InstallFix() {
    int32_t rel = (int32_t)((uint32_t)(uintptr_t)&FreeThunk - (FREE_PCM_CALL + 5));
    bool ok = WriteCode(FREE_PCM_CALL + 1, &rel, 4);
    for (size_t i = 0; i < sizeof(RESOLVE_CALLS) / sizeof(RESOLVE_CALLS[0]); ++i) {
        rel = (int32_t)((uint32_t)(uintptr_t)&ResolveHook - (RESOLVE_CALLS[i] + 5));
        ok = WriteCode(RESOLVE_CALLS[i] + 1, &rel, 4) && ok;
    }
    s_fixInstalled = true;                         // before the decode hook: it answers stale reads with silence
    uint32_t fn = (uint32_t)(uintptr_t)&DecodeHook;
    return WriteCode(ADPCM_VTABLE + 4 * 4, &fn, 4) && ok;
}

}  // namespace

void AudioAttach(const std::string& iniPath) {
    s_inAttach = true;
    InitializeCriticalSection(&s_cs);
    std::string v;
    if (ReadIniKey(iniPath, "audio", "doppler", v)) s_mode = _stricmp(v.c_str(), "wii") == 0 ? MODE_WII : MODE_ENGINE;
    if (ReadIniKey(iniPath, "audio", "doppler_rate", v) && !v.empty()) {
        float r = (float)atof(v.c_str());
        if (r >= 10.0f && r <= 1000.0f) s_rate = r;
    }
    s_window = 0.9f / s_rate;                       // a 60 Hz frame is a measurement even when it comes in a bit short
    s_log = ReadIniKey(iniPath, "audio", "doppler_log", v) && (IsOn(v) || v == "2");
    s_logLevel = s_log ? (v == "2" ? 2 : 1) : 0;
    if (ReadIniKey(iniPath, "audio", "dump_dir", v)) s_dumpDir = v;
    if (ReadIniKey(iniPath, "audio", "keep_pcm", v) && !v.empty() && !IsOn(v)) s_keepPcm = false;
    ProcessImage img;
    uint8_t probe[1];
    if (!img.Read(VEL_BLOCK, probe, 1)) {
        s_inAttach = false;                        // not the PC executable (wmtest)
        return;
    }
    // both checks before either patch (the fix and the log both hook the ngcadpcm vtable)
    std::string rep;
    bool doppler = s_mode != MODE_ENGINE || s_log;
    int badFix = s_keepPcm ? VerifyFix(img, rep) : 0;
    int bad = doppler ? Verify(img, rep) : 0;
    if (!s_keepPcm)
        AudLog("[audio] keep_pcm=0: sets that play their samples one after the other decode them again after the "
               "level's sound data is gone (noise)");
    else if (badFix)
        AudLog("the PC executable differs from the expected 2010 build in %d place(s): the sequence-set sound fix is "
               "off", badFix);
    else if (InstallFix())
        AudLog("PC executable verified: samples that sets play one after the other keep their decoded sound "
               "(keep_pcm=1)");
    else
        AudLog("cannot patch the sequence-set sound fix completely (error %lu)", GetLastError());
    if (!doppler) {
        AudLog("[audio] doppler=engine: the sources' velocities are left to the engine");
        s_inAttach = false;
        return;
    }
    if (bad) {
        AudLog("the PC executable differs from the expected 2010 build in %d place(s): Doppler speeds left to the "
               "engine", bad);
        s_inAttach = false;
        return;
    }
    uint8_t code[VEL_BLOCK_END - VEL_BLOCK];
    VelocityCode(code, sizeof(code));
    s_velInstalled = WriteCode(VEL_BLOCK, code, sizeof(code));
    if (s_log) {
        int32_t rel = (int32_t)((uint32_t)(uintptr_t)&DopplerFactorHook - (DOPPLER_CALL + 5));
        s_logInstalled = WriteCode(DOPPLER_CALL + 1, &rel, 4);
        uint32_t fn = (uint32_t)(uintptr_t)&DecodeHook;
        s_logInstalled = WriteCode(ADPCM_VTABLE + 4 * 4, &fn, 4) && s_logInstalled;
    }
    if (!s_velInstalled) AudLog("cannot patch %08X (error %lu): Doppler speeds left to the engine", VEL_BLOCK,
                                GetLastError());
    else if (s_mode == MODE_WII)
        AudLog("PC executable verified: sound source velocities for the Doppler effect are measured over at least "
               "1/%.0f s of game time, like the Wii's frames (source update %08X)%s", s_rate, VEL_BLOCK,
               s_log ? "; doppler_log=1: one summary per second" : "");
    else
        AudLog("PC executable verified: doppler=engine with doppler_log=1: the engine's velocities, one summary per "
               "second");
    s_inAttach = false;
}

void AudioAfterConfig() {
    for (size_t i = 0; i < s_pending.size(); ++i) Log("AUDIO: %s", s_pending[i].c_str());
    s_pending.clear();
}

extern "C" {

// wmtest only: verify the patch sites against an exe file.  Returns the number of differences.
int __cdecl WiimoteAudioCheckExe(const char* exePath, char* out, int outSize) {
    FileImage img;
    std::string rep;
    int bad = -1;
    if (!img.Load(exePath)) rep = "  FAIL  cannot read " + std::string(exePath ? exePath : "(null)") + "\n";
    else bad = Verify(img, rep) + VerifyFix(img, rep);
    if (out && outSize > 0) {
        size_t n = rep.size() < (size_t)outSize - 1 ? rep.size() : (size_t)outSize - 1;
        memcpy(out, rep.data(), n);
        out[n] = 0;
    }
    return bad;
}

// wmtest only: a source moving at a steady speed whose position advances only on some frames, run through the
// engine's velocity and the windowed one at several frame rates; the replacement code's bytes.
int __cdecl WiimoteAudioSelfTest(char* out, int outSize) {
    std::string rep;
    int fails = 0;
    const float speed = 20.0f;                     // m/s along x
    struct Case { double fps, stepHz; const char* what; };
    const Case cases[] = {
        { 60.0, 0.0, "60 fps, moves every frame" },
        { 500.0, 0.0, "500 fps, moves every frame" },
        { 500.0, 60.0, "500 fps, position advances at 60 Hz" },
        { 1900.0, 30.0, "1900 fps, position advances at 30 Hz" },
    };
    for (size_t c = 0; c < sizeof(cases) / sizeof(cases[0]); ++c) {
        const float window = 0.9f / 60.0f;
        float dt = (float)(1.0 / cases[c].fps);
        float pos[3] = { 0, 0, 0 };
        float prevE[3] = { 0, 0, 0 }, velE[3] = { 0, 0, 0 };
        float prevW[3] = { 0, 0, 0 }, velW[3] = { 0, 0, 0 }, acc = 0.0f;
        double t = 0, sumE = 0, sumW = 0, sqE = 0, sqW = 0, lastE = 0, lastW = 0, maxDevW = 0;
        int n = 0, frames = (int)(cases[c].fps * 2.0);
        for (int f = 0; f < frames; ++f) {
            t += dt;
            double tp = cases[c].stepHz > 0.0 ? floor(t * cases[c].stepHz) / cases[c].stepHz : t;
            pos[0] = (float)(tp * speed);
            VelocityEngine(pos, prevE, velE, dt);
            VelocityWindow(pos, prevW, velW, dt, acc, window);
            if (f < (int)cases[c].fps / 2) continue;      // settled
            if (n) {
                sqE += (velE[0] - lastE) * (velE[0] - lastE);
                sqW += (velW[0] - lastW) * (velW[0] - lastW);
            }
            lastE = velE[0];
            lastW = velW[0];
            sumE += velE[0];
            sumW += velW[0];
            maxDevW = std::max(maxDevW, (double)fabs(velW[0] - speed));
            ++n;
        }
        double meanE = sumE / n, meanW = sumW / n, stepE = sqrt(sqE / (n - 1)), stepW = sqrt(sqW / (n - 1));
        // the Doppler factor step these speeds make with scale 1 (1 + v / 300)
        bool ok = fabs(meanW - speed) < 0.5 && (cases[c].stepHz > 0.0 || maxDevW < 0.05);
        if (cases[c].fps <= 60.0) ok = ok && stepE == stepW;
        Report(rep, ok, "%-38s speed mean engine %.2f / windowed %.2f m/s, frame-to-frame step rms engine %.3f / "
               "windowed %.3f m/s (Doppler factor steps %.5f / %.5f)", cases[c].what, meanE, meanW, stepE, stepW,
               stepE / 300.0, stepW / 300.0);
        if (!ok) ++fails;
    }
    // at 60 fps (a frame of exactly 1/60 s, or a bit short) every frame is measured: identical to the engine
    {
        float pos[3] = { 0, 0, 0 }, pe[3] = { 0, 0, 0 }, ve[3], pw[3] = { 0, 0, 0 }, vw[3], acc = 0.0f;
        bool same = true;
        for (int f = 1; f <= 600; ++f) {
            float dt = (f % 7) ? 1.0f / 60.0f : 1.0f / 62.0f;
            pos[0] = (float)f * 0.3f + (float)(f % 3) * 0.01f;
            pos[2] = (float)(f % 5) * 0.2f;
            VelocityEngine(pos, pe, ve, dt);
            VelocityWindow(pos, pw, vw, dt, acc, 0.9f / 60.0f);
            same = same && !memcmp(ve, vw, sizeof(ve)) && !memcmp(pe, pw, sizeof(pe));
        }
        Report(rep, same, "60 fps (frames of 1/60 and 1/62 s): windowed velocity identical to the engine's");
        if (!same) ++fails;
    }
    // a new source: first position the origin, then its object 40 m away: a jump, not motion
    {
        float pos[3] = { 0, 0, 0 }, pw[3] = { 0, 0, 0 }, vw[3] = { 0, 0, 0 }, acc = 0.0f;
        pos[0] = 30.0f; pos[1] = -26.0f;
        for (int f = 0; f < 8; ++f) VelocityWindow(pos, pw, vw, 0.004f, acc, 0.9f / 60.0f);
        bool ok = vw[0] == 0.0f && vw[1] == 0.0f && pw[0] == 30.0f;
        Report(rep, ok, "a source placed 40 m from the origin after its first update: velocity 0 (jump), not 2600 m/s");
        if (!ok) ++fails;
    }
    // paused (DT 0): the engine's result
    {
        float pos[3] = { 5, 0, 0 }, pw[3] = { 4, 0, 0 }, vw[3] = { 9, 9, 9 }, acc = 0.004f;
        VelocityWindow(pos, pw, vw, 0.0f, acc, 0.015f);
        bool ok = vw[0] == 1.0f && vw[1] == 0.0f && pw[0] == 5.0f && acc == 0.0f;
        Report(rep, ok, "DT 0: velocity = the position difference, as the engine computes it");
        if (!ok) ++fails;
    }
    // the replacement code
    {
        uint8_t code[VEL_BLOCK_END - VEL_BLOCK];
        VelocityCode(code, sizeof(code));
        int32_t call = *(int32_t*)&code[3], jmp = *(int32_t*)&code[14];
        bool ok = code[0] == 0x57 && code[1] == 0x56 && code[2] == 0xE8 &&
                  VEL_BLOCK + 7 + call == (uint32_t)(uintptr_t)&SourceVelocity && code[7] == 0xD9 &&
                  code[8] == 0xE8 && code[9] == 0xD9 && code[10] == 0xEE && code[11] == 0x33 && code[12] == 0xD2 &&
                  code[13] == 0xE9 && VEL_BLOCK + 18 + jmp == VEL_BLOCK_END && code[18] == 0xCC &&
                  code[sizeof(code) - 1] == 0xCC;
        Report(rep, ok, "replacement of %08X..%08X: push edi, push esi, call, fld1, fldz, xor edx, jmp %08X (%u bytes, "
               "rest int3)", VEL_BLOCK, VEL_BLOCK_END, VEL_BLOCK_END, (unsigned)sizeof(code));
        if (!ok) ++fails;
    }
    // keep_pcm: a manager's object list with sets and samples laid out like the engine's
    {
        static uint8_t mgr[0xA00], files[4][0x80], objs[4][0x20], sets[5][0x100], list[6][8], kids[4][4 * 3];
        memset(mgr, 0, sizeof(mgr)); memset(files, 0, sizeof(files)); memset(objs, 0, sizeof(objs));
        memset(sets, 0, sizeof(sets)); memset(list, 0, sizeof(list)); memset(kids, 0, sizeof(kids));
        auto put32 = [](uint8_t* p, uint32_t off, uint32_t v) { memcpy(p + off, &v, 4); };
        auto put16 = [](uint8_t* p, uint32_t off, uint16_t v) { memcpy(p + off, &v, 2); };
        auto addr = [](const void* p) { return (uint32_t)(uintptr_t)p; };
        for (int i = 0; i < 4; ++i) {              // samples 0-2 are RAM samples (kind 1), 3 a stream (kind 2)
            put16(objs[i], OBJ_KIND, i == 3 ? 2 : 1);
            put32(objs[i], OBJ_FILE, addr(files[i]));
        }
        // set 0: type 2 (own), plays samples 0 and 3; set 1: type 1, sample 1; set 2: no own type (override bit 7
        // clear), template set 3 of type 8, sample 2; set 4: type 1, resolved already, template set 0, sample 1
        const uint32_t type[5] = { 2, 1, 0, 8, 1 }, over[5] = { 0x80, 0x80, 0, 0x80, 0 };
        const uint8_t* tpl[5] = { nullptr, nullptr, sets[3], nullptr, sets[0] };
        const int n[5] = { 3, 1, 1, 0, 1 };
        put32(kids[0], 0, addr(objs[0])); put32(kids[0], 8, addr(objs[3]));
        put32(kids[1], 0, addr(objs[1]));
        put32(kids[2], 0, addr(objs[2]));
        put32(kids[3], 0, addr(objs[1]));
        const int kidList[5] = { 0, 1, 2, -1, 3 };
        for (int i = 0; i < 5; ++i) {
            put32(sets[i], 0, SET_VTABLE);
            put32(sets[i], OBJ_KEY, 0x85004890 + i);
            put16(sets[i], OBJ_KIND, 3);
            put16(sets[i], OBJ_FLAGS, i == 4 ? 1 : 0);
            put32(sets[i], SET_TYPE, type[i]);
            put32(sets[i], SET_COUNT, n[i]);
            put32(sets[i], SET_CHILDREN, kidList[i] >= 0 ? addr(kids[kidList[i]]) : 0);
            put32(sets[i], SET_TEMPLATE, addr(tpl[i]));
            put32(sets[i], SET_OVERRIDE, over[i]);
            put32(list[i], 4, addr(sets[i]));
        }
        put32(list[5], 4, addr(objs[0]));          // a sample in the list: not a set
        put32(mgr, MGR_OBJ_DATA, addr(list));
        put32(mgr, MGR_OBJ_STRIDE, 8);
        put32(mgr, MGR_OBJ_COUNT, 6);
        uint32_t key[4] = {}, m = addr(mgr);
        int t[4] = {};
        bool in[4];
        for (int i = 0; i < 4; ++i) in[i] = SequenceChild(m, addr(files[i]), &key[i], &t[i]);
        bool ok = in[0] && key[0] == 0x85004890 && t[0] == 2 && !in[1] && in[2] && key[2] == 0x85004892 && t[2] == 8 &&
                  !in[3];
        Report(rep, ok, "sequence samples: type 2 set's sample kept, type 1's freed (also when a resolved type 1 set has a "
               "type 2 template), a sample of a set inheriting type 8 kept, a stream child ignored");
        if (!ok) ++fails;
        unsigned ns = 0, nsamp = 0;
        CountSequenceSets(m, &ns, &nsamp);
        ok = ns == 3 && nsamp == 2;
        Report(rep, ok, "sequence sets counted: %u (want 3: types 2, inherited 8, 8), their RAM samples %u (want 2)", ns,
               nsamp);
        if (!ok) ++fails;
        static uint8_t emptyMgr[0xA00];
        ok = !SequenceChild(addr(emptyMgr), addr(files[0]), &key[0], &t[0]);
        Report(rep, ok, "sequence samples: an empty object list keeps nothing");
        if (!ok) ++fails;
    }
    // keep_pcm: the reads that would come from the shadow BF's start
    {
        static uint8_t file[0x80];
        uint32_t closed = 0, open = 1, two = 2, one = 1, pos = 0x12345;
        memset(file, 0, sizeof(file));
        memcpy(file + FILE_STATE, &two, 4);
        uint32_t bin = (uint32_t)(uintptr_t)&closed, f = (uint32_t)(uintptr_t)file;
        bool stale = StaleStreamFile(f, bin), streamOpen = StaleStreamFile(f, (uint32_t)(uintptr_t)&open);
        memcpy(file + FILE_STATE, &one, 4);
        bool opened = StaleStreamFile(f, bin);
        memcpy(file + FILE_STATE, &two, 4);
        memcpy(file + FILE_HANDLE, &pos, 4);
        bool handle = StaleStreamFile(f, bin);
        bool ok = stale && !streamOpen && !opened && !handle;
        Report(rep, ok, "stale reads: state 2 with the load stream closed and no handle -> silence; stream open, state 1 "
               "or an opened handle -> the engine's read");
        if (!ok) ++fails;
    }
    // keep_pcm: the thunk at 004BC042
    {
        const uint8_t* c = (const uint8_t*)(uintptr_t)&FreeThunk;
        int32_t rel;
        memcpy(&rel, c + 3, 4);
        bool ok = c[0] == 0x56 && c[1] == 0x51 && c[2] == 0xE8 && c[7] == 0xC3 &&
                  (uint32_t)(uintptr_t)(c + 7) + rel == (uint32_t)(uintptr_t)&FreeAfterStatic;
        Report(rep, ok, "free thunk: push esi, push ecx, call FreeAfterStatic, ret");
        if (!ok) ++fails;
    }
    if (out && outSize > 0) {
        size_t m = rep.size() < (size_t)outSize - 1 ? rep.size() : (size_t)outSize - 1;
        memcpy(out, rep.data(), m);
        out[m] = 0;
    }
    return fails;
}

}  // extern "C"
