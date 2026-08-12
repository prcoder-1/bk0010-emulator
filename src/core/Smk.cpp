#include "Smk.h"

namespace bk {

namespace {
using C = Smk512::Cell;
constexpr Smk512::Slot no()           { return {C::None, 0}; }   // контроллер молчит
constexpr Smk512::Slot rw(uint8_t s)  { return {C::Rw,   s}; }   // сегмент s, чтение и запись
constexpr Smk512::Slot ro(uint8_t s)  { return {C::Ro,   s}; }   // «R» в Табл. 1
constexpr Smk512::Slot wo(uint8_t s)  { return {C::Wo,   s}; }   // «W» в Табл. 1
constexpr Smk512::Slot rom()          { return {C::Rom,  0}; }   // ПЗУ контроллера
} // namespace

// Табл. 1 «Режимы работы и распределение памяти» [АльтПро], столбцы развёрнуты
// в строки. Проверено против прошивки дешифратора [РТ5] и прошивки ПЛИС реплики
// [CPLD]: они расходятся ровно в двух ячейках, и это в точности те две, для
// которых сама Табл. 1 даёт двойное значение «старая версия / новая». Мы —
// реплика, то есть «новая»: ОЗУ10 в 0177000 не подключает ничего (в оригинале
// 7W), а Hlt10 в 0160000 даёт полноценное ОЗУ (в оригинале 6R).
//
//                        0100000 0110000 0120000 0130000 0140000 0150000 0160000 0170000 0177000
const Smk512::Slot Smk512::kTable[8][9] = {
/* SYS   */ {   no(),   no(), rw(6), rw(7), rw(0), rw(1),  rom(),  rom(),  rom()  },
/* Std10 */ {   no(),   no(), rw(2), rw(3), rw(4), rw(5),  rom(), rw(7),   no()   },
/* ОЗУ10 */ {  rw(0),  rw(1), rw(2), rw(3), rw(4), rw(5),  rw(6), rw(7),   no()   },
/* All   */ {  rw(4),  rw(5), rw(6), rw(7), rw(0), rw(1),  rw(2), rw(3),  ro(3)   },
/* Std11 */ {   no(),   no(),  no(),  no(),  no(),  no(),  rom(), rw(7),   no()   },
/* ОЗУ11 */ {   no(),   no(),  no(),  no(), rw(4), rw(5),  rw(6), rw(7),   no()   },
/* Hlt10 */ {  ro(0),  rw(1), rw(2), rw(3), rw(4), rw(5),  rw(6), rw(7),  wo(7)   },
/* Hlt11 */ {   no(),   no(),  no(),  no(), rw(4), rw(5),  rw(6), rw(7),  wo(7)   },
};

const char* Smk512::modeName(Mode m) {
    static const char* kNames[8] = {"SYS", "Std10", "ОЗУ10", "All",
                                    "Std11", "ОЗУ11", "Hlt10", "Hlt11"};
    return kNames[m & 7];
}

uint16_t Smk512::modeCode(Mode m) {
    // Разряды 04..06 регистра управления; сигналы P4/M10/M11 инверсны им, поэтому
    // «код 0» (Hlt11) и записывают как 020000 — чтобы слово не совпало со снятием
    // строба. Аппаратно разряд 13 никуда не подключён, и код режима у Hlt11 равен 0.
    static const uint16_t kCodes[8] = {0160, 060, 0120, 020, 0140, 040, 0100, 0};
    return kCodes[m & 7];
}

uint16_t Smk512::pageCode(int page) {
    const unsigned p = static_cast<unsigned>(page) & 15;
    return static_cast<uint16_t>(((p & 1) << 10)          // вес 1 — разряд 10
                               | (((p >> 1) & 1) << 2)    // вес 2 — разряд 02
                               | (((p >> 2) & 1) << 3)    // вес 4 — разряд 03
                               | ((p >> 3) & 1));         // вес 8 — разряд 00
}

void Smk512::powerOn() {
    ram_.assign(RAM_BYTES, 0);
    reset();
}

void Smk512::reset() {
    // «После включения питания и после сброса контроллер находится в режиме SYS,
    // страница 0» [CPLD], [ЭМ]. Содержимое ДОЗУ сброс по шине не трогает.
    mode_  = SYS;
    page_  = 0;
    armed_ = false;
}

void Smk512::writeCtrl(uint16_t value) {
    // Трёхтактный протокол [АльтПро], [ДОЗУ], подтверждённый самим ПЗУ v2.05:
    //     MOV     #6,@#177130                    ; строб
    //     MOV     <код режима + код страницы>,@#177130
    //     MOV     #0,@#177130                    ; снятие строба
    // Реплика опознаёт строб по ТОЧНОМУ значению 6 в младшей тетраде (оригинальным
    // СМК-64 и A16M достаточно разрядов 01 и 02). Спутать строб с рабочим кодом
    // нельзя: разряд 01 в коде страницы не участвует никогда, а коды режимов
    // занимают только разряды 04..06.
    if ((value & 017) == STROBE) { armed_ = true; return; }
    if (!armed_) return;            // без строба конфигурация не защёлкивается
    armed_ = false;
    static const Mode kByCode[8] = {HLT11, ALL, RAM11, STD10, HLT10, RAM10, STD11, SYS};
    mode_ = kByCode[(value >> 4) & 7];
    page_ = static_cast<uint8_t>(((value >> 10) & 1)          // разряд 10 — вес 1
                               | (((value >> 2) & 1) << 1)    // разряд 02 — вес 2
                               | (((value >> 3) & 1) << 2)    // разряд 03 — вес 4
                               | ((value & 1) << 3));         // разряд 00 — вес 8
}

bool Smk512::decode(uint16_t addr, Slot& s) const {
    if (ram_.empty()) return false;
    if (addr < ADDR_RAM_END) return false;   // A15 = 0: дешифратор не включается
    // Регистры НЖМД изъяты из теневой области («177 кр.740-760» в [РТ5]). Винчестер
    // мы не эмулируем, поэтому просто не отзываемся — отвечает сама БК.
    if (addr >= HDD_FIRST && addr <= HDD_LAST) return false;
    int blk = (addr >> 12) & 7;
    if (blk == 7 && addr >= 0177000) blk = 8;   // страница регистров — отдельная строка
    s = kTable[mode_][blk];
    return s.cell != Cell::None;
}

void Smk512::store(uint16_t addr, const Slot& s, uint16_t value, bool isByte) {
    if (isByte) {
        ram_[offset(addr, s)] = static_cast<uint8_t>(value & 0xff);
    } else {
        const size_t o = offset(static_cast<uint16_t>(addr & ~1), s);
        ram_[o]     = static_cast<uint8_t>(value & 0xff);
        ram_[o + 1] = static_cast<uint8_t>(value >> 8);
    }
}

bool Smk512::mpiRead(uint16_t addr, uint16_t& value) const {
    addr &= ~1;
    Slot s;
    if (decode(addr, s)) {
        if (s.cell == Cell::Rw || s.cell == Cell::Ro) {
            // «Дыра» по чтению в теневой области: реплика начиная с прошивки ПЛИС
            // 1.3 не перекрывает регистры клавиатуры и скролла, иначе в режиме All
            // машина ослепла бы (§17.10). Эти три адреса лежат только в блоке
            // 0177000..0177777, так что отдельная проверка блока не нужна.
            if (addr != 0177660 && addr != 0177662 && addr != 0177664) {
                const size_t o = offset(addr, s);
                value = static_cast<uint16_t>(ram_[o] | (ram_[o + 1] << 8));
                return true;
            }
        } else if (s.cell == Cell::Rom && addr == REG_VERS) {
            // Единственное, что мы отдаём из ПЗУ контроллера: слово модели и версии.
            // По нему программы и опознают контроллер: CMP @#167776,#176200 / BHIS.
            value = VERSION;
            return true;
        }
    }
    // Регистры НГМД. Дисковод не эмулируется, но отдавать по этим адресам ПЗУ
    // Бейсика ещё хуже, поэтому отвечаем «ничего не готово».
    if (addr == REG_CTRL || addr == REG_DATA) { value = 0; return true; }
    return false;
}

bool Smk512::mpiWrite(uint16_t addr, uint16_t value, bool isByte) {
    Slot s;
    const bool mapped = decode(isByte ? addr : static_cast<uint16_t>(addr & ~1), s);
    // Теневая запись выполняется ПО СТАРОМУ режиму: защёлка регистра и запись в
    // ОЗУ происходят в одном цикле шины. Отсюда и известный побочный эффект — в
    // режимах Hlt в ОЗУ по адресу 0177130 оседает код переключения режима (§17.10).
    if (mapped && (s.cell == Cell::Rw || s.cell == Cell::Wo)) store(addr, s, value, isByte);

    if ((addr & ~1) == REG_CTRL) {
        // Байтовая запись режим не защёлкивает: разработчики требуют одиночного MOV
        // словом («MOV #0 и CLR — не одно и то же»), да и разряд 10 номера страницы
        // в младший байт не помещается.
        if (!isByte) writeCtrl(value);
        return true;
    }
    if ((addr & ~1) == REG_DATA) return true;   // буфер данных НГМД

    if (!mapped) return false;
    if (s.cell == Cell::Rw) return true;        // контроллер поглотил запись
    return false;                               // Ro/Wo/ПЗУ — запись видит и сама БК
}

bool Smk512::mpiPoke(uint16_t addr, uint16_t value, bool isByte) {
    Slot s;
    if (!decode(isByte ? addr : static_cast<uint16_t>(addr & ~1), s)) return false;
    if (s.cell == Cell::Rom) return false;
    store(addr, s, value, isByte);   // отладчику ограничения «чтение/запись» не помеха
    return true;
}

} // namespace bk
