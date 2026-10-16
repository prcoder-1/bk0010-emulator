#include "Board.h"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <cctype>
#include <filesystem>
#include <system_error>
#include <vector>

namespace bk {

// BK-0010 internal register addresses (octal).
enum : uint16_t {
    REG_KBD_STATUS = 0177660,
    REG_KBD_DATA   = 0177662,
    REG_SCROLL     = 0177664,
    REG_TIMER_LIM  = 0177706,
    REG_TIMER_CNT  = 0177710,
    REG_TIMER_CSR  = 0177712,
    REG_CPU_MODE   = 0177700,   // CPU_MODE — управление режимом
    REG_CPU_IVEC   = 0177702,   // CPU_IVEC — вектор прерывания рестарта
    REG_CPU_ERROR  = 0177704,   // CPU_ERROR — флаги ошибок
    REG_PORT       = 0177714,
    REG_SYS        = 0177716,
    START_VECTOR   = 0100000,
    START_VECTOR11 = 0140000,   // БК-0011М: 0177716 читается как 0140000 — БОС
};

// 1801VM1 timer control-register bits (ported from the reference bk/timer.c).
enum : uint8_t {
    TIM_CONTINUOUS = 0002, // CAP: free-running (count without reload)
    TIM_ENBEND     = 0004, // MON: allow setting the FL/END event flag
    TIM_ONCE       = 0010, // OS:  one-shot
    TIM_START      = 0020, // RUN: counting enabled
    TIM_DIV16      = 0040,
    TIM_DIV4       = 0100,
    TIM_END        = 0200, // FL:  event flag (set on underflow)
};
static constexpr uint32_t TIMER_BASE_PERIOD = 128; // f/128
// Задержка сброса флага готовности клавиатуры (бит 7 регистра 0177660) после чтения
// кода из 0177662 — поведение ВП1-014 (GID: devemu/Board.cpp:1083).
static constexpr uint64_t KBD_READY_CLEAR_TICKS = 900;

// Lazily advance the counter based on how many CPU ticks have elapsed. Mirrors
// bk/timer.c: the counter is only recomputed when a register is read.
void Board::timerCheck() {
    if (!(timerCsr_ & TIM_START)) return;
    uint64_t delta = (totalTicks_ - timerStart_) / timerPeriod_;
    if (delta == 0) return;
    if (timerCount_ > delta) {
        timerCount_ -= static_cast<uint16_t>(delta);
        timerStart_ += delta * timerPeriod_;
        return;
    }
    // Counter reached / passed zero.
    if (timerCsr_ & TIM_ENBEND) timerCsr_ |= TIM_END;
    if ((timerCsr_ & TIM_ONCE) && !(timerCsr_ & TIM_CONTINUOUS)) {
        // Однократный режим (OS=1, CAP=0): «при достижении нуля бит RUN сбрасывается,
        // таймер ПЕРЕЗАГРУЖАЕТСЯ и останавливается» — TVE_LIMIT → TVE_COUNT
        // (04-процессор-К1801ВМ1.md, §4.4, разряд 3). Эталонный bk/timer.c оставляет
        // здесь ноль — это его ошибка: типовая идиома ожидания одновибратора
        // `CMP @#177706, @#177710 / BNE .-` (dizzy2021.bin, пауза между кадрами
        // уровня) тогда не дожидается конца периода никогда.
        timerCount_ = timerLimit_;
        timerCsr_ &= ~TIM_START;   // one-shot: stop
    } else if ((timerCsr_ & TIM_CONTINUOUS) || timerLimit_ == 0) {
        // Free-running: the counter wraps through zero (mod 2^16), no reload.
        // This also covers reload mode with a ZERO limit: a period of one is not a
        // meaningful reload interval, so the counter behaves as a full 16-bit
        // down-counter and reads negative (bit 15) in its upper half. Games use
        // that sign bit as a pacer — e.g. VALLEY.BIN sets limit 0 and its map-music
        // loop spins on `TST @#177710; BPL` until the counter goes negative.
        // Matches the reference bk/timer.c (`timer_count = timer_setup - delta`).
        timerCount_ = static_cast<uint16_t>(static_cast<uint32_t>(timerCount_)
                                            - static_cast<uint32_t>(delta));
        timerStart_ += delta * timerPeriod_;
    } else {
        // Reload mode (limit > 0): on each underflow the counter reloads from the
        // limit. `delta` may span many periods (e.g. a long busy loop between
        // register reads), so fold it modulo one full period — otherwise
        // `limit - delta` would underflow to a huge value and the next poll loop
        // would appear to hang for seconds (Digger frame pacer, commit 1d4128b).
        uint32_t period = static_cast<uint32_t>(timerLimit_) + 1;   // limit..0 inclusive
        uint32_t rem = static_cast<uint32_t>((delta - timerCount_) % period);
        timerCount_ = static_cast<uint16_t>(timerLimit_ - rem);
        timerStart_ += delta * timerPeriod_;
        // Only reload-mode underflow is a genuine frame pacer; free-running / one-shot
        // crossings aren't per-game-frame, so we don't mark them as frame boundaries.
        recordFrameBoundary();
    }
}

// Граница кадра эмулятора — кадровый синхроимпульс 037 (nVSYNC), в тот же момент
// выдаётся кадровое прерывание. Видимые строки предыдущего кадра к этому времени
// уже пройдены (импульс — на 21-й строке гашения), так что дорисовываем хвост и
// начинаем новый кадр с нулевой строки; до неё луч дойдёт через 43 строки.
void Board::beginFrameRaster() {
    if (scanlineRender_) {
        renderRasterTo(Screen::TEX_H, 0);
        renderedLine_ = 0; renderedWord_ = 0;
    }
    vp037_.syncToVsync();
    vpHalf_ = 0;
}

// Догнать отрисовку до строки `line`, слова `word` (всё ДО этой точки луч уже
// прошёл). Строки за пределами видимого поля (гашение) не рисуются.
void Board::renderRasterTo(int line, int word) {
    if (line < 0) return;                       // луч ещё в кадровом гашении
    if (line >= Screen::TEX_H) { line = Screen::TEX_H; word = 0; }
    while (renderedLine_ < line) {
        screen_.renderSpan(mem_, renderedLine_, scroll_, renderedWord_, Screen::WORDS_PER_LINE);
        ++renderedLine_;
        renderedWord_ = 0;
    }
    if (renderedLine_ == line && line < Screen::TEX_H && word > renderedWord_) {
        if (word > Screen::WORDS_PER_LINE) word = Screen::WORDS_PER_LINE;
        screen_.renderSpan(mem_, line, scroll_, renderedWord_, word);
        renderedWord_ = word;
    }
}

// Догнать растр до текущего положения луча — перед сменой палитры/экрана/скролла,
// чтобы уже пройденная лучом часть кадра осталась нарисованной старыми значениями.
void Board::flushRaster() {
    if (scanlineRender_) renderRasterTo(vp037_.fieldLine(), vp037_.lineWord());
}

// Record a frame-synchronisation boundary at the current tick (deduped, capped).
// The timer may cross zero several times between two register reads, but a game
// paces one frame per crossing it observes, so one boundary per timerCheck edge
// is the right granularity for marking frames.
void Board::recordFrameBoundary() {
    if (!frameTicks_.empty() && frameTicks_.back() == totalTicks_) return;
    frameTicks_.push_back(totalTicks_);
    if (frameTicks_.size() > 8192) frameTicks_.pop_front();
}

void Board::timerSetMode(uint8_t mode) {
    timerPeriod_ = TIMER_BASE_PERIOD;
    if (mode & TIM_DIV16) timerPeriod_ *= 16;
    if (mode & TIM_DIV4)  timerPeriod_ *= 4;
    // Real BK behaviour (verified against the GID BKemu core, CCPU::SetSysRegs):
    // ANY write to the control register reloads the counter from the limit — not
    // only on a stopped→running transition. Games rely on this to re-arm the
    // period while the timer keeps running: e.g. VALLEY.BIN's note player writes
    // limit = counter + note_period then rewrites the control register every note,
    // so the reload here is what makes the note period set the tone pitch.
    timerCount_ = timerLimit_;
    timerStart_ = totalTicks_;
    timerCsr_ = mode;
}

// Команда RESET сбрасывает периферию (на PSW она НЕ влияет). Состав — как у GID
// (devemu/Board.cpp:716-732): запрет прерываний от клавиатуры («проверено»),
// обнуление выходного регистра порта и динамика, останов таймера. Входной регистр
// порта (джойстик) не трогаем: это живые линии, а не защёлка.
void Board::resetDevices() {
    kbdStatus_ = 0100;          // бит 6 = маска прерываний: 1 — запрещены
    keyIntPending_ = false;
    kbdReadyClearAt_ = 0;
    portOut_ = 0;
    speaker_ = 0;
    sysWriteFlag_ = false;
    cpuMode_ = 0;
    timerSetMode(0);            // остановить таймер
    // Регистра режима СМК команда RESET НЕ касается. Она дёргает шинный СБРОС
    // (INIT, контакт Б19), а в прошивке ПЛИС реплики это отдельный сигнал
    // (`reset <= not rst_n`); `extended_reg` обнуляется только по `btn_reset` с
    // контакта А1 (ОСТ). Программа, выполнившая RESET в режиме ОЗУ10, карту
    // памяти не теряет. Режим сбрасывают включение питания и «СТОП» — см.
    // powerOn() и pressStop().
}

Board::Board() {
    mem_.setIoBus(this);
    cpu_.setResetHook([this]() { resetDevices(); });
    // Feed the access heatmap (no-op unless the trace is enabled) and the
    // data watchpoints (no-op unless any are set).
    mem_.setBusErrorHook([this](uint16_t) { busErrorPending_ = true; });
    mem_.setAccessHook([this](uint16_t a, bool w, bool b) {
        trace_.access(a, w, b);
        if (!watchpoints_.empty()) checkWatch(a, w, b);
        // Тайминг: журнал обменов команды для модели шины (см. busClass).
        if (nAcc_ < kMaxAcc) acc_[nAcc_] = {a, w, busClass(a, w)};
        ++nAcc_;
        // Журналу НГМД нужен адрес команды, которая лезет в его регистры.
        if (diskOn_ && (a == 0177130 || a == 0177132)) kngmd_.fdd().setContextPc(curInstrPc_);
    });
    // Смена режима СМК-512 на БК-0011М переключает и окно 1 машины (сигнал ВЭД).
    smk_.setModeHook([this]() { syncSmk11(); });
    // Intercept EMT 36 (tape/disk file I/O) and serve it from the host CWD.
    cpu_.setEmt36Hook([this]() { return handleEmt36(); });
}

// Кто отвечает на обмен. ДОЗУ БК (A15=0, оба банка) обслуживает 037 через своё
// окно. Регистры В-В (от 0177600) и ОЗУ СМК-512 отвечают сразу. ПЗУ (К1801РЕ2 БК, а
// заодно ПЗУ контроллеров, замеров для них нет) отвечает с задержкой: тест
// tests/hw/ROMIO.BIN на реальной БК-0010-01 в фазе 1 показал, что чтение из ПЗУ
// на окно 037 медленнее чтения из В-В (docs/slow-memory-timing.md).
uint8_t Board::busClass(uint16_t addr, bool write) const {
    if (bk11()) {
        // БК-0011М: всё ОЗУ (и страницы в окне 1) — ДОЗУ за 037; ПЗУ и пустые розетки — ПЗУ;
        // ОЗУ СМК-512 отвечает сразу, как и на БК-0010.
        if (addr >= 0177600) return BusFast;
        if (smkOn_ && smkRamAccess(addr, write)) return BusFast;
        if (diskOn_ && addr >= Kngmd::ROM_FIRST && addr <= Kngmd::ROM_LAST) return BusRom;
        return mem_.isRam(addr) ? BusDram : BusRom;
    }
    if (addr < 0100000) return BusDram;
    if (addr >= 0177600) return BusFast;
    if (smkOn_ && smkRamAccess(addr, write)) return BusFast;
    return BusRom;
}

// Обмен по адресу `addr` обслуживает ДОЗУ СМК-512, а не БК? Для чтения годятся
// ячейки Табл. 1 «чтение-запись» и «только чтение», для записи — «чтение-запись»
// и «только запись» (теневое ОЗУ); ПЗУ контроллера и молчащие адреса — нет.
bool Board::smkRamAccess(uint16_t addr, bool write) const {
    Smk512::Slot s{};
    if (!smk_.decode(addr, s)) return false;
    return write ? (s.cell == Smk512::Cell::Rw || s.cell == Smk512::Cell::Wo)
                 : (s.cell == Smk512::Cell::Rw || s.cell == Smk512::Cell::Ro);
}

// Время идёт для всего, что вокруг процессора: луч 037 (и построчная отрисовка),
// счётчик тактов (таймер ВМ1 считается от него), динамик и AY, вращение диска,
// отложенный сброс готовности клавиатуры.
void Board::advanceDevices(int t) {
    // Фаза 037 идёт в лок-степе с ЦП — по ней рисуются строки при построчной
    // отрисовке. 1 такт ЦП = 2 такта CLKIN на БК-0010 (3 МГц) и 1,5 на БК-0011М
    // (4 МГц), поэтому счёт — в полутактах CLKIN с переносом остатка.
    vpHalf_ += t * (2 * CLKIN_HZ / cpuFreqHz_);
    vp037_.tick(vpHalf_ >> 1);
    vpHalf_ &= 1;
    // Луч ушёл вперёд — дорисовать пройденные строки текущим значением скролла.
    if (scanlineRender_) renderRasterTo(vp037_.fieldLine(), 0);
    totalTicks_ += static_cast<uint64_t>(t);
    sound_.feed(speaker_, t);
    // Вращение диска. Микросхема 1801ВП1-128 формирует слово за 64 мкс (192 такта
    // при 3 МГц), и драйвер обязан успевать за этим темпом по флагу TR. Крутим
    // ПОСЛЕ КАЖДОЙ команды, а не раз в кадр: опросный цикл драйвера — три-четыре
    // команды, и раз в кадр он не увидел бы ни одного слова.
    if (diskOn_) {
        const uint64_t period = fddPeriod();
        if (totalTicks_ > fddNextTick_ + period * 64)
            fddNextTick_ = totalTicks_;   // большой разрыв (снимок, пауза) — не догоняем
        while (totalTicks_ >= fddNextTick_) {
            kngmd_.fdd().periodic();
            fddNextTick_ += period;
        }
    }
    // Отложенный сброс флага готовности клавиатуры (см. ioRead REG_KBD_DATA).
    if (kbdReadyClearAt_ && totalTicks_ >= kbdReadyClearAt_) {
        kbdStatus_ &= ~0200;
        kbdReadyClearAt_ = 0;
    }
}

// Простой по WAIT: команд нет, но время идёт. Раньше остаток кадра просто
// пропускался — и вместе с ним звук (AY-музыка по прерыванию звучала обрывками),
// вращение диска, таймер и луч. Запросы прерываний разбираются так же, как после
// команды: клавиатура может разбудить процессор посреди кадра.
int Board::idleTicks(int t) {
    advanceDevices(t);
    if (irqFramePending_ || keyIntPending_ || stopPending_ || busErrorPending_) {
        const int it = tryDeliverInterrupts();
        if (it) { advanceDevices(it); t += it; }
    }
    return t;
}

// Execute one instruction with sound + trace bookkeeping. Returns ticks.
int Board::stepCore() {
    uint16_t pcBefore = cpu_.pc();
    // К моменту ioRead/ioWrite регистр PC уже уехал за слова операндов, поэтому
    // лог обращений к В-В берёт адрес инструкции отсюда (тот же приём, что и
    // watchPc_ для точек наблюдения).
    curInstrPc_ = pcBefore;
    trace_.exec(pcBefore);
    watchArmed_ = false;
    uint16_t ir = arb037_ ? mem_.peekWord(pcBefore) : 0;
    nAcc_ = 0;
    int t = cpu_.step();
    if (watchArmed_) watchPc_ = pcBefore;   // the instruction that triggered a watch
    if (arb037_) t = busTicks(ir, t);
    advanceDevices(t);
    // Взведённые запросы разбираются после КАЖДОЙ команды, а не раз в кадр: иначе
    // программа, открывшая маску в середине кадра, ждала бы следующего кадра.
    if (irqFramePending_ || keyIntPending_ || stopPending_ || busErrorPending_) {
        const int it = tryDeliverInterrupts();
        if (it) { advanceDevices(it); t += it; }
    }
    if (trace_.enabled()) {
        uint16_t pcNow = cpu_.pc();
        int16_t delta = static_cast<int16_t>(pcNow - pcBefore);
        if (delta < 0 || delta > 6) trace_.edge(pcBefore, pcNow); // non-sequential = control flow
        trace_.profileStep(mem_.peekWord(pcBefore), pcNow, cpu_.sp(), t); // flame-graph CCT
    }
    return t;
}

// Время команды по модели шины ВМ1 (Vm1Timing.h) по журналу её обменов. Команды,
// для которых расписания нет или обмены на него не легли (HALT, WAIT, RESET, MARK,
// недопустимые коды, EMT 36 с перехватом), идут по таблице: `timingFor` + штраф
// 037 за чтение ДОЗУ, а целиком в быстрой памяти — `timingFast`.
int Board::busTicks(uint16_t ir, int tableTicks) {
    Vm1Sched sched;
    uint8_t cls[1 + 7];
    int from = 1;
    const bool trap = cpu_.traceTrapped();
    if (nAcc_ > 0 && nAcc_ <= kMaxAcc && !acc_[0].write && !cpu_.halted() && !cpu_.waiting()
        && vm1Schedule(ir, sched)) {
        cls[0] = acc_[0].cls;
        if (mapAccesses(sched, from, cls) && (trap ? nAcc_ - from == 4 : from == nAcc_)) {
            const int t = busStep(sched, cls, totalTicks_);
            if (!trap) return t;
            const int it = interruptTicks(false, cpu_.tracePc(), from, totalTicks_ + t, -1);
            if (it >= 0) return t + it;
        }
    }
    prevTeDram_ = prevTeFast_ = 0;
    bool dramRead = false, allFast = nAcc_ > 0;
    for (int i = 0; i < nAcc_ && i < kMaxAcc; ++i) {
        if (acc_[i].cls == BusDram && !acc_[i].write) dramRead = true;
        if (acc_[i].cls == BusDram) allFast = false;
    }
    if (allFast) {
        return cpu_.instrTicksFast(ir) + (trap ? Cpu::INT_ENTRY_TICKS : 0);
    }
    return tableTicks + (dramRead ? Cpu::arbReadPenalty(ir) : 0);
}

// Сопоставляет обмены с шагами по порядку. Команды «чтение-модификация-запись»,
// которые в эмуляторе только пишут (CLR, SXT, MFPS в память), чтения не делают —
// тогда чтение берёт класс следующей за ним записи.
bool Board::mapAccesses(const Vm1Sched& s, int& from, uint8_t* cls) const {
    int i = from;
    for (int k = 0; k < s.n; ++k) {
        const char kind = s.op[k].kind;
        if (kind == 'I') { cls[1 + k] = BusFast; continue; }
        if (i >= nAcc_ || i >= kMaxAcc) return false;
        const BusAccess& a = acc_[i];
        if (kind == 'R' && a.write) {
            if (k + 1 < s.n && s.op[k + 1].kind == 'M') { cls[1 + k] = a.cls; continue; }
            return false;
        }
        if (kind != 'R' && !a.write) return false;
        cls[1 + k] = a.cls;
        ++i;
    }
    from = i;
    return true;
}

// Единица времени U — 1/12 такта ЦП: общий знаменатель четверти такта (полупериод
// CLKIN у БК-0010, где 1 такт = 2 CLKIN) и полупериода CLKIN у БК-0011М (1 такт =
// 1,5 CLKIN). Модель исходно снята для БК-0010 в четвертях такта; там все величины
// просто умножены на 3, и результаты совпадают с прежними до такта.
//
// Окна 037 решаются на полупериодах CLKIN раз в 8 CLKIN (48 U у БК-0010, 64 U у
// БК-0011М). Запрос в ДОЗУ получает окно, если пришёл не позже чем за busReqU_ до
// решающего фронта (задержка запроса в кремнии, в RTL её нет); ответ ЦП видит на
// ближайшем своём такте через busRespU_ после решения. Смещение решений — фаза
// включения питания. Быстрая память отвечает сразу: строб через 6 четвертей после
// запроса, RPLY через полутакт на чтение и такт на запись (ещё такт — у записи в
// чтении-модификации-записи). Так считает eCat3, и это даёт таблицу СМК-512. ПЗУ на
// чтении отвечает на kRomReadExtra четвертей позже, ЦП видит RPLY на фронте своего
// полутакта. Параметры БК-0011М подобраны по замерам Manwe (docs/bk0011m.md).
int Board::busStep(const Vm1Sched& s, const uint8_t* cls, uint64_t ticks) {
    constexpr int64_t T = 12, Q = 3;              // такт и четверть такта в U
    const int64_t base = static_cast<int64_t>(ticks + static_cast<uint64_t>(busSkew_)) * T;
    const int64_t win = busWinU(), phi = busPhaseU();
    auto ceilTo = [](int64_t x, int64_t m) { return (x + m - 1) / m * m; };
    auto done = [&](int64_t q, char kind, uint8_t c) -> int64_t {
        if (c == BusRom && kind == 'R') return ceilTo(q + Q * (8 + kRomReadExtra), T / 2);
        if (c != BusDram) return q + Q * (kind == 'W' ? 10 : kind == 'M' ? 14 : 8);
        int64_t d = q + busReqU_;
        d += ((phi - d) % win + win) % win;
        return ceilTo(d + busRespU_, T);
    };
    int64_t c = done(base + busFrac_, 'R', cls[0]);        // выборка
    int64_t ref = c + T + (T / 2) * (cls[0] != BusDram ? prevTeFast_ : prevTeDram_);  // старт микрокода
    bool fast = cls[0] != BusDram;
    for (int i = 0; i < s.n; ++i) {
        const int gap = s.op[i].gap + (fast ? s.op[i].xf : 0);
        ref = done(ref + (T / 2) * gap, s.op[i].kind, cls[1 + i]);
        fast = cls[1 + i] != BusDram;
    }
    const int64_t next = ref + (T / 2) * (s.tail + (fast ? s.pfXf : 0));
    prevTeDram_ = s.early ? 2 : 0;
    prevTeFast_ = s.teFast;
    busFrac_ = static_cast<int>(next % T);
    return static_cast<int>(next / T - base / T);
}

// Вход в прерывание по шине. Обмены — от acc_[from]; если их нет или они не те
// (вектор не установлен и т.п.), остаётся `tableTicks` (-1 — сообщить вызывающему).
int Board::interruptTicks(bool iako, uint16_t pc, int from, uint64_t ticks, int tableTicks) {
    Vm1Sched s;
    vm1InterruptSchedule(iako, s);
    uint8_t cls[1 + 7];
    cls[0] = busClass(pc, false);
    if (!mapAccesses(s, from, cls) || from != nAcc_) return tableTicks;
    return busStep(s, cls, ticks);
}

// Evaluate a breakpoint's optional condition (unconditional breakpoints allow).
bool Board::breakAllows(uint16_t pc) const {
    auto it = breakConds_.find(pc);
    if (it == breakConds_.end()) return true;
    const BreakCond& c = it->second;
    uint32_t lhs = c.kind == 0 ? cpu_.r[c.a & 7]
                 : c.kind == 1 ? mem_.peekWord(c.a)
                               : mem_.peekByte(c.a);
    switch (c.op) {
    case 0: return lhs == c.val;   case 1: return lhs != c.val;
    case 2: return lhs <  c.val;   case 3: return lhs >  c.val;
    case 4: return lhs >= c.val;   case 5: return lhs <= c.val;
    }
    return true;
}

// Memory-access hook for data watchpoints: fires while an instruction executes,
// so it sets breakHit_ (and arms watchPc_ capture in stepCore) to stop the run.
void Board::checkWatch(uint16_t addr, bool write, bool isByte) {
    auto hit = [&](uint16_t a) {
        auto it = watchpoints_.find(a);
        if (it != watchpoints_.end() && (it->second & (write ? 2 : 1))) {
            watchHit_ = true; watchAddr_ = a; watchWrite_ = write;
            watchArmed_ = true; breakHit_ = true;
        }
    };
    hit(addr);
    if (!isByte) hit(static_cast<uint16_t>(addr + 1));
}

void Board::setMachine(Machine m) {
    machine_ = m;
    cpuFreqHz_ = bk11() ? 4000000 : 3000000;
    busReqU_  = bk11() ? bus11Req_  : 15;
    busRespU_ = bk11() ? bus11Resp_ : 33;
    sound_.setCpuFreq(cpuFreqHz_);
    mem_.setBk11(bk11());
    syncSmk11();
    setAy(bk11());
    if (!romDir_.empty()) loadRoms(romDir_);
}

bool Board::loadRoms(const std::string& romDir) {
    romDir_ = romDir;
    if (bk11()) {
        // Имена — как у GID BKemu (devemu/Config.cpp:245-259), по 8 Кбайт.
        static const struct { int slot; const char* file; } kRoms[] = {
            {Memory::ROM_BOS,    "bk11m_324_bos.rom"},
            {Memory::ROM_EXT,    "bk11m_325_ext.rom"},
            {Memory::ROM_BASIC1, "bk11m_327_basic1.rom"},
            {Memory::ROM_BASIC2, "bk11m_328_basic2.rom"},
            {Memory::ROM_BASIC3, "bk11m_329_basic3.rom"},
            {Memory::ROM_MSTD,   "bk11m_330_mstd.rom"},
        };
        bool ok = true;
        for (const auto& r : kRoms) {
            const bool got = mem_.loadRom11(r.slot, (romDir + "/" + r.file).c_str());
            if (r.slot == Memory::ROM_BOS) ok = got;
        }
        return ok;
    }
    bool ok = true;
    ok &= mem_.loadRomFile(ADDR_ROM_MON, (romDir + "/monit10.rom").c_str(), 8192);
    // BASIC is optional for running games but harmless; load if present.
    mem_.loadRomFile(ADDR_ROM_BASIC, (romDir + "/basic10.rom").c_str(), 24576);
    return ok;
}

void Board::reset() {
    // БК-0011М с СМК-512: контроллер с ПЗУ на шине всегда (см. controllerOn); без
    // платы и без дискет — снять.
    if (bk11() && smkOn_) controllerOn();
    else if (diskOn_) {
        bool any = false;
        for (int d = 0; d < Fdd::DRIVES; ++d) any |= kngmd_.fdd().attached(d);
        if (!any) { diskOn_ = false; updateMpi(); }
    }
    mem_.reset();
    // Включение питания сбрасывает и микросхему 1801ВП1-128 (дискеты остаются в
    // приводах): иначе после работы с диском на другой машине в ней оставались
    // флаги и поиск маркера, и загрузчик БОС на 11М дискету уже не находил.
    kngmd_.fdd().reset();
    // Адрес начального пуска процессор берёт из ячейки 0177716. Обычно там ПЗУ БК
    // с адресом 0100000, но контроллер с собственной прошивкой зеркалит своё ПЗУ
    // на эту ячейку и уводит старт на себя (§17.9) — так грузится машина с СМК.
    const uint16_t start = (diskOn_ && kngmd_.romStart()) ? kngmd_.romStart()
                         : bk11() ? START_VECTOR11 : START_VECTOR;
    // Power-up: priority 7 (all interrupts masked). The monitor lowers the
    // priority itself once it has installed its interrupt vectors.
    cpu_.reset(start, 0340);
    scroll_ = 01330;   // 0330 + бит 9: полный экран (256 строк) по умолчанию
    kbdStatus_ = 0;    // bit 6 (0100) = interrupt MASK (0 => enabled), bit 7 = ready
    kbdData_ = 0;
    keyIntPending_ = false;
    keyIntDeferAt_ = 0;
    irqFramePending_ = false;
    stopPending_ = false;
    busErrorPending_ = false;
    keyHeld_ = false;
    keyIntVec_ = 060;
    // Timer power-on state: остановлен, счётчик и предел — «все единицы».
    //
    // Регистр предела 0177706 по включении питания НЕ ОПРЕДЕЛЁН («зависит от того,
    // в какое псевдослучайное состояние встанут триггеры»; сброс его не меняет) —
    // см. docs/BK0010-hardware.md и нормативное описание блока К1801ВМ1. Эталонный
    // bk/timer.c (и BKBTL) кладут туда 011000, оставляя счётчик 0177777 — то есть
    // трактуют «неопределённое» по-разному для двух соседних регистров одного блока.
    // Берём для обоих одно и то же «все единицы»: тогда счётчик — обычный 16-битный
    // вычитающий (перезагрузка из 0177777 после нуля тождественна свободному ходу),
    // и он проходит через отрицательную половину диапазона.
    //
    // Это важно: распространённая идиома «ждать таймер» — `TST @#177710 / BPL` —
    // ждёт именно знакового бита счётчика. При пределе 011000 счётчик навсегда
    // остаётся в 0..011000, и любая программа, не программирующая предел сама,
    // виснет или молчит: так пропадала музыка «Коробейники» в tetr-music.bin,
    // где обработчик 014 сравнивает счётчик со своей константой по знаку.
    timerCsr_ = 0;
    timerCount_ = 0177777;
    timerLimit_ = 0177777;
    totalTicks_ = 0;
    // Счёт тактов начинается заново — и вращение диска вместе с ним. Иначе следующий
    // оборот оставался «в будущем» прошлой сессии, диск не крутился, и повторный сброс
    // с дискетой в приводе (на 11М её грузит БОС) загрузку не находил.
    fddNextTick_ = fddPeriod();
    busFrac_ = 0; prevTeDram_ = prevTeFast_ = 0; busSkew_ = 0;
    timerStart_ = 0;
    timerPeriod_ = TIMER_BASE_PERIOD;
    frameTicks_.clear();
    emtLog_.clear();
    ioLog_.clear();
    spkLog_.clear();
    speaker_ = 0;
    framesSinceReset_ = 0;
    std::memset(ioLastWrite_, 0, sizeof(ioLastWrite_));
    screen_.setScroll(scroll_);
    // БК-0011М: обе защёлки 0177716 — нули (оба окна — страница 1, ПЗУ в окне 1 нет;
    // GID Board.cpp:752), 0177662 = 047400: палитра 15, таймер выключен, экран 5.
    reg716tap_ = 0;
    reg662_ = 047400;
    if (bk11()) { mem_.setPageMap(0); apply662(); }
    else screen_.setPalette(0);
    ay_.reset();
    vp037_.reset();
    vp037_.setM256(scroll_ & 01000);   // бит 9 — полный/малый экран
    if (smkOn_) { smk_.powerOn(); smkStartMode(); }   // включение питания: ДОЗУ чисто
    trace_.reset();
}

// На шине МПИ одновременно живёт одно устройство. Контроллер НГМД и СМК-512
// занимают одни и те же адреса (0177130/0177132 — это вообще один и тот же узел
// 1801ВП1-128, а окно 0160000 — ПЗУ и того, и другого), поэтому вставленный диск
// вытесняет СМК: совмещать их можно только с ПЗУ самого СМК, которого у нас нет.
// ДОЗУ платы подключено по этому адресу? Ячейки «только запись» сюда не входят:
// читать их всё равно нельзя, чтение по таким адресам идёт мимо платы.
bool Board::MpiMux::smkRam(uint16_t addr) const {
    if (!smk) return false;
    Smk512::Slot s;
    return smk->decode(addr, s) && (s.cell == Smk512::Cell::Rw || s.cell == Smk512::Cell::Ro);
}

// Где именно видно ПЗУ контроллера, решает Табл. 1 платы: в режимах SYS, Std10 и
// Std11 это окно 0160000, а в SYS оно ещё и зеркалится на 0170000-0177777 —
// микросхеме ПЗУ достаются лишь младшие разряды адреса (§17.9). Зеркало закрывает
// и страницу регистров: регистры БК при этом тоже отвечают, и Memory складывает
// оба ответа монтажным ИЛИ. Вне закрытия остаются только «дыра» 0177660..0177664
// и регистры НЖМД (их изымает decode).
bool Board::MpiMux::smkRomCell(uint16_t addr) const {
    if (!smk || !fdc || Smk512::readHole(addr)) return false;
    Smk512::Slot s;
    if (!smk->decode(addr, s) || s.cell != Smk512::Cell::Rom) return false;
    // Зеркалит своё ПЗУ только плата с собственной прошивкой — та, что и старт
    // перехватывает. У простого КНГМД (драйвер 326) ПЗУ видно лишь в своём окне
    // 0160000, и подменять им ещё и 0170000 было бы выдумкой.
    return fdc->romStart() != 0 || addr <= Kngmd::ROM_LAST;
}

// Слово модели и версии СМК (0167776). На живой машине окно 0160000 занимает ПЗУ
// самой платы, и это слово там есть всегда; программы опознают плату только по нему
// (Prince of Persia для 11М: CMP @#167776,#177600 / ... #177700). Мы ставим в окно ПЗУ
// драйвера 326 — в нём по этому адресу 0, — поэтому слово отдаёт плата.
bool Board::MpiMux::smkVersion(uint16_t addr, uint16_t& value) const {
    if (!smk || addr != Smk512::REG_VERS) return false;
    Smk512::Slot s;
    if (!smk->decode(addr, s) || s.cell != Smk512::Cell::Rom) return false;
    value = Smk512::VERSION;
    return true;
}

bool Board::MpiMux::mpiRead(uint16_t addr, uint16_t& value) {
    if (fdc && (addr == Kngmd::REG_CTRL || addr == Kngmd::REG_DATA))
        return fdc->mpiRead(addr, value);          // регистры микросхемы НГМД
    if (smkRam(addr)) return smk->mpiRead(addr, value);
    if (smkVersion(addr, value)) return true;
    if (fdc && smkRomCell(addr) && fdc->romMirror(addr, value)) return true;  // зеркало ПЗУ
    if (fdc && fdc->mpiRead(addr, value)) return true;   // ПЗУ контроллера в своём окне
    return smk && smk->mpiRead(addr, value);       // слово версии СМК и прочее
}

bool Board::MpiMux::mpiPeek(uint16_t addr, uint16_t& value) const {
    if (fdc && (addr == Kngmd::REG_CTRL || addr == Kngmd::REG_DATA))
        return fdc->mpiPeek(addr, value);
    if (smkRam(addr)) return smk->mpiPeek(addr, value);
    if (smkVersion(addr, value)) return true;
    if (fdc && smkRomCell(addr) && fdc->romMirror(addr, value)) return true;
    if (fdc && fdc->mpiPeek(addr, value)) return true;
    return smk && smk->mpiPeek(addr, value);
}

bool Board::MpiMux::mpiWrite(uint16_t addr, uint16_t value, bool isByte) {
    // Регистр управления на живой плате ОДИН, поэтому запись достаётся обоим:
    // СМК защёлкивает режим, дисковод исполняет команду.
    //
    // Исключение — три такта протокола переключения памяти. На это время плата
    // БЛОКИРУЕТ регистры НГМД (§17.5), и не зря: строб «6» в младшей тетраде для
    // микросхемы 1801ВП1-128 означает «выбрать привод 1», а завершающий «MOV #0»
    // — «снять выбор и выключить двигатель». Пропусти мы их к дисководу, любое
    // переключение страниц посреди обмена рвало бы его. Ровно на этом вставала
    // ANDOS с диска LAND.bkd: она меняет страницы, пока драйвер читает сектор.
    const uint16_t a = static_cast<uint16_t>(addr & ~1);
    const bool fddReg = (a == Kngmd::REG_CTRL || a == Kngmd::REG_DATA);
    // Строб — только запись в регистр управления: слово данных с младшей тетрадой 6
    // (запись сектора) обязано дойти до дисковода.
    const bool inProtocol = smk && fddReg
        && (smk->fddBlocked()
            || (a == Kngmd::REG_CTRL && !isByte && (value & 017) == Smk512::STROBE));

    bool taken = false;
    if (smk) taken |= smk->mpiWrite(addr, value, isByte);
    if (fdc && !inProtocol) taken |= fdc->mpiWrite(addr, value, isByte);
    else if (fdc && inProtocol) taken = true;   // запись поглотила плата
    return taken;
}

bool Board::MpiMux::mpiPoke(uint16_t addr, uint16_t value, bool isByte) {
    if (smk && smk->mpiPoke(addr, value, isByte)) return true;
    return fdc && fdc->mpiPoke(addr, value, isByte);
}

void Board::updateMpi() {
    mux_.smk = smkOn_  ? &smk_   : nullptr;
    mux_.fdc = diskOn_ ? &kngmd_ : nullptr;
    mem_.setMpi(mux_.empty() ? nullptr : &mux_);
}

// Прошивка контроллера в окно 0160000. Какое ПЗУ ставить, решает конфигурация
// машины: у платы СМК-512 своя прошивка (v2.05 с ROM-BIOS, roms/smk512.rom) — именно
// её ждут системы для БК-0010 с расширением памяти; у простого КНГМД — драйвер 326.
bool Board::loadControllerRom() {
    if (!kngmd_.romLoaded()) {
        bool ok = false;
        if (!diskRom_.empty()) {
            ok = kngmd_.loadRom(diskRom_) || kngmd_.loadRom(romDir_ + "/" + diskRom_);
        } else {
            // По умолчанию — драйвер 326: с ним поднимается больше образов, чем с
            // фирменной прошивкой СМК (та нужна дискам, которые зовут её ROM-BIOS,
            // и включается ключом --disk-rom smk). См. docs/floppy.md.
            ok = kngmd_.loadRom(romDir_ + "/disk326.rom");
        }
        if (!ok) {
            std::fprintf(stderr, "КНГМД: нет прошивки в %s (нужен smk512.rom или disk326.rom)\n",
                         romDir_.c_str());
            return false;
        }
    }
    return true;
}

// Контроллер на шину. На БК-0011М с СМК-512 он стоит и без дискет: плата — это и
// контроллер НГМД со своим ПЗУ, а БОС ищет дисковод записью в 0177130 (запись СМК
// принимает) и при успехе зовёт загрузчик 0160000. Не будь там ПЗУ, БОС ушёл бы в
// пустоту вместо БЕЙСИКа.
bool Board::controllerOn() {
    if (!loadControllerRom()) return false;
    if (!diskOn_) {
        diskOn_ = true;
        fddNextTick_ = totalTicks_ + fddPeriod();
        updateMpi();
    }
    return true;
}

bool Board::attachDisk(int drive, const std::string& path, bool readOnly, int sides) {
    if (!loadControllerRom()) return false;
    if (!kngmd_.fdd().attach(drive, path, readOnly, sides)) {
        std::fprintf(stderr, "КНГМД: не удалось открыть образ %s\n", path.c_str());
        return false;
    }
    if (!diskOn_) {
        diskOn_ = true;
        fddNextTick_ = totalTicks_ + fddPeriod();
        updateMpi();
    }
    return true;
}

void Board::detachDisk(int drive) {
    kngmd_.fdd().detach(drive);
    for (int d = 0; d < Fdd::DRIVES; ++d) if (kngmd_.fdd().attached(d)) return;
    if (bk11() && smkOn_) return;         // СМК на 11М — контроллер остаётся (см. controllerOn)
    diskOn_ = false;                      // приводов не осталось — контроллер с шины
    updateMpi();
}

// Автозагрузка: управление на точку входа драйвера. Ровно то же делает человек,
// набирая в мониторе «160000G». Стек и векторы поднимает сам драйвер.
bool Board::bootFromDisk() {
    if (!diskOn_ || !kngmd_.romLoaded()) return false;
    // БК-0011М: БОС при пуске сам находит контроллер и вызывает загрузчик 0160000
    // (БК-docs §20.8) — достаточно сбросить машину с подключённым диском.
    if (bk11()) { reset(); return true; }
    // Прошивка СМК перехватывает адрес начального пуска: машина должна стартовать
    // с неё, а не с монитора, иначе своей инициализации (ROM-BIOS, векторы,
    // рабочий режим) контроллер не сделает и система с диска не поднимется.
    if (const uint16_t start = kngmd_.romStart()) {
        reset();
        return true;
    }
    cpu_.r[7] = Kngmd::BOOT_ENTRY;
    cpu_.psw = 0;                          // как при запуске программы из монитора:
                                           // плотный опросный цикл драйвер закрывает сам
    return true;
}

void Board::smkStartMode() {
    if (bk11() && !smkFirmware()) smk_.setMode(Smk512::STD11, 0);
}

void Board::setSmk512(bool on) {
    if (on == smkOn_) return;
    smkOn_ = on;
    if (on) { smk_.powerOn(); smkStartMode(); } else smk_.release();
    syncSmk11();
    updateMpi();
}

// ---- Save / restore state ---------------------------------------------------
namespace {
constexpr char kMagic[8] = {'B','K','1','0','S','T','1','\0'};
// БК-0011М: свой формат — 128 Кбайт ОЗУ и регистры страниц/палитры/AY.
// Раскладка: магия(8) + ОЗУ(128К) + R0-R7 + PSW + dev[7] + ext[2] + v037(u32)
// + m11[8] = {память 0177716, лента 0177716, 0177662, полутакт CLKIN, AY вкл,
// адрес AY, МСТД, резерв} + регистры AY(16) + bus[4].
constexpr char kMagic11[8] = {'B','K','1','1','S','T','1','\0'};
}

// Раскладка снимка: магия(8) + ОЗУ(0100000) + R0-R7(8 слов) + PSW + dev[7] и —
// с этой версии — хвост ext[2] = {joystick_, keyHeld_}. Магию не меняем: снимки
// старого формата просто короче на хвост и читаются как «ввод отпущен».
namespace {
constexpr size_t kStateBase = 8 + ADDR_RAM_END + (8 + 1 + 7) * sizeof(uint16_t);
constexpr size_t kStateFull = kStateBase + 2 * sizeof(uint16_t);          // + {joystick, keyHeld}
constexpr size_t kStateV037 = kStateFull + sizeof(uint32_t);              // + фаза развёртки 037
constexpr size_t kStateSmk  = kStateV037 + 4 * sizeof(uint16_t);          // + {есть, режим, страница, строб}
constexpr size_t kStateSmkRam = kStateSmk + Smk512::RAM_BYTES;            // + 512 Кбайт ДОЗУ
}

void Board::saveStateMem(std::vector<uint8_t>& out) const {
    out.clear();
    if (bk11()) { saveState11(out); return; }
    out.reserve(smkOn_ ? kStateSmkRam : kStateSmk);
    auto put = [&](const void* p, size_t n) {
        const uint8_t* b = static_cast<const uint8_t*>(p);
        out.insert(out.end(), b, b + n);
    };
    put(kMagic, 8);
    // Mutable memory is RAM 0..0100000 (ROM above is constant).
    put(mem_.raw(), ADDR_RAM_END);
    put(cpu_.r, 8 * sizeof(uint16_t));
    put(&cpu_.psw, sizeof(uint16_t));
    const uint16_t dev[7] = {scroll_, kbdStatus_, kbdData_, timerLimit_, timerCount_, timerCsr_, speaker_};
    put(dev, sizeof dev);
    const uint16_t ext[2] = {joystick_, static_cast<uint16_t>(keyHeld_ ? 1 : 0)};
    put(ext, sizeof ext);
    const uint32_t v037 = vp037_.pack();   // фаза развёртки: где стоял луч
    put(&v037, sizeof v037);
    // Блок СМК-512: конфигурация всегда, ДОЗУ — только если плата установлена.
    const uint16_t smk[4] = {static_cast<uint16_t>(smkOn_ ? 1 : 0),
                             static_cast<uint16_t>(smk_.mode()),
                             static_cast<uint16_t>(smk_.page()),
                             static_cast<uint16_t>(smk_.armed() ? 1 : 0)};
    put(smk, sizeof smk);
    if (smkOn_) put(smk_.ram().data(), Smk512::RAM_BYTES);
    // Фаза цепочки шины: где стоит окно 037 относительно счёта тактов и хвост
    // предыдущей команды — иначе прогоны из одного снимка расходятся по тактам.
    // busFrac_ БК-0010 кратен 3 U и пишется в четвертях такта, как до перехода на U.
    const uint16_t bus[4] = {static_cast<uint16_t>((totalTicks_ + busSkew_) % busPeriodTicks()),
                             static_cast<uint16_t>(bk11() ? busFrac_ : busFrac_ / 3),
                             static_cast<uint16_t>(prevTeDram_),
                             static_cast<uint16_t>(prevTeFast_)};
    put(bus, sizeof bus);
}

bool Board::loadStateMem(const std::vector<uint8_t>& in) {
    if (in.size() >= 8 && std::memcmp(in.data(), kMagic11, 8) == 0) return loadState11(in);
    if (in.size() < kStateBase || std::memcmp(in.data(), kMagic, 8) != 0) return false;
    if (bk11()) { setMachine(Machine::Bk0010); setAy(false); }   // снимок другой машины
    size_t off = 8;
    auto get = [&](void* p, size_t n) { std::memcpy(p, in.data() + off, n); off += n; };
    get(mem_.raw(), ADDR_RAM_END);
    get(cpu_.r, 8 * sizeof(uint16_t));
    get(&cpu_.psw, sizeof(uint16_t));
    uint16_t dev[7] = {0};
    get(dev, sizeof dev);
    uint16_t ext[2] = {0, 0};
    if (in.size() >= kStateFull) get(ext, sizeof ext);   // иначе — снимок старого формата
    if (in.size() >= kStateV037) { uint32_t v = 0; get(&v, sizeof v); vp037_.unpack(v); }
    if (in.size() >= kStateSmk) {
        uint16_t smk[4] = {0, 0, 0, 0};
        get(smk, sizeof smk);
        setSmk512(smk[0] != 0);           // сначала плата, потом её состояние
        if (smkOn_) {
            smk_.writeCtrl(Smk512::STROBE);
            smk_.writeCtrl(static_cast<uint16_t>(Smk512::modeCode(static_cast<Smk512::Mode>(smk[1] & 7))
                                               | Smk512::pageCode(smk[2])));
            if (smk[3]) smk_.writeCtrl(Smk512::STROBE);   // строб был взведён
            if (in.size() >= kStateSmkRam) get(smk_.ram().data(), Smk512::RAM_BYTES);
        }
    }
    uint16_t bus[4] = {0, 0, 0, 0};
    const bool hasBus = in.size() >= off + sizeof bus;
    if (hasBus) get(bus, sizeof bus);
    scroll_ = dev[0]; kbdStatus_ = dev[1]; kbdData_ = dev[2];
    keyIntPending_ = false; keyIntDeferAt_ = 0;
    timerLimit_ = dev[3]; timerCount_ = dev[4];
    timerCsr_ = static_cast<uint8_t>(dev[5]);
    speaker_ = static_cast<uint8_t>(dev[6]);
    joystick_ = ext[0];
    keyHeld_  = ext[1] != 0;
    // Re-derive the timer phase so timerCheck() doesn't see a huge stale delta.
    timerPeriod_ = TIMER_BASE_PERIOD;
    if (timerCsr_ & TIM_DIV16) timerPeriod_ *= 16;
    if (timerCsr_ & TIM_DIV4)  timerPeriod_ *= 4;
    timerStart_ = totalTicks_;
    busFrac_ = bk11() ? bus[1] % 12 : (bus[1] & 3) * 3; prevTeDram_ = bus[2]; prevTeFast_ = bus[3];
    {
        const uint64_t per = static_cast<uint64_t>(busPeriodTicks());
        busSkew_ = hasBus ? static_cast<int>((bus[0] + per - totalTicks_ % per) % per) : 0;
    }
    cpu_.clearHalt(); cpu_.clearWait();
    screen_.setScroll(scroll_);
    vp037_.setM256(scroll_ & 01000);   // фаза выровняется на следующем кадре
    return true;
}

void Board::saveState11(std::vector<uint8_t>& out) const {
    out.reserve(8 + Memory::RAM11_BYTES + 256);
    auto put = [&](const void* p, size_t n) {
        const uint8_t* b = static_cast<const uint8_t*>(p);
        out.insert(out.end(), b, b + n);
    };
    put(kMagic11, 8);
    put(mem_.ram11(), Memory::RAM11_BYTES);
    put(cpu_.r, 8 * sizeof(uint16_t));
    put(&cpu_.psw, sizeof(uint16_t));
    const uint16_t dev[7] = {scroll_, kbdStatus_, kbdData_, timerLimit_, timerCount_, timerCsr_, speaker_};
    put(dev, sizeof dev);
    const uint16_t ext[2] = {joystick_, static_cast<uint16_t>(keyHeld_ ? 1 : 0)};
    put(ext, sizeof ext);
    const uint32_t v037 = vp037_.pack();
    put(&v037, sizeof v037);
    const uint16_t m11[8] = {mem_.pageMap(), reg716tap_, reg662_, static_cast<uint16_t>(vpHalf_),
                             static_cast<uint16_t>(ayOn_ ? 1 : 0), ay_.address(),
                             static_cast<uint16_t>(mem_.mstd() ? 1 : 0), 0};
    put(m11, sizeof m11);
    put(ay_.regs(), 16);
    const uint16_t bus[4] = {static_cast<uint16_t>((totalTicks_ + busSkew_) % busPeriodTicks()),
                             static_cast<uint16_t>(busFrac_),
                             static_cast<uint16_t>(prevTeDram_),
                             static_cast<uint16_t>(prevTeFast_)};
    put(bus, sizeof bus);
    // Хвост СМК-512 (как у БК-0010): конфигурация всегда, ДОЗУ — если плата стоит.
    const uint16_t smk[4] = {static_cast<uint16_t>(smkOn_ ? 1 : 0),
                             static_cast<uint16_t>(smk_.mode()),
                             static_cast<uint16_t>(smk_.page()),
                             static_cast<uint16_t>(smk_.armed() ? 1 : 0)};
    put(smk, sizeof smk);
    if (smkOn_) put(smk_.ram().data(), Smk512::RAM_BYTES);
}

bool Board::loadState11(const std::vector<uint8_t>& in) {
    const size_t need = 8 + Memory::RAM11_BYTES + (8 + 1 + 7 + 2) * 2 + 4 + 8 * 2 + 16 + 4 * 2;
    if (in.size() < need) return false;
    if (!bk11()) setMachine(Machine::Bk0011m);   // снимок другой машины: ПЗУ и карта 11М
    size_t off = 8;
    auto get = [&](void* p, size_t n) { std::memcpy(p, in.data() + off, n); off += n; };
    get(mem_.ram11(), Memory::RAM11_BYTES);
    get(cpu_.r, 8 * sizeof(uint16_t));
    get(&cpu_.psw, sizeof(uint16_t));
    uint16_t dev[7], ext[2], m11[8], bus[4];
    uint32_t v037 = 0;
    uint8_t ayRegs[16];
    get(dev, sizeof dev); get(ext, sizeof ext); get(&v037, sizeof v037);
    get(m11, sizeof m11); get(ayRegs, sizeof ayRegs); get(bus, sizeof bus);
    uint16_t smk[4] = {0, 0, 0, 0};
    if (in.size() >= off + sizeof smk) get(smk, sizeof smk);   // снимки без хвоста — без платы
    setSmk512(smk[0] != 0);
    if (smkOn_) {
        smk_.setMode(static_cast<Smk512::Mode>(smk[1] & 7), smk[2]);
        if (smk[3]) smk_.writeCtrl(Smk512::STROBE);
        if (in.size() >= off + Smk512::RAM_BYTES) get(smk_.ram().data(), Smk512::RAM_BYTES);
    }
    vp037_.unpack(v037);
    scroll_ = dev[0]; kbdStatus_ = dev[1]; kbdData_ = dev[2];
    keyIntPending_ = false; keyIntDeferAt_ = 0;
    timerLimit_ = dev[3]; timerCount_ = dev[4];
    timerCsr_ = static_cast<uint8_t>(dev[5]);
    speaker_ = static_cast<uint8_t>(dev[6]);
    joystick_ = ext[0];
    keyHeld_  = ext[1] != 0;
    mem_.setMstd(m11[6] != 0);
    mem_.setPageMap(m11[0]);
    reg716tap_ = m11[1];
    reg662_ = m11[2];
    vpHalf_ = m11[3] & 1;
    apply662();
    setAy(m11[4] != 0);
    ay_.reset();
    for (int r = 0; r < 16; ++r) ay_.writeReg(static_cast<uint8_t>(r), ayRegs[r]);
    ay_.writeAddress(static_cast<uint8_t>(m11[5]));
    timerPeriod_ = TIMER_BASE_PERIOD;
    if (timerCsr_ & TIM_DIV16) timerPeriod_ *= 16;
    if (timerCsr_ & TIM_DIV4)  timerPeriod_ *= 4;
    timerStart_ = totalTicks_;
    busFrac_ = bus[1] % 12; prevTeDram_ = bus[2]; prevTeFast_ = bus[3];
    {
        const uint64_t per = static_cast<uint64_t>(busPeriodTicks());
        busSkew_ = static_cast<int>((bus[0] + per - totalTicks_ % per) % per);
    }
    cpu_.clearHalt(); cpu_.clearWait();
    screen_.setScroll(scroll_);
    vp037_.setM256(scroll_ & 01000);
    return true;
}

bool Board::saveState(const std::string& path) {
    std::vector<uint8_t> buf;
    saveStateMem(buf);
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
    const bool ok = std::fwrite(buf.data(), 1, buf.size(), f) == buf.size();
    std::fclose(f);
    return ok;
}

bool Board::loadState(const std::string& path) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    std::vector<uint8_t> buf;
    uint8_t chunk[65536];
    size_t got;
    while ((got = std::fread(chunk, 1, sizeof chunk, f)) > 0) buf.insert(buf.end(), chunk, chunk + got);
    std::fclose(f);
    return loadStateMem(buf);
}

int Board::runTicks(int ticks) {
    int done = 0;
    while (done < ticks) {
        if (cpu_.halted()) break;
        int t = stepCore();
        done += t;
        if (!breakpoints_.empty() && breakpoints_.count(cpu_.pc()) && breakAllows(cpu_.pc())) {
            breakHit_ = true;
            break;
        }
        if (breakHit_) break;   // data watchpoint fired inside the instruction
        // Простой по WAIT: до конца отрезка время идёт кусками без команд. Флаг
        // ожидания НЕ снимаем — его снимает Cpu::interrupt(), и только там PC
        // переставляется за команду WAIT. Если сбросить флаг здесь, пришедшее
        // следующим кадром прерывание вернётся по RTI на тот же WAIT, и программа
        // зависнет.
        while (cpu_.waiting() && done < ticks)
            done += idleTicks(std::min(ticks - done, 64));
    }
    screen_.setScroll(scroll_);
    return done;
}

bool Board::runUntil(uint16_t addr, int maxTicks) {
    int done = 0, frameTicks = 0;
    while (done < maxTicks) {
        if (cpu_.halted()) return false;
        // Прогон отладчика обязан продолжать выдавать кадровые прерывания 50 Гц:
        // подпрограмма, которая ждёт кадра (или стоит на WAIT), иначе не дождётся
        // никогда, и «шаг с обходом»/«до адреса» просто упрётся в лимит тактов.
        if (frameTicks == 0) { beginFrameRaster(); deliverFrameInterrupts(); }
        int t = stepCore();
        done += t; frameTicks += t;
        if (frameTicks >= ticksPerFrame()) { frameTicks = 0; trace_.tick(); ++framesSinceReset_; }
        if (cpu_.pc() == addr) { screen_.setScroll(scroll_); return true; }
        if (!breakpoints_.empty() && breakpoints_.count(cpu_.pc()) && breakAllows(cpu_.pc())) {
            breakHit_ = true; screen_.setScroll(scroll_); return true;
        }
        if (breakHit_) { screen_.setScroll(scroll_); return true; }  // watchpoint
    }
    screen_.setScroll(scroll_);
    return false;
}

bool Board::runUntilReturn(size_t targetDepth, int maxTicks) {
    int done = 0, frameTicks = 0;
    while (done < maxTicks) {
        if (cpu_.halted()) { screen_.setScroll(scroll_); return false; }
        if (frameTicks == 0) { beginFrameRaster(); deliverFrameInterrupts(); }  // 50 Hz IRQ
        int t = stepCore();
        done += t; frameTicks += t;
        if (frameTicks >= ticksPerFrame()) { frameTicks = 0; trace_.tick(); ++framesSinceReset_; }
        if (trace_.stackDepth() < targetDepth) { screen_.setScroll(scroll_); return true; }
        if (!breakpoints_.empty() && breakpoints_.count(cpu_.pc()) && breakAllows(cpu_.pc())) breakHit_ = true;
        if (breakHit_) { screen_.setScroll(scroll_); return true; }
    }
    screen_.setScroll(scroll_);
    return false;
}

void Board::runFrame() {
    beginFrameRaster();                     // верх кадра: доотрисовать прошлый, ресинк 037
    deliverFrameInterrupts();
    runTicks(ticksPerFrame());
    trace_.tick();
    ++framesSinceReset_;
}

void Board::runFrameSlice(int slice, int nslices) {
    if (nslices < 1) nslices = 1;
    if (slice <= 0) {
        beginFrameRaster();                     // верх кадра (VSYNC)
        deliverFrameInterrupts(); sliceIdle_ = false; sliceFrameTicks_ = 0;
    }
    if (!sliceIdle_) {
        // Run up to this slice's *cumulative* tick boundary, so per-slice overshoot
        // is compensated in the next slice and the frame totals exactly ticksPerFrame
        // — the whole frame then runs the identical instruction stream as runFrame().
        int target = static_cast<int>(static_cast<int64_t>(ticksPerFrame()) * (slice + 1) / nslices);
        int want = target - sliceFrameTicks_;
        if (want > 0) {
            int ran = runTicks(want);
            sliceFrameTicks_ += ran;
            // A short run means the CPU idled (WAIT/HALT) or a breakpoint stopped it;
            // it stays idle for the rest of the frame, so skip the later slices.
            if (ran < want) sliceIdle_ = true;
        }
    }
    if (slice >= nslices - 1) { trace_.tick(); ++framesSinceReset_; }
}

// Delivered once per 50 Hz frame. On BK-0010 the video controller raises IRQ2
// (vector 0100) every frame and the keyboard raises IRQ (vector 0060). Both are
// blocked when the processor priority bit (PSW bit 7, 0200) is set.
bool Board::pressKey(uint16_t bkCode) {
    // Real BK-0010 (контроллер ВП1-014, как в референсном bk/tty.c tty_keyevent):
    // новое нажатие ВСЕГДА перезаписывает регистр кода, даже если предыдущий код
    // не был прочитан — флаг готовности лишь отмечает «код поступал с последнего
    // чтения». Отбрасывание нового кода при занятом регистре — неверно: игры вроде
    // PITON читают 0177662 только пока клавиша физически нажата (бит 6 0177716), а
    // LAND — только по событию нажатия; обе получали бы код ПРЕДЫДУЩЕЙ клавиши.
    kbdData_ = bkCode & 0177;                 // 7-bit code -> 0177662
    kbdStatus_ |= 0200;                       // set "code ready"
    kbdReadyClearAt_ = 0;                     // новый код отменяет отложенный сброс флага
    // Latch the interrupt vector: function keys / АР2 (bit 0200 set) go through
    // 0274, ordinary keys through 060. Whether the interrupt is actually delivered
    // is decided in deliverFrameInterrupts by the status bit-6 mask and the CPU
    // priority; the code is always latched so polling software can read it.
    keyIntVec_ = (bkCode & 0200) ? 0274 : Cpu::VEC_KEYBOARD;
    keyIntPending_ = true;
    // Прерывание придерживаем на четверть кадра (см. keyIntDeferAt_ и разбор в
    // tryDeliverInterrupts): опрашивающая игра должна успеть прочитать код.
    keyIntDeferAt_ = totalTicks_ + static_cast<uint64_t>(ticksPerFrame() / 4);
    return true;
}

// Кадровый запрос 50 Гц ЗАЩЁЛКИВАЕТСЯ, а не выдаётся мгновенно: если в этот момент
// у процессора закрыта маска (программа в критической секции после MTPS #340),
// прерывание должно дождаться её открытия, а не пропасть. Раньше оно терялось
// целиком — отсюда рывки анимации и музыки. Реальный запрос разбирается в
// tryDeliverInterrupts() после каждой команды, как это делает GID (devemu/CPU.cpp:413).
void Board::deliverFrameInterrupts() {
    // БК-0011М: бит 14 регистра 0177662 — «таймер выключен» (0 = прерывание идёт;
    // БК-docs §20.4, проверено по ПЗУ БОС .BTIM).
    if (bk11() && (reg662_ & 040000)) return;
    irqFramePending_ = true;
    const int t = tryDeliverInterrupts();
    if (t) advanceDevices(t);
}

// Вход в прерывание: IRQ2 и СТОП — по фиксированному вектору, клавиатура — векторное
// с циклом IAKO. С моделью шины время считается по шаблону входа (Vm1Timing.h).
int Board::enterInterrupt(uint16_t vector, bool iako) {
    const uint16_t pc = static_cast<uint16_t>(cpu_.pc() + (cpu_.waiting() ? 2 : 0));
    nAcc_ = 0;
    const int t = cpu_.interrupt(vector);
    return arb037_ ? interruptTicks(iako, pc, 0, totalTicks_, t) : t;
}

// Возвращает стоимость входа в прерывание в тактах (0, если ничего не выдано).
int Board::tryDeliverInterrupts() {
    if (cpu_.halted()) return 0;
    // «СТОП» разбирается ДО проверки маски: это внеприоритетное прерывание, разряды
    // приоритета его не запрещают. Идёт по вектору 4 — тому же, что HALT и зависание.
    if (stopPending_ || busErrorPending_) {
        stopPending_ = busErrorPending_ = false;
        if (mem_.peekWord(Cpu::VEC_BUS_ERROR) != 0) {
            const int t = enterInterrupt(Cpu::VEC_BUS_ERROR, false);
            trace_.profileInterrupt(cpu_.pc(), cpu_.sp());
            return t;
        }
        // Вектор не установлен — прерывать некуда, запрос просто гасим.
    }
    if (cpu_.psw & 0200) return 0; // маска закрыта — запросы остаются взведёнными

    // Защёлкнутый код клавиатуры выдаёт своё прерывание раньше кадрового — но не
    // мгновенно и не при любых условиях.
    //
    // Разряд 6 регистра 0177660 — это МАСКА: программы, опрашивающие регистр сами
    // (Digger), её ставят, и прерывать тогда нельзя — код остаётся защёлкнутым для
    // опроса. Труднее случай, когда игра прерывание РАЗРЕШАЕТ и всё равно опрашивает
    // (SOKOBAN: CLR 177660, затем TST 177660 / CMPB 177662): выдай мы IRQ сразу,
    // ISR монитора забрал бы код первым и опрос вечно видел бы «нет клавиши».
    // Поэтому свежий код придерживается до keyIntDeferAt_ — примерно четверть кадра,
    // как в референсном bk, где прерывание клавиатуры планируется отложенным
    // событием. Успели прочитать за это время — запрос снимается без прерывания.
    if (keyIntPending_) {
        if (!(kbdStatus_ & 0200)) {
            keyIntPending_ = false;                  // код уже прочитан — прерывать незачем
        } else if (totalTicks_ >= keyIntDeferAt_) {
            keyIntPending_ = false;
            if (!(kbdStatus_ & 0100) && mem_.peekWord(keyIntVec_) != 0) {
                const int t = enterInterrupt(keyIntVec_, true);
                trace_.profileInterrupt(cpu_.pc(), cpu_.sp());  // ISR frame for the flame graph
                return t;
            }
        }
    }

    if (irqFramePending_ && mem_.peekWord(Cpu::VEC_IRQ2) != 0) {
        irqFramePending_ = false;
        const int t = enterInterrupt(Cpu::VEC_IRQ2, false);   // 0100 (50 Hz)
        trace_.profileInterrupt(cpu_.pc(), cpu_.sp());
        return t;
    }
    return 0;
}

int Board::stepInstruction() {
    int t = stepCore();
    screen_.setScroll(scroll_);
    return t;
}

bool Board::loadBin(const std::string& path, bool run, uint16_t* outAddr, uint16_t* outLen) {
    if (bk11()) {
        std::fprintf(stderr, "loadBin: загрузка .BIN на БК-0011М пока не поддерживается\n");
        return false;
    }
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) { std::fprintf(stderr, "loadBin: cannot open '%s'\n", path.c_str()); return false; }
    uint8_t hdr[4];
    if (std::fread(hdr, 1, 4, f) != 4) { std::fclose(f); return false; }
    uint16_t addr = hdr[0] | (hdr[1] << 8);
    uint16_t len  = hdr[2] | (hdr[3] << 8);
    std::vector<uint8_t> data(len);
    size_t got = std::fread(data.data(), 1, len, f);
    std::fclose(f);
    if (got == 0) return false;
    for (size_t i = 0; i < got; ++i)
        mem_.pokeByte(static_cast<uint16_t>(addr + i), data[i]);
    if (outAddr) *outAddr = addr;
    if (outLen)  *outLen  = static_cast<uint16_t>(got);
    if (run) {
        cpu_.clearHalt();
        cpu_.clearWait();
        // Обычно точка входа = адрес загрузки. Но есть игры с автозапуском через
        // затирание стека: монитор держит SP=01000, а его файловый EMT 36 расходует
        // стек в области ниже 01000 (trap кладёт PSW+PC, дальше вложенные JSR). Такая
        // игра грузится в 0760 и ЗАЛИВАЕТ весь этот «доадресный» слот [addr,01000)
        // копиями своего стартового адреса — тогда финальный RTS/RTI монитора снимает
        // со стека не адрес возврата, а этот адрес, и «возврат» уходит прямо в игру.
        // Формат .BIN хранить стартовый адрес отдельно не умеет, а мы грузим прямым
        // poke (без EMT 36), поэтому берём его сами. Признак приёма — именно ЗАЛИВКА:
        // все слова [addr,01000) одинаковы и указывают внутрь образа. Иначе (обычная
        // программа ниже 01000, напр. BOLDER грузится в 0606 и там реальный код —
        // MOV #1000,SP) точка входа = адрес загрузки. Примеры заливки: VALLEY (→01000),
        // БАТИСКАФ (→036576), BIL (→01000).
        // Признак заливки — не «весь слот одинаков» (у BIL последние слова слота иные),
        // а ДЛИННЫЙ ПРОГОН одинаковых слов с начала: одиночное слово-код (BOLDER) от
        // повторяющегося адреса-заливки (BIL: 001000 ×61, VALLEY/БАТИСКАФ ×8) так и
        // отличаем.
        uint16_t start = addr;
        if (addr < 01000) {
            const uint16_t w = mem_.peekWord(addr);
            const uint32_t end = static_cast<uint32_t>(addr) + got;
            int run = 0;                          // длина прогона одинаковых слов с addr
            for (uint16_t a = addr; a < 01000 && mem_.peekWord(a) == w; a += 2) ++run;
            if (w >= addr && w < end && run >= 4) // адрес-заливка внутрь образа
                start = w;                        // автозапуск через затирание стека
        }
        cpu_.r[7] = start; // точка входа
        // Стек — как у пускового монитора (100260: MOV #1000,SP), откуда программу и
        // запускают. Иначе SP остаётся от того, где стоял монитор или Бейсик в момент
        // загрузки (Бейсик в ожидании держит SP=01762), и программа, не ставящая стек
        // сама, затирает свой код (45com-lo).
        cpu_.r[6] = 01000;
    }
    return true;
}

// ---- EMT 36 (tape/disk driver) interception --------------------------------
// Serves the monitor ROM's file-I/O EMT from the host filesystem (CWD) instead
// of tape/disk. The parameter-block address is in R1; layout (bytes from base):
//   +0  COMMAND   0=STOP 1=START(motor) 2=WRITE 3=READ 4=FICT_READ
//   +1  RESPONSE  0=OK 1=INCORRECT_NAME 2=CRC_ERROR 3=STOP  (written back)
//   +2  DATA_PTR  load/save address (word)
//   +4  SIZE      byte count (word)
//   +6  NAME[16]  file name (space-padded)
//   +026 CUR_DATA_PTR / +030 CUR_SIZE / +032 CUR_NAME[16]  (found file, on read)
// Host files use the standard .BIN/tape layout: [addr_lo,addr_hi,len_lo,len_hi,data].
// BK tape names carry no extension, but the same single-file data is commonly
// stored on the host as "<name>.bin". Open the bare name first, then fall back to
// a ".bin"/".BIN" suffix so extracted game files load without renaming. (Whole-disk
// images like ".bkd" are NOT tried — they are container images, not tape files.)
static bool iequals(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (std::tolower(static_cast<unsigned char>(a[i])) !=
            std::tolower(static_cast<unsigned char>(b[i]))) return false;
    return true;
}

static FILE* openTapeRead(const std::string& name) {
    // Fast path: the name exactly as given, plus a common host ".bin"/".BIN" suffix.
    for (const char* ext : {"", ".bin", ".BIN"})
        if (FILE* f = std::fopen((name + ext).c_str(), "rb")) return f;
    // BK хранит имена в верхнем регистре (КОИ-7), а извлечённые на хост файлы часто
    // в другом регистре (игра просит "HARD.OVL", а на диске "Hard.ovl"). Linux
    // регистрозависим — поэтому ищем в текущем каталоге совпадение имени без учёта
    // регистра (для многосерийных загрузчиков с оверлеями .ovl и т.п.).
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(".", ec)) {
        if (ec) break;
        if (!entry.is_regular_file(ec)) continue;
        const std::string fn = entry.path().filename().string();
        if (iequals(fn, name) || iequals(fn, name + ".bin"))
            if (FILE* f = std::fopen(entry.path().string().c_str(), "rb")) return f;
    }
    return nullptr;
}

bool Board::handleEmt36() {
    enum : uint8_t { CMD_STOP = 0, CMD_START = 1, CMD_WRITE = 2, CMD_READ = 3, CMD_FICT = 4 };
    enum : uint8_t { RSP_OK = 0, RSP_BADNAME = 1, RSP_CRC = 2, RSP_STOP = 3 };

    // Вставлена дискета — файловая система у машины СВОЯ, и EMT 36 обслуживает
    // ДОС с диска (ANDOS ставит свой обработчик и отвечает через байт 0301).
    // Перехватывать вызов в этом случае нельзя: подмена «файл не найден» из
    // рабочего каталога хоста доходит до программы как успешный, но пустой
    // ответ. Так ломался onslaught.bkd: игра просит ANDOS прочитать ONSLAUG.SCR
    // по адресу 05000, затем расшифровывает загруженное (XOR ключом из вектора
    // 034) и передаёт туда управление — на нерасшифрованном мусоре ЦП улетал в
    // ПЗУ драйвера НГМД и вис в ожидании флага TR.
    if (diskOn_) return false;
    // У БОС БК-0011М своя схема EMT (ленточный драйвер — по .BLOAD), формат
    // блока параметров монитора БК-0010 к ней не относится.
    if (bk11()) return false;

    const uint16_t base = cpu_.r[1];
    const uint8_t  cmd  = mem_.peekByte(base);

    // Extract the 16-char name, trimming trailing spaces / NULs.
    char raw[17] = {0};
    for (int i = 0; i < 16; ++i) raw[i] = static_cast<char>(mem_.peekByte(base + 6 + i));
    int n = 16;
    while (n > 0 && (raw[n - 1] == ' ' || raw[n - 1] == '\0')) --n;
    std::string name(raw, raw + n);

    auto respond = [&](uint8_t code) {
        mem_.pokeByte(base + 1, code);
        if (cmd >= CMD_WRITE) {   // log actual file operations (not motor STOP/START)
            uint16_t la = mem_.peekWord(base + 2), ll = mem_.peekWord(base + 4);
            if (cmd != CMD_WRITE && code == RSP_OK) { la = mem_.peekWord(base + 026); ll = mem_.peekWord(base + 030); }
            emtLog_.push_back({cmd, code, name, la, ll, totalTicks_});
            if (emtLog_.size() > 256) emtLog_.pop_front();
        }
        return true;
    };

    switch (cmd) {
    case CMD_STOP:
    case CMD_START:
        // Motor control has no meaning without tape — just acknowledge.
        return respond(RSP_OK);

    case CMD_WRITE: {
        // No name, or a name that would escape the working directory: refuse.
        if (name.empty() || name.find('/') != std::string::npos ||
            name.find('\\') != std::string::npos)
            return respond(RSP_BADNAME);
        const uint16_t addr = mem_.peekWord(base + 2);
        const uint16_t len  = mem_.peekWord(base + 4);
        FILE* f = std::fopen(name.c_str(), "wb");
        if (!f) return respond(RSP_CRC);
        uint8_t hdr[4] = { static_cast<uint8_t>(addr & 0377),
                           static_cast<uint8_t>(addr >> 8),
                           static_cast<uint8_t>(len & 0377),
                           static_cast<uint8_t>(len >> 8) };
        bool ok = std::fwrite(hdr, 1, 4, f) == 4;
        for (uint16_t i = 0; ok && i < len; ++i) {
            uint8_t b = mem_.peekByte(static_cast<uint16_t>(addr + i));
            ok = std::fputc(b, f) != EOF;
        }
        std::fclose(f);
        return respond(ok ? RSP_OK : RSP_CRC);
    }

    case CMD_READ:
    case CMD_FICT: {
        if (name.empty() || name.find('/') != std::string::npos ||
            name.find('\\') != std::string::npos)
            return respond(RSP_BADNAME);
        FILE* f = openTapeRead(name);   // tolerates a ".bin"/".BIN" host extension
        if (!f) return respond(RSP_BADNAME);
        uint8_t hdr[4];
        if (std::fread(hdr, 1, 4, f) != 4) { std::fclose(f); return respond(RSP_CRC); }
        const uint16_t fileAddr = hdr[0] | (hdr[1] << 8);
        const uint16_t fileLen  = hdr[2] | (hdr[3] << 8);
        // Report the found ("current") file in the block.
        mem_.pokeWord(base + 026, fileAddr);
        mem_.pokeWord(base + 030, fileLen);
        for (int i = 0; i < 16; ++i)
            mem_.pokeByte(base + 032 + i, i < n ? static_cast<uint8_t>(name[i]) : ' ');
        if (cmd == CMD_READ) {
            // Load address: honour a caller-supplied DATA_PTR, else the file's own.
            const uint16_t caller = mem_.peekWord(base + 2);
            const uint16_t load = caller ? caller : fileAddr;
            std::vector<uint8_t> data(fileLen);
            size_t got = fileLen ? std::fread(data.data(), 1, fileLen, f) : 0;
            for (size_t i = 0; i < got; ++i)
                mem_.pokeByte(static_cast<uint16_t>(load + i), data[i]);
        }
        std::fclose(f);
        return respond(RSP_OK);
    }

    default:
        // Unknown command: let the ROM handler run (safe fallback).
        return false;
    }
}

// ---- I/O register dispatch --------------------------------------------------
bool Board::ioRead(uint16_t addr, uint16_t& value) {
    const bool handled = ioReadRaw(addr, value);
    if (handled && ioLogOn_ && ioLogReads_ && addr >= ADDR_IO_PAGE &&
        (!ioLogFilter_ || addr == ioLogFilter_)) {
        ioLog_.push_back({addr, value, curInstrPc_, totalTicks_, true,
                          static_cast<int16_t>(vp037_.scanline()), static_cast<uint8_t>(vp037_.lineWord())});
        while (ioLog_.size() > ioLogCap_) ioLog_.pop_front();
    }
    return handled;
}

bool Board::ioReadRaw(uint16_t addr, uint16_t& value) {
    switch (addr) {
    case REG_KBD_STATUS: value = kbdStatus_; return true;
    case REG_KBD_DATA:
        // Флаг готовности (бит 7 регистра 0177660) гаснет НЕ мгновенно, а примерно
        // через 900 тактов ЦП после чтения кода — так ведёт себя ВП1-014
        // (GID: devemu/Board.cpp:1081-1084, отсчёт :1855-1857). Программа, которая
        // читает код и тут же перепроверяет готовность, на железе видит её ещё раз.
        value = kbdData_;
        if (!kbdReadyClearAt_) kbdReadyClearAt_ = totalTicks_ + KBD_READY_CLEAR_TICKS;
        return true;
    case REG_SCROLL:     value = scroll_; return true;
    case REG_TIMER_LIM:  value = timerLimit_; return true;
    case REG_TIMER_CNT:  timerCheck(); value = timerCount_; return true;
    case REG_TIMER_CSR:  timerCheck(); value = 0177400 | timerCsr_; return true;
    // Внутренние регистры периферийного блока самого ЦП (04-процессор, §4.3).
    // В БК не используются, но физически существуют, и читать их программа может.
    case REG_CPU_MODE:   value = 0177740 | (cpuMode_ & 7); return true;
    case REG_CPU_IVEC:   value = 0177777; return true;     // только запись; читается как все единицы
    case REG_CPU_ERROR:  value = 0177440; return true;     // флаги сбрасываются на выборке каждой команды
    case REG_PORT:       value = joystick_; return true;   // джойстик на парал. порту
    case REG_SYS: {
        if (bk11()) {
            // БК-0011М (БК-docs §20.3): старший байт — адрес пуска 0140000, бит 7 всегда 1,
            // бит 6 — «нет нажатой клавиши», бит 5 — вход с ленты (у нас 0), бит 2 —
            // «была запись», гасится чтением.
            uint16_t v = START_VECTOR11 | 0200;
            if (!keyHeld_) v |= 0100;
            if (sysWriteFlag_) { v |= 004; sysWriteFlag_ = false; }
            value = v;
            return true;
        }
        // BK-0010: bit15 set, high byte 0200 (serial idle). Bit 6 (0100) is the
        // "key pressed" indicator, active-low: 0 while a key is physically held.
        // This tracks the key press/release directly (like the reference bk's
        // key_pressed), NOT the code-ready flag — so a game polling this bit keeps
        // seeing the key even after the monitor's ISR has drained the code register.
        uint16_t v = 0100000 | 0200;
        if (!keyHeld_) v |= 0100; // no key held -> bit 6 high
        // Бит 2 (004) — «была запись в системный регистр»: взводится любой записью,
        // гасится чтением (внешняя схема ВР1, см. GID devemu/Board.cpp:1264-1268).
        if (sysWriteFlag_) { v |= 004; sysWriteFlag_ = false; }
        value = v;
        return true;
    }
    default:
        // Any other address in the I/O page: read as 0 (handled).
        if (addr >= ADDR_IO_PAGE) { value = 0; return true; }
        return false;
    }
}

// Уровень пьезодинамика из бит 6, 5 и 2 (маска 0144): он слышит сумму трёх бит,
// бит 6 — основной, 5 и 2 подмешиваются в тот же аналоговый узел. Складываем их в
// уровень 0..7 так же, как GID (devemu/Speaker.cpp:132-139): бит 2 переносится в
// позицию 4, потом >>4.
void Board::setSpeakerBits(uint16_t value) {
    const uint8_t was = speaker_;
    uint16_t w = value & 0144;
    if (w & 004) w |= 020;
    speaker_ = static_cast<uint8_t>((w >> 4) & 7);
    if (spkLogOn_ && was != speaker_) {          // смена уровня динамика
        spkLog_.push_back({totalTicks_, speaker_});
        while (spkLog_.size() > spkLogCap_) spkLog_.pop_front();
    }
}

void Board::apply662() {
    screen_.setPalette((reg662_ >> 8) & 15);
    mem_.setVideoPage((reg662_ & 0100000) ? 6 : 5);
}

std::string Board::bk11Summary() const {
    char b[160];
    const int rom = mem_.window1Rom();
    char w1[24];
    if (mem_.window1Off()) std::snprintf(w1, sizeof w1, "откл(СМК)");
    else if (rom >= 0) std::snprintf(w1, sizeof w1, "ПЗУ%d", rom);
    else std::snprintf(w1, sizeof w1, "стр%d", mem_.window1Page());
    std::snprintf(b, sizeof b, "окно0=стр%d окно1=%s пал=%d экран=стр%d таймер=%s СТОП=%s",
                  mem_.window0Page(), w1, (reg662_ >> 8) & 15, mem_.videoPage(),
                  (reg662_ & 040000) ? "выкл" : "вкл", (reg716tap_ & 010000) ? "блок" : "да");
    return b;
}

std::string Board::aySummary() const {
    const uint8_t* r = ay_.regs();
    auto tone = [&](int c) { return r[c * 2] | ((r[c * 2 + 1] & 017) << 8); };
    auto vol = [&](int c) {
        char v[8];
        if (r[8 + c] & 020) std::snprintf(v, sizeof v, "E"); else std::snprintf(v, sizeof v, "%d", r[8 + c] & 017);
        return std::string(v);
    };
    char b[200];
    std::snprintf(b, sizeof b, "рег=%d  тон A/B/C=%d/%d/%d  громк=%s/%s/%s  смес=%03o  шум=%d  огиб=%d форма=%d",
                  ay_.address(), tone(0), tone(1), tone(2), vol(0).c_str(), vol(1).c_str(), vol(2).c_str(),
                  r[7], r[6] & 037, r[11] | (r[12] << 8), r[13] & 017);
    return b;
}

// addrIn при байтовой записи не выровнен: на БК-0011М нечётный байт идёт в старший
// (как у GID: `src &= 0377; if (addr & 1) src <<= 8`). Поведение БК-0010 прежнее —
// байт как младший.
bool Board::ioWrite(uint16_t addrIn, uint16_t value, bool isByte) {
    const uint16_t addr = static_cast<uint16_t>(addrIn & ~1);
    uint16_t wide = value;                     // значение, выровненное по байту, для 11М
    if (isByte) { wide &= 0377; if (addrIn & 1) wide = static_cast<uint16_t>(wide << 8); }
    if (addr >= ADDR_IO_PAGE) ioLastWrite_[(addr - ADDR_IO_PAGE) >> 1] = value; // for the debugger
    if (ioLogOn_ && addr >= ADDR_IO_PAGE && (!ioLogFilter_ || addr == ioLogFilter_)) {
        ioLog_.push_back({addr, value, curInstrPc_, totalTicks_, false,
                          static_cast<int16_t>(vp037_.scanline()), static_cast<uint8_t>(vp037_.lineWord())});
        while (ioLog_.size() > ioLogCap_) ioLog_.pop_front();
    }
    switch (addr) {
    // Скролл: биты 0-7 = строка, бит 9 (01000) = полный/малый экран. Бит 8 (0400)
    // аппаратно НЕ реализован — маскируем его (как bk/tty.c: & 01377). Иначе перенос
    // из младшего байта при ADD/INC регистра (прокрутка) прополз бы в бит 9 и ложно
    // включал бы малый экран (баг в RUNING.BIN при движении вниз по лабиринту).
    case REG_SCROLL:
        flushRaster();
        scroll_ = value & 01377; vp037_.setM256(scroll_ & 01000); return true;
    case REG_TIMER_LIM: timerLimit_ = value; return true;
    case REG_TIMER_CNT: return true;                       // counter is read-only
    case REG_TIMER_CSR: timerSetMode(value & 0377); return true;
    case REG_SYS: {
        sysWriteFlag_ = true;   // бит 2 на чтение: «была запись в системный регистр»
        if (bk11()) {
            // Два регистра по одному адресу, разводит бит 11 (БК-docs §20.3, GID
            // Board_11M.cpp:97-109): 1 — карта памяти, 0 — лента, динамик, блокировка СТОП.
            if (wide & 04000) {
                mem_.setPageMap(static_cast<uint16_t>((mem_.pageMap() & ~077433) | (wide & 077433)));
            } else {
                reg716tap_ = static_cast<uint16_t>((reg716tap_ & ~010344) | (wide & 010344));
                setSpeakerBits(reg716tap_);
            }
            return true;
        }
        setSpeakerBits(value);
        return true;
    }
    case REG_CPU_MODE:
        cpuMode_ = value & 7;   // доступны по записи только разряды 0..2 (WT/ST)
        return true;
    case REG_CPU_IVEC:  return true;   // запись сохраняется в ЦП, программе не видна
    case REG_CPU_ERROR: return true;   // писать бессмысленно: биты гаснут на выборке команды
    case REG_PORT:
        // Параллельный порт — ДВА раздельных регистра: чтение отдаёт джойстик
        // (входные линии), запись защёлкивается отдельно (принтер/ковокс/AY).
        // Раньше запись просто проглатывалась, и отладчику её видно не было.
        portOut_ = value;
        if (ayOn_) {
            // AY-3-8910 на порту (GID devemu/Board.cpp:1184-1193, AYSnd.cpp): запись
            // словом — номер регистра, байтом — данные; линии инверсные. Байт по
            // нечётному адресу приходит в старший байт, младший тогда 0 → данные 0377.
            if (isByte) ay_.writeData(static_cast<uint8_t>(~wide & 0377));
            else        ay_.writeAddress(static_cast<uint8_t>(~value & 017));   // один чип: как GID, номер — младшая тетрада
        }
        return true;
    case REG_KBD_STATUS: kbdStatus_ = (kbdStatus_ & ~0100) | (value & 0100); return true;
    case REG_KBD_DATA:
        if (bk11()) {
            // БК-0011М: регистр палитры, экранного буфера и кадрового таймера
            // (маска 0147400, GID Board_11M.cpp:191-222). Текущую строку растра
            // сначала дорисовываем старой палитрой — смена видна с места луча.
            flushRaster();
            reg662_ = static_cast<uint16_t>((reg662_ & ~0147400) | (wide & 0147400));
            apply662();
            return true;
        }
        // Регистр данных клавиатуры на БК-0010 доступен ТОЛЬКО по чтению. Запись
        // не обрабатываем — она проваливается дальше по шине, и если за адресом
        // нет записываемой памяти (обычно там ПЗУ), выходит «зависание» по вектору
        // 4. Ровно так прошивка контроллера СМК отличает БК-0010 от БК-0011М, где
        // по этому адресу лежит регистр управления экраном и запись проходит: без
        // этого контроллер считает машину одиннадцатой, ставит режим Std11, и ни
        // одна система с диска не поднимается.
        return false;
    default:
        if (addr >= ADDR_IO_PAGE) return true; // swallow writes to I/O page
        return false;
    }
}

uint16_t Board::peekReg(uint16_t addr) const {
    switch (addr) {
    case REG_KBD_STATUS: return kbdStatus_;
    case REG_KBD_DATA:   return kbdData_;
    case REG_SCROLL:     return scroll_;
    case REG_TIMER_LIM:  return timerLimit_;
    case REG_TIMER_CNT:  return timerCount_;                     // as of the last read
    case REG_TIMER_CSR:  return static_cast<uint16_t>(0177400 | timerCsr_);
    case REG_CPU_MODE:   return static_cast<uint16_t>(0177740 | (cpuMode_ & 7));
    case REG_CPU_IVEC:   return 0177777;
    case REG_CPU_ERROR:  return 0177440;
    case REG_PORT:       return joystick_;                       // джойстик на парал. порту
    case REG_SYS:        return static_cast<uint16_t>((bk11() ? START_VECTOR11 : 0100000) | 0200
                                                      | (keyHeld_ ? 0 : 0100)
                                                      | (sysWriteFlag_ ? 004 : 0)); // без побочки: флаг не гасим
    case 0176560:        return 0;   // ИРПС (последовательный порт) — не эмулируется
    default:             return mem_.peekWord(addr);
    }
}

uint16_t Board::peekRegWritten(uint16_t addr) const {
    return (addr >= ADDR_IO_PAGE) ? ioLastWrite_[(addr - ADDR_IO_PAGE) >> 1] : 0;
}

} // namespace bk
