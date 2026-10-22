#include "Ay8910.h"
#include <cstring>

namespace bk {

// Громкость YM2149: 32 ступени (emu2149 m_voltbl[1]), доля от максимума одного канала.
// AY-3-8910: 16 ступеней, каждая — на два индекса (emu2149 m_voltbl[0]).
// Стерео по схеме ABC с панорамой GID (devemu/Config.h: AY_LEFT_PAN_DEFAULT = 95,
// AY_CENTER_PAN_DEFAULT = 50; emu2149.cpp: k(n) = n/100 − 0,25·((n−50)²/10000 − 1) —
// квадратичная поправка, чтобы центр не казался тихим): A — 95 % влево, B — центр,
// C — 95 % вправо. В противоположный канал уходит ~20 % громкости крайнего.
static const double kPanL[3] = {1.149375, 0.75, 0.249375};
static const double kPanR[3] = {0.249375, 0.75, 1.149375};

static const uint16_t kVolYm[32] = {
    0x0000, 0x0000, 0x00EF, 0x01D0, 0x0290, 0x032A, 0x03EE, 0x04D2, 0x0611, 0x0782, 0x0912, 0x0A36,
    0x0C31, 0x0EB6, 0x1130, 0x13A0, 0x1751, 0x1BF5, 0x20E2, 0x2594, 0x2CA1, 0x357F, 0x3E45, 0x475E,
    0x5502, 0x6620, 0x7730, 0x8844, 0xA1D2, 0xC102, 0xE0A2, 0xFFFF};

static const uint16_t kVolAy[32] = {
    0x0000, 0x0000, 0x0340, 0x0340, 0x04C0, 0x04C0, 0x06F2, 0x06F2, 0x0A44, 0x0A44, 0x0F13, 0x0F13,
    0x1510, 0x1510, 0x227E, 0x227E, 0x289F, 0x289F, 0x414E, 0x414E, 0x5B21, 0x5B21, 0x7258, 0x7258,
    0x905E, 0x905E, 0xB550, 0xB550, 0xD7A0, 0xD7A0, 0xFFFF, 0xFFFF};

void Ay8910::reset() {
    std::memset(reg_, 0, sizeof reg_);
    addr_ = 0;
    for (auto& c : ch_) c = Chan{};
    noiseSeed_ = 1111; noiseCount_ = 0; noiseFreq_ = 0;
    envPtr_ = 0; envDir_ = 0; envFreq_ = 1; envCount_ = 0;
    envHold_ = envAlt_ = envLast_ = false;
    phase_ = accSum_ = accL_ = accR_ = 0.0; accN_ = 0; last_ = lastL_ = lastR_ = 0.0;
}

void Ay8910::writeReg(uint8_t r, uint8_t v) {
    if (r > 15) return;                 // номера выше 15 чип игнорирует
    if (r == 1 || r == 3 || r == 5 || r == 13) v &= 0x0F;
    else if (r == 6 || r == 8 || r == 9 || r == 10) v &= 0x1F;
    reg_[r] = v;
    switch (r) {
    case 0: case 1: case 2: case 3: case 4: case 5: {
        Chan& c = ch_[r >> 1];
        c.freq = reg_[r & ~1] | ((reg_[r | 1] & 0x0F) << 8);
        if (!c.freq) c.toggle = false;
        break;
    }
    case 6: noiseFreq_ = v << 1; break;
    case 7:                             // 0 — разрешено
        for (int i = 0; i < 3; ++i) {
            ch_[i].toneOn  = !(v & (1 << i));
            ch_[i].noiseOn = !(v & (8 << i));
        }
        break;
    case 8: case 9: case 10:
        ch_[r - 8].env = (v & 0x10) != 0;
        ch_[r - 8].vol = ((v & 0x0F) << 1) + 1;
        break;
    case 11: case 12: envFreq_ = reg_[11] | (reg_[12] << 8); break;
    case 13: {
        if (v < 4) v = 9; else if (v < 8) v = 15;   // формы 0-7 сводятся к 9 и 15
        envCount_ = 0;
        envHold_ = v & 1;
        envAlt_ = (v & 2) != 0;
        const bool attack = (v & 4) != 0;
        envLast_ = attack ^ envAlt_;
        envAlt_ = envAlt_ && !envHold_;
        envPtr_ = attack ? 0 : 0x1f;
        envDir_ = attack ? 1 : -1;
        break;
    }
    default: break;
    }
}

// Один шаг — 8 тактов чипа (emu2149 calc()).
void Ay8910::step() {
    if (++envCount_ >= envFreq_) {
        envCount_ = 0;
        envPtr_ += envDir_;
        if (envPtr_ & ~0x1f) {          // цикл огибающей кончился
            if (envAlt_) { envDir_ = -envDir_; envPtr_ += envDir_; }
            else envPtr_ &= 0x1f;
            if (envHold_) { envDir_ = 0; envPtr_ = envLast_ ? 0x1f : 0; }
        }
    }
    if (++noiseCount_ >= noiseFreq_) {
        noiseCount_ = 0;
        const uint32_t b = (noiseSeed_ ^ (noiseSeed_ >> 2)) & 1;
        noiseSeed_ = (noiseSeed_ >> 1) | (b << 16);
    }
    for (auto& c : ch_)
        if (++c.count >= c.freq) { c.count = 0; if (c.freq) c.toggle = !c.toggle; }
}

void Ay8910::output(double& mono, double& left, double& right) const {
    const bool noise = (noiseSeed_ & 2) != 0;
    double sum = 0.0, l = 0.0, r = 0.0;
    for (int i = 0; i < 3; ++i) {
        const Chan& c = ch_[i];
        // Выключенные в смесителе тон и шум дают постоянный уровень — на этом
        // держится воспроизведение оцифровок через регистр громкости.
        const bool silent = (c.toggle && c.toneOn) || (noise && c.noiseOn);
        if (silent) continue;
        const double v = (ym_ ? kVolYm : kVolAy)[c.env ? envPtr_ : c.vol] / 65535.0 / 3.0;
        sum += v;
        l += v * kPanL[i];
        r += v * kPanR[i];
    }
    mono = sum; left = l; right = r;
}

void Ay8910::advance(double dtSec) {
    phase_ += dtSec * clock_ / 8.0;
    while (phase_ >= 1.0) {
        phase_ -= 1.0;
        step();
        double m, l, r;
        output(m, l, r);
        accSum_ += m; accL_ += l; accR_ += r;
        ++accN_;
    }
}

double Ay8910::takeSample() {
    double l, r;
    if (accN_) { last_ = accSum_ / accN_; accSum_ = accL_ = accR_ = 0.0; accN_ = 0; }
    else output(last_, l, r);
    return last_;
}

void Ay8910::takeSampleLR(double& left, double& right) {
    double m;
    if (accN_) { lastL_ = accL_ / accN_; lastR_ = accR_ / accN_; accSum_ = accL_ = accR_ = 0.0; accN_ = 0; }
    else output(m, lastL_, lastR_);
    left = lastL_; right = lastR_;
}

} // namespace bk
