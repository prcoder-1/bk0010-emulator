#include "Speaker.h"
#include <algorithm>
#include <cmath>

namespace bk {

// Амплитуды восьми уровней (доля от полного размаха). Комбинации бит регистра
// 0177716: 0=нет, 1=004, 2=040, 3=044, 4=0100, 5=0104, 6=0140, 7=0144.
// Значения — из GID BKemu (devemu/Speaker.cpp:116-126), там честно помечены как
// «измерены на глаз»: точных данных по сопротивлениям цепи нет.
// Постоянная времени RC-цепи на выходе пищалки: 6.8 нФ x 8.2 кОм = 55.8 мкс
// (номиналы цепи взяты из GID, devemu/Speaker.cpp:18).
static const double kTauSeconds = 6.8e-9 * 8200.0;

static const double kLevelPct[8] = {0.00, 0.15, 0.30, 0.40, 0.70, 0.80, 0.94, 1.00};

void Speaker::feed(int level, int ticks) {
    level &= 7;
    if ((!enabled_ && !capture_) || ticks <= 0) { level_ = level; return; }
    level_ = level;
    // Number of output samples covered by these CPU ticks.
    const double samplesPerTick = static_cast<double>(sampleRate_) / cpuFreq_;
    // Полный размах прежний (36000 «в размахе», запас под выброс DC-блокера), но
    // теперь он делится по уровням: программа, дёргающая только бит 6, звучит на
    // 70 % размаха — ровно так эти уровни соотносятся на железе.
    const double target = -18000.0 + 36000.0 * kLevelPct[level_ & 7];
    const double dcRc = 0.002; // high-pass (DC block): a steady level decays to silence
    // Hard ceiling, only reached if the sink stalls completely (~250 ms of audio):
    // drop the oldest quarter to catch up, rather than dropping the newest samples
    // which would gap the live sound. In normal play read() keeps the backlog far
    // below this.
    const size_t maxBuf = static_cast<size_t>(sampleRate_) / 4 * 2;   // отсчётов (L+R)

    double span = ticks * samplesPerTick;   // длительность этого куска в отсчётах
    std::lock_guard<std::mutex> lk(mtx_);
    while (span > 0.0) {
        const double need = 1.0 - acc_;                  // до конца текущего отсчёта
        const double take = (span < need) ? span : need;
        // RC-цепь интегрируется в НЕПРЕРЫВНОМ времени — до выборки, а не после неё.
        // Это принципиально: фильтр, стоящий после децимации, от алиасинга не спасает
        // (проверено измерением: меандр у частоты Найквиста давал всплеск в полосе).
        // Здесь же всё выше ~10 кГц гасится ещё до того, как превратится в отсчёт.
        // Шаг точный: a = 1 - exp(-dt/tau), dt — реальная длительность куска.
        const double a = 1.0 - std::exp(-(take / sampleRate_) / kTauSeconds);
        filtered_ += (target - filtered_) * a;
        if (ay_) ay_->advance(take / sampleRate_);
        if (ay_ && ay2_) ay2_->advance(take / sampleRate_);
        if (covox_) { covoxAccL_ += covoxL_ * take; covoxAccR_ += covoxR_ * take; }
        acc_  += take;
        span  -= take;
        if (acc_ < 1.0) break;                            // отсчёт ещё не набран
        acc_ = 0.0;
        dc_ += (filtered_ - dc_) * dcRc;                  // DC-блокер: покой -> тишина
        const double beep = filtered_ - dc_;              // пищалка — в оба канала
        double l = beep, r = beep;
        if (ay_) {
            // Полная громкость трёх каналов моно (1.0) — 28000 в размахе; два чипа —
            // по 18000, чтобы шесть каналов не упирались в потолок. В стерео ABC канал
            // набирает до 0,716 от этого (панорама GID).
            double l1, r1;
            ay_->takeSampleLR(l1, r1);
            double al, ar;
            if (ay2_) {
                double l2, r2;
                ay2_->takeSampleLR(l2, r2);
                al = (l1 + l2) * 18000.0; ar = (r1 + r2) * 18000.0;
            } else {
                al = l1 * 28000.0; ar = r1 * 28000.0;
            }
            ayDcL_ += (al - ayDcL_) * dcRc;
            ayDcR_ += (ar - ayDcR_) * dcRc;
            l += al - ayDcL_;
            r += ar - ayDcR_;
        }
        if (covox_) {
            // Полная шкала ЦАП — ±16000: громко, но с запасом под AY и пищалку.
            const double cl = covoxAccL_ * 16000.0, cr = covoxAccR_ * 16000.0;
            covoxAccL_ = covoxAccR_ = 0.0;
            covoxDcL_ += (cl - covoxDcL_) * dcRc;
            covoxDcR_ += (cr - covoxDcR_) * dcRc;
            l += cl - covoxDcL_;
            r += cr - covoxDcR_;
        }
        auto clip = [](double v) { return static_cast<int16_t>(v > 32767.0 ? 32767.0 : v < -32767.0 ? -32767.0 : v); };
        const int16_t sl = clip(l), sr = clip(r);
        if (enabled_) {
            if (buf_.size() >= maxBuf) for (size_t i = 0; i < (maxBuf / 4 & ~size_t(1)); ++i) buf_.pop_front();
            buf_.push_back(sl);
            buf_.push_back(sr);
        }
        if (capture_) { cap_.push_back(sl); cap_.push_back(sr); }
    }
}

size_t Speaker::read(int16_t* outLR, size_t maxFrames) {
    std::lock_guard<std::mutex> lk(mtx_);
    // Plain FIFO drain. The consumer (AudioOut) primes a latency cushion before
    // it starts serving and caps runaway backlog via trimTo(), so no dropping is
    // done here — dropping mid-stream would gap the 1-bit beeper and crackle.
    const size_t n = std::min(maxFrames, buf_.size() / 2);
    for (size_t i = 0; i < 2 * n; ++i) { outLR[i] = buf_.front(); buf_.pop_front(); }  // O(1) per sample
    return n;
}

void Speaker::trimTo(size_t maxFrames) {
    std::lock_guard<std::mutex> lk(mtx_);
    while (buf_.size() > 2 * maxFrames) { buf_.pop_front(); buf_.pop_front(); }
}

size_t Speaker::available() {
    std::lock_guard<std::mutex> lk(mtx_);
    return buf_.size() / 2;
}

void Speaker::setCapture(bool on) {
    std::lock_guard<std::mutex> lk(mtx_);
    capture_ = on;
    cap_.clear();
}

void Speaker::takeCaptured(std::vector<int16_t>& out) {
    std::lock_guard<std::mutex> lk(mtx_);
    out.clear();
    out.swap(cap_);
}

void Speaker::clear() {
    std::lock_guard<std::mutex> lk(mtx_);
    buf_.clear();
    acc_ = 0.0;
}

} // namespace bk
