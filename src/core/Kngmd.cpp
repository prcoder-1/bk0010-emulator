#include "Kngmd.h"
#include <cstdio>

namespace bk {

bool Kngmd::loadRom(const std::string& path) {
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    std::vector<uint8_t> buf(ROM_BYTES, 0);
    const size_t got = std::fread(buf.data(), 1, buf.size(), f);
    std::fclose(f);
    if (got != ROM_BYTES) return false;   // прошивка 326 — ровно 4 Кбайт
    rom_ = std::move(buf);
    return true;
}

bool Kngmd::mpiRead(uint16_t addr, uint16_t& value) {
    if (inRom(addr)) { value = romWord(addr); return true; }
    // У этих двух чтений ЕСТЬ побочные эффекты: чтение 0177132 снимает флаг
    // готовности TR, чтение состояния пересчитывает индексное окно. Поэтому
    // отладочный путь идёт отдельно, через mpiPeek.
    if (addr == REG_CTRL) { value = fdd_.readStatus(); return true; }
    if (addr == REG_DATA) { value = fdd_.readData();   return true; }
    return false;
}

bool Kngmd::mpiPeek(uint16_t addr, uint16_t& value) const {
    if (inRom(addr)) { value = romWord(addr); return true; }
    if (addr == REG_CTRL) { value = fdd_.statusView(); return true; }
    if (addr == REG_DATA) { value = fdd_.dataView();   return true; }
    return false;
}

bool Kngmd::mpiWrite(uint16_t addr, uint16_t value, bool isByte) {
    const uint16_t a = static_cast<uint16_t>(addr & ~1);
    if (a == REG_CTRL) {
        // Обмен с микросхемой только словный (БК-docs, §14.2г), байтовую запись
        // просто поглощаем: до регистров она не доходит.
        if (!isByte) fdd_.writeCtrl(value);
        return true;
    }
    if (a == REG_DATA) {
        if (!isByte) fdd_.writeData(value);
        return true;
    }
    if (inRom(addr)) return true;   // ПЗУ контроллера: запись никуда не идёт
    return false;
}

bool Kngmd::mpiPoke(uint16_t addr, uint16_t value, bool isByte) {
    (void)value; (void)isByte;
    // Отладчику здесь править нечего: ПЗУ контроллера — постоянная память, а
    // регистры НГМД — не ячейки. Пусть запись уходит в собственную память БК.
    return inRom(addr);
}

} // namespace bk
