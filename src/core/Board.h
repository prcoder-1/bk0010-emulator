#pragma once
#include <string>
#include <set>
#include <map>
#include <deque>
#include <vector>
#include "Memory.h"
#include "Cpu.h"
#include "Screen.h"
#include "Speaker.h"
#include "Vp037.h"
#include "Vm1Timing.h"
#include "Smk.h"
#include "Kngmd.h"
#include "Hdd.h"
#include "Trace.h"
#include "Symbols.h"

namespace bk {

// Interactive-disassembler data markup: a region marked as data (rather than
// code) with a type. `len` is the total byte count it covers (an array/string of
// several elements is one item). The disassembler view renders these as data and
// skips over them.
enum class DataType : uint8_t { Byte = 0, Word = 1, String = 2, Ptr = 3 };
struct DataItem { DataType type; uint16_t len; };

// Эмулируемая машина. БК-0011М отличается всем, что вокруг процессора: 4 МГц,
// 128 Кбайт страничного ОЗУ, свои ПЗУ (БОС с 0140000), регистр палитры/экрана
// 0177662 на запись, кадровое прерывание с запретом. См. docs/bk0011m.md.
enum class Machine : uint8_t { Bk0010 = 0, Bk0011m = 1 };

// Выбор машины в настройках GUI, ключе --machine и MCP. Auto — машину выбирает
// программа (.BIN, Board::binTargetsBk11), без программы — БК-0011М. Явный выбор
// программу не проверяет: что выбрано, то и работает.
enum class MachineChoice : uint8_t { Auto, Bk0010, Bk0011m };

// The BK-0010 "motherboard": wires the CPU, memory, screen and the internal
// I/O registers together and drives the emulation. Pure C++ (no Qt) so it can
// run in a worker thread owned by the GUI.
class Board : public IoBus {
public:
    Board();

    // Выбор машины. Перегружает ПЗУ (если каталог уже известен) и требует reset().
    // По умолчанию звуковой сопроцессор включается только на БК-0011М — setAy()
    // после этого вызова его переопределяет.
    void setMachine(Machine m);
    Machine machine() const { return machine_; }
    bool bk11() const { return machine_ == Machine::Bk0011m; }
    static const char* machineName(Machine m) { return m == Machine::Bk0011m ? "БК-0011М" : "БК-0010-01"; }
    // Выбор машины из ключа или аргумента: auto / авто; bk10 / 0010 / bk0010 — БК-0010-01;
    // bk11m / 0011m / bk0011m — БК-0011М (латиница — в любом регистре). false — не опознано.
    static bool parseMachineChoice(const std::string& s, MachineChoice& out);
    static const char* machineChoiceName(MachineChoice c);    // "auto" / "bk10" / "bk11m"
    // Машина для выбора c: явный — он сам; Auto — по программе binPath (пусто — БК-0011М).
    static Machine machineFor(MachineChoice c, const std::string& binPath = {});
    int  cpuFreqHz() const { return cpuFreqHz_; }

    bool loadRoms(const std::string& romDir);
    void reset();                       // power-on reset (start vector 0100000 / 0140000)

    // Run at least `ticks` CPU ticks worth of instructions. Returns ticks run.
    int  runTicks(int ticks);
    void runFrame();
    // Run one of `nslices` equal slices of a frame, spread across several timer
    // ticks so audio is produced smoothly (not in one 20 ms burst). The 50 Hz frame
    // interrupt is delivered at the first slice; the frame bookkeeping at the last.
    // If the CPU goes idle (WAIT/HALT) it stays idle for the rest of the frame, just
    // like runFrame(). Equivalent to runFrame() when run for all slices 0..nslices-1.
    void runFrameSlice(int slice, int nslices);
    // Длительность кадра БК — НЕ 1/50 секунды: развёртка идёт на 48,83 Гц, что при
    // 3 МГц даёт ровно 61440 тактов ЦП на кадр (docs/БК-docs 11-экран, «Кадровая
    // частота»; подтверждается и конфигом bkemu-QT: -framerate 48.828). Берём величину
    // прямо из геометрии 037 — 320 строк x 384 такта CLKIN, 1 такт ЦП = 2 CLKIN, —
    // чтобы кадр эмулятора и кадр видеоконтроллера совпадали точно.
    // На БК-0011М 037 по-прежнему тактируется 6 МГц, а ЦП — 4 МГц: кадр той же
    // длительности 20,48 мс содержит 81920 тактов.
    int  ticksPerFrame() const {
        return static_cast<int>(int64_t(Vp037::CLKIN_PER_FRAME) * cpuFreqHz_ / CLKIN_HZ);
    }
    static constexpr int CLKIN_HZ = 6000000;   // такт 037 (CLC)
    // Кадровая частота, Гц (нецелая: 3'000'000 / 61440 = 48,83).
    double frameRateHz() const { return double(cpuFreqHz_) / ticksPerFrame(); }

    // Потактовая эмуляция арбитража КР1801ВП1-037: добавляет такты ожидания к
    // обращениям в ДОЗУ во время активной развёртки (см. Vp037). По умолчанию ВКЛ;
    // выключение возвращает прежнее поведение «идеальной» памяти.
    void setArbitration(bool on) { arb037_ = on; }

    // Фаза такта ЦП относительно окна 037. На реальной машине она выпадает случайно
    // при включении питания (делитель D8:B и счётчик PC[2:0] 037 ничем не
    // сбрасываются) и даёт одну из двух таблиц времён команд: 0 — `MOV (R0),R1` = 28
    // (по умолчанию; так сняты эталонные XOP2/XOP2N), 1 — то же = 24.
    void setCpuPhase(int p) { cpuPhase_ = p & 1; }
    // Параметры окна 037 для БК-0011М в U (1/12 такта ЦП): задержка запроса, задержка
    // ответа после решения, смещения решений для фаз 0 и 1. Для калибровки и тестов.
    void setBusModel11(int reqU, int respU, int phase0U, int phase1U) {
        bus11Req_ = reqU; bus11Resp_ = respU; bus11Phase_[0] = phase0U; bus11Phase_[1] = phase1U;
        if (bk11()) { busReqU_ = reqU; busRespU_ = respU; }
    }
    int  cpuPhase() const { return cpuPhase_; }

    // Построчная отрисовка: экран формируется по мере движения луча (модель Vp037),
    // и каждая строка рисуется тем значением регистра 0177664, какое стояло, когда
    // луч её проходил. Даёт построчные эффекты (параллакс, «разрезанный» экран),
    // которые при покадровой отрисовке теряются. Выключено — прежнее поведение:
    // весь кадр рисуется один раз последним значением скролла.
    void setScanlineRender(bool on) { scanlineRender_ = on; }
    bool scanlineRender() const { return scanlineRender_; }
    bool arbitration() const { return arb037_; }
    const Vp037& vp037() const { return vp037_; }

    // Блок расширения памяти СМК-512 в разъёме МПИ (опционально; по умолчанию ВЫКЛ).
    // Включение эквивалентно установке платы в выключенную машину: ДОЗУ обнуляется,
    // контроллер встаёт в режим SYS страницу 0, машину надо перезапустить —
    // раскладка верхней половины адресов меняется целиком. См. docs/smk512.md.
    void setSmk512(bool on);
    bool smk512() const { return smkOn_; }
    const Smk512& smk() const { return smk_; }
    Smk512&       smk()       { return smk_; }

    // Контроллер НГМД (КНГМД) и загрузка с образа диска. Образ — сырой дамп
    // секторов: 800 Кбайт (80 x 2 x 10 x 512), расширения .bkd и .img.
    // Появление контроллера на шине требует его ПЗУ (roms/disk326.rom): без
    // прошивки грузиться нечем — сектора читает драйвер, а не эмулятор.
    //
    // Контроллер и блок СМК-512 работают ВМЕСТЕ: обращения разводит MpiMux (см.
    // ниже). Это не роскошь — системы вроде BKUNIX грузятся только с ОЗУ платы:
    // у голой БК-0010-01 под ядро остаётся около 16 Кбайт.
    // Какое ПЗУ ставить в контроллер: пусто — драйвер 326 (roms/disk326.rom),
    // иначе имя файла в каталоге ПЗУ или полный путь. Фирменная прошивка СМК
    // (roms/smk512.rom) ставится ключом --disk-rom smk: она перехватывает адрес
    // начального пуска и приносит свой ROM-BIOS, но образов с ней поднимается
    // меньше (см. docs/floppy.md).
    void setDiskRom(const std::string& nameOrPath);
    bool attachDisk(int drive, const std::string& path, bool readOnly = true, int sides = 0);
    // Поставить контроллер (ПЗУ + 1801ВП1-128) на шину без дискет — так на БК-0011М
    // с СМК-512, где плата и есть контроллер.
    bool controllerOn();
    void detachDisk(int drive);
    bool diskAttached(int drive = 0) const { return kngmd_.fdd().attached(drive); }
    const std::string& diskPath(int drive = 0) const { return kngmd_.fdd().path(drive); }
    bool diskControllerOn() const { return diskOn_; }
    const Kngmd& kngmd() const { return kngmd_; }
    Kngmd&       kngmd()       { return kngmd_; }
    // Передать управление автозагрузчику в ПЗУ контроллера (точка входа 0160000).
    // Монитор к этому моменту должен быть поднят — см. ensureMonitorBooted().
    bool bootFromDisk();

    // Винчестер IDE (образ HDI, docs/hdd.md): unit 0 — master, 1 — slave. Контроллеров
    // два: канал платы СМК-512 (0177740–0177757, драйвер — в прошивке платы
    // roms/smk512.rom) и «Самара» (0177620–0177656, драйвер — в прошивке КНГМД
    // roms/samara.rom, плата памяти не нужна). Auto — по разметке образа первого
    // подключённого винчестера: разметка «Самары» — её контроллер, иначе СМК.
    // Регистры стоят на шине, пока подключён хотя бы один образ (у СМК — ещё и при
    // установленной плате). Без явного setDiskRom ставится прошивка этого контроллера.
    // readOnly: записанное БК остаётся в памяти поверх файла. Ошибка — в err.
    enum class HddController { Auto, Smk, Samara };
    // auto | smk (smk512) | samara (самара). false — не опознано.
    static bool parseHddController(const std::string& s, HddController& out);
    void setHddController(HddController c) { hddCtl_ = c; }
    HddController hddController() const { return hddCtl_; }
    // Какой контроллер получит образ path, если подключить его сейчас.
    HddController resolveHddController(const std::string& path) const;
    bool hddSamara() const { return samara_; }   // винчестеры сейчас на «Самаре»
    bool attachHdd(int unit, const std::string& path, bool readOnly = true, std::string* err = nullptr);
    void detachHdd(int unit);
    bool hddAttached(int unit = 0) const { return ide_.drive(unit).attached(); }
    bool hddOnBus() const { return mux_.ide != nullptr; }
    const Ide& ide() const { return ide_; }
    Ide&       ide()       { return ide_; }
    // Загрузка с винчестера. СМК: включение питания — прошивка платы перехватывает
    // пуск и грузит систему сама (MBR с блока 0). «Самара»: автозагрузчик КНГМД
    // 0160000 сначала пробует винчестер. false — нет образа, платы или прошивки.
    bool bootFromHdd();

    // Конфигурация машины одним куском — общая для безголового режима и MCP-сервера
    // (GUI берёт её из своих настроек). -1 — по машине: AY, второй AY и Covox стоят на
    // БК-0011М, построчная отрисовка — тоже.
    struct Setup {
        Machine machine = Machine::Bk0011m;
        int  ay = -1, ay2 = -1, covox = -1, scanline = -1;
        bool smk = true, mstd = false, arb037 = true;
        int  cpuPhase = 0;
        std::string diskRom;                         // пусто — прошивка по конфигурации
        HddController hddCtl = HddController::Auto;
    };
    // Включить питание машины в этой конфигурации. false — не загрузились ПЗУ.
    bool powerOn(const Setup& s, const std::string& romDir);
    // Носители из командной строки: дискеты A/B и винчестеры master/slave.
    struct Media {
        std::string disk, diskB, hdd, hddB;
        bool diskRw = false, hddRw = false;
        int  diskSides = 0;                          // 0 — по размеру образа
        bool empty() const { return disk.empty() && diskB.empty() && hdd.empty() && hddB.empty(); }
    };
    // Вставить носители и загрузиться: с дискеты A (из поднятого монитора, как «160000G»),
    // иначе с винчестера. false — носитель не вставился; err — что не так (и
    // предупреждения, если загрузки не будет).
    bool bootMedia(const Media& m, std::string& err);

    // Run enough frames for the monitor ROM to initialise (vectors, stack,
    // display driver) if it hasn't yet. A game must not be started before this,
    // or it will jump into an uninitialised system and crash. Idempotent.
    static constexpr int kBootFrames = 25;    // кадров до готового монитора после сброса
    void ensureMonitorBooted(int bootFrames = kBootFrames) {
        while (framesSinceReset_ < bootFrames) runFrame();
    }

    // Present a raw BK key code to the keyboard controller, exactly like the
    // real BK-0010 (ВП1-014, ср. bk/tty.c): the code register (0177662) holds a
    // SINGLE code, and a new keypress ALWAYS overwrites it — read or not; the
    // "ready" flag (0177660 bit 7) just marks "a code arrived since the last
    // read" and is cleared by reading 0177662. The interrupt (vector 060, or
    // 0274 for codes with bit 0200) fires only if enabled (0177660 bit 6).
    void pressKey(uint16_t bkCode);

    // Клавиша «СТОП». Физически вынесена из матрицы (отдельная цепь STOP_IN/STOP_OUT),
    // кода в 0177662 не даёт, а вызывает прерывание по вектору 4 — тому же, что HALT и
    // зависание. Прерывание ВНЕПРИОРИТЕТНОЕ: запретить его установкой разрядов приоритета
    // нельзя, «СТОП» прерывает программу всегда (см. docs/BK0010-hardware.md).
    // Кнопка заодно дёргает линию ОСТ разъёма МПИ (контакт А1), а по ней блок
    // расширения СМК-512 асинхронно обнуляет регистр режима — SYS, страница 0.
    // Без этого машина после «СТОП» осталась бы с ОЗУ платы на месте ПЗУ и
    // обработать вектор было бы нечем (docs/smk512.md).
    // На БК-0011М клавишу блокирует бит 12 ленточного регистра 0177716.
    void pressStop() {
        if (bk11() && (reg716tap_ & 010000)) return;
        stopPending_ = true;
        if (smkOn_) { smk_.reset(); smkStartMode(); }
    }

    // --- БК-0011М ---
    // Однострочные сводки для отладчика и MCP: карта памяти/палитра/экран/таймер
    // БК-0011М и регистры звукового сопроцессора.
    std::string bk11Summary() const;
    std::string aySummary(int chip = 0) const;
    uint16_t reg662() const { return reg662_; }        // 0177662 на запись: палитра/экран/таймер
    uint16_t reg716tap() const { return reg716tap_; }  // 0177716 (бит 11 = 0): лента/динамик
    // Блок МСТД (ПЗУ 330) в окне 0160000. Его первое слово 000240, поэтому БОС
    // при пуске отдаёт ему управление — по умолчанию блок не вставлен.
    void setMstd(bool on) { mem_.setMstd(on); }
    // Звуковой сопроцессор AY-3-8910 / YM2149 на параллельном порту 0177714.
    void setAy(bool on) { ayOn_ = on; syncSound(); }
    // Плата на два AY («2AY», docs/sound.md). Чип выбирается двумя способами сразу,
    // как их понимают программы для обеих плат (leCSIcon, «200%»): TurboSound — слово,
    // у которого в логическом значении разряды 3–7 единичные, разряд 0 выбирает чип
    // (1 — первый); «Грифон» — разряды 15/14 слова адреса (0 — чип выбран). Выключено —
    // всё идёт в первый чип, как на плате с одним AY.
    void setAy2(bool on);
    bool ay2() const { return ay2On_; }
    // Covox — два 8-разрядных ЦАП «Мультизвука» на порту 0177714: младший байт — левый
    // канал, старший — правый; только слова — стерео, есть байты — моно (левый отсчёт в
    // оба канала). Вместе с AY — автоопределение этой карты: слова и байты вперемешку —
    // протокол AY, ЦАП молчит; молчит он и пока программа читает порт (джойстик, мышь).
    void setCovox(bool on);
    bool covox() const { return covoxOn_; }
    enum class CovoxMode { Off, Idle, AyMuted, PortInput, Active };
    CovoxMode covoxMode() const { return covoxOn_ ? covoxMode_ : CovoxMode::Off; }
    uint8_t covoxSample() const { return covoxL_; }      // младший байт регистра порта (левый ЦАП)
    uint8_t covoxSampleR() const { return covoxH_; }     // старший байт (правый ЦАП в стерео)
    bool    covoxStereo() const { return covoxStereo_; } // стерео Covox: в порт идут только слова
    const Ay8910& ayChip(int chip = 0) const { return chip ? ay2_ : ay_; }
    Ay8910&       ayChip(int chip = 0)       { return chip ? ay2_ : ay_; }
    bool ay() const { return ayOn_; }
    bool ay2Active() const { return ayOn_ && ay2On_ && ay2Seen_; }   // программа обратилась ко второму AY
    int  ayTurboChip() const { return tsChip_; }                     // выбор TurboSound: 0 — первый

    // True while an unread code sits in the register (ready flag set).
    bool keyReady() const { return (kbdStatus_ & 0200) != 0; }

    // Physical key-held state, exposed through 0177716 bit 6 (active-low). Many
    // games (e.g. Digger) poll that bit to detect a key regardless of whether the
    // monitor's keyboard ISR has already read the code register. Set on key-down,
    // cleared on key-up; independent of the ready/DONE flag. See ioRead(REG_SYS).
    void setKeyHeld(bool held) { keyHeld_ = held; }
    bool keyHeld() const { return keyHeld_; }

    // Джойстик на параллельном порту 0177714 (активный уровень — высокий: нажато = 1).
    // Значение 16-битное: часть игр читают порт словом, а раскладка SWCorp держит
    // ВЛЕВО/ВВЕРХ в старшем байте (01000/02000). UI обновляет его каждый кадр из
    // геймпада (SDL2), выбранная раскладка — в классе Gamepad.
    void setJoystick(uint16_t bits) { joystick_ = bits; }
    uint16_t joystick() const { return joystick_; }
    // Последнее значение, записанное программой в 0177714 (отдельный выходной
    // регистр: принтер / ковокс). Чтение порта отдаёт джойстик, а не это.
    uint16_t portOut() const { return portOut_; }

    // --- Breakpoints / debugger support ---
    void toggleBreakpoint(uint16_t addr) {
        if (breakpoints_.count(addr)) removeBreakpoint(addr); else breakpoints_.insert(addr);
    }
    void addBreakpoint(uint16_t addr) { breakpoints_.insert(addr); }
    void removeBreakpoint(uint16_t addr) { breakpoints_.erase(addr); breakConds_.erase(addr); }

    // Optional condition on a breakpoint: only stop when `lhs op val` holds; lhs —
    // регистр Ra, слово или байт памяти по адресу a.
    struct BreakCond {
        enum class Lhs : uint8_t { Reg, Word, Byte } lhs;
        enum class Op : uint8_t { Eq, Ne, Lt, Gt, Ge, Le } op;
        uint16_t a, val;
    };
    void setBreakCond(uint16_t addr, BreakCond c) { breakConds_[addr] = c; }
    bool breakAllows(uint16_t pc) const;   // true if pc's breakpoint (if any) should fire
    bool hasBreakpoint(uint16_t addr) const { return breakpoints_.count(addr) != 0; }
    const std::set<uint16_t>& breakpoints() const { return breakpoints_; }

    // --- Symbols (linker .map) — address <-> name for the disassembler/debugger.
    SymbolTable& symbols() { return symbols_; }
    const SymbolTable& symbols() const { return symbols_; }
    int loadSymbols(const std::string& path) { return symbols_.loadMap(path); }
    // Имена из ПЗУ контроллера дисковода (roms/<прошивка>.sym рядом с образом ПЗУ):
    // точки входа и подпрограммы драйвера. Нижний слой symbols(): свои имена главнее.
    // nullptr — контроллера на шине нет.
    const SymbolTable* romSymbols() const { return diskOn_ ? &romSyms_ : nullptr; }

    // --- Comments (interactive disassembler annotations): address -> free text.
    void setComment(uint16_t a, const std::string& c) { if (c.empty()) comments_.erase(a); else comments_[a] = c; }
    const std::string* comment(uint16_t a) const {
        auto it = comments_.find(a); return it == comments_.end() ? nullptr : &it->second;
    }
    const std::map<uint16_t, std::string>& comments() const { return comments_; }
    void clearComments() { comments_.clear(); }

    // --- Data markup: address -> {type, byte-length}. The disassembler view shows
    // marked regions as data instead of code.
    void setData(uint16_t a, DataType t, uint16_t len) { data_[a] = {t, static_cast<uint16_t>(len ? len : 1)}; }
    void clearData(uint16_t a) { data_.erase(a); }
    const DataItem* dataAt(uint16_t a) const {
        auto it = data_.find(a); return it == data_.end() ? nullptr : &it->second;
    }
    const std::map<uint16_t, DataItem>& dataItems() const { return data_; }
    void clearAllData() { data_.clear(); }
    bool breakHit() const { return breakHit_; }
    void clearBreakHit() { breakHit_ = false; watchHit_ = false; }

    // --- Data watchpoints: stop when an address is read and/or written ---
    // mode bits: 1 = on read, 2 = on write. The access hook sets breakHit_ so the
    // run loops stop right after the accessing instruction.
    void addWatch(uint16_t addr, bool onRead, bool onWrite) {
        watchpoints_[addr] = (uint8_t)((onRead ? 1 : 0) | (onWrite ? 2 : 0));
    }
    void removeWatch(uint16_t addr) { watchpoints_.erase(addr); }
    void clearWatches() { watchpoints_.clear(); }
    const std::map<uint16_t, uint8_t>& watchpoints() const { return watchpoints_; }
    bool     watchHit()   const { return watchHit_; }   // last stop was a watchpoint
    uint16_t watchAddr()  const { return watchAddr_; }   // address that was accessed
    bool     watchWrite() const { return watchWrite_; }  // true = write, false = read
    uint16_t watchPc()    const { return watchPc_; }     // instruction that accessed it

    // Run until a breakpoint is hit, `addr` is reached, or `maxTicks` elapse.
    // Used for "step over" (stop at the return address). Returns true if `addr`
    // (or a breakpoint) was reached.
    bool runUntil(uint16_t addr, int maxTicks);

    // Run (with 50 Hz interrupts) until the call stack becomes shallower than
    // `targetDepth` (i.e. the current subroutine returns), a breakpoint, HALT, or
    // the tick limit. For "step out". Needs the flame/CCT tracker enabled.
    bool runUntilReturn(size_t targetDepth, int maxTicks);

    // Шаг с обходом (F8 отладчика, MCP bk_step_over): вызов — JSR или EMT/TRAP/IOT/BPT —
    // прогоняется до команды за ним (с кадрами и точками останова), прочее — один шаг.
    static constexpr int kDebugRunTicks = 20'000'000;   // бюджет прогонов отладчика (~5–7 с БК)
    void stepOver(int maxTicks = kDebugRunTicks);

    // I/O-register write log (0177600..0177776), captured only while enabled — for
    // debugging how a game programs the keyboard / timer / sound / port.
    // `pc` — адрес инструкции, выполнившей обращение (см. curInstrPc_); logReads
    // включает запись ЧТЕНИЙ (опрос джойстика/клавиатуры — это чтения, без них их
    // не видно). Монитор непрерывно опрашивает 0177660/0177662, поэтому чтения
    // имеет смысл включать вместе с addrFilter (0 = без фильтра).
    // line/word — положение луча 037 в момент обращения (строка от верха видимого
    // поля 0..319, слово строки 0..47): по нему видно, где в кадре сменилась палитра.
    // isByte/odd — байтовая запись и её настоящий адрес (addr выровнен, odd — старший байт).
    struct IoAccess { uint16_t addr, value, pc; uint64_t tick; bool isRead; int16_t line; uint8_t word;
                      bool isByte; bool odd; };
    void setIoLog(bool on, bool logReads = false, size_t cap = 2048, uint16_t addrFilter = 0) {
        ioLogOn_ = on; ioLogReads_ = logReads;
        ioLogCap_ = cap ? cap : 1; ioLogFilter_ = addrFilter;
    }
    void clearIoLog() { ioLog_.clear(); }
    const std::deque<IoAccess>& ioLog() const { return ioLog_; }
    bool     ioLogReads()  const { return ioLogReads_; }
    uint16_t ioLogFilter() const { return ioLogFilter_; }

    // Лог фронтов пьезодинамика (бит 6 регистра 0177716) с тактами: позволяет
    // разложить звук игры на тоны (частота = 1/период меандра) точно, а не гадать
    // по нулевым пересечениям PCM.
    struct SpkEdge { uint64_t tick; uint8_t level; };
    void setSpeakerLog(bool on, size_t cap = 65536) { spkLogOn_ = on; spkLogCap_ = cap ? cap : 1; }
    bool speakerLogOn() const { return spkLogOn_; }
    void clearSpeakerLog() { spkLog_.clear(); }
    const std::deque<SpkEdge>& speakerLog() const { return spkLog_; }

    // Execute exactly one instruction (for the debugger). Returns ticks.
    int  stepInstruction();

    // Load a .BIN file (4-byte header: load addr + length, little-endian).
    // If `run`, sets PC to the load address. Returns false on error.
    // БК-0011М: с `run` — как на живой машине с магнитофона: включение питания с
    // удерживаемой клавишей (БОС сразу уходит в клавиатурный монитор), затем команды
    // монитора «L», «R6/1000» и «G»: файл — в распределение страниц пользователя
    // (040402), SP = 01000, пуск кодом самой БОС. Без `run` файл кладётся в текущую карту.
    bool loadBin(const std::string& path, bool run,
                 uint16_t* outAddr = nullptr, uint16_t* outLen = nullptr);
    // Программа для БК-0011М? Признаки: запись в регистр 0177662/0177663 (палитра,
    // экранная страница, таймер 11М; на БК-0010 этот регистр только читается) — кроме
    // программ для обеих машин, — или вызов БОС (EMT с нечётным номером) в начале кода.
    // Нужен, когда машину не задали явно.
    static bool binTargetsBk11(const std::string& path);
    // БК-0011М: включение питания с удерживаемой клавишей — БОС сразу уходит в
    // клавиатурный монитор (БК-docs §20.8), не пробуя дисковод и не поднимая Бейсик.
    // false — проверки клавиши в БОС не нашли (не та машина или не то ПЗУ).
    bool bootBosMonitor();

    // Accessors
    Cpu&    cpu()    { return cpu_; }
    Memory& memory() { return mem_; }
    Screen& screen() { return screen_; }
    Speaker& sound() { return sound_; }
    Trace&  trace()  { return trace_; }

    // Running CPU-tick counter (free-running, monotonic).
    uint64_t totalTicks() const { return totalTicks_; }

    // Tick timestamps of recent frame-synchronisation events. BK frames are not a
    // fixed 50 Hz: a game paces itself on the programmable timer, spinning until it
    // reaches/passes zero. Each such underflow is recorded here (capped ring), so
    // the profiler can mark real per-game frame boundaries on its time axis.
    const std::deque<uint64_t>& frameBoundaries() const { return frameTicks_; }

    // Log of intercepted EMT 36 file operations (WRITE/READ/FICT), so a debugger
    // can see which tape/disk files a game loaded or saved and with what result.
    struct EmtOp { uint8_t cmd, response; std::string name; uint16_t addr, len; uint64_t tick; };
    const std::deque<EmtOp>& emtLog() const { return emtLog_; }

    // Side-effect-free snapshot of a system I/O register (0177660..0177716) for
    // the debugger — returns the value ioRead would yield, without the side
    // effects (no ready-flag clear, no timer advance). Unknown addr -> raw peek.
    uint16_t peekReg(uint16_t addr) const;

    // Last value the program wrote to an I/O register (0 if never written / not
    // an I/O-page address). Reads may differ from this on real hardware.
    uint16_t peekRegWritten(uint16_t addr) const;

    // Save/restore full emulator state (RAM, CPU, device registers, ввод).
    bool saveState(const std::string& path);
    bool loadState(const std::string& path);
    // То же самое в память — для быстрых чекпоинтов без файлов (MCP-слоты,
    // побитовый перебор джойстика с откатом).
    void saveStateMem(std::vector<uint8_t>& out) const;
    bool loadStateMem(const std::vector<uint8_t>& in);
    // Снимок БК-0011М (магия BK11ST1). loadStateMem переключает машину по магии.
    void saveState11(std::vector<uint8_t>& out) const;
    bool loadState11(const std::vector<uint8_t>& in);
    // Хвост «BKX1» обоих форматов: звук (AY, 2AY, Covox), выход порта, вектор и
    // отложенные события клавиатуры, взведённые прерывания, фаза таймера.
    void saveStateExt(std::vector<uint8_t>& out) const;
    void loadStateExt(const std::vector<uint8_t>& in, size_t off);
private:
    // Общие куски обоих форматов снимка (раскладка — у saveStateMem в Board.cpp).
    struct SnapReader;
    void saveCpuDev(std::vector<uint8_t>& out) const;   // R0–R7, PSW, устройства, ввод, фаза 037
    void loadCpuDev(SnapReader& r);
    void saveSmk(std::vector<uint8_t>& out) const;      // СМК-512: режим, страница, ДОЗУ
    void loadSmk(SnapReader& r);
    void saveBus(std::vector<uint8_t>& out) const;      // фаза цепочки шины
    void loadBus(SnapReader& r);
    void finishLoad(const std::vector<uint8_t>& in, size_t off);
public:

    // IoBus
    bool ioRead(uint16_t addr, uint16_t& value) override;
    bool ioWrite(uint16_t addr, uint16_t value, bool isByte) override;

private:
    Memory mem_;
    Cpu    cpu_{mem_};
    Screen screen_;
    Vp037  vp037_;                 // видеоконтроллер 037: арбитраж доступа к ДОЗУ
    Smk512 smk_;                   // блок расширения памяти в разъёме МПИ
    bool   smkOn_ = false;         // плата СМК-512 установлена
    // Есть ли на шине прошивка контроллера, перехватывающая пуск (smk512.rom): тогда
    // режим после включения и «СТОП» ставит она сама.
    bool smkFirmware() const { return diskOn_ && kngmd_.romStart() != 0; }
    // Режим платы после включения питания и «СТОП». Контроллер сам распознаёт
    // машину и на БК-0011М переходит в Std11 (БК-docs §15.9); без образа его ПЗУ
    // делаем это за прошивку — иначе в SYS сегменты 0,1 накрыли бы БОС на 0140000.
    void smkStartMode();
    // Сигнал ВЭД платы → карта памяти 11М (окно 1 отключено в SYS/Std10/ОЗУ10/All).
    void syncSmk11() { mem_.setWindow1Off(bk11() && smkOn_ && smk_.upperRamOff()); }
    Kngmd  kngmd_;                 // контроллер НГМД: ПЗУ драйвера + 1801ВП1-128
    Ide    ide_;                   // канал IDE (винчестеры) платы СМК-512 или «Самары»
    HddController hddCtl_ = HddController::Auto;
    bool   samara_ = false;        // канал IDE — контроллера «Самара»
    std::string loadedRom_;        // какое ПЗУ по умолчанию стоит в КНГМД (пусто — своё/нет)
    std::string defaultControllerRom() const;
    bool   diskOn_ = false;        // контроллер НГМД на шине
    uint64_t fddNextTick_ = 0;     // такт следующего поворота диска (раз в 64 мкс)
    std::string diskRom_;          // выбранная прошивка контроллера (пусто — авто)
    std::string romDir_;           // откуда грузили ПЗУ — оттуда же берём прошивку НГМД
    bool loadControllerRom();      // прошивка контроллера (diskRom_ или disk326.rom)
    bool loadBin11(uint16_t addr, const std::vector<uint8_t>& data, bool run,
                   uint16_t* outAddr, uint16_t* outLen);

    // На шине МПИ могут быть оба устройства сразу — блок расширения памяти и
    // контроллер НГМД, — поэтому обращения разводит мультиплексор.
    //
    // Порядок разбора повторяет живую машину: регистры 0177130/0177132 физически
    // принадлежат микросхеме 1801ВП1-128 (запись в них видят ОБА — у СМК это
    // защёлка режима, у дисковода команда); дальше приоритет у ДОЗУ платы, если
    // текущий режим Табл. 1 подключил её по этому адресу; и только потом окно
    // 0160000 отдаётся ПЗУ драйвера НГМД.
    class MpiMux : public MpiDevice {
    public:
        Smk512* smk = nullptr;
        Kngmd*  fdc = nullptr;
        Ide*    ide = nullptr;     // регистры НЖМД: СМК — узел платы, «Самара» — своя плата
        bool    samara = false;    // дешифратор «Самары» (0177620/22, 0177640–56)
        bool empty() const { return !smk && !fdc; }
        bool mpiRead(uint16_t addr, uint16_t& value) override;
        bool mpiPeek(uint16_t addr, uint16_t& value) const override;
        bool mpiWrite(uint16_t addr, uint16_t value, bool isByte) override;
        bool mpiPoke(uint16_t addr, uint16_t value, bool isByte) override;
    private:
        bool smkRam(uint16_t addr) const;      // ДОЗУ платы подключено по этому адресу?
        bool smkRomCell(uint16_t addr) const;  // здесь по Табл. 1 видно ПЗУ контроллера
        bool smkVersion(uint16_t addr, uint16_t& value) const;   // слово версии платы 0167776
        uint16_t hddBus(uint16_t addr, uint16_t v) const;       // регистр НЖМД -> шина БК
        bool hddReg(uint16_t addr, HddDrive::Reg& r) const {
            return ide && (samara ? Ide::decodeSamara(addr, r) : Ide::decodeSmk(addr, r));
        }
    };
    MpiMux mux_;
    void updateMpi();

    bool arb037_ = true;           // моделировать арбитраж 037 (по умолчанию ВКЛ)
    bool scanlineRender_ = false;  // построчная отрисовка экрана
    int  renderedLine_ = 0;        // следующая строка кадра, ещё не отрисованная
    int  renderedWord_ = 0;        // в строке renderedLine_ уже нарисованы слова [0, renderedWord_)
    uint16_t rasterScroll_ = 01330;  // скролл, защёлкнутый 037 для текущего поля
    bool scrollLatched_ = false;     // защёлкнут ли он уже в этом кадре
    void latchScroll();
    void renderRasterTo(int line, int word);   // догнать отрисовку до точки луча
    void beginFrameRaster(int over);      // верх кадра: доотрисовать, ресинк луча (+over тактов)
    void beginFrame();                    // граница кадра по абсолютному счёту тактов
    uint64_t frameEnd_ = 0;               // такт конца текущего кадра
    // Журнал обменов текущей инструкции (и ловушки за ней) — для модели шины:
    // адрес, запись ли, и кто ответил (BusDram / BusFast / BusRom).
    enum : uint8_t { BusDram = 0, BusFast = 1, BusRom = 2 };
    struct BusAccess { uint16_t addr; bool write; uint8_t cls; };
    uint8_t busClass(uint16_t addr, bool write) const;
    bool smkRamAccess(uint16_t addr, bool write) const;   // обслужила ли обмен ДОЗУ платы
    static constexpr int kMaxAcc = 16;
    BusAccess acc_[kMaxAcc];
    int  nAcc_ = 0;                // обменов всего (может быть больше kMaxAcc)
    // Часы шины ВМ1 (см. Vm1Timing.h): время в четвертях такта, отсчитанное от
    // 4·(totalTicks_ + busSkew_) — дробная часть момента запроса выборки следующей
    // команды и хвост предыдущей команды после её предвыборки (из ДОЗУ / из быстрой
    // памяти). busSkew_ восстанавливает фазу окна 037 из снимка.
    // Задержка ответа ПЗУ на чтение, в четвертях такта: 1 такт. Петля `SOB R0,.`
    // целиком в ПЗУ (tests/hw/ROMSOB.BIN) на реальной БК-0010-01 — 18,00 тактов вместо
    // 17 у памяти, отвечающей сразу; ROMIO.BIN допускает 1..14.
    static constexpr int kRomReadExtra = 4;
    int  busFrac_ = 0;             // дробная часть момента запроса, U (1/12 такта)
    // Параметры окна 037 в U. БК-0010: окно 8 CLKIN = 4 такта = 48 U, запрос 5/4 такта,
    // ответ 11/4 такта, решения на 3 или 9 U по модулю 48 (фаза 1 или 0). БК-0011М:
    // окно 8 CLKIN = 64 U; запрос/ответ/фаза — по замерам (setBusModel11).
    int  busReqU_ = 15, busRespU_ = 33;
    // БК-0011М: подобрано по таблице Manwe (BK0010-11-instructions-speed.xlsx, код и
    // данные в ДОЗУ) — 122 из 128 форм MOV/CMP/ADD точно, остальные 6 короче на 8
    // тактов. Совпадение даёт целое семейство, важна сумма запрос+ответ ≈ 54 U; запрос
    // оставлен, как у БК-0010. Фаза 0 — класс, в который попала машина Manwe, фаза 1 —
    // другой класс (25 расхождений), как два варианта таблиц у БК-0010.
    int  bus11Phase_[2] = {0, 1};  // смещение решений БК-0011М для --cpu-phase 0 / 1
    int  bus11Req_ = 15, bus11Resp_ = 39;
    int  busWinU() const { return bk11() ? 64 : 48; }
    int  busPhaseU() const { return bk11() ? bus11Phase_[cpuPhase_] : (cpuPhase_ ? 3 : 9); }
    // Период совпадения окна и тактов ЦП: 4 такта у БК-0010, 16 у БК-0011М (3 окна).
    int  busPeriodTicks() const { return bk11() ? 16 : 4; }
    int  prevTeDram_ = 0;
    int  prevTeFast_ = 0;
    int  busSkew_ = 0;
    int  cpuPhase_ = 0;
    // Классы обменов acc_[from..] по шагам расписания `s` (cls[0] — выборка, её
    // задаёт вызывающий). false — обмены не легли на расписание (тогда таблица).
    int  busTicks(uint16_t ir, int tableTicks);   // время команды (модель шины или таблица)
    bool mapAccesses(const Vm1Sched& s, int& from, uint8_t* cls) const;
    // Время по модели шины от момента `ticks` (в тактах ЦП, без busSkew_).
    // fromWait: вход в прерывание из WAIT — выборки нет (см. busStep).
    int  busStep(const Vm1Sched& s, const uint8_t* cls, uint64_t ticks, bool fromWait = false);
    // Вход в прерывание по обменам acc_[from..]; `pc` — адрес отброшенной предвыборки.
    int  enterInterrupt(uint16_t vector, bool iako);
    int  interruptTicks(bool iako, uint16_t pc, int from, uint64_t ticks, int tableTicks,
                        bool fromWait = false);

    Machine machine_ = Machine::Bk0010;
    int cpuFreqHz_ = 3000000;   // тактовая частота ЦП
    uint16_t reg662_ = 047400;  // БК-0011М: 0177662 на запись (палитра 15, таймер выкл, экран 5)
    uint16_t reg716tap_ = 0;    // БК-0011М: 0177716 при бите 11 = 0
    void apply662();            // палитра и экранная страница из reg662_
    void flushRaster();         // дорисовать растр до текущего положения луча
    // Запись в регистр экрана вступает в силу в конце команды (см. rasterWrite).
    void rasterWrite(uint16_t addr, uint16_t value);
    void applyRasterPending();
    void applyRasterValue(uint16_t addr, uint16_t value);
    bool     inStep_ = false;           // идёт cpu_.step() — запись регистра экрана отложить
    bool     rasterPending_ = false;
    bool     pendingLogged_ = false;
    uint16_t pendingAddr_ = 0, pendingVal_ = 0;
    int  vpHalf_ = 0;           // остаток полутактов CLKIN (1 такт ЦП = 1,5 CLKIN на 11М)
    bool ayOn_ = false;         // звуковой сопроцессор на 0177714
    // Микросхема НГМД выдаёт слово раз в 64 мкс: 192 такта при 3 МГц, 256 при 4 МГц.
    uint64_t fddPeriod() const { return static_cast<uint64_t>(cpuFreqHz_ / 15625); }
    void setSpeakerBits(uint16_t v);   // уровень динамика из бит 6,5,2
    int framesSinceReset_ = 0;   // for ensureMonitorBooted()
    bool sliceIdle_ = false;     // CPU went idle (WAIT/HALT) mid-frame — skip its remaining slices

    // Last value written to each I/O-page word (0177600..0177776), for the
    // debugger — hardware often reads back something different than was written.
    uint16_t ioLastWrite_[64] = {0};

    // Internal register state
    uint16_t scroll_    = 01330;  // младший байт — скролл; бит 9 — полный/малый экран
    uint16_t kbdStatus_ = 0;    // 0177660: bit7 = code ready, bit6 = IRQ mask (0=enabled)
    uint16_t kbdData_   = 0;    // 0177662: the single latched key code (7 bits)
    bool     keyIntPending_ = false;
    // Свежезащёлкнутый код придерживает своё прерывание примерно на четверть кадра:
    // игры, которые РАЗРЕШАЮТ прерывание клавиатуры и при этом сами опрашивают
    // регистр (SOKOBAN), должны успеть прочитать код раньше ISR монитора.
    uint64_t keyIntDeferAt_ = 0;        // такт, с которого IRQ клавиатуры выдавать можно
    bool     irqFramePending_ = false;  // взведённый запрос кадрового прерывания 0100
    bool     stopPending_ = false;      // нажата клавиша «СТОП» (вектор 4, вне приоритета)
    bool     busErrorPending_ = false;  // обращение, которое шина не подтвердила («зависание»)
    bool     keyHeld_ = false;   // physical key down (0177716 bit 6, active-low)
    uint16_t keyIntVec_ = 060;
    uint8_t  speaker_   = 0;     // уровень пьезодинамика 0..7 (биты 6,5,2 регистра 0177716)
    uint16_t joystick_  = 0;     // 0177714 на ЧТЕНИЕ: состояние джойстика (нажато = 1)
    uint16_t portOut_   = 0;     // 0177714 на ЗАПИСЬ: отдельная защёлка (принтер/ковокс)
    uint16_t cpuMode_ = 0;            // 0177700 CPU_MODE: доступные по записи разряды 0..2
    bool     sysWriteFlag_ = false;   // бит 2 регистра 0177716: «была запись в системный регистр»
    uint64_t kbdReadyClearAt_ = 0;    // такт отложенного сброса бита готовности (0 = не запланирован)
    void resetDevices();              // сброс периферии по команде RESET
    // Вход ВМ1 в пультовый режим (HALT, «СТОП»): микрокод до сохранения PC/PSW ставит
    // разряд 3 по адресу 177716 [ВМ1 § 8.4] — это запись в системный регистр, и внешняя
    // схема БК (триггер D9.1) взводит признак записи: разряд 2 при чтении 177716. По нему
    // обработчик вектора 4 отличает HALT и «СТОП» от зависания шины (БК-docs §4.7, §20.9;
    // РСП-11 § 1.5, РСП-11М § 1.3). Какое значение при этом попадает в выходной регистр
    // (динамик, двигатель магнитофона), не документировано — его не трогаем.
    void consoleEntry() { sysWriteFlag_ = true; }

    // 1801VM1 programmable interval timer (0177706 limit / 0177710 counter /
    // 0177712 control). Decrements at f/128 (optionally /4, /16); sets the FL
    // event flag on underflow. Polled by software (no interrupt on BK-0010).
    uint16_t timerLimit_ = 0, timerCount_ = 0;
    uint8_t  timerCsr_   = 0;       // control/status (low 8 bits, incl. FL flag)
    uint64_t totalTicks_ = 0;       // running CPU-tick counter
    uint64_t timerStart_ = 0;       // tick at which the current count began
    uint32_t timerPeriod_ = 128;    // CPU ticks per timer decrement
    void timerCheck();              // lazily advance the counter to "now"
    void timerSetMode(uint8_t mode);
    static uint32_t timerPeriodFor(uint8_t csr);   // тактов ЦП на отсчёт по делителям CSR
    // Счётчик продвигается лениво — при чтении регистров. Тот же расчёт без записи в
    // плату нужен отладчику (peekReg), поэтому он отдельно от timerCheck.
    struct TimerState { uint16_t count; uint8_t csr; uint64_t start; };
    bool timerAdvance(TimerState& t, uint64_t now) const;   // true — перезагрузка из предела
    // Значение регистра В-В, каким его прочтёт процессор, но без побочных эффектов
    // чтения. false — регистр не наш (на странице В-В тогда читается 0).
    bool regValue(uint16_t addr, uint16_t& value) const;

    // Tick timestamps of recent timer-underflow (frame-sync) events (capped ring).
    std::deque<uint64_t> frameTicks_;
    void recordFrameBoundary();

    std::deque<EmtOp> emtLog_;   // recent EMT 36 file operations (capped ring)

    std::deque<IoAccess> ioLog_; // recent I/O-register accesses (capped ring)
    bool ioLogOn_ = false, ioLogReads_ = false;
    size_t   ioLogCap_ = 2048;
    uint16_t ioLogFilter_ = 0;   // 0 = все адреса страницы В-В
    uint16_t curInstrPc_ = 0;    // PC исполняемой сейчас инструкции (для лога В-В)

    std::deque<SpkEdge> spkLog_; // фронты динамика (capped ring)
    bool   spkLogOn_ = false;
    size_t spkLogCap_ = 65536;

    // Разбор чтения регистра без побочки логирования (ioRead — обёртка над ним).
    bool ioReadRaw(uint16_t addr, uint16_t& value);
    void logIo(uint16_t addr, uint16_t value, bool read, bool isByte, bool odd);   // в ioLog_, если включён

    std::set<uint16_t> breakpoints_;
    std::map<uint16_t, BreakCond> breakConds_;   // optional per-breakpoint condition
    bool     breakHit_ = false;
    SymbolTable symbols_;                        // linker-map symbols (address <-> name)
    SymbolTable romSyms_;                        // имена из ПЗУ контроллера (нижний слой)
    void loadRomSymbols(const std::string& romPath);
    std::map<uint16_t, std::string> comments_;   // interactive per-address comments
    std::map<uint16_t, DataItem> data_;          // interactive code/data markup

    // Data watchpoints (addr -> mode bits: 1=read, 2=write) and last-hit info.
    std::map<uint16_t, uint8_t> watchpoints_;
    bool     watchHit_ = false, watchWrite_ = false, watchArmed_ = false;
    uint16_t watchAddr_ = 0, watchPc_ = 0;
    void checkWatch(uint16_t addr, bool write, bool isByte);

    // Общий цикл прогонов (runTicks, runUntil, runUntilReturn): команды, простой по WAIT,
    // точки останова и наблюдения. frames — цикл сам ведёт кадры (прогоны отладчика;
    // иначе кадр ведёт вызывающий). target() проверяется после каждой команды.
    enum class RunStop { Budget, Halt, Break, Target };
    template <class Target> RunStop runLoop(int maxTicks, bool frames, Target target, int& done);
    bool breakAt(uint16_t pc) const {
        return !breakpoints_.empty() && breakpoints_.count(pc) && breakAllows(pc);
    }
    void endFrame() { trace_.tick(); ++framesSinceReset_; }   // кадр досчитан
    void deliverFrameInterrupts(); // взвести кадровый запрос 50 Гц и попробовать выдать
    int  tryDeliverInterrupts();   // выдать взведённые запросы; возвращает такты входа
    int  stepCore();               // one instruction + sound/trace bookkeeping
    void advanceDevices(int t);    // время для периферии: луч, звук, диск, клавиатура
    int  idleTicks(int t);         // простой по WAIT: время идёт без команд

    // EMT 36 handler: reads the tape/disk parameter block (address in R1) and
    // performs a file read/write against the host CWD instead of tape/disk.
    // Returns true if it handled the call (so the ROM handler is skipped).
    bool handleEmt36();

    Speaker sound_;
    Ay8910  ay_;                // звуковой сопроцессор на 0177714 (если ayOn_)
    // Второй AY платы «2AY» (если ay2On_). ay2Seen_ — программа хоть раз выбрала его:
    // до этого он не подмешивается, и одиночный AY звучит в полную громкость.
    Ay8910  ay2_;
    bool    ay2On_ = false;
    bool    ay2Seen_ = false;
    uint8_t aySel_ = 1;         // выбранные чипы: бит 0 — первый, бит 1 — второй
    uint8_t tsChip_ = 0;        // чип, выбранный по TurboSound (0 — первый)
    void ayWrite(uint16_t value, uint16_t wide, bool isByte);
    void syncSound() {
        sound_.setAy(ayOn_ ? &ay_ : nullptr);
        sound_.setAy2(ayOn_ && ay2On_ && ay2Seen_ ? &ay2_ : nullptr);
    }
    // Covox на 0177714.
    bool    covoxOn_ = false;
    CovoxMode covoxMode_ = CovoxMode::Idle;
    uint8_t covoxHist_ = 0;     // ширина последних записей в порт: 1 — байт
    uint8_t covoxN_ = 0;        // сколько записей в истории (до 8)
    uint8_t covoxL_ = 0200;     // младший байт регистра порта — левый ЦАП
    uint8_t covoxH_ = 0200;     // старший байт — правый ЦАП (стерео Covox)
    bool    covoxStereo_ = false;   // записи только словами — стерео, иначе моно в оба канала
    uint64_t portReadAt_ = 0;   // такт последнего чтения 0177714 + 1 (0 — не читали)
    void covoxWrite(uint16_t wide, bool isByte, bool odd);
    Trace   trace_;
};

} // namespace bk
