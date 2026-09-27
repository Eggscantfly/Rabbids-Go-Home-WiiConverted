// main.cpp - RGHLauncher.exe: the Qt window.  In a game folder it is the launcher: it lives in the folder's "launcher"
// subfolder (with the Qt runtime next to it) and is started by the stub "WiiConverted Launcher.exe" in the game folder
// itself, which also carries the icon (the WC mark; the game's own executable keeps the moon).  The game folder is the
// parent of the folder this program runs from (or its own folder when the game is there, for a build run in place).
// Anywhere else - copied next to rghport-cli.exe as "WiiConverted Setup.exe", the packaged setup, or run from the
// build folder - it is the setup (setup.h): the same window, walking through the steps that build a game folder.
//
// Switches: --setup forces the setup.  Development: --step N opens a step; --autostart converts with the stored
// settings once the window is up and quits when the conversion ends (with --screenshot, the log page is saved too,
// as FILE-log, and the Home page after Go Home as FILE-home); --screenshot FILE saves the window as an image
// before quitting; --meter PCT shows the Convert step's meter part way.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <dwmapi.h>
#include <shellapi.h>

#include <QApplication>
#include <QDir>
#include <QFile>
#include <QFontDatabase>
#include <QIcon>
#include <QImage>
#include <QPixmap>
#include <QTimer>

#include "settings.h"
#include "ui.h"

namespace {

std::wstring gameFolder() {
    std::wstring here = rgh::ExeDir();
    if (rgh::IsGameFolder(here)) return here;
    size_t cut = here.find_last_of(L"\\/");
    if (cut != std::wstring::npos) return here.substr(0, cut);
    return here;
}

QIcon windowIcon(const std::wstring& dir) {
    // the stub's icon (the one the setup gave the game folder), else this program's own (launcher.rc): both are the
    // square .ico.  The WC mark's picture is twice as wide as it is high and would be squeezed into the square the
    // title bar and the taskbar show, so it is only the last resort.
    wchar_t self[MAX_PATH] = L"";
    GetModuleFileNameW(nullptr, self, MAX_PATH);
    const std::wstring sources[] = { dir + L"\\WiiConverted Launcher.exe", self };
    for (const std::wstring& exe : sources) {
        HICON large = nullptr;
        if (!exe.empty() && ExtractIconExW(exe.c_str(), 0, &large, nullptr, 1) > 0 && large) {
            QIcon icon(QPixmap::fromImage(QImage::fromHICON(large)));
            DestroyIcon(large);
            return icon;
        }
    }
    return QIcon(":/wcicon.png");
}

}  // namespace

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    app.setOrganizationName("RGHPort");

    rgh::SetupOptions options;
    bool forceSetup = false;
    QString shot;
    QStringList args = app.arguments();
    for (int i = 1; i < args.size(); ++i) {
        if (args[i] == "--setup") forceSetup = true;
        else if (args[i] == "--autostart") options.autostart = true;
        else if (args[i] == "--screenshot" && i + 1 < args.size()) shot = args[++i];
        else if (args[i] == "--step" && i + 1 < args.size()) options.step = args[++i].toInt();
        else if (args[i] == "--meter" && i + 1 < args.size()) options.meter = args[++i].toInt();
    }

    QFile fontFile(":/font.ttf");
    QString family = "Segoe UI";
    if (fontFile.open(QIODevice::ReadOnly)) {
        int id = QFontDatabase::addApplicationFontFromData(fontFile.readAll());
        QStringList families = QFontDatabase::applicationFontFamilies(id);
        if (!families.isEmpty()) family = families.first();
    }
    rgh::SetHeadFamily(family);
    app.setFont(rgh::BodyFont(14));

    std::wstring dir = gameFolder();
    bool setup = forceSetup || !rgh::IsGameFolder(dir);
    app.setApplicationName(setup ? "WiiConverted Setup" : "WiiConverted Launcher");
    if (setup) {
        // a folder with a bigfile but no game to start: an unfinished game folder, offered as the destination
        QString folder = QString::fromStdWString(dir);
        if (!QDir(folder).entryList(QStringList() << "*.bf" << "*.BF", QDir::Files).isEmpty()) options.presetOut = folder;
    }
    app.setWindowIcon(windowIcon(dir));
    rgh::Launcher window(dir, setup ? rgh::Launcher::Mode::Setup : rgh::Launcher::Mode::Game, options);
    window.showMaximized();                                                // always opens filling the screen
    // a dark title bar; on Windows 11 a purple one with white text (the attributes are ignored before that)
    HWND hwnd = (HWND)window.winId();
    BOOL dark = TRUE;
    DwmSetWindowAttribute(hwnd, 20, &dark, sizeof(dark));                  // DWMWA_USE_IMMERSIVE_DARK_MODE
    COLORREF caption = RGB(0x6A, 0x4F, 0xC1), text = RGB(255, 255, 255);
    DwmSetWindowAttribute(hwnd, 35, &caption, sizeof(caption));            // DWMWA_CAPTION_COLOR
    DwmSetWindowAttribute(hwnd, 36, &text, sizeof(text));                  // DWMWA_TEXT_COLOR

    auto grab = [&window, &app, shot] {
        if (!shot.isEmpty()) window.grab().save(shot);
        app.quit();
    };
    if (setup && options.autostart) {
        QObject::connect(window.setup(), &rgh::SetupFlow::finished, &app, [&window, &app, shot](int) {
            QTimer::singleShot(600, &app, [&window, &app, shot] {
                if (shot.isEmpty()) {
                    app.quit();
                    return;
                }
                window.grab().save(shot);
                window.setup()->jump(4);                                   // the log page too
                QTimer::singleShot(400, &app, [&window, &app, shot] {
                    int dot = shot.lastIndexOf('.');
                    QString stem = dot > 0 ? shot.left(dot) : shot, ext = dot > 0 ? shot.mid(dot) : QString(".png");
                    window.grab().save(stem + "-log" + ext);
                    window.setup()->jump(5);
                    if (window.setup()->nextText() != "Go Home") {      // no game to go to
                        app.quit();
                        return;
                    }
                    window.setup()->goNext();                              // Go Home: the launcher of the folder
                    QTimer::singleShot(600, &app, [&window, &app, stem, ext] {
                        window.grab().save(stem + "-home" + ext);
                        app.quit();
                    });
                });
            });
        });
    } else if (!shot.isEmpty()) {
        QTimer::singleShot(1500, &app, grab);
    }
    return app.exec();
}
