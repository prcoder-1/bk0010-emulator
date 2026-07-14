#pragma once
#include <cstdint>
#include <array>
#include <unordered_map>
#include <vector>
#include <utility>

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
        flameReset();
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

    // ---- Calling-context tree (flame graph) -------------------------------
    // A node is one call frame reached along a specific call path; `self` is the
    // ticks executed with exactly this stack on top. Built only while enabled via
    // setFlameEnabled(), so it costs nothing unless the flame graph is open.
    struct FlameNode {
        uint16_t func;      // entry address of this frame (0 = synthetic root)
        uint64_t self;      // ticks executed directly in this context
        int      parent;    // index of the parent frame (-1 for the root)
        int      depth;     // call depth (root = 0)
        std::unordered_map<uint16_t, int> kids;  // callee entry -> child node index
    };

    void setFlameEnabled(bool e) { flameOn_ = e; if (flame_.empty()) flameReset(); }
    bool flameEnabled() const { return flameOn_; }
    void flameClear() { flameReset(); }
    const std::vector<FlameNode>& flame() const { return flame_; }

    // Called by Board after every instruction: `ir` = executed opcode word, `pc`/`sp`
    // = state after it, `ticks` = its cost. A JSR pushes a frame; a return (RTS/RTI
    // or any code that raises SP above a frame's call-time SP) pops it. Each
    // instruction's ticks are charged to the current top-of-stack context.
    void profileStep(uint16_t ir, uint16_t pcNow, uint16_t sp, int ticks) {
        if (!flameOn_) return;
        if (ticks > 0) flame_[fstack_.back().first].self += static_cast<uint64_t>(ticks);
        while (fstack_.size() > 1 && sp > fstack_.back().second) fstack_.pop_back();
        if (ir >= 0004000 && ir <= 0004777) flamePush(pcNow, sp);   // JSR
    }
    // Called by Board when it dispatches an interrupt to `handler` (sp already
    // holds the pushed PSW+PC). The ISR becomes a frame above the interrupted one.
    void profileInterrupt(uint16_t handler, uint16_t sp) { if (flameOn_) flamePush(handler, sp); }

private:
    bool flameOn_ = false;
    std::vector<FlameNode> flame_;                    // flame_[0] = root
    std::vector<std::pair<int, uint16_t>> fstack_;    // (node index, call-time SP)

    void flameReset() {
        flame_.assign(1, FlameNode{0, 0, -1, 0, {}});
        fstack_.assign(1, {0, static_cast<uint16_t>(0xFFFF)});
    }
    void flamePush(uint16_t func, uint16_t callSP) {
        if (fstack_.size() >= 512) { fstack_.push_back({fstack_.back().first, callSP}); return; }
        int cur = fstack_.back().first, child;
        auto it = flame_[cur].kids.find(func);
        if (it != flame_[cur].kids.end()) child = it->second;
        else if (flame_.size() < 200000) {
            child = static_cast<int>(flame_.size());
            flame_.push_back(FlameNode{func, 0, cur, flame_[cur].depth + 1, {}});
            flame_[cur].kids[func] = child;
        } else child = cur;                            // node cap: degrade gracefully
        fstack_.push_back({child, callSP});
    }

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
