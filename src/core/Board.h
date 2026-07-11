#pragma once
#include <string>
#include <set>
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

    // Run enough frames for the monitor ROM to initialise (vectors, stack,
    // display driver) if it hasn't yet. A game must not be started before this,
    // or it will jump into an uninitialised system and crash. Idempotent.
    void ensureMonitorBooted(int bootFrames = 25) {
        while (framesSinceReset_ < bootFrames) runFrame();
    }

    // Present a raw BK key code to the keyboard controller, exactly like the
    // real BK-0010: the code register (0177662) holds a SINGLE code. A new code
    // is latched only while the "ready" flag (0177660 bit 7) is clear — i.e. the
    // previous code has been read; otherwise the keypress is lost. The interrupt
    // (vector 060, or 0274 for codes with bit 0200) fires only if enabled
    // (0177660 bit 6). Returns true if the code was latched, false if dropped.
    bool pressKey(uint16_t bkCode);

    // True while an unread code sits in the register (ready flag set).
    bool keyReady() const { return (kbdStatus_ & 0200) != 0; }

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
    int framesSinceReset_ = 0;   // for ensureMonitorBooted()

    // Internal register state
    uint16_t scroll_    = 0330;
    uint16_t kbdStatus_ = 0;    // 0177660: bit7 = code ready, bit6 = IRQ enable
    uint16_t kbdData_   = 0;    // 0177662: the single latched key code (7 bits)
    bool     keyIntPending_ = false;
    uint16_t keyIntVec_ = 060;
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

    std::set<uint16_t> breakpoints_;
    bool     breakHit_ = false;

    void deliverFrameInterrupts(); // 50 Hz IRQ (vector 0100) + keyboard (0060)
    int  stepCore();               // one instruction + sound/trace bookkeeping

    Speaker sound_;
    Trace   trace_;
};

} // namespace bk
