#pragma once
#include <cstdint>
#include <array>
#include <functional>
#include <vector>

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
    // При isByte адрес НЕ выровнен (чётный/нечётный байт); слово — всегда чётный адрес.
    virtual bool ioWrite(uint16_t addr, uint16_t value, bool isByte) = 0;
};

// Устройство в разъёме МПИ (контроллер СМК-512 — единственное, что мы эмулируем).
// Дешифратор такой платы включается только при A15 = 1, то есть на адреса
// 0..077777 контроллер не отзывается вовсе; поэтому Memory спрашивает его лишь
// про верхнюю половину адресного пространства (см. docs/smk512.md).
//
// Контроллер имеет ПРИОРИТЕТ над ПЗУ БК: он физически перебивает его на шине.
// Регистры БК (0177600 и выше) он не перебивает, а складывается с ними монтажным
// ИЛИ — это делает Memory, устройству об этом знать не нужно. Табл. 1 знает
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
    Memory(const Memory&) = delete;            // blk_ указывает внутрь самого объекта
    Memory& operator=(const Memory&) = delete;

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

    bool isRom(uint16_t addr) const {
        if (addr >= ADDR_IO_PAGE) return false;
        return bk11_ ? attr_[addr >> 13] != Ram : addr >= romStart_;
    }

    // Плоские 64 Кбайт БК-0010 (ОЗУ + ПЗУ). На БК-0011М не используются.
    const uint8_t* raw() const { return mem_.data(); }
    uint8_t*       raw()       { return mem_.data(); }

    // Начало отображаемого экранного буфера (16 Кбайт): на БК-0010 это 0040000,
    // на БК-0011М — страница 5 или 6 по биту 15 регистра 0177662.
    const uint8_t* videoRam() const {
        return bk11_ ? ram11_.data() + videoPage_ * PAGE11_BYTES : mem_.data() + ADDR_VIDEO;
    }

    // ---- БК-0011М: 128 Кбайт ОЗУ страницами по 16 Кбайт + страничное ПЗУ ----
    // Карта адресов (БК-docs §20.2): 000000-037777 — страница 0; окно 0 040000-077777 —
    // любая страница; окно 1 0100000-0137777 — страница или ПЗУ-страница 0..3;
    // 0140000-0157777 — БОС (ПЗУ 324); 0160000-0177577 — МСТД (330), если вставлен.
    static constexpr size_t PAGE11_BYTES = 040000;
    static constexpr size_t RAM11_BYTES  = 8 * PAGE11_BYTES;
    static constexpr size_t ROM11_BYTES  = 020000;
    enum Rom11 { ROM_BOS = 0, ROM_MSTD, ROM_BASIC1, ROM_EXT, ROM_BASIC2, ROM_BASIC3, ROM11_SLOTS };
    // Код страницы в регистре 0177716 не двоичный (GID Board_11M.cpp:485).
    static int pageForCode(int code) {
        static const int k[8] = {1, 5, 2, 3, 4, 7, 0, 6};
        return k[code & 7];
    }
    static int codeForPage(int page) {
        static const int k[8] = {6, 0, 2, 3, 4, 1, 7, 5};
        return k[page & 7];
    }
    // ПЗУ-страница, которую регистр ставит в окно 1, или -1 (ОЗУ). Приоритет бит —
    // 0, 1, 3, 4, как у GID.
    static int romPageForReg(uint16_t reg) {
        return (reg & 1) ? 0 : (reg & 2) ? 1 : (reg & 010) ? 2 : (reg & 020) ? 3 : -1;
    }

    void setBk11(bool on);                    // раскладка БК-0011М (выделяет 128 Кбайт)
    bool bk11() const { return bk11_; }
    bool loadRom11(int slot, const char* path);
    bool rom11Loaded(int slot) const { return slot >= 0 && slot < ROM11_SLOTS && romOk_[slot]; }
    void setMstd(bool on) { mstd_ = on; applyMap(); }  // блок МСТД в 0160000
    bool mstd() const { return mstd_; }
    void setPageMap(uint16_t reg);            // запись в 0177716 с битом 11
    uint16_t pageMap() const { return pageReg_; }
    int  window0Page() const { return pageForCode(pageReg_ >> 12); }
    int  window1Page() const { return romPageForReg(pageReg_) < 0 ? pageForCode(pageReg_ >> 8) : -1; }
    int  window1Rom() const { return romPageForReg(pageReg_); }
    void setVideoPage(int page) { videoPage_ = page & 7; }
    int  videoPage() const { return videoPage_; }
    // Образ ПЗУ БК-0011М в слоте (8 Кбайт) или nullptr, если слот пуст.
    const uint8_t* rom11(int slot) const {
        return rom11Loaded(slot) ? rom11_.data() + slot * ROM11_BYTES : nullptr;
    }
    uint8_t*       ram11()       { return ram11_.data(); }
    const uint8_t* ram11() const { return ram11_.data(); }
    // Блок адресов 8 Кбайт отвечает ДОЗУ (ОЗУ БК)? Для модели шины.
    bool isRam(uint16_t addr) const {
        return bk11_ ? (addr < ADDR_IO_PAGE && attr_[addr >> 13] == Ram) : addr < ADDR_RAM_END;
    }

private:
    uint16_t busRead(uint16_t addr);   // чтение слова процессором, addr выровнен
    // Ячейка по текущей карте блоков (на БК-0010 карта тождественна mem_).
    uint8_t*       at(uint16_t a)       { return blk_[a >> 13] + (a & 017777); }
    const uint8_t* at(uint16_t a) const { return blk_[a >> 13] + (a & 017777); }
    // Запись в собственную память БК, минуя и контроллер МПИ, и защиту ПЗУ.
    void rawWord(uint16_t addr, uint16_t value) {
        if (attr_[addr >> 13] == Absent) return;
        uint8_t* p = at(addr);
        p[0] = static_cast<uint8_t>(value & 0xff);
        p[1] = static_cast<uint8_t>(value >> 8);
    }
    void rawByte(uint16_t addr, uint8_t value) {
        if (attr_[addr >> 13] != Absent) *at(addr) = value;
    }
    void applyMap();                     // пересобрать blk_/attr_ по pageReg_ и mstd_

    std::array<uint8_t, 0x10000> mem_{}; // full 64 KB address space (RAM + ROM)
    uint16_t romStart_ = ADDR_ROM_MON;   // first ROM byte address

    enum : uint8_t { Ram = 0, Rom = 1, Absent = 2 };
    uint8_t* blk_[8];                    // текущая карта: 8 блоков по 8 Кбайт
    uint8_t  attr_[8] = {0};             // Ram / Rom / Absent (только для БК-0011М)
    bool     bk11_ = false;
    bool     mstd_ = false;
    uint16_t pageReg_ = 0;               // последнее значение регистра памяти 0177716
    int      videoPage_ = 5;
    std::vector<uint8_t> ram11_;         // 128 Кбайт ОЗУ БК-0011М
    std::vector<uint8_t> rom11_;         // ПЗУ БК-0011М: ROM11_SLOTS по 8 Кбайт
    bool     romOk_[ROM11_SLOTS] = {false};
    std::array<uint8_t, ROM11_BYTES> zero_{};   // заглушка для пустых блоков
    IoBus* io_ = nullptr;
    MpiDevice* mpi_ = nullptr;           // контроллер в разъёме МПИ (СМК-512)
    AccessHook hook_;
    BusErrorHook busError_;
};

} // namespace bk
