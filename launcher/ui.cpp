// ui.cpp - the window (see ui.h): the launcher in a game folder, the setup anywhere else.
#include "ui.h"

#include "music.h"

#include <QApplication>
#include <QCloseEvent>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFontMetrics>
#include <QGraphicsOpacityEffect>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLinearGradient>
#include <QListView>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QProcess>
#include <QRadialGradient>
#include <QScreen>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QStyle>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>

namespace rgh {

namespace {

QString g_headFamily = "Segoe UI";

const char* kStyle = R"(
QMainWindow, QWidget#central { background: #1E1E1E; }
QWidget { color: #F2F2F2; }
QWidget#sidebar { background: #252525; border-right: 1px solid #333333; }
QWidget#topbar { background: #252525; border-bottom: 1px solid #333333; }
QLabel#muted { color: #A8A8A8; }
QLabel#brand { color: #F2F2F2; }
QLabel#pageTitle { color: #F2F2F2; }
QLabel#version { color: #7A7A7A; font-size: 12px; }
QComboBox { background: #2D2D2D; border: 1px solid #3F3F3F; border-radius: 3px; padding: 4px 30px 4px 10px;
            min-height: 24px; color: #F2F2F2; }
QComboBox:hover { border-color: #6A4FC1; background: #333333; }
QComboBox::drop-down { border: none; width: 26px; subcontrol-origin: padding; subcontrol-position: center right; }
QComboBox::down-arrow { image: url(:/chevron.png); width: 12px; height: 12px; }
QComboBox QAbstractItemView { background: #2D2D2D; border: 1px solid #3F3F3F; padding: 2px; color: #F2F2F2;
                              selection-background-color: #5B49B8; selection-color: #FFFFFF; outline: 0; }
QComboBox QAbstractItemView::item { min-height: 26px; padding: 2px 8px; }
QPushButton { background: #3A3A3A; border: 1px solid #4A4A4A; border-radius: 3px; padding: 6px 14px; color: #F2F2F2; }
QPushButton:hover { background: #454545; border-color: #5A5A5A; }
QPushButton:pressed { background: #2F2F2F; }
QPushButton:disabled { background: #2F2F2F; border-color: #3A3A3A; color: #7A7A7A; }
QPushButton#action { background: #C0392B; border: 1px solid #A93226; color: #FFFFFF; }
QPushButton#action:hover { background: #D14837; border-color: #B7392A; }
QPushButton#action:pressed { background: #A93226; }
QPushButton#action:disabled { background: #3A3A3A; border-color: #3A3A3A; color: #8A8A8A; }
QPushButton#play { background: #43A047; border: 1px solid #388E3C; border-bottom: 3px solid #2E7D32;
                   border-radius: 4px; color: #FFFFFF; padding: 4px 28px 3px 24px; min-height: 30px; }
QPushButton#play:hover { background: #4CAF50; border-color: #43A047; border-bottom-color: #2E7D32; }
QPushButton#play:pressed { background: #388E3C; border-bottom: 1px solid #2E7D32; margin-top: 2px; }
QPushButton#play:disabled { background: #3A3A3A; border-color: #3A3A3A; color: #8A8A8A; }
QFrame#card { background: #2D2D2D; border: 1px solid #383838; border-radius: 4px; }
QFrame#card[clickable="true"]:hover { background: #363636; border-color: #6A4FC1; }
QFrame#card QLabel { background: transparent; }
QLabel#badge { color: #66BB6A; }
QPushButton#enable { background: #43A047; border: 1px solid #388E3C; border-radius: 3px; color: #FFFFFF;
                     padding: 6px 26px; min-height: 26px; }
QPushButton#enable:hover { background: #4CAF50; border-color: #43A047; }
QPushButton#enable:pressed { background: #388E3C; }
QPushButton#back { background: transparent; border: none; color: #B39DFF; padding: 4px 6px; text-align: left; }
QPushButton#back:hover { color: #D1C4FF; }
QFrame#shot { background: #262626; border: 1px solid #383838; border-radius: 3px; }
QScrollBar:horizontal { background: transparent; height: 10px; margin: 2px; }
QScrollBar::handle:horizontal { background: #4A4A4A; border-radius: 4px; min-width: 30px; }
QScrollBar::handle:horizontal:hover { background: #6A4FC1; }
QScrollBar::add-line:horizontal, QScrollBar::sub-line:horizontal { width: 0; }
QFrame#panel { background: #2D2D2D; border: 1px solid #383838; border-radius: 4px; }
QFrame#panel QLabel { background: transparent; }
QFrame#panelHead { background: transparent; border: none; border-bottom: 1px solid #4A4A4A; }
QFrame#row { background: transparent; border: none; border-top: 1px solid #383838; }
QScrollArea { border: none; background: transparent; }
QScrollArea > QWidget > QWidget { background: transparent; }
QScrollBar:vertical { background: transparent; width: 10px; margin: 2px; }
QScrollBar::handle:vertical { background: #4A4A4A; border-radius: 4px; min-height: 30px; }
QScrollBar::handle:vertical:hover { background: #6A4FC1; }
QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height: 0; }
QToolTip { background: #2D2D2D; color: #F2F2F2; border: 1px solid #3F3F3F; }
QLineEdit { background: #262626; border: 1px solid #3F3F3F; border-radius: 3px; padding: 5px 10px; min-height: 24px;
            color: #F2F2F2; selection-background-color: #5B49B8; selection-color: #FFFFFF; }
QLineEdit:hover { border-color: #5A5A5A; }
QLineEdit:focus { border-color: #6A4FC1; }
QPlainTextEdit { background: #1A1A1A; border: 1px solid #3F3F3F; border-radius: 3px; padding: 4px; color: #D8D8D8;
                 selection-background-color: #5B49B8; selection-color: #FFFFFF; }
QListWidget { background: #262626; border: 1px solid #3F3F3F; border-radius: 3px; padding: 2px; color: #F2F2F2;
              outline: 0; }
QListWidget::item { padding: 4px 6px; border-radius: 2px; }
QListWidget::item:hover { background: #363636; }
QListWidget::item:selected { background: #5B49B8; color: #FFFFFF; }
QProgressBar { background: #262626; border: 1px solid #3F3F3F; border-radius: 3px; text-align: center; min-height: 22px;
               color: #F2F2F2; }
QProgressBar::chunk { background: #6A4FC1; border-radius: 2px; }
)";

}  // namespace

void SetHeadFamily(const QString& family) { g_headFamily = family; }

// the links of the Home page and the credits, as in the RGH Audio Converter
const char* kGitHubUrl = "https://github.com/Eggscantfly/Rabbids-Go-Home-WiiConverted";
const char* kIssuesUrl = "https://github.com/Eggscantfly/Rabbids-Go-Home-WiiConverted/issues";
struct Credit { const char* name; const char* url; };
const Credit kMadeBy = { "Eggscantfly", "https://www.youtube.com/@EggsCantFly" };
const Credit kThanks[] = { { "Skibidi_Sigma", "https://www.youtube.com/@Skibidisigma-s5y" },
                           { "Cruwby", "https://www.youtube.com/@Cruwby" } };

qreal Dpr() {
    QScreen* screen = QGuiApplication::primaryScreen();
    return screen ? screen->devicePixelRatio() : 1.0;
}

QPixmap Icon(const QString& res, int box) {
    // the picture without its transparent margins, fitted into box x box logical pixels, rendered at the screen's
    // pixel density so it stays sharp
    QImage img(res);
    if (img.isNull()) return QPixmap();
    img = img.convertToFormat(QImage::Format_ARGB32);
    int left = img.width(), top = img.height(), right = -1, bottom = -1;
    for (int y = 0; y < img.height(); ++y) {
        const QRgb* line = reinterpret_cast<const QRgb*>(img.constScanLine(y));
        for (int x = 0; x < img.width(); ++x) {
            if (qAlpha(line[x]) > 8) {
                if (x < left) left = x;
                if (x > right) right = x;
                if (y < top) top = y;
                if (y > bottom) bottom = y;
            }
        }
    }
    if (right >= left && bottom >= top) img = img.copy(left, top, right - left + 1, bottom - top + 1);
    qreal dpr = Dpr();
    int px = int(box * dpr + 0.5);
    QPixmap out = QPixmap::fromImage(img.scaled(px, px, Qt::KeepAspectRatio, Qt::SmoothTransformation));
    out.setDevicePixelRatio(dpr);
    return out;
}

QIcon TrailingIcon(const QString& res, int box, int gap) {
    // the picture with `gap` clear pixels before it: on a button laid out right to left it follows the text at a
    // distance (set the icon size to (box + gap) x box)
    QPixmap pic = Icon(res, box);
    qreal dpr = pic.isNull() ? Dpr() : pic.devicePixelRatio();
    QPixmap out(int((box + gap) * dpr + 0.5), int(box * dpr + 0.5));
    out.setDevicePixelRatio(dpr);
    out.fill(Qt::transparent);
    if (!pic.isNull()) {
        QSizeF size = QSizeF(pic.size()) / dpr;
        QPainter p(&out);
        p.setRenderHint(QPainter::SmoothPixmapTransform, true);
        p.drawPixmap(QRectF(gap + (box - size.width()) / 2, (box - size.height()) / 2, size.width(), size.height()),
                     pic, pic.rect());
    }
    return QIcon(out);
}

QPixmap Sharp(const QString& res, int width) {
    // a picture `width` logical pixels wide at the screen's pixel density
    QPixmap src(res);
    if (src.isNull()) return src;
    qreal dpr = Dpr();
    QPixmap out = src.scaledToWidth(int(width * dpr + 0.5), Qt::SmoothTransformation);
    out.setDevicePixelRatio(dpr);
    return out;
}

QLabel* Link(const QString& text, const QString& url, int px, int weight, QWidget* parent) {
    auto* l = new QLabel(parent);
    l->setText(QString("<a href=\"%1\" style=\"color:#B39DFF;\">%2</a>").arg(url, text.toHtmlEscaped()));
    l->setFont(BodyFont(px, weight));
    l->setOpenExternalLinks(true);
    l->setCursor(Qt::PointingHandCursor);
    l->setToolTip(url);
    return l;
}

QFont HeadFont(int px) {
    QFont f(g_headFamily);
    f.setPixelSize(px);
    return f;
}

QFont BodyFont(int px, int weight) {
    QFont f("Segoe UI");
    f.setPixelSize(px);
    f.setWeight(QFont::Weight(weight));
    return f;
}

QLabel* Muted(const QString& text, QWidget* parent, int px) {
    auto* l = new QLabel(text, parent);
    l->setObjectName("muted");
    l->setFont(BodyFont(px));
    l->setWordWrap(true);
    return l;
}

// a card with a heading: rows (AddRow) go under it
QFrame* Panel(const QString& title, QWidget* parent, QVBoxLayout** rows) {
    auto* f = new QFrame(parent);
    f->setObjectName("panel");
    auto* lay = new QVBoxLayout(f);
    lay->setContentsMargins(20, 14, 20, 8);
    lay->setSpacing(0);
    auto* head = new QFrame(f);
    head->setObjectName("panelHead");
    auto* hl = new QHBoxLayout(head);
    hl->setContentsMargins(0, 0, 0, 8);
    auto* text = new QLabel(title, head);
    text->setFont(BodyFont(16, 600));
    hl->addWidget(text);
    lay->addWidget(head);
    *rows = lay;
    return f;
}

// the same for a row of panels sharing the width equally (the Home page's three, the setup's two)
QFrame* HomePanel(const QString& title, QWidget* parent, QVBoxLayout** body) {
    auto* f = new QFrame(parent);
    f->setObjectName("panel");
    f->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);   // the columns share the width equally
    auto* lay = new QVBoxLayout(f);
    lay->setContentsMargins(18, 12, 18, 14);
    lay->setSpacing(6);
    auto* head = new QFrame(f);
    head->setObjectName("panelHead");
    auto* hl = new QHBoxLayout(head);
    hl->setContentsMargins(0, 0, 0, 8);
    auto* text = new QLabel(title, head);
    text->setFont(BodyFont(15, 600));
    hl->addWidget(text);
    lay->addWidget(head);
    lay->addSpacing(4);
    *body = lay;
    return f;
}

// a row of a panel: a label with a hint at the left, a control at the right
QFrame* AddRow(QVBoxLayout* rows, const QString& label, const QString& hint, QWidget* control) {
    auto* r = new QFrame(rows->parentWidget());
    r->setObjectName("row");
    auto* lay = new QHBoxLayout(r);
    lay->setContentsMargins(0, 10, 0, 10);
    lay->setSpacing(16);
    auto* col = new QVBoxLayout();
    col->setSpacing(2);
    auto* name = new QLabel(label, r);
    name->setFont(BodyFont(14, 600));
    col->addWidget(name);
    if (!hint.isEmpty()) {
        auto* h = new QLabel(hint, r);
        h->setObjectName("muted");
        h->setFont(BodyFont(12));
        h->setWordWrap(true);
        col->addWidget(h);
    }
    lay->addLayout(col, 1);
    control->setParent(r);
    if (auto* c = qobject_cast<QComboBox*>(control)) c->setMinimumWidth(220);
    lay->addWidget(control, 0, Qt::AlignRight | Qt::AlignVCenter);
    rows->addWidget(r);
    return r;
}

// ---------------------------------------------------------------------------------------------------- NavButton
NavButton::NavButton(const QString& icon, const QString& text, QWidget* parent) : QAbstractButton(parent), m_text(text) {
    m_icon = Icon(icon, 34);
    setCheckable(true);
    setCursor(Qt::PointingHandCursor);
    setMinimumHeight(48);
}

NavButton::NavButton(int number, const QString& text, QWidget* parent)
    : QAbstractButton(parent), m_text(text), m_number(number) {
    setCheckable(true);
    setCursor(Qt::PointingHandCursor);
    setMinimumHeight(48);
}

void NavButton::setDone(bool on) {
    if (m_done == on) return;
    m_done = on;
    update();
}

void NavButton::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setRenderHint(QPainter::SmoothPixmapTransform, true);
    QRectF r = rect();
    bool dim = !isEnabled() && !isChecked();
    if (isChecked() || (m_hover && isEnabled())) {
        p.setPen(Qt::NoPen);
        p.setBrush(isChecked() ? QColor("#5B49B8") : QColor("#333333"));
        p.drawRoundedRect(r.adjusted(8, 3, -8, -3), 3, 3);
    }
    QRectF box(24, (r.height() - 34) / 2, 34, 34);
    if (m_number > 0) {
        // a step: its number in a circle, a check mark once it is done
        QPointF c = box.center();
        QColor fill = isChecked() ? QColor("#FFFFFF") : m_done ? QColor("#43A047") : dim ? QColor("#2E2E2E") : QColor("#3F3F3F");
        p.setPen(Qt::NoPen);
        p.setBrush(fill);
        p.drawEllipse(c, 13, 13);
        p.setPen(isChecked() ? QColor("#5B49B8") : dim && !m_done ? QColor("#6E6E6E") : QColor("#FFFFFF"));
        p.setFont(BodyFont(13, 700));
        QString mark = m_done && !isChecked() ? QString::fromUtf8("\xE2\x9C\x93") : QString::number(m_number);
        p.drawText(QRectF(c.x() - 13, c.y() - 13, 26, 26), Qt::AlignCenter, mark);
    } else if (!m_icon.isNull()) {
        QSizeF size = QSizeF(m_icon.size()) / m_icon.devicePixelRatio();
        QRectF dst(box.center() - QPointF(size.width() / 2.0, size.height() / 2.0), size);
        if (dim) p.setOpacity(0.45);
        p.drawPixmap(dst, m_icon, m_icon.rect());
        p.setOpacity(1.0);
    }
    p.setPen(isChecked() ? QColor("#FFFFFF") : dim ? QColor("#6E6E6E") : QColor("#D8D8D8"));
    p.setFont(BodyFont(15, isChecked() ? 600 : 400));
    p.drawText(QRectF(78, 0, r.width() - 90, r.height()), Qt::AlignVCenter | Qt::AlignLeft, m_text);
}

// ------------------------------------------------------------------------------------------------- ToggleSwitch
ToggleSwitch::ToggleSwitch(bool* flag, QWidget* parent) : QAbstractButton(parent), m_flag(flag), m_knob(*flag ? 1.f : 0.f) {
    setCheckable(true);
    setChecked(*flag);
    setCursor(Qt::PointingHandCursor);
    setFixedSize(56, 30);
}

void ToggleSwitch::nextCheckState() {
    *m_flag = !*m_flag;
    sync();
}

void ToggleSwitch::sync() {
    setChecked(*m_flag);
    auto* a = new QPropertyAnimation(this, "knob");
    a->setStartValue(m_knob);
    a->setEndValue(*m_flag ? 1.f : 0.f);
    a->setDuration(160);
    a->setEasingCurve(QEasingCurve::OutCubic);
    a->start(QAbstractAnimation::DeleteWhenStopped);
}

void ToggleSwitch::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);
    QColor off("#4A4A4A"), on("#6A4FC1");
    QColor c(int(off.red() + (on.red() - off.red()) * m_knob), int(off.green() + (on.green() - off.green()) * m_knob),
             int(off.blue() + (on.blue() - off.blue()) * m_knob));
    QRectF track(1, 3, 54, 24);
    p.setPen(Qt::NoPen);
    p.setBrush(c);
    p.drawRoundedRect(track, 12, 12);
    float kx = 13 + m_knob * 30;
    p.setBrush(QColor("#FFFFFF"));
    p.drawEllipse(QPointF(kx, 15), 9, 9);
}

// ------------------------------------------------------------------------------------------------------- Banner
Banner::Banner(QWidget* parent) : QWidget(parent) {
    m_logo = Sharp(":/logo.png", 400);
    m_wordmark = Sharp(":/wordmark.png", 300);
    setMinimumHeight(250);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
}

void Banner::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setRenderHint(QPainter::SmoothPixmapTransform, true);
    QRectF r = rect();
    QPainterPath clip;
    clip.addRoundedRect(r, 4, 4);
    p.setClipPath(clip);
    QLinearGradient bg(0, 0, 0, r.height());
    bg.setColorAt(0, QColor("#303030"));
    bg.setColorAt(1, QColor("#262626"));
    p.fillRect(r, bg);
    QSizeF logo = QSizeF(m_logo.size()) / m_logo.devicePixelRatio();
    QSizeF mark = QSizeF(m_wordmark.size()) / m_wordmark.devicePixelRatio();
    float y = (r.height() - (logo.height() + 12 + mark.height())) / 2;
    if (!m_logo.isNull()) {
        p.drawPixmap(QRectF((r.width() - logo.width()) / 2, y, logo.width(), logo.height()), m_logo, m_logo.rect());
        y += logo.height() + 12;
    }
    if (!m_wordmark.isNull()) {
        p.drawPixmap(QRectF((r.width() - mark.width()) / 2, y, mark.width(), mark.height()), m_wordmark, m_wordmark.rect());
    }
    p.setClipping(false);
    p.setPen(QPen(QColor("#3A3A3A"), 1));
    p.setBrush(Qt::NoBrush);
    p.drawRoundedRect(r.adjusted(0.5, 0.5, -0.5, -0.5), 4, 4);
}

// --------------------------------------------------------------------------------------------------------- Card
Card::Card(const QString& icon, const QString& title, const QString& text, QWidget* parent) : QFrame(parent) {
    setObjectName("card");
    auto* lay = new QHBoxLayout(this);
    lay->setContentsMargins(18, 16, 18, 16);
    lay->setSpacing(16);
    auto* pic = new QLabel(this);
    pic->setPixmap(Icon(icon, 44));
    pic->setFixedSize(48, 48);
    pic->setAlignment(Qt::AlignCenter);
    auto* col = new QVBoxLayout();
    col->setSpacing(4);
    auto* head = new QLabel(title, this);
    head->setFont(BodyFont(15, 600));
    auto* body = new QLabel(text, this);
    body->setObjectName("muted");
    body->setWordWrap(true);
    col->addWidget(head);
    col->addWidget(body);
    lay->addWidget(pic);
    lay->addLayout(col, 1);
    m_badge = new QLabel(this);
    m_badge->setObjectName("badge");
    m_badge->setFont(BodyFont(13, 600));
    m_badge->hide();
    lay->addWidget(m_badge, 0, Qt::AlignVCenter);
}

void Card::setBadge(const QString& text) {
    m_badge->setText(text);
    m_badge->setVisible(!text.isEmpty());
}

void Card::setClickable(bool on) {
    setProperty("clickable", on);
    setCursor(on ? Qt::PointingHandCursor : Qt::ArrowCursor);
    style()->unpolish(this);
    style()->polish(this);
}

void Card::mouseReleaseEvent(QMouseEvent* e) {
    if (e->button() == Qt::LeftButton && rect().contains(e->pos())) emit clicked();
    QFrame::mouseReleaseEvent(e);
}

// ----------------------------------------------------------------------------------------------------- Launcher
Launcher::Launcher(const std::wstring& gameDir, Mode mode, const SetupOptions& setup, QWidget* parent)
    : QMainWindow(parent), m_mode(mode), m_dir(gameDir) {
    if (m_mode == Mode::Game) {
        m_ini = ReadIni(m_dir);
        m_launch = ReadLaunch(m_dir, m_ini);
        m_langs = GameLanguages(m_dir, m_launch);
        m_set = LoadSettings(m_launch, m_ini, m_langs);
    }

    setWindowTitle(m_mode == Mode::Setup ? "WiiConverted Setup" : "WiiConverted Launcher");
    setMinimumSize(960, 600);
    resize(1180, 720);                   // the size it takes when un-maximized (it opens maximized)
    qApp->setStyleSheet(kStyle);
    if (m_mode == Mode::Setup) {
        m_setup = new SetupFlow(QString::fromStdWString(ExeDir()), setup, this);
        connect(m_setup, &SetupFlow::stepChanged, this, &Launcher::showPage);
        connect(m_setup, &SetupFlow::stateChanged, this, &Launcher::refreshSetup);
        connect(m_setup, &SetupFlow::home, this, [this](const QString& folder) {
            switchToGame(QDir::toNativeSeparators(folder).toStdWString());
        });
        m_music = new Music(this);                    // the setup's music, until the setup hands over
        m_music->play(":/setup.rghs");
    }
    buildCentral();
    QTimer::singleShot(0, this, [this] {
        if (m_mode == Mode::Setup) m_setup->begin();
        else showPage(0);
    });
}

void Launcher::buildCentral() {
    auto* central = new QWidget(this);
    central->setObjectName("central");
    auto* row = new QHBoxLayout(central);
    row->setContentsMargins(0, 0, 0, 0);
    row->setSpacing(0);
    row->addWidget(buildSidebar());
    auto* right = new QWidget(central);
    auto* col = new QVBoxLayout(right);
    col->setContentsMargins(0, 0, 0, 0);
    col->setSpacing(0);
    col->addWidget(buildTopBar());
    m_pages = new QStackedWidget(right);
    if (m_mode == Mode::Game) {
        m_pages->addWidget(buildHome());
        m_pages->addWidget(buildMods());
        m_pages->addWidget(buildSettings());
    } else {
        for (int i = 0; i < SetupFlow::N_STEPS; ++i) m_pages->addWidget(m_setup->page(i));
    }
    col->addWidget(m_pages, 1);
    row->addWidget(right, 1);
    if (QWidget* old = takeCentralWidget()) old->deleteLater();
    setCentralWidget(central);
    if (m_mode == Mode::Game && !m_launch.found) {
        m_play->setEnabled(false);
        m_play->setToolTip("There is no game in " + QString::fromStdWString(m_dir) +
                           ": the launcher starts the game's program on the bigfile beside it, and one of the two is "
                           "missing. Run the setup again into this folder.");
    }
}

void Launcher::switchToGame(const std::wstring& dir) {
    // the game folder the setup built becomes this window's game: the launcher's pages replace the steps
    if (m_setup) {
        disconnect(m_setup, nullptr, this, nullptr);
        m_setup->deleteLater();                  // its pages go with the old central widget
        m_setup = nullptr;
    }
    if (m_music) m_music->fadeOut(1500);         // the launcher plays nothing
    m_mode = Mode::Game;
    m_dir = dir;
    m_ini = ReadIni(m_dir);
    m_launch = ReadLaunch(m_dir, m_ini);
    m_langs = GameLanguages(m_dir, m_launch);
    m_set = LoadSettings(m_launch, m_ini, m_langs);
    m_nav.clear();
    m_combos.clear();
    m_comboValues.clear();
    m_toggles.clear();
    m_mods.clear();
    m_play = m_back = m_stop = m_next = nullptr;
    m_launching = false;
    setWindowTitle("WiiConverted Launcher");
    buildCentral();
    QTimer::singleShot(0, this, [this] { showPage(0); });
}

QWidget* Launcher::buildSidebar() {
    auto* side = new QWidget(this);
    side->setObjectName("sidebar");
    side->setFixedWidth(250);
    auto* lay = new QVBoxLayout(side);
    lay->setContentsMargins(0, 18, 0, 14);
    lay->setSpacing(4);

    auto* header = new QWidget(side);
    auto* h = new QHBoxLayout(header);
    h->setContentsMargins(24, 0, 12, 12);          // the icon sits on the sidebar icons' column
    h->setSpacing(20);
    auto* icon = new QLabel(header);
    icon->setPixmap(Icon(":/wcicon.png", 34));
    icon->setFixedSize(34, 34);
    icon->setAlignment(Qt::AlignCenter);
    auto* names = new QVBoxLayout();
    names->setSpacing(0);
    auto* title = new QLabel("WiiConverted", header);
    title->setObjectName("brand");
    title->setFont(BodyFont(17, 600));
    auto* sub = new QLabel(m_mode == Mode::Setup ? "Rabbids Go Home setup" : "Rabbids Go Home launcher", header);
    sub->setObjectName("muted");
    sub->setFont(BodyFont(12));
    names->addWidget(title);
    names->addWidget(sub);
    h->addWidget(icon);
    h->addLayout(names, 1);
    lay->addWidget(header);

    auto* navHost = new QWidget(side);
    auto* nav = new QVBoxLayout(navHost);
    nav->setContentsMargins(0, 0, 0, 0);
    nav->setSpacing(2);
    if (m_mode == Mode::Game) {
        const char* icons[] = { ":/moonicon.png", ":/mods.png", ":/settings.png" };
        const char* names2[] = { "HOME", "MODS", "SETTINGS" };
        for (int i = 0; i < 3; ++i) {
            auto* b = new NavButton(icons[i], names2[i], navHost);
            connect(b, &QAbstractButton::clicked, this, [this, i] { showPage(i); });
            nav->addWidget(b);
            m_nav.push_back(b);
        }
    } else {
        // the steps: the welcome page with the moon, then the numbered steps
        for (int i = 0; i < SetupFlow::N_STEPS; ++i) {
            NavButton* b = i == 0 ? new NavButton(":/moonicon.png", SetupFlow::kStepTitle[0], navHost)
                                  : new NavButton(i, SetupFlow::kStepTitle[i], navHost);
            connect(b, &QAbstractButton::clicked, this, [this, i] { m_setup->jump(i); });
            nav->addWidget(b);
            m_nav.push_back(b);
        }
    }
    m_marker = new QWidget(navHost);
    m_marker->setFixedSize(0, 0);
    m_marker->hide();
    lay->addWidget(navHost);
    lay->addStretch(1);

    auto* version = new QLabel(m_mode == Mode::Setup ? "Setup 2.2" : "Launcher 2.2", side);
    version->setObjectName("version");
    version->setContentsMargins(22, 0, 0, 0);
    lay->addWidget(version);
    return side;
}

QComboBox* Launcher::combo(const Choice* items, int n, int* value) {
    auto* c = new QComboBox(this);
    c->setView(new QListView(c));
    c->setCursor(Qt::PointingHandCursor);
    for (int i = 0; i < n; ++i) c->addItem(QString::fromWCharArray(items[i].label));
    c->setCurrentIndex(*value);
    connect(c, &QComboBox::currentIndexChanged, this, [this, value](int i) {
        if (i >= 0) { *value = i; refreshCombos(); }
    });
    m_combos.push_back(c);
    m_comboValues.push_back(value);
    return c;
}

QWidget* Launcher::buildTopBar() {
    auto* bar = new QWidget(this);
    bar->setObjectName("topbar");
    bar->setFixedHeight(76);
    auto* lay = new QHBoxLayout(bar);
    lay->setContentsMargins(28, 0, 28, 0);
    lay->setSpacing(12);
    m_pageTitle = new QLabel(m_mode == Mode::Setup ? SetupFlow::kStepTitle[0] : "HOME", bar);
    m_pageTitle->setObjectName("pageTitle");
    m_pageTitle->setFont(BodyFont(20, 600));
    lay->addWidget(m_pageTitle);
    lay->addStretch(1);
    if (m_mode == Mode::Setup) {
        // the setup walks with Back and Next; Stop while a conversion runs
        m_back = new QPushButton("Back", bar);
        m_back->setFont(BodyFont(15, 600));
        m_back->setMinimumSize(110, 38);
        m_back->setCursor(Qt::PointingHandCursor);
        connect(m_back, &QPushButton::clicked, this, [this] { m_setup->goBack(); });
        m_stop = new QPushButton("Stop", bar);
        m_stop->setObjectName("action");
        m_stop->setFont(BodyFont(15, 600));
        m_stop->setMinimumSize(110, 38);
        m_stop->setCursor(Qt::PointingHandCursor);
        connect(m_stop, &QPushButton::clicked, this, [this] { m_setup->stopRun(); });
        m_next = new QPushButton("Next", bar);
        m_next->setObjectName("play");
        m_next->setFont(BodyFont(15, 600));
        m_next->setMinimumWidth(150);
        m_next->setCursor(Qt::PointingHandCursor);
        m_next->setDefault(true);
        connect(m_next, &QPushButton::clicked, this, [this] { m_setup->goNext(); });
        lay->addWidget(m_back);
        lay->addWidget(m_stop);
        lay->addWidget(m_next);
        return bar;
    }
    auto* lang = combo(m_langs.data(), (int)m_langs.size(), &m_set.lang);
    lang->setToolTip("Language");
    lang->setMinimumWidth(130);
    auto* res = combo(kResolution, N_RESOLUTION, &m_set.res);
    res->setToolTip("Window size");
    res->setMinimumWidth(150);
    lay->addWidget(lang);
    lay->addWidget(res);
    m_play = new QPushButton(QString::fromUtf8("\xE2\x96\xB6  Play"), bar);
    m_play->setObjectName("play");
    m_play->setFont(BodyFont(15, 600));
    m_play->setMinimumWidth(130);
    m_play->setCursor(Qt::PointingHandCursor);
    m_play->setDefault(true);
    connect(m_play, &QPushButton::clicked, this, &Launcher::startGame);
    lay->addWidget(m_play);
    return bar;
}

namespace {

int countFiles(const QString& folder, const QString& pattern) {
    return int(QDir(folder).entryList(QStringList() << pattern, QDir::Files).size());
}

int countDirs(const QString& folder) {
    return int(QDir(folder).entryList(QDir::Dirs | QDir::NoDotAndDotDot).size());
}

}  // namespace

QWidget* Launcher::buildHome() {
    auto* page = new QWidget(this);
    auto* lay = new QVBoxLayout(page);
    lay->setContentsMargins(28, 24, 28, 24);
    lay->setSpacing(18);
    lay->addWidget(new Banner(page));

    auto* row = new QGridLayout();          // three columns of the same width
    row->setSpacing(16);
    for (int c = 0; c < 3; ++c) row->setColumnStretch(c, 1);
    QVBoxLayout* body = nullptr;

    QFrame* links = HomePanel("Links", page, &body);
    body->addWidget(Link("Project page on GitHub", kGitHubUrl, 14, 400, links));
    body->addWidget(Link("Report a problem", kIssuesUrl, 14, 400, links));
    body->addStretch(1);
    row->addWidget(links, 0, 0);

    QString dir = QString::fromStdWString(m_dir);
    QFrame* game = HomePanel("This game", page, &body);
    QStringList names;
    for (const Choice& c : m_langs) names << QString::fromWCharArray(c.label);
    auto* info = new QLabel(QString("Languages: %1\nSaves: %2 file(s)\nMods: %3 installed\nLauncher 2.2")
                                .arg(names.join(", ")).arg(countFiles(dir + "/sav", "*.sav")).arg(countDirs(dir + "/mods")),
                            game);
    info->setWordWrap(true);
    info->setFont(BodyFont(14));
    body->addWidget(info);
    auto* path = Muted(QFontMetrics(BodyFont(12)).elidedText(dir, Qt::ElideMiddle, 210), game);
    path->setWordWrap(false);
    path->setToolTip(dir);
    body->addWidget(path);
    auto* open = new QPushButton("Open game folder", game);
    open->setObjectName("action");
    open->setCursor(Qt::PointingHandCursor);
    connect(open, &QPushButton::clicked, this, [this] { openFolder(); });
    body->addStretch(1);
    body->addWidget(open, 0, Qt::AlignLeft);
    row->addWidget(game, 0, 1);

    QFrame* credits = HomePanel("Credits", page, &body);
    body->addWidget(Muted("Made by", credits));
    body->addWidget(Link(kMadeBy.name, kMadeBy.url, 16, 700, credits));
    body->addSpacing(6);
    body->addWidget(Muted("Special thanks", credits));
    for (const Credit& c : kThanks) body->addWidget(Link(c.name, c.url, 14, 600, credits));
    body->addStretch(1);
    row->addWidget(credits, 0, 2);

    lay->addLayout(row);
    lay->addStretch(1);
    return page;
}

namespace {

void clearLayout(QLayout* lay) {
    while (QLayoutItem* it = lay->takeAt(0)) {
        if (it->widget()) it->widget()->deleteLater();
        delete it;
    }
}

QString modLine(const Mod& m) {
    QString line;
    if (!m.author.empty()) line += "by " + QString::fromStdWString(m.author);
    if (!m.version.empty()) line += (line.isEmpty() ? "" : "  -  ") + QString("Version ") + QString::fromStdWString(m.version);
    return line;
}

}  // namespace

QWidget* Launcher::buildMods() {
    auto* page = new QWidget(this);
    auto* outer = new QVBoxLayout(page);
    outer->setContentsMargins(0, 0, 0, 0);
    m_modViews = new QStackedWidget(page);
    outer->addWidget(m_modViews);

    // ---- the list
    m_modList = new QWidget(m_modViews);
    auto* lay = new QVBoxLayout(m_modList);
    lay->setContentsMargins(28, 24, 28, 24);
    lay->setSpacing(14);
    auto* head = new QHBoxLayout();
    auto* title = new QLabel("Mod loader", m_modList);
    title->setFont(BodyFont(20, 600));
    head->addWidget(title);
    m_modCount = new QLabel(m_modList);
    m_modCount->setObjectName("muted");
    m_modCount->setFont(BodyFont(14));
    head->addSpacing(14);
    head->addWidget(m_modCount);
    head->addStretch(1);
    auto* refresh = new QPushButton("Refresh", m_modList);
    refresh->setCursor(Qt::PointingHandCursor);
    connect(refresh, &QPushButton::clicked, this, &Launcher::rebuildModList);
    head->addWidget(refresh);
    auto* open = new QPushButton("Open mods folder", m_modList);
    open->setObjectName("action");
    open->setCursor(Qt::PointingHandCursor);
    connect(open, &QPushButton::clicked, this, [this] { openFolder("mods"); });
    head->addWidget(open);
    lay->addLayout(head);
    auto* sub = new QLabel("A mod is a folder in the game's mods folder: a mod.json (name, author, version, description, icon, "
                           "screenshots), and what the game reads instead of its archive - entries, records, main.lua. Click a mod to "
                           "see it and turn it on; the game reads the enabled mods from their folders when it starts.", m_modList);
    sub->setObjectName("muted");
    sub->setWordWrap(true);
    lay->addWidget(sub);
    auto* scroll = new QScrollArea(m_modList);
    scroll->setWidgetResizable(true);
    auto* inner = new QWidget(scroll);
    m_modListLayout = new QVBoxLayout(inner);
    m_modListLayout->setContentsMargins(0, 6, 8, 0);
    m_modListLayout->setSpacing(10);
    scroll->setWidget(inner);
    lay->addWidget(scroll, 1);
    m_modViews->addWidget(m_modList);

    // ---- one mod: its screenshots, author, description and the switch
    m_modDetail = new QWidget(m_modViews);
    auto* det = new QVBoxLayout(m_modDetail);
    det->setContentsMargins(28, 18, 28, 24);
    det->setSpacing(14);
    auto* back = new QPushButton(QString::fromUtf8("\xE2\x86\x90  Back to the mods"), m_modDetail);
    back->setObjectName("back");
    back->setCursor(Qt::PointingHandCursor);
    back->setFont(BodyFont(14, 600));
    connect(back, &QPushButton::clicked, this, [this] { m_modViews->setCurrentWidget(m_modList); });
    det->addWidget(back, 0, Qt::AlignLeft);
    auto* trow = new QHBoxLayout();
    trow->setSpacing(14);
    m_modTitle = new QLabel(m_modDetail);
    m_modTitle->setFont(BodyFont(22, 600));
    trow->addWidget(m_modTitle);
    m_modBadge = new QLabel("Enabled", m_modDetail);
    m_modBadge->setObjectName("badge");
    m_modBadge->setFont(BodyFont(14, 600));
    trow->addWidget(m_modBadge, 0, Qt::AlignBottom);
    trow->addStretch(1);
    det->addLayout(trow);
    auto* shots = new QScrollArea(m_modDetail);
    shots->setWidgetResizable(true);
    shots->setFixedHeight(320);
    shots->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    shots->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_modShots = new QWidget(shots);
    auto* srow = new QHBoxLayout(m_modShots);
    srow->setContentsMargins(0, 0, 0, 10);
    srow->setSpacing(16);
    shots->setWidget(m_modShots);
    det->addWidget(shots);
    m_modAuthor = new QLabel(m_modDetail);
    m_modAuthor->setFont(BodyFont(15));
    det->addWidget(m_modAuthor);
    m_modVersion = new QLabel(m_modDetail);
    m_modVersion->setFont(BodyFont(15));
    det->addWidget(m_modVersion);
    m_modText = new QLabel(m_modDetail);
    m_modText->setFont(BodyFont(15));
    m_modText->setWordWrap(true);
    m_modText->setTextInteractionFlags(Qt::TextSelectableByMouse);
    det->addWidget(m_modText);
    det->addStretch(1);
    auto* brow = new QHBoxLayout();
    brow->addStretch(1);
    m_modEnable = new QPushButton("Enable", m_modDetail);
    m_modEnable->setObjectName("enable");
    m_modEnable->setFont(BodyFont(15, 600));
    m_modEnable->setCursor(Qt::PointingHandCursor);
    m_modEnable->setMinimumWidth(150);
    connect(m_modEnable, &QPushButton::clicked, this, [this] { toggleMod(m_modOpen); });
    brow->addWidget(m_modEnable);
    det->addLayout(brow);
    m_modViews->addWidget(m_modDetail);

    rebuildModList();
    return page;
}

void Launcher::rebuildModList() {
    m_mods = ScanMods(m_dir, m_ini);
    clearLayout(m_modListLayout);
    QWidget* inner = m_modListLayout->parentWidget();
    int enabled = 0;
    for (const Mod& m : m_mods) enabled += m.enabled ? 1 : 0;
    if (m_mods.empty()) {
        m_modCount->setText("no mods installed");
        m_modListLayout->addWidget(new Card(":/mods.png", "0 mods installed", "Mods go into the mods folder.", inner));
    } else {
        m_modCount->setText(QString("%1 installed, %2 enabled").arg(m_mods.size()).arg(enabled));
        for (size_t i = 0; i < m_mods.size(); ++i) {
            const Mod& m = m_mods[i];
            QString icon = m.icon.empty() ? QString(":/mods.png") : QString::fromStdWString(m.icon);
            auto* card = new Card(icon, QString::fromStdWString(m.name), modLine(m), inner);
            card->setClickable(true);
            if (m.enabled) card->setBadge("Enabled");
            connect(card, &Card::clicked, this, [this, i] { showMod(i); });
            m_modListLayout->addWidget(card);
        }
    }
    m_modListLayout->addStretch(1);
    m_modViews->setCurrentWidget(m_modList);
}

void Launcher::showMod(size_t index) {
    if (index >= m_mods.size()) return;
    m_modOpen = index;
    const Mod& m = m_mods[index];
    m_modTitle->setText(QString::fromStdWString(m.name));
    m_modBadge->setVisible(m.enabled);
    m_modAuthor->setText(m.author.empty() ? "Author: unknown" : "Author: " + QString::fromStdWString(m.author));
    m_modVersion->setText("Version: " + QString::fromStdWString(m.version));
    m_modVersion->setVisible(!m.version.empty());
    m_modText->setText(m.description.empty() ? "No description." : QString::fromStdWString(m.description));
    m_modEnable->setText(m.enabled ? "Disable" : "Enable");
    m_modEnable->setObjectName(m.enabled ? "action" : "enable");
    m_modEnable->style()->unpolish(m_modEnable);
    m_modEnable->style()->polish(m_modEnable);
    // the screenshots, 300 logical pixels high, sharp at the screen's pixel density
    auto* row = static_cast<QHBoxLayout*>(m_modShots->layout());
    clearLayout(row);
    qreal dpr = Dpr();
    int n = 0;
    for (const std::wstring& path : m.screenshots) {
        QImage img(QString::fromStdWString(path));
        if (img.isNull()) continue;
        QPixmap pm = QPixmap::fromImage(img.scaledToHeight(int(300 * dpr + 0.5), Qt::SmoothTransformation));
        pm.setDevicePixelRatio(dpr);
        auto* frame = new QFrame(m_modShots);
        frame->setObjectName("shot");
        auto* fl = new QVBoxLayout(frame);
        fl->setContentsMargins(0, 0, 0, 0);
        auto* pic = new QLabel(frame);
        pic->setPixmap(pm);
        pic->setFixedSize(int(pm.width() / dpr + 0.5), 300);
        pic->setToolTip(QFileInfo(QString::fromStdWString(path)).fileName());
        fl->addWidget(pic);
        row->addWidget(frame);
        ++n;
    }
    if (n == 0) {
        auto* none = new QLabel("No screenshots.", m_modShots);
        none->setObjectName("muted");
        none->setFont(BodyFont(14));
        row->addWidget(none, 0, Qt::AlignTop);
    }
    row->addStretch(1);
    m_modViews->setCurrentWidget(m_modDetail);
}

void Launcher::toggleMod(size_t index) {
    if (index >= m_mods.size()) return;
    Mod& m = m_mods[index];
    m.enabled = !m.enabled;
    SetModEnabled(m_ini, m.folder, m.enabled);
    if (!WriteIni(m_dir, m_ini)) {
        QMessageBox::warning(this, "WiiConverted Launcher", "options.ini could not be written.");
    }
    // the list reflects the change; the opened mod stays on screen
    std::wstring folder = m.folder;
    rebuildModList();
    for (size_t i = 0; i < m_mods.size(); ++i)
        if (m_mods[i].folder == folder) { showMod(i); break; }
}

QWidget* Launcher::buildSettings() {
    auto* page = new QWidget(this);
    auto* outer = new QVBoxLayout(page);
    outer->setContentsMargins(28, 24, 28, 24);
    outer->setSpacing(14);
    auto* title = new QLabel("Settings", page);
    title->setFont(BodyFont(20, 600));
    outer->addWidget(title);
    auto* sub = new QLabel("Saved when you press Play or close the launcher.", page);
    sub->setObjectName("muted");
    outer->addWidget(sub);

    auto* scroll = new QScrollArea(page);
    scroll->setWidgetResizable(true);
    auto* inner = new QWidget(scroll);
    auto* lay = new QVBoxLayout(inner);
    lay->setContentsMargins(0, 6, 8, 0);
    lay->setSpacing(14);

    QVBoxLayout* rows = nullptr;
    QFrame* display = Panel("Display", inner, &rows);
    AddRow(rows, "Display mode", "A window, or a borderless window over the whole screen.", combo(kDisplay, N_DISPLAY, &m_set.display));
    AddRow(rows, "Window size", "The size the game window opens at.", combo(kResolution, N_RESOLUTION, &m_set.res));
    auto* vsync = new ToggleSwitch(&m_set.vsync);
    m_toggles.push_back(vsync);
    AddRow(rows, "VSync", "Waits for the screen's refresh: no tearing.", vsync);
    lay->addWidget(display);

    QFrame* game = Panel("Game", inner, &rows);
    AddRow(rows, "Frame rate", "The Wii ran at 60 Hz; the game's timing follows the cap.", combo(kFrameRate, N_FRAMERATE, &m_set.frameRate));
    auto* effects = new ToggleSwitch(&m_set.effects);
    m_toggles.push_back(effects);
    AddRow(rows, "Wii picture effects", "The colour grade and the soft glow the Wii put over the picture.", effects);
    auto* fps = new ToggleSwitch(&m_set.fps);
    m_toggles.push_back(fps);
    AddRow(rows, "FPS counter", "Shows the frame rate in the game.", fps);
    AddRow(rows, "Language", "The game's texts and voices: the languages the Wii disc had.",
           combo(m_langs.data(), (int)m_langs.size(), &m_set.lang));
    lay->addWidget(game);

    lay->addStretch(1);
    scroll->setWidget(inner);
    outer->addWidget(scroll, 1);
    return page;
}

void Launcher::showPage(int index) {
    static const char* gameTitles[] = { "HOME", "MODS", "SETTINGS" };
    bool first = m_pages->currentIndex() < 0 || !m_pages->currentWidget()->isVisible();
    m_page = index;
    m_pageTitle->setText(m_mode == Mode::Game ? gameTitles[index] : SetupFlow::kStepTitle[index]);
    for (size_t i = 0; i < m_nav.size(); ++i) m_nav[i]->setChecked(int(i) == index);
    // the page fades in
    QWidget* page = m_pages->widget(index);
    if (m_pages->currentWidget() != page || first) {
        auto* fx = new QGraphicsOpacityEffect(page);
        page->setGraphicsEffect(fx);
        m_pages->setCurrentWidget(page);
        auto* a = new QPropertyAnimation(fx, "opacity");
        a->setStartValue(0.0);
        a->setEndValue(1.0);
        a->setDuration(220);
        a->setEasingCurve(QEasingCurve::OutCubic);
        connect(a, &QAbstractAnimation::finished, page, [page] { page->setGraphicsEffect(nullptr); });
        a->start(QAbstractAnimation::DeleteWhenStopped);
    }
}

void Launcher::refreshSetup() {
    if (!m_setup) return;
    m_back->setVisible(m_setup->backVisible());
    m_back->setEnabled(!m_setup->busy());
    m_stop->setVisible(m_setup->stopVisible());
    m_stop->setEnabled(m_setup->busy());
    m_next->setVisible(m_setup->nextVisible());
    QString word = m_setup->nextText();
    if (word == "Go Home") {                                            // the moon, as the Home entry
        m_next->setText(word);
        m_next->setIcon(TrailingIcon(":/moonicon.png", 22, 10));
        m_next->setIconSize(QSize(32, 22));
        m_next->setLayoutDirection(Qt::RightToLeft);                       // the icon after the text
    } else {
        m_next->setIcon(QIcon());
        m_next->setLayoutDirection(Qt::LeftToRight);
        m_next->setText(word == "Close" ? word : word + QString::fromUtf8("  \xE2\x96\xB6"));
    }
    m_next->setEnabled(m_setup->nextEnabled());
    for (size_t i = 0; i < m_nav.size(); ++i) {
        bool open = m_setup->reached(int(i)) || int(i) == m_setup->step();
        m_nav[i]->setEnabled(open);
        m_nav[i]->setCursor(open ? Qt::PointingHandCursor : Qt::ArrowCursor);
        m_nav[i]->setDone(m_setup->done(int(i)));
    }
}

void Launcher::refreshCombos() {
    // the same setting can sit in two boxes (the top bar and the settings page)
    for (size_t i = 0; i < m_combos.size(); ++i) {
        if (m_combos[i]->currentIndex() != *m_comboValues[i]) {
            QSignalBlocker block(m_combos[i]);
            m_combos[i]->setCurrentIndex(*m_comboValues[i]);
        }
    }
}

bool Launcher::save() {
    if (m_mode != Mode::Game) return true;
    return SaveSettings(m_dir, m_set, m_launch, m_ini, m_langs);
}

void Launcher::openFolder(const QString& sub) {
    QString path = QString::fromStdWString(m_dir);
    if (!sub.isEmpty()) {
        path += "/" + sub;
        QDir().mkpath(path);
    }
    QDesktopServices::openUrl(QUrl::fromLocalFile(path));
}

void Launcher::startGame() {
    if (m_mode != Mode::Game || m_launching || !m_launch.found) return;
    m_launching = true;
    m_play->setText("...");
    if (!save()) {
        QMessageBox::warning(this, "WiiConverted Launcher", "The settings could not be saved (is the folder read-only?).");
    }
    std::wstring exe, cmd;
    std::vector<std::string> switches;
    GameCommand(m_dir, m_launch, m_mods, exe, cmd, switches);
    QString program = QString::fromStdWString(exe);
    if (!QFile::exists(program)) {
        QMessageBox::critical(this, "WiiConverted Launcher", "The game's executable is missing:\n" + program);
        m_launching = false;
        m_play->setText(QString::fromUtf8("\xE2\x96\xB6  Play"));
        return;
    }
    QStringList args;
    args << QString::fromStdWString(m_dir + L"\\" + Widen(m_launch.bigfile));
    for (const std::string& s : switches) args << QString::fromStdString(s);
    if (!QProcess::startDetached(program, args, QString::fromStdWString(m_dir))) {
        QMessageBox::critical(this, "WiiConverted Launcher", "The game could not be started.");
        m_launching = false;
        m_play->setText(QString::fromUtf8("\xE2\x96\xB6  Play"));
        return;
    }
    QTimer::singleShot(350, qApp, &QApplication::quit);
}

void Launcher::keyPressEvent(QKeyEvent* e) {
    if (e->key() == Qt::Key_Escape) {
        close();
    } else if (e->key() == Qt::Key_Return || e->key() == Qt::Key_Enter) {
        if (m_mode == Mode::Setup) {
            if (m_setup->nextVisible() && m_setup->nextEnabled()) m_setup->goNext();
        } else {
            startGame();
        }
    } else {
        QMainWindow::keyPressEvent(e);
    }
}

void Launcher::closeEvent(QCloseEvent* e) {
    if (m_mode == Mode::Setup) {
        if (!m_setup->confirmClose()) {
            e->ignore();
            return;
        }
    } else {
        save();
    }
    QMainWindow::closeEvent(e);
}

}  // namespace rgh
