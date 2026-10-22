#pragma once
#include <cstdint>

namespace bk {

// Звуковой сопроцессор AY-3-8910 / AY-3-8912 / YM2149F. Логика — как у emu2149
// (М. Окадзаки) в редакции GID BKemu (devemu/emu2149.cpp): шаг расчёта — 8 тактов
// чипа; тон переключается каждые TP шагов (период 16·TP тактов), шум — каждые
// 2·NP шагов (17-битный сдвиговый регистр), огибающая — 32 ступени YM2149 по EP
// шагов. Выход — сумма трёх каналов по логарифмической таблице громкости YM2149,
// 0..1, и стерео по схеме ABC (A — влево, B — в центр, C — вправо) с коэффициентами
// GID (см. kPanL/kPanR в Ay8910.cpp). Порты В-В (регистры 14, 15) хранятся, но никуда
// не ведут.
//
// На БК чип висит на параллельном порту 0177714: запись СЛОВОМ — номер регистра,
// БАЙТОМ — данные, оба инвертированы (см. Board::ioWrite, docs/bk0011m.md).
class Ay8910 {
public:
    static constexpr int DEFAULT_CLOCK = 1714286;   // 12 МГц / 7, как на «Мультизвуке»

    Ay8910() { reset(); }
    void setClock(int hz) { clock_ = hz > 0 ? hz : DEFAULT_CLOCK; }
    // Модель чипа: YM2149 (по умолчанию) — 32 ступени громкости, AY-3-8910 — 16 (ступень
    // повторена дважды, огибающая тоже идёт 16 ступенями). Таблицы — emu2149 GID.
    // Сброс чипа модель не меняет.
    void setYm(bool ym) { ym_ = ym; }
    bool ym() const { return ym_; }
    int  clock() const { return clock_; }
    void reset();

    void writeAddress(uint8_t a) { addr_ = a; }
    void writeData(uint8_t v) { writeReg(addr_, v); }
    void writeReg(uint8_t reg, uint8_t v);
    uint8_t address() const { return addr_; }
    const uint8_t* regs() const { return reg_; }

    // Продвинуть чип на dt секунд, копя выход для текущего отсчёта.
    void advance(double dtSec);
    // Средний выход (0..1) с прошлого вызова — один выходной отсчёт (моно).
    double takeSample();
    // То же в стерео ABC: левый и правый канал, каждый до (1,149+0,75+0,249)/3 ≈ 0,716.
    // Оба вызова забирают накопленное — звуку нужен один из них (Speaker берёт этот).
    void takeSampleLR(double& left, double& right);

private:
    void step();
    void output(double& mono, double& left, double& right) const;

    int clock_ = DEFAULT_CLOCK;
    bool ym_ = true;
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
    double accL_ = 0.0, accR_ = 0.0;
    int accN_ = 0;
    double last_ = 0.0, lastL_ = 0.0, lastR_ = 0.0;
};

} // namespace bk
