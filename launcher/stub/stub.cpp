// stub.cpp - "WiiConverted Launcher.exe", the file in the game folder: it carries the launcher's icon (the WC mark,
// from the build; /FIXED keeps its resources replaceable) and starts launcher\RGHLauncher.exe, the Qt window, with the game folder as its
// working directory.  Nothing else, so it stays a plain small executable next to the game.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <string>

int WINAPI wWinMain(HINSTANCE, HINSTANCE, LPWSTR, int) {
    wchar_t buf[MAX_PATH * 2];
    DWORD n = GetModuleFileNameW(NULL, buf, MAX_PATH * 2);
    std::wstring dir(buf, n);
    size_t cut = dir.find_last_of(L"\\/");
    dir = cut == std::wstring::npos ? L"." : dir.substr(0, cut);
    std::wstring app = dir + L"\\launcher\\RGHLauncher.exe";
    if (GetFileAttributesW(app.c_str()) == INVALID_FILE_ATTRIBUTES) {
        MessageBoxW(NULL, L"The launcher folder is missing next to this program.\nRun the setup again into this folder "
                          L"to put it back.", L"WiiConverted Launcher", MB_ICONERROR);
        return 1;
    }
    std::wstring cmd = L"\"" + app + L"\"";
    STARTUPINFOW si = { sizeof(si) };
    PROCESS_INFORMATION pi = {};
    if (!CreateProcessW(app.c_str(), &cmd[0], NULL, NULL, FALSE, 0, NULL, dir.c_str(), &si, &pi)) {
        MessageBoxW(NULL, L"The launcher could not be started.", L"WiiConverted Launcher", MB_ICONERROR);
        return 1;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return 0;
}
