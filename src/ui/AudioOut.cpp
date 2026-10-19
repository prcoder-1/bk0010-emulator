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
    fmt.setChannelCount(2);   // стерео: пищалка в оба канала, AY по ABC, Covox L/R
    fmt.setSampleFormat(QAudioFormat::Int16);

    QAudioDevice dev = QMediaDevices::defaultAudioOutput();
    if (dev.isNull()) return; // no output device available
    if (!dev.isFormatSupported(fmt)) fmt = dev.preferredFormat();
    channels_ = fmt.channelCount() > 0 ? fmt.channelCount() : 1;
    sampleRate_ = fmt.sampleRate() > 0 ? fmt.sampleRate() : 44100;

    sink_ = new QAudioSink(dev, fmt, this);
    // Two buffers matter and they are decoupled:
    //  * the sink's own buffer (below) covers audio-thread scheduling — how late
    //    the OS may call readData; a modest ~40 ms is plenty.
    //  * the Speaker FIFO cushion (primeSamples_) covers *producer* jitter — the
    //    emulation runs in the GUI thread, which hitches ~every 20 ms on the screen
    //    render, so production arrives unevenly. readData drains greedily, which
    //    would keep the FIFO near-empty and make every hitch insert a click; instead
    //    we prime a ~90 ms cushion before serving so hitches are absorbed. With
    //    exact rate-matching (ticksPerFrame × 48.83 Hz = cpuFreq: 3 or 4 MHz) the
    //    cushion is stable; maxSamples_ is a valve against pathological drift.
    sink_->setBufferSize(fmt.bytesForDuration(40000)); // ~40 ms
    sink_->setVolume(0.6);

    primeSamples_ = sampleRate_ * 90 / 1000;   // ~90 ms cushion before playback
    maxSamples_   = sampleRate_ * 180 / 1000;  // hard cap on backlog
    primed_ = false;

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

    // Safety valve: never let the FIFO grow unbounded (would add audible latency).
    if (maxSamples_ > 0) spk_->trimTo(static_cast<size_t>(maxSamples_));

    // Prime a latency cushion before serving real audio, so GUI-thread jitter can
    // be absorbed without inserting silence gaps. While filling, output flat last
    // level (silence at startup). Re-arm after a full dry-out to avoid dribbling
    // one-sample gaps continuously — one clean cushion refill instead.
    if (!primed_) {
        if (spk_->available() < static_cast<size_t>(primeSamples_)) {
            fill(out, frames, ch, 0);
            return frames * ch * 2;
        }
        primed_ = true;
    }

    // Стерео-кадры (L, R) из FIFO; недогрузку добиваем последним уровнем каналов.
    tmp_.resize(static_cast<size_t>(frames) * 2);
    const size_t got = spk_->read(tmp_.data(), static_cast<size_t>(frames));
    if (got > 0) { lastL_ = tmp_[2 * got - 2]; lastR_ = tmp_[2 * got - 1]; }
    else primed_ = false;            // FIFO ran dry — rebuild the cushion
    for (size_t i = got; i < static_cast<size_t>(frames); ++i) { tmp_[2 * i] = lastL_; tmp_[2 * i + 1] = lastR_; }
    fill(out, frames, ch, tmp_.data());
    return frames * ch * 2;
}

// Разложить стерео-кадры по каналам устройства: 2 — как есть, 1 — среднее, больше —
// L и R в первые два, остальные — среднее. lr == nullptr — последний уровень.
void AudioOut::fill(int16_t* out, qint64 frames, int ch, const int16_t* lr) {
    for (qint64 i = 0; i < frames; ++i) {
        const int16_t l = lr ? lr[2 * i] : lastL_, r = lr ? lr[2 * i + 1] : lastR_;
        const int16_t m = static_cast<int16_t>((int(l) + int(r)) / 2);
        int16_t* f = out + i * ch;
        if (ch == 1) { f[0] = m; continue; }
        f[0] = l; f[1] = r;
        for (int c = 2; c < ch; ++c) f[c] = m;
    }
}
#endif // HAVE_QT_MULTIMEDIA
