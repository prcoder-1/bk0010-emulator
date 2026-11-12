#include "GigaScreen.h"
#include <cmath>

namespace bk {

namespace {
// Переход sRGB <-> линейный свет таблицами, свет — 16 бит.
struct Gamma {
    uint16_t lin[256];
    uint8_t  srgb[65536];
    Gamma() {
        for (int i = 0; i < 256; ++i) {
            const double c = i / 255.0;
            const double l = c <= 0.04045 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4);
            lin[i] = static_cast<uint16_t>(std::lround(l * 65535.0));
        }
        for (int i = 0; i < 65536; ++i) {
            const double l = i / 65535.0;
            const double c = l <= 0.0031308 ? 12.92 * l : 1.055 * std::pow(l, 1 / 2.4) - 0.055;
            srgb[i] = static_cast<uint8_t>(std::lround(std::fmin(1.0, c) * 255.0));
        }
    }
};
const Gamma& gamma() { static const Gamma g; return g; }
} // namespace

uint32_t GigaScreen::mix(uint32_t a, uint32_t b) {
    if (a == b) return a;
    const Gamma& g = gamma();
    uint32_t out = 0xFF000000;
    for (int sh = 0; sh < 24; sh += 8) {
        const int s = g.lin[(a >> sh) & 255] + g.lin[(b >> sh) & 255];   // 2 x среднее
        out |= static_cast<uint32_t>(g.srgb[s >> 1]) << sh;
    }
    return out;
}

GigaScreen::Frame& GigaScreen::next() {
    head_ = (head_ + 1) % 3;
    if (frames_ < 3) ++frames_;
    return hist_[head_];
}

void GigaScreen::push(const uint32_t* frame) {
    Frame& f = next();
    f.px.assign(frame, frame + W * H);
    f.drawn = true;
}

void GigaScreen::push(const Screen& how, const uint8_t* vram) {
    Frame& f = next();
    f.vram.assign(vram, vram + VRAM_BYTES);
    f.color = how.colorMode();
    f.palette = how.palette();
    f.scroll = how.scroll();
    f.drawn = false;
}

const uint32_t* GigaScreen::pixels(Frame& f) {
    if (!f.drawn) {
        if (!scr_) scr_ = std::make_unique<Screen>();
        scr_->setColorMode(f.color);
        scr_->setPalette(f.palette);
        scr_->setScroll(f.scroll);
        scr_->render(f.vram.data());
        f.px.assign(scr_->pixels(), scr_->pixels() + W * H);
        f.drawn = true;
    }
    return f.px.data();
}

const uint32_t* GigaScreen::output(const uint32_t* raw) {
    if (frames_ < 3) return raw;
    const uint32_t* f0 = pixels(hist_[head_]);
    const uint32_t* f1 = pixels(hist_[(head_ + 2) % 3]);
    const uint32_t* f2 = pixels(hist_[(head_ + 1) % 3]);
    out_.resize(W * H);
    bool any = false;
    uint32_t pa = 0, pb = 0, pm = 0;   // цветов на экране единицы — последняя пара под рукой
    for (int i = 0; i < W * H; ++i) {
        const uint32_t c = raw[i];
        if (c == f0[i] && c == f2[i] && c != f1[i]) {
            if (c != pa || f1[i] != pb) { pa = c; pb = f1[i]; pm = mix(pa, pb); }
            out_[i] = pm;
            any = true;
        } else {
            out_[i] = c;
        }
    }
    return any ? out_.data() : raw;
}

} // namespace bk
