// meter.h - the conversion's meter: the game's own end-of-level count, played from its menu data.  The sheet
// (Assets/GUI/meter/HUD_endlevel_3_0.png, the sprites as the game ships them) and the layout (layout.json: the areas,
// elements, keyframes with their rotations and pivots, mask modes and blend modes of the HUD_Toilet page) are Qt
// resources.  The widget renders the page the way the game's menu renderer does, with the conversion's progress as
// the tower's frame: the orange fill rises inside the pipes, the reward icons light up one by one with their burst,
// the splash rides the surface and the tag climbs beside it with the count (0 to 1000, as the game shows it).
#pragma once

#include <QColor>
#include <QElapsedTimer>
#include <QHash>
#include <QImage>
#include <QPointF>
#include <QSet>
#include <QRectF>
#include <QString>
#include <QVector>
#include <QWidget>
#include <vector>

class QPainter;

namespace rgh {

class JunkMeter : public QWidget {
    Q_OBJECT
public:
    explicit JunkMeter(QWidget* parent = nullptr);
    QSize sizeHint() const override { return QSize(300, 520); }
    QSize minimumSizeHint() const override { return QSize(240, 380); }
    void reset();                                     // empty and still
    void setBusy(bool on);                            // the splash plays while the converter works
    void setProgress(int done, int total, bool instant = false);   // the fill and the count (instant: no rise)
    void setDone(bool ok);                            // ok: full; else it stays where it was
    bool rising() const { return m_frame < m_target; }
signals:                                              // the scene's moments, for its sounds
    void fillStarted();                               // the fill begins to rise
    void fillStopped();                               // ... and rests
    void fillLevel(float part);                       // how full the tower is (0..1), while the fill rises
    void giftLit();                                   // a reward icon lights up
    void tagShown();                                  // the tag with the count appears
    void filled();                                    // the tower is full
protected:
    void paintEvent(QPaintEvent*) override;
    void timerEvent(QTimerEvent*) override;
    void showEvent(QShowEvent*) override;             // the scene plays while the widget is on screen
    void hideEvent(QHideEvent*) override;
private:
    struct Key {
        int frame = 0; QRectF rect; QPointF pos; QPointF scale{ 1, 1 }; QColor color; QColor fill; bool hasRect = false;
        float rot = 0.f; QPointF origin;              // rotation in degrees about the pivot (relative to the element)
        int gotoFrame = -1; bool play = false;        // an instance keyframe's actions on its area
    };
    struct Elem { QString type, id, material, text, link; int mask = 0, blend = 0; bool filled = false; QVector<Key> keys; };
    struct Area { QString name; int fps = 10; QPointF origin; QVector<Elem> elems; };
    struct State { bool shown = false; QRectF rect; QPointF pos; QPointF scale; QColor color; QColor fill; float rot = 0.f; QPointF origin; };
    // an affine transform, local -> device: (a*x + c*y + tx, b*x + d*y + ty)
    struct Xform {
        float a = 1.f, b = 0.f, c = 0.f, d = 1.f, tx = 0.f, ty = 0.f;
        Xform then(const Xform& inner) const;         // this after inner (inner is applied first)
        Xform inverse() const;
        QPointF map(float x, float y) const { return QPointF(a * x + c * y + tx, b * x + d * y + ty); }
        static Xform translate(float x, float y);
        static Xform scale(float sx, float sy);
        static Xform rotate(float degrees);
        static Xform pivot(const QPointF& origin, float degrees);   // rotation about a point
    };
    struct Stack {                                    // the display stack: mask modes and their depth levels
        float z = 1.f, zprev = 1.f, step = 0.01f;
        int depth = 0;
        int mask[16] = {};
        float zsave[16] = {};
        QColor color{ 255, 255, 255, 255 };
    };
    struct Text { QPointF at; float angle = 0.f; float px = 12.f; QString text; };
    bool load();
    static State at(const Elem& e, float frame);
    int instanceState(const Elem& inst, const Area& linked, float* localFrame, float parentFrame, int depth);
                                                      // 0 hidden, 1 waiting (its area's first frame), 2 playing
    void animate(bool on);
    void render();                                    // the page into m_image
    void drawArea(const QString& areaId, float frame, const Xform& xf, int depth, int first = 0, int last = -1);
    int staticPrefix() const;                         // the root's leading elements that never change
    void drawQuad(const Xform& xf, float w, float h, float z, int maskCode, int blend, const QColor& color, const QRectF* src);
    void setMaskMode(int mode);
    void pushMaskMode();
    void popMaskMode();
    float stackZ() const { return m_stack.mask[m_stack.depth] <= 1 ? m_stack.z : m_stack.zprev; }

    QImage m_sheet;                                   // the sheet, ARGB32
    QHash<QString, QRectF> m_materials;               // material id -> pixel box on the sheet
    QHash<QString, Area> m_areas;
    QString m_root;
    bool m_loaded = false;
    int m_done = 0, m_total = 0;
    float m_target = 0.f, m_frame = 0.f;              // the tower's frame asked for / shown (0..30)
    float m_number = 0.f;                             // the count shown (counts up to 1000)
    bool m_busy = false, m_finished = false, m_ok = false;
    float m_clock = 0.f;                              // seconds, for the instances' own animations
    bool m_rising = false, m_full = false;            // for the signals
    QHash<QString, float> m_started;                  // instance id -> the clock its area started at
    int m_timer = 0;
    QElapsedTimer m_tick;                             // real time between ticks: the pace holds when frames drop
    float m_maxGap = 0.f;                             // the longest gap seen (for the timing log)
    // the frame being rendered
    QImage m_image;
    std::vector<float> m_zbuf;
    Stack m_stack;
    // the static prefix, rendered once for a size: every frame starts from this colour, depth and stack state
    QImage m_base;
    std::vector<float> m_baseZ;
    Stack m_baseStack;
    bool m_baseValid = false;
    QVector<Text> m_texts;                            // text drawn after the pass
};

}  // namespace rgh
