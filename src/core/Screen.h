#pragma once
#include <cstdint>
#include "Memory.h"

namespace bk {

// Renders the BK-0010 16 KB screen RAM into a 512x256 RGBA8888 texture.
//   Color mode : 256x256, 2 bits/pixel, 4 colours (each pixel doubled to fill 512 width)
//   Mono  mode : 512x256, 1 bit/pixel, black/white
// Palette 0 (BK-0010 hardware order): {black, blue, green, red}.
class Screen {
public:
    static constexpr int TEX_W = 512;
    static constexpr int TEX_H = 256;

    Screen();

    void setColorMode(bool color) { colorMode_ = color; }
    bool colorMode() const { return colorMode_; }
    void setPalette(int p) { palette_ = p & 15; }
    int  palette() const { return palette_; }
    void setScroll(uint16_t reg) { scroll_ = reg; }

    // Rebuild the RGBA texture from the current screen RAM contents.
    void render(const Memory& mem);

    const uint32_t* pixels() const { return tex_; }

private:
    uint32_t tex_[TEX_W * TEX_H];
    bool colorMode_ = true;
    int palette_ = 0;
    uint16_t scroll_ = 01330; // младший байт — скролл; бит 9 (01000) — полный/малый экран
};

} // namespace bk
