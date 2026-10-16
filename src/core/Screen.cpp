#include "Screen.h"

namespace bk {

// 16 палитр БК-0011М x 4 цвета, 0xAARRGGBB, по значению пары бит точки 00/01/10/11.
// Значения — GID BKemu (devemu/ScreenColors_Shared.cpp:4-23), совпадают с таблицей
// [Багаев] в БК-docs §20.11. Палитра 0 — фиксированные цвета БК-0010.
static const uint32_t kPalettes[16][4] = {
    {0xFF000000, 0xFF0000FF, 0xFF00FF00, 0xFFFF0000}, // 0  black blue  green red
    {0xFF000000, 0xFFFFFF00, 0xFFFF00FF, 0xFFFF0000}, // 1
    {0xFF000000, 0xFF00FFFF, 0xFF0000FF, 0xFFFF00FF}, // 2
    {0xFF000000, 0xFF00FF00, 0xFF00FFFF, 0xFFFFFF00}, // 3
    {0xFF000000, 0xFFFF00FF, 0xFF00FFFF, 0xFFFFFFFF}, // 4
    {0xFF000000, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF}, // 5
    {0xFF000000, 0xFFAA0000, 0xFF700000, 0xFFFF0000}, // 6
    {0xFF000000, 0xFFAAFF00, 0xFF55FF00, 0xFFFFFF00}, // 7
    {0xFF000000, 0xFFAA00FF, 0xFF7000FF, 0xFFFF00FF}, // 8
    {0xFF000000, 0xFF55FF00, 0xFF7000FF, 0xFF700000}, // 9
    {0xFF000000, 0xFFAAFF00, 0xFFAA00FF, 0xFFAA0000}, // 10
    {0xFF000000, 0xFF00FFFF, 0xFFFFFF00, 0xFFFF0000}, // 11 CSI-DOS
    {0xFF000000, 0xFFFF0000, 0xFF00FF00, 0xFF00FFFF}, // 12
    {0xFF000000, 0xFF00FFFF, 0xFFFFFF00, 0xFFFFFFFF}, // 13
    {0xFF000000, 0xFFFFFF00, 0xFF00FF00, 0xFFFFFFFF}, // 14
    {0xFF000000, 0xFF00FFFF, 0xFF00FF00, 0xFFFFFFFF}, // 15
};

Screen::Screen() {
    for (auto& p : tex_) p = 0xFF000000;
}

// Отрисовать ОДНУ строку экрана значением регистра смещения, каким оно было, когда
// луч эту строку проходил. Отсюда берутся построчные эффекты (параллакс, «разрезанный»
// экран): при покадровой отрисовке весь кадр рисуется последним значением скролла,
// и всё, что игра меняла в середине кадра, теряется.
void Screen::renderSpan(const Memory& mem, int y, uint16_t scrollReg, int fromWord, int toWord) {
    if (y < 0 || y >= TEX_H) return;
    if (fromWord < 0) fromWord = 0;
    if (toWord > WORDS_PER_LINE) toWord = WORDS_PER_LINE;
    if (fromWord >= toWord) return;
    const uint32_t* pal = kPalettes[palette_];
    const uint32_t white = 0xFFFFFFFF, black = 0xFF000000;
    // Scroll: register low byte gives the memory line shown at the top.
    // Default 0330 -> offset 0. Vertical scroll shifts the visible window.
    const int scrollOffset = ((int)(scrollReg & 0377) - 0330) & 0377;
    // Бит 9 (01000) — режим экрана: установлен = полный экран (256 строк,
    // 040000..077777); сброшен = «малый экран» (режим расширения памяти) — видны
    // только 64 строки окна прокрутки (при штатной настройке это 070000..077777),
    // остальные 3/4 экрана — чёрные, а 040000..067777 отданы под ОЗУ пользователя.
    const int nlines = (scrollReg & 01000) ? TEX_H : 64;

    // Слово — 16 точек текстуры (8 цветных, удвоенных, или 16 монохромных).
    uint32_t* dst = tex_ + y * TEX_W + fromWord * 16;
    if (y >= nlines) {                            // малый экран: низ — чёрный
        for (int x = fromWord * 16; x < toWord * 16; ++x) *dst++ = black;
        return;
    }
    const int memLine = (y + scrollOffset) & 0377;      // 0..255
    const uint8_t* row = mem.videoRam() + memLine * 64; // 64 bytes/line

    for (int bx = fromWord * 2; bx < toWord * 2; ++bx) {
        const uint8_t b = row[bx];
        if (colorMode_) {
            // 256 pixels wide, 2 bits/pixel; double horizontally to fill 512.
            for (int p = 0; p < 4; ++p) {
                const uint32_t c = pal[(b >> (p * 2)) & 3];
                *dst++ = c;
                *dst++ = c; // horizontal doubling
            }
        } else {
            // 512 pixels wide, 1 bit/pixel (mono).
            for (int p = 0; p < 8; ++p) *dst++ = (b >> p) & 1 ? white : black;
        }
    }
}

void Screen::render(const Memory& mem) {
    for (int y = 0; y < TEX_H; ++y) renderLine(mem, y, scroll_);
}

} // namespace bk
