// wm_patch.cpp - helpers shared by the patching modules (wm_sav.cpp, wm_ctl.cpp): reading the PC executable (in
// memory or from its file), CRC32 signatures, code writes and handler replacement in the script native table.
#include "wiimote.h"

#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace wmpatch {

uint32_t Crc32(const uint8_t* p, size_t n) {
    uint32_t c = 0xFFFFFFFF;
    for (size_t i = 0; i < n; ++i) {
        c ^= p[i];
        for (int k = 0; k < 8; ++k) c = (c >> 1) ^ (0xEDB88320 & (0 - (c & 1)));
    }
    return ~c;
}

bool ProcessImage::Read(uint32_t va, void* out, uint32_t n) const {
    SIZE_T got = 0;
    return ReadProcessMemory(GetCurrentProcess(), (LPCVOID)(uintptr_t)va, out, n, &got) && got == n;
}

uint32_t FileImage::U32(size_t off) const {
    uint32_t v = 0;
    if (off + 4 <= data.size()) memcpy(&v, &data[off], 4);
    return v;
}

bool FileImage::Load(const char* path) {
    HANDLE h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size;
    bool ok = GetFileSizeEx(h, &size) && size.QuadPart > 0x400 && size.QuadPart < (64 << 20);
    if (ok) {
        data.resize((size_t)size.QuadPart);
        DWORD got = 0;
        ok = ReadFile(h, &data[0], (DWORD)data.size(), &got, NULL) && got == data.size();
    }
    CloseHandle(h);
    if (!ok) return false;
    uint32_t pe = U32(0x3C);
    if (pe + 0xF8 > data.size() || U32(pe) != 0x00004550) return false;
    uint16_t nsec = (uint16_t)(data[pe + 6] | (data[pe + 7] << 8));
    uint16_t optsz = (uint16_t)(data[pe + 20] | (data[pe + 21] << 8));
    uint32_t base = U32(pe + 24 + 28);
    for (uint32_t i = 0, off = pe + 24 + optsz; i < nsec && off + 40 <= data.size(); ++i, off += 40) {
        Sec s = { base + U32(off + 12), U32(off + 8), U32(off + 16), U32(off + 20), (U32(off + 36) & 0x20) != 0 };
        secs.push_back(s);
    }
    return !secs.empty();
}

bool FileImage::Read(uint32_t va, void* out, uint32_t n) const {
    for (size_t i = 0; i < secs.size(); ++i) {
        const Sec& s = secs[i];
        if (va >= s.va && va + n <= s.va + s.rsize && s.rptr + s.rsize <= data.size()) {
            memcpy(out, &data[s.rptr + (va - s.va)], n);
            return true;
        }
    }
    return false;
}

void Report(std::string& rep, bool ok, const char* fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf(buf, sizeof(buf) - 1, fmt, ap);
    buf[sizeof(buf) - 1] = 0;
    va_end(ap);
    rep += ok ? "  PASS  " : "  FAIL  ";
    rep += buf;
    rep += "\n";
}

bool CheckCrc(const Image& img, uint32_t va, uint32_t len, uint32_t want, uint32_t* got) {
    std::vector<uint8_t> b(len);
    if (!img.Read(va, &b[0], len)) {
        *got = 0;
        return false;
    }
    *got = Crc32(&b[0], len);
    return *got == want;
}

bool WriteCode(uint32_t va, const void* bytes, uint32_t n) {
    DWORD old, tmp;
    if (!VirtualProtect((LPVOID)(uintptr_t)va, n, PAGE_EXECUTE_READWRITE, &old)) return false;
    memcpy((void*)(uintptr_t)va, bytes, n);
    VirtualProtect((LPVOID)(uintptr_t)va, n, old, &tmp);
    FlushInstructionCache(GetCurrentProcess(), (LPCVOID)(uintptr_t)va, n);
    return true;
}

// The native table is built by ViD::b_Create (SCR_b_Init, then the RegisterScript functions) before the DLL is
// loaded; SCR::ApplyCommon reads the handler of an element at every call, so an element is replaced in place.
int PatchNativeTable(uint32_t key, uint32_t keyMask, uint32_t oldFn, uint32_t newFn, std::string& notes) {
    return PatchNativeTableAt(0x00A718AC, key, keyMask, oldFn, newFn, notes);
}

int PatchNativeTableAt(uint32_t table, uint32_t key, uint32_t keyMask, uint32_t oldFn, uint32_t newFn,
                       std::string& notes) {
    uint32_t hdr[4];                                // TOOsarray {data, element size, capacity, count}
    ProcessImage img;
    if (!img.Read(table, hdr, sizeof(hdr))) return 0;
    uint32_t data = hdr[0], esize = hdr[1], count = hdr[3];
    if (!data || esize < 8 || esize > 64 || !count || count > 0x10000) return 0;
    std::vector<uint8_t> all(count * esize);
    if (!img.Read(data, &all[0], (uint32_t)all.size())) return 0;
    int result = 0;
    char line[160];
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t k, fn;
        memcpy(&k, &all[i * esize], 4);
        memcpy(&fn, &all[i * esize + 4], 4);
        if ((k & keyMask) != (key & keyMask)) continue;
        if (fn == newFn) {
            result = 1;
            continue;
        }
        if (fn != oldFn) {
            _snprintf(line, sizeof(line) - 1, "native table entry %u (word %08X) holds %08X, expected %08X: left alone",
                      i, k, fn, oldFn);
            line[sizeof(line) - 1] = 0;
            notes += line;
            notes += "\n";
            if (!result) result = -1;
            continue;
        }
        SIZE_T put = 0;
        if (WriteProcessMemory(GetCurrentProcess(), (LPVOID)(uintptr_t)(data + i * esize + 4), &newFn, 4, &put) &&
            put == 4) {
            _snprintf(line, sizeof(line) - 1, "native table entry %u (word %08X) -> wiimote.dll", i, k);
            line[sizeof(line) - 1] = 0;
            notes += line;
            notes += "\n";
            result = 1;
        }
    }
    return result;
}

int PatchNativeHandler(uint32_t oldFn, uint32_t newFn, std::string& notes) {
    return PatchNativeHandlerAt(0x00A718AC, oldFn, newFn, notes);
}

// Every element holding oldFn gets newFn (module words whose key is assigned at registration). Returns the number of
// elements that hold newFn afterwards.
int PatchNativeHandlerAt(uint32_t table, uint32_t oldFn, uint32_t newFn, std::string& notes) {
    uint32_t hdr[4];
    ProcessImage img;
    if (!img.Read(table, hdr, sizeof(hdr))) return 0;
    uint32_t data = hdr[0], esize = hdr[1], count = hdr[3];
    if (!data || esize < 8 || esize > 64 || !count || count > 0x10000) return 0;
    std::vector<uint8_t> all(count * esize);
    if (!img.Read(data, &all[0], (uint32_t)all.size())) return 0;
    int n = 0;
    char line[160];
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t k, fn;
        memcpy(&k, &all[i * esize], 4);
        memcpy(&fn, &all[i * esize + 4], 4);
        if (fn == newFn) {
            ++n;
            continue;
        }
        if (fn != oldFn) continue;
        SIZE_T put = 0;
        if (WriteProcessMemory(GetCurrentProcess(), (LPVOID)(uintptr_t)(data + i * esize + 4), &newFn, 4, &put) &&
            put == 4) {
            _snprintf(line, sizeof(line) - 1, "native table entry %u (word %08X) -> wiimote.dll", i, k);
            line[sizeof(line) - 1] = 0;
            notes += line;
            notes += "\n";
            ++n;
        }
    }
    return n;
}

// ViD::RegisterScript element: "mov [esp+x], handler" (C7 44 24 xx or C7 04 24) with "push key" within 0x24 bytes.
bool CheckRegisterScriptEntry(const Image& img, uint32_t reg, uint32_t handler, uint32_t key) {
    const int at = 0x24;
    uint32_t v = 0;
    uint8_t win[0x48];
    if (!img.Read(reg, &v, 4) || v != handler || !img.Read(reg - at, win, sizeof(win))) return false;
    bool mov = (win[at - 4] == 0xC7 && win[at - 3] == 0x44 && win[at - 2] == 0x24) ||
               (win[at - 3] == 0xC7 && win[at - 2] == 0x04 && win[at - 1] == 0x24);
    if (!mov) return false;
    for (int i = 0; i + 5 <= (int)sizeof(win); ++i)
        if (win[i] == 0x68 && memcmp(&win[i + 1], &key, 4) == 0) return true;
    return false;
}

// Module registrar element (key = module word base + n << 16): "mov [esp+x], handler" (C7 44 24 xx) with
// "movzx eax, word [esi+0Ah]" and "add eax, n << 16" within 0x24 bytes on either side.
bool CheckModuleEntry(const Image& img, uint32_t reg, uint32_t handler) {
    const int at = 0x24;
    uint32_t v = 0;
    uint8_t win[0x48];
    if (!img.Read(reg, &v, 4) || v != handler || !img.Read(reg - at, win, sizeof(win))) return false;
    if (!(win[at - 4] == 0xC7 && win[at - 3] == 0x44 && win[at - 2] == 0x24)) return false;
    bool movzx = false, add = false;
    for (int i = 0; i + 5 <= (int)sizeof(win); ++i) {
        if (i >= at - 4 && i < at + 4) continue;    // the mov itself
        if (win[i] == 0x0F && win[i + 1] == 0xB7 && win[i + 2] == 0x46 && win[i + 3] == 0x0A) movzx = true;
        if (win[i] == 0x05 && win[i + 1] == 0x00 && win[i + 2] == 0x00 && win[i + 4] == 0x00) add = true;
    }
    return movzx && add;
}

bool ReadIniKey(const std::string& path, const char* section, const char* key, std::string& value) {
    HANDLE h = CreateFileA(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) return false;
    std::string text;
    char buf[16384];
    DWORD got = 0;
    while (ReadFile(h, buf, sizeof(buf), &got, NULL) && got) text.append(buf, got);
    CloseHandle(h);
    // an empty section means the keys written before any [header] at all, which is what a small file - a mod's own
    // config.ini - usually is
    std::string want = (section && *section) ? (std::string("[") + section + "]") : std::string();
    std::string current;
    bool found = false;
    size_t pos = 0;
    while (pos < text.size()) {
        size_t end = text.find('\n', pos);
        std::string line = text.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
        pos = end == std::string::npos ? text.size() : end + 1;
        size_t a = line.find_first_not_of(" \t\r");
        if (a == std::string::npos) continue;
        line = line.substr(a);
        while (!line.empty() && strchr("\r \t", line[line.size() - 1])) line.resize(line.size() - 1);
        if (line[0] == ';' || line[0] == '#') continue;
        if (line[0] == '[') {
            current = line.substr(0, line.find(']') + 1);
            continue;
        }
        if (_stricmp(current.c_str(), want.c_str()) != 0) continue;
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string k = line.substr(0, eq), v = line.substr(eq + 1);
        while (!k.empty() && strchr(" \t", k[k.size() - 1])) k.resize(k.size() - 1);
        if (_stricmp(k.c_str(), key) != 0) continue;
        size_t c = v.find_first_of(";#");
        if (c != std::string::npos) v.resize(c);
        size_t v0 = v.find_first_not_of(" \t");
        v = v0 == std::string::npos ? std::string() : v.substr(v0);
        while (!v.empty() && strchr(" \t", v[v.size() - 1])) v.resize(v.size() - 1);
        value = v;
        found = true;
    }
    return found;
}

}  // namespace wmpatch
