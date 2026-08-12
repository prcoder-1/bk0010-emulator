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
    if (!enabled_ || ticks <= 0) { level_ = level; return; }
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
    const size_t maxBuf = static_cast<size_t>(sampleRate_) / 4;

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
        acc_  += take;
        span  -= take;
        if (acc_ < 1.0) break;                            // отсчёт ещё не набран
        acc_ = 0.0;
        dc_ += (filtered_ - dc_) * dcRc;                  // DC-блокер: покой -> тишина
        double v = filtered_ - dc_;
        if (v > 32767.0) v = 32767.0; else if (v < -32767.0) v = -32767.0;
        const int16_t smp = static_cast<int16_t>(v);
        if (buf_.size() >= maxBuf) for (size_t i = 0; i < maxBuf / 4; ++i) buf_.pop_front();
        buf_.push_back(smp);
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
