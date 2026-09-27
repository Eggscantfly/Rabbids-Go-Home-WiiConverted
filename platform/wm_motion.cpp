// wm_motion.cpp - the Wii remote's motion in controls mode pc: the accelerometer samples and the horizon that the
// Wii-only scripts read, built once per frame from the Options bindings (wm_options.cpp).
//
// Two parts of the Wii version need the remote's motion and have no PC replacement:
//   * the controls check that opens a new game (IZW_TRC, CTRC_STATE_UPDATE): shake the Nunchuk and the remote (the
//     length of the accelerometer vector above 3 g), turn the remote over (CWII_LIB_GetJoyBanking (-x, z, -y) within
//     11 degrees of (0, 0, -1)), press A;
//   * Inside Zee Wiimote, whose rabbid reacts to shakes and to the remote's roll.  WII_LIB_MOVE (list 60000C18) keeps
//     the last 6 extremums of the accelerometer samples per axis and averages them: a sideways (x), up and down (y),
//     along the remote (z, a maraca) or circular (x and y) shake of 3.5 to 10 g after averaging, 0.5 to 10 Hz, is an
//     immediate move; it stays the "smart" move the rabbid follows while a new extremum comes every 0.2 s.
// wm_ctl.cpp answers IO_JoystickAccelGet, IO_JoystickAccelSampleGet, IO_JoystickSampleNumberGet and
// IO_JoystickHorizonGet from this module:
//   SCREAM (the attack)   the remote and the Nunchuk shake up and down
//   SHAKE LEFT/RIGHT      the remote shakes along its x axis (default binding: the mouse wiggled left and right)
//   SHAKE UP/DOWN         along its y axis (the mouse wiggled up and down)
//   MARACA SHAKE          along its length, z (the scroll wheel)
//   SHAKE CLOCKWISE / COUNTER-CW   a circle in the plane facing the screen: x and y in quadrature, WII_LIB_MOVE's
//                         rotations 4 / 5 (the mouse moved in circles)
//   TILT LEFT / RIGHT     the remote rolls 90 degrees while held (both: it turns over), and back on release; a click
//                         shorter than 0.1 s does not roll it
// A shake is a 3.4 g sine (6.8 g peak to peak, 8 to 9 g after WII_LIB_MOVE's averaging) at 5 Hz, or at the rhythm of the
// mouse wiggle or the wheel that drives it, kept between 3.5 and 7 Hz: slower, the extremums come further apart than
// the 0.2 s the smart move needs.  Every frame carries samples at 200 Hz like the remote, newest first.
//
// KPAD frame (wm_input.cpp): at rest, buttons up, gravity reads (0, -1, 0); a roll r reads (-sin r, -cos r, 0) and
// the horizon is (cos r, sin r); a positive roll turns the remote to the right.  So +x is the remote's left side, +y
// its button face, and a linear acceleration a reads -a (the sensor reports gravity minus acceleration).  A remote
// stirred clockwise as the player sees it, position R (sin wt, cos wt) in (right, up), accelerates by
// -w^2 R (sin wt, cos wt): it reads x = -A sin wt, y = -1 + A cos wt; the other way round x = +A sin wt.
#include "wiimote.h"

#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>

using namespace wmpatch;

namespace {

const double kTwoPi = 6.283185307179586;
const double kPi = 3.141592653589793;

const float  SHAKE_G        = 3.4f;             // above the 3 g some scripts want of one axis (the paint page's sticker shake)
const double SHAKE_HZ       = 5.0;
const double RHYTHM_HZ_MIN  = 3.5, RHYTHM_HZ_MAX = 7.0;
const double ENV_ATTACK     = 0.06;             // seconds from still to a full shake
const double ENV_RELEASE    = 0.12;
const double TILT_RATE      = 450.0 * kPi / 180.0;
const double TILT_DELAY     = 0.1;
const double SAMPLE_HZ      = 200.0;
enum { MAX_SAMPLES = 12 };

struct Osc {
    double phase;           // radians, at the end of the frame
    double hz;
    float  env, envStart;   // 0..1 at the end / start of the frame
};

struct State {
    bool   started;
    double t, dt;
    int    samples;
    Osc    axis[MOTION_AXES];
    Osc    attack;
    Osc    rot;             // the circular shake
    int    rotDir;          // 1 clockwise, -1
    double roll, rollStart; // radians
    double leftSince, rightSince;
    bool   noPad;
    float  move[2];
    double polled;          // time of the last accelerometer read, 0 = never
};

State s_st;

float Approach(float v, float target, double dt, double secs) {
    float step = secs > 0.0 ? (float)(dt / secs) : 1.0f;
    if (v < target) return v + step > target ? target : v + step;
    return v - step < target ? target : v - step;
}

void Advance(Osc& o, bool want, double hz, double dt) {
    o.envStart = o.env;
    if (want) {
        if (o.env <= 0.0f) o.phase = 0.0;       // a new shake starts from the rest point
        double f = hz > 0.0 ? hz : SHAKE_HZ;
        o.hz = f < RHYTHM_HZ_MIN ? RHYTHM_HZ_MIN : f > RHYTHM_HZ_MAX ? RHYTHM_HZ_MAX : f;
    } else if (o.hz <= 0.0) {
        o.hz = SHAKE_HZ;
    }
    o.phase = fmod(o.phase + kTwoPi * o.hz * dt, kTwoPi);
    o.env = Approach(o.env, want ? 1.0f : 0.0f, dt, want ? ENV_ATTACK : ENV_RELEASE);
}

void Tilt(State& s, const MotionInput& in, double dt) {
    s.rollStart = s.roll;
    bool l = in.on && in.tiltLeft, r = in.on && in.tiltRight;
    if (!l) s.leftSince = -1.0;
    else if (s.leftSince < 0.0) s.leftSince = in.t;
    if (!r) s.rightSince = -1.0;
    else if (s.rightSince < 0.0) s.rightSince = in.t;
    bool lOn = l && in.t - s.leftSince >= TILT_DELAY, rOn = r && in.t - s.rightSince >= TILT_DELAY;
    double target = 0.0;
    if (lOn && rOn) target = s.roll < 0.0 ? -kPi : kPi;
    else if (lOn) target = -kPi / 2.0;
    else if (rOn) target = kPi / 2.0;
    target += kTwoPi * floor((s.roll - target) / kTwoPi + 0.5);    // the same orientation nearest to the roll
    double step = TILT_RATE * dt, diff = target - s.roll;
    s.roll += diff > step ? step : diff < -step ? -step : diff;
    if (s.roll > kPi + 1e-6 || s.roll < -kPi - 1e-6) {              // back into (-pi, pi], the start with it
        double shift = kTwoPi * floor((s.roll + kPi) / kTwoPi);
        s.roll -= shift;
        s.rollStart -= shift;
    }
}

float Value(const Osc& o, double back, float w) {
    float env = o.env + (o.envStart - o.env) * w;
    if (env <= 0.0f) return 0.0f;
    return SHAKE_G * env * (float)sin(o.phase - kTwoPi * o.hz * back);
}

// ---------------------------------------------------------------------------------------------------------------
// self-test (wmtest motion): the frames above, and WII_LIB_MOVE as the port's scripts run it (work/pcport/scr/
// wii_fill3/_shared/WII_LIB_MOVE.fcl with the list's initial values and CWII_LIB_SetShakeAssistant's parameters)
// ---------------------------------------------------------------------------------------------------------------
struct Ext { float t, v, o; };
struct MinMax { float t, mn, mx; };
struct Axis {
    Ext    ext[6];
    int    first, last, size;
    MinMax mm[50];
    int    mmFirst, mmLast, mmSize;
    float  detectTime, avgAmp, avgPer;
};
struct Params { float maxAmp, minAmp, minPer, maxPer, diffAmp, diffPer; };

float Sign(float x) { return x >= 0.0f ? 1.0f : -1.0f; }        // MTH_FloatSign_C (fsel)
float Limit(float x, float lo, float hi) { return x < lo ? lo : x > hi ? hi : x; }
float Blend(float a, float b, float t) { return t >= 1.0f ? b : t < 0.0f ? a : a * (1.0f - t) + b * t; }
int   IntMin(int a, int b) { return a < b ? a : b; }

struct LibMove {
    // the list's initial values (Wii record 60000C18)
    int    level = 2;
    float  maxLife = 4.0f, ampMin = 0.5f, perMin = 1e-5f, attMin = 0.3f, attMax = 1.2f;
    float  idleTime = 0.2f, idleDiff = 0.4f, smoothFactor = 10.0f, noiseDur = 0.0f, noiseMax = 2.5f;
    float  idleToNext = 0.0f, curToNext = 0.0f, idleMin = 0.01f, maxIdleAmp = 0.3f;
    Axis   ax[3];
    float  prevVal[3], prevTime[3];
    Params rot, hor, dep, ver;
    int    immediate = 0, smart = 0, smartNext = 0, idleMode = 0;
    float  smartUpdate = 0.0f, idleUpdate = 0.0f;
    float  now = 0.0f, dt = 0.0f;

    void SetParams(Params& p, float a0, float a1, float a2, float a3, float a4, float a5) {
        p.minAmp = a0; p.maxAmp = a1; p.minPer = 1.0f / a3; p.maxPer = 1.0f / a2; p.diffAmp = a4;
        p.diffPer = a5 != 0.0f ? 1.0f / a5 : 0.0f;
    }

    void Init(float t) {
        now = t;
        for (int a = 0; a < 3; ++a) {
            Axis& x = ax[a];
            memset(&x, 0, sizeof(x));
            x.size = 1;
            x.ext[0].t = t;
            x.ext[0].o = 1.0f;
            prevVal[a] = prevTime[a] = 0.0f;
        }
        SetParams(dep, 4.5f, 10.0f, 0.5f, 10.0f, 1.5f, 0.0f);       // CWII_LIB_SetShakeAssistant, enter mode 1
        SetParams(hor, 3.5f, 10.0f, 0.5f, 10.0f, 0.0f, 0.0f);
        SetParams(ver, 3.5f, 10.0f, 0.5f, 10.0f, 0.0f, 0.0f);
        SetParams(rot, 3.5f, 10.0f, 0.5f, 10.0f, 5.0f, 2.0f);
    }

    bool Noise(const Axis& x) const {
        return (now - x.detectTime) < noiseDur && x.avgPer < 1.0f / (noiseMax > 0.001f ? noiseMax : 0.001f);
    }

    void Commit(Axis& x, float value, float time) {
        float sign = Sign(value - x.ext[x.last].v);
        if (!(x.ext[x.last].o == sign && x.size >= 1)) {
            x.last = (x.last + 1) % 6;
            x.size = IntMin(x.size + 1, 6);
            x.first = (x.last + (7 - x.size)) % 6;
        }
        x.ext[x.last].t = time;
        x.ext[x.last].v = value;
        x.ext[x.last].o = sign;
    }

    bool DetectsExtremum(Axis& x, float prev, float prevT, float cand) {
        if (!(x.ext[x.last].o == Sign(cand - prev))) return false;
        float amp = x.size > 1 ? fabs(x.ext[x.last].v - prev) : 0.0f;
        float per = x.size > 2 ? prevT - x.ext[(x.last + 5) % 6].t : 0.0f;
        if (!(x.size < 3 || (amp > ampMin && per > perMin))) return false;
        Commit(x, prev, prevT);
        return true;
    }

    static void Track(const Axis& x, float s, float t, float& cand, float& candT) {
        float o = x.ext[x.last].o;
        if ((o > 0.0f && s < cand) || (o < 0.0f && s > cand)) {
            cand = s;
            candT = t;
        }
    }

    void ParseSamples() {
        int n = MotionSampleCount();
        if (n <= 0) return;
        float step = dt / (float)n;
        float v[3], cand[3], candT[3], mn[3], mx[3];
        MotionAccel(0, n - 1, v);
        float t0 = now - (float)(n - 1) * step;
        float first[3] = { -v[0], v[2], v[1] };
        for (int a = 0; a < 3; ++a) cand[a] = mn[a] = mx[a] = first[a], candT[a] = t0;
        for (int i = 0; i < n; ++i) {
            float t = now - (float)i * step;
            MotionAccel(0, i, v);
            float s[3] = { -v[0], v[2], v[1] };
            for (int a = 0; a < 3; ++a) {
                Track(ax[a], s[a], t, cand[a], candT[a]);
                if (s[a] < mn[a]) mn[a] = s[a];
                if (s[a] > mx[a]) mx[a] = s[a];
            }
        }
        for (int a = 0; a < 3; ++a) {
            Axis& x = ax[a];
            x.mmLast = (x.mmLast + 1) % 50;
            x.mmSize = IntMin(x.mmSize + 1, 50);
            x.mmFirst = (x.mmLast + (51 - x.mmSize)) % 50;
            x.mm[x.mmLast].t = now;
            x.mm[x.mmLast].mn = mn[a];
            x.mm[x.mmLast].mx = mx[a];
        }
        bool e[3];
        for (int a = 0; a < 3; ++a) e[a] = DetectsExtremum(ax[a], prevVal[a], prevTime[a], cand[a]);
        if (e[0] || e[1] || e[2]) {
            for (int i = 0; i < n; ++i) {
                float t = now - (float)i * step;
                MotionAccel(0, i, v);
                float s[3] = { -v[0], v[2], v[1] };
                for (int a = 0; a < 3; ++a)
                    if (e[a]) Track(ax[a], s[a], t, cand[a], candT[a]);
            }
        }
        for (int a = 0; a < 3; ++a) prevVal[a] = cand[a], prevTime[a] = candT[a];
    }

    void RemoveDead(Axis& x) {
        int first = x.first;
        for (int j = 0, stop = 0; !stop && j < x.size; ++j) {
            if (now - x.ext[(first + j) % 6].t > maxLife) {
                x.size--;
                x.first = (x.first + 1) % 6;
            } else {
                stop = 1;
            }
        }
        int mfirst = x.mmFirst;
        for (int j = 0, stop = 0; !stop && j < x.mmSize; ++j) {
            if (now - x.mm[(mfirst + j) % 50].t > idleTime) {
                x.mmSize--;
                x.mmFirst = (x.mmFirst + 1) % 50;
            } else {
                stop = 1;
            }
        }
    }

    bool Idle(const Axis& x) const {
        if (x.mmSize <= 0) return true;
        float mn = x.mm[x.mmFirst].mn, mx = x.mm[x.mmFirst].mx;
        for (int left = x.mmSize - 1, i = (x.mmFirst + 1) % 50; left > 0; --left, i = (i + 1) % 50) {
            if (x.mm[i].mn < mn) mn = x.mm[i].mn;
            if (x.mm[i].mx > mx) mx = x.mm[i].mx;
        }
        return !(fabs(mx - mn) > idleDiff);
    }

    void Average(Axis& x) {
        float sumAmp = 0.0f, sumPer = 0.0f;
        int count = 0, ok = 0;
        if (Idle(x)) {
            x.size = 0;
            x.first = (x.last + 6) % 6;
        }
        if (x.size > 2) {
            for (int i = 0; i < x.size - 2; ++i) {
                int a = (x.first + i) % 6, b = (a + 1) % 6, c = (b + 1) % 6;
                float amp = fabs(x.ext[b].v - x.ext[a].v), per = x.ext[c].t - x.ext[a].t;
                if (amp > ampMin && per > perMin) {
                    float age = now - x.ext[b].t;
                    float att = Blend(1.0f, 0.0f, Limit((age - attMin) / (attMax - attMin), 0.0f, 1.0f));
                    amp *= att;
                    per /= Limit(att, 0.1f, 1.0f);
                    ++count;
                    sumAmp += amp;
                    sumPer += per;
                }
            }
            if (count > 2) {
                ok = 1;
                x.avgAmp = sumAmp / (float)(count - 1);
                x.avgPer = sumPer / (float)(count - 2);
            }
        }
        if (!ok) {
            x.avgAmp = x.avgPer = 0.0f;
            x.detectTime = now;
        }
    }

    static bool In(float v, float lo, float hi) { return v >= lo && v <= hi; }

    void Immediate() {
        Axis& X = ax[0], &Y = ax[1], &Z = ax[2];
        immediate = 0;
        if (!Noise(X) && !Noise(Z) && In(X.avgAmp, rot.minAmp, rot.maxAmp) && In(Z.avgAmp, rot.minAmp, rot.maxAmp) &&
            Y.avgAmp < X.avgAmp / 1.5f && fabs(X.avgAmp - Z.avgAmp) < rot.diffAmp &&
            fabs(X.avgPer - Z.avgPer) < rot.diffPer) {
            int k = (X.last + 5) % 6;
            float o = X.ext[k].o, tx = X.ext[k].t, best = 10000.0f, bestT = 0.0f;
            for (int i = 0; i < X.size; ++i) {
                int k2 = (X.first + i) % 6;
                if (o == Z.ext[k2].o && fabs(tx - Z.ext[k2].t) < best) {
                    best = fabs(tx - Z.ext[k2].t);
                    bestT = Z.ext[k2].t;
                }
            }
            float per = X.avgPer / 7.0f;
            if (bestT >= 0.0f && best > 0.1f * per && best < 3.0f * per) immediate = tx > bestT ? 4 : 5;
        }
        if (immediate != 0) return;
        if (!Noise(Y) && Y.avgAmp >= X.avgAmp - dep.diffAmp && Y.avgAmp >= Z.avgAmp - dep.diffAmp &&
            In(Y.avgAmp, dep.minAmp, dep.maxAmp) && In(Y.avgPer, dep.minPer, dep.maxPer))
            immediate = 3;
        else if (!Noise(X) && X.avgAmp >= Y.avgAmp && X.avgAmp >= Z.avgAmp && In(X.avgAmp, hor.minAmp, hor.maxAmp) &&
                 In(X.avgPer, hor.minPer, hor.maxPer))
            immediate = 1;
        else if (!Noise(Z) && Z.avgAmp >= X.avgAmp && Z.avgAmp >= Y.avgAmp && In(Z.avgAmp, ver.minAmp, ver.maxAmp) &&
                 In(Z.avgPer, ver.minPer, ver.maxPer))
            immediate = 2;
    }

    bool Variation(const Axis& x) const {
        return x.size > 1 && fabs(x.ext[x.last].v - x.ext[(x.last + 5) % 6].v) > maxIdleAmp &&
               (now - x.ext[x.last].t) < 0.2f;
    }

    void Smart() {
        if (Variation(ax[0]) || Variation(ax[1]) || Variation(ax[2])) {
            idleMode = 0;
            if (immediate != smartNext) {
                smartNext = immediate;
                smartUpdate = now;
            }
            float wait = smart == 0 ? idleToNext : curToNext;
            if (!(smartUpdate == 0.0f || !((now - smartUpdate) > wait))) smart = smartNext;
        } else if (idleMode != 0) {
            if (now - idleUpdate > idleMin) smart = 0;
        } else {
            idleMode = 1;
            idleUpdate = smartUpdate = now;
        }
    }

    void Update(float t, float frameDt) {
        now = t;
        dt = frameDt;
        ParseSamples();
        for (int a = 0; a < 3; ++a) RemoveDead(ax[a]);
        for (int a = 0; a < 3; ++a) Average(ax[a]);
        if (level >= 1) Immediate();
        if (level >= 2) Smart();
    }
};

#define MOT_EXPECT(cond, ...) do { bool ok_ = (cond); Report(rep, ok_, __VA_ARGS__); if (!ok_) ++fails; } while (0)

struct Run { int finalSmart, stableFrames, immediateMax; float maxNorm[2], avgAmp[3], avgPer[3]; };

// fps frames per second for secs seconds with the input; WII_LIB_MOVE on the samples
Run Simulate(const MotionInput& base, double fps, double secs, double hz[MOTION_AXES] = NULL) {
    s_st = State();
    LibMove* lib = new LibMove();
    double t0 = 1000.0;
    lib->Init((float)t0);
    Run r;
    memset(&r, 0, sizeof(r));
    int frames = (int)(secs * fps + 0.5), last = -1;
    for (int f = 1; f <= frames; ++f) {
        MotionInput in = base;
        in.t = t0 + f / fps;
        if (hz)
            for (int a = 0; a < MOTION_AXES; ++a) in.hz[a] = hz[a];
        MotionFrame(in);
        lib->Update((float)in.t, (float)(1.0 / fps));
        for (int s = 0; s < 2; ++s) {
            float v[3];
            MotionAccel(s, 0, v);
            float n = (float)sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
            if (n > r.maxNorm[s]) r.maxNorm[s] = n;
        }
        if (lib->immediate > r.immediateMax) r.immediateMax = lib->immediate;
        if (lib->smart != last) r.stableFrames = 0;
        last = lib->smart;
        ++r.stableFrames;
    }
    r.finalSmart = lib->smart;
    for (int a = 0; a < 3; ++a) r.avgAmp[a] = lib->ax[a].avgAmp, r.avgPer[a] = lib->ax[a].avgPer;
    delete lib;
    return r;
}

int RunMotionSelfTest(std::string& rep) {
    int fails = 0;
    State saved = s_st;
    float v[3], h[2];

    s_st = State();
    MotionAccel(0, 0, v);
    float n1[3];
    MotionAccel(1, 3, n1);
    MotionHorizon(h);
    MOT_EXPECT(!MotionActive() && MotionSampleCount() == 1 && v[0] == 0.0f && v[1] == -1.0f && v[2] == 0.0f &&
               n1[1] == -1.0f && h[0] == 1.0f && h[1] == 0.0f,
               "before the first frame: at rest (0, -1, 0), Nunchuk at rest, horizon (1, 0), one sample");

    MotionInput in;
    memset(&in, 0, sizeof(in));
    in.on = true;
    const double fpsList[3] = { 60.0, 144.0, 500.0 };
    for (int k = 0; k < 3; ++k) {
        double fps = fpsList[k];
        MotionInput a = in;
        a.attack = true;
        Run r = Simulate(a, fps, 1.5);
        MOT_EXPECT(r.maxNorm[0] > 3.5f && r.maxNorm[1] > 3.5f && r.finalSmart == 2 && r.stableFrames > fps * 0.8,
                   "%3.0f fps, SCREAM held 1.5 s: remote and Nunchuk peak %.2f / %.2f g (the controls check wants "
                   "above 3), WII_LIB_MOVE smart move %d (want 2, up and down) for the last %d frames, averages "
                   "amplitude %.2f period %.3f", fps, r.maxNorm[0], r.maxNorm[1], r.finalSmart, r.stableFrames,
                   r.avgAmp[2], r.avgPer[2]);
        const int want[3] = { 1, 2, 3 };
        const char* const names[3] = { "SHAKE LEFT/RIGHT", "SHAKE UP/DOWN", "MARACA SHAKE" };
        const int libAxis[3] = { 0, 2, 1 };
        for (int ax = 0; ax < 3; ++ax) {
            MotionInput s = in;
            s.shake[ax] = true;
            r = Simulate(s, fps, 1.5);
            MOT_EXPECT(r.finalSmart == want[ax] && r.stableFrames > fps * 0.8 && r.maxNorm[1] == 1.0f,
                       "%3.0f fps, %-16s held: smart move %d (want %d) for the last %d frames, amplitude %.2f period "
                       "%.3f, Nunchuk still", fps, names[ax], r.finalSmart, want[ax], r.stableFrames,
                       r.avgAmp[libAxis[ax]], r.avgPer[libAxis[ax]]);
        }
    }
    const double rhythms[4] = { 2.0, 3.5, 6.0, 12.0 };
    for (int k = 0; k < 4; ++k) {
        MotionInput s = in;
        s.shake[0] = true;
        double hz[MOTION_AXES] = { rhythms[k], 0.0, 0.0 };
        Run r = Simulate(s, 60.0, 2.0, hz);
        MOT_EXPECT(r.finalSmart == 1 && r.stableFrames > 60 && fabs(s_st.axis[0].hz - (rhythms[k] < RHYTHM_HZ_MIN ?
                   RHYTHM_HZ_MIN : rhythms[k] > RHYTHM_HZ_MAX ? RHYTHM_HZ_MAX : rhythms[k])) < 1e-9,
                   "a mouse wiggle at %.1f Hz shakes at %.1f Hz: smart move %d for the last %d frames", rhythms[k],
                   s_st.axis[0].hz, r.finalSmart, r.stableFrames);
    }
    int rotMoves[3][2];
    bool rotOk = true;
    for (int k = 0; k < 3; ++k)
        for (int d = 0; d < 2; ++d) {
            MotionInput ro = in;
            ro.rotate = d == 0 ? 1 : -1;
            Run r = Simulate(ro, fpsList[k], 1.5);
            rotMoves[k][d] = r.finalSmart;
            rotOk = rotOk && (r.finalSmart == 4 || r.finalSmart == 5) && r.stableFrames > fpsList[k] * 0.7 &&
                    r.maxNorm[1] == 1.0f;
        }
    rotOk = rotOk && rotMoves[0][0] != rotMoves[0][1] && rotMoves[1][0] == rotMoves[0][0] &&
            rotMoves[2][0] == rotMoves[0][0] && rotMoves[1][1] == rotMoves[0][1] && rotMoves[2][1] == rotMoves[0][1];
    MOT_EXPECT(rotOk, "SHAKE CLOCKWISE / COUNTER-CW held 1.5 s at 60, 144, 500 fps: smart moves %d / %d, %d / %d, "
               "%d / %d (the two rotations 4 and 5, the same at every frame rate), Nunchuk still", rotMoves[0][0],
               rotMoves[0][1], rotMoves[1][0], rotMoves[1][1], rotMoves[2][0], rotMoves[2][1]);
    MotionInput rs = in;
    rs.rotate = 1;
    rs.rotateHz = 2.0;
    Run slow = Simulate(rs, 60.0, 2.0);
    double slowHz = s_st.rot.hz;
    rs.rotateHz = 12.0;
    Run quick = Simulate(rs, 60.0, 2.0);
    MOT_EXPECT(slow.finalSmart == rotMoves[0][0] && quick.finalSmart == rotMoves[0][0] && fabs(slowHz - RHYTHM_HZ_MIN) < 1e-9 &&
               fabs(s_st.rot.hz - RHYTHM_HZ_MAX) < 1e-9,
               "mouse circles at 2 and 12 turns a second stir at %.1f and %.1f Hz: smart move %d, %d", slowHz,
               s_st.rot.hz, slow.finalSmart, quick.finalSmart);
    Run idle = Simulate(in, 60.0, 1.5);
    MotionInput tl = in;
    tl.tiltLeft = true;
    Run tilt = Simulate(tl, 60.0, 1.5);
    MOT_EXPECT(idle.finalSmart == 0 && idle.immediateMax == 0 && tilt.finalSmart == 0 && tilt.immediateMax == 0,
               "nothing held, TILT LEFT held: no move detected (%d %d, %d %d)", idle.finalSmart, idle.immediateMax,
               tilt.finalSmart, tilt.immediateMax);

    // tilt: 90 degrees each way, turned over with both, back on release, clicks do not roll
    s_st = State();
    MotionInput t = in;
    double now = 50.0;
    auto frames = [&](int count) {
        for (int i = 0; i < count; ++i) {
            now += 1.0 / 60.0;
            t.t = now;
            MotionFrame(t);
        }
    };
    t.tiltLeft = true;
    frames(30);
    MotionAccel(0, 0, v);
    MotionHorizon(h);
    bool left = fabs(v[0] - 1.0f) < 1e-4f && fabs(v[1]) < 1e-4f && fabs(h[0]) < 1e-4f && fabs(h[1] + 1.0f) < 1e-4f;
    t.tiltRight = true;
    frames(30);
    MotionAccel(0, 0, v);
    float bank[3] = { -v[0], v[2], -v[1] };                          // CWII_LIB_GetJoyBanking
    bool over = -bank[2] > 0.98f && s_st.roll < 0.0;
    t.tiltLeft = false;
    frames(30);
    MotionAccel(0, 0, v);
    bool right = fabs(v[0] + 1.0f) < 1e-4f && fabs(v[1]) < 1e-4f;
    t.tiltRight = false;
    frames(30);
    MotionAccel(0, 0, v);
    bool rest = fabs(v[0]) < 1e-4f && fabs(v[1] + 1.0f) < 1e-4f && fabs(s_st.roll) < 1e-6;
    t.tiltLeft = true;
    frames(5);                                                       // an 83 ms click
    t.tiltLeft = false;
    frames(10);
    MotionAccel(0, 0, v);
    bool click = fabs(v[0]) < 1e-6f && fabs(v[1] + 1.0f) < 1e-6f;
    MOT_EXPECT(left && over && right && rest && click,
               "TILT LEFT 0.5 s: (1, 0, 0), horizon (0, -1) %d; with TILT RIGHT: turned over, banking z %.3f %d; "
               "left released: the short way to the right (-1, 0, 0) %d; released: at rest %d; an 83 ms click: no "
               "roll %d", left, bank[2], over, right, rest, click);

    // samples: count per frame, newest first, spaced dt / n
    s_st = State();
    MotionInput sm = in;
    sm.shake[2] = true;
    sm.t = 10.0;
    MotionFrame(sm);
    sm.t = 10.0 + 1.0 / 60.0;
    MotionFrame(sm);
    int n60 = MotionSampleCount();
    sm.t += 1.0 / 400.0;
    MotionFrame(sm);
    int n400 = MotionSampleCount();
    sm.t += 0.2;                                                     // a hitch: 12 at most
    MotionFrame(sm);
    int nHitch = MotionSampleCount();
    s_st = State();
    sm.t = 20.0;
    for (int i = 0; i < 20; ++i) {
        sm.t += 1.0 / 60.0;
        MotionFrame(sm);
    }
    float s0[3], s1[3];
    MotionAccel(0, 0, s0);
    MotionAccel(0, 1, s1);
    double back = (1.0 / 60.0) / MotionSampleCount();
    float expect0 = SHAKE_G * (float)sin(s_st.axis[2].phase), expect1 = SHAKE_G * (float)sin(s_st.axis[2].phase -
                                                                                            kTwoPi * 5.0 * back);
    MOT_EXPECT(n60 == 3 && n400 == 1 && nHitch == MAX_SAMPLES && fabs(s0[2] - expect0) < 1e-4f &&
               fabs(s1[2] - expect1) < 1e-4f,
               "samples per frame: 60 fps %d, 400 fps %d, a 0.2 s frame %d; sample 0 = the frame's end, sample 1 "
               "%.4f s earlier", n60, n400, nHitch, back);

    MotionInput off = in;
    off.attack = true;
    s_st = State();
    off.t = 30.0;
    for (int i = 0; i < 30; ++i) {
        off.t += 1.0 / 60.0;
        MotionFrame(off);
    }
    off.on = false;
    for (int i = 0; i < 12; ++i) {
        off.t += 1.0 / 60.0;
        MotionFrame(off);
    }
    MotionAccel(1, 0, v);
    MOT_EXPECT(s_st.attack.env == 0.0f && v[1] == -1.0f, "the Options page opens: the shake stops within 0.2 s");

    // the Nunchuk stick from the movement actions
    s_st = State();
    MotionInput st = in;
    st.noPad = true;
    st.move[0] = 1.0f, st.move[1] = 1.0f;
    st.t = 40.0;
    MotionFrame(st);
    float sv[2];
    bool unpolled = !MotionStick(sv);
    MotionPolled();
    st.t += 1.0 / 60.0;
    MotionFrame(st);
    bool diag = MotionStick(sv) && fabs(sv[0] - 0.70710678f) < 1e-5f && fabs(sv[1] - 0.70710678f) < 1e-5f;
    st.noPad = false;
    st.t += 1.0 / 60.0;
    MotionFrame(st);
    bool pad = !MotionStick(sv);
    st.noPad = true;
    st.t += 0.2;
    MotionFrame(st);
    bool recent = MotionStick(sv);
    st.t += 0.2;
    MotionFrame(st);
    st.t += 0.2;
    MotionFrame(st);
    bool stale = !MotionStick(sv);
    MOT_EXPECT(unpolled && diag && pad && recent && stale,
               "the movement keys as the Nunchuk stick: not before a script reads the accelerometers %d, forward + "
               "right = (0.707, 0.707) %d, never with a controller %d, kept 0.4 s after the last read %d, not 0.6 s "
               "after %d", unpolled, diag, pad, recent, stale);

    s_st = saved;
    return fails;
}

}  // namespace

// ---------------------------------------------------------------------------------------------------------------
// entry points
// ---------------------------------------------------------------------------------------------------------------
void MotionFrame(const MotionInput& in) {
    State& s = s_st;
    double dt = s.started ? in.t - s.t : 0.0;
    if (!(dt >= 0.0 && dt <= 0.25)) dt = 0.0;   // the first frame, a clock jump
    s.started = true;
    s.t = in.t;
    s.dt = dt;
    int n = (int)floor(dt * SAMPLE_HZ + 0.5);
    s.samples = n < 1 ? 1 : n > MAX_SAMPLES ? MAX_SAMPLES : n;
    for (int a = 0; a < MOTION_AXES; ++a) Advance(s.axis[a], in.on && in.shake[a], in.hz[a], dt);
    Advance(s.attack, in.on && in.attack, 0.0, dt);
    s.noPad = in.on && in.noPad;
    s.move[0] = in.on ? in.move[0] : 0.0f;
    s.move[1] = in.on ? in.move[1] : 0.0f;
    bool rotate = in.on && in.rotate != 0;
    if (rotate && s.rot.env <= 0.0f) s.rotDir = in.rotate > 0 ? 1 : -1;    // a circle keeps its way round until it ends
    Advance(s.rot, rotate && (in.rotate > 0) == (s.rotDir > 0), in.rotateHz, dt);
    Tilt(s, in, dt);
}

bool MotionActive() { return s_st.started; }

int MotionSampleCount() { return s_st.started ? s_st.samples : 1; }

void MotionAccel(int sensor, int sample, float out[3]) {
    const State& s = s_st;
    out[0] = 0.0f;
    out[1] = sensor == 0 || sensor == 1 ? -1.0f : 0.0f;
    out[2] = 0.0f;
    if (!s.started || (sensor != 0 && sensor != 1)) return;
    if (sample < 0) sample = 0;
    double back = s.samples > 0 ? (double)sample * s.dt / s.samples : 0.0;
    float w = s.samples > 0 ? (float)sample / (float)s.samples : 0.0f;
    if (w > 1.0f) w = 1.0f;
    float attack = Value(s.attack, back, w);
    if (sensor == 1) {
        out[1] += attack;
        return;
    }
    double r = s.roll + (s.rollStart - s.roll) * w;
    float rotEnv = s.rot.env + (s.rot.envStart - s.rot.env) * w;
    float rotX = 0.0f, rotY = 0.0f;
    if (rotEnv > 0.0f) {
        double ph = s.rot.phase - kTwoPi * s.rot.hz * back;
        rotX = -(float)s.rotDir * SHAKE_G * rotEnv * (float)sin(ph);
        rotY = SHAKE_G * rotEnv * (float)cos(ph);
    }
    out[0] = -(float)sin(r) + Value(s.axis[0], back, w) + rotX;
    out[1] = -(float)cos(r) + Value(s.axis[1], back, w) + attack + rotY;
    out[2] = Value(s.axis[2], back, w);
}

void MotionHorizon(float out[2]) {
    out[0] = (float)cos(s_st.roll);
    out[1] = (float)sin(s_st.roll);
}

void MotionPolled() { s_st.polled = s_st.t; }

// IO_JoystickStickGet(0, 0) of the Wii-only scripts (the paint view's scroll, the challenges' pause page, the music
// maker's instrument choice): the movement actions, while no controller is connected and the scripts that read the
// remote's accelerometer samples run (Inside Zee Wiimote's shake detection asks for the sample count every frame;
// the levels' scripts only read IO_JoystickAccelGet, handle the keyboard themselves and keep the engine's answer)
bool MotionStick(float out[2]) {
    const State& s = s_st;
    out[0] = out[1] = 0.0f;
    if (!s.started || !s.noPad || s.polled <= 0.0 || s.t - s.polled > 0.5) return false;
    float x = s.move[0], y = s.move[1], n = (float)sqrt(x * x + y * y);
    if (n > 1.0f) x /= n, y /= n;
    out[0] = x;
    out[1] = y;
    return true;
}

extern "C" {

// wmtest only: the motion frames and WII_LIB_MOVE's detection on them.  Returns the number of failures.
int __cdecl WiimoteMotionSelfTest(char* out, int outSize) {
    std::string rep;
    int fails = RunMotionSelfTest(rep);
    if (out && outSize > 0) {
        size_t n = rep.size() < (size_t)outSize - 1 ? rep.size() : (size_t)outSize - 1;
        memcpy(out, rep.c_str(), n);
        out[n] = 0;
    }
    return fails;
}

}  // extern "C"
