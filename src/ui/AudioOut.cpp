#ifdef HAVE_QT_MULTIMEDIA
#include "AudioOut.h"
#include "Speaker.h"
#include <QAudioSink>
#include <QAudioFormat>
#include <QMediaDevices>

AudioOut::AudioOut(bk::Speaker* spk, QObject* parent)
    : QIODevice(parent), spk_(spk) {}

AudioOut::~AudioOut() { stop(); }

void AudioOut::start() {
    QAudioFormat fmt;
    fmt.setSampleRate(44100);
    fmt.setChannelCount(1);
    fmt.setSampleFormat(QAudioFormat::Int16);

    QAudioDevice dev = QMediaDevices::defaultAudioOutput();
    if (dev.isNull()) return; // no output device available
    if (!dev.isFormatSupported(fmt)) fmt = dev.preferredFormat();

    sink_ = new QAudioSink(dev, fmt, this);
    sink_->setBufferSize(44100 * 2 / 10); // ~100 ms
    sink_->setVolume(0.6);

    spk_->clear();                  // drop samples accumulated during boot
    open(QIODevice::ReadOnly);
    sink_->start(this);
}

void AudioOut::stop() {
    if (sink_) { sink_->stop(); sink_->deleteLater(); sink_ = nullptr; }
    if (isOpen()) close();
}

void AudioOut::setVolume(float v) { if (sink_) sink_->setVolume(v); }

qint64 AudioOut::bytesAvailable() const {
    return 44100 * 2 + QIODevice::bytesAvailable(); // report as always ready
}

qint64 AudioOut::readData(char* data, qint64 maxlen) {
    qint64 samples = maxlen / 2;
    if (samples <= 0) return 0;
    auto* out = reinterpret_cast<int16_t*>(data);
    size_t got = spk_->read(out, static_cast<size_t>(samples));
    if (got > 0) last_ = out[got - 1];
    for (size_t i = got; i < static_cast<size_t>(samples); ++i)
        out[i] = last_; // pad underrun with the last level
    return samples * 2;
}
#endif // HAVE_QT_MULTIMEDIA
