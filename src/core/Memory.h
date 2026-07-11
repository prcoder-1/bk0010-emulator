#pragma once
#include <cstdint>
#include <array>
#include <functional>

namespace bk {

// BK-0010-01 memory layout (word-addressable, 64 KB):
//   000000-037777  user RAM (16 KB, incl. system cells & stack)
//   040000-077777  screen RAM (16 KB) — also normal RAM
//   100000-117777  Monitor ROM (8 KB)
//   120000-177577  BASIC ROM (24 KB)
//   177600-177777  I/O register page
enum : uint16_t {
    ADDR_RAM_END   = 0100000, // first non-RAM address (RAM is 0..077777)
    ADDR_VIDEO     = 0040000, // start of screen RAM
    ADDR_ROM_MON   = 0100000,
    ADDR_ROM_BASIC = 0120000,
    ADDR_IO_PAGE   = 0177600, // I/O register page start
};

// Interface for memory-mapped I/O devices (keyboard, timer, screen scroll,
// system register, etc.). The Board implements this. Return true if the access
// was handled; otherwise Memory falls back to plain RAM/ROM behaviour.
class IoBus {
public:
    virtual ~IoBus() = default;
    // Non-const: reads may have side effects (e.g. clearing a status bit).
    virtual bool ioRead(uint16_t addr, uint16_t& value) = 0;
    virtual bool ioWrite(uint16_t addr, uint16_t value, bool isByte) = 0;
};

// Access-trace hook: called on every CPU-side read/write so the debugger can
// build heatmaps. addr is the byte address; write==true for stores.
using AccessHook = std::function<void(uint16_t addr, bool write, bool isByte)>;

class Memory {
public:
    Memory();

    void reset();                       // clear RAM (keeps ROM)
    void setIoBus(IoBus* bus) { io_ = bus; }
    void setAccessHook(AccessHook h) { hook_ = std::move(h); }

    // Load a ROM image into a byte-address range; region becomes read-only.
    bool loadRom(uint16_t addr, const uint8_t* data, size_t len);
    bool loadRomFile(uint16_t addr, const char* path, size_t expectedLen = 0);

    // CPU-facing access (dispatches I/O page, honours ROM protection, calls hook)
    uint16_t readWord(uint16_t addr);
    uint8_t  readByte(uint16_t addr);
    void     writeWord(uint16_t addr, uint16_t value);
    void     writeByte(uint16_t addr, uint8_t value);

    // Side-effect-free access for the debugger / disassembler / screen.
    uint16_t peekWord(uint16_t addr) const;
    uint8_t  peekByte(uint16_t addr) const;
    void     pokeWord(uint16_t addr, uint16_t value); // ignores ROM protection
    void     pokeByte(uint16_t addr, uint8_t value);

    bool isRom(uint16_t addr) const { return addr >= romStart_ && addr < ADDR_IO_PAGE; }

    const uint8_t* raw() const { return mem_.data(); }
    uint8_t*       raw()       { return mem_.data(); }

    // Direct pointer into the 16 KB screen RAM (starts at 0040000).
    const uint8_t* videoRam() const { return mem_.data() + ADDR_VIDEO; }

private:
    std::array<uint8_t, 0x10000> mem_{}; // full 64 KB address space (RAM + ROM)
    uint16_t romStart_ = ADDR_ROM_MON;   // first ROM byte address
    IoBus* io_ = nullptr;
    AccessHook hook_;
};

} // namespace bk
