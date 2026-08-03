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
        if (buf_.size() >= maxBuf) for (size_t i = 0; i < maxBuf / 4; ++i) buf_.pop_front();
        buf_.push_back(s);
        acc_ -= 1.0;
    }
}

size_t Speaker::read(int16_t* out, size_t maxSamples) {
    std::lock_guard<std::mutex> lk(mtx_);
    // Plain FIFO drain. The consumer (AudioOut) primes a latency cushion before
    // it starts serving and caps runaway backlog via trimTo(), so no dropping is
    // done here — dropping mid-stream would gap the 1-bit beeper and crackle.
    size_t n = std::min(maxSamples, buf_.size());
    for (size_t i = 0; i < n; ++i) { out[i] = buf_.front(); buf_.pop_front(); }  // O(1) per sample
    return n;
}

void Speaker::trimTo(size_t maxSamples) {
    std::lock_guard<std::mutex> lk(mtx_);
    while (buf_.size() > maxSamples) buf_.pop_front();
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
