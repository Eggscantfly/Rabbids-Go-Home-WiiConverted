// settings.cpp - the game's launch line, options.ini and the mods folder (see settings.h).
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <algorithm>
#include <cstdlib>
#include <cwchar>
#include <cwctype>
#include <fstream>
#include <sstream>

#include "settings.h"

namespace rgh {

const Choice kLanguage[]   = { {L"English", "en"}, {L"French", "fr"}, {L"German", "de"}, {L"Italian", "it"},
                               {L"Spanish", "es"}, {L"Dutch", "nl"}, {L"Japanese", "ja"} };
const int N_LANGUAGE = 7;
const int N_RELEASE_LANGUAGE = 6;
const Choice kResolution[] = { {L"1280 x 720", "1280x720"}, {L"1600 x 900", "1600x900"}, {L"1920 x 1080", "1920x1080"},
                               {L"2560 x 1440", "2560x1440"}, {L"Game default", ""} };
const int N_RESOLUTION = 5;
const Choice kDisplay[]    = { {L"Window", "window"}, {L"Borderless full screen", "borderless"} };
const int N_DISPLAY = 2;
const Choice kFrameRate[]  = { {L"60 Hz (like the Wii)", "-1"}, {L"30 Hz", "30"}, {L"120 Hz", "120"}, {L"144 Hz", "144"},
                               {L"Uncapped", "0"} };
const int N_FRAMERATE = 5;

namespace {

const wchar_t* kOptionsFile = L"options.ini";
const wchar_t* kModsDir = L"mods";
// how the game is started when the folder has no launch line of its own: the loading mode the release uses and a
// language.  /versionindex:5 is not among them - the release's own launcher passed it and the game then calls itself
// "Rabbids Go Home - DVD"; this is the whole game, so it is left off (kDropped).
const char* kDefaultSwitches = "/binload/fe /lang/en";
const char* kDropped[] = { "/versionindex" };

bool ReadText(const std::wstring& path, std::string& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    std::ostringstream ss;
    ss << f.rdbuf();
    out = ss.str();
    return true;
}

bool WriteText(const std::wstring& path, const std::string& text) {
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) return false;
    f << text;
    return bool(f);
}

std::string Lower(std::string s) {
    for (char& c : s) c = (char)tolower((unsigned char)c);
    return s;
}

std::string Trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t");
    size_t b = s.find_last_not_of(" \t");
    return a == std::string::npos ? "" : s.substr(a, b - a + 1);
}

std::vector<std::string> Lines(const std::string& text) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : text) {
        if (c == '\n') { out.push_back(cur); cur.clear(); }
        else if (c != '\r') cur += c;
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

std::string Join(const std::vector<std::string>& lines) {
    std::string out;
    for (const std::string& l : lines) { out += l; out += "\r\n"; }
    return out;
}

std::string GetSwitch(const Launch& L, const std::string& prefix) {
    for (const std::string& s : L.switches) {
        std::string low = Lower(s);
        if (low.compare(0, prefix.size(), prefix) == 0) return low.substr(prefix.size());
    }
    return "";
}

bool HasSwitch(const Launch& L, const std::string& sw) {
    for (const std::string& s : L.switches) if (Lower(s) == sw) return true;
    return false;
}

// every switch starting with `prefix` becomes prefix + value (when keep), or goes away
void SetSwitch(Launch& L, const std::string& prefix, const std::string& value, bool keep) {
    std::vector<std::string> out;
    bool done = false;
    for (const std::string& s : L.switches) {
        if (Lower(s).compare(0, prefix.size(), prefix) == 0) {
            if (keep && !done) { out.push_back(prefix + value); done = true; }
        } else {
            out.push_back(s);
        }
    }
    if (keep && !done) out.push_back(prefix + value);
    L.switches = out;
}

std::vector<std::string> Words(const std::string& s) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : s) {
        if (c == ' ' || c == '\t') { if (!cur.empty()) out.push_back(cur); cur.clear(); }
        else cur += c;
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

std::string JoinWords(const std::vector<std::string>& v) {
    std::string out;
    for (const std::string& s : v) { if (!out.empty()) out += " "; out += s; }
    return out;
}

// every switch starting with `prefix` goes away
void DropSwitch(Launch& L, const std::string& prefix) {
    std::vector<std::string> out;
    for (const std::string& s : L.switches)
        if (Lower(s).compare(0, prefix.size(), prefix) != 0) out.push_back(s);
    L.switches = out;
}

std::wstring LowerW(std::wstring s) {
    for (wchar_t& c : s) c = (wchar_t)towlower(c);
    return s;
}

// the file names of a folder matching a pattern, with their sizes
void FilesIn(const std::wstring& dir, const wchar_t* pattern,
             std::vector<std::pair<std::wstring, unsigned long long>>& out) {
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"\\" + pattern).c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        out.push_back(std::make_pair(std::wstring(fd.cFileName),
                                     ((unsigned long long)fd.nFileSizeHigh << 32) | fd.nFileSizeLow));
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

// the game's own program in a folder: the one the setup installs, else the largest program that is not a launcher or
// an uninstaller (the launcher itself lives in a subfolder, so it is not in the way)
std::wstring GameExe(const std::wstring& dir) {
    std::vector<std::pair<std::wstring, unsigned long long>> files;
    FilesIn(dir, L"*.exe", files);
    std::wstring best;
    unsigned long long bestSize = 0;
    for (size_t i = 0; i < files.size(); ++i) {
        std::wstring low = LowerW(files[i].first);
        if (low == L"lyn_f.exe") return files[i].first;
        if (low.find(L"launcher") != std::wstring::npos || low.find(L"unins") != std::wstring::npos ||
            low.find(L"setup") != std::wstring::npos) continue;
        if (files[i].second > bestSize) { bestSize = files[i].second; best = files[i].first; }
    }
    return best;
}

// the main bigfile: <base>.bf with no branch part (the siblings are <base>.<branch>.<ext>.bf), the largest of them
std::wstring MainBigfile(const std::wstring& dir) {
    std::vector<std::pair<std::wstring, unsigned long long>> files;
    FilesIn(dir, L"*.bf", files);
    std::wstring best;
    unsigned long long bestSize = 0;
    for (size_t i = 0; i < files.size(); ++i) {
        std::wstring low = LowerW(files[i].first);
        if (low.size() < 4 || low.substr(low.size() - 3) != L".bf") continue;
        if (low.substr(0, low.size() - 3).find(L'.') != std::wstring::npos) continue;
        if (files[i].second > bestSize) { bestSize = files[i].second; best = files[i].first; }
    }
    return best;
}

std::string Narrow(const std::wstring& w) {
    if (w.empty()) return "";
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), NULL, 0, NULL, NULL);
    std::string s(n, 0);
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), &s[0], n, NULL, NULL);
    return s;
}

std::string IniGet(const Ini& ini, const std::string& section, const std::string& key, const std::string& def) {
    bool in = false;
    for (const std::string& raw : ini.lines) {
        std::string l = Trim(raw);
        if (l.empty() || l[0] == ';' || l[0] == '#') continue;
        if (l[0] == '[') { in = Lower(l) == "[" + section + "]"; continue; }
        if (!in) continue;
        size_t eq = l.find('=');
        if (eq == std::string::npos) continue;
        if (Lower(Trim(l.substr(0, eq))) == key) return Trim(l.substr(eq + 1));
    }
    return def;
}

void IniSet(Ini& ini, const std::string& section, const std::string& key, const std::string& value) {
    bool in = false;
    size_t last = std::string::npos;
    for (size_t i = 0; i < ini.lines.size(); ++i) {
        std::string l = Trim(ini.lines[i]);
        if (l.empty() || l[0] == ';' || l[0] == '#') continue;
        if (l[0] == '[') {
            if (in) break;
            in = Lower(l) == "[" + section + "]";
            if (in) last = i;
            continue;
        }
        if (!in) continue;
        last = i;
        size_t eq = l.find('=');
        if (eq != std::string::npos && Lower(Trim(l.substr(0, eq))) == key) {
            ini.lines[i] = key + "=" + value;
            return;
        }
    }
    if (last == std::string::npos) {
        if (!ini.lines.empty() && !Trim(ini.lines.back()).empty()) ini.lines.push_back("");
        ini.lines.push_back("[" + section + "]");
        ini.lines.push_back(key + "=" + value);
    } else {
        ini.lines.insert(ini.lines.begin() + last + 1, key + "=" + value);
    }
}

int IndexOf(const Choice* items, int n, const std::string& value) {
    for (int i = 0; i < n; ++i) if (items[i].value == value) return i;
    return 0;
}

// the keys of the object a JSON key holds: "languages": {"fr": 0, "en": 1} -> fr en (enough for the setup's summary)
std::vector<std::string> JsonObjectKeys(const std::string& text, const std::string& key) {
    std::vector<std::string> out;
    size_t k = text.find("\"" + key + "\"");
    if (k == std::string::npos) return out;
    size_t a = text.find('{', k);
    size_t colon = text.find(':', k);
    if (a == std::string::npos || colon == std::string::npos || a < colon) return out;
    size_t b = text.find('}', a);
    if (b == std::string::npos) return out;
    size_t p = a;
    while (true) {
        size_t q1 = text.find('"', p);
        if (q1 == std::string::npos || q1 > b) break;
        size_t q2 = text.find('"', q1 + 1);
        if (q2 == std::string::npos || q2 > b) break;
        size_t c = text.find_first_not_of(" \t\r\n", q2 + 1);
        if (c != std::string::npos && c < b && text[c] == ':') out.push_back(text.substr(q1 + 1, q2 - q1 - 1));
        p = q2 + 1;
    }
    return out;
}

// a JSON string literal starting at the opening quote `q1`: its decoded value; `end` = the position after the
// closing quote (std::string::npos when the literal is not closed)
std::string JsonLiteral(const std::string& text, size_t q1, size_t& end) {
    std::string out;
    size_t i = q1 + 1;
    while (i < text.size()) {
        char c = text[i];
        if (c == '"') { end = i + 1; return out; }
        if (c == '\\' && i + 1 < text.size()) {
            char e = text[++i];
            switch (e) {
            case 'n': out += '\n'; break;
            case 't': out += '\t'; break;
            case 'r': break;
            case 'b': case 'f': break;
            case 'u':
                if (i + 4 < text.size()) {
                    unsigned code = (unsigned)strtoul(text.substr(i + 1, 4).c_str(), NULL, 16);
                    i += 4;
                    // UTF-8 of the code point (surrogate pairs are left as two code points: good enough here)
                    if (code < 0x80) out += (char)code;
                    else if (code < 0x800) { out += (char)(0xC0 | (code >> 6)); out += (char)(0x80 | (code & 0x3F)); }
                    else { out += (char)(0xE0 | (code >> 12)); out += (char)(0x80 | ((code >> 6) & 0x3F)); out += (char)(0x80 | (code & 0x3F)); }
                }
                break;
            default: out += e; break;       // \" \\ \/
            }
        } else {
            out += c;
        }
        ++i;
    }
    end = std::string::npos;
    return out;
}

// the position of the value of a top-level-ish key ("key": ...), or npos
size_t JsonValue(const std::string& text, const std::string& key) {
    size_t k = text.find("\"" + key + "\"");
    if (k == std::string::npos) return std::string::npos;
    size_t c = text.find(':', k + key.size() + 2);
    if (c == std::string::npos) return std::string::npos;
    return text.find_first_not_of(" \t\r\n", c + 1);
}

std::wstring JsonString(const std::string& text, const std::string& key) {
    size_t v = JsonValue(text, key);
    if (v == std::string::npos || text[v] != '"') return L"";
    size_t end;
    return Widen(JsonLiteral(text, v, end));
}

// "key": ["a", "b"] -> a b
std::vector<std::wstring> JsonStringArray(const std::string& text, const std::string& key) {
    std::vector<std::wstring> out;
    size_t v = JsonValue(text, key);
    if (v == std::string::npos || text[v] != '[') return out;
    size_t i = v + 1;
    while (i < text.size() && text[i] != ']') {
        if (text[i] == '"') {
            size_t end;
            std::string s = JsonLiteral(text, i, end);
            if (end == std::string::npos) break;
            out.push_back(Widen(s));
            i = end;
        } else {
            ++i;
        }
    }
    return out;
}

bool FileExists(const std::wstring& path) {
    DWORD a = GetFileAttributesW(path.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

bool IsPicture(const std::wstring& name) {
    size_t dot = name.find_last_of(L'.');
    if (dot == std::wstring::npos) return false;
    std::wstring ext = name.substr(dot + 1);
    for (wchar_t& c : ext) c = (wchar_t)towlower(c);
    return ext == L"png" || ext == L"jpg" || ext == L"jpeg" || ext == L"bmp" || ext == L"webp";
}

// the picture files of a folder, sorted by name
std::vector<std::wstring> Pictures(const std::wstring& folder) {
    std::vector<std::wstring> out;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((folder + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return out;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && IsPicture(fd.cFileName))
            out.push_back(folder + L"\\" + fd.cFileName);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    std::sort(out.begin(), out.end());
    return out;
}

std::vector<std::wstring> SplitList(const std::wstring& s) {
    std::vector<std::wstring> out;
    std::wstring cur;
    for (wchar_t c : s) {
        if (c == L'|') { if (!cur.empty()) out.push_back(cur); cur.clear(); }
        else cur += c;
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

}  // namespace

std::wstring Widen(const std::string& s) {
    if (s.empty()) return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), NULL, 0);
    std::wstring w(n, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], n);
    return w;
}

std::wstring ExeDir() {
    wchar_t buf[MAX_PATH * 2];
    DWORD n = GetModuleFileNameW(NULL, buf, MAX_PATH * 2);
    std::wstring p(buf, n);
    size_t cut = p.find_last_of(L"\\/");
    return cut == std::wstring::npos ? L"." : p.substr(0, cut);
}

bool IsGameFolder(const std::wstring& dir) {
    return !GameExe(dir).empty() && !MainBigfile(dir).empty();
}

Launch ReadLaunch(const std::wstring& dir, const Ini& ini) {
    Launch L;
    std::wstring exe = GameExe(dir), big = MainBigfile(dir);
    if (!exe.empty() && !big.empty()) {
        L.exe = Narrow(exe);
        L.bigfile = Narrow(big);
        std::string sw = IniGet(ini, "launch", "switches", "");
        L.switches = Words(sw.empty() ? std::string(kDefaultSwitches) : sw);
        L.found = true;
    }
    for (size_t i = 0; i < sizeof(kDropped) / sizeof(kDropped[0]); ++i) DropSwitch(L, kDropped[i]);
    return L;
}

Ini ReadIni(const std::wstring& dir) {
    Ini ini;
    std::string text;
    ini.present = ReadText(dir + L"\\" + kOptionsFile, text);
    if (ini.present) ini.lines = Lines(text);
    return ini;
}

std::vector<Choice> GameLanguages(const std::wstring& dir, const Launch& L) {
    std::vector<Choice> out;
    std::string text;
    std::vector<std::string> codes;
    if (L.found && ReadText(dir + L"\\" + Widen(L.bigfile) + L".json", text)) codes = JsonObjectKeys(text, "languages");
    if (codes.empty()) {
        for (int i = 0; i < N_RELEASE_LANGUAGE; ++i) out.push_back(kLanguage[i]);
        return out;
    }
    for (int i = 0; i < N_LANGUAGE; ++i)
        if (std::find(codes.begin(), codes.end(), kLanguage[i].value) != codes.end()) out.push_back(kLanguage[i]);
    if (out.empty()) out.push_back(kLanguage[0]);
    return out;
}

Settings LoadSettings(const Launch& L, const Ini& ini, const std::vector<Choice>& langs) {
    Settings s;
    s.lang = IndexOf(langs.data(), (int)langs.size(), GetSwitch(L, "/lang/"));
    s.res = IndexOf(kResolution, N_RESOLUTION, GetSwitch(L, "/res"));
    s.display = IndexOf(kDisplay, N_DISPLAY, IniGet(ini, "graphics", "display", "window"));
    s.frameRate = IndexOf(kFrameRate, N_FRAMERATE, IniGet(ini, "graphics", "frame_rate", "-1"));
    s.vsync = IniGet(ini, "graphics", "vsync", "1") != "0";
    s.effects = IniGet(ini, "graphics", "wii_effects", "1") != "0";
    s.fps = HasSwitch(L, "/fps");
    return s;
}

bool SaveSettings(const std::wstring& dir, const Settings& s, Launch& L, Ini& ini, const std::vector<Choice>& langs) {
    if (!L.found || langs.empty()) return false;
    IniSet(ini, "graphics", "display", kDisplay[s.display].value);
    IniSet(ini, "graphics", "frame_rate", kFrameRate[s.frameRate].value);
    IniSet(ini, "graphics", "vsync", s.vsync ? "1" : "0");
    IniSet(ini, "graphics", "wii_effects", s.effects ? "1" : "0");
    SetSwitch(L, "/lang/", langs[std::min<int>(s.lang, (int)langs.size() - 1)].value, true);
    std::string res = kResolution[s.res].value;
    SetSwitch(L, "/res", res, !res.empty());
    SetSwitch(L, "/fps", "", s.fps);
    IniSet(ini, "launch", "switches", JoinWords(L.switches));   // the command line, for the next start
    bool ok = WriteIni(dir, ini);
    return ok;
}

bool WriteIni(const std::wstring& dir, Ini& ini) {
    if (!ini.present) {
        ini.lines.insert(ini.lines.begin(), "; options.ini - the game's settings (the launcher and the Options screen of the pause menu write it).");
        ini.present = true;
    }
    return WriteText(dir + L"\\" + kOptionsFile, Join(ini.lines));
}

std::vector<Mod> ScanMods(const std::wstring& dir, const Ini& ini) {
    std::vector<Mod> out;
    std::vector<std::wstring> enabled = SplitList(Widen(IniGet(ini, "mods", "enabled", "")));
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"\\" + kModsDir + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return out;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || fd.cFileName[0] == L'.') continue;
        std::wstring folder = dir + L"\\" + kModsDir + L"\\" + fd.cFileName;
        std::string text;
        Mod m;
        m.folder = fd.cFileName;
        m.name = fd.cFileName;
        std::wstring icon = L"icon.png";
        std::vector<std::wstring> shots;
        if (ReadText(folder + L"\\mod.json", text)) {
            std::wstring n = JsonString(text, "name");
            if (!n.empty()) m.name = n;
            m.author = JsonString(text, "author");
            m.version = JsonString(text, "version");
            m.description = JsonString(text, "description");
            std::wstring i = JsonString(text, "icon");
            if (!i.empty()) icon = i;
            shots = JsonStringArray(text, "screenshots");
            for (const std::wstring& sw : JsonStringArray(text, "switches"))
                if (!sw.empty() && sw[0] == L'/') m.switches.push_back(Narrow(sw));
        }
        if (FileExists(folder + L"\\" + icon)) m.icon = folder + L"\\" + icon;
        if (shots.empty()) {
            m.screenshots = Pictures(folder + L"\\screenshots");
        } else {
            for (const std::wstring& s : shots)
                if (FileExists(folder + L"\\" + s)) m.screenshots.push_back(folder + L"\\" + s);
        }
        m.enabled = std::find(enabled.begin(), enabled.end(), m.folder) != enabled.end();
        out.push_back(m);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    std::sort(out.begin(), out.end(), [](const Mod& a, const Mod& b) { return _wcsicmp(a.name.c_str(), b.name.c_str()) < 0; });
    return out;
}

void SetModEnabled(Ini& ini, const std::wstring& folder, bool on) {
    std::vector<std::wstring> enabled = SplitList(Widen(IniGet(ini, "mods", "enabled", "")));
    enabled.erase(std::remove(enabled.begin(), enabled.end(), folder), enabled.end());
    if (on) enabled.push_back(folder);
    std::wstring joined;
    for (const std::wstring& e : enabled) joined += (joined.empty() ? L"" : L"|") + e;
    IniSet(ini, "mods", "enabled", Narrow(joined));
}

bool GameCommand(const std::wstring& dir, const Launch& L, const std::vector<Mod>& mods, std::wstring& exe,
                 std::wstring& cmd, std::vector<std::string>& switches) {
    if (!L.found) return false;
    exe = dir + L"\\" + Widen(L.exe);
    cmd = L"\"" + exe + L"\" \"" + dir + L"\\" + Widen(L.bigfile) + L"\"";
    switches = L.switches;
    for (const Mod& m : mods)                      // the enabled mods' own switches (e.g. /wog<key>: a test world)
        if (m.enabled)
            for (const std::string& sw : m.switches)
                if (std::find(switches.begin(), switches.end(), sw) == switches.end()) switches.push_back(sw);
    for (const std::string& s : switches) cmd += L" " + Widen(s);
    return true;
}

}  // namespace rgh
