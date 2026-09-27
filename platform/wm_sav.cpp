// wm_sav.cpp - the Wii save natives (SAV_*) for the RGH PC executable, implemented on the PC save files in <exe dir>/sav.
//
// The RGH PC engine stubs the Wii save layer: nothing allocates the slot tables (0x00A93734), SAV_InitSystem /
// SAV_Step call a "return 1" body, SAV_GetSlotDataEx returns 0, SAV_SaveSlotEx / SetSlotUserDataEx /
// SetSlotUserBufferEx / UpdateValid do nothing.  The Wii scripts (GST_SaveManager, MapStartup_MagmaMenu) need the Wii
// behaviour (Wii executable: SAV_b_InitSystem 80071AC8, SAV_i_Step 80071BDC, SAV_u32_GetSlotData 80071170,
// SAV_b_WriteSlot 80072480 ...).  This module replaces those VM handlers with Wii-equivalent ones.  The file format is
// the one the PC readers expect (FUN_006f4830 / FUN_006f49a0), see docs/platform.md section 12.
#include "wiimote.h"

#include <cctype>
#include <cstdarg>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <vector>

namespace {

// ---------------------------------------------------------------------------------------------------------------
// layout (identical on the Wii, little-endian on PC)
// ---------------------------------------------------------------------------------------------------------------
using namespace wmpatch;                // Image, Crc32, Report, CheckCrc, WriteCode, PatchNativeTable (wm_patch.cpp)

const int      SAV_TABLES = 6;
const uint32_t REC_SIZE   = 0xC0;
const uint32_t UB_SIZE    = 0x2020;     // user buffer block: u32 -1, u32 -1, u32 0x2014, 0x2014 bytes
const uint32_t UB_DATA    = 0x2014;
const uint16_t REC_MAGIC  = 0x67;
const int      NAME_UNITS = 64;         // +0x38 .. +0xB7

#pragma pack(push, 1)
struct SavRecord {
    uint16_t magic;         // +00 0x67
    uint16_t pad02;
    uint32_t valid;         // +04 field 0
    uint8_t  sec;           // +08 field 1
    uint8_t  min;           // +09 field 2
    uint8_t  hour;          // +0A field 3
    uint8_t  day;           // +0B field 4
    uint8_t  month;         // +0C field 5
    uint8_t  pad0d;
    uint16_t year;          // +0E field 6
    uint32_t user[10];      // +10 fields 100..109
    uint16_t name[NAME_UNITS];  // +38
    uint32_t dataSize;      // +B8 bytes of entry data after the header (and the user buffer)
    uint32_t userBuffer;    // +BC runtime pointer to the 0x2020 user buffer (0 in files)
};
#pragma pack(pop)
C_ASSERT(sizeof(SavRecord) == REC_SIZE);
C_ASSERT(offsetof(SavRecord, user) == 0x10);
C_ASSERT(offsetof(SavRecord, name) == 0x38);
C_ASSERT(offsetof(SavRecord, dataSize) == 0xB8);

struct SavTime { uint8_t sec, min, hour, day, month; uint16_t year; };

// Everything the handlers touch goes through this context: PC executable addresses in the game, fakes in the self-test.
struct SavCtx {
    uint32_t* vmIndex;      // 0x00A7E188 entry index
    uint32_t* vmOffset;     // 0x00A7E18C data offset
    uint32_t* vmData;       // 0x00A7E198 cell: data buffer base
    uint32_t* vmPtrs;       // 0x00A7E19C cell: entry pointer array
    uint32_t* natives;      // 0x00A718AC TOOsarray {data, elem size, capacity, count}
    uint32_t* cfg;          // 0x00A93698 = Wii SAV_gt_Config (+0 n, +4 req count, +1C slot size, +4C merged,
                            //   +6C alloc count, +84 user buffer size) followed by the 6 table pointers (+9C)
    uint32_t* save;         // 0x00A9374C = Wii SAV_gt_SaveContext (+0 tmp buf, +4 cap, +8 size, ..., +18 init done)
                            //   followed by the PC first-boot flag (+1C)
    void*       (*alloc)(uint32_t size);
    void        (*release)(void* p);
    const char* (*execPath)();
    void        (*now)(SavTime& t);
    int         (*readHeader)(int slot, int table, bool merged);   // FUN_006f4910 / FUN_006f4ac0
    int         (*readSlot)(int slot, int table, bool merged);     // FUN_006f4830 / FUN_006f49a0
    int         (*writeHeaderFile)(int slot, int table);           // FUN_006f4690 (per-slot file)
    void        (*deleteSlot)(int slot, int table);                // SAV_DeleteSlotEx_C 005D5BF0
    void        (*getUserBuffer)(int slot, int table, uint32_t* data, uint32_t* size, uint32_t* cur);  // FUN_006eb950
    const uint16_t* emptyName;                                     // L"Empty" 0x008CC128 (Wii @10552)
};

SavCtx   s_game;
SavCtx*  s_ctx = &s_game;
bool     s_bootDone;                    // our SAV_InitSystem ran (Wii: RVL_BootUpFlow initialized)
uint32_t s_tableOwn[SAV_TABLES];        // table pointer we allocated
uint32_t s_tableCap[SAV_TABLES];        // its slot count
bool     s_channelInstalled = true;
bool     s_inAttach;                    // DllMain: buffer log lines until the config is loaded
std::vector<std::string> s_pending;
std::string* s_testLog;                 // self-test: collect log lines here
bool     s_testVerbose;
std::string s_lastCall;
unsigned s_lastRepeat;
char     s_saveDir[MAX_PATH + 16];

inline SavCtx& C() { return *s_ctx; }

inline uint32_t& CfgN()            { return C().cfg[0]; }
inline uint32_t& ReqCount(int t)   { return C().cfg[1 + t]; }    // +0x04
inline uint32_t& SlotSize(int t)   { return C().cfg[7 + t]; }    // +0x1C
inline uint32_t& Merged(int t)     { return C().cfg[19 + t]; }   // +0x4C
inline uint32_t& AllocCount(int t) { return C().cfg[27 + t]; }   // +0x6C
inline uint32_t& UbSize(int t)     { return C().cfg[33 + t]; }   // +0x84
inline uint32_t& TablePtr(int t)   { return C().cfg[39 + t]; }   // +0x9C = 0x00A93734
inline uint32_t& TmpBuf()          { return C().save[0]; }
inline uint32_t& TmpSize()         { return C().save[2]; }
inline uint32_t& InitDone()        { return C().save[6]; }       // 0x00A93764
inline uint32_t& FirstBoot()       { return C().save[7]; }       // 0x00A93768

// ---------------------------------------------------------------------------------------------------------------
// log
// ---------------------------------------------------------------------------------------------------------------
void VFormat(char* buf, size_t n, const char* fmt, va_list ap) {
    _vsnprintf(buf, n - 1, fmt, ap);
    buf[n - 1] = 0;
}

void SavLog(const char* fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    VFormat(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (s_testLog) {
        *s_testLog += "    ";
        *s_testLog += buf;
        *s_testLog += "\n";
    } else if (s_inAttach) {
        s_pending.push_back(buf);
    } else {
        Log("SAV: %s", buf);
    }
}

// Every SAV call (log_verbose=1).  Identical consecutive lines (SAV_Step polling) are counted, not repeated.
void SavCall(const char* fmt, ...) {
    bool on = s_testLog ? s_testVerbose : (g_cfg.log && g_cfg.logVerbose);
    if (!on) return;
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    VFormat(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (s_lastCall == buf) {
        ++s_lastRepeat;
        return;
    }
    if (s_lastRepeat) SavLog("  (previous call repeated %u more times)", s_lastRepeat);
    s_lastRepeat = 0;
    s_lastCall = buf;
    SavLog("%s", buf);
}

// ---------------------------------------------------------------------------------------------------------------
// script VM protocol (handlers 005D64B0 & co): pop = last argument first, push = one int result
// ---------------------------------------------------------------------------------------------------------------
inline uint32_t* VmPtrArray() { return (uint32_t*)(uintptr_t)*C().vmPtrs; }

int32_t PopInt() {
    uint32_t idx = *C().vmIndex - 1;
    *C().vmIndex = idx;
    *C().vmOffset -= 4;
    return *(int32_t*)(uintptr_t)VmPtrArray()[idx];
}

uint32_t* PopRef(uint32_t dataSize) {           // SCR_tt_FixedBuffer_ (12 bytes): pointer to the copy, no deref
    uint32_t idx = *C().vmIndex - 1;
    *C().vmIndex = idx;
    *C().vmOffset -= dataSize;
    return (uint32_t*)(uintptr_t)VmPtrArray()[idx];
}

void PushInt(int32_t v) {
    uint8_t* data = (uint8_t*)(uintptr_t)*C().vmData;
    uint32_t off = *C().vmOffset;
    memcpy(data + off, &v, 4);
    VmPtrArray()[*C().vmIndex] = (uint32_t)(uintptr_t)(data + off);
    *C().vmIndex += 1;
    *C().vmOffset += 4;
}

// Native word of the current call (the compiled script holds the TOOsarray rank at ip, see SCR::ApplyCommon).
uint32_t CurrentWord(void* ip) {
    uint32_t* n = C().natives;
    if (!n || !ip || !n[0] || n[1] < 8) return 0;
    uint32_t rank = *(uint32_t*)ip;
    if (rank >= n[3]) return 0;
    return *(uint32_t*)(uintptr_t)(n[0] + rank * n[1]);
}

// ---------------------------------------------------------------------------------------------------------------
// slot tables
// ---------------------------------------------------------------------------------------------------------------
SavRecord* Rec(int slot, int t) {
    if ((unsigned)t >= (unsigned)SAV_TABLES) return NULL;
    uint32_t p = TablePtr(t);
    if (!p || p != s_tableOwn[t] || (unsigned)slot >= s_tableCap[t]) return NULL;
    return (SavRecord*)(uintptr_t)(p + (uint32_t)slot * REC_SIZE);
}

// Wii SAV_u32_GetSlotData 80071170 (plus the NULL table / slot bounds checks the Wii does not have)
int32_t GetSlotData(int slot, int t, int field) {
    SavRecord* r = Rec(slot, t);
    if (!r) return 0;
    switch (field) {
    case 0: return (int32_t)r->valid;
    case 1: return r->sec;
    case 2: return r->min;
    case 3: return r->hour;
    case 4: return r->day;
    case 5: return r->month;
    case 6: return r->year;
    default:
        if ((unsigned)(field - 100) < 10u) return (int32_t)r->user[field - 100];
        return 0;
    }
}

// ---------------------------------------------------------------------------------------------------------------
// files: <STD_IOGetExecPath()>/sav/slt_<slot>_<table>.sav and slt_xx_<table>.sav (merged)
// ---------------------------------------------------------------------------------------------------------------
const char* SaveDir() {
    if (!s_saveDir[0]) {
        const char* base = C().execPath ? C().execPath() : NULL;
        _snprintf(s_saveDir, sizeof(s_saveDir) - 1, "%s/sav", base ? base : ".");
        s_saveDir[sizeof(s_saveDir) - 1] = 0;
        if (!CreateDirectoryA(s_saveDir, NULL) && GetLastError() != ERROR_ALREADY_EXISTS)
            SavLog("cannot create %s (error %lu)", s_saveDir, GetLastError());
    }
    return s_saveDir;
}

std::string SlotPath(int slot, int t, bool merged) {
    char name[48];
    if (merged) sprintf(name, "/slt_xx_%d.sav", t);
    else sprintf(name, "/slt_%d_%d.sav", slot, t);
    return std::string(SaveDir()) + name;
}

inline uint32_t RegionOffset(int slot, int t, bool merged) {
    return merged ? (uint32_t)slot * (REC_SIZE + SlotSize(t)) : 0;      // FUN_006f4ac0 / FUN_006f4390
}

bool AnySaveFile() {
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA((std::string(SaveDir()) + "/slt_*.sav").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return false;
    FindClose(h);
    return true;
}

// Header of a slot as the PC readers would find it on disk (read only).  false: file or region missing.
bool PeekHeader(int slot, int t, SavRecord& out) {
    bool merged = Merged(t) != 0;
    HANDLE h = CreateFileA(SlotPath(slot, t, merged).c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return false;
    bool ok = false;
    LARGE_INTEGER size, pos;
    uint32_t off = RegionOffset(slot, t, merged);
    pos.QuadPart = off;
    DWORD got = 0;
    if (GetFileSizeEx(h, &size) && size.QuadPart >= (LONGLONG)off + REC_SIZE &&
        SetFilePointerEx(h, pos, NULL, FILE_BEGIN) && ReadFile(h, &out, REC_SIZE, &got, NULL) && got == REC_SIZE)
        ok = true;
    CloseHandle(h);
    return ok;
}

// Writes up to three blocks at offset.  truncate: the file is replaced (per-slot files), else updated in place.
bool WriteBlocks(const std::string& path, uint32_t offset, bool truncate, const void* a, uint32_t na,
                 const void* b, uint32_t nb, const void* c, uint32_t nc) {
    HANDLE h = CreateFileA(path.c_str(), GENERIC_WRITE, 0, NULL, truncate ? CREATE_ALWAYS : OPEN_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        SavLog("cannot open %s for writing (error %lu)", path.c_str(), GetLastError());
        return false;
    }
    LARGE_INTEGER pos;
    pos.QuadPart = offset;
    bool ok = SetFilePointerEx(h, pos, NULL, FILE_BEGIN) != 0;
    const void* blk[3] = { a, b, c };
    uint32_t len[3] = { na, nb, nc };
    for (int i = 0; ok && i < 3; ++i) {
        if (!blk[i] || !len[i]) continue;
        DWORD put = 0;
        ok = WriteFile(h, blk[i], len[i], &put, NULL) && put == len[i];
    }
    if (!ok) SavLog("write error on %s (error %lu)", path.c_str(), GetLastError());
    CloseHandle(h);
    return ok;
}

// ---------------------------------------------------------------------------------------------------------------
// save operations (Wii semantics on the PC files)
// ---------------------------------------------------------------------------------------------------------------
std::string Narrow(const uint16_t* w) {
    std::string s;
    for (int i = 0; w && i < NAME_UNITS && w[i]; ++i) s += (w[i] >= 0x20 && w[i] < 0x7F) ? (char)w[i] : '?';
    return s;
}

// Wii SAV_b_InitSystem: every NULL table gets Config+0x6C[t] records of 0xC0, zeroed, magic 0x67 at each record.
void AllocTables() {
    for (int t = 0; t < SAV_TABLES; ++t) {
        uint32_t p = TablePtr(t);
        if (p && p == s_tableOwn[t]) continue;
        if (p) {
            s_tableOwn[t] = p;
            s_tableCap[t] = AllocCount(t);
            SavLog("table %d: adopting the table already at %08X (%u slots)", t, p, s_tableCap[t]);
            continue;
        }
        s_tableOwn[t] = 0;
        s_tableCap[t] = 0;
        uint32_t count = AllocCount(t);
        if (!count && t < 3) {                  // SAVE_STRUCTURE_InitAll always declares 3 / 90 / 1 before InitSystem
            static const uint32_t kWii[3] = { 3, 90, 1 };
            count = kWii[t];
            SavLog("structure %d: no SAV_SetStructureMaxSize before SAV_InitSystem, using the Wii count %u", t, count);
        }
        if (!count) continue;                   // the Wii allocates 0 bytes: the structure has no slot
        void* mem = C().alloc(count * REC_SIZE);
        if (!mem) {
            SavLog("table %d: allocation of %u slots failed", t, count);
            continue;
        }
        memset(mem, 0, count * REC_SIZE);
        for (uint32_t i = 0; i < count; ++i) ((SavRecord*)mem)[i].magic = REC_MAGIC;
        TablePtr(t) = (uint32_t)(uintptr_t)mem;
        s_tableOwn[t] = TablePtr(t);
        s_tableCap[t] = count;
    }
}

// The PC reader leaves a record with a bad magic in place (FUN_006f44b0): reset it, keep the user buffer pointer.
int FixAfterRead(SavRecord* r, uint32_t ub, int ok) {
    if (r->magic == REC_MAGIC) return ok;
    memset(r, 0, REC_SIZE);
    r->magic = REC_MAGIC;
    r->userBuffer = ub;
    return 0;
}

// One slot header from disk through the PC reader.  No file or region: invalid (the Wii index record is empty).
int LoadHeader(int slot, int t) {
    SavRecord* r = Rec(slot, t);
    if (!r) return 0;
    SavRecord disk;
    if (!PeekHeader(slot, t, disk)) {
        r->valid = 0;
        return 0;
    }
    uint32_t ub = r->userBuffer;
    return FixAfterRead(r, ub, C().readHeader(slot, t, Merged(t) != 0));
}

// Wii SAV_ScanIfExists: every declared slot of every structure gets its header from the save index.
int ScanAll() {
    int valid = 0;
    for (int t = 0; t < SAV_TABLES; ++t) {
        if (!TablePtr(t) || TablePtr(t) != s_tableOwn[t]) continue;
        uint32_t n = ReqCount(t) < s_tableCap[t] ? ReqCount(t) : s_tableCap[t];
        for (uint32_t s = 0; s < n; ++s) {
            LoadHeader((int)s, t);
            if (Rec((int)s, t)->valid) ++valid;
        }
    }
    return valid;
}

void InitSystem(int n) {
    AllocTables();
    CfgN() = (uint32_t)n;
    const char* dir = SaveDir();
    bool any = AnySaveFile();
    FirstBoot() = any ? 0 : 1;              // RVL_BootUpFlow::Step -> SetFirstBoot(1) when it had to create the save
    int valid = ScanAll();
    InitDone() = 1;                         // SAV_gt_SaveContext+0x18
    s_bootDone = true;
    SavLog("SAV_InitSystem(%d): folder %s, %s, %d valid slot(s)", n, dir,
           any ? "save files present" : "no save file (first boot)", valid);
    for (int t = 0; t < SAV_TABLES; ++t) {
        if (!ReqCount(t) && !s_tableCap[t]) continue;
        SavLog("  structure %d: %u slots declared, %u allocated at %08X, merged %u, slot size 0x%X, user buffer %u",
               t, ReqCount(t), s_tableCap[t], TablePtr(t), Merged(t), SlotSize(t), UbSize(t));
    }
}

// Wii SAV_b_WriteSlot / SAV_b_SeekWriteSlot: magic, valid 1, time, data size = tmp buffer size; PC file layout.
bool SaveSlot(int slot, int t) {
    SavRecord* r = Rec(slot, t);
    if (!r) {
        SavLog("SAV_SaveSlotEx(%d, %d): no such slot, nothing written", slot, t);
        return false;
    }
    bool merged = Merged(t) != 0;
    SavRecord old = *r;
    SavTime tm;
    C().now(tm);
    r->magic = REC_MAGIC;
    r->valid = 1;
    r->sec = tm.sec;
    r->min = tm.min;
    r->hour = tm.hour;
    r->day = tm.day;
    r->month = tm.month;
    r->year = tm.year;
    uint8_t* data = (uint8_t*)(uintptr_t)TmpBuf();
    r->dataSize = data ? TmpSize() : 0;
    uint8_t* ub = NULL;
    if (UbSize(t)) {
        if (!r->userBuffer) {               // Wii @10881 trace: the header is restored, nothing is written
            *r = old;
            SavLog("SAV_SaveSlotEx(%d, %d): structure has a user buffer but the slot has none, nothing written",
                   slot, t);
            return false;
        }
        ub = (uint8_t*)(uintptr_t)r->userBuffer;
    }
    uint32_t payload = (ub ? UB_SIZE : 0) + r->dataSize;
    if (merged && payload > SlotSize(t)) {
        *r = old;
        SavLog("SAV_SaveSlotEx(%d, %d): 0x%X bytes do not fit the 0x%X-byte region, nothing written", slot, t,
               payload, SlotSize(t));
        return false;
    }
    SavRecord disk = *r;
    disk.userBuffer = 0;
    std::string path = SlotPath(slot, t, merged);
    if (!WriteBlocks(path, RegionOffset(slot, t, merged), !merged, &disk, REC_SIZE, ub, ub ? UB_SIZE : 0, data,
                     r->dataSize)) {
        *r = old;
        return false;
    }
    SavLog("slot %d of structure %d saved to %s (offset 0x%X, 0x%X bytes of data%s)", slot, t, path.c_str(),
           RegionOffset(slot, t, merged), r->dataSize, ub ? " + user buffer" : "");
    return true;
}

// Wii SAV_b_WriteSlotHeader: header (+ user buffer) only.  Per-slot files: the PC writer FUN_006f4690.
bool SaveHeader(int slot, int t) {
    SavRecord* r = Rec(slot, t);
    if (!r) return false;
    if (!Merged(t)) return C().writeHeaderFile(slot, t) != 0;
    SavRecord disk = *r;
    disk.userBuffer = 0;
    uint8_t* ub = (UbSize(t) && r->userBuffer) ? (uint8_t*)(uintptr_t)r->userBuffer : NULL;
    return WriteBlocks(SlotPath(slot, t, true), RegionOffset(slot, t, true), false, &disk, REC_SIZE, ub,
                       ub ? UB_SIZE : 0, NULL, 0);
}

int ReadSlot(int slot, int t) {
    SavRecord* r = Rec(slot, t);
    if (!r) return 0;
    SavRecord disk;
    if (!PeekHeader(slot, t, disk)) {
        r->valid = 0;
        return 0;
    }
    uint32_t ub = r->userBuffer;
    return FixAfterRead(r, ub, C().readSlot(slot, t, Merged(t) != 0));
}

// Wii SAV_UpdateValidityEx: the slot's valid flag from the save index (here: the header on disk).
int ReadValidity(int slot, int t) {
    SavRecord* r = Rec(slot, t);
    if (!r) return 0;
    SavRecord disk;
    r->valid = (PeekHeader(slot, t, disk) && disk.magic == REC_MAGIC) ? disk.valid : 0;
    return (int)r->valid;
}

// Wii SAV_SetSlotUserBuffer 80071374
void SetUserBuffer(int slot, int t, const uint8_t* data, uint32_t size) {
    if ((unsigned)t >= (unsigned)SAV_TABLES || size > UB_DATA || !UbSize(t) || size != UbSize(t)) return;
    SavRecord* r = Rec(slot, t);
    if (!r) return;
    if (!r->userBuffer) {
        void* p = C().alloc(UB_SIZE);
        if (!p) return;
        r->userBuffer = (uint32_t)(uintptr_t)p;
    }
    uint32_t* ub = (uint32_t*)(uintptr_t)r->userBuffer;
    ub[0] = 0xFFFFFFFF;
    ub[1] = 0xFFFFFFFF;
    ub[2] = UB_DATA;
    memset(ub + 3, 0, UB_DATA);
    if (data) memcpy(ub + 3, data, size);
}

// Wii SAV_GetSlotUserBuffer 80071490 = PC FUN_006eb950; fb = SCR_tt_FixedBuffer_ {data, cur, size}
void GetUserBuffer(int slot, int t, uint32_t* fb) {
    if (Rec(slot, t)) {
        C().getUserBuffer(slot, t, &fb[0], &fb[2], &fb[1]);
        return;
    }
    fb[2] = 0;                              // the "no data" branch of the same function
    if (fb[0]) {
        C().release((void*)(uintptr_t)fb[0]);
        fb[0] = 0;
    }
}

void SetName(int slot, int t, const uint16_t* name) {
    SavRecord* r = Rec(slot, t);
    if (!r) return;
    int i = 0;
    if (name)
        for (; i < NAME_UNITS - 1 && name[i]; ++i) r->name[i] = name[i];
    for (; i < NAME_UNITS; ++i) r->name[i] = 0;
}

const uint16_t* GetName(int slot, int t) {     // Wii SAV_pz_GetSlotName: the name of a valid slot, else L"Empty"
    SavRecord* r = Rec(slot, t);
    return (r && r->valid) ? r->name : C().emptyName;
}

// ---------------------------------------------------------------------------------------------------------------
// VM handlers: void* __cdecl handler(void* ip), return ip + 4
// ---------------------------------------------------------------------------------------------------------------
void TryWordPatches();                          // part 3

inline void Enter() {
    if (!s_testLog) WmEnsureInit();
}

inline void* Next(void* ip) { return (uint8_t*)ip + 4; }

void* __cdecl H_InitSystem(void* ip) {
    Enter();
    int n = PopInt();
    SavCall("SAV_InitSystem(%d)", n);
    InitSystem(n);
    TryWordPatches();
    return Next(ip);
}

void* __cdecl H_Step(void* ip) {                // Wii SAV_i_Step: 1 = no NAND flow pending (PC I/O is synchronous)
    Enter();
    PushInt(1);
    SavCall("SAV_Step() -> 1");
    return Next(ip);
}

void* __cdecl H_GetSlotDataEx(void* ip) {
    Enter();
    int field = PopInt();
    int t = PopInt();
    int slot = PopInt();
    int32_t v = GetSlotData(slot, t, field);
    PushInt(v);
    SavCall("SAV_GetSlotDataEx(%d, %d, %d) -> %d", slot, t, field, v);
    return Next(ip);
}

void* __cdecl H_IsEnabled(void* ip) {           // Wii SAV_b_IsEnabled: init done && boot flow initialized
    Enter();
    int v = (InitDone() && s_bootDone) ? 1 : 0;
    PushInt(v);
    SavCall("SAV_IsEnabled() -> %d", v);
    return Next(ip);
}

void* __cdecl H_IsFirstBoot(void* ip) {         // Wii RVL_NandHelper::bIsFirstBoot
    Enter();
    int v = FirstBoot() ? 1 : 0;
    PushInt(v);
    SavCall("SAV_IsFirstBoot() -> %d", v);
    return Next(ip);
}

void* __cdecl H_NeedFirstBoot(void* ip) {       // Wii bNeedFirstBoot: no banner.bin = the game never created its save
    Enter();
    int v = (s_bootDone || AnySaveFile()) ? 0 : 1;
    PushInt(v);
    SavCall("SAV_NeedFirstBoot() -> %d", v);
    return Next(ip);
}

void* __cdecl H_ReadSlotValidity(void* ip) {
    Enter();
    int t = PopInt();
    int slot = PopInt();
    int v = ReadValidity(slot, t);
    SavCall("SAV_ReadSlotValidity(%d, %d): valid %d", slot, t, v);
    return Next(ip);
}

void* __cdecl H_ForceProgressMessage(void* ip) {    // Wii: NAND "saving" message on/off; PC has no such message
    Enter();
    uint32_t w = CurrentWord(ip) >> 16;
    SavCall("SAV_ForceProgressMessage_%s()", w == 0x1E46 ? "Start" : (w == 0x1E47 ? "End" : "?"));
    return Next(ip);
}

void* __cdecl H_UpdateValid(void* ip) {
    Enter();
    int valid = ScanAll();
    SavCall("SAV_UpdateValid(): %d valid slot(s)", valid);
    return Next(ip);
}

void* __cdecl H_SaveSlotEx(void* ip) {
    Enter();
    int t = PopInt();
    int slot = PopInt();
    bool ok = SaveSlot(slot, t);
    SavCall("SAV_SaveSlotEx(%d, %d) -> %s", slot, t, ok ? "written" : "failed");
    return Next(ip);
}

void* __cdecl H_SetSlotUserDataEx(void* ip) {   // Wii SAV_SetSlotUserData: fields 100..109 only
    Enter();
    int value = PopInt();
    int field = PopInt();
    int t = PopInt();
    int slot = PopInt();
    SavRecord* r = Rec(slot, t);
    if (r && (unsigned)(field - 100) < 10u) r->user[field - 100] = (uint32_t)value;
    SavCall("SAV_SetSlotUserDataEx(%d, %d, %d, %d)%s", slot, t, field, value, r ? "" : ": no such slot");
    return Next(ip);
}

void* __cdecl H_SetSlotUserBufferEx(void* ip) {
    Enter();
    uint32_t* fb = PopRef(0xC);
    int t = PopInt();
    int slot = PopInt();
    SetUserBuffer(slot, t, fb ? (const uint8_t*)(uintptr_t)fb[0] : NULL, fb ? fb[2] : 0);
    SavCall("SAV_SetSlotUserBufferEx(%d, %d, size %u)", slot, t, fb ? fb[2] : 0);
    return Next(ip);
}

void* __cdecl H_GetSlotUserBufferEx(void* ip) {
    Enter();
    uint32_t* fb = PopRef(0xC);
    int t = PopInt();
    int slot = PopInt();
    if (fb) GetUserBuffer(slot, t, fb);
    SavCall("SAV_GetSlotUserBufferEx(%d, %d) -> size %u", slot, t, fb ? fb[2] : 0);
    return Next(ip);
}

void* __cdecl H_GetSlotNameW(void* ip) {
    Enter();
    int slot = PopInt();
    const uint16_t* name = GetName(slot, 0);
    PushInt((int32_t)(uintptr_t)name);
    SavCall("SAV_GetSlotNameW(%d) -> \"%s\"", slot, Narrow(name).c_str());
    return Next(ip);
}

void* __cdecl H_GetSlotNameWEx(void* ip) {
    Enter();
    int t = PopInt();
    int slot = PopInt();
    const uint16_t* name = GetName(slot, t);
    PushInt((int32_t)(uintptr_t)name);
    SavCall("SAV_GetSlotNameWEx(%d, %d) -> \"%s\"", slot, t, Narrow(name).c_str());
    return Next(ip);
}

void* __cdecl H_SetSlotNameWEx(void* ip) {
    Enter();
    const uint16_t* name = (const uint16_t*)(uintptr_t)(uint32_t)PopInt();
    int t = PopInt();
    int slot = PopInt();
    SetName(slot, t, name);
    SavCall("SAV_SetSlotNameWEx(%d, %d, \"%s\")%s", slot, t, Narrow(name).c_str(), Rec(slot, t) ? "" : ": no such slot");
    return Next(ip);
}

void* __cdecl H_ReadSlotEx(void* ip) {
    Enter();
    int t = PopInt();
    int slot = PopInt();
    int ok = ReadSlot(slot, t);
    SavCall("SAV_ReadSlotEx(%d, %d) -> %d (0x%X bytes of data)", slot, t, ok, ok ? TmpSize() : 0);
    return Next(ip);
}

void* __cdecl H_ReadSlotHeaderEx(void* ip) {
    Enter();
    int t = PopInt();
    int slot = PopInt();
    int ok = LoadHeader(slot, t);
    SavCall("SAV_ReadSlotHeaderEx(%d, %d) -> %d", slot, t, ok);
    return Next(ip);
}

void* __cdecl H_SaveSlotHeaderEx(void* ip) {
    Enter();
    int t = PopInt();
    int slot = PopInt();
    bool ok = SaveHeader(slot, t);
    SavCall("SAV_SaveSlotHeaderEx(%d, %d) -> %s", slot, t, ok ? "written" : "failed");
    return Next(ip);
}

void* __cdecl H_DeleteSlotEx(void* ip) {        // PC SAV_DeleteSlotEx_C: header with valid 0 written to the file
    Enter();
    int t = PopInt();
    int slot = PopInt();
    bool ok = Rec(slot, t) != NULL;
    if (ok) C().deleteSlot(slot, t);
    SavCall("SAV_DeleteSlotEx(%d, %d)%s", slot, t, ok ? "" : ": no such slot");
    return Next(ip);
}

// shared engine handlers: replaced per native word (part 3)
void* __cdecl H_IsChannelInstalled(void* ip) {  // Wii RVL_Nand::bChannelExists
    Enter();
    int v = s_channelInstalled ? 1 : 0;
    PushInt(v);
    SavCall("SAV_IsChannelInstalled() -> %d", v);
    return Next(ip);
}

void* __cdecl H_ReturnCommand(void* ip) {       // Wii: answer to the pending NAND flow; no flow is ever pending
    Enter();
    int cmd = PopInt();
    SavCall("SAV_ReturnCommand(%d): no pending save prompt", cmd);
    return Next(ip);
}

void* __cdecl H_InstallChannel(void* ip) {      // Wii: RVL_ChannelInstallFlow; done at once, SAV_Step() stays 1
    Enter();
    s_channelInstalled = true;
    SavCall("SAV_InstallChannel(): channel reported installed");
    return Next(ip);
}

// ---------------------------------------------------------------------------------------------------------------
// the PC executable (2010 build, image base 0x400000): what is verified and what is patched
// ---------------------------------------------------------------------------------------------------------------
typedef void* (__cdecl* NativeFn)(void* ip);

struct CodeCrc { uint32_t va, len, crc; const char* what; };

struct HandlerSite {
    uint32_t va, len, crc;          // engine handler (whole body up to its last ret); a JMP to fn replaces its entry
    uint32_t reg[2];                // RegisterFunctions imm32 holding va (the only references in the exe)
    uint16_t word[2];               // native word (high 16 bits) registered with it
    NativeFn fn;
    const char* name;
};

const HandlerSite kHandlers[] = {
    { 0x005D6C90, 0x030, 0xDEADD724, { 0x0050FF84, 0 }, { 0x1E26, 0 }, H_InitSystem, "SAV_InitSystem" },
    { 0x005D6D90, 0x045, 0xDFD8C8FA, { 0x0051004A, 0 }, { 0x1E29, 0 }, H_Step, "SAV_Step" },
    { 0x005D64B0, 0x096, 0x0C40B75B, { 0x0050FDF8, 0 }, { 0x1E20, 0 }, H_GetSlotDataEx, "SAV_GetSlotDataEx" },
    { 0x005D6DE0, 0x045, 0x144C8313, { 0x00510110, 0 }, { 0x1E2C, 0 }, H_IsEnabled, "SAV_IsEnabled" },
    { 0x005D6E60, 0x045, 0xB780EC5A, { 0x005105B4, 0 }, { 0x1E3E, 0 }, H_IsFirstBoot, "SAV_IsFirstBoot" },
    { 0x005D6EB0, 0x045, 0x5F318338, { 0x0051067A, 0 }, { 0x1E41, 0 }, H_NeedFirstBoot, "SAV_NeedFirstBoot" },
    { 0x005D6460, 0x045, 0xF23B0C21, { 0x00510782, 0 }, { 0x1E45, 0 }, H_ReadSlotValidity, "SAV_ReadSlotValidity" },
    { 0x005D5CC0, 0x00D, 0xAB30BFA3, { 0x005107C4, 0x00510806 }, { 0x1E46, 0x1E47 }, H_ForceProgressMessage,
      "SAV_ForceProgressMessage_Start/End" },
    { 0x005D5930, 0x00D, 0x8B4978CD, { 0x0050FBE8, 0 }, { 0x1E18, 0 }, H_UpdateValid, "SAV_UpdateValid" },
    { 0x005D5CD0, 0x06B, 0x4B11E192, { 0x0050FD32, 0 }, { 0x1E1D, 0 }, H_SaveSlotEx, "SAV_SaveSlotEx" },
    { 0x005D6630, 0x075, 0x7E521273, { 0x0050FE3A, 0 }, { 0x1E21, 0 }, H_SetSlotUserDataEx, "SAV_SetSlotUserDataEx" },
    { 0x005D6AC0, 0x065, 0xDA8C0980, { 0x00510362, 0 }, { 0x1E35, 0 }, H_SetSlotUserBufferEx, "SAV_SetSlotUserBufferEx" },
    { 0x005D69B0, 0x06A, 0x9A44420A, { 0x00510218, 0 }, { 0x1E30, 0 }, H_GetSlotNameW, "SAV_GetSlotNameW" },
    { 0x005D6930, 0x07D, 0xCFFFCDED, { 0x00510289, 0 }, { 0x1E32, 0 }, H_GetSlotNameWEx, "SAV_GetSlotNameWEx" },
    { 0x005D6780, 0x05C, 0xFDEF0037, { 0x0051025A, 0 }, { 0x1E31, 0 }, H_SetSlotNameWEx, "SAV_SetSlotNameWEx" },
    { 0x005D6B80, 0x067, 0x8E60E509, { 0x005103A4, 0 }, { 0x1E36, 0 }, H_GetSlotUserBufferEx, "SAV_GetSlotUserBufferEx" },
    { 0x005D5E70, 0x06B, 0x1820A95C, { 0x0050FD74, 0 }, { 0x1E1E, 0 }, H_ReadSlotEx, "SAV_ReadSlotEx" },
    { 0x005D5F40, 0x06B, 0x266AA752, { 0x005104EE, 0 }, { 0x1E3B, 0 }, H_ReadSlotHeaderEx, "SAV_ReadSlotHeaderEx" },
    { 0x005D5DA0, 0x06B, 0x5DA8A0C7, { 0x005106FE, 0 }, { 0x1E43, 0 }, H_SaveSlotHeaderEx, "SAV_SaveSlotHeaderEx" },
    { 0x005D6CC0, 0x06B, 0xE97AA2BB, { 0x00510008, 0 }, { 0x1E28, 0 }, H_DeleteSlotEx, "SAV_DeleteSlotEx" },
};

// Natives whose engine handler body is shared with other natives: the handler is replaced for that word only, in the
// native table (TOOsarray 0x00A718AC, read at every call) and in its RegisterFunctions entry.
struct WordSite { uint16_t word; uint32_t reg; uint32_t oldFn; NativeFn fn; const char* name; };

const WordSite kWords[] = {
    { 0x1E3C, 0x00510530, 0x005ECA50, H_IsChannelInstalled, "SAV_IsChannelInstalled" },   // "push 0", 11 natives
    { 0x1E2A, 0x0051008C, 0x005D6430, H_ReturnCommand, "SAV_ReturnCommand" },             // pop + no-op, 3 natives
    { 0x1E2E, 0x00510194, 0x00560580, H_InstallChannel, "SAV_InstallChannel" },           // no-op, 22 natives
};

const CodeCrc kEngine[] = {
    { 0x006BB890, 0x040, 0xDBFB9619, "MEM::p_Alloc" },
    { 0x006BBC90, 0x040, 0x9A256EAE, "MEM::Free" },
    { 0x006D4BD0, 0x071, 0xED5B8846, "STD_IOGetExecPath" },
    { 0x006D5560, 0x01F, 0x4ACAC20B, "STD_u8_TimeGetCurrentSec" },
    { 0x006D5580, 0x020, 0x6D3F6297, "STD_u8_TimeGetCurrentMin" },
    { 0x006D55A0, 0x020, 0xA8CF8F96, "STD_u8_TimeGetCurrentHour" },
    { 0x006D55C0, 0x020, 0x5D4F2956, "STD_u8_TimeGetCurrentDay" },
    { 0x006D55E0, 0x020, 0xF85F53D5, "STD_u8_TimeGetCurrentMonth" },
    { 0x006D5600, 0x026, 0x3F56F5CD, "STD_u16_TimeGetCurrentYear" },
    { 0x006F4910, 0x08C, 0x85927DF9, "FUN_006f4910 slot header read" },
    { 0x006F4AC0, 0x0B4, 0x1C470B20, "FUN_006f4ac0 merged slot header read" },
    { 0x006F4830, 0x0D4, 0xE6F4C9A6, "FUN_006f4830 slot read" },
    { 0x006F49A0, 0x11B, 0x201AB658, "FUN_006f49a0 merged slot read" },
    { 0x006F4690, 0x0F5, 0x5B9BCF38, "FUN_006f4690 slot header write" },
    { 0x005D5BF0, 0x031, 0x60B81A91, "SAV_DeleteSlotEx_C" },
    { 0x006EBA00, 0x03F, 0x8EFEFF61, "FUN_006eba00 delete" },
    { 0x006EBA40, 0x03C, 0xCFEC53F5, "FUN_006eba40 merged delete" },
    { 0x006F4290, 0x0F1, 0x3D742DC4, "FUN_006f4290 delete write" },
    { 0x006F4390, 0x11A, 0x2D3CB38E, "FUN_006f4390 merged delete write" },
    { 0x006F44B0, 0x0CA, 0x60B1FE36, "FUN_006f44b0 record read" },
    { 0x006F4580, 0x10F, 0x84B8ABF7, "FUN_006f4580 merged record read" },
    { 0x006EB950, 0x0AB, 0x9644F291, "FUN_006eb950 user buffer copy" },
    { 0x006EB7C0, 0x039, 0x627394D0, "FUN_006eb7c0 tmp buffer size" },
    { 0x006F47B0, 0x069, 0x1CEED101, "FUN_006f47b0 NeedFirstBoot" },
    { 0x006EBBE0, 0x006, 0xB6088798, "FUN_006ebbe0 init done flag" },
    { 0x006F47A0, 0x006, 0x73F86A99, "FUN_006f47a0 first boot flag" },
    { 0x004F6A90, 0x024, 0x777EDD37, "SCR_u32_GetRankID (native table)" },
    { 0x006EBAA0, 0x043, 0xD6A39087, "FUN_006ebaa0 SetStructureMaxSize" },
    { 0x006EBBC0, 0x019, 0x6510D5FD, "FUN_006ebbc0 SetUserBufferSize" },
    { 0x006EBBF0, 0x010, 0xE3E42815, "FUN_006ebbf0 SetSlotsMerge" },
    { 0x006EBC00, 0x00C, 0x7FF89E39, "FUN_006ebc00 merged flag" },
    { 0x006EB920, 0x027, 0xE38F0A1C, "FUN_006eb920 slot name" },
};

// "call STD_IOGetExecPath" inside the eight save functions (header write/read, reads, deletes, NeedFirstBoot)
const uint32_t kExecPathCalls[] = { 0x006F4297, 0x006F4398, 0x006F46B8, 0x006F47C4, 0x006F4858, 0x006F4938,
                                    0x006F49C6, 0x006F4AE6 };
const uint32_t STD_IOGETEXECPATH = 0x006D4BD0;
const uint32_t EMPTY_NAME = 0x008CC128;         // L"Empty"
const uint32_t NATIVE_TABLE = 0x00A718AC;

// Crc32, Image / ProcessImage / FileImage, Report, CheckCrc, WriteCode and PatchNativeTable: wm_patch.cpp

// RegisterFunctions entry: imm32 == handler at reg, "add eax, word<<16" within 0x20 bytes
bool CheckRegistration(const Image& img, uint32_t reg, uint32_t handler, uint16_t word) {
    uint32_t v = 0;
    uint8_t win[0x44];
    if (!img.Read(reg, &v, 4) || v != handler || !img.Read(reg - 0x20, win, sizeof(win))) return false;
    for (int i = 0; i + 5 <= (int)sizeof(win); ++i)
        if (win[i] == 0x05 && win[i + 1] == 0 && win[i + 2] == 0 && win[i + 3] == (word & 0xFF) && win[i + 4] == (word >> 8))
            return true;
    return false;
}

// Everything the save layer relies on.  exclusivity: also count every reference to the handler addresses (file only).
int Verify(const Image& img, std::string& rep, const FileImage* file) {
    int bad = 0;
    uint32_t got;
    for (size_t i = 0; i < sizeof(kHandlers) / sizeof(kHandlers[0]); ++i) {
        const HandlerSite& h = kHandlers[i];
        bool ok = CheckCrc(img, h.va, h.len, h.crc, &got);
        for (int r = 0; r < 2; ++r)
            if (h.reg[r]) ok = ok && CheckRegistration(img, h.reg[r], h.va, h.word[r]);
        Report(rep, ok, "%-36s handler %08X len 0x%03X crc %08X (want %08X), registered at %08X%s", h.name, h.va,
               h.len, got, h.crc, h.reg[0], h.reg[1] ? " and 00510806" : "");
        if (!ok) ++bad;
    }
    for (size_t i = 0; i < sizeof(kWords) / sizeof(kWords[0]); ++i) {
        const WordSite& w = kWords[i];
        uint8_t op[3] = { 0 };
        bool ok = img.Read(w.reg - 7, op, 3) && op[0] == 0xC7 && op[1] == 0x84 && op[2] == 0x24 &&
                  CheckRegistration(img, w.reg, w.oldFn, w.word);
        Report(rep, ok, "%-36s word %04X0000 registered at %08X with shared handler %08X", w.name, w.word, w.reg, w.oldFn);
        if (!ok) ++bad;
    }
    for (size_t i = 0; i < sizeof(kEngine) / sizeof(kEngine[0]); ++i) {
        const CodeCrc& e = kEngine[i];
        bool ok = CheckCrc(img, e.va, e.len, e.crc, &got);
        Report(rep, ok, "%-36s %08X len 0x%03X crc %08X (want %08X)", e.what, e.va, e.len, got, e.crc);
        if (!ok) ++bad;
    }
    for (size_t i = 0; i < sizeof(kExecPathCalls) / sizeof(kExecPathCalls[0]); ++i) {
        uint8_t b[5] = { 0 };
        int32_t rel = 0;
        bool ok = img.Read(kExecPathCalls[i], b, 5) && b[0] == 0xE8;
        memcpy(&rel, b + 1, 4);
        ok = ok && kExecPathCalls[i] + 5 + (uint32_t)rel == STD_IOGETEXECPATH;
        Report(rep, ok, "save path call site %08X: call STD_IOGetExecPath", kExecPathCalls[i]);
        if (!ok) ++bad;
    }
    uint8_t empty[12];
    static const uint8_t kEmpty[12] = { 'E', 0, 'm', 0, 'p', 0, 't', 0, 'y', 0, 0, 0 };
    bool eok = img.Read(EMPTY_NAME, empty, 12) && memcmp(empty, kEmpty, 12) == 0;
    Report(rep, eok, "L\"Empty\" at %08X", EMPTY_NAME);
    if (!eok) ++bad;

    if (file) {                             // exclusivity: the patched handler bodies are used by these natives only
        const size_t nh = sizeof(kHandlers) / sizeof(kHandlers[0]);
        std::vector<int> imm(nh, 0), calls(nh, 0);
        for (size_t s = 0; s < file->secs.size(); ++s) {
            const FileImage::Sec& sec = file->secs[s];
            if (!sec.code || sec.rptr + sec.rsize > file->data.size() || sec.rsize < 5) continue;
            const uint8_t* p = &file->data[sec.rptr];
            for (uint32_t k = 0; k + 5 <= sec.rsize; ++k) {
                uint32_t v;
                memcpy(&v, p + k, 4);
                bool rel32 = p[k] == 0xE8 || p[k] == 0xE9;
                uint32_t dst = 0;
                if (rel32) {
                    int32_t rel;
                    memcpy(&rel, p + k + 1, 4);
                    dst = sec.va + k + 5 + (uint32_t)rel;
                }
                if ((v & 0xFF000000) != 0 && !rel32) continue;      // every handler is below 0x01000000
                for (size_t i = 0; i < nh; ++i) {
                    if (v == kHandlers[i].va) ++imm[i];
                    if (rel32 && dst == kHandlers[i].va) ++calls[i];
                }
            }
        }
        for (size_t i = 0; i < nh; ++i) {
            const HandlerSite& h = kHandlers[i];
            int expect = h.reg[1] ? 2 : 1;
            bool ok = imm[i] == expect && calls[i] == 0;
            Report(rep, ok, "%-36s handler %08X: %d imm32 reference(s) in code (want %d), %d call/jmp", h.name, h.va,
                   imm[i], expect, calls[i]);
            if (!ok) ++bad;
        }
    }
    return bad;
}

// ---------------------------------------------------------------------------------------------------------------
// game context: engine thunks
// ---------------------------------------------------------------------------------------------------------------
void* GameAlloc(uint32_t n) {                   // MEM::p_Alloc(MEM_gpo_Main, n), thiscall via fastcall
    typedef void* (__fastcall* Fn)(void* self, void* edx, uint32_t size);
    return ((Fn)0x006BB890)(*(void**)0x009D8C08, NULL, n);
}
void GameFree(void* p) {
    typedef void (__fastcall* Fn)(void* self, void* edx, void* p);
    ((Fn)0x006BBC90)(*(void**)0x009D8C08, NULL, p);
}
const char* GameExecPath() { return ((const char* (__cdecl*)())STD_IOGETEXECPATH)(); }
void GameNow(SavTime& t) {
    typedef uint8_t (__cdecl* F8)();
    t.sec = ((F8)0x006D5560)();
    t.min = ((F8)0x006D5580)();
    t.hour = ((F8)0x006D55A0)();
    t.day = ((F8)0x006D55C0)();
    t.month = ((F8)0x006D55E0)();
    t.year = ((uint16_t (__cdecl*)())0x006D5600)();
}
typedef int (__cdecl* SlotFn)(int slot, int table);
int GameReadHeader(int slot, int t, bool merged) { return ((SlotFn)(merged ? 0x006F4AC0 : 0x006F4910))(slot, t); }
int GameReadSlot(int slot, int t, bool merged) { return ((SlotFn)(merged ? 0x006F49A0 : 0x006F4830))(slot, t); }
int GameWriteHeader(int slot, int t) { return ((SlotFn)0x006F4690)(slot, t); }
void GameDelete(int slot, int t) { ((void (__cdecl*)(int, int))0x005D5BF0)(slot, t); }
void GameGetUserBuffer(int slot, int t, uint32_t* data, uint32_t* size, uint32_t* cur) {
    ((void (__cdecl*)(int, int, uint32_t*, uint32_t*, uint32_t*))0x006EB950)(slot, t, data, size, cur);
}

void SetupGameCtx() {
    s_game.vmIndex = (uint32_t*)0x00A7E188;
    s_game.vmOffset = (uint32_t*)0x00A7E18C;
    s_game.vmData = (uint32_t*)0x00A7E198;
    s_game.vmPtrs = (uint32_t*)0x00A7E19C;
    s_game.natives = (uint32_t*)NATIVE_TABLE;
    s_game.cfg = (uint32_t*)0x00A93698;
    s_game.save = (uint32_t*)0x00A9374C;
    s_game.alloc = GameAlloc;
    s_game.release = GameFree;
    s_game.execPath = GameExecPath;
    s_game.now = GameNow;
    s_game.readHeader = GameReadHeader;
    s_game.readSlot = GameReadSlot;
    s_game.writeHeaderFile = GameWriteHeader;
    s_game.deleteSlot = GameDelete;
    s_game.getUserBuffer = GameGetUserBuffer;
    s_game.emptyName = (const uint16_t*)EMPTY_NAME;
}

// replaces "call STD_IOGetExecPath" in the save functions: "<exe dir>/sav" (the engine appends "/slt_...")
const char* __cdecl SavExecPathForSaves() { return SaveDir(); }

bool s_patched;
bool s_wordDone[sizeof(kWords) / sizeof(kWords[0])];

// The native table is built by ViD::b_Create (SCR_b_Init + RegisterFunctions) before wiimote.dll is loaded; the VM
// reads the handler from the table at every call (SCR::ApplyCommon), so the element is replaced in place.
bool PatchNativeElement(const WordSite& w) {
    std::string notes;
    int r = PatchNativeTable((uint32_t)w.word << 16, 0xFFFF0000, w.oldFn, (uint32_t)(uintptr_t)w.fn, notes);
    size_t pos = 0;
    while (pos < notes.size()) {
        size_t end = notes.find('\n', pos);
        SavLog("%s: %s", w.name, notes.substr(pos, end == std::string::npos ? std::string::npos : end - pos).c_str());
        pos = end == std::string::npos ? notes.size() : end + 1;
    }
    return r == 1;
}

void TryWordPatches() {
    if (s_ctx != &s_game || !s_patched) return;
    for (size_t i = 0; i < sizeof(kWords) / sizeof(kWords[0]); ++i)
        if (!s_wordDone[i]) s_wordDone[i] = PatchNativeElement(kWords[i]);
}

// [save] of wiimote.ini read with plain file I/O (DllMain: no profile API)
void ReadSaveIni(const std::string& path, bool& enabled, bool& channel, bool& found) {
    found = false;
    HANDLE h = CreateFileA(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) return;
    char buf[65536];
    DWORD got = 0;
    ReadFile(h, buf, sizeof(buf) - 1, &got, NULL);
    CloseHandle(h);
    buf[got] = 0;
    found = true;
    std::string text(buf), section;
    size_t pos = 0;
    while (pos < text.size()) {
        size_t end = text.find('\n', pos);
        std::string line = text.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
        pos = end == std::string::npos ? text.size() : end + 1;
        size_t a = line.find_first_not_of(" \t\r");
        if (a == std::string::npos) continue;
        line = line.substr(a);
        while (!line.empty() && (line[line.size() - 1] == '\r' || line[line.size() - 1] == ' ' || line[line.size() - 1] == '\t'))
            line.resize(line.size() - 1);
        if (line[0] == ';' || line[0] == '#') continue;
        for (size_t i = 0; i < line.size(); ++i) line[i] = (char)tolower((unsigned char)line[i]);
        if (line[0] == '[') {
            section = line.substr(1, line.find(']') == std::string::npos ? std::string::npos : line.find(']') - 1);
            continue;
        }
        if (section != "save") continue;
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string key = line.substr(0, eq), val = line.substr(eq + 1);
        while (!key.empty() && (key[key.size() - 1] == ' ' || key[key.size() - 1] == '\t')) key.resize(key.size() - 1);
        size_t c = val.find_first_of(";#");
        if (c != std::string::npos) val.resize(c);
        size_t v0 = val.find_first_not_of(" \t");
        val = v0 == std::string::npos ? std::string() : val.substr(v0);
        while (!val.empty() && (val[val.size() - 1] == ' ' || val[val.size() - 1] == '\t')) val.resize(val.size() - 1);
        bool on = val == "1" || val == "true" || val == "yes" || val == "on";
        if (key == "enabled") enabled = on;
        else if (key == "channel_installed") channel = on;
    }
}

bool s_iniEnabled = true;

// ---------------------------------------------------------------------------------------------------------------
// self-test (wmtest sav): the real handlers on a fake VM and a fake engine whose file functions follow the PC
// decompile (FUN_006f44b0 / 006f4580 / 006f4830 / 006f49a0 / 006f4690 / 006f4290 / 006f4390 / 006eb950)
// ---------------------------------------------------------------------------------------------------------------
struct Fake {
    uint32_t cfg[45];
    uint32_t save[8];
    uint32_t vmIndex, vmOffset, vmData, vmPtrs;
    uint8_t  data[256];
    uint32_t ptrs[64];
    std::string dir;
};
Fake* s_fake;

void* FkAlloc(uint32_t n) { return calloc(1, n ? n : 1); }
void FkFree(void* p) { free(p); }
const char* FkExecPath() { return s_fake->dir.c_str(); }
void FkNow(SavTime& t) { t.sec = 1; t.min = 2; t.hour = 3; t.day = 4; t.month = 5; t.year = 2026; }

FILE* FkOpen(int slot, int t, bool merged, const char* mode) { return fopen(SlotPath(slot, t, merged).c_str(), mode); }

int FkReadRecord(FILE* f, int slot, int t, long off, bool merged) {     // FUN_006f44b0 / FUN_006f4580
    SavRecord* r = Rec(slot, t);
    uint32_t ub = UbSize(t) ? r->userBuffer : 0;
    if (merged) fseek(f, off, SEEK_SET);
    fread(r, REC_SIZE, 1, f);
    if (r->magic != REC_MAGIC) {
        r->valid = 0;
        return 0;
    }
    r->userBuffer = ub;
    if (UbSize(t)) {
        if (!r->userBuffer) r->userBuffer = (uint32_t)(uintptr_t)FkAlloc(UB_SIZE);
        if (merged) fseek(f, off + REC_SIZE, SEEK_SET);
        fread((void*)(uintptr_t)r->userBuffer, UB_SIZE, 1, f);
    }
    return 1;
}

int FkReadHeader(int slot, int t, bool merged) {
    FILE* f = FkOpen(slot, t, merged, "rb");
    if (!f) return 0;
    int ok = FkReadRecord(f, slot, t, (long)RegionOffset(slot, t, merged), merged);
    fclose(f);
    return ok;
}

int FkReadSlot(int slot, int t, bool merged) {
    FILE* f = FkOpen(slot, t, merged, "rb");
    if (!f) return 0;
    long off = (long)RegionOffset(slot, t, merged);
    int ok = FkReadRecord(f, slot, t, off, merged);
    if (ok) {
        uint32_t n = Rec(slot, t)->dataSize;                            // FUN_006eb7c0
        TmpBuf() = (uint32_t)(uintptr_t)realloc((void*)(uintptr_t)TmpBuf(), n + 0x400);
        TmpSize() = n;
        fseek(f, off + REC_SIZE + (UbSize(t) ? UB_SIZE : 0), SEEK_SET);
        fread((void*)(uintptr_t)TmpBuf(), 1, n, f);
    }
    fclose(f);
    return ok;
}

int FkWriteHeader(int slot, int t) {                                    // FUN_006f4690 (per-slot file)
    FILE* f = FkOpen(slot, t, false, "r+b");
    if (!f) f = FkOpen(slot, t, false, "w+b");
    if (!f) return 0;
    SavRecord* r = Rec(slot, t);
    fwrite(r, REC_SIZE, 1, f);
    if (UbSize(t) && r->userBuffer) fwrite((void*)(uintptr_t)r->userBuffer, UB_SIZE, 1, f);
    fclose(f);
    return 1;
}

void FkDelete(int slot, int t) {                                        // FUN_006f4290 / FUN_006f4390
    bool merged = Merged(t) != 0;
    FILE* f = FkOpen(slot, t, merged, "r+b");
    if (!f) f = FkOpen(slot, t, merged, "w+b");
    if (!f) return;
    SavRecord* r = Rec(slot, t);
    r->valid = 0;
    fseek(f, (long)RegionOffset(slot, t, merged), SEEK_SET);
    fwrite(r, REC_SIZE, 1, f);
    fclose(f);
}

void FkGetUserBuffer(int slot, int t, uint32_t* data, uint32_t* size, uint32_t* cur) {   // FUN_006eb950
    SavRecord* r = Rec(slot, t);
    if (r->valid && UbSize(t) && UbSize(t) <= *size) {
        if (!*data) *data = (uint32_t)(uintptr_t)FkAlloc(*size);
        memcpy((void*)(uintptr_t)*data, (uint8_t*)(uintptr_t)r->userBuffer + 0xC, UbSize(t));
        *cur = *data + UbSize(t);
        return;
    }
    *size = 0;
    if (*data) {
        FkFree((void*)(uintptr_t)*data);
        *data = 0;
    }
}

// script call: push the arguments like the VM does, run the handler, return the int result (if any)
int32_t Call(NativeFn fn, int nargs, const uint32_t* args, bool* stackOk, int results) {
    Fake& f = *s_fake;
    f.vmIndex = 0;
    f.vmOffset = 0;
    for (int i = 0; i < nargs; ++i) {
        memcpy(f.data + f.vmOffset, &args[i], 4);
        f.ptrs[f.vmIndex++] = (uint32_t)(uintptr_t)(f.data + f.vmOffset);
        f.vmOffset += 4;
    }
    uint32_t word = 0;
    void* next = fn(&word);
    int32_t v = 0;
    if (results) memcpy(&v, f.data, 4);
    if (stackOk) *stackOk = next == (uint8_t*)&word + 4 && f.vmIndex == (uint32_t)results && f.vmOffset == 4u * results;
    return v;
}

int32_t CallR(NativeFn fn, int n, const uint32_t* a) {
    bool ok;
    int32_t v = Call(fn, n, a, &ok, 1);
    return ok ? v : (int32_t)0x80000000;
}

void CallV(NativeFn fn, int n, const uint32_t* a) { Call(fn, n, a, NULL, 0); }

// fixed buffer argument: the VM passes a pointer to a 12-byte copy
void CallFb(NativeFn fn, int slot, int t, uint32_t* fb) {
    Fake& f = *s_fake;
    f.vmIndex = 0;
    f.vmOffset = 0;
    uint32_t a[2] = { (uint32_t)slot, (uint32_t)t };
    for (int i = 0; i < 2; ++i) {
        memcpy(f.data + f.vmOffset, &a[i], 4);
        f.ptrs[f.vmIndex++] = (uint32_t)(uintptr_t)(f.data + f.vmOffset);
        f.vmOffset += 4;
    }
    f.ptrs[f.vmIndex++] = (uint32_t)(uintptr_t)fb;
    f.vmOffset += 0xC;
    uint32_t word = 0;
    fn(&word);
}

long FileSize(const std::string& p) {
    FILE* f = fopen(p.c_str(), "rb");
    if (!f) return -1;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fclose(f);
    return n;
}

int RunSelfTest(const char* dir, std::string& rep) {
    Fake* fk = new Fake();
    memset(fk->cfg, 0, sizeof(fk->cfg));
    memset(fk->save, 0, sizeof(fk->save));
    fk->dir = dir;
    SavCtx ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.vmIndex = &fk->vmIndex;
    ctx.vmOffset = &fk->vmOffset;
    ctx.vmData = &fk->vmData;
    ctx.vmPtrs = &fk->vmPtrs;
    fk->vmData = (uint32_t)(uintptr_t)fk->data;
    fk->vmPtrs = (uint32_t)(uintptr_t)fk->ptrs;
    ctx.cfg = fk->cfg;
    ctx.save = fk->save;
    ctx.alloc = FkAlloc;
    ctx.release = FkFree;
    ctx.execPath = FkExecPath;
    ctx.now = FkNow;
    ctx.readHeader = FkReadHeader;
    ctx.readSlot = FkReadSlot;
    ctx.writeHeaderFile = FkWriteHeader;
    ctx.deleteSlot = FkDelete;
    ctx.getUserBuffer = FkGetUserBuffer;
    static const uint16_t kEmpty[] = { 'E', 'm', 'p', 't', 'y', 0 };
    ctx.emptyName = kEmpty;

    // save the game state, switch to the fake
    SavCtx* oldCtx = s_ctx;
    bool oldBoot = s_bootDone, oldChan = s_channelInstalled;
    uint32_t oldOwn[SAV_TABLES], oldCap[SAV_TABLES];
    memcpy(oldOwn, s_tableOwn, sizeof(oldOwn));
    memcpy(oldCap, s_tableCap, sizeof(oldCap));
    char oldDir[sizeof(s_saveDir)];
    memcpy(oldDir, s_saveDir, sizeof(oldDir));
    s_ctx = &ctx;
    s_fake = fk;
    s_bootDone = false;
    s_channelInstalled = true;
    memset(s_tableOwn, 0, sizeof(s_tableOwn));
    memset(s_tableCap, 0, sizeof(s_tableCap));
    s_saveDir[0] = 0;
    std::string log;
    s_testLog = &log;
    s_testVerbose = true;
    int fails = 0;
    #define EXPECT(cond, ...) do { bool ok_ = (cond); Report(rep, ok_, __VA_ARGS__); if (!ok_) ++fails; } while (0)

    for (const char* n : { "slt_0_0.sav", "slt_1_0.sav", "slt_0_2.sav", "slt_xx_1.sav" })
        DeleteFileA((std::string(dir) + "/sav/" + n).c_str());

    // before SAV_InitSystem (tables NULL): every native must be safe
    uint32_t a3[3] = { 0, 0, 0 };
    EXPECT(CallR(H_GetSlotDataEx, 3, a3) == 0, "GetSlotDataEx(0,0,0) with NULL tables -> 0, VM stack balanced");
    EXPECT(CallR(H_IsEnabled, 0, NULL) == 0, "IsEnabled before InitSystem -> 0");
    EXPECT(CallR(H_NeedFirstBoot, 0, NULL) == 1, "NeedFirstBoot with no save file -> 1");
    uint32_t a2[2] = { 0, 0 };
    CallV(H_SaveSlotEx, 2, a2);
    EXPECT(FileSize(std::string(dir) + "/sav/slt_0_0.sav") < 0, "SaveSlotEx with NULL tables writes nothing");
    const uint16_t* nm = (const uint16_t*)(uintptr_t)(uint32_t)CallR(H_GetSlotNameWEx, 2, a2);
    EXPECT(nm == kEmpty, "GetSlotNameWEx with NULL tables -> L\"Empty\"");

    // SAVE_STRUCTURE_InitAll values, then SAV_InitSystem(1)
    for (int t = 0; t < 3; ++t) {
        static const uint32_t cnt[3] = { 3, 90, 1 }, sz[3] = { 0x400, 0x2027C, 0x800 };
        ReqCount(t) = AllocCount(t) = cnt[t];
        SlotSize(t) = sz[t];
    }
    Merged(1) = 1;
    UbSize(1) = 6144;
    uint32_t a1[1] = { 1 };
    CallV(H_InitSystem, 1, a1);
    bool magic = true;
    for (int t = 0; t < 3; ++t)
        for (int s = 0; s < (int)AllocCount(t); ++s) magic = magic && Rec(s, t) && Rec(s, t)->magic == REC_MAGIC;
    EXPECT(TablePtr(0) && TablePtr(1) && TablePtr(2) && !TablePtr(3) && magic,
           "InitSystem: tables 3/90/1 allocated, magic 0x67 in every record, structures 3..5 stay NULL");
    EXPECT(CallR(H_IsEnabled, 0, NULL) == 1 && CallR(H_IsFirstBoot, 0, NULL) == 1 && CallR(H_NeedFirstBoot, 0, NULL) == 0,
           "after InitSystem: IsEnabled 1, IsFirstBoot 1 (no save file), NeedFirstBoot 0");
    EXPECT(CallR(H_Step, 0, NULL) == 1 && CallR(H_IsChannelInstalled, 0, NULL) == 1, "Step -> 1, IsChannelInstalled -> 1");

    // GetSlotDataEx field mapping (Wii SAV_u32_GetSlotData)
    SavRecord* r = Rec(1, 0);
    r->valid = 1; r->sec = 5; r->min = 17; r->hour = 17; r->day = 18; r->month = 9; r->year = 1973;
    for (int i = 0; i < 10; ++i) r->user[i] = 1000 + i;
    struct { int field; int32_t want; } map[] = { {0, 1}, {1, 5}, {2, 17}, {3, 17}, {4, 18}, {5, 9}, {6, 1973},
        {100, 1000}, {105, 1005}, {109, 1009}, {7, 0}, {99, 0}, {110, 0}, {-1, 0} };
    int mapBad = 0;
    for (size_t i = 0; i < sizeof(map) / sizeof(map[0]); ++i) {
        uint32_t a[3] = { 1, 0, (uint32_t)map[i].field };
        if (CallR(H_GetSlotDataEx, 3, a) != map[i].want) ++mapBad;
    }
    EXPECT(mapBad == 0, "GetSlotDataEx mapping: 0 valid, 1..5 u8 +8..+C, 6 u16 +E, 100..109 u32 +10.., others 0");
    uint32_t oob[][3] = { {3, 0, 0}, {90, 1, 0}, {1, 2, 0}, {0, 3, 0}, {0, 6, 0}, {(uint32_t)-1, 0, 0} };
    int oobBad = 0;
    for (size_t i = 0; i < 6; ++i) if (CallR(H_GetSlotDataEx, 3, oob[i]) != 0) ++oobBad;
    EXPECT(oobBad == 0, "GetSlotDataEx out of range (slot 3 of 3, slot 90 of 90, table 3/6, slot -1) -> 0");
    memset(r, 0, REC_SIZE);
    r->magic = REC_MAGIC;

    // New Game: name, feet, validate (tmp buffer), save; then read back from the file
    static const uint16_t kName[] = { 'C', 'r', 'u', 'w', 'b', 'y', 0 };
    uint32_t an[3] = { 0, 0, (uint32_t)(uintptr_t)kName };
    CallV(H_SetSlotNameWEx, 3, an);
    uint32_t au[4] = { 0, 0, 105, 31750 };
    CallV(H_SetSlotUserDataEx, 4, au);
    TmpBuf() = (uint32_t)(uintptr_t)FkAlloc(0x1000);
    TmpSize() = 64;
    for (int i = 0; i < 64; ++i) ((uint8_t*)(uintptr_t)TmpBuf())[i] = (uint8_t)(i * 7);
    CallV(H_SaveSlotEx, 2, a2);
    std::string p00 = std::string(dir) + "/sav/slt_0_0.sav";
    EXPECT(FileSize(p00) == (long)(REC_SIZE + 64), "SaveSlotEx(0,0): sav/slt_0_0.sav = header 0xC0 + 64 bytes (%ld)", FileSize(p00));
    SavRecord saved = *Rec(0, 0);
    memset(Rec(0, 0), 0, REC_SIZE);
    Rec(0, 0)->magic = REC_MAGIC;
    memset((void*)(uintptr_t)TmpBuf(), 0, 64);
    TmpSize() = 0;
    CallV(H_ReadSlotEx, 2, a2);
    bool dataOk = TmpSize() == 64;
    for (int i = 0; dataOk && i < 64; ++i) dataOk = ((uint8_t*)(uintptr_t)TmpBuf())[i] == (uint8_t)(i * 7);
    uint32_t af[3] = { 0, 0, 105 };
    const uint16_t* back = (const uint16_t*)(uintptr_t)(uint32_t)CallR(H_GetSlotNameWEx, 2, a2);
    EXPECT(dataOk && Rec(0, 0)->valid == 1 && CallR(H_GetSlotDataEx, 3, af) == 31750 && Narrow(back) == "Cruwby" &&
           Rec(0, 0)->year == 2026 && memcmp(&saved, Rec(0, 0), REC_SIZE) == 0,
           "ReadSlotEx(0,0) through the PC reader: valid 1, \"Cruwby\", feet 31750, time, 64 data bytes");

    // figurine (merged, user buffer): SetSlotUserBufferEx + SaveSlotEx(3,1) at 3*(0xC0+S_1), header read back
    std::vector<uint8_t> jpeg(6144);
    for (size_t i = 0; i < jpeg.size(); ++i) jpeg[i] = (uint8_t)(i ^ 0x5A);
    uint32_t fb[3] = { (uint32_t)(uintptr_t)&jpeg[0], 0, 6144 };
    CallFb(H_SetSlotUserBufferEx, 3, 1, fb);
    uint32_t* ubp = (uint32_t*)(uintptr_t)Rec(3, 1)->userBuffer;
    EXPECT(ubp && ubp[0] == 0xFFFFFFFF && ubp[1] == 0xFFFFFFFF && ubp[2] == UB_DATA && memcmp(ubp + 3, &jpeg[0], 6144) == 0,
           "SetSlotUserBufferEx(3,1): 0x2020 block {-1,-1,0x2014} + data (Wii SAV_SetSlotUserBuffer)");
    TmpSize() = 16;
    uint32_t a31[2] = { 3, 1 };
    CallV(H_SaveSlotEx, 2, a31);
    long want = (long)(3 * (REC_SIZE + 0x2027C) + REC_SIZE + UB_SIZE + 16);
    EXPECT(FileSize(std::string(dir) + "/sav/slt_xx_1.sav") == want, "SaveSlotEx(3,1): slt_xx_1.sav region at 3*(0xC0+S), size %ld (want %ld)",
           FileSize(std::string(dir) + "/sav/slt_xx_1.sav"), want);
    memset(ubp + 3, 0, 6144);
    Rec(3, 1)->valid = 0;
    CallV(H_ReadSlotHeaderEx, 2, a31);
    uint32_t fb2[3] = { 0, 0, 6144 };
    CallFb(H_GetSlotUserBufferEx, 3, 1, fb2);
    EXPECT(Rec(3, 1)->valid == 1 && fb2[0] && fb2[2] == 6144 && fb2[1] == fb2[0] + 6144 &&
           memcmp((void*)(uintptr_t)fb2[0], &jpeg[0], 6144) == 0,
           "ReadSlotHeaderEx(3,1) + GetSlotUserBufferEx: user buffer restored from the merged file");
    uint32_t fb3[3] = { (uint32_t)(uintptr_t)FkAlloc(8), 0, 6144 };
    CallFb(H_GetSlotUserBufferEx, 50, 2, fb3);
    EXPECT(fb3[0] == 0 && fb3[2] == 0, "GetSlotUserBufferEx on a missing slot frees the buffer, size 0 (engine behaviour)");

    // validity, delete, update
    EXPECT(CallR(H_GetSlotDataEx, 3, a3) == 1, "slot 0 of structure 0 valid before delete");
    CallV(H_DeleteSlotEx, 2, a2);
    Rec(0, 0)->valid = 1;
    CallV(H_ReadSlotValidity, 2, a2);
    EXPECT(Rec(0, 0)->valid == 0, "DeleteSlotEx writes valid 0; ReadSlotValidity takes the flag from the file");
    uint32_t a20[2] = { 2, 0 };
    Rec(2, 0)->valid = 1;
    CallV(H_ReadSlotValidity, 2, a20);
    EXPECT(Rec(2, 0)->valid == 0, "ReadSlotValidity of a slot without file -> 0");
    Rec(1, 1)->valid = 1;
    CallV(H_UpdateValid, 0, NULL);
    EXPECT(Rec(3, 1)->valid == 1 && Rec(1, 1)->valid == 0 && Rec(0, 0)->valid == 0,
           "UpdateValid rescans every header (figurine 3 valid, figurine 1 without region invalid)");
    uint32_t arc[1] = { 5 };
    bool rcOk;
    Call(H_ReturnCommand, 1, arc, &rcOk, 0);
    EXPECT(rcOk, "ReturnCommand pops its argument");

    #undef EXPECT
    rep += "  --- handler log ---\n" + log;
    // restore the game state
    for (int t = 0; t < SAV_TABLES; ++t) {
        if (!TablePtr(t)) continue;
        for (uint32_t s = 0; s < s_tableCap[t]; ++s)
            if (Rec((int)s, t) && Rec((int)s, t)->userBuffer) FkFree((void*)(uintptr_t)Rec((int)s, t)->userBuffer);
        FkFree((void*)(uintptr_t)TablePtr(t));
    }
    FkFree((void*)(uintptr_t)TmpBuf());
    FkFree((void*)(uintptr_t)fb2[0]);
    s_testLog = NULL;
    s_ctx = oldCtx;
    s_bootDone = oldBoot;
    s_channelInstalled = oldChan;
    memcpy(s_tableOwn, oldOwn, sizeof(oldOwn));
    memcpy(s_tableCap, oldCap, sizeof(oldCap));
    memcpy(s_saveDir, oldDir, sizeof(oldDir));
    s_lastCall.clear();
    s_lastRepeat = 0;
    delete fk;
    return fails;
}

}  // namespace

extern "C" int __cdecl WiimoteSavSelfTest(const char* dir, char* out, int outSize) {
    std::string rep;
    int fails = RunSelfTest(dir ? dir : ".", rep);
    if (out && outSize > 0) {
        size_t n = rep.size() < (size_t)outSize - 1 ? rep.size() : (size_t)outSize - 1;
        memcpy(out, rep.c_str(), n);
        out[n] = 0;
    }
    return fails;
}

// ---------------------------------------------------------------------------------------------------------------
// entry points
// ---------------------------------------------------------------------------------------------------------------
void SavAttach(const std::string& iniPath) {
    s_inAttach = true;
    bool found = false;
    ReadSaveIni(iniPath, s_iniEnabled, s_channelInstalled, found);
    if (!s_iniEnabled) {
        SavLog("[save] enabled=0: the SAV natives of the PC executable are left untouched");
        s_inAttach = false;
        return;
    }
    SetupGameCtx();
    ProcessImage img;
    uint8_t probe[5];
    if (!img.Read(kHandlers[0].va, probe, 5)) {
        SavLog("host process is not the RGH PC executable (no code at %08X): save natives not installed", kHandlers[0].va);
        s_inAttach = false;
        return;
    }
    std::string rep;
    int bad = Verify(img, rep, NULL);
    if (bad) {
        SavLog("the PC executable differs from the expected 2010 build in %d place(s): save natives NOT installed", bad);
        size_t pos = 0;
        while (pos < rep.size()) {
            size_t end = rep.find('\n', pos);
            std::string line = rep.substr(pos, end - pos);
            if (line.find("FAIL") != std::string::npos) SavLog("%s", line.c_str());
            pos = end == std::string::npos ? rep.size() : end + 1;
        }
        s_inAttach = false;
        return;
    }
    int n = 0;
    for (size_t i = 0; i < sizeof(kHandlers) / sizeof(kHandlers[0]); ++i) {
        uint8_t jmp[5] = { 0xE9 };
        int32_t rel = (int32_t)((uint32_t)(uintptr_t)kHandlers[i].fn - (kHandlers[i].va + 5));
        memcpy(jmp + 1, &rel, 4);
        if (WriteCode(kHandlers[i].va, jmp, 5)) ++n;
        else SavLog("cannot patch %s at %08X (error %lu)", kHandlers[i].name, kHandlers[i].va, GetLastError());
    }
    int m = 0;
    for (size_t i = 0; i < sizeof(kExecPathCalls) / sizeof(kExecPathCalls[0]); ++i) {
        int32_t rel = (int32_t)((uint32_t)(uintptr_t)&SavExecPathForSaves - (kExecPathCalls[i] + 5));
        if (WriteCode(kExecPathCalls[i] + 1, &rel, 4)) ++m;
    }
    for (size_t i = 0; i < sizeof(kWords) / sizeof(kWords[0]); ++i) {
        uint32_t v = (uint32_t)(uintptr_t)kWords[i].fn;
        WriteCode(kWords[i].reg, &v, 4);        // in case RegisterFunctions runs (again) later
    }
    s_patched = true;
    TryWordPatches();
    SavLog("PC executable verified: %d/%d SAV handlers redirected, %d/%d save path call sites -> <exe dir>/sav, "
           "per-word natives %s%s%s (channel_installed=%d)", n, (int)(sizeof(kHandlers) / sizeof(kHandlers[0])), m,
           (int)(sizeof(kExecPathCalls) / sizeof(kExecPathCalls[0])), s_wordDone[0] ? "IsChannelInstalled " : "",
           s_wordDone[1] ? "ReturnCommand " : "", s_wordDone[2] ? "InstallChannel" : "(pending until SAV_InitSystem)",
           s_channelInstalled ? 1 : 0);
    s_inAttach = false;
}

void SavAfterConfig() {
    for (size_t i = 0; i < s_pending.size(); ++i) Log("SAV: %s", s_pending[i].c_str());
    s_pending.clear();
    if (g_cfg.savEnabled != s_iniEnabled || g_cfg.savChannelInstalled != s_channelInstalled)
        Log("SAV: note: [save] read in DllMain (enabled=%d channel_installed=%d) differs from the profile API (%d %d)",
            s_iniEnabled, s_channelInstalled, g_cfg.savEnabled, g_cfg.savChannelInstalled);
}

extern "C" {

// wmtest only: verify every patch site against an exe file.  Returns the number of differences (-1: unreadable).
int __cdecl WiimoteSavCheckExe(const char* exePath, char* out, int outSize) {
    FileImage img;
    std::string rep;
    int bad = -1;
    if (!img.Load(exePath)) rep = "  FAIL  cannot read " + std::string(exePath ? exePath : "(null)") + "\n";
    else bad = Verify(img, rep, &img);
    if (out && outSize > 0) {
        size_t n = rep.size() < (size_t)outSize - 1 ? rep.size() : (size_t)outSize - 1;
        memcpy(out, rep.c_str(), n);
        out[n] = 0;
    }
    return bad;
}

}  // extern "C"
