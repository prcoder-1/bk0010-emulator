#include "Vp037.h"

namespace bk {

void Vp037::reset() {
    // posedge RESET в va_037.v: PC<=0, VA[5:1]<=0, HGATE<=0, VGATE<=1, LC<=0xFF.
    pc_ = 0;
    va51_ = 0;
    lc_ = 0xFF;
    hgate_ = false;
    vgate_ = true;
}

void Vp037::syncToFrameTop() {
    // Начало видимого поля: VGATE=0 (видимо), LC=0xFF (LC5:0=63, LC7:6=11 — первый
    // 64-строчный банд, где активна и видеовыборка малого экрана), PC/VA/HGATE=0.
    // Прогон отсюда даёт ровно 256 видимых строк, затем 64 строки гашения.
    pc_ = 0;
    va51_ = 0;
    lc_ = 0xFF;
    hgate_ = false;
    vgate_ = false;
}

// Один такт CLKIN = один negedge-фронт в va_037.v. Все счётчики обновляются
// одновременно из ТЕКУЩИХ (до-фронтовых) значений — как nonblocking-присваивания.
void Vp037::tick(int clkin) {
    for (int i = 0; i < clkin; ++i) {
        // Текущие значения (до фронта)
        const uint8_t va41 = va51_ & 017;         // VA[4:1]
        const uint8_t va5  = (va51_ >> 4) & 1;     // VA[5]
        const uint8_t lc50 = lc_ & 077;            // LC[5:0]
        const uint8_t lc6  = (lc_ >> 6) & 1;       // LC[6]
        const uint8_t lc7  = (lc_ >> 7) & 1;       // LC[7]
        const bool    pcFull = (pc_ == 7);         // &PC[2:0]

        // TV51 = ~(&PC & &VA[5:1]); ~TV51 (конец строки) при PC==7 и VA[5:1]==31.
        const bool endLine = pcFull && (va51_ == 037);

        // --- PC[2:0] ---
        const uint8_t pc_n = (pc_ + 1) & 7;

        // --- VA[5:1] --- (инкремент раз в слово, при &PC)
        uint8_t va41_n = va41, va5_n = va5;
        if (pcFull) {
            va41_n = (va41 + 1) & 017;
            if (va41 == 017 && !hgate_) va5_n ^= 1;
        }

        // --- HGATE --- переключается в конце 16-словной группы
        bool hgate_n = hgate_;
        if (pcFull && va41 == 017 && (hgate_ || va5)) hgate_n = !hgate_;

        // --- VGATE --- переключается по концу строки при LC[5:0]==0
        bool vgate_n = vgate_;
        if (endLine && lc50 == 0 && (vgate_ || ((lc_ & 0300) == 0))) vgate_n = !vgate_;

        // --- LC[7:0] --- убывает раз в строку (~TV51)
        uint8_t lc_n = lc_;
        if (endLine) {
            const uint8_t lc50_n = (lc50 - 1) & 077;
            uint8_t nlc6 = lc6, nlc7 = lc7;
            if (lc50 == 0 && !vgate_) nlc6 ^= 1;   // LC[6] при LC[5:0]==0 & ~VGATE
            if ((lc_ & 0177) == 0)    nlc7 ^= 1;   // LC[7] при LC[6:0]==0
            lc_n = static_cast<uint8_t>((nlc7 << 7) | (nlc6 << 6) | lc50_n);
        }

        // Коммит
        pc_    = pc_n;
        va51_  = static_cast<uint8_t>((va5_n << 4) | va41_n);
        hgate_ = hgate_n;
        vgate_ = vgate_n;
        lc_    = lc_n;
    }
}

} // namespace bk
