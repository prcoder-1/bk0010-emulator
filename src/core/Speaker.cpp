#include "Speaker.h"
#include <algorithm>

namespace bk {

void Speaker::feed(int bit, int ticks) {
    if (!enabled_ || ticks <= 0) { level_ = bit; return; }
    level_ = bit ? 1 : 0;
    // Number of output samples covered by these CPU ticks.
    const double samplesPerTick = static_cast<double>(sampleRate_) / cpuFreq_;
    acc_ += ticks * samplesPerTick;
    const double target = level_ ? 18000.0 : -18000.0; // headroom for the DC-block overshoot
    const double rc = 0.25;    // low-pass: smooths the square edges
    const double dcRc = 0.002; // high-pass (DC block): a steady level decays to silence
    // Hard ceiling, only reached if the sink stalls completely (~250 ms of audio):
    // drop the oldest quarter to catch up, rather than dropping the newest samples
    // which would gap the live sound. In normal play read() keeps the backlog far
    // below this.
    const size_t maxBuf = static_cast<size_t>(sampleRate_) / 4;
    std::lock_guard<std::mutex> lk(mtx_);
    while (acc_ >= 1.0) {
        filtered_ += (target - filtered_) * rc;
        dc_ += (filtered_ - dc_) * dcRc;
        double v = filtered_ - dc_;
        if (v > 32767.0) v = 32767.0; else if (v < -32767.0) v = -32767.0;
        int16_t s = static_cast<int16_t>(v);
        if (buf_.size() >= maxBuf) buf_.erase(buf_.begin(), buf_.begin() + maxBuf / 4);
        buf_.push_back(s);
        acc_ -= 1.0;
    }
}

size_t Speaker::read(int16_t* out, size_t maxSamples) {
    std::lock_guard<std::mutex> lk(mtx_);
    // Latency guard: the QAudioSink's own buffer is the reservoir that absorbs the
    // 20 ms production bursts, so this FIFO only needs to hand samples across. If
    // the producer runs ahead (timer jitter / catch-up) and the backlog grows past
    // ~1 frame, discard a small slice of the *oldest* samples per read so output
    // latency is pulled back gradually instead of accumulating. The correction is a
    // fraction of the excess (a few ms at most), which is inaudible for the 1-bit
    // beeper — far better than a one-shot skip or an ever-growing delay.
    const size_t reservoir = static_cast<size_t>(sampleRate_) / 50; // ~1 frame (20 ms)
    if (buf_.size() > 2 * reservoir) {
        size_t excess = buf_.size() - reservoir;
        size_t drop = (excess >> 6) + 1;                            // ~1.5 % of the excess
        buf_.erase(buf_.begin(), buf_.begin() + drop);
    }
    size_t n = std::min(maxSamples, buf_.size());
    std::copy(buf_.begin(), buf_.begin() + n, out);
    buf_.erase(buf_.begin(), buf_.begin() + n);
    return n;
}

size_t Speaker::available() {
    std::lock_guard<std::mutex> lk(mtx_);
    return buf_.size();
}

void Speaker::clear() {
    std::lock_guard<std::mutex> lk(mtx_);
    buf_.clear();
    acc_ = 0.0;
}

} // namespace bk
