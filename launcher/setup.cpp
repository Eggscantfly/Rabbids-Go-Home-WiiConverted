// setup.cpp - the setup (see setup.h).
#include "setup.h"

#include <QApplication>
#include <QClipboard>
#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QProcessEnvironment>
#include <QPushButton>
#include <QRegularExpression>
#include <QScrollArea>
#include <QStorageInfo>
#include <QTextCursor>
#include <QUrl>
#include <QVBoxLayout>
#include <memory>

#include "meter.h"
#include "ui.h"

namespace rgh {

const char* const SetupFlow::kStepTitle[SetupFlow::N_STEPS] = { "WELCOME", "WII FILES", "PC RELEASE", "DESTINATION",
                                                                "CONVERT", "DONE" };

namespace {

const char* kTitle = "WiiConverted Setup";
const char* kCliExe = "rghport-cli.exe";
const char* kBigfile = "RGH_WC.bf";                        // the converted archive's name
const char* kLayersDir = "script_overrides";               // next to the app: folders of script records
const char* kLayersOrder = "layers.txt";                   // their order, one folder per line
const qint64 kNeededBytes = 3LL * 1024 * 1024 * 1024;      // a game folder takes about 2.5 GB
const char* kOk = "#66BB6A";
const char* kWarn = "#F0B45A";
const char* kBad = "#FF7B72";
const char* kNote = "#A8A8A8";

QString native(const QString& p) { return QDir::toNativeSeparators(p); }
QString absolute(const QString& p) { return QDir::cleanPath(QFileInfo(p).absoluteFilePath()); }

QString sizeText(qint64 n) {
    const char* units[] = { "bytes", "KB", "MB", "GB" };
    double v = double(n);
    for (int i = 0; i < 4; ++i) {
        if (v < 1024 || i == 3)
            return i == 0 ? QString("%1 %2").arg(qint64(v)).arg(units[i]) : QString("%1 %2").arg(v, 0, 'f', 1).arg(units[i]);
        v /= 1024.0;
    }
    return QString::number(v);
}

QString quoted(const QStringList& args) {
    QStringList out;
    for (const QString& a : args) out << ((a.contains(' ') || a.contains(';')) ? "\"" + a + "\"" : a);
    return out.join(' ');
}

// true when `path` is `folder` or lies inside it
bool isInside(const QString& path, const QString& folder) {
    QString a = absolute(path).toLower(), b = absolute(folder).toLower();
    if (a == b) return true;
    if (!b.endsWith('/')) b += '/';
    return a.startsWith(b);
}

// true when an existing file cannot be opened for writing (the game is running from that folder)
bool lockedFile(const QString& path) {
    QFile f(path);
    if (!f.exists()) return false;
    return !f.open(QIODevice::ReadWrite);
}

// true when a folder holds a finished conversion: the archive and the summary the converter writes beside it at the
// end of a whole conversion (a test run says so in its summary; a stopped run leaves none)
bool completeConversion(const QString& folder) {
    QDir d(folder);
    if (!QFileInfo(d.filePath(kBigfile)).isFile()) return false;
    QFile summary(d.filePath(QString(kBigfile) + ".json"));
    if (!summary.open(QIODevice::ReadOnly)) return false;
    QJsonDocument doc = QJsonDocument::fromJson(summary.readAll());
    if (!doc.isObject() || doc.object().value("test_run").toBool(false)) return false;
    // an archive built without the script records (the game never starts from it) is converted again
    QJsonObject o = doc.object();
    if (o.value("builder_sources").toObject().value("override").toInt(0) <= 0) return false;
    if (o.value("builder_missing").toObject().value("script:key").toInt(0) > 0) return false;
    return true;
}

QString rstrip(QByteArray b) {
    while (!b.isEmpty() && (b.endsWith(' ') || b.endsWith('\t') || b.endsWith('\r'))) b.chop(1);
    return QString::fromUtf8(b);
}

QString exitText(int code) {
    switch (code) {
    case 0: return "Done: the game is ready.";
    case 1: return "The conversion stopped on an error (see the log).";
    case 2: return "An option was refused (see the log).";
    case 3: return "Converted, but the game folder is incomplete: every problem is listed in the log.";
    case 4: return "Converted, but the save import failed (see the log).";
    default: return QString("The conversion ended with exit code %1 (see the log).").arg(code);
    }
}

QProcessEnvironment cliEnvironment() {
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert("PYTHONUNBUFFERED", "1");
    env.insert("PYTHONIOENCODING", "utf-8");
    return env;
}

// a card holding a step's fields
QFrame* box(QWidget* parent, QVBoxLayout** body) {
    auto* f = new QFrame(parent);
    f->setObjectName("panel");
    auto* lay = new QVBoxLayout(f);
    lay->setContentsMargins(20, 18, 20, 18);
    lay->setSpacing(16);
    *body = lay;
    return f;
}

QLabel* heading(const QString& text, QWidget* parent) {
    auto* l = new QLabel(text, parent);
    l->setFont(BodyFont(20, 600));
    return l;
}

QLabel* paragraph(const QString& text, QWidget* parent, int px = 14) {
    auto* l = new QLabel(text, parent);
    l->setFont(BodyFont(px));
    l->setWordWrap(true);
    return l;
}

QLabel* bullet(const QString& text, QWidget* parent) {
    return paragraph(QString::fromUtf8("\xE2\x80\xA2  ") + text, parent);
}

QPushButton* button(const QString& text, QWidget* parent, const char* name = nullptr) {
    auto* b = new QPushButton(text, parent);
    if (name) b->setObjectName(name);
    b->setFont(BodyFont(14));
    b->setCursor(Qt::PointingHandCursor);
    return b;
}

// the page in a scroll area: a small screen scrolls instead of squeezing it
QScrollArea* scrolled(QWidget* page) {
    auto* area = new QScrollArea();
    area->setWidgetResizable(true);
    area->setFrameShape(QFrame::NoFrame);
    area->setWidget(page);
    return area;
}

}  // namespace

// ---------------------------------------------------------------------------------------------------------- Cli
Cli FindCli(const QString& appDir) {
    Cli c;
    // the packaged converter next to the app, or one folder up
    QDir d(appDir);
    for (int i = 0; i < 2; ++i) {
        QString exe = d.filePath(kCliExe);
        if (QFileInfo::exists(exe)) {
            c.program = native(exe);
            c.workDir = native(d.absolutePath());
            c.found = true;
            return c;
        }
        if (!d.cdUp()) break;
    }
    // the repository the app was built in: python -m rghport from its root
    d = QDir(appDir);
    for (int i = 0; i < 6; ++i) {
        if (QFileInfo::exists(d.filePath("rghport/cli.py"))) {
            c.program = "python";
            c.prefix = QStringList() << "-u" << "-m" << "rghport";
            c.workDir = native(d.absolutePath());
            c.found = true;
            return c;
        }
        if (!d.cdUp()) break;
    }
    c.workDir = native(appDir);
    return c;
}

// ------------------------------------------------------------------------------------------------------ PathRow
PathRow::PathRow(const QString& caption, Pick pick, const QString& filter, QWidget* parent)
    : QWidget(parent), m_pick(pick), m_filter(filter) {
    m_caption = new QLabel(caption, this);
    m_caption->setFont(BodyFont(14, 600));
    m_good = new QLabel(this);
    m_good->setFont(BodyFont(12));
    m_good->setWordWrap(true);
    m_good->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    m_edit = new QLineEdit(this);
    m_edit->setFont(BodyFont(14));
    m_edit->setClearButtonEnabled(true);
    auto* browse = button("Browse...", this);
    m_status = new QLabel(this);
    m_status->setFont(BodyFont(12));
    m_status->setWordWrap(true);
    m_status->hide();
    auto* head = new QHBoxLayout();
    head->setContentsMargins(0, 0, 0, 0);
    head->setSpacing(12);
    head->addWidget(m_caption);
    head->addWidget(m_good, 1);
    auto* line = new QHBoxLayout();
    line->setContentsMargins(0, 0, 0, 0);
    line->setSpacing(8);
    line->addWidget(m_edit, 1);
    line->addWidget(browse);
    auto* lay = new QVBoxLayout(this);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->setSpacing(5);
    lay->addLayout(head);
    lay->addLayout(line);
    lay->addWidget(m_status);
    connect(browse, &QPushButton::clicked, this, &PathRow::browse);
    connect(m_edit, &QLineEdit::textChanged, this, [this] { emit changed(); });
}

QString PathRow::path() const {
    QString s = m_edit->text().trimmed();
    if (s.size() >= 2 && s.startsWith('"') && s.endsWith('"')) s = s.mid(1, s.size() - 2).trimmed();
    return s;
}

void PathRow::setPath(const QString& p) { m_edit->setText(p); }

void PathRow::setStatus(const QString& state, const QString& text) {
    const char* color = state == "ok" ? kOk : state == "warn" ? kWarn : state == "bad" ? kBad : kNote;
    QString mark = state == "ok" ? QString::fromUtf8("\xE2\x9C\x93 ") : state == "warn" ? QString("! ")
                 : state == "bad" ? QString::fromUtf8("\xE2\x9C\x97 ") : QString();
    bool below = state == "warn" || state == "bad" || state == "busy";
    m_good->setText(state == "ok" ? mark + text : QString());
    m_good->setStyleSheet(QString("color: %1;").arg(color));
    m_edit->setPlaceholderText(state.isEmpty() ? text : QString());
    m_edit->setToolTip(state.isEmpty() ? text : QString());
    m_status->setText(below ? mark + text : QString());
    m_status->setStyleSheet(QString("color: %1;").arg(color));
    m_status->setVisible(below);
}

void PathRow::browse() {
    QString start = path().isEmpty() ? QDir::homePath() : path();
    QString p = m_pick == File ? QFileDialog::getOpenFileName(this, m_caption->text(), start, m_filter)
                               : QFileDialog::getExistingDirectory(this, m_caption->text(), start);
    if (!p.isEmpty()) setPath(native(p));
}

// ------------------------------------------------------------------------------------------------------ Command
Command::Command(const Cli& cli, QObject* parent) : QObject(parent), m_cli(cli), m_proc(new QProcess(this)) {
    m_proc->setProcessChannelMode(QProcess::MergedChannels);
    m_proc->setProcessEnvironment(cliEnvironment());
    m_proc->setWorkingDirectory(cli.workDir);
    connect(m_proc, &QProcess::readyReadStandardOutput, this, &Command::read);
    connect(m_proc, &QProcess::finished, this, &Command::finished);
    connect(m_proc, &QProcess::errorOccurred, this, [this](QProcess::ProcessError err) {
        if (err == QProcess::FailedToStart) {
            emit line("cannot start " + m_cli.program);
            report(1);
        }
    });
}

void Command::start(const QStringList& args) {
    m_stopped = m_reported = false;
    m_buf.clear();
    m_proc->start(m_cli.program, m_cli.prefix + args);
}

bool Command::running() const { return m_proc->state() != QProcess::NotRunning; }

void Command::stop() {
    m_stopped = true;
    m_proc->kill();
}

void Command::read() {
    m_buf += m_proc->readAllStandardOutput();
    m_buf.replace("\r\n", "\n");
    int nl;
    while ((nl = m_buf.indexOf('\n')) >= 0) {
        emit line(rstrip(m_buf.left(nl)));
        m_buf.remove(0, nl + 1);
    }
}

void Command::finished(int code, QProcess::ExitStatus) {
    read();
    if (!m_buf.isEmpty()) {
        emit line(rstrip(m_buf));
        m_buf.clear();
    }
    report(m_stopped ? -1 : code);
}

void Command::report(int code) {
    if (m_reported) return;
    m_reported = true;
    emit done(code);
}

// ---------------------------------------------------------------------------------------------------- Inspector
Inspector::Inspector(const Cli& cli, QObject* parent) : QObject(parent), m_cli(cli), m_proc(new QProcess(this)) {
    m_proc->setProcessEnvironment(cliEnvironment());
    m_proc->setWorkingDirectory(cli.workDir);
    connect(m_proc, &QProcess::finished, this, &Inspector::finished);
    connect(m_proc, &QProcess::errorOccurred, this, [this](QProcess::ProcessError err) {
        if (err == QProcess::FailedToStart) finished(-1, QProcess::CrashExit);
    });
}

void Inspector::request(const QStringList& args) {
    if (running()) {                     // the answer on its way is stale: this question goes next
        m_pending = args;
        m_hasPending = true;
        return;
    }
    m_proc->start(m_cli.program, m_cli.prefix + (QStringList() << "inspect") + args);
}

bool Inspector::running() const { return m_proc->state() != QProcess::NotRunning; }

void Inspector::finished(int code, QProcess::ExitStatus status) {
    QByteArray out = m_proc->readAllStandardOutput();
    m_proc->readAllStandardError();
    if (m_hasPending) {
        m_hasPending = false;
        QStringList next = m_pending;
        m_pending.clear();
        request(next);
        return;
    }
    QJsonDocument doc = QJsonDocument::fromJson(out);
    bool ok = code == 0 && status == QProcess::NormalExit && doc.isObject();
    emit result(ok ? doc.object() : QJsonObject(), ok);
}

// ---------------------------------------------------------------------------------------------------- SetupFlow
SetupFlow::SetupFlow(const QString& appDir, const SetupOptions& options, QWidget* host)
    : QObject(host), m_appDir(appDir), m_cli(FindCli(appDir)), m_options(options), m_host(host),
      m_settings(QSettings::IniFormat, QSettings::UserScope, "RGHPort", "RGHPort") {
    m_timer.setSingleShot(true);
    m_timer.setInterval(350);
    connect(&m_timer, &QTimer::timeout, this, &SetupFlow::validate);
    m_wiiInspector = new Inspector(m_cli, this);
    connect(m_wiiInspector, &Inspector::result, this, &SetupFlow::wiiResult);
    m_pcInspector = new Inspector(m_cli, this);
    connect(m_pcInspector, &Inspector::result, this, &SetupFlow::pcResult);
    m_command = new Command(m_cli, this);
    connect(m_command, &Command::line, this, &SetupFlow::onLine);
    connect(m_command, &Command::done, this, &SetupFlow::onDone);
    m_pages[0] = scrolled(buildWelcome());
    m_pages[1] = scrolled(buildWii());
    m_pages[2] = scrolled(buildPc());
    m_pages[3] = scrolled(buildDestination());
    m_pages[4] = scrolled(buildConvert());
    m_pages[5] = scrolled(buildDone());
    load();
}

void SetupFlow::begin() {
    int i = m_options.step;
    if (i < 0 || i >= N_STEPS) i = 0;
    if (i >= 4) m_ran = true;
    m_reached = qMin(i, 3);
    if (m_options.meter >= 0) {                     // development: the meter part way, as during a conversion
        m_meter->setBusy(true);
        m_meter->setProgress(m_options.meter * 43, 4300, true);
        m_stage->setText(QString("Converting: %1 of 4300 archive entries").arg(m_options.meter * 43));
    }
    show(i);
    if (m_options.autostart) QTimer::singleShot(900, this, &SetupFlow::start);
}

// ---- the pages
QWidget* SetupFlow::buildWelcome() {
    auto* page = new QWidget();
    auto* lay = new QVBoxLayout(page);
    lay->setContentsMargins(28, 24, 28, 24);
    lay->setSpacing(18);
    lay->addWidget(new Banner(page));
    lay->addWidget(paragraph("This setup builds a Windows version of the Wii game from files you own and gives it the "
                             "launcher. Nothing is downloaded or uploaded; your original files are only read.", page, 15));
    auto* row = new QGridLayout();
    row->setSpacing(16);
    for (int c = 0; c < 2; ++c) row->setColumnStretch(c, 1);
    QVBoxLayout* body = nullptr;
    QFrame* need = HomePanel("You need", page, &body);
    body->addWidget(bullet("The Wii disc's game data: the folder with RGH.BF, RGH.wii.sns.BF and RGH.$hd$.bik.BF, "
                           "extracted from your own disc.", need));
    body->addWidget(bullet("The PC release of Rabbids Go Home, installed or copied: its executable, shaders folder, "
                           "binkw32.dll and archive.", need));
    body->addWidget(bullet("About 2.5 GB of free space for the game folder.", need));
    body->addStretch(1);
    row->addWidget(need, 0, 0);
    QFrame* what = HomePanel("What happens", page, &body);
    body->addWidget(paragraph("The Wii archive is converted into a new game folder (a few minutes) next to a copy of the "
                              "PC release's executable, and the folder gets the launcher: this window becomes it, with "
                              "Play on its Home page, the mods and the settings.", what));
    body->addWidget(paragraph("An earlier game folder can be picked again: its own files are refreshed and its archive "
                              "kept.", what));
    body->addWidget(Muted("Press Next to begin; the steps are listed at the left.", what));
    body->addStretch(1);
    row->addWidget(what, 0, 1);
    lay->addLayout(row);
    lay->addStretch(1);
    return page;
}

QWidget* SetupFlow::buildWii() {
    auto* page = new QWidget();
    auto* lay = new QVBoxLayout(page);
    lay->setContentsMargins(28, 24, 28, 24);
    lay->setSpacing(14);
    lay->addWidget(heading("Step 1: the Wii game data", page));
    lay->addWidget(Muted("Pick the folder with your Wii disc's files: RGH.BF and the sound and video archives beside it.",
                         page, 14));
    QVBoxLayout* body = nullptr;
    QFrame* b = box(page, &body);
    m_wii = new PathRow("Wii game data folder", PathRow::Folder, QString(), b);
    connect(m_wii, &PathRow::changed, this, [this] { m_timer.start(); });
    body->addWidget(m_wii);
    lay->addWidget(b);
    lay->addStretch(1);
    return page;
}

QWidget* SetupFlow::buildPc() {
    auto* page = new QWidget();
    auto* lay = new QVBoxLayout(page);
    lay->setContentsMargins(28, 24, 28, 24);
    lay->setSpacing(14);
    lay->addWidget(heading("Step 2: the PC release", page));
    lay->addWidget(Muted("Pick the folder of the PC release of Rabbids Go Home, installed or copied.", page, 14));
    QVBoxLayout* body = nullptr;
    QFrame* b = box(page, &body);
    m_pc = new PathRow("PC release folder", PathRow::Folder, QString(), b);
    connect(m_pc, &PathRow::changed, this, [this] { m_timer.start(); });
    body->addWidget(m_pc);
    lay->addWidget(b);
    lay->addStretch(1);
    return page;
}

QWidget* SetupFlow::buildDestination() {
    auto* page = new QWidget();
    auto* lay = new QVBoxLayout(page);
    lay->setContentsMargins(28, 24, 28, 24);
    lay->setSpacing(14);
    lay->addWidget(heading("Step 3: where the game goes", page));
    lay->addWidget(Muted("Pick an empty folder, or an earlier game folder to refresh it. Your Wii save can come along.",
                         page, 14));
    QVBoxLayout* body = nullptr;
    QFrame* b = box(page, &body);
    m_out = new PathRow("Game folder", PathRow::Folder, QString(), b);
    m_save = new PathRow("Wii save folder to import (optional)", PathRow::Folder, QString(), b);
    body->addWidget(m_out);
    body->addWidget(m_save);
    for (PathRow* r : { m_out, m_save }) connect(r, &PathRow::changed, this, [this] { m_timer.start(); });
    // the question: the Options page in the game's pause menu
    m_optionsSwitch = new ToggleSwitch(&m_optionsPage);
    m_optionsRow = AddRow(body, "Options page in the pause menu",
                          "Adds OPTIONS to the game's pause menu, a page with the control bindings, the volumes and the "
                          "graphics settings. Off: the pause menu stays as the Wii had it.", m_optionsSwitch);
    lay->addWidget(b);
    lay->addStretch(1);
    return page;
}

QWidget* SetupFlow::buildConvert() {
    auto* page = new QWidget();
    auto* lay = new QVBoxLayout(page);
    lay->setContentsMargins(28, 24, 28, 24);
    lay->setSpacing(14);
    lay->addWidget(heading("Step 4: converting", page));
    lay->addWidget(Muted("This takes a few minutes. The first conversion of a disc also indexes its archives, which takes "
                         "a few minutes more.", page, 14));
    auto* row = new QHBoxLayout();
    row->setSpacing(14);
    QVBoxLayout* body = nullptr;
    QFrame* bottom = box(page, &body);
    body->setSpacing(10);
    m_log = new QPlainTextEdit(bottom);
    m_log->setReadOnly(true);
    m_log->setMaximumBlockCount(20000);
    m_log->setLineWrapMode(QPlainTextEdit::NoWrap);
    QFont mono("Consolas");
    mono.setStyleHint(QFont::Monospace);
    mono.setPixelSize(12);
    m_log->setFont(mono);
    m_log->setMinimumHeight(240);
    body->addWidget(m_log, 1);
    auto* tools = new QHBoxLayout();
    tools->addStretch(1);
    auto* copy = button("Copy log", bottom);
    connect(copy, &QPushButton::clicked, this, [this] { QApplication::clipboard()->setText(m_log->toPlainText()); });
    auto* save = button("Save log...", bottom);
    connect(save, &QPushButton::clicked, this, &SetupFlow::saveLog);
    tools->addWidget(copy);
    tools->addWidget(save);
    body->addLayout(tools);
    row->addWidget(bottom, 1);
    // the game's end-of-level count as the meter, with the stage under it
    QFrame* side = box(page, &body);
    side->setFixedWidth(440);
    body->setSpacing(8);
    m_meter = new JunkMeter(side);
    // the scene's sounds, as the game's end-of-level script plays them: the transfer loop while the fill rises, its
    // pitch climbing as its sound set's curve prescribes (0.45 times at the start; the game's transfer only climbs
    // the first part of the curve, to about 0.75), and a ring for every reward.  The loop stays on across the
    // fill's short rests between the converter's progress lines.
    m_sfxFill.load(":/sfx/fill.wav");
    m_sfxGift.load(":/sfx/gift.wav");
    connect(m_meter, &JunkMeter::fillStarted, this, [this] {
        if (!m_sfxFill.looping()) m_sfxFill.startLoop(0.3f, 0.45f);
    });
    connect(m_meter, &JunkMeter::fillLevel, this, [this](float part) {
        float x = 0.35f * part;                                     // the part of the curve the transfer reaches
        float ease = x * x * (3.f - 2.f * x);
        m_sfxFill.setRate(0.45f + (1.5f - 0.45f) * ease);
    });
    connect(m_meter, &JunkMeter::fillStopped, this, [this] {
        // the loop plays on through the rests between progress lines; once the run is over it dies away slowly
        // under the last ring
        if (!busy()) m_sfxFill.stopLoop(1500);
    });
    connect(m_meter, &JunkMeter::giftLit, this, [this] { m_sfxGift.play(0.45f); });
    body->addWidget(m_meter, 1);
    m_stage = Muted("", side, 13);
    m_stage->setAlignment(Qt::AlignHCenter);
    body->addWidget(m_stage);
    row->addWidget(side);
    lay->addLayout(row, 1);
    return page;
}

QWidget* SetupFlow::buildDone() {
    auto* page = new QWidget();
    auto* lay = new QVBoxLayout(page);
    lay->setContentsMargins(28, 24, 28, 24);
    lay->setSpacing(14);
    lay->addWidget(heading("Done", page));
    QVBoxLayout* body = nullptr;
    QFrame* b = box(page, &body);
    m_result = new QLabel(b);
    m_result->setFont(BodyFont(16, 600));
    m_result->setWordWrap(true);
    m_where = paragraph("", b, 14);
    m_where->setTextFormat(Qt::RichText);
    m_where->setTextInteractionFlags(Qt::TextSelectableByMouse);
    body->addWidget(m_result);
    body->addWidget(m_where);
    auto* row = new QHBoxLayout();
    row->setSpacing(12);
    m_home = new QPushButton("Go Home", b);
    m_home->setObjectName("play");
    m_home->setIcon(TrailingIcon(":/moonicon.png", 22, 10));               // the moon, as the Home entry
    m_home->setIconSize(QSize(32, 22));
    m_home->setLayoutDirection(Qt::RightToLeft);                           // the icon after the text
    m_home->setFont(BodyFont(15, 600));
    m_home->setMinimumWidth(170);
    m_home->setCursor(Qt::PointingHandCursor);
    connect(m_home, &QPushButton::clicked, this, &SetupFlow::goHome);
    m_open = button("Open the game folder", b, "action");
    m_open->setMinimumHeight(36);
    connect(m_open, &QPushButton::clicked, this, [this] {
        QDesktopServices::openUrl(QUrl::fromLocalFile(m_folder));
    });
    row->addWidget(m_home);
    row->addWidget(m_open);
    row->addStretch(1);
    body->addLayout(row);
    lay->addWidget(b);
    lay->addStretch(1);
    return page;
}

// ---- the steps
void SetupFlow::show(int i) {
    m_step = i;
    if (i <= 3 && i > m_reached) m_reached = i;
    emit stepChanged(i);
    validate();
}

bool SetupFlow::busy() const { return m_command->running(); }

bool SetupFlow::reached(int i) const {
    if (busy()) return i == m_step;
    if (i <= m_reached) return true;
    return i >= 4 && m_ran;
}

bool SetupFlow::done(int i) const {
    if (i >= 1 && i <= 3) return i < m_step && ready(i);
    if (i == 4) return m_runOver && m_step > 4;
    return false;
}

bool SetupFlow::backVisible() const { return m_step == 1 || m_step == 2 || m_step == 3 || m_step == 5; }
bool SetupFlow::stopVisible() const { return m_step == 4 && !m_runOver; }
bool SetupFlow::nextVisible() const { return m_step != 4 || m_runOver; }
bool SetupFlow::nextEnabled() const { return !busy() && !m_pendingStart && ready(m_step); }

QString SetupFlow::nextText() const {
    if (m_step == 3) return m_existing ? "Update" : "Convert";
    if (m_step == 5) return m_canHome ? "Go Home" : "Close";
    return "Next";
}

void SetupFlow::goBack() {
    if (m_step == 5) show(3);
    else if (m_step > 0) show(m_step - 1);
}

void SetupFlow::goNext() {
    if (m_step == 5) {
        if (m_canHome) goHome();
        else m_host->close();
    } else if (m_step == 3) {
        start();
    } else if (m_step == 4) {
        if (m_runOver) show(5);                                      // the user's own click, never automatic
    } else if (m_step < 4) {
        show(m_step + 1);
    }
}

void SetupFlow::jump(int i) {
    if (i >= 0 && i < N_STEPS && reached(i)) show(i);
}

bool SetupFlow::ready(int i) const {
    switch (i) {
    case 1: return m_wiiState == "ok" || m_wiiState == "warn";
    case 2: return m_pcState == "ok" || m_pcState == "warn";
    case 3: return (m_outState == "ok" || m_outState == "warn") &&
                   (m_save->path().isEmpty() || QFileInfo::exists(QDir(m_save->path()).filePath("index.dat")));
    case 4: return m_runOver;
    default: return true;
    }
}

// ---- the checks
void SetupFlow::validate() {
    if (m_step == 1) validateWii();
    else if (m_step == 2) validatePc();
    else if (m_step == 3) validateDestination();
    emit stateChanged();
}

void SetupFlow::validateWii() {
    QString p = m_wii->path();
    if (p.isEmpty()) {
        m_wiiState.clear();
        m_wiiChecked.clear();
        m_wii->setStatus("", "The folder with RGH.BF, RGH.wii.sns.BF and RGH.$hd$.bik.BF from your Wii disc.");
        return;
    }
    if (p == m_wiiChecked && m_wiiState != "busy") return;      // the status shown is this folder's
    if (!QFileInfo(p).isDir()) {
        m_wiiState = "bad";
        m_wiiChecked = p;
        m_wii->setStatus("bad", "This is not a folder.");
        return;
    }
    if (!m_cli.found) {
        m_wiiState = "bad";
        m_wiiChecked = p;
        m_wii->setStatus("bad", "The converter (rghport-cli.exe) is not next to this program: the folder cannot be checked.");
        return;
    }
    m_wiiState = "busy";
    m_wiiAsked = p;
    m_wii->setStatus("busy", "Looking at the archives...");
    m_wiiInspector->request(QStringList() << "--wii" << p);
}

void SetupFlow::wiiResult(const QJsonObject& rep, bool ok) {
    if (m_wii->path() != m_wiiAsked) return;         // an older question; the current folder's answer is on its way
    m_wiiChecked = m_wiiAsked;
    if (!ok) {
        m_wiiState = "bad";
        m_wii->setStatus("bad", "The folder could not be checked: the converter did not answer.");
    } else {
        QJsonObject w = rep.value("wii").toObject();
        m_wiiState = w.value("state").toString();
        m_wii->setStatus(m_wiiState, w.value("text").toString());
    }
    emit stateChanged();
    if (m_pendingStart && m_wiiState != "busy" && m_pcState != "busy") start();
}

void SetupFlow::validatePc() {
    QString p = m_pc->path();
    if (p.isEmpty()) {
        m_pcState.clear();
        m_pcChecked.clear();
        m_pc->setStatus("", "The folder of the PC release: its executable, shaders folder, binkw32.dll and archive.");
        return;
    }
    if (p == m_pcChecked && m_pcState != "busy") return;
    if (!QFileInfo(p).isDir()) {
        m_pcState = "bad";
        m_pcChecked = p;
        m_pc->setStatus("bad", "This is not a folder.");
        return;
    }
    if (!m_cli.found) {
        m_pcState = "bad";
        m_pcChecked = p;
        m_pc->setStatus("bad", "The converter (rghport-cli.exe) is not next to this program: the folder cannot be checked.");
        return;
    }
    m_pcState = "busy";
    m_pcAsked = p;
    m_pc->setStatus("busy", "Looking at the executable and the archive...");
    m_pcInspector->request(QStringList() << "--pc" << p);
}

void SetupFlow::pcResult(const QJsonObject& rep, bool ok) {
    if (m_pc->path() != m_pcAsked) return;
    m_pcChecked = m_pcAsked;
    if (!ok) {
        m_pcState = "bad";
        m_pc->setStatus("bad", "The folder could not be checked: the converter did not answer.");
    } else {
        QJsonObject w = rep.value("pc").toObject();
        m_pcState = w.value("state").toString();
        m_pc->setStatus(m_pcState, w.value("text").toString());
    }
    emit stateChanged();
    if (m_pendingStart && m_wiiState != "busy" && m_pcState != "busy") start();
}

void SetupFlow::validateDestination() {
    QString p = m_out->path();
    m_existing = false;
    if (p.isEmpty()) {
        m_outState.clear();
        m_out->setStatus("", "Where the playable game is written (about 2.5 GB). An earlier game folder is refreshed.");
    } else {
        QString abs = absolute(p);
        bool inside = false;
        for (const QString& o : { m_wii->path(), m_pc->path() }) {
            if (!o.isEmpty() && QFileInfo(o).isDir() && isInside(abs, o)) inside = true;
        }
        if (inside) {
            m_outState = "bad";
            m_out->setStatus("bad", "Pick a folder outside the game folders: nothing is ever written into them.");
        } else {
            QString probe = abs;                     // the nearest folder that exists: the drive's free space
            while (!probe.isEmpty() && !QFileInfo(probe).isDir()) {
                QString parent = QFileInfo(probe).path();
                if (parent == probe) break;
                probe = parent;
            }
            QStorageInfo drive(probe);
            if (!drive.isValid() || !drive.isReady()) {
                m_outState = "warn";
                m_out->setStatus("warn", "Cannot tell the free space of this drive.");
            } else {
                qint64 free = drive.bytesAvailable();
                m_existing = completeConversion(abs);
                if (m_existing) {
                    m_outState = "ok";
                    m_out->setStatus("ok", QString("A game folder already: its files are refreshed, the archive kept "
                                                   "(delete %1 there to convert again). %2 free.")
                                               .arg(kBigfile, sizeText(free)));
                } else if (free < kNeededBytes) {
                    m_outState = "warn";
                    m_out->setStatus("warn", QString("Only %1 free on this drive; a game folder takes about 2.5 GB.")
                                                 .arg(sizeText(free)));
                } else {
                    m_outState = "ok";
                    m_out->setStatus("ok", QString("%1 free on this drive.").arg(sizeText(free)));
                }
            }
        }
    }
    m_optionsRow->setVisible(!m_existing);           // a refresh keeps the archive, menus included
    QString s = m_save->path();
    if (s.isEmpty()) m_save->setStatus("", "The game's save data from a Wii or an emulator (index.dat, slt_*.sav). Only read.");
    else if (QFileInfo::exists(QDir(s).filePath("index.dat"))) m_save->setStatus("ok", "Found index.dat.");
    else m_save->setStatus("bad", "No index.dat in this folder.");
}

// ---- the script record folders
QStringList SetupFlow::defaultLayers() const {
    // the script_overrides folder next to the app, or next to one of the folders above it (the packaged app inside
    // the repository's dist folder, the development build under platform/build), in the order of its layers.txt
    QDir d(m_appDir);
    for (int i = 0; i < 7; ++i) {
        QString base = d.filePath(kLayersDir);
        if (QFileInfo(base).isDir()) {
            QStringList names;
            QFile order(QDir(base).filePath(kLayersOrder));
            if (order.open(QIODevice::ReadOnly | QIODevice::Text)) {
                for (const QByteArray& raw : order.readAll().split('\n')) {
                    QString ln = QString::fromUtf8(raw).trimmed();
                    if (!ln.isEmpty() && !ln.startsWith('#')) names << ln;
                }
            } else {
                names << "wii_records" << "overrides";
            }
            QStringList out;
            for (const QString& n : names) {
                QString p = QDir::isAbsolutePath(n) ? n : QDir(base).filePath(n);
                if (QFileInfo(p).isDir()) out << native(QDir::cleanPath(p));
            }
            return out;
        }
        if (!d.cdUp()) break;
    }
    return QStringList();
}

// ---- the settings kept between runs
void SetupFlow::load() {
    QSettings& s = m_settings;
    m_wii->setPath(s.value("convert/wii").toString());
    m_pc->setPath(s.value("convert/pc").toString());
    m_out->setPath(s.value("convert/out").toString());
    m_save->setPath(s.value("convert/save").toString());
    m_optionsPage = s.value("convert/options_page", "0").toString() == "1";
    m_optionsSwitch->sync();
    if (!m_options.presetOut.isEmpty()) m_out->setPath(native(m_options.presetOut));
}

void SetupFlow::store() {
    QSettings& s = m_settings;
    s.setValue("convert/wii", m_wii->path());
    s.setValue("convert/pc", m_pc->path());
    s.setValue("convert/out", m_out->path());
    s.setValue("convert/save", m_save->path());
    s.setValue("convert/options_page", m_optionsPage ? "1" : "0");
    s.sync();
}

// ---- the run
QStringList SetupFlow::arguments() const {
    QStringList args;
    if (m_existing) {
        // an existing game folder: its own files again; the language it has stays
        args << "assemble" << "--pc" << m_pc->path() << "--out" << m_out->path();
        return args;
    }
    args << "convert" << "--wii" << m_wii->path() << "--pc" << m_pc->path() << "--out" << m_out->path()
         << "--name" << kBigfile << "--twin-kinds" << "spec:trl";
    if (!m_optionsPage) args << "--no-options-page";
    if (!m_save->path().isEmpty()) args << "--import-save" << m_save->path();
    QStringList layers = defaultLayers();
    if (!layers.isEmpty()) args << "--script-overrides" << layers.join(';');
    QString extra = qEnvironmentVariable("RGHPORT_EXTRA_ARGS").trimmed();   // development: a test run
    if (!extra.isEmpty()) args += QProcess::splitCommand(extra);
    return args;
}

void SetupFlow::start() {
    if (busy()) return;
    // the Wii and PC checks answer in the background: a folder never checked is checked now, and Convert waits
    if (m_wiiState.isEmpty() && !m_wii->path().isEmpty()) validateWii();
    if (m_pcState.isEmpty() && !m_pc->path().isEmpty()) validatePc();
    if (m_wiiState == "busy" || m_pcState == "busy") {
        m_pendingStart = true;
        emit stateChanged();
        return;
    }
    m_pendingStart = false;
    validateDestination();
    for (int i = 1; i <= 3; ++i) {
        if (!ready(i)) {
            show(i);
            return;
        }
    }
    m_refreshing = m_existing;
    QString target = QDir(m_out->path()).filePath(kBigfile);
    if (!m_refreshing && lockedFile(target)) {
        QMessageBox::warning(m_host, kTitle, native(target) + " is in use: close the game that runs from this folder, then "
                                             "try again.");
        emit stateChanged();
        return;
    }
    if (!m_refreshing && defaultLayers().isEmpty()) {
        // without the script records the archive builds but the game never starts: never write such a folder
        QMessageBox::critical(m_host, kTitle, "The setup's script_overrides folder is missing next to the app, so the "
                                             "game it would write could not start. Put the setup back together as "
                                             "it was shipped, then try again.");
        emit stateChanged();
        return;
    }
    store();
    QStringList args = arguments();
    m_log->clear();
    m_logPending.clear();
    m_meter->reset();
    m_meter->setBusy(true);
    m_stage->setText(m_refreshing ? "Refreshing the game's own files..."
                                  : "Reading the archives (the first conversion indexes them: a few minutes)...");
    m_log->appendPlainText("rghport " + quoted(args));
    m_started = QDateTime::currentSecsSinceEpoch();
    m_ran = true;
    m_runOver = false;
    m_command->start(args);
    show(4);
}

void SetupFlow::stopRun() {
    if (!busy()) return;
    if (QMessageBox::question(m_host, kTitle, "Stop the conversion? The game folder stays unfinished until you run the "
                                              "setup again.") == QMessageBox::Yes) {
        m_command->stop();
    }
}

void SetupFlow::onLine(const QString& text) {
    // appended in batches: appending and scrolling the log for every line of a talkative run stalls the window
    m_logPending.append(text);
    if (!m_logFlush) {
        m_logFlush = new QTimer(this);
        m_logFlush->setSingleShot(true);
        m_logFlush->setInterval(120);
        connect(m_logFlush, &QTimer::timeout, this, [this] {
            if (m_logPending.isEmpty()) return;
            m_log->appendPlainText(m_logPending.join(QChar(10)));
            m_logPending.clear();
            m_log->moveCursor(QTextCursor::End);
        });
    }
    if (!m_logFlush->isActive()) m_logFlush->start();
    static const QRegularExpression progress("\\[\\s*(\\d+)/(\\d+)\\]\\s+(\\d+) s");
    QRegularExpressionMatch m = progress.match(text);
    if (m.hasMatch()) {
        int n = m.captured(1).toInt(), total = m.captured(2).toInt();
        m_meter->setProgress(n, total);
        m_stage->setText(QString("Converting: %1 of %2 archive entries, %3 s").arg(n).arg(total).arg(m.captured(3)));
    } else if (text.contains(".sns.") || text.contains(".bik.")) {
        m_stage->setText("Copying the sound and video archives...");
    } else if (text.contains("port folder")) {
        m_stage->setText("Finishing the game folder...");
    }
}

void SetupFlow::onDone(int code) {
    qint64 took = QDateTime::currentSecsSinceEpoch() - m_started;
    m_meter->setDone(code == 0);
    m_stage->clear();
    showResult(code, m_out->path(), took);
    // the tower fills up (three seconds at most) before the Done page takes over
    auto fired = std::make_shared<bool>(false);                 // once, whichever comes first
    auto go = [this, code, fired] {
        if (*fired) return;
        *fired = true;
        m_runOver = true;                                             // Next is offered; the page stays
        emit stateChanged();
        emit finished(code);
    };
    if (code == 0 && m_meter->rising()) {
        connect(m_meter, &JunkMeter::fillStopped, this, [go] { QTimer::singleShot(700, go); }, Qt::SingleShotConnection);
        QTimer::singleShot(5000, this, go);                         // never later than this
    } else {
        QTimer::singleShot(300, this, go);
    }
}

void SetupFlow::showResult(int code, const QString& folder, qint64 took) {
    m_folder = folder;
    QString text;
    const char* color;
    if (code == -1) {
        text = "Stopped. Run the setup again to finish the game folder.";
        color = kWarn;
    } else {
        text = exitText(code);
        if (code == 0 && m_refreshing) text = "Done: the game's files are refreshed.";
        color = code == 0 ? kOk : (code == 3 || code == 4) ? kWarn : kBad;
    }
    m_result->setText(QString("%1  (%2 min %3 s)").arg(text).arg(took / 60).arg(took % 60, 2, 10, QChar('0')));
    m_result->setStyleSheet(QString("color: %1;").arg(color));
    m_canHome = !folder.isEmpty() && IsGameFolder(native(folder).toStdWString());
    QString shown = native(folder).toHtmlEscaped();
    if (m_canHome) {
        m_where->setText(QString("The game is in <b>%1</b>. Go Home to play: this window becomes the game's launcher, "
                                 "with Play, the mods and the settings. Controls and volumes: OPTIONS in the game's "
                                 "pause menu, when the page was added.").arg(shown));
    } else {
        m_where->setText(QString("The game folder <b>%1</b> is not complete: see the log on the previous step.").arg(shown));
    }
    m_home->setVisible(m_canHome);
    m_open->setEnabled(QFileInfo(folder).isDir());
}

void SetupFlow::goHome() {
    if (m_canHome && !busy()) emit home(m_folder);
}

void SetupFlow::saveLog() {
    QString p = QFileDialog::getSaveFileName(m_host, "Save log", "rghport_log.txt", "Text (*.txt)");
    if (p.isEmpty()) return;
    QFile f(p);
    if (f.open(QIODevice::WriteOnly | QIODevice::Truncate)) f.write(m_log->toPlainText().toUtf8());
}

bool SetupFlow::confirmClose() {
    if (busy()) {
        if (QMessageBox::question(m_host, kTitle, "A conversion is running. Stop it and quit?") != QMessageBox::Yes)
            return false;
        m_command->stop();
    }
    store();
    return true;
}

}  // namespace rgh
