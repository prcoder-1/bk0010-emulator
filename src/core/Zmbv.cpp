#include "Zmbv.h"
#include <zlib.h>
#include <cstdlib>
#include <cstring>
#include <unordered_map>

namespace bk {

const std::vector<std::string>& videoCodecs() {
    static const std::vector<std::string> names = {"zmbv"};
    return names;
}

std::unique_ptr<VideoEncoder> makeVideoEncoder(const std::string& name, int width, int height) {
    if (name == "zmbv") return std::make_unique<ZmbvEncoder>(width, height);
    return nullptr;
}

struct ZmbvEncoder::Z {
    z_stream s{};
    bool ok = false;
    std::unordered_map<uint32_t, uint8_t> index;
};

ZmbvEncoder::ZmbvEncoder(int width, int height, int level, int keyInterval)
    : z_(std::make_unique<Z>()), w_(width), h_(height), pitch_(width + 2 * MAX_VECTOR),
      keyInterval_(keyInterval > 0 ? keyInterval : 300) {
    z_->ok = deflateInit(&z_->s, level) == Z_OK;
    cur_.assign(static_cast<size_t>(pitch_) * (h_ + 2 * MAX_VECTOR), 0);
    prev_ = cur_;
    // Индекс 0 — чёрный: им же считаются точки за краем кадра (поле вокруг буфера
    // нулевое, а декодер вне кадра подставляет 0).
    colors_.push_back(0xFF000000u);
    z_->index[0xFF000000u] = 0;
    lastColor_ = 0xFF000000u; lastIndex_ = 0;
    // Векторы — кольцами вокруг (0,0), как в DOSBox: сначала ближние сдвиги.
    for (int s = 1; s <= 10; ++s)
        for (int y = -s; y <= s; ++y)
            for (int x = -s; x <= s; ++x)
                if (std::abs(x) == s || std::abs(y) == s) vectors_.push_back({int8_t(x), int8_t(y)});
}

ZmbvEncoder::~ZmbvEncoder() {
    if (z_->ok) deflateEnd(&z_->s);
}

uint32_t ZmbvEncoder::fourcc() const {
    return uint32_t('Z') | uint32_t('M') << 8 | uint32_t('B') << 16 | uint32_t('V') << 24;
}

uint8_t ZmbvEncoder::colorIndex(uint32_t argb) {
    argb |= 0xFF000000u;
    if (argb == lastColor_) return lastIndex_;
    auto it = z_->index.find(argb);
    uint8_t idx;
    if (it != z_->index.end()) {
        idx = it->second;
    } else if (colors_.size() < 256) {
        idx = static_cast<uint8_t>(colors_.size());
        colors_.push_back(argb);
        z_->index[argb] = idx;
        pal_[idx * 3 + 0] = uint8_t(argb >> 16);
        pal_[idx * 3 + 1] = uint8_t(argb >> 8);
        pal_[idx * 3 + 2] = uint8_t(argb);
    } else {                                   // палитра полна — ближайший цвет
        int best = 0; long bd = -1;
        for (size_t i = 0; i < colors_.size(); ++i) {
            const long dr = long((colors_[i] >> 16) & 255) - long((argb >> 16) & 255);
            const long dg = long((colors_[i] >> 8) & 255) - long((argb >> 8) & 255);
            const long db = long(colors_[i] & 255) - long(argb & 255);
            const long d = dr * dr + dg * dg + db * db;
            if (bd < 0 || d < bd) { bd = d; best = int(i); }
        }
        idx = static_cast<uint8_t>(best);
        z_->index[argb] = idx;
    }
    lastColor_ = argb; lastIndex_ = idx;
    return idx;
}

// Сколько точек блока отличается от прошлого кадра со сдвигом (vx, vy).
int ZmbvEncoder::compareBlock(int bx, int by, int bw, int bh, int vx, int vy) const {
    int diff = 0;
    for (int y = 0; y < bh; ++y) {
        const uint8_t* n = at(cur_, bx, by + y);
        const uint8_t* o = at(prev_, bx + vx, by + vy + y);
        for (int x = 0; x < bw; ++x) diff += n[x] != o[x];
    }
    return diff;
}

// Быстрая прикидка по каждой четвёртой точке каждой четвёртой строки.
int ZmbvEncoder::sampleBlock(int bx, int by, int bw, int bh, int vx, int vy) const {
    int diff = 0;
    for (int y = 0; y < bh; y += 4) {
        const uint8_t* n = at(cur_, bx, by + y);
        const uint8_t* o = at(prev_, bx + vx, by + vy + y);
        for (int x = 0; x < bw; x += 4) diff += n[x] != o[x];
    }
    return diff;
}

bool ZmbvEncoder::encode(const uint32_t* argb, bool keyframe, std::vector<uint8_t>& out, bool& isKey) {
    if (!z_->ok) return false;
    for (int y = 0; y < h_; ++y) {
        uint8_t* d = at(cur_, 0, y);
        const uint32_t* s = argb + static_cast<size_t>(y) * w_;
        for (int x = 0; x < w_; ++x) d[x] = colorIndex(s[x]);
    }
    isKey = keyframe || frame_ % keyInterval_ == 0;
    ++frame_;
    out.clear();
    work_.clear();
    uint8_t flags = 0;
    if (isKey) {
        flags = KEYFRAME;
        out.push_back(flags);
        const uint8_t hdr[6] = {0, 1, 1, FMT_8BPP, BLOCK, BLOCK};   // версия 0.1, zlib, 8 бит, блоки 16x16
        out.insert(out.end(), hdr, hdr + 6);
        work_.insert(work_.end(), pal_, pal_ + 768);
        std::memcpy(sentPal_, pal_, 768);
        for (int y = 0; y < h_; ++y) { const uint8_t* r = at(cur_, 0, y); work_.insert(work_.end(), r, r + w_); }
        deflateReset(&z_->s);
    } else {
        if (std::memcmp(pal_, sentPal_, 768) != 0) {
            flags |= DELTA_PALETTE;
            for (int i = 0; i < 768; ++i) work_.push_back(pal_[i] ^ sentPal_[i]);
            std::memcpy(sentPal_, pal_, 768);
        }
        out.push_back(flags);
        const int bxN = (w_ + BLOCK - 1) / BLOCK, byN = (h_ + BLOCK - 1) / BLOCK;
        const size_t vecAt = work_.size();
        work_.resize(vecAt + ((static_cast<size_t>(bxN * byN) * 2 + 3) & ~size_t(3)), 0);
        int b = 0;
        for (int by = 0; by < h_; by += BLOCK) {
            const int bh = std::min<int>(BLOCK, h_ - by);
            for (int bx = 0; bx < w_; bx += BLOCK, ++b) {
                const int bw = std::min<int>(BLOCK, w_ - bx);
                int best = compareBlock(bx, by, bw, bh, 0, 0), vx = 0, vy = 0;
                for (size_t v = 0, tries = 64; best >= 4 && v < vectors_.size() && tries; ++v) {
                    const Vec& c = vectors_[v];
                    if (sampleBlock(bx, by, bw, bh, c.x, c.y) >= 4) continue;
                    --tries;
                    const int d = compareBlock(bx, by, bw, bh, c.x, c.y);
                    if (d < best) { best = d; vx = c.x; vy = c.y; }
                }
                work_[vecAt + 2 * b] = uint8_t((vx << 1) | (best ? 1 : 0));
                work_[vecAt + 2 * b + 1] = uint8_t(vy << 1);
                if (!best) continue;
                for (int y = 0; y < bh; ++y) {
                    const uint8_t* n = at(cur_, bx, by + y);
                    const uint8_t* o = at(prev_, bx + vx, by + vy + y);
                    for (int x = 0; x < bw; ++x) work_.push_back(n[x] ^ o[x]);
                }
            }
        }
    }
    const size_t hdr = out.size();
    out.resize(hdr + deflateBound(&z_->s, static_cast<uLong>(work_.size())) + 64);
    z_->s.next_in = work_.data();
    z_->s.avail_in = static_cast<uInt>(work_.size());
    z_->s.next_out = out.data() + hdr;
    z_->s.avail_out = static_cast<uInt>(out.size() - hdr);
    if (deflate(&z_->s, Z_SYNC_FLUSH) != Z_OK || z_->s.avail_in != 0) return false;
    out.resize(out.size() - z_->s.avail_out);
    cur_.swap(prev_);
    return true;
}

} // namespace bk
