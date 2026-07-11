#include "Screen.h"

namespace bk {

// 16 palettes x 4 colours, packed 0xAARRGGBB (hardware order: idx0..3).
// Taken from the BK-0010 palette set (palette 0 = black/blue/green/red).
static const uint32_t kPalettes[16][4] = {
    {0xFF000000, 0xFF0000FF, 0xFF00FF00, 0xFFFF0000}, // 0  black blue  green red
    {0xFF000000, 0xFFFFFF00, 0xFFFF00FF, 0xFFFF0000}, // 1
    {0xFF000000, 0xFF00FFFF, 0xFF0000FF, 0xFFFF00FF}, // 2
    {0xFF000000, 0xFF00FF00, 0xFF00FFFF, 0xFFFFFF00}, // 3
    {0xFF000000, 0xFFFF00FF, 0xFF00FFFF, 0xFFFFFFFF}, // 4
    {0xFF000000, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF}, // 5
    {0xFF000000, 0xFFC00000, 0xFF8E0000, 0xFFFF0000}, // 6
    {0xFF000000, 0xFFC0FF00, 0xFF8EFF00, 0xFFFFFF00}, // 7
    {0xFF000000, 0xFFC000FF, 0xFF8E00FF, 0xFFFF00FF}, // 8
    {0xFF000000, 0xFF8EFF00, 0xFF8E00FF, 0xFF8E0000}, // 9
    {0xFF000000, 0xFFC0FF00, 0xFFC000FF, 0xFFC00000}, // 10
    {0xFF000000, 0xFF00FFFF, 0xFFFFFF00, 0xFFFF0000}, // 11 CSI-DOS
    {0xFF000000, 0xFFFF0000, 0xFF00FF00, 0xFF00FFFF}, // 12
    {0xFF000000, 0xFF00FFFF, 0xFFFFFF00, 0xFFFFFFFF}, // 13
    {0xFF000000, 0xFFFFFF00, 0xFF00FF00, 0xFFFFFFFF}, // 14
    {0xFF000000, 0xFF00FFFF, 0xFF00FF00, 0xFFFFFFFF}, // 15
};

Screen::Screen() {
    for (auto& p : tex_) p = 0xFF000000;
}

void Screen::render(const Memory& mem) {
    const uint32_t* pal = kPalettes[palette_];
    const uint32_t white = 0xFFFFFFFF, black = 0xFF000000;
    // Scroll: register low byte gives the memory line shown at the top.
    // Default 0330 -> offset 0. Vertical scroll shifts the visible window.
    int scrollOffset = ((int)(scroll_ & 0377) - 0330) & 0377;

    for (int y = 0; y < TEX_H; ++y) {
        int memLine = (y + scrollOffset) & 0377;      // 0..255
        const uint8_t* row = mem.videoRam() + memLine * 64; // 64 bytes/line
        uint32_t* dst = tex_ + y * TEX_W;

        if (colorMode_) {
            // 256 pixels wide, 2 bits/pixel; double horizontally to fill 512.
            for (int bx = 0; bx < 64; ++bx) {
                uint8_t b = row[bx];
                for (int p = 0; p < 4; ++p) {
                    uint32_t c = pal[(b >> (p * 2)) & 3];
                    *dst++ = c;
                    *dst++ = c; // horizontal doubling
                }
            }
        } else {
            // 512 pixels wide, 1 bit/pixel (mono).
            for (int bx = 0; bx < 64; ++bx) {
                uint8_t b = row[bx];
                for (int p = 0; p < 8; ++p)
                    *dst++ = (b >> p) & 1 ? white : black;
            }
        }
    }
}

} // namespace bk
