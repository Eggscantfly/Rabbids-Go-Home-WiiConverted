// setup.h - the setup: the pages of the window in setup mode (ui.h), six steps: welcome, the Wii game data, the PC
// release, the destination (with the Wii save import and the Options page question), the conversion, done.  The
// conversion is the converter in a child process - rghport-cli.exe next to the app (the packaged setup), else
// "python -m rghport" in the repository the app was built in - whose output is shown as it arrives; the two folder
// checks that need the archive reader and the executable are "rghport inspect" in the background.  A game folder
// that already holds a converted archive is refreshed (rghport assemble) instead of converted again.  Everything else
// is the converter's defaults: the archive RGH_WC.bf, the PC release's animation lists, the cache next to the game
// folder, the script record folders of the script_overrides folder next to the app.  When the game folder is built,
// Go Home turns the window into its launcher.  The folders picked last time are kept per user in RGHPort.ini.
#pragma once

#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QObject>
#include <QProcess>
#include <QSettings>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <QWidget>
#include "sfx.h"

class QFrame;
class QPlainTextEdit;
class QPushButton;

namespace rgh {

class JunkMeter;
class ToggleSwitch;

// where the converter is
struct Cli {
    QString program;        // rghport-cli.exe, or python
    QStringList prefix;     // -u -m rghport for python
    QString workDir;        // the app folder, or the repository
    bool found = false;
};
Cli FindCli(const QString& appDir);

// a caption, a path field with Browse, and the status: green beside the caption when all is well, under the field for
// a warning or a problem, in the empty field as a hint
class PathRow : public QWidget {
    Q_OBJECT
public:
    enum Pick { Folder, File };
    PathRow(const QString& caption, Pick pick = Folder, const QString& filter = QString(), QWidget* parent = nullptr);
    QString path() const;
    void setPath(const QString& p);
    void setStatus(const QString& state, const QString& text);   // "ok", "warn", "bad", "busy" or "" (a hint)
signals:
    void changed();
private:
    void browse();
    QLabel* m_caption;
    QLabel* m_good;
    QLabel* m_status;
    QLineEdit* m_edit;
    Pick m_pick;
    QString m_filter;
};

// one rghport command in a child process: its lines as they arrive, then its exit code (-1: stopped)
class Command : public QObject {
    Q_OBJECT
public:
    Command(const Cli& cli, QObject* parent = nullptr);
    void start(const QStringList& args);
    bool running() const;
    void stop();
signals:
    void line(const QString& text);
    void done(int code);
private:
    void read();
    void finished(int code, QProcess::ExitStatus status);
    void report(int code);
    Cli m_cli;
    QProcess* m_proc;
    QByteArray m_buf;
    bool m_stopped = false;
    bool m_reported = false;
};

// "rghport inspect" in the background; a request during a run waits and runs next (only the last one)
class Inspector : public QObject {
    Q_OBJECT
public:
    Inspector(const Cli& cli, QObject* parent = nullptr);
    void request(const QStringList& args);
    bool running() const;
signals:
    void result(const QJsonObject& report, bool ok);
private:
    void finished(int code, QProcess::ExitStatus status);
    Cli m_cli;
    QProcess* m_proc;
    QStringList m_pending;
    bool m_hasPending = false;
};

struct SetupOptions {
    QString presetOut;      // the destination to start with (a game folder the launcher found incomplete)
    bool autostart = false; // development: convert with the stored settings as soon as the window is up
    int step = -1;          // development: open this step
    int meter = -1;         // development: the meter at this percentage
};

class SetupFlow : public QObject {
    Q_OBJECT
public:
    static const int N_STEPS = 6;
    static const char* const kStepTitle[N_STEPS];       // WELCOME, WII FILES, ... (the sidebar and the top bar)
    SetupFlow(const QString& appDir, const SetupOptions& options, QWidget* host);
    QWidget* page(int i) const { return m_pages[i]; }
    void begin();                        // shows the first step (or the one asked for)
    int step() const { return m_step; }
    bool busy() const;
    bool reached(int i) const;           // the sidebar may open step i
    bool done(int i) const;              // the sidebar shows step i with a check mark
    bool backVisible() const;
    bool stopVisible() const;
    bool nextVisible() const;
    bool nextEnabled() const;
    QString nextText() const;            // Next, Convert, Update, Go Home or Close
    void goBack();
    void goNext();
    void jump(int i);
    void stopRun();
    void start();                        // the conversion, or the refresh of an existing game folder
    bool confirmClose();                 // false when the window must stay (a run the user keeps)
signals:
    void stepChanged(int i);
    void stateChanged();
    void finished(int code);
    void home(const QString& folder);    // Go Home: the window becomes the launcher of the game folder
private:
    QWidget* buildWelcome();
    QWidget* buildWii();
    QWidget* buildPc();
    QWidget* buildDestination();
    QWidget* buildConvert();
    QWidget* buildDone();
    void show(int i);
    void validate();
    void validateWii();
    void validatePc();
    void validateDestination();
    void wiiResult(const QJsonObject& rep, bool ok);
    void pcResult(const QJsonObject& rep, bool ok);
    bool ready(int i) const;
    QStringList defaultLayers() const;
    void load();
    void store();
    QStringList arguments() const;
    void onLine(const QString& text);
    void onDone(int code);
    void showResult(int code, const QString& folder, qint64 took);
    void goHome();
    void saveLog();

    QString m_appDir;
    Cli m_cli;
    SetupOptions m_options;
    QWidget* m_host;
    QSettings m_settings;
    QTimer m_timer;                      // the checks run a moment after the last keystroke
    QWidget* m_pages[N_STEPS] = {};
    int m_step = 0;
    int m_reached = 0;                   // the furthest input step opened so far
    bool m_ran = false;                  // a conversion ran: Convert and Done may be opened again
    bool m_runOver = false;              // ... and it ended and the tower filled: Next opens Done
    bool m_pendingStart = false;         // Convert was pressed while a folder check was still running
    // the Wii game data
    PathRow* m_wii = nullptr;
    Inspector* m_wiiInspector = nullptr;
    QString m_wiiAsked;                  // the folder the running check is about
    QString m_wiiChecked;                // the folder the shown status is about
    QString m_wiiState;
    // the PC release
    PathRow* m_pc = nullptr;
    Inspector* m_pcInspector = nullptr;
    QString m_pcAsked;
    QString m_pcChecked;
    QString m_pcState;
    // the destination
    PathRow* m_out = nullptr;
    PathRow* m_save = nullptr;
    QString m_outState;
    bool m_existing = false;             // the folder holds a converted archive: refreshed, not converted again
    bool m_optionsPage = false;          // the question: an OPTIONS entry and page in the game's pause menu
    ToggleSwitch* m_optionsSwitch = nullptr;
    QFrame* m_optionsRow = nullptr;
    // the conversion
    Command* m_command = nullptr;
    JunkMeter* m_meter = nullptr;        // the game's end-of-level count, filling with the conversion
    Sfx m_sfxFill, m_sfxGift;            // its sounds (Assets/Audio/sfx, Qt resources)
    QLabel* m_stage = nullptr;
    QPlainTextEdit* m_log = nullptr;
    QStringList m_logPending;            // lines not yet appended: the log is appended in batches
    QTimer* m_logFlush = nullptr;
    qint64 m_started = 0;
    bool m_refreshing = false;
    // done
    QLabel* m_result = nullptr;
    QLabel* m_where = nullptr;
    QPushButton* m_home = nullptr;
    QPushButton* m_open = nullptr;
    QString m_folder;
    bool m_canHome = false;              // the folder holds a game the launcher can start
};

}  // namespace rgh
