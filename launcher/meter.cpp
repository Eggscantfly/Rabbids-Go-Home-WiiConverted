// meter.cpp - the conversion's meter (see meter.h).
//
// The page is rendered the way the game's menu renderer renders it, element by element in the page's order:
//   * every element is a quad placed by its rectangle, rotated by its keyframe's angle about its pivot; an instance
//     places its area by position, rotation about its pivot and scale, and the area is drawn shifted by the negative
//     of its box's top left corner.
//   * masks are depth levels.  A run of mask rectangles (mask mode 1) is written into a depth buffer at a new level
//     one step below the current one, without colour; an element with mask mode 3 draws only where the buffer holds
//     the level of that run (its depth is the level above, tested "greater"), an element with mask mode 2 draws only
//     outside it (tested "less or equal").  Unmasked elements are tested "less or equal" at the current level, so a
//     run of masks also keeps later unmasked elements off its rectangles until the level is left: that is how the
//     white rectangles around the pipes' outlines keep the splash inside the pipes.
//   * an area instance pushes a level of that stack (with a five-times finer step), moves the current depth back to
//     the level before its mask when it is masked, and restores it when it pops.
//   * the fill is drawn with multiply blending: orange over the pipes' white insides, black outlines left black.
//     Everything else is normal alpha blending, and normal-blended pixels with no alpha are discarded (so masks and
//     sprites do not write depth or colour where they are transparent).
#include "meter.h"

#include <QFile>
#include <QFontMetrics>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPainter>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QHideEvent>
#include <QShowEvent>
#include <QTimerEvent>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

#include "ui.h"

namespace rgh {

namespace {

const int kTowerFrames = 30;                 // the fill rises over these frames of the tower area
const float kCountMax = 1000.f;              // the tag counts to 1000, as in the game
// the composition's extent in the page's own units (854 x 480 screen): the tag at the left, the pipes and icons
const float kLeft = -132.f, kRight = 172.f, kTop = 0.f, kBottom = 420.f;
const float kTagShift = -14.f;                // the tag a little further from the tower than the page puts it
const float kFar = std::numeric_limits<float>::infinity();
const int kBlendMultiply = 20;               // the page's blend modes: 0 normal alpha, 20 multiply
const float kPi = 3.14159265358979f;

QColor abgr(const QString& hex) {            // AABBGGRR, as the menu data stores colours
    bool ok = false;
    unsigned v = hex.toUInt(&ok, 16);
    if (!ok) return QColor(255, 255, 255, 255);
    return QColor(int(v & 0xFF), int((v >> 8) & 0xFF), int((v >> 16) & 0xFF), int((v >> 24) & 0xFF));
}

QRectF rectOf(const QJsonArray& a) {         // [left, right, top, bottom]; a left past the right mirrors the image
    return QRectF(a[0].toDouble(), a[2].toDouble(), a[1].toDouble() - a[0].toDouble(), a[3].toDouble() - a[2].toDouble());
}

float lerp(float a, float b, float t) { return a + (b - a) * t; }

QColor lerpColor(const QColor& a, const QColor& b, float t) {
    return QColor(int(lerp(a.red(), b.red(), t)), int(lerp(a.green(), b.green(), t)), int(lerp(a.blue(), b.blue(), t)),
                  int(lerp(a.alpha(), b.alpha(), t)));
}

QRectF lerpRect(const QRectF& a, const QRectF& b, float t) {
    return QRectF(lerp(a.x(), b.x(), t), lerp(a.y(), b.y(), t), lerp(a.width(), b.width(), t), lerp(a.height(), b.height(), t));
}

QColor modulate(const QColor& a, const QColor& b) {
    return QColor(a.red() * b.red() / 255, a.green() * b.green() / 255, a.blue() * b.blue() / 255, a.alpha() * b.alpha() / 255);
}

}  // namespace

// ---- transforms
JunkMeter::Xform JunkMeter::Xform::then(const Xform& q) const {
    Xform r;
    r.a = a * q.a + c * q.b;
    r.b = b * q.a + d * q.b;
    r.c = a * q.c + c * q.d;
    r.d = b * q.c + d * q.d;
    r.tx = a * q.tx + c * q.ty + tx;
    r.ty = b * q.tx + d * q.ty + ty;
    return r;
}

JunkMeter::Xform JunkMeter::Xform::inverse() const {
    float det = a * d - b * c;
    Xform r;
    if (std::fabs(det) < 1e-12f) return r;
    r.a = d / det; r.b = -b / det; r.c = -c / det; r.d = a / det;
    r.tx = -(r.a * tx + r.c * ty);
    r.ty = -(r.b * tx + r.d * ty);
    return r;
}

JunkMeter::Xform JunkMeter::Xform::translate(float x, float y) { Xform r; r.tx = x; r.ty = y; return r; }
JunkMeter::Xform JunkMeter::Xform::scale(float sx, float sy) { Xform r; r.a = sx; r.d = sy; return r; }

JunkMeter::Xform JunkMeter::Xform::rotate(float degrees) {
    // the page's angles: a negative angle turns the bottom of an element to the right (y grows downwards)
    float t = degrees * kPi / 180.f;
    Xform r;
    r.a = std::cos(t); r.b = std::sin(t); r.c = -std::sin(t); r.d = std::cos(t);
    return r;
}

JunkMeter::Xform JunkMeter::Xform::pivot(const QPointF& o, float degrees) {
    if (std::fabs(degrees) < 1e-6f) return Xform();
    return translate(float(o.x()), float(o.y())).then(rotate(degrees)).then(translate(-float(o.x()), -float(o.y())));
}

JunkMeter::JunkMeter(QWidget* parent) : QWidget(parent) {
    setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Expanding);
    m_loaded = load();
}

bool JunkMeter::load() {
    QFile f(":/meter/layout.json");
    if (!f.open(QIODevice::ReadOnly)) return false;
    QJsonDocument doc = QJsonDocument::fromJson(f.readAll());
    if (!doc.isObject()) return false;
    QJsonObject o = doc.object();
    m_sheet = QImage(":/meter/" + o.value("sheet").toString()).convertToFormat(QImage::Format_ARGB32);
    if (m_sheet.isNull()) return false;
    m_root = o.value("root").toString();
    QJsonObject mats = o.value("materials").toObject();
    for (auto it = mats.begin(); it != mats.end(); ++it) {
        QJsonArray b = it.value().toObject().value("box").toArray();
        m_materials[it.key()] = QRectF(b[0].toDouble(), b[1].toDouble(), b[2].toDouble() - b[0].toDouble(),
                                       b[3].toDouble() - b[1].toDouble());
    }
    QJsonObject areas = o.value("areas").toObject();
    for (auto it = areas.begin(); it != areas.end(); ++it) {
        QJsonObject ao = it.value().toObject();
        Area area;
        area.name = ao.value("name").toString();
        area.fps = ao.value("fps").toInt(10);
        QJsonArray box = ao.value("box").toArray();   // [left, right, top, bottom]
        if (box.size() == 4) area.origin = QPointF(box[0].toDouble(), box[2].toDouble());
        for (const QJsonValue& ev : ao.value("elements").toArray()) {
            QJsonObject eo = ev.toObject();
            Elem e;
            e.type = eo.value("type").toString();
            e.id = eo.value("id").toString();
            e.mask = eo.value("mask").toInt();
            e.blend = eo.value("blend").toInt();
            e.material = eo.value("material").toString();
            e.text = eo.value("text").toString();
            e.link = eo.value("link").toString();
            e.filled = eo.value("filled").toInt() != 0;
            for (const QJsonValue& kv : eo.value("keys").toArray()) {
                QJsonObject ko = kv.toObject();
                Key k;
                k.frame = ko.value("frame").toInt();
                k.color = abgr(ko.value("color").toString("FFFFFFFF"));
                k.fill = abgr(ko.value("fill").toString("FFFFFFFF"));
                k.rot = float(ko.value("rot").toDouble(0.0));
                k.gotoFrame = ko.contains("goto") ? ko.value("goto").toInt() : -1;
                k.play = ko.value("play").toBool(false);
                QJsonArray og = ko.value("origin").toArray();
                if (og.size() == 2) k.origin = QPointF(og[0].toDouble(), og[1].toDouble());
                if (ko.contains("rect")) {
                    k.rect = rectOf(ko.value("rect").toArray());
                    k.hasRect = true;
                }
                if (ko.contains("pos")) {
                    QJsonArray pa = ko.value("pos").toArray();
                    k.pos = QPointF(pa[0].toDouble(), pa[1].toDouble());
                    QJsonArray sa = ko.value("scale").toArray();
                    if (sa.size() == 2) k.scale = QPointF(sa[0].toDouble(1), sa[1].toDouble(1));
                }
                e.keys.push_back(k);
            }
            area.elems.push_back(e);
        }
        m_areas[it.key()] = area;
    }
    return m_areas.contains(m_root);
}

// ---- the public state
void JunkMeter::reset() {
    m_done = m_total = 0;
    m_target = m_frame = m_number = 0.f;
    m_busy = m_finished = m_ok = false;
    m_started.clear();
    if (m_rising) emit fillStopped();
    m_rising = m_full = false;
    update();
}

void JunkMeter::setBusy(bool on) {
    m_busy = on;
    if (on) m_finished = false;
}

void JunkMeter::setProgress(int done, int total, bool instant) {
    m_done = std::max(0, done);
    m_total = std::max(0, total);
    float part = m_total > 0 ? std::min(1.f, float(m_done) / float(m_total)) : 0.f;
    m_target = part * kTowerFrames;
    if (instant) {
        m_frame = m_target;
        m_number = part * kCountMax;
    }
    update();
}

void JunkMeter::setDone(bool ok) {
    m_finished = true;
    m_ok = ok;
    m_busy = false;
    if (ok) {
        m_target = float(kTowerFrames);
        if (m_total > 0) m_done = m_total;
    }
    update();
}

void JunkMeter::animate(bool on) {
    if (on && m_timer == 0) {
        m_timer = startTimer(16, Qt::PreciseTimer);                 // sixty frames a second
        m_tick.restart();
    }
    if (!on && m_timer != 0) {
        killTimer(m_timer);
        m_timer = 0;
    }
}

void JunkMeter::showEvent(QShowEvent*) { animate(true); }
void JunkMeter::hideEvent(QHideEvent*) { animate(false); }

void JunkMeter::timerEvent(QTimerEvent* e) {
    if (e->timerId() != m_timer) return;
    float raw = float(m_tick.restart()) / 1000.f;
    m_maxGap = std::max(m_maxGap, raw);
    float dt = std::clamp(raw, 0.001f, 0.1f);
    m_clock += dt;
    // the fill rises at the game's pace (its area runs at 10 frames a second: three seconds for the whole tower)
    float step = 10.f * dt;
    if (m_frame < m_target) m_frame = std::min(m_target, m_frame + step);
    else if (m_frame > m_target) m_frame = m_target;
    m_number = m_frame / kTowerFrames * kCountMax;                  // the count rises with the fill, as in the game
    bool moving = m_frame < m_target;
    bool rising = m_frame < m_target;
    if (rising && !m_rising) emit fillStarted();
    if (rising) emit fillLevel(m_frame / kTowerFrames);
    if (!rising && m_rising) emit fillStopped();
    m_rising = rising;
    if (m_frame >= float(kTowerFrames) - 0.01f && !m_full) {
        m_full = true;
        emit filled();
    }
    (void)moving;
    update();                                                       // the splash keeps playing while on screen
}

// ---- the keyframes
JunkMeter::State JunkMeter::at(const Elem& e, float frame) {
    State s;
    if (e.keys.isEmpty() || frame < float(e.keys.first().frame)) return s;   // not there before its first keyframe
    s.shown = true;
    const Key* a = &e.keys.first();
    const Key* b = a;
    for (int i = 0; i < e.keys.size(); ++i) {
        if (float(e.keys[i].frame) <= frame) {
            a = &e.keys[i];
            b = (i + 1 < e.keys.size()) ? &e.keys[i + 1] : a;
        }
    }
    float t = (b == a || b->frame == a->frame) ? 0.f : (frame - float(a->frame)) / float(b->frame - a->frame);
    t = std::clamp(t, 0.f, 1.f);
    s.rect = a->hasRect ? lerpRect(a->rect, b->rect, t) : QRectF();
    s.pos = QPointF(lerp(a->pos.x(), b->pos.x(), t), lerp(a->pos.y(), b->pos.y(), t));
    s.scale = QPointF(lerp(a->scale.x(), b->scale.x(), t), lerp(a->scale.y(), b->scale.y(), t));
    s.color = lerpColor(a->color, b->color, t);
    s.fill = lerpColor(a->fill, b->fill, t);
    s.rot = lerp(a->rot, b->rot, t);
    s.origin = QPointF(lerp(a->origin.x(), b->origin.x(), t), lerp(a->origin.y(), b->origin.y(), t));
    return s;
}

int JunkMeter::instanceState(const Elem& inst, const Area& linked, float* localFrame, float parentFrame, int depth) {
    int last = 0;
    for (const Elem& e : linked.elems) for (const Key& k : e.keys) last = std::max(last, k.frame);
    if (depth > 0) {
        // an instance inside a playing area (the mirrored half of the splash) runs in step with it
        *localFrame = std::min(std::max(parentFrame - float(inst.keys.first().frame), 0.f), float(last));
        return 2;
    }
    // an instance starts its area when the page reaches the keyframe whose actions send the area to a frame and
    // play it (the icons jump straight to their burst as the fill passes them); before that it shows the area's
    // first frame if it stays in place, nothing if it moves (the splash, the tag)
    bool still = true;
    for (const Key& k : inst.keys) if (k.pos != inst.keys.first().pos) still = false;
    float trigger = float(inst.keys.first().frame);
    int startAt = 0;
    for (const Key& k : inst.keys) {
        if (k.gotoFrame >= 0 || k.play) {
            trigger = float(k.frame);
            startAt = std::max(0, k.gotoFrame);
            break;
        }
    }
    // the tag opens with the count from the very start: a conversion takes minutes, and the page's own fade-in
    // over the first sixth of the fill would keep the count out of sight for too long
    if (linked.name == "tag") trigger = float(inst.keys.first().frame);
    if (m_frame + 0.01f < trigger) {
        m_started.remove(inst.id);
        *localFrame = 0.f;
        return still ? 1 : 0;
    }
    if (!m_started.contains(inst.id)) {
        m_started[inst.id] = m_clock;
        if (linked.name == "tag") emit tagShown();
        else if (linked.name == "silhouette") emit giftLit();     // the icon lights up now: its ring with it
    }
    float f = float(startAt) + (m_clock - m_started[inst.id]) * float(linked.fps);
    bool loops = linked.name.startsWith("sparkle");
    if (loops) {
        f = last > 0 ? std::fmod(f, float(last)) : 0.f;            // its last keyframe sends it back to 0: it plays on
    } else if (linked.name == "tag") {
        f = std::min(f, 20.f);                                      // the tag opens and stays open (its data closes it again at 24)
    } else {
        f = std::min(f, float(last));
    }
    *localFrame = f;
    return 2;
}

// ---- the display stack (mask modes and depth levels), as the game keeps it
void JunkMeter::setMaskMode(int mode) {
    Stack& s = m_stack;
    int cur = s.mask[s.depth];
    if (mode == cur) return;
    if ((mode == 2 || mode == 3) && cur == 0) return;               // nothing to be masked by
    s.mask[s.depth] = mode;
    if (mode == 1) {                                                // a new run of masks: one level down
        s.zprev = s.z;
        s.z = s.z - s.step;
    }
}

void JunkMeter::pushMaskMode() {
    Stack& s = m_stack;
    if (s.depth >= 15) return;
    s.step *= 0.2f;
    int m = s.mask[s.depth];
    if (m == 2 || m == 3) {
        s.zsave[s.depth] = s.z;
        s.z = s.zprev;
    } else if (m == 0) {
        s.zsave[s.depth] = s.z;
    }
    s.mask[s.depth + 1] = s.mask[s.depth];
    s.depth++;
}

void JunkMeter::popMaskMode() {
    Stack& s = m_stack;
    if (s.depth <= 0) return;
    s.depth--;
    s.step /= 0.2f;
    int m = s.mask[s.depth];
    if (m == 2 || m == 3) {
        s.zprev = s.z;
        s.z = s.zsave[s.depth];
    } else if (m == 0) {
        s.zprev = s.z;
    }
}

// ---- the rasterizer
void JunkMeter::drawQuad(const Xform& xf, float w, float h, float z, int maskCode, int blend, const QColor& color, const QRectF* src) {
    // xf: the element's local space (its rectangle from (0,0) to (w,h), w or h negative for a mirrored image) to
    // device pixels; src: the sprite's pixel box on the sheet, or null for a plain colour
    if (w == 0.f || h == 0.f) return;
    QPointF c0 = xf.map(0, 0), c1 = xf.map(w, 0), c2 = xf.map(w, h), c3 = xf.map(0, h);
    float xa = float(std::min({ c0.x(), c1.x(), c2.x(), c3.x() })), xb = float(std::max({ c0.x(), c1.x(), c2.x(), c3.x() }));
    float ya = float(std::min({ c0.y(), c1.y(), c2.y(), c3.y() })), yb = float(std::max({ c0.y(), c1.y(), c2.y(), c3.y() }));
    int W = m_image.width(), H = m_image.height();
    int px0 = std::max(0, int(std::ceil(xa - 0.5f))), px1 = std::min(W, int(std::ceil(xb - 0.5f)));
    int py0 = std::max(0, int(std::ceil(ya - 0.5f))), py1 = std::min(H, int(std::ceil(yb - 0.5f)));
    if (px0 >= px1 || py0 >= py1) return;
    Xform inv = xf.inverse();
    float cr = color.redF(), cg = color.greenF(), cb = color.blueF(), ca = color.alphaF();
    bool writeColor = maskCode != 4;
    bool writeZ = maskCode == 4;
    const int sw = m_sheet.width(), sh = m_sheet.height();
    float sx0 = 0, sy0 = 0, sww = 0, shh = 0;
    if (src) {
        sx0 = float(src->left()); sy0 = float(src->top()); sww = float(src->width()); shh = float(src->height());
    }
    auto texel = [&](int x, int y, float& tr, float& tg, float& tb, float& ta) {
        x = std::clamp(x, int(sx0), int(sx0 + sww) - 1);
        y = std::clamp(y, int(sy0), int(sy0 + shh) - 1);
        x = std::clamp(x, 0, sw - 1);
        y = std::clamp(y, 0, sh - 1);
        QRgb p = reinterpret_cast<const QRgb*>(m_sheet.constScanLine(y))[x];
        tr = qRed(p) / 255.f; tg = qGreen(p) / 255.f; tb = qBlue(p) / 255.f; ta = qAlpha(p) / 255.f;
    };
    for (int py = py0; py < py1; ++py) {
        QRgb* row = reinterpret_cast<QRgb*>(m_image.scanLine(py));
        float* zrow = &m_zbuf[size_t(py) * size_t(W)];
        for (int px = px0; px < px1; ++px) {
            // the pixel's centre back in the element's space
            float dx = px + 0.5f, dy = py + 0.5f;
            float lx = inv.a * dx + inv.c * dy + inv.tx, ly = inv.b * dx + inv.d * dy + inv.ty;
            float u = lx / w, v = ly / h;
            if (u < 0.f || u >= 1.f || v < 0.f || v >= 1.f) continue;
            float r = cr, g = cg, b = cb, a = ca;
            if (src) {
                // bilinear sample of the sprite (texel centres at half pixels), clamped to its box; the alpha weighs
                // the colours so transparent texels do not darken their neighbours
                float fx = sx0 + u * sww - 0.5f, fy = sy0 + v * shh - 0.5f;
                int ix = int(std::floor(fx)), iy = int(std::floor(fy));
                float tx = fx - ix, ty = fy - iy;
                float r00, g00, b00, a00, r10, g10, b10, a10, r01, g01, b01, a01, r11, g11, b11, a11;
                texel(ix, iy, r00, g00, b00, a00);
                texel(ix + 1, iy, r10, g10, b10, a10);
                texel(ix, iy + 1, r01, g01, b01, a01);
                texel(ix + 1, iy + 1, r11, g11, b11, a11);
                float w00 = (1 - tx) * (1 - ty), w10 = tx * (1 - ty), w01 = (1 - tx) * ty, w11 = tx * ty;
                float ta = a00 * w00 + a10 * w10 + a01 * w01 + a11 * w11;
                float tr = 0, tg = 0, tb = 0;
                if (ta > 0.f) {
                    tr = (r00 * a00 * w00 + r10 * a10 * w10 + r01 * a01 * w01 + r11 * a11 * w11) / ta;
                    tg = (g00 * a00 * w00 + g10 * a10 * w10 + g01 * a01 * w01 + g11 * a11 * w11) / ta;
                    tb = (b00 * a00 * w00 + b10 * a10 * w10 + b01 * a01 * w01 + b11 * a11 * w11) / ta;
                }
                r *= tr; g *= tg; b *= tb; a *= ta;
            }
            if (blend != kBlendMultiply && a <= 0.002f) continue;   // the alpha test of normal blending
            float zb = zrow[px];
            bool pass = (maskCode == 2) ? (z > zb)                  // masked: inside the latest run of masks
                                        : (z <= zb);                // unmasked, inverse masked, mask writers
            if (!pass) continue;
            if (writeZ) zrow[px] = z;
            if (!writeColor) continue;
            QRgb d = row[px];
            float dr = qRed(d) / 255.f, dg = qGreen(d) / 255.f, db = qBlue(d) / 255.f;
            float orr, og, ob;
            if (blend == kBlendMultiply) {
                orr = dr * r; og = dg * g; ob = db * b;
            } else {
                orr = r * a + dr * (1 - a); og = g * a + dg * (1 - a); ob = b * a + db * (1 - a);
            }
            row[px] = qRgb(int(std::clamp(orr, 0.f, 1.f) * 255.f + 0.5f), int(std::clamp(og, 0.f, 1.f) * 255.f + 0.5f),
                           int(std::clamp(ob, 0.f, 1.f) * 255.f + 0.5f));
        }
    }
}

int JunkMeter::staticPrefix() const {
    // the root's elements up to the first one whose look depends on the frame (several keyframes, an instance, a
    // text): the toilet, the pipes and the mask rectangles before the fill
    if (!m_areas.contains(m_root)) return 0;
    const Area& area = m_areas[m_root];
    int n = 0;
    for (const Elem& e : area.elems) {
        bool fixed = e.type == "Placeholder" || ((e.type == "Image" || e.type == "RectShape") && e.keys.size() == 1 && e.keys.first().frame == 0);
        if (!fixed) break;
        ++n;
    }
    return n;
}

void JunkMeter::drawArea(const QString& areaId, float frame, const Xform& xf, int depth, int first, int last) {
    if (depth > 4 || !m_areas.contains(areaId)) return;
    const Area& area = m_areas[areaId];
    Xform tagImage;                                                 // the tag's arrow, for the count drawn on it
    float tagW = 0.f, tagH = 0.f;
    int end = last < 0 ? area.elems.size() : std::min(last, int(area.elems.size()));
    for (int index = std::max(0, first); index < end; ++index) {
        const Elem& e = area.elems[index];
        if (e.type == "Placeholder") continue;
        State s = at(e, frame);
        if (!s.shown) continue;
        setMaskMode(e.mask);                                        // every element sets its own mode
        int code = 0;                                               // the renderer's code for the current mode
        switch (m_stack.mask[m_stack.depth]) {
        case 1: code = 4; break;
        case 2: code = 1; break;
        case 3: code = 2; break;
        default: code = 0; break;
        }
        float z = stackZ();
        QColor color = modulate(s.color, m_stack.color);
        // the element's space: its rectangle's top left, then its rotation about its pivot
        Xform local = xf.then(Xform::translate(float(s.rect.left()), float(s.rect.top()))).then(Xform::pivot(s.origin, s.rot));
        if (e.type == "Image" && m_materials.contains(e.material)) {
            if (color.alpha() == 0) continue;
            QRectF src = m_materials[e.material];
            drawQuad(local, float(s.rect.width()), float(s.rect.height()), z, code, e.blend, color, &src);
            if (area.name == "tag") {
                tagImage = local;
                tagW = float(s.rect.width());
                tagH = float(s.rect.height());
            }
        } else if (e.type == "RectShape") {
            if (!e.filled) continue;
            QColor fill = modulate(s.fill, m_stack.color);
            if (fill.alpha() == 0) continue;
            drawQuad(local, float(s.rect.width()), float(s.rect.height()), z, code, e.blend, fill, nullptr);
        } else if (e.type == "Text") {
            if (color.alpha() == 0) continue;
            Text t;
            t.text = area.name == "tag" ? QString::number(int(m_number + 0.5f)) : e.text;
            float scale = std::sqrt(std::fabs(local.a * local.d - local.b * local.c));
            t.px = 0.62f * std::fabs(float(s.rect.height())) * scale;
            if (area.name == "tag" && tagW != 0.f) {
                // the count sits in the middle of the arrow (the tip takes the right end of the sprite)
                t.at = tagImage.map(tagW * 0.47f, tagH * 0.5f);
                t.angle = std::atan2(tagImage.b, tagImage.a) * 180.f / kPi;
            } else {
                t.at = local.map(float(s.rect.width()) / 2.f, float(s.rect.height()) / 2.f);
                t.angle = std::atan2(local.b, local.a) * 180.f / kPi;
            }
            m_texts.push_back(t);
        } else if (e.type == "AreaInstance" && m_areas.contains(e.link)) {
            const Area& child = m_areas[e.link];
            if (child.name == "tag") {                               // shown at once (see instanceState)
                s.color.setAlpha(255);
                color = modulate(s.color, m_stack.color);
            }
            if (color.alpha() == 0) continue;                       // an invisible instance is not drawn at all
            float local2 = 0.f;
            if (instanceState(e, m_areas[e.link], &local2, frame, depth) == 0) continue;
            // translate(pos), rotate about the pivot, scale, then the area's own origin
            float shift = child.name == "tag" ? kTagShift : 0.f;
            Xform cx = xf.then(Xform::translate(float(s.pos.x()) + shift, float(s.pos.y())))
                         .then(Xform::pivot(s.origin, s.rot))
                         .then(Xform::scale(float(s.scale.x()), float(s.scale.y())))
                         .then(Xform::translate(-float(child.origin.x()), -float(child.origin.y())));
            pushMaskMode();
            QColor saved = m_stack.color;
            m_stack.color = modulate(saved, s.color);
            drawArea(e.link, local2, cx, depth + 1);
            m_stack.color = saved;
            popMaskMode();
        }
    }
}

void JunkMeter::render() {
    static const bool timing = qEnvironmentVariableIsSet("RGHPORT_METER_TIMING");   // development: frame times
    QElapsedTimer clock;
    if (timing) clock.start();
    qreal dpr = devicePixelRatioF();
    int W = std::max(1, int(width() * dpr)), H = std::max(1, int(height() * dpr));
    if (m_image.width() != W || m_image.height() != H) {
        m_image = QImage(W, H, QImage::Format_RGB32);
        m_zbuf.assign(size_t(W) * size_t(H), kFar);
        m_baseValid = false;
    }
    m_image.setDevicePixelRatio(dpr);
    // the page's units fitted into the widget, the whole composition kept in view
    float sx = W / (kRight - kLeft), sy = H / (kBottom - kTop);
    float scale = std::min({ sx, sy, 1.6f * float(dpr) });
    Xform xf = Xform::translate((W - (kRight - kLeft) * scale) / 2.f - kLeft * scale,
                                (H - (kBottom - kTop) * scale) / 2.f - kTop * scale).then(Xform::scale(scale, scale));
    int prefix = staticPrefix();
    if (!m_baseValid) {
        // the static prefix once: its colour, depth and mask state are the start of every frame
        std::fill(m_zbuf.begin(), m_zbuf.end(), kFar);
        m_image.fill(palette().color(QPalette::Window));
        m_texts.clear();
        m_stack = Stack();
        drawArea(m_root, 0.f, xf, 0, 0, prefix);
        m_base = m_image.copy();
        m_baseZ = m_zbuf;
        m_baseStack = m_stack;
        m_baseValid = true;
    }
    std::memcpy(m_image.bits(), m_base.constBits(), size_t(m_image.sizeInBytes()));
    std::copy(m_baseZ.begin(), m_baseZ.end(), m_zbuf.begin());
    m_stack = m_baseStack;
    m_texts.clear();
    drawArea(m_root, m_frame, xf, 0, prefix, -1);
    if (!m_texts.isEmpty()) {
        QPainter p(&m_image);
        p.setRenderHint(QPainter::Antialiasing, true);
        p.setRenderHint(QPainter::TextAntialiasing, true);
        for (const Text& t : m_texts) {
            QFont font = HeadFont(std::max(8, int(t.px / dpr)));
            QFontMetrics fm(font);
            p.save();
            p.translate(t.at.x() / dpr, t.at.y() / dpr);
            p.rotate(t.angle);
            p.setFont(font);
            p.setPen(Qt::white);
            QRectF box(-200, -100, 400, 200);
            p.drawText(box, Qt::AlignCenter, t.text);
            p.restore();
        }
    }
    if (timing) {
        static int frames = 0;
        static double total = 0.0;
        total += clock.nsecsElapsed() / 1e6;
        if (++frames % 30 == 0) {
            QFile f(QDir::tempPath() + "/rghport_meter_timing.txt");
            if (f.open(QIODevice::Append | QIODevice::Text))
                f.write(QString("%1 frames, %2 ms each (%3 x %4), longest gap between ticks %5 ms").arg(frames).arg(total / frames, 0, 'f', 2).arg(W).arg(H).arg(int(m_maxGap * 1000)).toUtf8() + QByteArray(1, 10));
            m_maxGap = 0.f;
        }
    }
}

void JunkMeter::paintEvent(QPaintEvent*) {
    QPainter p(this);
    if (!m_loaded) {
        p.setPen(QColor("#A8A8A8"));
        p.setFont(BodyFont(13));
        p.drawText(rect(), Qt::AlignCenter, "the meter's sprites are missing from the build");
        return;
    }
    render();
    p.drawImage(QPointF(0, 0), m_image);
}

}  // namespace rgh
