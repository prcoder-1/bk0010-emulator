#pragma once
#include <algorithm>
#include <cstdint>
#include <cstdlib>

namespace bk {

// Мышь «Марсианка» (УВК-01, МОКБ «Марс») на параллельном порту 0177714 (БК-docs §13.4).
// Шаровой манипулятор: квадратурные датчики взводят D-триггеры направления платы УЛ УВК,
// выход ВД03 порта (разряд 3 записи) — их «СБРОС». На чтение (активный уровень 1, как у
// джойстика): 001 вверх, 002 вправо, 004 вниз, 010 влево, 040 левая кнопка, 0100 правая
// (распиновка адаптера PS/2→MARS TheGWBV; CSI-DOS 3 разбирает так же).
//
// Программа читает порт и сбрасывает триггеры импульсом 0 на разряде 3 (CSI-DOS: чтение,
// CLR @#177714, MOV #10,@#177714), так что за опрос курсор сдвигается на шаг по оси.
// Движение мыши хоста копится в шагах шара; флаг оси стоит, пока шаги есть, а каждый спад
// разряда 3 расходует по шагу. Игры, читающие мышь как джойстик и «СБРОС» не дающие вовсе,
// получают по шагу на кадр, в котором они порт читали, — иначе направление залипло бы, а
// при трате по кадру игра, опрашивающая раз в 2–3 кадра (MANIAC), теряла бы шаги. Как только
// программа хоть раз дала «СБРОС», шаги сами не убывают: между опросами она может кадрами
// рисовать или подолгу ждать ввода (Chess Club превращает шаги в нажатия стрелок и в цикле
// ожидания «СБРОС» не даёт).
class MarsMouse {
public:
    static constexpr int MAX_PENDING = 64;    // шагов в запасе по оси (дальше — потеря, как у шара)

    void setEnabled(bool on) { on_ = on; clear(); }
    bool enabled() const { return on_; }
    void clear() { dx_ = dy_ = 0; protocol_ = seen_ = seenPrev_ = false; }
    bool protocol() const { return protocol_; }   // программа давала «СБРОС»

    // Шаги шара: dx > 0 — вправо, dy > 0 — вниз.
    void move(int dx, int dy) {
        dx_ = std::clamp(dx_ + dx, -MAX_PENDING, MAX_PENDING);
        dy_ = std::clamp(dy_ + dy, -MAX_PENDING, MAX_PENDING);
    }
    void setButtons(bool left, bool right) { buttons_ = static_cast<uint16_t>((left ? 040 : 0) | (right ? 0100 : 0)); }
    uint16_t buttons() const { return buttons_; }
    int pendingX() const { return dx_; }
    int pendingY() const { return dy_; }

    uint16_t bits() const {
        if (!on_) return 0;
        return static_cast<uint16_t>(buttons_ | (dy_ < 0 ? 001 : 0) | (dx_ > 0 ? 002 : 0)
                                     | (dy_ > 0 ? 004 : 0) | (dx_ < 0 ? 010 : 0));
    }
    // Запись в 0177714: спад разряда 3 — «СБРОС» триггеров (шаг по каждой оси израсходован).
    void write(uint16_t v) {
        const bool strobe = v & 010;
        if (strobe_ && !strobe) { step(); protocol_ = true; }
        strobe_ = strobe;
    }
    // Чтение 0177714 программой: шаг, стоявший на линиях, она увидела.
    void noteRead() { if (dx_ || dy_) seen_ = true; }
    // Раз в кадр: программа «СБРОС» не даёт — увиденный шаг расходуется кадром позже: опрос
    // из двух чтений подряд (MANIAC: порт, затем joy_poll) может попасть на границу кадра.
    void frame() {
        if (on_ && !protocol_ && seenPrev_) step();
        seenPrev_ = seen_;
        seen_ = false;
    }

private:
    void step() {
        if (dx_) dx_ += dx_ > 0 ? -1 : 1;
        if (dy_) dy_ += dy_ > 0 ? -1 : 1;
    }
    bool on_ = false, strobe_ = false, protocol_ = false, seen_ = false, seenPrev_ = false;
    int dx_ = 0, dy_ = 0;
    uint16_t buttons_ = 0;
};

} // namespace bk
