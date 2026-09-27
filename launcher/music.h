// music.h - the setup's music: an .RGHS file (tools/rghs.py: a 32-byte header with the loop points, then Ogg Vorbis)
// decoded with stb_vorbis and played through waveOut in a thread of its own, looping between the two frames of the
// header.  The window in setup mode plays Assets/Audio/setup.rghs (a Qt resource); when the setup hands over to the
// launcher the music fades out, and the launcher itself never plays it.
#pragma once

#include <QObject>
#include <QString>
#include <atomic>
#include <thread>
#include <vector>

namespace rgh {

class Music : public QObject {              // no signals or slots: no Q_OBJECT, so it links without moc
public:
    explicit Music(QObject* parent = nullptr);
    ~Music() override;
    bool play(const QString& path, float gain = 0.6f);   // a file, or a Qt resource (":/setup.rghs")
    void fadeOut(int ms);                                 // down to silence over `ms`, then stops
    void stop();
    bool playing() const { return m_playing; }
    // for the tests
    long long framesPlayed() const { return m_frames; }
    int loops() const { return m_loops; }
    QString error() const { return m_error; }
private:
    void run();                                           // the playback thread
    std::vector<unsigned char> m_file;
    std::thread m_thread;
    std::atomic<bool> m_stop{ false };
    std::atomic<int> m_fadeMs{ 0 };                       // > 0: start a fade of that length
    std::atomic<float> m_gain{ 0.6f };
    std::atomic<bool> m_playing{ false };
    std::atomic<long long> m_frames{ 0 };
    std::atomic<int> m_loops{ 0 };
    QString m_error;
};

}  // namespace rgh
