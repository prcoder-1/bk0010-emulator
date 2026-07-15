#pragma once
#include <string>
#include <set>
#include <map>
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

    // Physical key-held state, exposed through 0177716 bit 6 (active-low). Many
    // games (e.g. Digger) poll that bit to detect a key regardless of whether the
    // monitor's keyboard ISR has already read the code register. Set on key-down,
    // cleared on key-up; independent of the ready/DONE flag. See ioRead(REG_SYS).
    void setKeyHeld(bool held) { keyHeld_ = held; }
    bool keyHeld() const { return keyHeld_; }

    // --- Breakpoints / debugger support ---
    void toggleBreakpoint(uint16_t addr) {
        if (breakpoints_.count(addr)) breakpoints_.erase(addr); else breakpoints_.insert(addr);
    }
    void addBreakpoint(uint16_t addr) { breakpoints_.insert(addr); }
    void removeBreakpoint(uint16_t addr) { breakpoints_.erase(addr); breakConds_.erase(addr); }

    // Optional condition on a breakpoint: only stop when it holds. kind: 0=register
    // Ra, 1=memory word @a, 2=memory byte @a. op: 0 ==, 1 !=, 2 <, 3 >, 4 >=, 5 <=.
    struct BreakCond { uint8_t kind, op; uint16_t a, val; };
    void setBreakCond(uint16_t addr, BreakCond c) { breakConds_[addr] = c; }
    bool breakAllows(uint16_t pc) const;   // true if pc's breakpoint (if any) should fire
    bool hasBreakpoint(uint16_t addr) const { return breakpoints_.count(addr) != 0; }
    const std::set<uint16_t>& breakpoints() const { return breakpoints_; }
    bool breakHit() const { return breakHit_; }
    void clearBreakHit() { breakHit_ = false; watchHit_ = false; }

    // --- Data watchpoints: stop when an address is read and/or written ---
    // mode bits: 1 = on read, 2 = on write. The access hook sets breakHit_ so the
    // run loops stop right after the accessing instruction.
    void addWatch(uint16_t addr, bool onRead, bool onWrite) {
        watchpoints_[addr] = (uint8_t)((onRead ? 1 : 0) | (onWrite ? 2 : 0));
    }
    void removeWatch(uint16_t addr) { watchpoints_.erase(addr); }
    void clearWatches() { watchpoints_.clear(); }
    const std::map<uint16_t, uint8_t>& watchpoints() const { return watchpoints_; }
    bool     watchHit()   const { return watchHit_; }   // last stop was a watchpoint
    uint16_t watchAddr()  const { return watchAddr_; }   // address that was accessed
    bool     watchWrite() const { return watchWrite_; }  // true = write, false = read
    uint16_t watchPc()    const { return watchPc_; }     // instruction that accessed it

    // Run until a breakpoint is hit, `addr` is reached, or `maxTicks` elapse.
    // Used for "step over" (stop at the return address). Returns true if `addr`
    // (or a breakpoint) was reached.
    bool runUntil(uint16_t addr, int maxTicks);

    // Run (with 50 Hz interrupts) until the call stack becomes shallower than
    // `targetDepth` (i.e. the current subroutine returns), a breakpoint, HALT, or
    // the tick limit. For "step out". Needs the flame/CCT tracker enabled.
    bool runUntilReturn(size_t targetDepth, int maxTicks);

    // I/O-register write log (0177600..0177776), captured only while enabled — for
    // debugging how a game programs the keyboard / timer / sound / port.
    struct IoWrite { uint16_t addr, value; uint64_t tick; };
    void setIoLog(bool on) { ioLogOn_ = on; }
    void clearIoLog() { ioLog_.clear(); }
    const std::deque<IoWrite>& ioLog() const { return ioLog_; }

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

    // Running CPU-tick counter (free-running, monotonic).
    uint64_t totalTicks() const { return totalTicks_; }

    // Tick timestamps of recent frame-synchronisation events. BK frames are not a
    // fixed 50 Hz: a game paces itself on the programmable timer, spinning until it
    // reaches/passes zero. Each such underflow is recorded here (capped ring), so
    // the profiler can mark real per-game frame boundaries on its time axis.
    const std::deque<uint64_t>& frameBoundaries() const { return frameTicks_; }

    // Log of intercepted EMT 36 file operations (WRITE/READ/FICT), so a debugger
    // can see which tape/disk files a game loaded or saved and with what result.
    struct EmtOp { uint8_t cmd, response; std::string name; uint16_t addr, len; uint64_t tick; };
    const std::deque<EmtOp>& emtLog() const { return emtLog_; }

    // Side-effect-free snapshot of a system I/O register (0177660..0177716) for
    // the debugger — returns the value ioRead would yield, without the side
    // effects (no ready-flag clear, no timer advance). Unknown addr -> raw peek.
    uint16_t peekReg(uint16_t addr) const;

    // Last value the program wrote to an I/O register (0 if never written / not
    // an I/O-page address). Reads may differ from this on real hardware.
    uint16_t peekRegWritten(uint16_t addr) const;

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

    // Last value written to each I/O-page word (0177600..0177776), for the
    // debugger — hardware often reads back something different than was written.
    uint16_t ioLastWrite_[64] = {0};

    // Internal register state
    uint16_t scroll_    = 0330;
    uint16_t kbdStatus_ = 0;    // 0177660: bit7 = code ready, bit6 = IRQ mask (0=enabled)
    uint16_t kbdData_   = 0;    // 0177662: the single latched key code (7 bits)
    bool     keyIntPending_ = false;
    bool     keyHeld_ = false;   // physical key down (0177716 bit 6, active-low)
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

    // Tick timestamps of recent timer-underflow (frame-sync) events (capped ring).
    std::deque<uint64_t> frameTicks_;
    void recordFrameBoundary();

    std::deque<EmtOp> emtLog_;   // recent EMT 36 file operations (capped ring)

    std::deque<IoWrite> ioLog_;  // recent I/O-register writes (capped ring)
    bool ioLogOn_ = false;

    std::set<uint16_t> breakpoints_;
    std::map<uint16_t, BreakCond> breakConds_;   // optional per-breakpoint condition
    bool     breakHit_ = false;

    // Data watchpoints (addr -> mode bits: 1=read, 2=write) and last-hit info.
    std::map<uint16_t, uint8_t> watchpoints_;
    bool     watchHit_ = false, watchWrite_ = false, watchArmed_ = false;
    uint16_t watchAddr_ = 0, watchPc_ = 0;
    void checkWatch(uint16_t addr, bool write, bool isByte);

    void deliverFrameInterrupts(); // 50 Hz IRQ (vector 0100) + keyboard (0060)
    int  stepCore();               // one instruction + sound/trace bookkeeping

    // EMT 36 handler: reads the tape/disk parameter block (address in R1) and
    // performs a file read/write against the host CWD instead of tape/disk.
    // Returns true if it handled the call (so the ROM handler is skipped).
    bool handleEmt36();

    Speaker sound_;
    Trace   trace_;
};

} // namespace bk
