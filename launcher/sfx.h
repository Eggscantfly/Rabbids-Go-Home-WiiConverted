// sfx.h - short sounds for the setup (the game's own end-of-level sounds during the conversion meter): 16-bit PCM
// WAV files from the Qt resources, mixed by a thread of the launcher's own into one waveOut stream (32 kHz mono, a
// few tens of milliseconds ahead), so a sound starts at once, plays over the ones before it, and a loop's pitch can
// follow a value set while it plays (the game's sound sets drive the transfer sound's pitch from the fill).
#pragma once

#include <QString>
#include <atomic>
#include <mutex>
#include <thread>
#include <vector>

namespace rgh {

class Sfx;

class Mixer {                                         // one for the process
public:
    static Mixer& get();
    int start(const std::vector<short>* pcm, int rate, float gain, bool loop, float pitch);   // a voice; its id
    void setPitch(int id, float pitch);
    void stop(int id);
    void fade(int id, int ms);                        // the voice fades to silence and ends
    void stopAll(const std::vector<short>* pcm);      // every voice of a sample
    bool alive(int id);
    static const int kRate = 32000;
private:
    struct Voice { int id; const std::vector<short>* pcm; double pos; float gain, step; bool loop; float fadePer = 0.f; };
    Mixer() = default;
    ~Mixer();
    void ensure();                                    // the thread and the device, once
    void run();
    std::mutex m_lock;
    std::vector<Voice> m_voices;
    int m_next = 1;
    std::thread m_thread;
    std::atomic<bool> m_stop{ false };
    bool m_started = false;
};

class Sfx {
public:
    Sfx() = default;
    ~Sfx();
    Sfx(const Sfx&) = delete;
    Sfx& operator=(const Sfx&) = delete;
    bool load(const QString& path);                   // a WAV file or a Qt resource (":/sfx/gift.wav")
    bool loaded() const { return !m_pcm.empty(); }
    void play(float gain = 1.f);                      // at once, over whatever else plays
    void stop();                                      // every voice of this sound, the pitched loop too
    bool playing();
    // the pitched loop: one at a time, its playback rate changed while it plays
    void startLoop(float gain, float rate = 1.f);
    void setRate(float rate);
    void stopLoop(int fadeMs = 0);                    // at once, or fading over fadeMs
    bool looping();
private:
    std::vector<short> m_pcm;                         // mono samples at the mixer's rate
    std::vector<short> m_loop;                        // the samples again, with a crossfaded wrap (built once)
    std::vector<int> m_shots;
    int m_loopId = -1;
};

}  // namespace rgh
