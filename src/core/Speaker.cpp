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
    std::lock_guard<std::mutex> lk(mtx_);
    while (acc_ >= 1.0) {
        filtered_ += (target - filtered_) * rc;
        dc_ += (filtered_ - dc_) * dcRc;
        double v = filtered_ - dc_;
        if (v > 32767.0) v = 32767.0; else if (v < -32767.0) v = -32767.0;
        int16_t s = static_cast<int16_t>(v);
        if (buf_.size() < 1 << 18) buf_.push_back(s); // cap ~256k samples
        acc_ -= 1.0;
    }
}

size_t Speaker::read(int16_t* out, size_t maxSamples) {
    std::lock_guard<std::mutex> lk(mtx_);
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
