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
    fmt.setChannelCount(2);   // stereo: same mono signal in both channels
    fmt.setSampleFormat(QAudioFormat::Int16);

    QAudioDevice dev = QMediaDevices::defaultAudioOutput();
    if (dev.isNull()) return; // no output device available
    if (!dev.isFormatSupported(fmt)) fmt = dev.preferredFormat();
    channels_ = fmt.channelCount() > 0 ? fmt.channelCount() : 1;

    sink_ = new QAudioSink(dev, fmt, this);
    // Output latency ≈ this buffer, since the sink is kept full (readData never
    // starves it). One BK frame = 20 ms of audio is produced per 50 Hz tick, so
    // ~50 ms (2.5 frames) bridges the production bursts and timer jitter without
    // underruns, roughly halving the old 100 ms latency.
    sink_->setBufferSize(fmt.bytesForDuration(50000)); // ~50 ms
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
    const int ch = channels_ > 0 ? channels_ : 1;
    qint64 frames = (maxlen / 2) / ch;   // whole multi-channel frames that fit
    if (frames <= 0) return 0;
    auto* out = reinterpret_cast<int16_t*>(data);
    // Read the mono stream compactly into the front of the buffer...
    size_t got = spk_->read(out, static_cast<size_t>(frames));
    if (got > 0) last_ = out[got - 1];
    for (size_t i = got; i < static_cast<size_t>(frames); ++i)
        out[i] = last_; // pad underrun with the last level
    // ...then fan each mono sample out to every channel (L = R = ...), expanding
    // in place from the back so the still-unread source samples aren't clobbered.
    for (qint64 i = frames - 1; i >= 0; --i) {
        int16_t s = out[i];
        for (int c = 0; c < ch; ++c) out[i * ch + c] = s;
    }
    return frames * ch * 2;
}
#endif // HAVE_QT_MULTIMEDIA
