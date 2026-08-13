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

// Устройство в разъёме МПИ (контроллер СМК-512 — единственное, что мы эмулируем).
// Дешифратор такой платы включается только при A15 = 1, то есть на адреса
// 0..077777 контроллер не отзывается вовсе; поэтому Memory спрашивает его лишь
// про верхнюю половину адресного пространства (см. docs/smk512.md).
//
// Контроллер имеет ПРИОРИТЕТ над ПЗУ и над внутренними регистрами БК: он
// физически перебивает их на шине. Но перебивает не всегда — Табл. 1 знает
// ячейки «только чтение» и «только запись», поэтому запись возвращает признак
// «поглотил ли контроллер обращение»: для «теневых» ячеек запись видят ОБА, и
// контроллер, и сама БК.
class MpiDevice {
public:
    virtual ~MpiDevice() = default;
    // Чтение процессором: МОЖЕТ иметь побочные эффекты. У КНГМД они есть — любое
    // обращение к регистру данных 0177132 сбрасывает флаг готовности TR, поэтому
    // отладчику, дизассемблеру и экрану нужен отдельный путь (mpiPeek), иначе
    // простой показ регистра в дампе ломал бы обмен с диском.
    // addr выровнен по слову вызывающим. true — обращение обслужил контроллер.
    virtual bool mpiRead(uint16_t addr, uint16_t& value) = 0;
    // То же, но БЕЗ побочных эффектов — для отладочного просмотра.
    virtual bool mpiPeek(uint16_t addr, uint16_t& value) const = 0;
    // addr НЕ выровнен при isByte. true — контроллер поглотил запись (БК её не видит).
    virtual bool mpiWrite(uint16_t addr, uint16_t value, bool isByte) = 0;
    // Отладочная запись: игнорирует ограничения «только чтение/только запись».
    virtual bool mpiPoke(uint16_t addr, uint16_t value, bool isByte) = 0;
};

// Access-trace hook: called on every CPU-side read/write so the debugger can
// build heatmaps. addr is the byte address; write==true for stores.
using AccessHook = std::function<void(uint16_t addr, bool write, bool isByte)>;

class Memory {
public:
    Memory();

    void reset();                       // clear RAM (keeps ROM)
    void setIoBus(IoBus* bus) { io_ = bus; }
    // Контроллер в разъёме МПИ; nullptr — разъём пуст (штатная БК).
    void setMpi(MpiDevice* d) { mpi_ = d; }
    MpiDevice* mpi() const { return mpi_; }
    void setAccessHook(AccessHook h) { hook_ = std::move(h); }
    // «Зависание»: запись, которую никто не подтвердил — ни устройство МПИ, ни
    // регистры БК, ни ОЗУ (адрес ПЗУ). На живой машине шина не отвечает СИП, и
    // процессор идёт по вектору 4 (§7.4). Плата превращает это в прерывание.
    using BusErrorHook = std::function<void(uint16_t addr)>;
    void setBusErrorHook(BusErrorHook h) { busError_ = std::move(h); }

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
    // Запись в собственную память БК, минуя и контроллер МПИ, и защиту ПЗУ.
    void rawWord(uint16_t addr, uint16_t value) {
        mem_[addr] = static_cast<uint8_t>(value & 0xff);
        mem_[addr + 1] = static_cast<uint8_t>(value >> 8);
    }
    void rawByte(uint16_t addr, uint8_t value) { mem_[addr] = value; }

    std::array<uint8_t, 0x10000> mem_{}; // full 64 KB address space (RAM + ROM)
    uint16_t romStart_ = ADDR_ROM_MON;   // first ROM byte address
    IoBus* io_ = nullptr;
    MpiDevice* mpi_ = nullptr;           // контроллер в разъёме МПИ (СМК-512)
    AccessHook hook_;
    BusErrorHook busError_;
};

} // namespace bk
