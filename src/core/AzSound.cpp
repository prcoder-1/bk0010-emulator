#include "Az.h"
#include <algorithm>

namespace bk {

// DMA-звук (0177160–0177170): 0177160 — разряд 0 пуск, 1 однократно, 2 стоп, 3 готово
// (по чтению), 4 держать состояние ADPCM на повторе, 11:9 формат: 0 — PCM 16 бит моно,
// 4 — IMA ADPCM моно (полубайт на отсчёт, младший первый), 5 — IMA ADPCM стерео (младший
// полубайт — левый, старший — правый); 0177162 — страница начала, 0177164/0177166 — длина
// в словах (24 разряда), 0177170 — текущая страница. 44100 Гц. Ход — как в
// retro-tang-bk azsound.v, декодер — стандартный IMA (GID shared/ima.cpp).
void Azbk::dmaWrite(uint16_t v) {
    dmaCsr_ = v;
    dmaDone_ = false;
    if (v & 4) {
        dmaPlaying_ = false;
    } else if ((v & 1) && !(v & 010)) {
        dmaPlaying_ = true;
        dmaLeft_ = dmaLen_;
        dmaAddr_ = uint32_t(dmaPage_) << 11;
        dmaCur_ = dmaPage_;
        dmaUnits_ = 0;
        imaCur_[0] = imaCur_[1] = imaIdx_[0] = imaIdx_[1] = 0;
    }
    if (!dmaPlaying_) { pcmL_ = pcmR_ = 0; soundOut(); }
}

int16_t Azbk::imaDecode(int ch, int n) {
    static const int kStep[89] = {
        7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88, 97,
        107, 118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796,
        876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428,
        4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350,
        22385, 24623, 27086, 29794, 32767};
    static const int kIndex[8] = {-1, -1, -1, -1, 2, 4, 6, 8};
    const int step = kStep[imaIdx_[ch]];
    int diff = step >> 3;
    if (n & 1) diff += step >> 2;
    if (n & 2) diff += step >> 1;
    if (n & 4) diff += step;
    imaCur_[ch] = std::clamp(imaCur_[ch] + ((n & 8) ? -diff : diff), -32768, 32767);
    imaIdx_[ch] = std::clamp(imaIdx_[ch] + kIndex[n & 7], 0, 88);
    return static_cast<int16_t>(imaCur_[ch]);
}

void Azbk::dmaSample() {
    const int mode = (dmaCsr_ >> 9) & 7;
    if (dmaUnits_ == 0) {
        if (dmaLeft_ > 0) {
            const uint16_t w = memWord(dmaAddr_ * 2);
            ++dmaAddr_; --dmaLeft_;
            dmaCur_ = static_cast<uint16_t>((dmaAddr_ >> 11) & 017777);
            if (mode == 4) { pcmL_ = pcmR_ = imaDecode(0, w & 017); dmaWord_ = static_cast<uint16_t>(w >> 4); dmaUnits_ = 3; }
            else if (mode == 5) { pcmL_ = imaDecode(0, w & 017); pcmR_ = imaDecode(1, (w >> 4) & 017); dmaWord_ = static_cast<uint16_t>(w >> 8); dmaUnits_ = 1; }
            else { pcmL_ = pcmR_ = static_cast<int16_t>(w); }
        } else if (dmaCsr_ & 2) {
            dmaDone_ = true;                               // однократно — конец
            dmaPlaying_ = false;
            pcmL_ = pcmR_ = 0;
        } else {
            dmaLeft_ = dmaLen_;                            // по кругу
            dmaAddr_ = uint32_t(dmaPage_) << 11;
            dmaCur_ = dmaPage_;
            if (!(dmaCsr_ & 020)) imaCur_[0] = imaCur_[1] = imaIdx_[0] = imaIdx_[1] = 0;
        }
    } else {
        --dmaUnits_;
        if (mode == 4) { pcmL_ = pcmR_ = imaDecode(0, dmaWord_ & 017); dmaWord_ >>= 4; }
        else if (mode == 5) { pcmL_ = imaDecode(0, dmaWord_ & 017); pcmR_ = imaDecode(1, (dmaWord_ >> 4) & 017); dmaWord_ >>= 8; }
    }
    soundOut();
}

// Выход ЦАП платы: Covox (16 бит без знака, середина — 0100000) плюс DMA (16 бит со
// знаком). Постоянную составляющую снимает микшер.
void Azbk::soundOut() {
    if (!sound) return;
    const double l = (cvxL_ - 32768.0) / 32768.0 + pcmL_ / 32768.0;
    const double r = (cvxR_ - 32768.0) / 32768.0 + pcmR_ / 32768.0;
    sound(std::clamp(l, -1.0, 1.0), std::clamp(r, -1.0, 1.0));
}

} // namespace bk
