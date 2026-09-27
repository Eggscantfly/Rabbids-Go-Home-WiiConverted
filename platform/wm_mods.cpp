// wm_mods.cpp - mods read from the game folder, no archive rewrite ([mods] of wiimote.ini; the enabled list in
// options.ini, written by the launcher).
//
// A mod is a folder under <game folder>\mods\ (the launcher shows its mod.json).  What the game reads from it:
//   entries\<KEY>.<ext>   a whole bigfile entry (KEY = 8 hex digits, or the entry's file name such as default.cfg):
//                         a per-world bin (FFF package, FEF texture bank, FDF sound bin, FCF language index, FBF Magma
//                         blob, a language bin) or a loose file (.wol .fct .cfg .smx ...), raw, as the converter
//                         stores them.  <file>.refs next to it is the entry's reference table (12-byte references:
//                         what a .wol / .wog lists); without it the archive entry's own table is kept
//   records\<KEY>.bin     one record of the world packages (the body after {key, len}: the same files as the
//                         converter's script_overrides layers): every package that holds the key gets the new body
// Later mods in the enabled list win.  A key the archive lacks can be added as an entry.
//
// How the engine reads its archive (see rghport/archive/patch.py and bigfile.py): BIG::b_Open (006BCC90) loads the
// file table into a key -> position map; every read starts with BIG::i64_KeySearchPos (006E9DB0: {hi, lo} of the
// entry, {-1, -1} for none), then the file layer seeks there (FIL_Seek 006D50C0 -> SetFilePointer(h, lo, &hi,
// FILE_BEGIN)) and reads the 32-byte file header {length, user length, reference bytes, flags} and the payload
// (FIL_Read 006D4ED0 -> ReadFile); the per-world bins go the same way through the BIG stream (vtable +0x40 / +0x4C).
// The header inside the file gives the sizes, the file table's length is not used for reading.
//
// The mod loader therefore
//   1. detours i64_KeySearchPos: a key a mod provides gets a virtual position {VIRTUAL_HI + n, 0} (the archive is
//      smaller than 4 GB: real positions have hi 0);
//   2. hooks the executable's imports SetFilePointer / ReadFile / CloseHandle: a seek to a virtual position puts the
//      handle in virtual mode, reads then come from the virtual entry - the synthesized header (raw, no shortcut),
//      the mod's payload, and the original entry's reference table - until the next seek to a real position.
// A virtual package is the original package with the overridden record bodies swapped in, built the first time the
// engine asks for it (the original is read through the loader's own handle on the archive).
#include "wiimote.h"

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <map>

using namespace wmpatch;

namespace {

const uint32_t KEY_SEARCH       = 0x006E9DB0;   // BIG::i64_KeySearchPos(this, i64* out, key)
const uint32_t KEY_SEARCH_TAIL  = 0x006E9DBA;   // after its first two instructions (10 bytes, no relocation)
const uint32_t G_MAIN_BIG       = 0x00A70B9C;   // the game's archive object (gameChecks 00409ACB)
const uint32_t IAT_SET_FILE_PTR = 0x0089D1AC;
const uint32_t IAT_READ_FILE    = 0x0089D1B0;
const uint32_t IAT_CLOSE_HANDLE = 0x0089D0BC;
const uint32_t VIRTUAL_HI       = 0x4D4F4400;   // position hi word of virtual entries ("DMO")
const uint32_t FILE_HDR         = 32;
const uint32_t BIG_MAGIC        = 0x00454241;

struct Code { uint32_t va, len, crc; const char* what; };
const Code kCode[] = {
    { 0x006E9DB0, 0x040, 0xBC7F3A18, "BIG::i64_KeySearchPos" },
    { 0x00409ACB, 0x014, 0x174A21B2, "gameChecks: the archive object and its name -> BIG::b_Open" },
};

// ------------------------------------------------------------------------------------------------- what mods give
struct EntryFile { std::string mod, path; };
struct RecordFile { std::string mod, path; std::vector<uint8_t> body; };

struct Fat {                                    // the archive's file table, read by this module
    struct Entry { uint32_t key, hi, lo, length; std::string name; };
    std::vector<Entry> entries;
    std::map<uint32_t, size_t> byKey;
    std::map<std::string, uint32_t> byName;     // lower case
};

struct Virtual {                                // one entry served from memory
    uint32_t key;
    std::string from;
    std::vector<uint8_t> bytes;                 // header + payload + references
};

struct HandleState { int index; uint64_t offset; };

std::vector<std::string> s_pending;
bool  s_inAttach, s_enabled = true, s_log = true, s_installed;
std::string s_bigfile;                          // the archive's path
Fat   s_fat;
std::map<uint32_t, EntryFile>  s_entryFiles;    // key -> file
std::map<uint32_t, RecordFile> s_recordFiles;   // record key -> body
std::vector<Virtual*>          s_virtual;       // index = position hi - VIRTUAL_HI
std::map<uint32_t, EntryFile>  s_musicFiles;    // mods\<mod>\music: entries of the streamed sibling archive
std::map<uint32_t, int>        s_musicState;    // what a key resolved to there (its own archive, its own keys)
std::map<uint32_t, int>        s_keyState;      // key -> virtual index, -1 = looked at, nothing to serve
std::map<HANDLE, HandleState>  s_handles;
CRITICAL_SECTION s_cs;
bool s_csInit;
std::vector<std::string> s_mods;                // enabled, in order
int s_served, s_servedLogs;

typedef int64_t* (__fastcall* KeySearchFn)(void* self, void* edx, int64_t* out, uint32_t key);
typedef DWORD (WINAPI* SetFilePointerFn)(HANDLE, LONG, PLONG, DWORD);
typedef BOOL (WINAPI* ReadFileFn)(HANDLE, LPVOID, DWORD, LPDWORD, LPOVERLAPPED);
typedef BOOL (WINAPI* CloseHandleFn)(HANDLE);
KeySearchFn      s_origKeySearch;               // the trampoline: the first instructions, then the rest
SetFilePointerFn s_origSetFilePointer;
ReadFileFn       s_origReadFile;
CloseHandleFn    s_origCloseHandle;

void ModLog(const char* fmt, ...) {
    char buf[600];
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf(buf, sizeof(buf) - 1, fmt, ap);
    buf[sizeof(buf) - 1] = 0;
    va_end(ap);
    if (s_inAttach) s_pending.push_back(buf);
    else if (s_log) Log("MODS: %s", buf);
}

struct Lock {
    Lock() { EnterCriticalSection(&s_cs); }
    ~Lock() { LeaveCriticalSection(&s_cs); }
};

std::string Lower(std::string s) {
    for (size_t i = 0; i < s.size(); ++i) s[i] = (char)tolower((unsigned char)s[i]);
    return s;
}

bool ReadWhole(const std::string& path, std::vector<uint8_t>& out) {
    HANDLE h = CreateFileA(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size;
    bool ok = GetFileSizeEx(h, &size) && size.QuadPart >= 0 && size.QuadPart < (512 << 20);
    if (ok) {
        out.resize((size_t)size.QuadPart);
        DWORD got = 0;
        ok = out.empty() || (ReadFile(h, &out[0], (DWORD)out.size(), &got, NULL) && got == out.size());
    }
    CloseHandle(h);
    return ok;
}

// the archive: the first argument of the command line that is not a switch
std::string BigfileFromCommandLine() {
    const char* cl = GetCommandLineA();
    std::vector<std::string> args;
    std::string cur;
    bool quoted = false, any = false;
    for (const char* p = cl; ; ++p) {
        char c = *p;
        if (c == '"') { quoted = !quoted; any = true; continue; }
        if (c == 0 || (!quoted && (c == ' ' || c == '\t'))) {
            if (any || !cur.empty()) args.push_back(cur);
            cur.clear();
            any = false;
            if (c == 0) break;
            continue;
        }
        cur += c;
    }
    for (size_t i = 1; i < args.size(); ++i)
        if (!args[i].empty() && args[i][0] != '/' && args[i][0] != '-') return args[i];
    return std::string();
}

// ------------------------------------------------------------------------------------------------- the file table
bool ReadAt(HANDLE h, uint64_t pos, void* out, uint32_t n) {
    LARGE_INTEGER li;
    li.QuadPart = (LONGLONG)pos;
    if (!SetFilePointerEx(h, li, NULL, FILE_BEGIN)) return false;
    DWORD got = 0;
    return ReadFile(h, out, n, &got, NULL) && got == n;
}

inline uint32_t U32(const uint8_t* p) { uint32_t v; memcpy(&v, p, 4); return v; }
inline uint64_t Pos64(const uint8_t* p) { return ((uint64_t)U32(p) << 32) | U32(p + 4); }   // {hi, lo}

bool LoadFat(const std::string& path, Fat& fat, std::string& why) {
    HANDLE h = CreateFileA(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) { why = "cannot open the archive"; return false; }
    uint8_t hdr[0x54];
    bool ok = ReadAt(h, 0, hdr, sizeof(hdr)) && U32(hdr) == BIG_MAGIC;
    if (!ok) { CloseHandle(h); why = "not a bigfile"; return false; }
    uint32_t nblocks = U32(hdr + 0x0C), numFiles = U32(hdr + 0x2C);
    uint64_t pos = Pos64(hdr + 0x14);
    uint32_t seen = 0;
    for (uint32_t b = 0; b < nblocks && pos && seen < numFiles; ++b) {
        uint8_t bh[12];
        if (!ReadAt(h, pos, bh, sizeof(bh))) { why = "file table block unreadable"; break; }
        uint32_t count = U32(bh);
        uint64_t next = Pos64(bh + 4);
        if (count > 4000000) { why = "file table block count"; break; }
        std::vector<uint8_t> recs((size_t)count * 200);
        if (count && !ReadAt(h, pos + 12, &recs[0], (uint32_t)recs.size())) { why = "file table records unreadable"; break; }
        for (uint32_t i = 0; i < count && seen < numFiles; ++i, ++seen) {
            const uint8_t* r = &recs[(size_t)i * 200];
            Fat::Entry e;
            e.name.assign((const char*)r, strnlen((const char*)r, 64));
            e.key = U32(r + 0x64);
            e.hi = U32(r + 0x68);
            e.lo = U32(r + 0x6C);
            e.length = U32(r + 0x58);
            if (e.name.empty() || e.lo == 0xFFFFFFFF) continue;
            if (fat.byKey.find(e.key) == fat.byKey.end()) fat.byKey[e.key] = fat.entries.size();
            std::string low = Lower(e.name);
            if (fat.byName.find(low) == fat.byName.end()) fat.byName[low] = e.key;
            fat.entries.push_back(e);
        }
        pos = next;
    }
    CloseHandle(h);
    return !fat.entries.empty();
}

// the original entry: its header and the bytes after the payload (the reference table)
bool OriginalEntry(uint32_t key, std::vector<uint8_t>* payload, std::vector<uint8_t>& refs, uint32_t hdr[4]) {
    std::map<uint32_t, size_t>::const_iterator it = s_fat.byKey.find(key);
    if (it == s_fat.byKey.end()) return false;
    const Fat::Entry& e = s_fat.entries[it->second];
    uint64_t pos = ((uint64_t)e.hi << 32) | e.lo;
    HANDLE h = CreateFileA(s_bigfile.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) return false;
    uint8_t head[FILE_HDR];
    bool ok = ReadAt(h, pos, head, FILE_HDR);
    if (ok) {
        hdr[0] = U32(head), hdr[1] = U32(head + 4), hdr[2] = U32(head + 8), hdr[3] = U32(head + 12);
        if (payload) {
            payload->resize(hdr[0]);
            ok = hdr[0] == 0 || ReadAt(h, pos + FILE_HDR, &(*payload)[0], hdr[0]);
        }
        if (ok && hdr[2] && hdr[2] < (16 << 20)) {
            refs.resize(hdr[2]);
            ok = ReadAt(h, pos + FILE_HDR + hdr[0], &refs[0], hdr[2]);
        }
    }
    CloseHandle(h);
    return ok;
}

// ------------------------------------------------------------------------------------------------- virtual entries
void PutHeader(std::vector<uint8_t>& out, uint32_t length, uint32_t refBytes, uint32_t flags = 0) {
    uint8_t h[FILE_HDR];
    memset(h, 0, sizeof(h));
    memcpy(h, &length, 4);
    memcpy(h + 4, &length, 4);          // user length = length: stored raw (no LZO), no shortcut
    memcpy(h + 8, &refBytes, 4);
    memcpy(h + 12, &flags, 4);          // the entry's own kind: a video's is 2, and the engine reads them apart
    memset(h + 28, 0xFF, 4);
    out.insert(out.end(), h, h + FILE_HDR);
}

// a package with the overridden record bodies swapped in; false when it holds none of them
bool PatchPackage(const std::vector<uint8_t>& in, const std::map<uint32_t, RecordFile>& records,
                  std::vector<uint8_t>& out, std::string& from, int* replaced) {
    *replaced = 0;
    size_t o = 0;
    bool any = false;
    while (o + 8 <= in.size()) {
        uint32_t key = U32(&in[o]), len = U32(&in[o + 4]);
        if (o + 8 + len > in.size()) return false;             // not a package
        if (records.find(key) != records.end()) any = true;
        o += 8 + len;
    }
    if (!any || o != in.size()) return false;
    out.reserve(in.size());
    o = 0;
    while (o + 8 <= in.size()) {
        uint32_t key = U32(&in[o]), len = U32(&in[o + 4]);
        std::map<uint32_t, RecordFile>::const_iterator r = records.find(key);
        if (r != records.end()) {
            uint32_t n = (uint32_t)r->second.body.size();
            out.insert(out.end(), &in[o], &in[o + 4]);
            out.insert(out.end(), (const uint8_t*)&n, (const uint8_t*)&n + 4);
            out.insert(out.end(), r->second.body.begin(), r->second.body.end());
            if (from.find(r->second.mod) == std::string::npos) from += (from.empty() ? "" : ", ") + r->second.mod;
            ++*replaced;
        } else {
            out.insert(out.end(), &in[o], &in[o + 8 + len]);
        }
        o += 8 + len;
    }
    return true;
}

// the virtual index of a key, building the entry the first time; -1 when nothing replaces it (called under the lock)
int VirtualIndex(uint32_t key) {
    std::map<uint32_t, int>::iterator st = s_keyState.find(key);
    if (st != s_keyState.end()) return st->second;
    int result = -1;
    Virtual* v = NULL;
    std::map<uint32_t, EntryFile>::const_iterator ef = s_entryFiles.find(key);
    if (ef != s_entryFiles.end()) {
        std::vector<uint8_t> data, refs;
        uint32_t hdr[4] = { 0, 0, 0, 0 };
        if (ReadWhole(ef->second.path, data)) {
            // the reference table: <file>.refs next to the file (12-byte references), else the archive entry's
            if (!ReadWhole(ef->second.path + ".refs", refs) || (refs.size() % 12) != 0) {
                refs.clear();
                OriginalEntry(key, NULL, refs, hdr);
            }
            v = new Virtual;
            v->key = key;
            v->from = ef->second.mod + ": " + ef->second.path.substr(ef->second.path.find_last_of("\\/") + 1);
            PutHeader(v->bytes, (uint32_t)data.size(), (uint32_t)refs.size());
            v->bytes.insert(v->bytes.end(), data.begin(), data.end());
            v->bytes.insert(v->bytes.end(), refs.begin(), refs.end());
        } else {
            ModLog("%08X: %s cannot be read, the archive's entry is used", key, ef->second.path.c_str());
        }
    } else if ((key >> 20) == 0xFFF && !s_recordFiles.empty()) {
        std::vector<uint8_t> data, refs, patched;
        uint32_t hdr[4] = { 0, 0, 0, 0 };
        std::string from;
        int replaced = 0;
        if (OriginalEntry(key, &data, refs, hdr) && !(hdr[3] & 5) && hdr[0] == hdr[1] &&
            PatchPackage(data, s_recordFiles, patched, from, &replaced)) {
            v = new Virtual;
            v->key = key;
            v->from = from;
            char n[40];
            _snprintf(n, sizeof(n) - 1, " (%d record%s)", replaced, replaced == 1 ? "" : "s");
            n[sizeof(n) - 1] = 0;
            v->from += n;
            PutHeader(v->bytes, (uint32_t)patched.size(), (uint32_t)refs.size());
            v->bytes.insert(v->bytes.end(), patched.begin(), patched.end());
            v->bytes.insert(v->bytes.end(), refs.begin(), refs.end());
        }
    }
    if (v) {
        result = (int)s_virtual.size();
        s_virtual.push_back(v);
        ModLog("%08X served from %s (%u bytes)", key, v->from.c_str(), (unsigned)v->bytes.size());
    }
    s_keyState[key] = result;
    return result;
}

// The game keeps a track's beats inside the sound itself: a cue chunk of marker positions and a LIST of their
// labels, sitting between the sound's fact chunk and its audio.  Two things follow.  The audio of such a track
// begins thousands of bytes into the file - Bubamara's at 18440 - and the engine reads the stream from where it
// expects that to be, so a replacement without the beats is read from the wrong place and comes out as noise.
// And the beats are what the levels dance to.  So whatever sound is dropped into a mod's music folder, the game's
// own beats are put back into it here, their positions stretched to the length of the new sound; nobody has to
// prepare anything or run a tool of their own.
#define FOURCC(a, b, c, d) ((uint32_t)(a) | ((uint32_t)(b) << 8) | ((uint32_t)(c) << 16) | ((uint32_t)(d) << 24))
const uint32_t CC_RIFF = FOURCC('R', 'I', 'F', 'F');
const uint32_t CC_FACT = FOURCC('f', 'a', 'c', 't');
const uint32_t CC_CUE  = FOURCC('c', 'u', 'e', ' ');
const uint32_t CC_DATA = FOURCC('d', 'a', 't', 'a');
const uint32_t CC_LIST = FOURCC('L', 'I', 'S', 'T');
const uint32_t CC_LTXT = FOURCC('l', 't', 'x', 't');

std::string s_snsFile;                           // the archive the long sounds are streamed from
Fat s_snsFat;
int s_snsState;                                  // 0 not tried, 1 read, -1 not available

struct Chunk { uint32_t id, at, size; };

void RiffChunks(const uint8_t* b, uint32_t n, std::vector<Chunk>& out) {
    uint32_t p = 12;
    while (p + 8 <= n) {
        Chunk c;
        c.id = U32(b + p);
        c.size = U32(b + p + 4);
        c.at = p;
        out.push_back(c);
        if (c.id == CC_DATA) return;             // the audio: everything before it is the header
        uint64_t next = (uint64_t)p + 8 + c.size + (c.size & 1);
        if (next + 8 > n) return;
        p = (uint32_t)next;
    }
}

// The videos are streamed the same way the sounds are, from a sibling of their own, and their entries are not of
// the same kind: a video's header carries flags 2 where a sound's carries none.  The engine reads them apart by
// that, so a replacement has to be handed over as the kind it replaces or it is not played at all.
std::string s_bikFile;
Fat s_bikFat;
int s_bikState;

bool BikFat() {
    if (s_bikState) return s_bikState > 0;
    s_bikState = -1;
    size_t dot = s_bigfile.find_last_of('.');
    if (dot == std::string::npos) return false;
    s_bikFile = s_bigfile.substr(0, dot) + ".$hd$.bik.bf";
    std::string why;
    if (!LoadFat(s_bikFile, s_bikFat, why)) return false;
    s_bikState = 1;
    return true;
}

bool SnsFat();

// the flags the archive itself gives an entry of one of the siblings
bool SiblingFlags(uint32_t key, uint32_t& flags) {
    for (int which = 0; which < 2; ++which) {
        Fat* fat = NULL;
        const std::string* path = NULL;
        if (which == 0 && SnsFat()) { fat = &s_snsFat; path = &s_snsFile; }
        if (which == 1 && BikFat()) { fat = &s_bikFat; path = &s_bikFile; }
        if (!fat) continue;
        std::map<uint32_t, size_t>::const_iterator it = fat->byKey.find(key);
        if (it == fat->byKey.end()) continue;
        const Fat::Entry& e = fat->entries[it->second];
        uint64_t pos = ((uint64_t)e.hi << 32) | e.lo;
        HANDLE h = CreateFileA(path->c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                               OPEN_EXISTING, 0, NULL);
        if (h == INVALID_HANDLE_VALUE) continue;
        uint8_t head[FILE_HDR];
        bool ok = ReadAt(h, pos, head, FILE_HDR);
        CloseHandle(h);
        if (!ok) continue;
        flags = U32(head + 12);
        return true;
    }
    return false;
}

bool SnsFat() {
    if (s_snsState) return s_snsState > 0;
    s_snsState = -1;
    size_t dot = s_bigfile.find_last_of('.');
    if (dot == std::string::npos) return false;
    s_snsFile = s_bigfile.substr(0, dot) + ".wii.sns.bf";
    std::string why;
    if (!LoadFat(s_snsFile, s_snsFat, why)) {
        ModLog("the streamed archive %s cannot be read (%s): a sound's beats are left to the sound itself",
               s_snsFile.c_str(), why.c_str());
        return false;
    }
    s_snsState = 1;
    return true;
}

// the game's own beat chunks for a sound, and the number of samples they were written for
bool OriginalBeats(uint32_t key, std::vector<uint8_t>& beats, uint32_t& samples) {
    beats.clear();
    samples = 0;
    if (!SnsFat()) return false;
    std::map<uint32_t, size_t>::const_iterator it = s_snsFat.byKey.find(key);
    if (it == s_snsFat.byKey.end()) return false;
    const Fat::Entry& e = s_snsFat.entries[it->second];
    uint64_t pos = ((uint64_t)e.hi << 32) | e.lo;
    uint32_t want = 64 * 1024;                   // a header is never near this big
    if (e.length && e.length < want) want = e.length;
    if (want < 64) return false;
    HANDLE h = CreateFileA(s_snsFile.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                           OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) return false;
    std::vector<uint8_t> head(want);
    bool ok = ReadAt(h, pos + FILE_HDR, &head[0], want);
    CloseHandle(h);
    if (!ok) return false;
    std::vector<Chunk> cs;
    RiffChunks(&head[0], want, cs);
    uint32_t from = 0, to = 0;
    for (size_t i = 0; i < cs.size(); ++i) {
        if (cs[i].id == CC_FACT && cs[i].size >= 4) samples = U32(&head[0] + cs[i].at + 8);
        if (cs[i].id == CC_CUE && !from) from = cs[i].at;
        if (cs[i].id == CC_DATA) { to = cs[i].at; break; }
    }
    if (!from || to <= from) return false;
    beats.assign(head.begin() + from, head.begin() + to);
    return true;
}

// the marker positions moved to where they fall in a sound of another length
void StretchBeats(std::vector<uint8_t>& b, double f) {
    if (f <= 0 || b.size() < 12) return;
    uint32_t p = 0;
    while (p + 8 <= b.size()) {
        uint32_t id = U32(&b[0] + p), size = U32(&b[0] + p + 4);
        uint64_t next = (uint64_t)p + 8 + size + (size & 1);
        if (id == CC_CUE && size >= 4) {
            uint32_t n = U32(&b[0] + p + 8);
            for (uint32_t i = 0; i < n && p + 12 + (i + 1) * 24 <= b.size(); ++i) {
                uint8_t* pt = &b[0] + p + 12 + i * 24;
                uint32_t at = (uint32_t)(U32(pt + 4) * f + 0.5), off = (uint32_t)(U32(pt + 20) * f + 0.5);
                memcpy(pt + 4, &at, 4);
                memcpy(pt + 20, &off, 4);
            }
        } else if (id == CC_LIST && size >= 4) { // the labels: an ltxt carries the length of its own marker
            uint32_t q = p + 12;
            while (q + 8 <= p + 8 + size && q + 8 <= b.size()) {
                uint32_t sid = U32(&b[0] + q), ssz = U32(&b[0] + q + 4);
                if (sid == CC_LTXT && ssz >= 8 && q + 16 <= b.size()) {
                    uint32_t len = (uint32_t)(U32(&b[0] + q + 12) * f + 0.5);
                    memcpy(&b[0] + q + 12, &len, 4);
                }
                uint64_t sn = (uint64_t)q + 8 + ssz + (ssz & 1);
                if (sn <= q) break;
                q = (uint32_t)sn;
            }
        }
        if (next <= p) break;
        p = (uint32_t)next;
    }
}

// a sound from a mod, with the game's beats put into it when it brought none of its own
int PutBeatsIn(std::vector<uint8_t>& snd, uint32_t key) {
    if (snd.size() < 44 || U32(&snd[0]) != CC_RIFF) return 0;
    std::vector<Chunk> cs;
    RiffChunks(&snd[0], (uint32_t)snd.size(), cs);
    uint32_t dataAt = 0, samples = 0;
    for (size_t i = 0; i < cs.size(); ++i) {
        if (cs[i].id == CC_CUE) return 0;        // it has beats of its own: left alone
        if (cs[i].id == CC_FACT && cs[i].size >= 4) samples = U32(&snd[0] + cs[i].at + 8);
        if (cs[i].id == CC_DATA) dataAt = cs[i].at;
    }
    if (!dataAt) return 0;
    std::vector<uint8_t> beats;
    uint32_t was = 0;
    if (!OriginalBeats(key, beats, was) || beats.size() < 12) return 0;
    if (was && samples) StretchBeats(beats, (double)samples / (double)was);
    std::vector<uint8_t> out;
    out.reserve(snd.size() + beats.size());
    out.insert(out.end(), snd.begin(), snd.begin() + dataAt);
    out.insert(out.end(), beats.begin(), beats.end());
    out.insert(out.end(), snd.begin() + dataAt, snd.end());
    uint32_t riff = (uint32_t)out.size() - 8;
    memcpy(&out[4], &riff, 4);
    snd.swap(out);
    return (int)U32(&beats[0] + 8);
}

// The music and the other long sounds are not in the main archive: its entry for one of them is a shadow, a path
// like <$shadow$>/8e/d4/8e0022d4.$hd$.sns into the sibling <base>.wii.sns.bf, which is a bigfile of its own that
// the engine streams from.  A mod replaces one by putting the new sound in mods\<mod>\music under the key or the
// name the main archive gives it; this answers the sibling's own lookups the same way the entries above answer
// the main archive's.  The engine takes the length from the header synthesised here rather than from the file
// table, so a replacement may be longer than what it replaces.
int VirtualIndexSns(uint32_t key) {
    std::map<uint32_t, int>::iterator st = s_musicState.find(key);
    if (st != s_musicState.end()) return st->second;
    int result = -1;
    std::map<uint32_t, EntryFile>::const_iterator mf = s_musicFiles.find(key);
    if (mf != s_musicFiles.end()) {
        std::vector<uint8_t> data;
        if (ReadWhole(mf->second.path, data)) {
            int beats = PutBeatsIn(data, key);   // the game's own beats, stretched to this sound
            Virtual* v = new Virtual;
            v->key = key;
            v->from = mf->second.mod + ": " + mf->second.path.substr(mf->second.path.find_last_of("\/") + 1);
            uint32_t flags = 0;
            SiblingFlags(key, flags);            // a video's kind is not a sound's: hand it over as what it is
            PutHeader(v->bytes, (uint32_t)data.size(), 0, flags);
            v->bytes.insert(v->bytes.end(), data.begin(), data.end());
            result = (int)s_virtual.size();
            s_virtual.push_back(v);
            if (beats)
                ModLog("%08X streamed from %s (%u bytes, the game's %d beats stretched into it)", key,
                       v->from.c_str(), (unsigned)v->bytes.size(), beats);
            else
                ModLog("%08X streamed from %s (%u bytes)", key, v->from.c_str(), (unsigned)v->bytes.size());
        } else {
            ModLog("%08X: %s cannot be read, the archive's own sound is used", key, mf->second.path.c_str());
        }
    }
    s_musicState[key] = result;
    return result;
}

// ------------------------------------------------------------------------------------------------- the hooks
std::vector<uint32_t> s_worldsSeen;             // the world packages the engine has looked up (newest last)

int64_t* __fastcall KeySearchHook(void* self, void* edx, int64_t* out, uint32_t key) {
    if ((key >> 20) == 0xFFF) {                 // a world's package: remember which levels have been asked for
        Lock lock;
        if (s_worldsSeen.empty() || s_worldsSeen.back() != key) {
            for (size_t i = 0; i < s_worldsSeen.size(); ++i)
                if (s_worldsSeen[i] == key) { s_worldsSeen.erase(s_worldsSeen.begin() + i); break; }
            if (s_worldsSeen.size() >= 64) s_worldsSeen.erase(s_worldsSeen.begin());
            s_worldsSeen.push_back(key);
        }
    }
    bool main = self == *(void**)(uintptr_t)G_MAIN_BIG;
    if (main || !s_musicFiles.empty()) {
        int idx;
        {
            Lock lock;
            idx = main ? VirtualIndex(key) : VirtualIndexSns(key);
        }
        if (idx >= 0) {
            uint32_t* w = (uint32_t*)out;
            w[0] = VIRTUAL_HI + (uint32_t)idx;      // {hi, lo}
            w[1] = 0;
            return out;
        }
    }
    return s_origKeySearch(self, edx, out, key);
}

DWORD WINAPI SetFilePointerHook(HANDLE h, LONG lo, PLONG hi, DWORD method) {
    if (method == FILE_BEGIN && hi) {
        uint32_t hv = (uint32_t)*hi;
        if (hv >= VIRTUAL_HI) {
            Lock lock;
            uint32_t idx = hv - VIRTUAL_HI;
            if (idx < s_virtual.size()) {
                HandleState& st = s_handles[h];
                st.index = (int)idx;
                st.offset = (uint32_t)lo;
                return (DWORD)lo;
            }
        }
    }
    {
        Lock lock;
        std::map<HANDLE, HandleState>::iterator it = s_handles.find(h);
        if (it != s_handles.end()) {
            if (method == FILE_CURRENT) {
                int64_t delta = hi ? (((int64_t)*hi << 32) | (uint32_t)lo) : (int64_t)lo;
                int64_t at = (int64_t)it->second.offset + delta;
                if (at < 0) at = 0;
                it->second.offset = (uint64_t)at;
                if (hi) *hi = (LONG)(VIRTUAL_HI + (uint32_t)it->second.index);
                return (DWORD)(uint32_t)at;
            }
            s_handles.erase(it);                     // a real position: back to the archive
        }
    }
    return s_origSetFilePointer(h, lo, hi, method);
}

BOOL WINAPI ReadFileHook(HANDLE h, LPVOID buf, DWORD n, LPDWORD got, LPOVERLAPPED ov) {
    {
        Lock lock;
        std::map<HANDLE, HandleState>::iterator it = s_handles.find(h);
        if (it != s_handles.end() && !ov) {
            const Virtual* v = s_virtual[it->second.index];
            uint64_t off = it->second.offset;
            DWORD k = off >= v->bytes.size() ? 0 : (DWORD)std::min<uint64_t>(n, v->bytes.size() - off);
            if (k) memcpy(buf, &v->bytes[(size_t)off], k);
            it->second.offset = off + k;
            if (got) *got = k;
            ++s_served;
            return TRUE;
        }
    }
    return s_origReadFile(h, buf, n, got, ov);
}

BOOL WINAPI CloseHandleHook(HANDLE h) {
    {
        Lock lock;
        s_handles.erase(h);
    }
    return s_origCloseHandle(h);
}

// ------------------------------------------------------------------------------------------------- the mod folders
bool ParseKey(const std::string& stem, uint32_t* key) {
    if (stem.size() != 8) return false;
    uint32_t v = 0;
    for (size_t i = 0; i < 8; ++i) {
        char c = stem[i];
        int d = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
        if (d < 0) return false;
        v = (v << 4) | (uint32_t)d;
    }
    *key = v;
    return true;
}

std::vector<std::string> SplitList(const std::string& s) {
    std::vector<std::string> out;
    std::string cur;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '|') { if (!cur.empty()) out.push_back(cur); cur.clear(); }
        else cur += s[i];
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

void ScanFolder(const std::string& mod, const std::string& folder, int kind, int* count) {
    bool records = kind == 1;
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA((folder + "\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        std::string name = fd.cFileName, path = folder + "\\" + name;
        if (name.size() > 5 && _stricmp(name.c_str() + name.size() - 5, ".refs") == 0) continue;   // a reference table
        size_t dot = name.find_last_of('.');
        std::string stem = dot == std::string::npos ? name : name.substr(0, dot);
        uint32_t key = 0;
        if (!ParseKey(stem, &key)) {
            std::map<std::string, uint32_t>::const_iterator it = s_fat.byName.find(Lower(name));
            if (records || it == s_fat.byName.end()) {
                ModLog("%s: %s is not a key (8 hex digits)%s: ignored", mod.c_str(), name.c_str(),
                       records ? "" : " nor an entry name of the archive");
                continue;
            }
            key = it->second;
        }
        if (records) {
            RecordFile r;
            r.mod = mod;
            r.path = path;
            if (!ReadWhole(path, r.body)) {
                ModLog("%s: %s cannot be read: ignored", mod.c_str(), name.c_str());
                continue;
            }
            s_recordFiles[key] = r;
        } else {
            EntryFile e;
            e.mod = mod;
            e.path = path;
            if (kind == 2) s_musicFiles[key] = e;
            else s_entryFiles[key] = e;
        }
        ++*count;
    } while (FindNextFileA(h, &fd));
    FindClose(h);
}

void ReadEnabledList() {
    std::string list;
    ReadIniKey(g_dllDir + "options.ini", "mods", "enabled", list);
    s_mods = SplitList(list);
}

void ScanMods() {
    int entries = 0, records = 0, sounds = 0;
    for (size_t i = 0; i < s_mods.size(); ++i) {
        std::string folder = g_dllDir + "mods\\" + s_mods[i];
        DWORD a = GetFileAttributesA(folder.c_str());
        if (a == INVALID_FILE_ATTRIBUTES || !(a & FILE_ATTRIBUTE_DIRECTORY)) {
            ModLog("enabled mod \"%s\" has no folder under mods: skipped", s_mods[i].c_str());
            continue;
        }
        int ne = 0, nr = 0, nm = 0;
        ScanFolder(s_mods[i], folder + "\\entries", 0, &ne);
        ScanFolder(s_mods[i], folder + "\\records", 1, &nr);
        ScanFolder(s_mods[i], folder + "\\music", 2, &nm);
        ScanFolder(s_mods[i], folder + "\\video", 2, &nm);   // the same path: a sibling archive streams both
        ModLog("mod \"%s\": %d entr%s, %d record%s, %d sound%s", s_mods[i].c_str(),
               ne, ne == 1 ? "y" : "ies", nr, nr == 1 ? "" : "s", nm, nm == 1 ? "" : "s");
        entries += ne;
        records += nr;
        sounds += nm;
    }
    ModLog("%u mod%s enabled (options.ini [mods]): %d entry file%s, %d record file%s, %d sound%s",
           (unsigned)s_mods.size(), s_mods.size() == 1 ? "" : "s", entries, entries == 1 ? "" : "s",
           records, records == 1 ? "" : "s", sounds, sounds == 1 ? "" : "s");
}

// ------------------------------------------------------------------------------------------------- install
bool InstallHooks(std::string& why) {
    // the detour: the first 10 bytes of i64_KeySearchPos run in a trampoline, then the function continues
    uint8_t* tramp = (uint8_t*)VirtualAlloc(NULL, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!tramp) { why = "no trampoline memory"; return false; }
    ProcessImage img;
    if (!img.Read(KEY_SEARCH, tramp, 10)) { why = "cannot read the key search"; return false; }
    tramp[10] = 0xE9;
    int32_t rel = (int32_t)(KEY_SEARCH_TAIL - ((uint32_t)(uintptr_t)tramp + 15));
    memcpy(tramp + 11, &rel, 4);
    s_origKeySearch = (KeySearchFn)tramp;
    // the imports first (they only act on virtual positions, which none exist yet)
    HMODULE k32 = GetModuleHandleA("kernel32.dll");
    void* sfp = *(void**)(uintptr_t)IAT_SET_FILE_PTR;
    void* rf = *(void**)(uintptr_t)IAT_READ_FILE;
    void* ch = *(void**)(uintptr_t)IAT_CLOSE_HANDLE;
    if (!k32 || sfp != (void*)GetProcAddress(k32, "SetFilePointer") || rf != (void*)GetProcAddress(k32, "ReadFile") ||
        ch != (void*)GetProcAddress(k32, "CloseHandle")) {
        why = "the file imports are not the kernel32 functions (another hook is installed)";
        return false;
    }
    s_origSetFilePointer = (SetFilePointerFn)sfp;
    s_origReadFile = (ReadFileFn)rf;
    s_origCloseHandle = (CloseHandleFn)ch;
    void* hsfp = (void*)&SetFilePointerHook;
    void* hrf = (void*)&ReadFileHook;
    void* hch = (void*)&CloseHandleHook;
    if (!WriteCode(IAT_SET_FILE_PTR, &hsfp, 4) || !WriteCode(IAT_READ_FILE, &hrf, 4) || !WriteCode(IAT_CLOSE_HANDLE, &hch, 4)) {
        why = "the import table is not writable";
        return false;
    }
    uint8_t jmp[10] = { 0xE9, 0, 0, 0, 0, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC };
    int32_t r2 = (int32_t)((uint32_t)(uintptr_t)&KeySearchHook - (KEY_SEARCH + 5));
    memcpy(jmp + 1, &r2, 4);
    if (!WriteCode(KEY_SEARCH, jmp, 10)) { why = "the key search is not writable"; return false; }
    return true;
}

}  // namespace

int ModsVerify(const Image& img, std::string& rep) {
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

void ModsAttach(const std::string& iniPath) {
    s_inAttach = true;
    if (!s_csInit) { InitializeCriticalSection(&s_cs); s_csInit = true; }
    std::string v;
    if (ReadIniKey(iniPath, "mods", "enabled", v)) s_enabled = atoi(v.c_str()) != 0;
    if (ReadIniKey(iniPath, "mods", "log", v)) s_log = atoi(v.c_str()) != 0;
    if (s_enabled) ReadEnabledList();
    ProcessImage img;
    uint8_t probe[4];
    if (!s_enabled || !img.Read(KEY_SEARCH, probe, 4)) {
        if (!s_enabled) ModLog("off ([mods] enabled=0)");
        s_inAttach = false;
        return;
    }
    std::string rep;
    int bad = ModsVerify(img, rep);
    if (bad) {
        ModLog("PC executable differs at %d site%s: mods are NOT loaded", bad, bad == 1 ? "" : "s");
        s_inAttach = false;
        return;
    }
    s_bigfile = BigfileFromCommandLine();
    if (!s_bigfile.empty() && s_bigfile.find(':') == std::string::npos && s_bigfile[0] != '\\')
        s_bigfile = g_dllDir + s_bigfile;
    std::string why;
    if (s_bigfile.empty() || !LoadFat(s_bigfile, s_fat, why)) {
        ModLog("the archive \"%s\" could not be read (%s): mods are NOT loaded", s_bigfile.c_str(), why.c_str());
        s_inAttach = false;
        return;
    }
    ScanMods();
    if (s_entryFiles.empty() && s_recordFiles.empty() && s_musicFiles.empty()) {
        ModLog("nothing to load: the hooks stay out");
        s_inAttach = false;
        return;
    }
    if (!InstallHooks(why)) ModLog("hooks NOT installed: %s", why.c_str());
    else {
        s_installed = true;
        ModLog("loader on: archive %s (%u entries in its file table)", s_bigfile.c_str(), (unsigned)s_fat.entries.size());
    }
    s_inAttach = false;
}

void ModsAfterConfig() {
    for (size_t i = 0; i < s_pending.size(); ++i) if (s_log) Log("MODS: %s", s_pending[i].c_str());
    s_pending.clear();
}

bool ModsInstalled() { return s_installed; }

const std::vector<std::string>& ModsEnabled() { return s_mods; }

std::string ModsFolder(const std::string& mod) { return g_dllDir + "mods\\" + mod; }

bool ModsEnabledIs(const std::string& mod) {
    for (size_t i = 0; i < s_mods.size(); ++i)
        if (_stricmp(s_mods[i].c_str(), mod.c_str()) == 0) return true;
    return false;
}

// A mod's own settings, out of its own folder: mods\<mod>\config.ini.  A mod keeps what it needs beside itself
// rather than in wiimote.ini, which is the platform's own file - so a mod can be copied, shared or thrown away in
// one piece.  `section` may be empty for keys written before any [section].
bool ModsSetting(const std::string& mod, const char* section, const char* key, std::string& value) {
    return ReadIniKey(ModsFolder(mod) + "\\config.ini", section && *section ? section : "", key, value);
}

int ModsSettingInt(const std::string& mod, const char* section, const char* key, int fallback) {
    std::string v;
    if (!ModsSetting(mod, section, key, v)) return fallback;
    const char* s = v.c_str();
    while (*s == ' ' || *s == '\t') ++s;
    if (!*s) return fallback;
    return (int)strtol(s, NULL, 0);
}

// the first file with this extension in the mod's folder (so a mod can simply carry the file it needs)
std::string ModsFileByExt(const std::string& mod, const char* ext) {
    std::string dir = ModsFolder(mod);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA((dir + "\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return "";
    std::string found;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        const char* dot = strrchr(fd.cFileName, '.');
        if (dot && _stricmp(dot + 1, ext) == 0) { found = dir + "\\" + fd.cFileName; break; }
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    return found;
}

bool ModsWorldSeen(uint32_t key) {              // has the engine loaded this world's package? (low 20 bits)
    Lock lock;
    for (size_t i = 0; i < s_worldsSeen.size(); ++i)
        if ((s_worldsSeen[i] & 0xFFFFF) == (key & 0xFFFFF)) return true;
    return false;
}

extern "C" {

// wmtest only: verify the sites against an exe file.  Returns the number of differences.
int __cdecl WiimoteModsCheckExe(const char* exePath, char* out, int outSize) {
    FileImage img;
    std::string rep;
    int bad = -1;
    if (!img.Load(exePath)) rep = "  FAIL  cannot read " + std::string(exePath ? exePath : "(null)") + "\n";
    else bad = ModsVerify(img, rep);
    if (out && outSize > 0) {
        size_t n = rep.size() < (size_t)outSize - 1 ? rep.size() : (size_t)outSize - 1;
        memcpy(out, rep.data(), n);
        out[n] = 0;
    }
    return bad;
}

// wmtest only: the pure parts - the header, package patching, key parsing, the enabled list, the command line.
int __cdecl WiimoteModsSelfTest(char* out, int outSize) {
    std::string rep;
    int fails = 0;
    // a package of three records, the middle one replaced by a longer body
    std::vector<uint8_t> pkg;
    const uint32_t keys[3] = { 0x11111111, 0x22222222, 0x33333333 };
    const char* bodies[3] = { "abc", "defgh", "ij" };
    for (int i = 0; i < 3; ++i) {
        uint32_t len = (uint32_t)strlen(bodies[i]);
        pkg.insert(pkg.end(), (const uint8_t*)&keys[i], (const uint8_t*)&keys[i] + 4);
        pkg.insert(pkg.end(), (const uint8_t*)&len, (const uint8_t*)&len + 4);
        pkg.insert(pkg.end(), (const uint8_t*)bodies[i], (const uint8_t*)bodies[i] + len);
    }
    std::map<uint32_t, RecordFile> recs;
    RecordFile r;
    r.mod = "test";
    r.body.assign((const uint8_t*)"REPLACED", (const uint8_t*)"REPLACED" + 8);
    recs[0x22222222] = r;
    std::vector<uint8_t> outPkg;
    std::string from;
    int replaced = 0;
    bool ok = PatchPackage(pkg, recs, outPkg, from, &replaced) && replaced == 1 && outPkg.size() == pkg.size() + 3 &&
              U32(&outPkg[0]) == keys[0] && U32(&outPkg[11]) == keys[1] && U32(&outPkg[15]) == 8 &&
              !memcmp(&outPkg[19], "REPLACED", 8) && U32(&outPkg[27]) == keys[2] && from == "test";
    Report(rep, ok, "package record replaced in place (3 records, the middle one grows by 3 bytes)");
    fails += !ok;
    std::map<uint32_t, RecordFile> none;
    none[0x44444444] = r;
    ok = !PatchPackage(pkg, none, outPkg, from, &replaced);
    Report(rep, ok, "a package without the record is left to the archive");
    fails += !ok;
    std::vector<uint8_t> notPkg(pkg.begin(), pkg.begin() + 10);
    ok = !PatchPackage(notPkg, recs, outPkg, from, &replaced);
    Report(rep, ok, "a truncated package is not patched");
    fails += !ok;
    // the header
    std::vector<uint8_t> h;
    PutHeader(h, 1234, 24);
    ok = h.size() == 32 && U32(&h[0]) == 1234 && U32(&h[4]) == 1234 && U32(&h[8]) == 24 && U32(&h[12]) == 0 &&
         U32(&h[28]) == 0xFFFFFFFF;
    Report(rep, ok, "file header: raw, no shortcut, 24 reference bytes, 0xFFFFFFFF tail");
    fails += !ok;
    // keys and the list
    uint32_t k = 0;
    ok = ParseKey("FFF01D10", &k) && k == 0xFFF01D10 && ParseKey("0163ca8b", &k) && k == 0x0163CA8B &&
         !ParseKey("default", &k) && !ParseKey("FFF01D1", &k) && !ParseKey("FFF01D1G", &k);
    Report(rep, ok, "key file names: 8 hex digits, either case");
    fails += !ok;
    std::vector<std::string> l = SplitList("Example mod|Second|");
    ok = l.size() == 2 && l[0] == "Example mod" && l[1] == "Second" && SplitList("").empty();
    Report(rep, ok, "options.ini [mods] enabled=a|b list");
    fails += !ok;
    if (out && outSize > 0) {
        size_t m = rep.size() < (size_t)outSize - 1 ? rep.size() : (size_t)outSize - 1;
        memcpy(out, rep.data(), m);
        out[m] = 0;
    }
    return fails;
}

}  // extern "C"
