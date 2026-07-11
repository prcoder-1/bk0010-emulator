#pragma once
#include <cstdint>
#include <array>
#include <unordered_map>

namespace bk {

// Records runtime activity for the debugger visualisations:
//  * per-byte last read / write / execute timestamps (memory heatmap with fade:
//    green = read, red = write, blue = code execution),
//  * per-address execution counts (hot-code highlighting),
//  * branch/jump/call edges (control-flow graph).
class Trace {
public:
    void reset() {
        now_ = 1; execMax_ = 0;
        lastRead_.fill(0); lastWrite_.fill(0); lastExec_.fill(0); execCount_.fill(0);
        edges_.clear();
    }

    void setEnabled(bool e) { enabled_ = e; }
    bool enabled() const { return enabled_; }

    // Called once per emulated frame; advances the logical clock used for fade.
    void tick() { ++now_; }
    uint32_t now() const { return now_; }

    // CPU memory-access hook (byte address). Records read or write time; for a
    // word access also marks the high byte.
    void access(uint16_t addr, bool write, bool isByte) {
        if (!enabled_) return;
        auto& arr = write ? lastWrite_ : lastRead_;
        arr[addr] = now_;
        if (!isByte) arr[static_cast<uint16_t>(addr + 1)] = now_;
    }

    // Instruction executed at `pc` — records exec time (opcode word) + count.
    void exec(uint16_t pc) {
        if (!enabled_) return;
        lastExec_[pc] = now_;
        lastExec_[static_cast<uint16_t>(pc + 1)] = now_;
        if (++execCount_[pc] > execMax_) execMax_ = execCount_[pc];
    }
    void edge(uint16_t from, uint16_t to) { if (enabled_) ++edges_[(uint32_t(from) << 16) | to]; }

    // Raw last-access timestamps (0 = never).
    uint32_t lastRead(uint16_t a)  const { return lastRead_[a]; }
    uint32_t lastWrite(uint16_t a) const { return lastWrite_[a]; }
    uint32_t lastExec(uint16_t a)  const { return lastExec_[a]; }

    // Fade factor 0..1 for a timestamp: 1 = this frame, 0 after `frames`.
    double fade(uint32_t t, int frames) const {
        if (t == 0) return 0.0;
        uint32_t age = now_ - t;
        if (age >= static_cast<uint32_t>(frames)) return 0.0;
        return 1.0 - static_cast<double>(age) / frames;
    }

    uint32_t execCount(uint16_t pc) const { return execCount_[pc]; }
    uint32_t execMax() const { return execMax_; }
    const std::unordered_map<uint32_t, uint32_t>& edges() const { return edges_; }

private:
    bool enabled_ = false;
    uint32_t now_ = 1;
    uint32_t execMax_ = 0;
    std::array<uint32_t, 0x10000> lastRead_{};
    std::array<uint32_t, 0x10000> lastWrite_{};
    std::array<uint32_t, 0x10000> lastExec_{};
    std::array<uint32_t, 0x10000> execCount_{};
    std::unordered_map<uint32_t, uint32_t> edges_; // (from<<16|to) -> count
};

} // namespace bk
