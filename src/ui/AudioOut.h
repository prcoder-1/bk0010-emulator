#pragma once
#ifdef HAVE_QT_MULTIMEDIA
#include <QIODevice>
#include <cstdint>

namespace bk { class Speaker; }
class QAudioSink;

// Pull-mode audio output: acts as the QIODevice that QAudioSink reads from,
// draining 16-bit mono samples produced by the core Speaker. Underruns are
// padded with the last sample level to avoid clicks. Thread-safe via Speaker's
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
    int16_t last_ = 0;
};
#endif // HAVE_QT_MULTIMEDIA
