#pragma once
#include <cstdint>
#include <vector>

namespace bk {

// Блиттер AZБК (0177270/0177272). Пачка команд по 8 слов читается в буфер целиком и
// исполняется после конца кадра VGA (строка 769) либо по ручному пуску. Шаг — как у
// GID AZ_Blitter и retro-tang-bk azblit.v: операция над словом, ширина, высота
// (0 строк — ничего), шаг строки, зеркалирование и перестановка байтов. За строку VGA
// блиттер успевает WORDS_PER_LINE слов (у GID — 100).
//
// Слово команды: 1 — {ст. байт адреса SRC, ст. байт DST}, 2 — мл. слово SRC, 3 — мл.
// слово DST, 4 — команда (бит 0 читать SRC, 1 читать DST, 2 NOP, 5:3 операция, 6
// перестановка байтов, 9/10 зеркало по X/Y), 5 — {строк, ширина−1}, 6 — шаг строки
// (мл. байт), 7 — Y (DST += Y·256), 8 — {константа DST, константа SRC}.
class AzBlitter {
public:
    static constexpr uint16_t RUN = 0100000, MANUAL = 040000, START = 010000, READING = 01000;
    static constexpr int WORDS_PER_LINE = 100;

    void attach(uint16_t* mem, uint32_t words) { mem_ = mem; mask_ = words - 1; }
    void reset();
    uint16_t readCtrl() const;
    void writeCtrl(uint16_t v);
    void writePage(uint16_t v) { page_ = v & 017777; }
    uint16_t page() const { return page_; }
    void frameEnd();                     // строка 769: автоматический пуск
    void line();                         // время одной строки VGA
    bool running() const { return running_; }
    uint64_t runs() const { return runs_; }

    void saveState(std::vector<uint8_t>& out) const;
    bool loadState(const uint8_t*& p, const uint8_t* end);

private:
    void start(int n);
    bool fetch();                        // следующая команда; false — пачка кончилась
    void step();                         // одно слово текущей команды

    uint16_t* mem_ = nullptr;
    uint32_t mask_ = 0;
    uint16_t count_ = 0;                 // команд в пачке (9 разрядов)
    bool manual_ = false;
    uint16_t page_ = 0;
    bool running_ = false;
    std::vector<uint16_t> buf_ = std::vector<uint16_t>(511 * 8, 0);
    int n_ = 0, i_ = 0;                  // команд в пачке, номер следующей
    uint16_t c_[8] = {0};
    bool active_ = false;                // текущая команда ещё идёт
    uint32_t dst_ = 0, src_ = 0;
    int wLeft_ = 0, hLeft_ = 0;
    uint16_t s_ = 0, d_ = 0;             // последние прочитанные SRC и DST (держатся, как в ПЛИС)
    uint64_t runs_ = 0;
};

} // namespace bk
