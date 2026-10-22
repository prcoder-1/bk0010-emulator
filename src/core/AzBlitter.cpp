#include "AzBlitter.h"
#include <cstring>

namespace bk {

void AzBlitter::reset() {
    count_ = 0; manual_ = false; running_ = false; active_ = false;
    n_ = i_ = 0;
}

uint16_t AzBlitter::readCtrl() const {
    return static_cast<uint16_t>((running_ ? RUN : 0) | (manual_ ? MANUAL : 0) | count_);
}

// Запись 0177270: 0 команд — асинхронный останов; пока блиттер работает, меняется только
// число команд; ручной пуск — разряд 12 при уже установленном (до этой записи) разряде 14.
void AzBlitter::writeCtrl(uint16_t v) {
    const uint16_t n = v & 0777;
    if (n == 0) {
        running_ = false; active_ = false;
        count_ = 0;
        manual_ = (v & MANUAL) != 0;
        return;
    }
    if (running_) { count_ = n; return; }
    const bool wasManual = manual_;
    count_ = n;
    manual_ = (v & MANUAL) != 0;
    if (wasManual && (v & START)) start(n);
}

void AzBlitter::frameEnd() {
    if (!running_ && !manual_ && count_) start(count_);
}

void AzBlitter::start(int n) {
    if (!mem_) return;
    // Пачка читается в буфер сразу: страницу можно переписывать, как только сброшен разряд 9.
    const uint32_t base = uint32_t(page_) << 11;
    for (int i = 0; i < n * 8; ++i) buf_[i] = mem_[(base + uint32_t(i)) & mask_];
    n_ = n; i_ = 0;
    running_ = true; active_ = false;
    ++runs_;
}

bool AzBlitter::fetch() {
    for (;;) {
        if (i_ >= n_) { running_ = false; return false; }
        std::memcpy(c_, &buf_[i_ * 8], sizeof c_);
        ++i_;
        const uint16_t cmd = c_[3];
        const int op = (cmd >> 3) & 7;
        const int height = c_[4] >> 8;
        if ((cmd & 4) || op > 5 || height == 0) continue;     // NOP, неизвестная, 0 строк
        dst_ = ((uint32_t(c_[0] & 0377) << 16) | c_[2]) + (uint32_t(c_[6]) << 8);
        src_ = (uint32_t(c_[0] >> 8) << 16) | c_[1];
        wLeft_ = (c_[4] & 0377) + 1;
        hLeft_ = height;
        active_ = true;
        return true;
    }
}

void AzBlitter::step() {
    const uint16_t cmd = c_[3];
    const int op = (cmd >> 3) & 7;
    const bool swap = cmd & 0100, mirH = cmd & 01000, mirV = cmd & 02000;
    const uint8_t sc = static_cast<uint8_t>(c_[7]), dc = static_cast<uint8_t>(c_[7] >> 8);
    if (cmd & 1) s_ = mem_[src_ & mask_];
    if (cmd & 2) d_ = mem_[dst_ & mask_];
    auto sw = [&](uint16_t x) { return swap ? static_cast<uint16_t>((x << 8) | (x >> 8)) : x; };
    const uint16_t s = sw(s_), d = sw(d_);
    auto ovl = [](uint16_t x, uint8_t k) {
        return static_cast<uint16_t>(((x >> 8) != k ? 0xFF00 : 0) | ((x & 0xFF) != k ? 0x00FF : 0));
    };
    uint16_t val = 0, mask = 0xFFFF;
    bool toSrc = false;
    switch (op) {
    case 0: val = static_cast<uint16_t>(sc * 0x101); break;               // заполнение
    case 1: val = s; break;                                               // SRC -> DST
    case 2: val = s; mask = ovl(s, sc); break;                            // наложение
    case 3: val = s; mask = ovl(s, sc) & static_cast<uint16_t>(~ovl(d, dc)); break;   // под фон
    case 4: val = d; toSrc = true; break;                                 // DST -> SRC (фон)
    case 5: val = static_cast<uint16_t>(sc * 0x101); mask = ovl(s, dc); break;  // контур
    }
    uint16_t& m = mem_[(toSrc ? src_ : dst_) & mask_];
    m = static_cast<uint16_t>((m & ~mask) | (val & mask));
    // Шаг: по строке ±1, в конце строки — шаг строки, как его считает GID по зеркалам.
    const int hstep = mirH ? -1 : 1;
    const int pitch = static_cast<int>(c_[5] & 0377) - 1;
    const int vstep = (!mirH && !mirV) ? pitch : (mirH && !mirV) ? pitch + 2 : (!mirH && mirV) ? -(pitch + 2) : -pitch;
    dst_ += static_cast<uint32_t>(hstep);
    if (op != 0) ++src_;
    if (--wLeft_ == 0) {
        wLeft_ = (c_[4] & 0377) + 1;
        dst_ += static_cast<uint32_t>(vstep);
        if (--hLeft_ == 0) active_ = false;
    }
}

void AzBlitter::line() {
    if (!running_ || !mem_) return;
    for (int k = 0; k < WORDS_PER_LINE; ++k) {
        if (!active_ && !fetch()) return;
        step();
    }
}

void AzBlitter::saveState(std::vector<uint8_t>& out) const {
    auto put = [&](const void* p, size_t n) { const uint8_t* b = static_cast<const uint8_t*>(p); out.insert(out.end(), b, b + n); };
    const uint8_t fl = static_cast<uint8_t>((manual_ ? 1 : 0) | (running_ ? 2 : 0) | (active_ ? 4 : 0));
    put(&count_, 2); put(&fl, 1); put(&page_, 2);
    const int32_t v[4] = {n_, i_, wLeft_, hLeft_};
    put(v, sizeof v); put(c_, sizeof c_); put(&dst_, 4); put(&src_, 4); put(&s_, 2); put(&d_, 2);
    const uint32_t nb = running_ ? static_cast<uint32_t>(n_ * 8) : 0;
    put(&nb, 4);
    put(buf_.data(), nb * 2);
}

bool AzBlitter::loadState(const uint8_t*& p, const uint8_t* end) {
    auto get = [&](void* d, size_t n) { if (static_cast<size_t>(end - p) < n) return false; std::memcpy(d, p, n); p += n; return true; };
    uint8_t fl; int32_t v[4]; uint32_t nb;
    if (!get(&count_, 2) || !get(&fl, 1) || !get(&page_, 2) || !get(v, sizeof v) || !get(c_, sizeof c_)
        || !get(&dst_, 4) || !get(&src_, 4) || !get(&s_, 2) || !get(&d_, 2) || !get(&nb, 4)) return false;
    if (nb > buf_.size() || !get(buf_.data(), nb * 2)) return false;
    manual_ = fl & 1; running_ = fl & 2; active_ = fl & 4;
    n_ = v[0]; i_ = v[1]; wLeft_ = v[2]; hLeft_ = v[3];
    return true;
}

} // namespace bk
