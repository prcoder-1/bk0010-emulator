#include "Az.h"
#include <algorithm>
#include <cstring>

namespace bk {

// Палитра после включения — таблица GID (AZBK.cpp, m_azPalette): 0–255 — 256 цветов,
// 256–319 — шестнадцать четырёхцветных палитр БК-0011М, 320–335 — 16 цветов,
// 336–337 — монохром.
static const uint32_t kPalette[Azbk::PALETTE] = {
    0xff000000, 0xff0f0f0f, 0xff101010, 0xff1f1f1f, 0xff202020, 0xff2f2f2f, 0xff303030, 0xff3f3f3f,
    0xff404040, 0xff4f4f4f, 0xff505050, 0xff5f5f5f, 0xff606060, 0xff6f6f6f, 0xff707070, 0xff7f7f7f,
    0xff808080, 0xff8f8f8f, 0xff909090, 0xff9f9f9f, 0xffa0a0a0, 0xffafafaf, 0xffb0b0b0, 0xffbfbfbf,
    0xffc0c0c0, 0xffcfcfcf, 0xffd0d0d0, 0xffdfdfdf, 0xffe0e0e0, 0xffefefef, 0xfff0f0f0, 0xffffffff,
    0xff000000, 0xff0000ff, 0xff00ff00, 0xff00ffff, 0xffff0000, 0xffff00ff, 0xffffff00, 0xffffffff,
    0xff000000, 0xff000033, 0xff000066, 0xff000099, 0xff0000cc, 0xff0000ff, 0xff003300, 0xff003333,
    0xff003366, 0xff003399, 0xff0033cc, 0xff0033ff, 0xff006600, 0xff006633, 0xff006666, 0xff006699,
    0xff0066cc, 0xff0066ff, 0xff009900, 0xff009933, 0xff009966, 0xff009999, 0xff0099cc, 0xff0099ff,
    0xff00cc00, 0xff00cc33, 0xff00cc66, 0xff00cc99, 0xff00cccc, 0xff00ccff, 0xff00ff00, 0xff00ff33,
    0xff00ff66, 0xff00ff99, 0xff00ffcc, 0xff00ffff, 0xff330000, 0xff330033, 0xff330066, 0xff330099,
    0xff3300cc, 0xff3300ff, 0xff333300, 0xff333333, 0xff333366, 0xff333399, 0xff3333cc, 0xff3333ff,
    0xff336600, 0xff336633, 0xff336666, 0xff336699, 0xff3366cc, 0xff3366ff, 0xff339900, 0xff339933,
    0xff339966, 0xff339999, 0xff3399cc, 0xff3399ff, 0xff33cc00, 0xff33cc33, 0xff33cc66, 0xff33cc99,
    0xff33cccc, 0xff33ccff, 0xff33ff00, 0xff33ff33, 0xff33ff66, 0xff33ff99, 0xff33ffcc, 0xff33ffff,
    0xff660000, 0xff660033, 0xff660066, 0xff660099, 0xff6600cc, 0xff6600ff, 0xff663300, 0xff663333,
    0xff663366, 0xff663399, 0xff6633cc, 0xff6633ff, 0xff666600, 0xff666633, 0xff666666, 0xff666699,
    0xff6666cc, 0xff6666ff, 0xff669900, 0xff669933, 0xff669966, 0xff669999, 0xff6699cc, 0xff6699ff,
    0xff66cc00, 0xff66cc33, 0xff66cc66, 0xff66cc99, 0xff66cccc, 0xff66ccff, 0xff66ff00, 0xff66ff33,
    0xff66ff66, 0xff66ff99, 0xff66ffcc, 0xff66ffff, 0xff990000, 0xff990033, 0xff990066, 0xff990099,
    0xff9900cc, 0xff9900ff, 0xff993300, 0xff993333, 0xff993366, 0xff993399, 0xff9933cc, 0xff9933ff,
    0xff996600, 0xff996633, 0xff996666, 0xff996699, 0xff9966cc, 0xff9966ff, 0xff999900, 0xff999933,
    0xff999966, 0xff999999, 0xff9999cc, 0xff9999ff, 0xff99cc00, 0xff99cc33, 0xff99cc66, 0xff99cc99,
    0xff99cccc, 0xff99ccff, 0xff99ff00, 0xff99ff33, 0xff99ff66, 0xff99ff99, 0xff99ffcc, 0xff99ffff,
    0xffcc0000, 0xffcc0033, 0xffcc0066, 0xffcc0099, 0xffcc00cc, 0xffcc00ff, 0xffcc3300, 0xffcc3333,
    0xffcc3366, 0xffcc3399, 0xffcc33cc, 0xffcc33ff, 0xffcc6600, 0xffcc6633, 0xffcc6666, 0xffcc6699,
    0xffcc66cc, 0xffcc66ff, 0xffcc9900, 0xffcc9933, 0xffcc9966, 0xffcc9999, 0xffcc99cc, 0xffcc99ff,
    0xffcccc00, 0xffcccc33, 0xffcccc66, 0xffcccc99, 0xffcccccc, 0xffccccff, 0xffccff00, 0xffccff33,
    0xffccff66, 0xffccff99, 0xffccffcc, 0xffccffff, 0xffff0000, 0xffff0033, 0xffff0066, 0xffff0099,
    0xffff00cc, 0xffff00ff, 0xffff3300, 0xffff3333, 0xffff3366, 0xffff3399, 0xffff33cc, 0xffff33ff,
    0xffff6600, 0xffff6633, 0xffff6666, 0xffff6699, 0xffff66cc, 0xffff66ff, 0xffff9900, 0xffff9933,
    0xffff9966, 0xffff9999, 0xffff99cc, 0xffff99ff, 0xffffcc00, 0xffffcc33, 0xffffcc66, 0xffffcc99,
    0xffffcccc, 0xffffccff, 0xffffff00, 0xffffff33, 0xffffff66, 0xffffff99, 0xffffffcc, 0xffffffff,
    0xff000000, 0xff0000ff, 0xff00ff00, 0xffff0000, 0xff000000, 0xffffff00, 0xffff00ff, 0xffff0000,
    0xff000000, 0xff00ffff, 0xff0000ff, 0xffff00ff, 0xff000000, 0xff00ff00, 0xff00ffff, 0xffffff00,
    0xff000000, 0xffff00ff, 0xff00ffff, 0xffffffff, 0xff000000, 0xffffffff, 0xffffffff, 0xffffffff,
    0xff000000, 0xffbf0000, 0xff9f0000, 0xffff0000, 0xff000000, 0xffbfff00, 0xff9fff00, 0xffffff00,
    0xff000000, 0xffbf00ff, 0xff9f00ff, 0xffff00ff, 0xff000000, 0xff9fff00, 0xff9f00ff, 0xff9f0000,
    0xff000000, 0xffb8ff00, 0xffbf00ff, 0xffbf0000, 0xff000000, 0xff00ffff, 0xffffff00, 0xffff0000,
    0xff000000, 0xffff0000, 0xff00ff00, 0xff00ffff, 0xff000000, 0xff00ffff, 0xffffff00, 0xffffffff,
    0xff000000, 0xffffff00, 0xff00ff00, 0xffffffff, 0xff000000, 0xff00ffff, 0xff00ff00, 0xffffffff,
    0xff000000, 0xff0000aa, 0xff00aa00, 0xff00aaaa, 0xffaa0000, 0xffaa00aa, 0xffaa5500, 0xffaaaaaa,
    0xff555555, 0xff0000ff, 0xff00ff00, 0xff00ffff, 0xffff0000, 0xffff00ff, 0xffffff00, 0xffffffff,
    0xff000080, 0xffffffff,
};

void Azbk::resetPalette() { std::memcpy(pal_, kPalette, sizeof pal_); }

// 0177230: разряды 2:0 — режим (бит/точку, 4–7 — со слоями), 4:3 — длина строки 32…256
// слов, 7:6 — растяжение по X ×1/×2/×4/×8, 10:9 — повтор строк ×1…×4, 11 — страницы слоёв
// фиксируются на кадр, 15:12 — длина рулона (12 значений).
void Azbk::setMode(uint16_t v) {
    scrCsr_ = v;
}

namespace {
const int kLlen[4] = {32, 64, 128, 256};
const int kXs[4] = {1, 2, 4, 8};
const int kYs[4] = {1, 2, 3, 4};
const int kRoll[12] = {6144, 8192, 12288, 16384, 24576, 32768, 49152, 65536, 98304, 131072, 196608, 262144};
}

// Строка VGA (GID MakeScreenLine). Режим, длина строки, растяжения, рулон и скроллы
// фиксируются в начале кадра; страницы слоёв — тоже, если задан разряд 11, иначе живые
// (перелистывание страниц видно сразу). На строке 769 — готовый кадр, кадровое
// прерывание 60 Гц и автоматический пуск блиттера.
void Azbk::vgaLine() {
    if (vline_ == 0) {
        const uint16_t v = scrCsr_;
        sync_ = (v & 04000) != 0;
        fr_.mode = v & 7;
        fr_.llen = kLlen[(v >> 3) & 3];
        fr_.xs = kXs[(v >> 6) & 3];
        fr_.ys = kYs[(v >> 9) & 3];
        fr_.lines = kRoll[std::min(11, (v >> 12) & 15)] / fr_.llen;
        for (int i = 0; i < 3; ++i) { fr_.vscr[i] = vscr_[i]; fr_.hscr[i] = hscr_[i]; }
        fr_.page[0] = pageL0_; fr_.page[1] = pageL1_; fr_.page[2] = pageL2_;
        srcLine_ = 0; rep_ = 0;
    }
    if (vline_ < VGA_H) {
        if (rep_ == 0) {
            // Малый экран БК (бит 9 0177664 = 0): строки изображения за 192-й гаснут.
            if (smallScreen_ && srcLine_ >= 192) std::fill(std::begin(lineBuf_), std::end(lineBuf_), 0xff000000u);
            else renderLine(lineBuf_);
        }
        std::memcpy(back_.data() + size_t(vline_) * VGA_W, lineBuf_, sizeof lineBuf_);
        if (++rep_ >= fr_.ys) { rep_ = 0; ++srcLine_; }
    }
    ++vline_;
    blt_.line();
    if (vline_ == 769) {
        blt_.frameEnd();
        std::swap(back_, front_);
        ++frames_;
        if ((mmu_ & (MMU_V100 | MMU_50HZ)) == MMU_V100 && irq2) irq2();
    }
    if (vline_ >= VGA_LINES) vline_ = 0;
}

void Azbk::renderLine(uint32_t* out) {
    const int llen = fr_.llen;
    const uint16_t pages[3] = {sync_ ? fr_.page[0] : pageL0_, sync_ ? fr_.page[1] : pageL1_, sync_ ? fr_.page[2] : pageL2_};
    // Слой k: начало строки (адрес слова) и первое слово по горизонтальному скроллу.
    uint32_t row[3];
    int idx[3];
    for (int k = 0; k < 3; ++k) {
        const uint32_t r = static_cast<uint32_t>((srcLine_ + fr_.vscr[k]) % fr_.lines);
        row[k] = (uint32_t(pages[k]) << 11) + uint32_t(llen) * r;
        idx[k] = fr_.hscr[k] % llen;
    }
    const int layers = (fr_.mode & 4) ? 3 : 1;
    const int bpp = 1 << (fr_.mode & 3);
    const int per = 16 / bpp;
    const uint16_t pmask = static_cast<uint16_t>((1 << bpp) - 1);
    const uint32_t* legacy = &pal_[256 + legacyPal_ * 4];
    const uint32_t* legacy2 = &pal_[256 + ((legacyPal_ + 1) & 15) * 4];
    const uint32_t* legacy3 = &pal_[256 + ((legacyPal_ + 2) & 15) * 4];
    int x = 0;
    const int xs = fr_.xs;
    for (int c = 0; c < llen && x < VGA_W; ++c) {
        uint16_t w[3] = {0, 0, 0};
        for (int k = 0; k < layers; ++k) {
            w[k] = vword(row[k] + uint32_t(idx[k]));
            idx[k] = (idx[k] + 1) % llen;
        }
        for (int i = 0; i < per && x < VGA_W; ++i) {
            const unsigned p1 = w[0] & pmask, p2 = w[1] & pmask, p3 = w[2] & pmask;
            w[0] >>= bpp; w[1] >>= bpp; w[2] >>= bpp;
            uint32_t px;
            switch (fr_.mode) {
            default:
            case 0: px = pal_[336 + p1]; break;
            case 1: px = legacy[p1]; break;
            case 2: px = pal_[320 + p1]; break;
            case 3: px = pal_[p1]; break;
            case 4: px = pal_[(p1 << 2) | (p2 << 1) | p3]; break;
            case 5: px = p1 ? legacy[p1] : p2 ? legacy2[p2] : legacy3[p3]; break;
            case 6: px = p1 ? pal_[p1] : p2 ? pal_[16 + p2] : pal_[32 + p3]; break;
            case 7: px = pal_[p1 ? p1 : p2 ? p2 : p3]; break;
            }
            for (int s = 0; s < xs && x < VGA_W; ++s) out[x++] = px;
        }
    }
    while (x < VGA_W) out[x++] = 0xff000000u;
}

// АР2+ЛАТ: старый экран БК — 512 точек монохромом (×2) или 256 цветом (×4).
void Azbk::ar2Lat() {
    if (scrCsr_ == 012201) setMode(012100);
    else if (scrCsr_ == 012100) setMode(012201);
}

// Для снимка экрана (команда 044): теневые регистры видео в слове 0x1F000 (режим — слово
// 29, страницы слоёв — 30–32) и первые 256 ячеек палитры в 0x1F100.
void Azbk::prepareScreenshot() {
    for (uint32_t i = 0; i < 512; ++i) setMemWord((0x1F000 + i) * 2, 0);
    setMemWord((0x1F000 + 29) * 2, scrCsr_);
    setMemWord((0x1F000 + 30) * 2, pageL0_);
    setMemWord((0x1F000 + 31) * 2, pageL1_);
    setMemWord((0x1F000 + 32) * 2, pageL2_);
    for (uint32_t i = 0; i < 256; ++i) {
        const uint32_t c = pal_[i];
        setMemWord((0x1F100 + i) * 2, static_cast<uint16_t>((((c >> 19) & 037) << 10) | (((c >> 11) & 037) << 5) | ((c >> 3) & 037)));
    }
}

// Время для платы: строки VGA (806 строк × 60 Гц = 48360 в секунду) и отсчёты DMA
// (44100 Гц), точной дробью от тактов ЦП.
void Azbk::advance(int ticks) {
    if (!on() || ticks <= 0) return;
    lineAcc_ += uint64_t(ticks) * uint64_t(VGA_LINES * VGA_HZ);
    while (lineAcc_ >= uint64_t(cpuHz_)) { lineAcc_ -= uint64_t(cpuHz_); vgaLine(); }
    if (dmaPlaying_) {
        dmaAcc_ += uint64_t(ticks) * 44100u;
        while (dmaAcc_ >= uint64_t(cpuHz_)) { dmaAcc_ -= uint64_t(cpuHz_); dmaSample(); }
    } else {
        dmaAcc_ = 0;
    }
}

} // namespace bk
