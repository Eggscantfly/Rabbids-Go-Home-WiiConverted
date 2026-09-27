// settings.h - what the launcher reads and writes in the game folder, with no Qt in it:
//
//   the game      the launcher starts the executable itself, with the bigfile and the switches as its arguments; there
//                is no batch file.  Both are found in the game folder: the executable rghport's setup installs
//                (LyN_f.exe, else the only program there) and the main bigfile (<base>.bf, no branch part); the
//                switches are [launch] switches= of options.ini (the setup stores the release's there, with the
//                language; a switch added by hand is kept), else the defaults.
//   options.ini  [graphics] of wiimote.dll's Options screen: display, vsync, frame_rate, wii_effects; [launch]
//                switches, the game's command line; the other sections and every comment are kept as they are.
//                /lang/xx is what the release's own launcher wrote (en fr de it es nl); the game shows texts only
//                for the languages its Wii disc had, listed in <bigfile>.json by the setup ("languages"):
//                GameLanguages
//   mods\        one folder per mod, each with a mod.json: name, author, version, description, icon (a picture
//                file in the folder; default icon.png) and screenshots (a list of picture files; default: every
//                picture in the folder's screenshots subfolder).  Which mods are enabled is [mods] enabled=a|b of
//                options.ini (the folder names).  Enabling only records the choice for now: the mod loader that
//                applies them to the game is the next step.
#pragma once

#include <string>
#include <vector>

namespace rgh {

struct Choice { const wchar_t* label; const char* value; };
extern const Choice kLanguage[];   extern const int N_LANGUAGE;   // every language the launcher knows, /lang/<value>
extern const int N_RELEASE_LANGUAGE;                              // the first ones: the release launcher's six
extern const Choice kResolution[]; extern const int N_RESOLUTION;
extern const Choice kDisplay[];    extern const int N_DISPLAY;
extern const Choice kFrameRate[];  extern const int N_FRAMERATE;

struct Launch {
    bool found = false;                  // there is a game here: an executable and a bigfile
    std::string exe, bigfile;
    std::vector<std::string> switches;   // as stored: /binload/fe /lang/en /fps ...
};

struct Ini {
    std::vector<std::string> lines;
    bool present = false;
};

struct Settings {
    int lang = 0, res = 0, display = 0, frameRate = 0;   // indexes into the Choice tables (lang: the GameLanguages list)
    bool vsync = true, effects = true, fps = false;
};

struct Mod {
    std::wstring folder;                       // the folder name under mods\ (what the enabled list holds)
    std::wstring name, author, version, description;
    std::wstring icon;                         // full path of the icon picture, empty for none
    std::vector<std::wstring> screenshots;     // full paths
    std::vector<std::string> switches;         // command line switches the game gets while the mod is on
    bool enabled = false;
};

std::wstring ExeDir();
Ini ReadIni(const std::wstring& dir);
// the game's launch line: the executable and bigfile found in the folder, with the switches out of the ini (read the
// ini first)
Launch ReadLaunch(const std::wstring& dir, const Ini& ini);
// true when the folder holds a game the launcher can start (an executable and a main bigfile)
bool IsGameFolder(const std::wstring& dir);
// the languages the game folder has texts for, in kLanguage order: those <bigfile>.json lists, else (a folder from
// an older setup, where every language shows English) the release launcher's six
std::vector<Choice> GameLanguages(const std::wstring& dir, const Launch& L);
Settings LoadSettings(const Launch& L, const Ini& ini, const std::vector<Choice>& langs);
bool SaveSettings(const std::wstring& dir, const Settings& s, Launch& L, Ini& ini, const std::vector<Choice>& langs);
bool WriteIni(const std::wstring& dir, Ini& ini);
std::vector<Mod> ScanMods(const std::wstring& dir, const Ini& ini);
// records a mod's enabled state in the ini (in memory; WriteIni saves it)
void SetModEnabled(Ini& ini, const std::wstring& folder, bool on);
std::wstring Widen(const std::string& s);
// the game's command line and executable path from the launch line, plus the enabled mods' switches
bool GameCommand(const std::wstring& dir, const Launch& L, const std::vector<Mod>& mods, std::wstring& exe,
                 std::wstring& cmd, std::vector<std::string>& switches);

}  // namespace rgh
