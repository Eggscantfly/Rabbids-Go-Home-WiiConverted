// sfx.cpp - short sounds through one mixed waveOut stream (see sfx.h).
#include "sfx.h"

#include <QFile>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <mmsystem.h>

namespace rgh {

namespace {
const int kBuffers = 5, kFrames = 384;                // the stream: 5 x 12 ms; its thread runs above normal
}

// ---- the mixer
Mixer& Mixer::get() {
    static Mixer mixer;
    return mixer;
}

Mixer::~Mixer() {
    m_stop = true;
    if (m_thread.joinable()) m_thread.join();
}

void Mixer::ensure() {
    if (m_started) return;
    m_started = true;
    m_thread = std::thread([this] { run(); });
}

int Mixer::start(const std::vector<short>* pcm, int rate, float gain, bool loop, float pitch) {
    if (!pcm || pcm->empty()) return -1;
    std::lock_guard<std::mutex> hold(m_lock);
    ensure();
    Voice v;
    v.id = m_next++;
    v.pcm = pcm;
    v.pos = 0.0;
    v.gain = std::clamp(gain, 0.f, 1.f);
    v.step = std::clamp(pitch, 0.1f, 4.f) * float(rate) / float(kRate);
    v.loop = loop;
    m_voices.push_back(v);
    return v.id;
}

void Mixer::setPitch(int id, float pitch) {
    std::lock_guard<std::mutex> hold(m_lock);
    for (Voice& v : m_voices)
        if (v.id == id) v.step = std::clamp(pitch, 0.1f, 4.f);      // the loop's samples are at the mixer's rate
}

void Mixer::stop(int id) {
    std::lock_guard<std::mutex> hold(m_lock);
    m_voices.erase(std::remove_if(m_voices.begin(), m_voices.end(), [id](const Voice& v) { return v.id == id; }), m_voices.end());
}

void Mixer::fade(int id, int ms) {
    std::lock_guard<std::mutex> hold(m_lock);
    for (Voice& v : m_voices)
        if (v.id == id) v.fadePer = v.gain / float(std::max(1, ms) * kRate / 1000);   // gain lost per frame
}

void Mixer::stopAll(const std::vector<short>* pcm) {
    std::lock_guard<std::mutex> hold(m_lock);
    m_voices.erase(std::remove_if(m_voices.begin(), m_voices.end(), [pcm](const Voice& v) { return v.pcm == pcm; }), m_voices.end());
}

bool Mixer::alive(int id) {
    std::lock_guard<std::mutex> hold(m_lock);
    for (const Voice& v : m_voices) if (v.id == id) return true;
    return false;
}

void Mixer::run() {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
    WAVEFORMATEX fmt{};
    fmt.wFormatTag = WAVE_FORMAT_PCM;
    fmt.nChannels = 1;
    fmt.nSamplesPerSec = DWORD(kRate);
    fmt.wBitsPerSample = 16;
    fmt.nBlockAlign = 2;
    fmt.nAvgBytesPerSec = fmt.nSamplesPerSec * fmt.nBlockAlign;
    HWAVEOUT h = nullptr;
    if (waveOutOpen(&h, WAVE_MAPPER, &fmt, 0, 0, CALLBACK_NULL) != MMSYSERR_NOERROR) return;
    std::vector<std::vector<short>> bufs(kBuffers, std::vector<short>(kFrames));
    std::vector<WAVEHDR> hdrs(kBuffers);
    std::vector<float> mix(kFrames);
    auto fill = [&](int b) {
        std::fill(mix.begin(), mix.end(), 0.f);
        {
            std::lock_guard<std::mutex> hold(m_lock);
            for (size_t vi = 0; vi < m_voices.size();) {
                Voice& v = m_voices[vi];
                const std::vector<short>& s = *v.pcm;
                const size_t n = s.size();
                bool done = false;
                for (int i = 0; i < kFrames; ++i) {
                    if (v.pos >= double(n)) {
                        if (!v.loop) { done = true; break; }
                        v.pos -= double(n);
                    }
                    size_t i0 = size_t(v.pos);
                    size_t i1 = i0 + 1 < n ? i0 + 1 : (v.loop ? 0 : i0);
                    float u = float(v.pos - double(i0));
                    mix[size_t(i)] += (s[i0] + (s[i1] - s[i0]) * u) * v.gain;
                    v.pos += double(v.step);
                    if (v.fadePer > 0.f) {
                        v.gain -= v.fadePer;
                        if (v.gain <= 0.f) { done = true; break; }
                    }
                }
                if (done) m_voices.erase(m_voices.begin() + long(vi));
                else ++vi;
            }
        }
        std::vector<short>& out = bufs[size_t(b)];
        for (int i = 0; i < kFrames; ++i) out[size_t(i)] = short(std::clamp(mix[size_t(i)], -32767.f, 32767.f));
    };
    for (int b = 0; b < kBuffers; ++b) {
        hdrs[size_t(b)] = WAVEHDR{};
        hdrs[size_t(b)].lpData = reinterpret_cast<LPSTR>(bufs[size_t(b)].data());
        hdrs[size_t(b)].dwBufferLength = DWORD(bufs[size_t(b)].size() * 2);
        waveOutPrepareHeader(h, &hdrs[size_t(b)], sizeof(WAVEHDR));
        fill(b);
        waveOutWrite(h, &hdrs[size_t(b)], sizeof(WAVEHDR));
    }
    int next = 0;
    while (!m_stop) {
        WAVEHDR& hd = hdrs[size_t(next)];
        if (hd.dwFlags & WHDR_DONE) {
            fill(next);
            hd.dwFlags &= ~WHDR_DONE;
            waveOutWrite(h, &hd, sizeof(WAVEHDR));
            next = (next + 1) % kBuffers;
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
    waveOutReset(h);
    for (int b = 0; b < kBuffers; ++b) waveOutUnprepareHeader(h, &hdrs[size_t(b)], sizeof(WAVEHDR));
    waveOutClose(h);
}

// ---- a sound
Sfx::~Sfx() {
    stop();
}

bool Sfx::load(const QString& path) {
    m_pcm.clear();
    m_loop.clear();
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return false;
    QByteArray bytes = f.readAll();
    const unsigned char* d = reinterpret_cast<const unsigned char*>(bytes.constData());
    size_t n = size_t(bytes.size());
    if (n < 12 || std::memcmp(d, "RIFF", 4) != 0 || std::memcmp(d + 8, "WAVE", 4) != 0) return false;
    auto u32 = [&](size_t o) { return unsigned(d[o]) | unsigned(d[o + 1]) << 8 | unsigned(d[o + 2]) << 16 | unsigned(d[o + 3]) << 24; };
    auto u16 = [&](size_t o) { return unsigned(d[o]) | unsigned(d[o + 1]) << 8; };
    size_t pos = 12;
    int bits = 0, channels = 0, rate = 0;
    const unsigned char* pcm = nullptr;
    size_t pcmSize = 0;
    while (pos + 8 <= n) {
        unsigned size = u32(pos + 4);
        const unsigned char* body = d + pos + 8;
        if (pos + 8 + size > n) size = unsigned(n - pos - 8);
        if (std::memcmp(d + pos, "fmt ", 4) == 0 && size >= 16) {
            if (u16(pos + 8) != 1) return false;      // PCM only
            channels = int(u16(pos + 10));
            rate = int(u32(pos + 12));
            bits = int(u16(pos + 22));
        } else if (std::memcmp(d + pos, "data", 4) == 0) {
            pcm = body;
            pcmSize = size;
        }
        pos += 8 + size + (size & 1);
    }
    if (!pcm || bits != 16 || channels < 1 || channels > 2 || rate <= 0) return false;
    // mono at the mixer's rate: channels averaged, the rate converted by linear interpolation
    size_t frames = pcmSize / 2 / size_t(channels);
    if (frames == 0) return false;
    std::vector<float> mono(frames);
    for (size_t i = 0; i < frames; ++i) {
        float sum = 0.f;
        for (int c = 0; c < channels; ++c) {
            const unsigned char* p = pcm + (i * size_t(channels) + size_t(c)) * 2;
            sum += float(short(unsigned(p[0]) | unsigned(p[1]) << 8));
        }
        mono[i] = sum / float(channels);
    }
    if (rate == Mixer::kRate) {
        m_pcm.resize(frames);
        for (size_t i = 0; i < frames; ++i) m_pcm[i] = short(mono[i]);
    } else {
        double step = double(rate) / double(Mixer::kRate);
        size_t out = size_t(double(frames) / step);
        m_pcm.resize(out);
        for (size_t i = 0; i < out; ++i) {
            double p = i * step;
            size_t i0 = size_t(p), i1 = std::min(i0 + 1, frames - 1);
            float u = float(p - double(i0));
            m_pcm[i] = short(mono[i0] + (mono[i1] - mono[i0]) * u);
        }
    }
    return !m_pcm.empty();
}

void Sfx::play(float gain) {
    if (m_pcm.empty()) return;
    m_shots.erase(std::remove_if(m_shots.begin(), m_shots.end(), [](int id) { return !Mixer::get().alive(id); }), m_shots.end());
    int id = Mixer::get().start(&m_pcm, Mixer::kRate, gain, false, 1.f);
    if (id > 0) m_shots.push_back(id);
}

void Sfx::stop() {
    stopLoop();
    Mixer::get().stopAll(&m_pcm);
    Mixer::get().stopAll(&m_loop);                    // a loop still fading out must not outlive its samples
    m_shots.clear();
}

bool Sfx::playing() {
    m_shots.erase(std::remove_if(m_shots.begin(), m_shots.end(), [](int id) { return !Mixer::get().alive(id); }), m_shots.end());
    return !m_shots.empty() || looping();
}

void Sfx::startLoop(float gain, float rate) {
    if (m_pcm.empty()) return;
    stopLoop();
    if (m_loop.empty()) {
        // a seamless loop: the last 30 ms crossfade into the first 30 ms, and the loop runs from there
        size_t frames = m_pcm.size();
        size_t xf = std::min(frames / 4, size_t(Mixer::kRate * 30 / 1000));
        m_loop.assign(m_pcm.begin() + long(xf), m_pcm.end());
        size_t n = m_loop.size();
        for (size_t i = 0; i < xf; ++i) {
            float w = float(i + 1) / float(xf + 1);
            m_loop[n - xf + i] = short(m_loop[n - xf + i] * (1.f - w) + m_pcm[i] * w);
        }
    }
    m_loopId = Mixer::get().start(&m_loop, Mixer::kRate, gain, true, rate);
}

void Sfx::setRate(float rate) {
    if (m_loopId > 0) Mixer::get().setPitch(m_loopId, rate);
}

void Sfx::stopLoop(int fadeMs) {
    if (m_loopId > 0) {
        if (fadeMs > 0) Mixer::get().fade(m_loopId, fadeMs);
        else Mixer::get().stop(m_loopId);
    }
    m_loopId = -1;
}

bool Sfx::looping() {
    if (m_loopId > 0 && !Mixer::get().alive(m_loopId)) m_loopId = -1;
    return m_loopId > 0;
}

}  // namespace rgh
