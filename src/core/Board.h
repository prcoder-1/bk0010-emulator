#pragma once
#include <string>
#include <set>
#include <deque>
#include "Memory.h"
#include "Cpu.h"
#include "Screen.h"
#include "Speaker.h"
#include "Trace.h"

namespace bk {

// The BK-0010 "motherboard": wires the CPU, memory, screen and the internal
// I/O registers together and drives the emulation. Pure C++ (no Qt) so it can
// run in a worker thread owned by the GUI.
class Board : public IoBus {
public:
    Board();

    bool loadRoms(const std::string& romDir);
    void reset();                       // power-on reset (start vector 0100000)

    // Run at least `ticks` CPU ticks worth of instructions. Returns ticks run.
    int  runTicks(int ticks);
    void runFrame();
    int  ticksPerFrame() const { return cpuFreqHz_ / frameHz_; }

    // Queue a raw BK key code to be delivered via the keyboard IRQ. Codes with
    // bit 0200 set (function keys / АР2) use vector 0274; the rest use 060. The
    // low 7 bits are placed in the data register 0177662.
    void pressKey(uint16_t bkCode) { if (keyQueue_.size() < 64) keyQueue_.push_back(bkCode); }
    void clearKeys() { keyQueue_.clear(); }

    // --- Breakpoints / debugger support ---
    void toggleBreakpoint(uint16_t addr) {
        if (breakpoints_.count(addr)) breakpoints_.erase(addr); else breakpoints_.insert(addr);
    }
    void addBreakpoint(uint16_t addr) { breakpoints_.insert(addr); }
    void removeBreakpoint(uint16_t addr) { breakpoints_.erase(addr); }
    bool hasBreakpoint(uint16_t addr) const { return breakpoints_.count(addr) != 0; }
    const std::set<uint16_t>& breakpoints() const { return breakpoints_; }
    bool breakHit() const { return breakHit_; }
    void clearBreakHit() { breakHit_ = false; }

    // Run until a breakpoint is hit, `addr` is reached, or `maxTicks` elapse.
    // Used for "step over" (stop at the return address). Returns true if `addr`
    // (or a breakpoint) was reached.
    bool runUntil(uint16_t addr, int maxTicks);

    // Execute exactly one instruction (for the debugger). Returns ticks.
    int  stepInstruction();

    // Load a .BIN file (4-byte header: load addr + length, little-endian).
    // If `run`, sets PC to the load address. Returns false on error.
    bool loadBin(const std::string& path, bool run,
                 uint16_t* outAddr = nullptr, uint16_t* outLen = nullptr);

    // Accessors
    Cpu&    cpu()    { return cpu_; }
    Memory& memory() { return mem_; }
    Screen& screen() { return screen_; }
    Speaker& sound() { return sound_; }
    Trace&  trace()  { return trace_; }

    // Save/restore full emulator state (RAM, CPU, device registers).
    bool saveState(const std::string& path);
    bool loadState(const std::string& path);

    // IoBus
    bool ioRead(uint16_t addr, uint16_t& value) override;
    bool ioWrite(uint16_t addr, uint16_t value, bool isByte) override;

private:
    Memory mem_;
    Cpu    cpu_{mem_};
    Screen screen_;

    int cpuFreqHz_ = 3000000;
    int frameHz_   = 50;

    // Internal register state
    uint16_t scroll_    = 0330;
    uint16_t kbdStatus_ = 0;
    uint16_t kbdData_   = 0;
    uint8_t  speaker_   = 0;

    // 1801VM1 programmable interval timer (0177706 limit / 0177710 counter /
    // 0177712 control). Decrements at f/128 (optionally /4, /16); sets the FL
    // event flag on underflow. Polled by software (no interrupt on BK-0010).
    uint16_t timerLimit_ = 0, timerCount_ = 0;
    uint8_t  timerCsr_   = 0;       // control/status (low 8 bits, incl. FL flag)
    uint64_t totalTicks_ = 0;       // running CPU-tick counter
    uint64_t timerStart_ = 0;       // tick at which the current count began
    uint32_t timerPeriod_ = 128;    // CPU ticks per timer decrement
    void timerCheck();              // lazily advance the counter to "now"
    void timerSetMode(uint8_t mode);

    std::deque<uint16_t> keyQueue_; // pending raw BK key codes

    std::set<uint16_t> breakpoints_;
    bool     breakHit_ = false;

    void deliverFrameInterrupts(); // 50 Hz IRQ (vector 0100) + keyboard (0060)
    int  stepCore();               // one instruction + sound/trace bookkeeping

    Speaker sound_;
    Trace   trace_;
};

} // namespace bk
