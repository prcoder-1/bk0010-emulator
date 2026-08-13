#include "Memory.h"
#include <cstdio>
#include <cstring>

namespace bk {

Memory::Memory() { reset(); }

void Memory::reset() {
    // Clear RAM only (0..romStart_); leave ROM contents intact.
    std::memset(mem_.data(), 0, romStart_);
}

bool Memory::loadRom(uint16_t addr, const uint8_t* data, size_t len) {
    if (static_cast<size_t>(addr) + len > mem_.size()) return false;
    std::memcpy(mem_.data() + addr, data, len);
    if (addr < romStart_) romStart_ = addr;
    return true;
}

bool Memory::loadRomFile(uint16_t addr, const char* path, size_t expectedLen) {
    FILE* f = std::fopen(path, "rb");
    if (!f) { std::fprintf(stderr, "Memory: cannot open ROM '%s'\n", path); return false; }
    std::fseek(f, 0, SEEK_END);
    long sz = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (sz <= 0) { std::fclose(f); return false; }
    if (expectedLen && static_cast<size_t>(sz) != expectedLen) {
        std::fprintf(stderr, "Memory: ROM '%s' size %ld != expected %zu\n", path, sz, expectedLen);
    }
    if (static_cast<size_t>(addr) + sz > mem_.size()) sz = mem_.size() - addr;
    size_t got = std::fread(mem_.data() + addr, 1, static_cast<size_t>(sz), f);
    std::fclose(f);
    if (addr < romStart_) romStart_ = addr;
    return got > 0;
}

// ---- CPU-facing access ------------------------------------------------------

// Порядок опроса везде один: контроллер МПИ (если он занял этот адрес) — потом
// внутренние регистры БК — потом собственная память. Контроллер спрашивается
// только при A15 = 1: его дешифратор ниже 0100000 не включается вовсе.

uint16_t Memory::readWord(uint16_t addr) {
    addr &= ~1; // word access ignores the low address bit
    if (hook_) hook_(addr, false, false);
    if (mpi_ && addr >= ADDR_RAM_END) {
        uint16_t v = 0;
        if (mpi_->mpiRead(addr, v)) return v;
    }
    if (addr >= ADDR_IO_PAGE && io_) {
        uint16_t v = 0;
        if (io_->ioRead(addr, v)) return v;
    }
    return static_cast<uint16_t>(mem_[addr] | (mem_[addr + 1] << 8));
}

uint8_t Memory::readByte(uint16_t addr) {
    if (hook_) hook_(addr, false, true);
    if (mpi_ && addr >= ADDR_RAM_END) {
        uint16_t v = 0;
        if (mpi_->mpiRead(addr & ~1, v)) return (addr & 1) ? (v >> 8) : (v & 0xff);
    }
    if (addr >= ADDR_IO_PAGE && io_) {
        uint16_t v = 0;
        if (io_->ioRead(addr & ~1, v)) return (addr & 1) ? (v >> 8) : (v & 0xff);
    }
    return mem_[addr];
}

void Memory::writeWord(uint16_t addr, uint16_t value) {
    addr &= ~1;
    if (hook_) hook_(addr, true, false);
    if (mpi_ && addr >= ADDR_RAM_END && mpi_->mpiWrite(addr, value, false)) return;
    if (addr >= ADDR_IO_PAGE && io_) {
        if (io_->ioWrite(addr, value, false)) return;
    }
    if (isRom(addr)) return; // ROM is read-only
    rawWord(addr, value);
}

void Memory::writeByte(uint16_t addr, uint8_t value) {
    if (hook_) hook_(addr, true, true);
    if (mpi_ && addr >= ADDR_RAM_END && mpi_->mpiWrite(addr, value, true)) return;
    if (addr >= ADDR_IO_PAGE && io_) {
        if (io_->ioWrite(addr & ~1, value, true)) return;
    }
    if (isRom(addr)) return;
    rawByte(addr, value);
}

// ---- Side-effect-free access ------------------------------------------------

// Отладочные обращения тоже идут через контроллер: дизассемблер, дамп и
// визуализатор памяти должны показывать то же, что видит процессор.

uint16_t Memory::peekWord(uint16_t addr) const {
    addr &= ~1;
    if (mpi_ && addr >= ADDR_RAM_END) {
        uint16_t v = 0;
        if (mpi_->mpiPeek(addr, v)) return v;
    }
    return static_cast<uint16_t>(mem_[addr] | (mem_[addr + 1] << 8));
}

uint8_t Memory::peekByte(uint16_t addr) const {
    if (mpi_ && addr >= ADDR_RAM_END) {
        uint16_t v = 0;
        if (mpi_->mpiPeek(addr & ~1, v)) return (addr & 1) ? (v >> 8) : (v & 0xff);
    }
    return mem_[addr];
}

void Memory::pokeWord(uint16_t addr, uint16_t value) {
    addr &= ~1;
    if (mpi_ && addr >= ADDR_RAM_END && mpi_->mpiPoke(addr, value, false)) return;
    rawWord(addr, value);
}

void Memory::pokeByte(uint16_t addr, uint8_t value) {
    if (mpi_ && addr >= ADDR_RAM_END && mpi_->mpiPoke(addr, value, true)) return;
    rawByte(addr, value);
}

} // namespace bk
