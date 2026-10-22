#include "Memory.h"
#include <cstdio>
#include <cstring>

namespace bk {

Memory::Memory() { applyMap(); reset(); }

void Memory::reset() {
    // Clear RAM only (0..romStart_); leave ROM contents intact.
    if (bk11_) std::fill(ram11_.begin(), ram11_.end(), 0);
    else std::memset(mem_.data(), 0, romStart_);
}

void Memory::setBk11(bool on) {
    bk11_ = on;
    if (on) {
        ram11_.assign(RAM11_BYTES, 0);
        if (rom11_.empty()) rom11_.assign(ROM11_SLOTS * ROM11_BYTES, 0);
    } else {
        ram11_.clear(); ram11_.shrink_to_fit();
    }
    pageReg_ = 0;
    videoPage_ = 5;
    applyMap();
}

bool Memory::loadRom11(int slot, const char* path) {
    if (slot < 0 || slot >= ROM11_SLOTS) return false;
    if (rom11_.empty()) rom11_.assign(ROM11_SLOTS * ROM11_BYTES, 0);
    romOk_[slot] = false;
    FILE* f = std::fopen(path, "rb");
    if (!f) { std::fprintf(stderr, "Memory: cannot open ROM '%s'\n", path); applyMap(); return false; }
    uint8_t* dst = rom11_.data() + slot * ROM11_BYTES;
    std::memset(dst, 0, ROM11_BYTES);
    const size_t got = std::fread(dst, 1, ROM11_BYTES, f);
    std::fclose(f);
    if (got != ROM11_BYTES)
        std::fprintf(stderr, "Memory: ROM '%s' size %zu != expected %zu\n", path, got, ROM11_BYTES);
    romOk_[slot] = got > 0;
    applyMap();
    return romOk_[slot];
}

void Memory::setPageMap(uint16_t reg) {
    pageReg_ = reg;
    applyMap();
}

// Карта блоков по 8 Кбайт. На БК-0010 — тождественная (защита ПЗУ — по romStart_).
// На БК-0011М — по регистру памяти 0177716 (GID devemu/Board_11M.cpp:485-555).
void Memory::applyMap() {
    if (!bk11_) {
        for (int i = 0; i < 8; ++i) { blk_[i] = mem_.data() + i * ROM11_BYTES; attr_[i] = Ram; }
        if (romsOff_) for (int i = 5; i < 8; ++i) { blk_[i] = zero_.data(); attr_[i] = Absent; }
        return;
    }
    auto ramBlk = [&](int i, int page, int half) {
        blk_[i] = ram11_.data() + page * PAGE11_BYTES + half * ROM11_BYTES;
        attr_[i] = Ram;
    };
    auto romBlk = [&](int i, int slot) {
        if (slot >= 0 && romOk_[slot]) { blk_[i] = rom11_.data() + slot * ROM11_BYTES; attr_[i] = Rom; }
        else { blk_[i] = zero_.data(); attr_[i] = Absent; }   // никто не отвечает — зависание
    };
    ramBlk(0, 0, 0); ramBlk(1, 0, 1);
    const int w0 = pageForCode(pageReg_ >> 12);
    ramBlk(2, w0, 0); ramBlk(3, w0, 1);
    switch (win1Off_ ? -2 : romPageForReg(pageReg_)) {
    case -1: { const int w1 = pageForCode(pageReg_ >> 8); ramBlk(4, w1, 0); ramBlk(5, w1, 1); break; }
    case 0:  romBlk(4, ROM_BASIC2); romBlk(5, ROM_BASIC3); break;
    case 1:  romBlk(4, ROM_BASIC1); romBlk(5, ROM_EXT);    break;
    default: romBlk(4, -1); romBlk(5, -1); break;            // ПЗУ-страницы 2, 3 — пустые розетки;
                                                             // -2 — окно отключено ВЭД (СМК-512)
    }
    romBlk(6, ROM_BOS);
    romBlk(7, mstd_ ? ROM_MSTD : -1);
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
//
// На странице регистров плата регистры БК не отключает: отвечают оба, и при
// конфликте «читается 1» (БК-docs §17.2) — монтажное ИЛИ. Так плата «закрывает»
// регистры в режиме SYS (зеркало ПЗУ) и в All (сегмент 3). Исключение —
// периферийный блок ВМ1 0177700..0177712: его процессор читает изнутри, мимо
// выводов AD (ad_rd = 0 в vm1_qbus.v), так что плата там ни на что не влияет.
uint16_t Memory::busRead(uint16_t raw) {
    const uint16_t addr = raw & ~1;
    const bool io = addr >= ADDR_IO_PAGE && io_;
    if (mpiAt(addr) && !(addr >= 0177700 && addr <= 0177712)) {
        uint16_t v = 0;
        if (mpi_->mpiRead(raw, v)) {
            uint16_t r = 0;
            if (io && io_->ioRead(addr, r)) v |= r;
            return v;
        }
    }
    if (io) {
        uint16_t v = 0;
        if (io_->ioRead(addr, v)) return v;
    }
    // Пустой блок БК-0011М (нет ПЗУ в розетке): ответа на шине нет — зависание.
    if (attr_[addr >> 13] == Absent && addr < ADDR_IO_PAGE) {
        if (busError_) busError_(addr);
        return 0;
    }
    const uint8_t* p = at(addr);
    return static_cast<uint16_t>(p[0] | (p[1] << 8));
}

uint16_t Memory::readWord(uint16_t addr) {
    if (hook_) hook_(addr & ~1, false, false);
    return busRead(addr);   // память A0 при слове не смотрит, а устройство МПИ — может
}

uint8_t Memory::readByte(uint16_t addr) {
    if (hook_) hook_(addr, false, true);
    if (addr < ADDR_RAM_END && !mpiFull_ && attr_[addr >> 13] == Ram) return *at(addr);
    const uint16_t v = busRead(addr);
    return (addr & 1) ? (v >> 8) : (v & 0xff);
}

// Запись в ПЗУ: ПЗУ записи не подтверждает — зависание по вектору 4, на обеих машинах
// (БК-docs §6: «Запись по адресам ПЗУ запрещена аппаратно и приводит к прерыванию по
// вектору 4», монитор сообщает «ЗВ(адрес)»; так же GID devemu/Board.cpp:458-466).
// Пустой блок 11М — тоже. Программы определяют устройства МПИ записью с перехватом
// вектора 4 (AZБК — по 0177172 в области ПЗУ Бейсика БК-0010).
void Memory::writeWord(uint16_t addr, uint16_t value) {
    const uint16_t raw = addr;
    addr &= ~1;
    if (hook_) hook_(addr, true, false);
    if (mpiAt(addr) && mpi_->mpiWrite(raw, value, false)) return;
    if (addr >= ADDR_IO_PAGE && io_) {
        if (io_->ioWrite(addr, value, false)) return;
        // Регистр есть, но запись в него не принимается (0177662 доступен только по
        // чтению): шина не отвечает, и процессор получает «зависание» по вектору 4.
        if (busError_) busError_(addr);
        return;
    }
    if (isRom(addr)) {
        if (busError_) busError_(addr);
        return;
    }
    rawWord(addr, value);
}

void Memory::writeByte(uint16_t addr, uint8_t value) {
    if (hook_) hook_(addr, true, true);
    if (mpiAt(addr) && mpi_->mpiWrite(addr, value, true)) return;
    if (addr >= ADDR_IO_PAGE && io_) {
        if (io_->ioWrite(addr, value, true)) return;   // адрес не выровнен: нужен IoBus для 11М
        if (busError_) busError_(addr);
        return;
    }
    if (isRom(addr)) {
        if (busError_) busError_(addr);
        return;
    }
    rawByte(addr, value);
}

// ---- Side-effect-free access ------------------------------------------------

// Отладочные обращения тоже идут через контроллер: дизассемблер, дамп и
// визуализатор памяти должны показывать то же, что видит процессор.

uint16_t Memory::peekWord(uint16_t addr) const {
    addr &= ~1;
    if (mpiAt(addr)) {
        uint16_t v = 0;
        if (mpi_->mpiPeek(addr, v)) return v;
    }
    const uint8_t* p = at(addr);
    return static_cast<uint16_t>(p[0] | (p[1] << 8));
}

uint8_t Memory::peekByte(uint16_t addr) const {
    if (mpiAt(addr)) {
        uint16_t v = 0;
        if (mpi_->mpiPeek(addr, v)) return (addr & 1) ? (v >> 8) : (v & 0xff);
    }
    return *at(addr);
}

void Memory::pokeWord(uint16_t addr, uint16_t value) {
    addr &= ~1;
    if (mpiAt(addr) && mpi_->mpiPoke(addr, value, false)) return;
    rawWord(addr, value);
}

void Memory::pokeByte(uint16_t addr, uint8_t value) {
    if (mpiAt(addr) && mpi_->mpiPoke(addr, value, true)) return;
    rawByte(addr, value);
}

} // namespace bk
