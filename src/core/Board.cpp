#include "Board.h"
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
        timerCount_ = 0;
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

// Верх кадра: доотрисовать хвост предыдущего кадра и начать новый с нулевой строки,
// затем выровнять фазу 037 к VSYNC.
void Board::beginFrameRaster() {
    if (scanlineRender_) { renderScanlinesUpTo(Screen::TEX_H); renderedLine_ = 0; }
    if (arb037_ || scanlineRender_) vp037_.syncToFrameTop();
}

// Догнать построчную отрисовку до строки `line` (строки ДО неё луч уже прошёл).
void Board::renderScanlinesUpTo(int line) {
    if (line > Screen::TEX_H) line = Screen::TEX_H;
    while (renderedLine_ < line) {
        screen_.renderLine(mem_, renderedLine_, scroll_);
        ++renderedLine_;
    }
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
}

Board::Board() {
    mem_.setIoBus(this);
    cpu_.setResetHook([this]() { resetDevices(); });
    // Feed the access heatmap (no-op unless the trace is enabled) and the
    // data watchpoints (no-op unless any are set).
    mem_.setAccessHook([this](uint16_t a, bool w, bool b) {
        trace_.access(a, w, b);
        if (!watchpoints_.empty()) checkWatch(a, w, b);
        // Арбитраж 037: обращения в ДОЗУ (A15=0, т.е. addr < 0100000 — оба банка
        // ОЗУ) во время активной развёртки получают такты ожидания. ПЗУ/В-В (A15=1)
        // 037 не арбитрирует. Фаза 037 берётся на начало инструкции — все обращения
        // одной инструкции видят одну фазу (табличный тайминг ЦП не даёт точный
        // внутриинструкционный график обращений; сама модель 037 потактовая).
        if (arb037_ && a < 0100000) pendingWaitClkin_ += vp037_.stallForAccess();
    });
    // Intercept EMT 36 (tape/disk file I/O) and serve it from the host CWD.
    cpu_.setEmt36Hook([this]() { return handleEmt36(); });
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
    pendingWaitClkin_ = 0;
    int t = cpu_.step();
    if (watchArmed_) watchPc_ = pcBefore;   // the instruction that triggered a watch
    if (arb037_) {
        // Свести накопленные ожидания ДОЗУ (CLKIN → такты ЦП, ÷2) в стоимость
        // инструкции и продвинуть фазу 037 в лок-степе (1 такт ЦП = 2 такта CLKIN).
        t += pendingWaitClkin_ / 2;
        vp037_.tick(2 * t);
    } else if (scanlineRender_) {
        vp037_.tick(2 * t);   // фаза нужна и без арбитража — по ней рисуются строки
    }
    // Луч ушёл вперёд — дорисовать пройденные строки текущим значением скролла.
    if (scanlineRender_) renderScanlinesUpTo(vp037_.scanline());
    totalTicks_ += static_cast<uint64_t>(t);
    sound_.feed(speaker_, t);
    // Отложенный сброс флага готовности клавиатуры (см. ioRead REG_KBD_DATA).
    if (kbdReadyClearAt_ && totalTicks_ >= kbdReadyClearAt_) {
        kbdStatus_ &= ~0200;
        kbdReadyClearAt_ = 0;
    }
    // Взведённые запросы разбираются после КАЖДОЙ команды, а не раз в кадр: иначе
    // программа, открывшая маску в середине кадра, ждала бы следующего кадра.
    if (irqFramePending_ || keyIntPending_ || stopPending_) {
        const int it = tryDeliverInterrupts();
        if (it) { totalTicks_ += static_cast<uint64_t>(it); sound_.feed(speaker_, it); t += it; }
    }
    if (trace_.enabled()) {
        uint16_t pcNow = cpu_.pc();
        int16_t delta = static_cast<int16_t>(pcNow - pcBefore);
        if (delta < 0 || delta > 6) trace_.edge(pcBefore, pcNow); // non-sequential = control flow
        trace_.profileStep(mem_.peekWord(pcBefore), pcNow, cpu_.sp(), t); // flame-graph CCT
    }
    return t;
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

bool Board::loadRoms(const std::string& romDir) {
    bool ok = true;
    ok &= mem_.loadRomFile(ADDR_ROM_MON, (romDir + "/monit10.rom").c_str(), 8192);
    // BASIC is optional for running games but harmless; load if present.
    mem_.loadRomFile(ADDR_ROM_BASIC, (romDir + "/basic10.rom").c_str(), 24576);
    return ok;
}

void Board::reset() {
    mem_.reset();
    // Power-up: priority 7 (all interrupts masked). The monitor lowers the
    // priority itself once it has installed its interrupt vectors.
    cpu_.reset(START_VECTOR, 0340);
    scroll_ = 01330;   // 0330 + бит 9: полный экран (256 строк) по умолчанию
    kbdStatus_ = 0;    // bit 6 (0100) = interrupt MASK (0 => enabled), bit 7 = ready
    kbdData_ = 0;
    keyIntPending_ = false;
    irqFramePending_ = false;
    stopPending_ = false;
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
    vp037_.reset();
    vp037_.setM256(scroll_ & 01000);   // бит 9 — полный/малый экран
    pendingWaitClkin_ = 0;
    trace_.reset();
}

// ---- Save / restore state ---------------------------------------------------
namespace {
constexpr char kMagic[8] = {'B','K','1','0','S','T','1','\0'};
}

// Раскладка снимка: магия(8) + ОЗУ(0100000) + R0-R7(8 слов) + PSW + dev[7] и —
// с этой версии — хвост ext[2] = {joystick_, keyHeld_}. Магию не меняем: снимки
// старого формата просто короче на хвост и читаются как «ввод отпущен».
namespace {
constexpr size_t kStateBase = 8 + ADDR_RAM_END + (8 + 1 + 7) * sizeof(uint16_t);
constexpr size_t kStateFull = kStateBase + 2 * sizeof(uint16_t);          // + {joystick, keyHeld}
constexpr size_t kStateV037 = kStateFull + sizeof(uint32_t);              // + фаза развёртки 037
}

void Board::saveStateMem(std::vector<uint8_t>& out) const {
    out.clear();
    out.reserve(kStateFull);
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
}

bool Board::loadStateMem(const std::vector<uint8_t>& in) {
    if (in.size() < kStateBase || std::memcmp(in.data(), kMagic, 8) != 0) return false;
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
    scroll_ = dev[0]; kbdStatus_ = dev[1]; kbdData_ = dev[2];
    keyIntPending_ = false;
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
    cpu_.clearHalt(); cpu_.clearWait();
    screen_.setScroll(scroll_);
    vp037_.setM256(scroll_ & 01000);   // фаза выровняется на следующем кадре
    pendingWaitClkin_ = 0;
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
        // Простой по WAIT: остаток кадра пропускаем, но флаг ожидания НЕ снимаем —
        // его снимает Cpu::interrupt(), и только там PC переставляется за команду
        // WAIT. Если сбросить флаг здесь, пришедшее следующим кадром прерывание
        // вернётся по RTI на тот же WAIT, и программа зависнет.
        if (cpu_.waiting()) break;
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
    // PITON читают 0177662 только пока клавиша физически нажата (бит 6 0177716),
    // не сбрасывая флаг за каждое нажатие, и получали бы код ПРЕДЫДУЩЕЙ клавиши.
    kbdData_ = bkCode & 0177;                 // 7-bit code -> 0177662
    kbdStatus_ |= 0200;                       // set "code ready"
    kbdReadyClearAt_ = 0;                     // новый код отменяет отложенный сброс флага
    // Latch the interrupt vector: function keys / АР2 (bit 0200 set) go through
    // 0274, ordinary keys through 060. Whether the interrupt is actually delivered
    // is decided in deliverFrameInterrupts by the status bit-6 mask and the CPU
    // priority; the code is always latched so polling software can read it.
    keyIntVec_ = (bkCode & 0200) ? 0274 : Cpu::VEC_KEYBOARD;
    keyIntPending_ = true;
    return true;
}

// Кадровый запрос 50 Гц ЗАЩЁЛКИВАЕТСЯ, а не выдаётся мгновенно: если в этот момент
// у процессора закрыта маска (программа в критической секции после MTPS #340),
// прерывание должно дождаться её открытия, а не пропасть. Раньше оно терялось
// целиком — отсюда рывки анимации и музыки. Реальный запрос разбирается в
// tryDeliverInterrupts() после каждой команды, как это делает GID (devemu/CPU.cpp:413).
void Board::deliverFrameInterrupts() {
    irqFramePending_ = true;
    const int t = tryDeliverInterrupts();
    if (t) { totalTicks_ += static_cast<uint64_t>(t); sound_.feed(speaker_, t); }
}

// Возвращает стоимость входа в прерывание в тактах (0, если ничего не выдано).
int Board::tryDeliverInterrupts() {
    if (cpu_.halted()) return 0;
    // «СТОП» разбирается ДО проверки маски: это внеприоритетное прерывание, разряды
    // приоритета его не запрещают. Идёт по вектору 4 — тому же, что HALT и зависание.
    if (stopPending_) {
        stopPending_ = false;
        if (mem_.peekWord(Cpu::VEC_BUS_ERROR) != 0) {
            const int t = cpu_.interrupt(Cpu::VEC_BUS_ERROR);
            trace_.profileInterrupt(cpu_.pc(), cpu_.sp());
            return t;
        }
        // Вектор не установлен — прерывать некуда, запрос просто гасим.
    }
    if (cpu_.psw & 0200) return 0; // маска закрыта — запросы остаются взведёнными

    // A latched, not-yet-serviced key raises its interrupt first — but only while
    // the keyboard interrupt is enabled. On BK-0010 status bit 6 (0100) is the
    // interrupt MASK: software that polls the register (e.g. Digger) sets it to
    // stop the monitor's ISR from stealing the code. When masked we leave the
    // code latched (ready bit stays set) for polling and fall through to the
    // frame IRQ. Matches the reference bk emulator (tty.c: raise when !TTY_IE).
    if (keyIntPending_) {
        keyIntPending_ = false;
        if (!(kbdStatus_ & 0100) && mem_.peekWord(keyIntVec_) != 0) {
            const int t = cpu_.interrupt(keyIntVec_);
            trace_.profileInterrupt(cpu_.pc(), cpu_.sp());  // ISR frame for the flame graph
            return t;
        }
    }

    if (irqFramePending_ && mem_.peekWord(Cpu::VEC_IRQ2) != 0) {
        irqFramePending_ = false;
        const int t = cpu_.interrupt(Cpu::VEC_IRQ2);   // 0100 (50 Hz)
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
        ioLog_.push_back({addr, value, curInstrPc_, totalTicks_, true});
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

bool Board::ioWrite(uint16_t addr, uint16_t value, bool /*isByte*/) {
    if (addr >= ADDR_IO_PAGE) ioLastWrite_[(addr - ADDR_IO_PAGE) >> 1] = value; // for the debugger
    if (ioLogOn_ && addr >= ADDR_IO_PAGE && (!ioLogFilter_ || addr == ioLogFilter_)) {
        ioLog_.push_back({addr, value, curInstrPc_, totalTicks_, false});
        while (ioLog_.size() > ioLogCap_) ioLog_.pop_front();
    }
    switch (addr) {
    // Скролл: биты 0-7 = строка, бит 9 (01000) = полный/малый экран. Бит 8 (0400)
    // аппаратно НЕ реализован — маскируем его (как bk/tty.c: & 01377). Иначе перенос
    // из младшего байта при ADD/INC регистра (прокрутка) прополз бы в бит 9 и ложно
    // включал бы малый экран (баг в RUNING.BIN при движении вниз по лабиринту).
    case REG_SCROLL:    scroll_ = value & 01377; vp037_.setM256(scroll_ & 01000); return true;
    case REG_TIMER_LIM: timerLimit_ = value; return true;
    case REG_TIMER_CNT: return true;                       // counter is read-only
    case REG_TIMER_CSR: timerSetMode(value & 0377); return true;
    case REG_SYS: {
        sysWriteFlag_ = true;   // бит 2 на чтение: «была запись в системный регистр»
        const uint8_t was = speaker_;
        // Динамик слышит сумму трёх бит (маска 0144): бит 6 — основной, биты 5 и 2
        // подмешиваются в тот же аналоговый узел. Складываем их в уровень 0..7 так же,
        // как GID (devemu/Speaker.cpp:132-139): бит 2 переносится в позицию 4, потом >>4.
        uint16_t w = value & 0144;
        if (w & 004) w |= 020;
        speaker_ = static_cast<uint8_t>((w >> 4) & 7);
        if (spkLogOn_ && was != speaker_) {          // смена уровня динамика
            spkLog_.push_back({totalTicks_, speaker_});
            while (spkLog_.size() > spkLogCap_) spkLog_.pop_front();
        }
        return true;
    }
    case REG_CPU_MODE:
        cpuMode_ = value & 7;   // доступны по записи только разряды 0..2 (WT/ST)
        return true;
    case REG_CPU_IVEC:  return true;   // запись сохраняется в ЦП, программе не видна
    case REG_CPU_ERROR: return true;   // писать бессмысленно: биты гаснут на выборке команды
    case REG_PORT:
        // Параллельный порт — ДВА раздельных регистра: чтение отдаёт джойстик
        // (входные линии), запись защёлкивается отдельно (принтер/ковокс).
        // Раньше запись просто проглатывалась, и отладчику её видно не было.
        portOut_ = value;
        return true;
    case REG_KBD_STATUS: kbdStatus_ = (kbdStatus_ & ~0100) | (value & 0100); return true;
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
    case REG_SYS:        return static_cast<uint16_t>(0100000 | 0200 | (keyHeld_ ? 0 : 0100)
                                                      | (sysWriteFlag_ ? 004 : 0)); // без побочки: флаг не гасим
    case 0176560:        return 0;   // ИРПС (последовательный порт) — не эмулируется
    default:             return mem_.peekWord(addr);
    }
}

uint16_t Board::peekRegWritten(uint16_t addr) const {
    return (addr >= ADDR_IO_PAGE) ? ioLastWrite_[(addr - ADDR_IO_PAGE) >> 1] : 0;
}

} // namespace bk
