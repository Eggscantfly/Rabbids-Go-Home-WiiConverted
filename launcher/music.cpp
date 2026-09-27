// music.cpp - the setup's music (see music.h).
#include "music.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <mmsystem.h>

#include <QFile>
#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>

extern "C" {
#define STB_VORBIS_HEADER_ONLY
#include "../platform/vendor/stb_vorbis.c"                 // Sean Barrett's ogg decoder, public domain
#undef STB_VORBIS_HEADER_ONLY
}

namespace rgh {

namespace {

// the header tools/rghs.py writes (little-endian)
struct RghsHeader {
    char magic[4];             // "RGHS"
    uint32_t version;
    uint32_t rate;
    uint32_t channels;
    uint32_t loopStart;        // the first frame of the loop
    uint32_t loopEnd;          // the frame after the loop's last; 0: the whole piece
    uint32_t frames;           // 0: unknown
    uint32_t oggSize;
};

const int kBuffers = 4;
const int kBufferFrames = 4096;        // 128 ms at 32 kHz: the fade and the stop answer within that

}  // namespace

Music::Music(QObject* parent) : QObject(parent) {}

Music::~Music() { stop(); }

bool Music::play(const QString& path, float gain) {
    stop();
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        m_error = "cannot open " + path;
        return false;
    }
    QByteArray bytes = f.readAll();
    if (bytes.size() < int(sizeof(RghsHeader)) || memcmp(bytes.constData(), "RGHS", 4) != 0) {
        m_error = path + " is not an RGHS file";
        return false;
    }
    m_file.assign(bytes.constData(), bytes.constData() + bytes.size());
    m_error.clear();
    m_gain = gain;
    m_stop = false;
    m_fadeMs = 0;
    m_frames = 0;
    m_loops = 0;
    m_playing = true;
    m_thread = std::thread([this] { run(); });
    return true;
}

void Music::fadeOut(int ms) {
    if (m_playing) m_fadeMs = ms > 0 ? ms : 1;
}

void Music::stop() {
    m_stop = true;
    if (m_thread.joinable()) m_thread.join();
    m_playing = false;
}

void Music::run() {
    RghsHeader h;
    memcpy(&h, m_file.data(), sizeof h);
    const unsigned char* ogg = m_file.data() + sizeof h;
    size_t oggSize = std::min<size_t>(h.oggSize, m_file.size() - sizeof h);
    int channels = 0, rate = 0;
    short* pcm = nullptr;
    int frames = stb_vorbis_decode_memory(ogg, int(oggSize), &channels, &rate, &pcm);
    if (frames <= 0 || pcm == nullptr || (channels != 1 && channels != 2)) {
        m_error = "the Ogg Vorbis stream does not decode";
        free(pcm);
        m_playing = false;
        return;
    }
    // the loop: [loopStart, loopEnd) in frames; without a usable one, the whole piece
    int loopStart = int(std::min<uint32_t>(h.loopStart, uint32_t(frames)));
    int loopEnd = (h.loopEnd > uint32_t(loopStart) && h.loopEnd <= uint32_t(frames)) ? int(h.loopEnd) : frames;
    if (loopEnd <= loopStart) {
        loopStart = 0;
        loopEnd = frames;
    }

    WAVEFORMATEX wf = {};
    wf.wFormatTag = WAVE_FORMAT_PCM;
    wf.nChannels = WORD(channels);
    wf.nSamplesPerSec = DWORD(rate);
    wf.wBitsPerSample = 16;
    wf.nBlockAlign = WORD(channels * 2);
    wf.nAvgBytesPerSec = DWORD(rate) * wf.nBlockAlign;
    HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    HWAVEOUT out = nullptr;
    if (waveOutOpen(&out, WAVE_MAPPER, &wf, DWORD_PTR(ev), 0, CALLBACK_EVENT) != MMSYSERR_NOERROR) {
        m_error = "no sound output";
        CloseHandle(ev);
        free(pcm);
        m_playing = false;
        return;
    }
    std::vector<short> buf[kBuffers];
    WAVEHDR hdr[kBuffers] = {};
    for (int i = 0; i < kBuffers; ++i) {
        buf[i].assign(size_t(kBufferFrames) * channels, 0);
        hdr[i].lpData = reinterpret_cast<LPSTR>(buf[i].data());
        hdr[i].dwBufferLength = DWORD(buf[i].size() * sizeof(short));
        waveOutPrepareHeader(out, &hdr[i], sizeof(WAVEHDR));
        hdr[i].dwFlags |= WHDR_DONE;                     // free to fill
    }

    int pos = 0;                                         // the frame played next
    float gain = m_gain;
    float fadeStep = 0.f;                                // per frame, once a fade started
    bool fading = false, over = false;
    while (!m_stop && !over) {
        for (int i = 0; i < kBuffers && !over; ++i) {
            if (!(hdr[i].dwFlags & WHDR_DONE)) continue;
            int fade = m_fadeMs.exchange(0);
            if (fade > 0 && !fading) {
                fading = true;
                fadeStep = gain / (float(rate) * float(fade) / 1000.f);
            }
            short* dst = buf[i].data();
            for (int f = 0; f < kBufferFrames; ++f) {
                if (pos >= loopEnd) {
                    pos = loopStart;
                    ++m_loops;
                }
                for (int c = 0; c < channels; ++c)
                    dst[f * channels + c] = short(float(pcm[size_t(pos) * channels + c]) * gain);
                ++pos;
                if (fading) {
                    gain -= fadeStep;
                    if (gain <= 0.f) {                   // silence for the rest, and that was the last buffer
                        gain = 0.f;
                        over = true;
                        std::fill(dst + (f + 1) * channels, dst + size_t(kBufferFrames) * channels, short(0));
                        break;
                    }
                }
            }
            m_frames += kBufferFrames;
            hdr[i].dwFlags &= ~WHDR_DONE;
            if (waveOutWrite(out, &hdr[i], sizeof(WAVEHDR)) != MMSYSERR_NOERROR) {
                over = true;
                break;
            }
        }
        WaitForSingleObject(ev, 100);
    }
    if (over && !m_stop) {                               // let the fade's tail play out (at most a second)
        for (int t = 0; t < 10; ++t) {
            bool all = true;
            for (int i = 0; i < kBuffers; ++i)
                if (!(hdr[i].dwFlags & WHDR_DONE)) all = false;
            if (all) break;
            WaitForSingleObject(ev, 100);
        }
    }
    waveOutReset(out);
    for (int i = 0; i < kBuffers; ++i) waveOutUnprepareHeader(out, &hdr[i], sizeof(WAVEHDR));
    waveOutClose(out);
    CloseHandle(ev);
    free(pcm);
    m_playing = false;
}

}  // namespace rgh
