#pragma once
#include <vector>
#ifdef HAVE_QT_MULTIMEDIA
#include <QIODevice>
#include <cstdint>

namespace bk { class Speaker; }
class QAudioSink;

// Pull-mode audio output: acts as the QIODevice that QAudioSink reads from,
// draining 16-bit stereo frames (L, R) produced by the core Speaker. Underruns are
// padded with the last level of each channel to avoid clicks. Thread-safe via Speaker's
// internal mutex (QAudioSink may pull from its own thread).
class AudioOut : public QIODevice {
    Q_OBJECT
public:
    explicit AudioOut(bk::Speaker* spk, QObject* parent = nullptr);
    ~AudioOut() override;

    void start();
    void stop();
    void setVolume(float v);

protected:
    qint64 readData(char* data, qint64 maxlen) override;
    qint64 writeData(const char*, qint64) override { return 0; }
    qint64 bytesAvailable() const override;
    bool isSequential() const override { return true; }

private:
    bk::Speaker* spk_;
    QAudioSink* sink_ = nullptr;
    int16_t lastL_ = 0, lastR_ = 0;
    std::vector<int16_t> tmp_;        // стерео-кадры из Speaker
    void fill(int16_t* out, qint64 frames, int ch, const int16_t* lr);
    int channels_ = 1;   // каналы устройства: 2 — L/R как есть, 1 — среднее (см. fill)
    int sampleRate_ = 44100;
    int primeSamples_ = 0;   // cushion (in frames) to build before serving audio
    int maxSamples_ = 0;     // hard backlog cap in frames (runaway-latency valve)
    bool primed_ = false;    // false until the cushion is first filled (or after a dry-out)
};
#endif // HAVE_QT_MULTIMEDIA
