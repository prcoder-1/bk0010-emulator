#pragma once
#include <cstdint>

namespace bk {

// Звуковой сопроцессор AY-3-8910 / AY-3-8912 / YM2149F. Логика — как у emu2149
// (М. Окадзаки) в редакции GID BKemu (devemu/emu2149.cpp): шаг расчёта — 8 тактов
// чипа; тон переключается каждые TP шагов (период 16·TP тактов), шум — каждые
// 2·NP шагов (17-битный сдвиговый регистр), огибающая — 32 ступени YM2149 по EP
// шагов. Выход — сумма трёх каналов по логарифмической таблице громкости YM2149,
// 0..1. Порты В-В (регистры 14, 15) хранятся, но никуда не ведут.
//
// На БК чип висит на параллельном порту 0177714: запись СЛОВОМ — номер регистра,
// БАЙТОМ — данные, оба инвертированы (см. Board::ioWrite, docs/bk0011m.md).
class Ay8910 {
public:
    static constexpr int DEFAULT_CLOCK = 1714286;   // 12 МГц / 7, как на «Мультизвуке»

    Ay8910() { reset(); }
    void setClock(int hz) { clock_ = hz > 0 ? hz : DEFAULT_CLOCK; }
    int  clock() const { return clock_; }
    void reset();

    void writeAddress(uint8_t a) { addr_ = a; }
    void writeData(uint8_t v) { writeReg(addr_, v); }
    void writeReg(uint8_t reg, uint8_t v);
    uint8_t address() const { return addr_; }
    const uint8_t* regs() const { return reg_; }

    // Продвинуть чип на dt секунд, копя выход для текущего отсчёта.
    void advance(double dtSec);
    // Средний выход (0..1) с прошлого вызова — один выходной отсчёт.
    double takeSample();

private:
    void step();
    double output() const;

    int clock_ = DEFAULT_CLOCK;
    uint8_t reg_[16] = {0};
    uint8_t addr_ = 0;
    struct Chan { int count = 0, freq = 1, vol = 0; bool env = false, toggle = false, toneOn = false, noiseOn = false; };
    Chan ch_[3];
    uint32_t noiseSeed_ = 1111;
    int noiseCount_ = 0, noiseFreq_ = 0;
    int envPtr_ = 0, envDir_ = 0, envFreq_ = 1, envCount_ = 0;
    bool envHold_ = false, envAlt_ = false, envLast_ = false;
    double phase_ = 0.0;        // доля шага, накопленная к следующему расчёту
    double accSum_ = 0.0;       // сумма выходов шагов текущего отсчёта
    int accN_ = 0;
    double last_ = 0.0;
};

} // namespace bk
