#include "Board.h"
#include <cstdio>
#include <cstring>
#include <vector>

namespace bk {

// BK-0010 internal register addresses (octal).
enum : uint16_t {
    REG_KBD_STATUS = 0177660,
    REG_KBD_DATA   = 0177662,
    REG_SCROLL     = 0177664,
    REG_TIMER_LIM  = 0177706,
    REG_TIMER_CNT  = 0177710,
    REG_TIMER_CSR  = 0177712,
    REG_PORT       = 0177714,
    REG_SYS        = 0177716,
    START_VECTOR   = 0100000,
};

Board::Board() {
    mem_.setIoBus(this);
    // Feed the access heatmap (no-op unless the trace is enabled).
    mem_.setAccessHook([this](uint16_t a, bool w, bool /*b*/) { trace_.access(a, w); });
}

// Execute one instruction with sound + trace bookkeeping. Returns ticks.
int Board::stepCore() {
    uint16_t pcBefore = cpu_.pc();
    trace_.exec(pcBefore);
    int t = cpu_.step();
    sound_.feed(speaker_ & 1, t);
    if (trace_.enabled()) {
        uint16_t pcNow = cpu_.pc();
        int16_t delta = static_cast<int16_t>(pcNow - pcBefore);
        if (delta < 0 || delta > 6) trace_.edge(pcBefore, pcNow); // non-sequential = control flow
    }
    return t;
}

bool Board::loadRoms(const std::string& romDir) {
    bool ok = true;
    ok &= mem_.loadRomFile(ADDR_ROM_MON, (romDir + "/monit10.rom").c_str(), 8192);
    // BASIC is optional for running games but harmless; load if present.
    mem_.loadRomFile(ADDR_ROM_BASIC, (romDir + "/basic10.rom").c_str(), 24576);
    return ok;
}

void Board::reset() {
    mem_.reset();
    // Power-up: priority 7 (all interrupts masked). The monitor lowers the
    // priority itself once it has installed its interrupt vectors.
    cpu_.reset(START_VECTOR, 0340);
    scroll_ = 0330;
    kbdStatus_ = 0100; // keyboard interrupt enabled (bit 6), no key ready
    kbdData_ = 0;
    keyQueue_.clear();
    timerLimit_ = timerCount_ = timerCsr_ = 0;
    speaker_ = 0;
    screen_.setScroll(scroll_);
    trace_.reset();
}

// ---- Save / restore state ---------------------------------------------------
namespace {
constexpr char kMagic[8] = {'B','K','1','0','S','T','1','\0'};
}

bool Board::saveState(const std::string& path) {
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
    std::fwrite(kMagic, 1, 8, f);
    // Mutable memory is RAM 0..0100000 (ROM above is constant).
    std::fwrite(mem_.raw(), 1, ADDR_RAM_END, f);
    std::fwrite(cpu_.r, sizeof(uint16_t), 8, f);
    std::fwrite(&cpu_.psw, sizeof(uint16_t), 1, f);
    uint16_t dev[7] = {scroll_, kbdStatus_, kbdData_, timerLimit_, timerCount_, timerCsr_, speaker_};
    std::fwrite(dev, sizeof(uint16_t), 7, f);
    std::fclose(f);
    return true;
}

bool Board::loadState(const std::string& path) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    char magic[8];
    if (std::fread(magic, 1, 8, f) != 8 || std::memcmp(magic, kMagic, 8) != 0) { std::fclose(f); return false; }
    if (std::fread(mem_.raw(), 1, ADDR_RAM_END, f) != ADDR_RAM_END) { std::fclose(f); return false; }
    std::fread(cpu_.r, sizeof(uint16_t), 8, f);
    std::fread(&cpu_.psw, sizeof(uint16_t), 1, f);
    uint16_t dev[7] = {0};
    std::fread(dev, sizeof(uint16_t), 7, f);
    std::fclose(f);
    scroll_ = dev[0]; kbdStatus_ = dev[1]; kbdData_ = dev[2];
    timerLimit_ = dev[3]; timerCount_ = dev[4]; timerCsr_ = dev[5];
    speaker_ = static_cast<uint8_t>(dev[6]);
    cpu_.clearHalt(); cpu_.clearWait();
    screen_.setScroll(scroll_);
    return true;
}

int Board::runTicks(int ticks) {
    int done = 0;
    while (done < ticks) {
        if (cpu_.halted()) break;
        int t = stepCore();
        done += t;
        if (!breakpoints_.empty() && breakpoints_.count(cpu_.pc())) {
            breakHit_ = true;
            break;
        }
        if (cpu_.waiting()) { // idle: consume the rest of the frame quietly
            cpu_.clearWait();
            break;
        }
    }
    screen_.setScroll(scroll_);
    return done;
}

bool Board::runUntil(uint16_t addr, int maxTicks) {
    int done = 0;
    while (done < maxTicks) {
        if (cpu_.halted()) return false;
        int t = stepCore();
        done += t;
        if (cpu_.pc() == addr) { screen_.setScroll(scroll_); return true; }
        if (!breakpoints_.empty() && breakpoints_.count(cpu_.pc())) {
            breakHit_ = true; screen_.setScroll(scroll_); return true;
        }
    }
    screen_.setScroll(scroll_);
    return false;
}

void Board::runFrame() {
    deliverFrameInterrupts();
    runTicks(ticksPerFrame());
    trace_.tick();
}

// Delivered once per 50 Hz frame. On BK-0010 the video controller raises IRQ2
// (vector 0100) every frame and the keyboard raises IRQ (vector 0060). Both are
// blocked when the processor priority bit (PSW bit 7, 0200) is set.
void Board::deliverFrameInterrupts() {
    if (cpu_.halted()) return;
    if (cpu_.psw & 0200) return; // interrupts masked

    // Keyboard: deliver the next queued key only once the previous one has been
    // read (ready bit 0200 clear), so the data register value is never lost.
    if (!(kbdStatus_ & 0200) && !keyQueue_.empty()) {
        uint16_t raw = keyQueue_.front();
        keyQueue_.pop_front();
        kbdData_ = raw & 0177;                 // low 7 bits go to 0177662
        kbdStatus_ |= 0200;                    // "key ready"
        // Function keys / АР2 (bit 0200 set) vector through 0274, others 060.
        uint16_t vec = (raw & 0200) ? 0274 : Cpu::VEC_KEYBOARD;
        if (mem_.peekWord(vec) != 0) cpu_.interrupt(vec);
        return;
    }

    if (mem_.peekWord(Cpu::VEC_IRQ2) != 0)
        cpu_.interrupt(Cpu::VEC_IRQ2);         // 0100 (50 Hz)
}

int Board::stepInstruction() {
    int t = stepCore();
    screen_.setScroll(scroll_);
    return t;
}

bool Board::loadBin(const std::string& path, bool run, uint16_t* outAddr, uint16_t* outLen) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) { std::fprintf(stderr, "loadBin: cannot open '%s'\n", path.c_str()); return false; }
    uint8_t hdr[4];
    if (std::fread(hdr, 1, 4, f) != 4) { std::fclose(f); return false; }
    uint16_t addr = hdr[0] | (hdr[1] << 8);
    uint16_t len  = hdr[2] | (hdr[3] << 8);
    std::vector<uint8_t> data(len);
    size_t got = std::fread(data.data(), 1, len, f);
    std::fclose(f);
    if (got == 0) return false;
    for (size_t i = 0; i < got; ++i)
        mem_.pokeByte(static_cast<uint16_t>(addr + i), data[i]);
    if (outAddr) *outAddr = addr;
    if (outLen)  *outLen  = static_cast<uint16_t>(got);
    if (run) {
        cpu_.clearHalt();
        cpu_.clearWait();
        cpu_.r[7] = addr; // jump to load address
    }
    return true;
}

// ---- I/O register dispatch --------------------------------------------------
bool Board::ioRead(uint16_t addr, uint16_t& value) {
    switch (addr) {
    case REG_KBD_STATUS: value = kbdStatus_; return true;
    case REG_KBD_DATA:   value = kbdData_; kbdStatus_ &= ~0200; return true;
    case REG_SCROLL:     value = scroll_; return true;
    case REG_TIMER_LIM:  value = timerLimit_; return true;
    case REG_TIMER_CNT:  value = timerCount_; return true;
    case REG_TIMER_CSR:  value = timerCsr_; return true;
    case REG_PORT:       value = 0; return true;
    case REG_SYS: {
        // BK-0010: bit15 set, high byte 0200 (serial idle). Bit 6 (0100) is the
        // "key pressed" indicator, active-low: 0 while a key is waiting.
        uint16_t v = 0100000 | 0200;
        if (!(kbdStatus_ & 0200)) v |= 0100; // no key pending -> bit 6 high
        value = v;
        return true;
    }
    default:
        // Any other address in the I/O page: read as 0 (handled).
        if (addr >= ADDR_IO_PAGE) { value = 0; return true; }
        return false;
    }
}

bool Board::ioWrite(uint16_t addr, uint16_t value, bool /*isByte*/) {
    switch (addr) {
    case REG_SCROLL:    scroll_ = value & 0777; return true;
    case REG_TIMER_LIM: timerLimit_ = value; return true;
    case REG_TIMER_CSR: timerCsr_ = value; return true;
    case REG_SYS:
        speaker_ = (value >> 6) & 3; // bits 6,7: tape/speaker output
        return true;
    case REG_KBD_STATUS: kbdStatus_ = (kbdStatus_ & ~0100) | (value & 0100); return true;
    default:
        if (addr >= ADDR_IO_PAGE) return true; // swallow writes to I/O page
        return false;
    }
}

} // namespace bk
