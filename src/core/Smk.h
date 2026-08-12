#pragma once
#include <cstdint>
#include <cstddef>
#include <vector>
#include "Memory.h"

namespace bk {

// Контроллер СМК-512 («АльтПро») в разъёме МПИ — блок расширения памяти.
//
// Эмулируется ТОЛЬКО память: 512 Кбайт ДОЗУ, регистр управления 0177130 и
// таблица режимов. Собственное ПЗУ контроллера (драйверы, ROM-BIOS), НГМД и
// НЖМД не эмулируются — что именно из этого следует, разобрано в
// docs/smk512.md. Источник: БК-docs, 17-контроллер-СМК.md; там же ссылки на
// первоисточники ([АльтПро] — документация разработчиков, [РТ5] — прошивка
// дешифратора 556РТ5, [CPLD] — прошивка ПЛИС реплики).
//
// Три уровня деления памяти:
//   страница — 32 Кбайт, единица переключения, одновременно активна ровно одна;
//   сегмент  — 010000 байт (4 Кбайт), 8 штук в странице, номера 0..7;
//   окно     — вся верхняя половина адресов, 0100000..0177777; раскладка
//              сегментов в окне задаётся НОМЕРОМ РЕЖИМА и жёстко зашита в
//              дешифратор — «этот сегмент в это окно» выбрать нельзя.
//
// Мы моделируем РЕПЛИКУ СМК-512, а не оригинальный СМК-64. Различий три, и все
// они подтверждены сравнением прошивок ([CPLD] против [РТ5]): реплика ведёт
// себя как «новая» версия в двух спорных ячейках Табл. 1, требует ТОЧНОГО кода
// строба и не имеет разряда 03 «подключение ПЗУ Бейсика».
class Smk512 : public MpiDevice {
public:
    enum : uint16_t {
        REG_CTRL  = 0177130,  // регистр управления (он же регистр НГМД 1801ВП1-128)
        REG_DATA  = 0177132,  // буфер данных НГМД
        REG_VERS  = 0167776,  // модель контроллера и версия ПЗУ
        VERSION   = 0177605,  // СМК-512, ПЗУ v2.05 — сверено с DISK_SMK512_v2.05.rom
        STROBE    = 6,        // строб: точное значение младшей тетрады
        HDD_FIRST = 0177740,  // регистры НЖМД: изъяты из теневой области
        HDD_LAST  = 0177757,
    };
    static constexpr size_t SEG_BYTES  = 010000;              // сегмент 4 Кбайт
    static constexpr size_t SEGS       = 8;                   // сегментов в странице
    static constexpr size_t PAGE_BYTES = SEG_BYTES * SEGS;    // страница 32 Кбайт
    static constexpr size_t PAGES      = 16;                  // 512 Кбайт
    static constexpr size_t RAM_BYTES  = PAGE_BYTES * PAGES;

    // Восемь режимов в порядке индекса прошивки дешифратора [РТ5]: индекс — это
    // тройка инвертированных сигналов (P4, M10, M11), то есть разряды 06,05,04
    // регистра управления.
    enum Mode : uint8_t {
        SYS = 0, STD10 = 1, RAM10 = 2, ALL = 3,
        STD11 = 4, RAM11 = 5, HLT10 = 6, HLT11 = 7,
    };
    static const char* modeName(Mode m);
    // Код включения режима — то, что складывается с кодом страницы и пишется в
    // 0177130 (обратное преобразование к разбору в writeCtrl).
    static uint16_t modeCode(Mode m);
    // Код страницы 0..15 в разрядах 00, 02, 03 и 10.
    static uint16_t pageCode(int page);

    void powerOn();                    // включение питания: SYS, страница 0, ОЗУ обнулено
    void reset();                      // СБРОС по шине: SYS, страница 0, ОЗУ сохраняется
    void release() { ram_.clear(); ram_.shrink_to_fit(); }

    // Запись в регистр управления 0177130 (трёхтактный протокол, см. writeCtrl).
    void writeCtrl(uint16_t value);

    Mode mode() const { return mode_; }
    int  page() const { return page_; }
    bool armed() const { return armed_; }   // строб взведён, ждём код режима

    // Сырой доступ к ДОЗУ по линейному смещению — для отладчика и снимков состояния.
    const std::vector<uint8_t>& ram() const { return ram_; }
    std::vector<uint8_t>&       ram()       { return ram_; }

    // Какой сегмент какой страницы виден по адресу и с какими ограничениями.
    // Возвращает false, если контроллер по этому адресу молчит.
    enum class Cell : uint8_t {
        None,   // контроллер молчит — отвечает сама БК
        Rw,     // ОЗУ контроллера: чтение и запись
        Ro,     // «только чтение»: запись проходит мимо, в БК (квази-ПЗУ)
        Wo,     // «только запись»: чтение идёт из БК, запись оседает в ОЗУ (теневое)
        Rom,    // ПЗУ контроллера — у нас не эмулируется, кроме слова версии
    };
    struct Slot { Cell cell; uint8_t seg; };
    bool decode(uint16_t addr, Slot& s) const;

    // MpiDevice
    bool mpiRead(uint16_t addr, uint16_t& value) const override;
    bool mpiWrite(uint16_t addr, uint16_t value, bool isByte) override;
    bool mpiPoke(uint16_t addr, uint16_t value, bool isByte) override;

private:
    // Табл. 1: [режим][блок]. Блоки 0..6 — по 010000 байт с 0100000; блок 7 —
    // 0170000..0176777; блок 8 — 0177000..0177777 (страница регистров).
    static const Slot kTable[8][9];

    size_t offset(uint16_t addr, const Slot& s) const {
        return static_cast<size_t>(page_) * PAGE_BYTES
             + static_cast<size_t>(s.seg) * SEG_BYTES
             + (addr & (SEG_BYTES - 1));
    }
    void store(uint16_t addr, const Slot& s, uint16_t value, bool isByte);

    std::vector<uint8_t> ram_;   // 512 Кбайт; пусто, пока контроллер не включён
    Mode    mode_  = SYS;
    uint8_t page_  = 0;
    bool    armed_ = false;
};

} // namespace bk
