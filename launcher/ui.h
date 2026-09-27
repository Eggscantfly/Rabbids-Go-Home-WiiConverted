// ui.h - the window: a dark Windows application in the spirit of the Minecraft launcher - a sidebar with the pages, a
// top bar, a banner with the game's art, and cards.  Real Qt widgets under a dark stylesheet; the few animations (the
// active tab's marker, the page crossfade, the toggles, the moon in the banner) are QPropertyAnimations and a slow
// timer.  Two modes: in a game folder it is the launcher (Home, Mods, Settings, PLAY); anywhere else it is the setup,
// the same window walking through the steps that build a game folder (setup.h), and once the game folder is built
// the window turns into its launcher (Go Home).
#pragma once

#include <QAbstractButton>
#include <QComboBox>
#include <QFrame>
#include <QIcon>
#include <QLabel>
#include <QMainWindow>
#include <QPixmap>
#include <QPropertyAnimation>
#include <QPushButton>
#include <QStackedWidget>
#include <QVBoxLayout>
#include <QWidget>
#include <vector>

#include "settings.h"
#include "setup.h"

namespace rgh {

class Music;

void SetHeadFamily(const QString& family);
QFont HeadFont(int px);              // the Rabbids font
QFont BodyFont(int px, int weight = 400);

// the building blocks the pages share
QPixmap Icon(const QString& res, int box);                                     // a picture without its margins, sharp
QIcon TrailingIcon(const QString& res, int box, int gap);                      // the same after a gap: a button's end
QLabel* Link(const QString& text, const QString& url, int px, int weight, QWidget* parent);
QLabel* Muted(const QString& text, QWidget* parent, int px = 12);             // a grey line of text, wrapped
QFrame* Panel(const QString& title, QWidget* parent, QVBoxLayout** rows);     // a card with a heading and rows
QFrame* HomePanel(const QString& title, QWidget* parent, QVBoxLayout** body); // the same, sharing a row's width equally
QFrame* AddRow(QVBoxLayout* rows, const QString& label, const QString& hint, QWidget* control);

// a sidebar entry: an icon or a step number, and a label; checked = the page shown
class NavButton : public QAbstractButton {
    Q_OBJECT
public:
    NavButton(const QString& icon, const QString& text, QWidget* parent = nullptr);
    NavButton(int number, const QString& text, QWidget* parent = nullptr);    // a numbered step
    QSize sizeHint() const override { return QSize(240, 48); }
    void setDone(bool on);                                                     // a numbered step shows a check mark
protected:
    void paintEvent(QPaintEvent*) override;
    void enterEvent(QEnterEvent*) override { m_hover = true; update(); }
    void leaveEvent(QEvent*) override { m_hover = false; update(); }
private:
    QPixmap m_icon;
    QString m_text;
    int m_number = 0;
    bool m_done = false;
    bool m_hover = false;
};

// an on / off switch with a sliding knob
class ToggleSwitch : public QAbstractButton {
    Q_OBJECT
    Q_PROPERTY(float knob READ knob WRITE setKnob)
public:
    ToggleSwitch(bool* flag, QWidget* parent = nullptr);
    QSize sizeHint() const override { return QSize(56, 30); }
    float knob() const { return m_knob; }
    void setKnob(float k) { m_knob = k; update(); }
    void sync();
protected:
    void paintEvent(QPaintEvent*) override;
    void nextCheckState() override;
private:
    bool* m_flag;
    float m_knob;
};

// the banner: a gradient, the logo, the wordmark and the moon turning slowly
class Banner : public QWidget {
    Q_OBJECT
public:
    explicit Banner(QWidget* parent = nullptr);
    QSize sizeHint() const override { return QSize(800, 300); }
protected:
    void paintEvent(QPaintEvent*) override;
private:
    QPixmap m_logo, m_wordmark, m_moon;
    float m_angle = 0.f;
};

// a card: an icon, a title, a line of text; clickable when it has a target
class Card : public QFrame {
    Q_OBJECT
public:
    Card(const QString& icon, const QString& title, const QString& text, QWidget* parent = nullptr);
    void setBadge(const QString& text);      // a short green word at the right (e.g. "Enabled"); empty hides it
    void setClickable(bool on);
signals:
    void clicked();
protected:
    void mouseReleaseEvent(QMouseEvent* e) override;
private:
    QLabel* m_badge = nullptr;
};

class Launcher : public QMainWindow {
    Q_OBJECT
public:
    enum class Mode { Game, Setup };
    explicit Launcher(const std::wstring& gameDir, Mode mode = Mode::Game, const SetupOptions& setup = SetupOptions(),
                      QWidget* parent = nullptr);
    SetupFlow* setup() const { return m_setup; }
    void switchToGame(const std::wstring& dir);   // the setup is over: this window becomes the launcher of that folder
protected:
    void closeEvent(QCloseEvent*) override;
    void keyPressEvent(QKeyEvent*) override;
private:
    void buildCentral();                 // the sidebar, the top bar and the pages of the current mode
    QWidget* buildSidebar();
    QWidget* buildTopBar();
    QWidget* buildHome();
    QWidget* buildMods();
    void rebuildModList();
    void showMod(size_t index);
    void toggleMod(size_t index);
    QWidget* buildSettings();
    QComboBox* combo(const Choice* items, int n, int* value);
    void showPage(int index);
    void refreshCombos();
    void refreshSetup();                 // the setup's buttons and sidebar after a change
    bool save();
    void startGame();
    void openFolder(const QString& sub = QString());

    Mode m_mode;
    std::wstring m_dir;
    Launch m_launch;
    Ini m_ini;
    std::vector<Choice> m_langs;         // the languages this game has texts for
    Settings m_set;
    QStackedWidget* m_pages = nullptr;
    QLabel* m_pageTitle = nullptr;
    QWidget* m_marker = nullptr;
    std::vector<NavButton*> m_nav;
    std::vector<QComboBox*> m_combos;
    std::vector<int*> m_comboValues;
    std::vector<ToggleSwitch*> m_toggles;
    QPushButton* m_play = nullptr;
    bool m_launching = false;
    int m_page = 0;
    // the setup (Mode::Setup): its pages and the top bar's buttons
    SetupFlow* m_setup = nullptr;
    Music* m_music = nullptr;            // the setup's music; fades out on Go Home
    QPushButton* m_back = nullptr;
    QPushButton* m_stop = nullptr;
    QPushButton* m_next = nullptr;
    // the Mods page: a list of the mods, and one mod opened
    std::vector<Mod> m_mods;
    QStackedWidget* m_modViews = nullptr;
    QWidget* m_modList = nullptr;
    QVBoxLayout* m_modListLayout = nullptr;
    QWidget* m_modDetail = nullptr;
    QLabel* m_modTitle = nullptr;
    QLabel* m_modBadge = nullptr;
    QWidget* m_modShots = nullptr;
    QLabel* m_modAuthor = nullptr;
    QLabel* m_modVersion = nullptr;
    QLabel* m_modText = nullptr;
    QPushButton* m_modEnable = nullptr;
    QLabel* m_modCount = nullptr;
    size_t m_modOpen = 0;
};

}  // namespace rgh
