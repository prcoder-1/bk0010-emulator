#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include "Memory.h"
#include "Fdd.h"

namespace bk {

// КНГМД — контроллер НГМД в разъёме МПИ: собственное ПЗУ драйвера (прошивка 326,
// 4 Кбайт по адресам 0160000..0167777) плюс микросхема 1801ВП1-128 с приводами
// (регистры 0177130 и 0177132). Именно так выглядит штатная дисковая
// конфигурация БК-0010-01.
//
// ПЗУ контроллера перебивает ПЗУ Бейсика самой БК в этом окне — на живой машине
// для этого дорабатывают контакты А14 и А29 разъёма МПИ (БК-docs, §14.2), у нас
// это получается само: устройство МПИ опрашивается раньше собственной памяти.
//
// Точки входа драйвера (БК-docs, §17.13): 0160000 — автозагрузка, 0160002 —
// загрузка с выбранного привода (R0), 0160004/0160006 — чтение-запись по номеру
// блока или по координатам, 0160010 — инициализация блока параметров,
// 0160012 — форматирование.
class Kngmd : public MpiDevice {
public:
    enum : uint16_t {
        ROM_FIRST = 0160000,
        ROM_LAST  = 0167777,
        BOOT_ENTRY = 0160000,   // автозагрузка
        REG_CTRL  = 0177130,
        REG_DATA  = 0177132,
    };
    static constexpr size_t ROM_BYTES = 010000;   // 4 Кбайт

    bool loadRom(const std::string& path);
    bool romLoaded() const { return rom_.size() == ROM_BYTES; }
    void unloadRom() { rom_.clear(); }

    Fdd&       fdd()       { return fdd_; }
    const Fdd& fdd() const { return fdd_; }

    // MpiDevice
    bool mpiRead(uint16_t addr, uint16_t& value) override;
    bool mpiPeek(uint16_t addr, uint16_t& value) const override;
    bool mpiWrite(uint16_t addr, uint16_t value, bool isByte) override;
    bool mpiPoke(uint16_t addr, uint16_t value, bool isByte) override;

private:
    bool inRom(uint16_t addr) const { return romLoaded() && addr >= ROM_FIRST && addr <= ROM_LAST; }
    uint16_t romWord(uint16_t addr) const {
        const size_t o = static_cast<size_t>(addr - ROM_FIRST) & (ROM_BYTES - 1);
        return static_cast<uint16_t>(rom_[o] | (rom_[o + 1] << 8));
    }

    std::vector<uint8_t> rom_;
    Fdd fdd_;
};

} // namespace bk
