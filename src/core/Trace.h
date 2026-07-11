#pragma once
#include <cstdint>
#include <array>
#include <unordered_map>

namespace bk {

// Records runtime activity for the debugger visualisations:
//  * per-byte last-access timestamps (for the memory heatmap with fade),
//  * per-address execution counts (hot-code highlighting),
//  * branch/jump/call edges (control-flow graph).
class Trace {
public:
    void reset() {
        now_ = 1; execMax_ = 0;
        lastAccess_.fill(0); lastWrite_.fill(0); execCount_.fill(0);
        edges_.clear();
    }

    void setEnabled(bool e) { enabled_ = e; }
    bool enabled() const { return enabled_; }

    // Called once per emulated frame; advances the logical clock used for fade.
    void tick() { ++now_; }
    uint32_t now() const { return now_; }

    // Memory access hook (byte address). Marks read and/or write times.
    void access(uint16_t addr, bool write) {
        if (!enabled_) return;
        lastAccess_[addr] = now_;
        if (write) lastWrite_[addr] = now_;
    }

    void exec(uint16_t pc) { if (enabled_) { ++execCount_[pc]; if (execCount_[pc] > execMax_) execMax_ = execCount_[pc]; } }
    void edge(uint16_t from, uint16_t to) { if (enabled_) ++edges_[(uint32_t(from) << 16) | to]; }

    // Heat 0..255: 255 = accessed this frame, fading to 0 over `fadeFrames`.
    uint8_t heat(uint16_t addr, bool write = false, int fadeFrames = 60) const {
        uint32_t t = write ? lastWrite_[addr] : lastAccess_[addr];
        if (t == 0) return 0;
        uint32_t age = now_ - t;
        if (age >= (uint32_t)fadeFrames) return 0;
        return static_cast<uint8_t>(255 - (age * 255) / fadeFrames);
    }

    uint32_t execCount(uint16_t pc) const { return execCount_[pc]; }
    uint32_t execMax() const { return execMax_; }
    const std::unordered_map<uint32_t, uint32_t>& edges() const { return edges_; }

private:
    bool enabled_ = false;
    uint32_t now_ = 1;
    uint32_t execMax_ = 0;
    std::array<uint32_t, 0x10000> lastAccess_{};
    std::array<uint32_t, 0x10000> lastWrite_{};
    std::array<uint32_t, 0x10000> execCount_{};
    std::unordered_map<uint32_t, uint32_t> edges_; // (from<<16|to) -> count
};

} // namespace bk
