// Minimal unit tests for the BK-0010 CPU core.
// Loads short instruction sequences into RAM and checks register/flag results.
#include "Cpu.h"
#include "Memory.h"
#include "Disasm.h"
#include "Board.h"
#include "Vp037.h"
#include "Joystick.h"
#include "BkKeys.h"
#include "ScreenOcr.h"
#include "Speaker.h"
#include "Fdd.h"
#include <cstdlib>
#include <cstdio>
#include <cstdint>
#include <string>
#include <vector>
#include <filesystem>

using namespace bk;

static int g_fail = 0;
static int g_total = 0;

#define CHECK(cond, msg) do { \
    ++g_total; \
    if (!(cond)) { std::printf("FAIL: %s (line %d)\n", msg, __LINE__); ++g_fail; } \
} while (0)

// Helper: assemble a program of raw words into RAM at addr, reset PC there.
static void loadProg(Memory& m, uint16_t addr, std::initializer_list<uint16_t> words) {
    uint16_t a = addr;
    for (uint16_t w : words) { m.pokeWord(a, w); a += 2; }
}

int main() {
    // ---- MOV immediate ----
    {
        Memory m; Cpu c(m);
        loadProg(m, 01000, {012700, 0123456}); // MOV #123456, R0
        c.reset(01000);
        c.step();
        CHECK(c.r[0] == 0123456, "MOV #123456,R0 sets R0");
        CHECK((c.psw & Cpu::CC_N), "MOV negative sets N");
        CHECK(!(c.psw & Cpu::CC_Z), "MOV nonzero clears Z");
    }
    // ---- ADD with carry/overflow ----
    {
        Memory m; Cpu c(m);
        loadProg(m, 01000, {012700, 0100000, 012701, 0100000, 060100}); // R0=100000; R1=100000; ADD R1,R0
        c.reset(01000);
        c.step(); c.step(); c.step();
        CHECK(c.r[0] == 0, "100000+100000 = 0 (mod 2^16)");
        CHECK((c.psw & Cpu::CC_Z), "sum zero sets Z");
        CHECK((c.psw & Cpu::CC_C), "carry out sets C");
        CHECK((c.psw & Cpu::CC_V), "neg+neg->pos sets V");
    }
    // ---- SUB / CMP flags ----
    {
        Memory m; Cpu c(m);
        loadProg(m, 01000, {012700, 5, 012701, 5, 020001}); // R0=5;R1=5; CMP R0,R1
        c.reset(01000);
        c.step(); c.step(); c.step();
        CHECK((c.psw & Cpu::CC_Z), "CMP equal sets Z");
        CHECK(!(c.psw & Cpu::CC_N), "CMP equal clears N");
        CHECK(!(c.psw & Cpu::CC_C), "CMP equal: no borrow -> C clear");
    }
    // ---- INC / DEC ----
    {
        Memory m; Cpu c(m);
        loadProg(m, 01000, {012700, 0077777, 005200}); // R0=077777; INC R0
        c.reset(01000);
        c.step(); c.step();
        CHECK(c.r[0] == 0100000, "INC 077777 -> 100000");
        CHECK((c.psw & Cpu::CC_V), "INC to 100000 sets V");
        CHECK((c.psw & Cpu::CC_N), "INC result negative sets N");
    }
    // ---- Rotate / shift ----
    {
        Memory m; Cpu c(m);
        loadProg(m, 01000, {012700, 1, 006000}); // R0=1; ROR R0
        c.reset(01000);
        c.step(); c.step();
        CHECK(c.r[0] == 0, "ROR 1 -> 0 (bit into carry)");
        CHECK((c.psw & Cpu::CC_C), "ROR shifts LSB into C");
    }
    {
        Memory m; Cpu c(m);
        loadProg(m, 01000, {012700, 0040000, 006300}); // R0=040000; ASL R0
        c.reset(01000);
        c.step(); c.step();
        CHECK(c.r[0] == 0100000, "ASL 040000 -> 100000");
        CHECK((c.psw & Cpu::CC_N), "ASL result negative");
    }
    // ---- Branch taken (BEQ) ----
    {
        Memory m; Cpu c(m);
        // CLR R0 (Z=1); BEQ +2 (skip next); MOV #1,R0 (skipped); MOV #2,R0
        loadProg(m, 01000, {005000, 001402, 012700, 1, 012700, 2});
        c.reset(01000);
        c.step(); // CLR R0
        CHECK((c.psw & Cpu::CC_Z), "CLR sets Z");
        c.step(); // BEQ taken, skip the MOV #1
        CHECK(c.r[7] == 01010, "BEQ taken skips one 2-word instruction");
    }
    // ---- JSR / RTS ----
    {
        Memory m; Cpu c(m);
        // 01000: JSR PC, sub ; 01004: HALT
        // sub at 01100: MOV #7,R0 ; RTS PC
        loadProg(m, 01000, {004767, 0000074}); // JSR PC, 01100 (disp from pc-after=01004 -> 01100 = +074)
        loadProg(m, 01004, {000000});          // HALT
        loadProg(m, 01100, {012700, 7, 000207}); // MOV #7,R0 ; RTS PC
        c.reset(01000);
        c.r[6] = 02000; // stack
        c.step(); // JSR
        CHECK(c.r[7] == 01100, "JSR jumps to subroutine");
        c.step(); // MOV #7,R0
        c.step(); // RTS PC
        CHECK(c.r[0] == 7, "subroutine ran");
        CHECK(c.r[7] == 01004, "RTS returns to caller");
    }
    // ---- SOB loop ----
    {
        Memory m; Cpu c(m);
        // R1=3; loop: DEC R0 (dummy) ; SOB R1, loop
        loadProg(m, 01000, {012701, 3, 005300, 077102}); // MOV#3,R1; DEC R0; SOB R1, back 2 words
        c.reset(01000);
        c.step(); // MOV
        int iters = 0;
        while (iters < 100) {
            c.step(); // DEC R0
            uint16_t before = c.r[1];
            c.step(); // SOB
            ++iters;
            if (c.r[1] == 0 && before == 1) break;
        }
        CHECK(c.r[1] == 0, "SOB counts R1 down to 0");
        CHECK(iters == 3, "SOB loops exactly 3 times");
    }
    // ---- Disassembler smoke test ----
    {
        Memory m;
        m.pokeWord(01000, 012700); m.pokeWord(01002, 0123456); // MOV #123456,R0
        DisasmLine d = disasm(m, 01000);
        CHECK(d.words == 2, "MOV #imm,R0 is 2 words");
        CHECK(d.text.rfind("MOV", 0) == 0, "disasm mnemonic MOV");
        std::printf("disasm: %s\n", d.text.c_str());
    }

    // ---- Save / restore state round-trip ----
    {
        Board b;
        b.reset();
        b.memory().pokeWord(01000, 0123456);
        b.memory().pokeWord(042000, 0154321); // video RAM
        b.cpu().r[3] = 07777;
        b.cpu().r[7] = 02000;
        b.cpu().psw = 017;
        // Use the platform temp directory (which always exists) rather than a
        // hard-coded path, so the test passes on any machine.
        std::string tmp = (std::filesystem::temp_directory_path() / "bk_state_test.bkst").string();
        bool saved = b.saveState(tmp.c_str());
        CHECK(saved, "saveState succeeds");
        // Corrupt everything, then restore.
        b.memory().pokeWord(01000, 0);
        b.memory().pokeWord(042000, 0);
        b.cpu().r[3] = 0; b.cpu().r[7] = 0; b.cpu().psw = 0;
        bool loaded = b.loadState(tmp.c_str());
        CHECK(loaded, "loadState succeeds");
        CHECK(b.memory().peekWord(01000) == 0123456, "RAM restored");
        CHECK(b.memory().peekWord(042000) == 0154321, "video RAM restored");
        CHECK(b.cpu().r[3] == 07777, "R3 restored");
        CHECK(b.cpu().r[7] == 02000, "PC restored");
        CHECK(b.cpu().psw == 017, "PSW restored");
    }

    // ---- HALT traps through vector 0004 (BK-0010), not a permanent stop ----
    {
        Memory m; Cpu c(m);
        m.pokeWord(0004, 03000);   // STOP vector -> handler
        m.pokeWord(0006, 0);
        m.pokeWord(01000, 000000); // HALT
        c.reset(01000); c.r[6] = 02000;
        c.step();
        CHECK(!c.halted(), "HALT is not a permanent stop when vector 0004 is set");
        CHECK(c.pc() == 03000, "HALT traps to the vector-0004 handler");
        CHECK(m.peekWord(01774) == 01002, "HALT pushes return PC on the stack");
    }
    {
        Memory m; Cpu c(m);
        m.pokeWord(0004, 0);       // no handler
        m.pokeWord(01000, 000000); // HALT
        c.reset(01000); c.r[6] = 02000;
        c.step();
        CHECK(c.halted(), "HALT with no vector-0004 handler stops the CPU (no loop)");
    }

    // ---- 1801VM1 programmable timer (0177706/0177710/0177712) ----
    // Bits: CONTINUOUS=002 ENBEND=004 ONCE=010 START=020 DIV16=040 DIV4=0100 END=0200
    {
        Board b;
        b.reset();
        b.memory().pokeWord(01000, 000777);      // BR . (16 ticks/iter)
        b.cpu().reset(01000);
        // Continuous reload mode, event flag enabled, limit 100.
        b.memory().writeWord(0177706, 100);
        b.memory().writeWord(0177712, 020 | 004); // START | ENBEND
        b.runTicks(50 * 128);                     // ~50 timer periods
        uint16_t cnt = b.memory().readWord(0177710);
        CHECK(cnt <= 60 && cnt >= 40, "timer counts down (~50 from 100)");
        CHECK(!(b.memory().readWord(0177712) & 0200), "END flag not set before underflow");
        b.runTicks(70 * 128);                     // pass zero
        uint16_t csr = b.memory().readWord(0177712);
        CHECK(csr & 0200, "END/FL flag set on underflow");
        CHECK(csr & 020, "continuous mode keeps running (START still set)");
        // Writing the CSR clears the FL flag.
        b.memory().writeWord(0177712, 020 | 004);
        CHECK(!(b.memory().readWord(0177712) & 0200), "writing CSR clears END flag");
    }
    {
        // One-shot mode stops after underflow.
        Board b;
        b.reset();
        b.memory().pokeWord(01000, 000777);
        b.cpu().reset(01000);
        b.memory().writeWord(0177706, 10);
        b.memory().writeWord(0177712, 020 | 010 | 004); // START | ONCE | ENBEND
        b.runTicks(40 * 128);
        uint16_t csr = b.memory().readWord(0177712);
        CHECK(!(csr & 020), "one-shot timer stops (START cleared)");
        CHECK(csr & 0200, "one-shot sets END flag");
        // Регрессия («зависание» dizzy2021.bin на уровне): по ВМ1 §4.4 однократный
        // режим при достижении нуля не только останавливает счёт, но и
        // ПЕРЕЗАГРУЖАЕТ счётчик из регистра предела. Типовая пауза
        // `CMP @#177706, @#177710 / BNE .-` ждёт именно этого равенства.
        CHECK(b.memory().readWord(0177710) == 10,
              "однократный таймер по концу периода перезагружается из предела");
    }
    {
        // Regression (Digger freeze): a long gap between reads — delta spanning
        // many periods — must NOT wrap the reload counter to a huge value, else
        // the game's "while (FL==0)" frame-pacing loop hangs for seconds.
        Board b;
        b.reset();
        b.memory().pokeWord(01000, 000777);
        b.cpu().reset(01000);
        b.memory().writeWord(0177706, 100);             // limit
        b.memory().writeWord(0177712, 020 | 004);       // START | ENBEND (reload mode)
        b.runTicks(10000 * 128);                        // ~100 full periods, no reads between
        uint16_t cnt = b.memory().readWord(0177710);
        CHECK(cnt <= 100, "timer reload stays bounded after a long gap (no huge wrap)");
        CHECK(b.memory().readWord(0177712) & 0200, "timer FL set after a long gap");
    }
    {
        // DIV4 makes the timer 4x slower.
        Board b;
        b.reset();
        b.memory().pokeWord(01000, 000777);
        b.cpu().reset(01000);
        b.memory().writeWord(0177706, 1000);
        b.memory().writeWord(0177712, 020 | 0100);      // START | DIV4
        b.runTicks(100 * 128);                          // 100 base periods = 25 with /4
        uint16_t cnt = b.memory().readWord(0177710);
        CHECK(cnt <= 1000 - 20 && cnt >= 1000 - 30, "DIV4 slows timer ~4x (~25 counts)");
    }

    // ---- Keyboard: single-code register, new press OVERWRITES an unread code ----
    // (как на реальной БК и в референсном bk/tty.c — иначе игры, читающие код
    // только по событию (LAND), получают предыдущую клавишу вместо новой)
    {
        Board b;
        b.reset();
        CHECK(!b.keyReady(), "keyboard: no key ready initially");
        CHECK(b.pressKey(0101), "keyboard: first key (A) latched");   // 'A'
        CHECK(b.keyReady(), "keyboard: ready flag set after keypress");
        // Реальная ВП1-014 (как bk/tty.c): новое нажатие ПЕРЕЗАПИСЫВАЕТ регистр,
        // даже если старый код не прочитан. Игры (PITON) читают 0177662 по биту
        // «клавиша нажата» и должны видеть код ПОСЛЕДНЕЙ клавиши, а не застрявший.
        CHECK(b.pressKey(0102), "keyboard: second key overwrites unread code");
        CHECK((b.memory().readWord(0177662) & 0177) == 0102, "keyboard: register holds LATEST code");
        // Флаг готовности гаснет НЕ мгновенно, а через ~900 тактов после чтения кода
        // (ВП1-014). Сразу после чтения он ещё стоит.
        CHECK(b.keyReady(), "keyboard: флаг готовности ещё стоит сразу после чтения");
        b.memory().pokeWord(01000, 000777);   // BR . — крутимся на месте
        b.cpu().reset(01000);
        b.runTicks(1000);
        CHECK(!b.keyReady(), "keyboard: флаг готовности гаснет через ~900 тактов");
        CHECK(b.pressKey(0103), "keyboard: next key latched after previous was read");
        CHECK((b.memory().readWord(0177662) & 0177) == 0103, "keyboard: register holds next code");
        // Register 0177716 bit 6 (key-pressed, active-low) tracks the *physical*
        // key-held state, set/cleared independently of the code register — games
        // like Digger poll it and must keep seeing the key after the monitor ISR
        // has already drained 0177662.
        CHECK(b.memory().readWord(0177716) & 0100, "keyboard: 0177716 bit high with no key held");
        b.setKeyHeld(true);
        CHECK(!(b.memory().readWord(0177716) & 0100), "keyboard: 0177716 bit low while key held");
        b.memory().readWord(0177662);   // draining the code register must not release it
        CHECK(!(b.memory().readWord(0177716) & 0100), "keyboard: 0177716 still low after code read while held");
        b.setKeyHeld(false);
        CHECK(b.memory().readWord(0177716) & 0100, "keyboard: 0177716 bit high after release");
    }

    // ---- Клавиатура: отсрочка прерывания и его отмена по прочтению кода ----
    // Прерывание клавиатуры выдаётся НЕ мгновенно: игра, которая его разрешила и
    // при этом сама опрашивает регистр (SOKOBAN: CLR 177660, затем TST/CMPB),
    // должна успеть прочитать код раньше ISR монитора. Отсрочка — четверть кадра.
    {
        Board b;
        b.reset();
        b.memory().pokeWord(Cpu::VEC_KEYBOARD, 02000);      // обработчик по 02000
        b.memory().pokeWord(Cpu::VEC_KEYBOARD + 2, 0);      // ССП обработчика
        b.memory().pokeWord(01000, 000777);                 // BR . — крутимся на месте
        b.memory().pokeWord(02000, 000777);                 // и в «обработчике» тоже
        b.cpu().reset(01000, 0);                            // маска открыта
        b.pressKey(0101);
        b.runTicks(2000);
        CHECK(b.cpu().pc() == 01000, "клавиатура: свежий код не прерывает сразу");
        b.runTicks(b.ticksPerFrame() / 2);
        CHECK(b.cpu().pc() == 02000, "клавиатура: прерывание выдано после отсрочки");

        // А если код успели прочитать — прерывать уже нечем, запрос снимается.
        Board c;
        c.reset();
        c.memory().pokeWord(Cpu::VEC_KEYBOARD, 02000);
        c.memory().pokeWord(Cpu::VEC_KEYBOARD + 2, 0);
        c.memory().pokeWord(01000, 000777);
        c.memory().pokeWord(02000, 000777);
        c.cpu().reset(01000, 0);
        c.pressKey(0101);
        CHECK((c.memory().readWord(0177662) & 0177) == 0101, "клавиатура: опрос успел прочитать код");
        c.runTicks(c.ticksPerFrame());
        CHECK(c.cpu().pc() == 01000, "клавиатура: прочитанный код прерывание отменяет");
    }

    // ---- Vp037: геометрия развёртки (порт va_037.v) ----
    {
        // Свободный прогон от начала видимого поля. Считаем строки по перепадам
        // HGATE (одна активная зона ~HGATE на строку) и такты с активной развёрткой.
        auto measure = [](bool full) {
            Vp037 v; v.setM256(full); v.syncToFrameTop();
            int visLines = 0, blankLines = 0, activeClkin = 0;
            bool prevH = v.hgate(), prevV = v.vgate();
            // Прогоняем один полный кадр 037 = 320 строк × 384 CLKIN = 122880 тактов.
            for (int i = 0; i < 320 * 384; ++i) {
                if (v.inActiveDisplay()) ++activeClkin;
                v.tick(1);
                // Начало строки — по спаду HGATE (конец гашения предыдущей строки).
                if (prevH && !v.hgate()) { if (!v.vgate()) ++visLines; else ++blankLines; }
                prevH = v.hgate(); prevV = v.vgate();
            }
            return std::make_tuple(visLines, blankLines, activeClkin);
        };
        auto [visF, blkF, actF] = measure(true);
        CHECK(visF == 256, "Vp037: 256 видимых строк в кадре (полный экран)");
        CHECK(blkF == 64,  "Vp037: 64 строки кадрового гашения");
        // Активная зона (полный экран): 256 строк × 256 CLKIN видимой части = 65536.
        CHECK(actF == 256 * 256, "Vp037: активная развёртка = 256×256 CLKIN (полный)");

        auto [visS, blkS, actS] = measure(false);
        CHECK(visS == 256 && blkS == 64, "Vp037: та же геометрия строк в малом экране");
        // Малый экран: видеовыборка только на первых 64 видимых строках.
        CHECK(actS == 64 * 256, "Vp037: активная развёртка = 64×256 CLKIN (малый экран)");

        // Ресинк на верх кадра даёт видимую часть, гашение — в конце (обрезается там).
        Vp037 v; v.setM256(true); v.syncToFrameTop();
        CHECK(v.inActiveDisplay(), "Vp037: верх кадра — активная развёртка");
        v.tick(60000 * 2);   // столько CLKIN проходит эмулятор за кадр (60000 тактов)
        CHECK(!v.inActiveDisplay(), "Vp037: на границе кадра эмулятора луч в гашении");
    }

    // ---- Board: арбитраж 037 замедляет исполнение в активной развёртке ----
    {
        // Тесный цикл в ОЗУ, активно бьющий в ДОЗУ (INC слова + переход обратно).
        // За фиксированный бюджет тактов с арбитражем проходит МЕНЬШЕ инструкций —
        // часть тактов уходит в ожидание доступа к ДОЗУ во время активной развёртки.
        auto instrs = [](bool arb) {
            Board b; b.setArbitration(arb); b.reset();
            b.memory().pokeWord(01000, 0005237);   // INC @#004000  (чтение+запись ДОЗУ)
            b.memory().pokeWord(01002, 0004000);
            b.memory().pokeWord(01004, 0000775);   // BR 01000
            b.cpu().reset(01000, 0340);            // PC=01000, прерывания замаскированы
            long long before = (long long)b.totalTicks();
            long long budget = 4LL * b.ticksPerFrame();
            int n = 0;
            while ((long long)b.totalTicks() - before < budget) { b.stepInstruction(); ++n; }
            return n;
        };
        int on  = instrs(true);
        int off = instrs(false);
        CHECK(on < off, "Board: арбитраж 037 снижает число инструкций за интервал");
    }

    // ---- Раскладки джойстика (общая таблица ядра, src/core/Joystick.h) ----
    {
        std::string err;
        CHECK(joyMask(JoyStandard::Klad2, "right", &err) == 0001, "Клад-2: ВПРАВО = 001");
        CHECK(joyMask(JoyStandard::Klad2, "left") == 0002, "Клад-2: ВЛЕВО = 002");
        CHECK(joyMask(JoyStandard::SWCorp, "up") == 02000, "SWCorp: ВВЕРХ = 02000 (старший байт)");
        CHECK(joyMask(JoyStandard::Standard, "up+fire1") == 0041, "Стандарт: ВВЕРХ+ОГОНЬ1 = 041");
        CHECK(joyMask(JoyStandard::Standard, "вправо, огонь") == 0042, "русские имена и запятая");
        CHECK(joyMask(JoyStandard::Standard, "ВПРАВО") == 0002, "имена нечувствительны к регистру");
        CHECK(joyMask(JoyStandard::Klad2, "bit3") == 0010, "bitN — сырой бит вне раскладки");
        CHECK(joyMask(JoyStandard::Standard, "") == 0 && joyMask(JoyStandard::Standard, "none") == 0,
              "пусто/none -> 0");
        err.clear();
        CHECK(joyMask(JoyStandard::Standard, "up+нетакой", &err) == 0 && !err.empty(),
              "неизвестная кнопка -> ошибка");

        JoyStandard s = JoyStandard::Standard;
        CHECK(joyParseStandard("klad2", s) && s == JoyStandard::Klad2, "разбор имени раскладки");
        CHECK(joyParseStandard("Клад-2", s) && s == JoyStandard::Klad2, "русский алиас раскладки");
        CHECK(!joyParseStandard("нетакой", s), "неизвестная раскладка отвергается");

        CHECK(joyDecode(JoyStandard::Klad2, 0001) == "ВПРАВО", "расшифровка одного бита");
        CHECK(joyDecode(JoyStandard::Standard, 0) == "нет", "расшифровка нуля");
        CHECK(joyDecode(JoyStandard::Klad2, 0400).find("неизв") != std::string::npos,
              "бит вне раскладки помечается как неизвестный");
    }

    // ---- Имена клавиш и кодирование текста в КОИ-7 (src/core/BkKeys.h) ----
    {
        CHECK(bkKeyByName("enter") == 012, "имя enter -> 012");
        CHECK(bkKeyByName("ВВОД") == 012, "русское имя, верхний регистр");
        CHECK(bkKeyByName("рус") == BK_CODE_RUS, "рус -> 016");
        CHECK(bkKeyByName("right") == 031 && bkKeyByName("up") == 032, "стрелки");
        CHECK(bkKeyByName("нетакой") == BK_KEY_NONE, "неизвестное имя");
        CHECK(std::string(bkKeyName(012)) == "enter", "обратный поиск имени");

        bool cyr = false;
        auto v = bkEncodeText("A", cyr);
        CHECK(v.size() == 1 && v[0] == 'A' && !cyr, "латиница из ЛАТ — без префикса");
        v = bkEncodeText("б", cyr);
        // КОИ-7 Н1 начинается с Ю(0140), А(0141), Б(0142) — см. kKoi7H1Utf8.
        CHECK(v.size() == 2 && v[0] == BK_CODE_RUS && v[1] == 0142 && cyr,
              "кириллица вставляет РУС и кодирует Б=0142");
        v = bkEncodeText("A", cyr);
        CHECK(v.size() == 2 && v[0] == BK_CODE_LAT && v[1] == 'A' && !cyr,
              "возврат к латинице вставляет ЛАТ");
        v = bkEncodeText("1\n", cyr);
        CHECK(v.size() == 2 && v[0] == '1' && v[1] == 012, "цифра без префикса, \\n -> ВВОД");
    }

    // ---- Снимок состояния: джойстик и удержание клавиши ----
    {
        Board b; b.reset();
        b.memory().pokeWord(01000, 0123456);
        b.setJoystick(02000);
        b.setKeyHeld(true);
        std::vector<uint8_t> snap;
        b.saveStateMem(snap);
        b.setJoystick(0);
        b.setKeyHeld(false);
        b.memory().pokeWord(01000, 0);
        CHECK(b.loadStateMem(snap), "снимок читается");
        CHECK(b.joystick() == 02000, "джойстик восстановлен");
        CHECK(b.keyHeld(), "удержание клавиши восстановлено");
        CHECK(b.memory().peekWord(01000) == 0123456, "ОЗУ восстановлено");
        CHECK(b.peekReg(0177714) == 02000, "0177714 отдаёт восстановленное значение");

        // Фаза развёртки 037 тоже входит в снимок: с построчной отрисовкой от неё
        // будет зависеть картинка, а сейчас — такты ожидания ДОЗУ.
        {
            Board b2; b2.reset();
            b2.runTicks(12345);                       // увести луч в произвольную фазу
            std::vector<uint8_t> s2; b2.saveStateMem(s2);
            const int lc = b2.vp037().lc();
            const bool hg = b2.vp037().hgate();
            b2.runTicks(7777);                        // сдвинуть фазу
            CHECK(b2.loadStateMem(s2), "снимок с фазой развёртки читается");
            CHECK(b2.vp037().lc() == lc && b2.vp037().hgate() == hg, "фаза развёртки восстановлена");
        }

        // Снимок старого формата (без хвоста ext[2]) должен читаться как «ввод отпущен».
        // Длину базовой части считаем явно: магия + ОЗУ + R0..R7 + PSW + dev[7]. Раньше
        // здесь было «snap.end() - 8», и любой новый хвост (037, СМК) молча ломал смысл.
        const size_t kBaseLen = 8 + 0100000 + (8 + 1 + 7) * sizeof(uint16_t);
        std::vector<uint8_t> old(snap.begin(), snap.begin() + kBaseLen);
        CHECK(b.loadStateMem(old), "снимок старого формата читается");
        CHECK(b.joystick() == 0 && !b.keyHeld(), "старый формат -> ввод отпущен");
        CHECK(!b.loadStateMem(std::vector<uint8_t>(8, 0)), "мусор отвергается");
    }

    // ---- Лог обращений к В-В: чтения, фильтр адреса, PC инструкции ----
    {
        Board b; b.reset();
        b.setJoystick(0123);
        b.setIoLog(true, true, 64, 0177714);
        loadProg(b.memory(), 01000, {0013700, 0177714,    // MOV @#177714, R0
                                     0013701, 0177660});  // MOV @#177660, R1 — отфильтруется
        b.cpu().reset(01000, 0340);
        b.stepInstruction();
        b.stepInstruction();
        const auto& log = b.ioLog();
        CHECK(log.size() == 1, "фильтр адреса пропускает только 0177714");
        if (log.size() == 1) {
            CHECK(log[0].isRead, "обращение помечено как чтение");
            CHECK(log[0].addr == 0177714 && log[0].value == 0123, "адрес и значение");
            CHECK(log[0].pc == 01000, "PC = адрес инструкции, а не значение регистра PC");
        }
    }

    // ---- Лог фронтов динамика ----
    {
        Board b; b.reset();
        b.setSpeakerLog(true);
        b.memory().writeWord(0177716, 0100);   // бит 6 -> уровень 4
        b.memory().writeWord(0177716, 0100);   // повтор — не фронт
        b.memory().writeWord(0177716, 0);      // -> уровень 0
        const auto& sl = b.speakerLog();
        CHECK(sl.size() == 2, "пишутся только смены уровня");
        if (sl.size() == 2) CHECK(sl[0].level == 4 && sl[1].level == 0, "уровни динамика");
    }

    // ---- Динамик: 8 уровней из битов 6, 5 и 2 регистра 0177716 ----
    {
        Board b; b.reset();
        b.setSpeakerLog(true);
        struct { uint16_t reg; uint8_t level; } cases[] = {
            {0000, 0}, {0004, 1}, {0040, 2}, {0044, 3},
            {0100, 4}, {0104, 5}, {0140, 6}, {0144, 7},
        };
        bool ok = true;
        for (auto& c : cases) {
            b.memory().writeWord(0177716, c.reg);
            if (b.speakerLog().empty() || b.speakerLog().back().level != c.level) {
                if (!(c.level == 0 && b.speakerLog().empty())) ok = false;
            }
        }
        CHECK(ok, "динамик: все 8 комбинаций битов 6/5/2 дают уровни 0..7");
        // Бит 7 (мотор ленты) на динамик не влияет.
        b.memory().writeWord(0177716, 0144);
        size_t n = b.speakerLog().size();
        b.memory().writeWord(0177716, 0344);   // добавили бит 7
        CHECK(b.speakerLog().size() == n, "бит 7 (мотор ленты) уровень динамика не меняет");
    }

    // ---- Параллельный порт: раздельные регистры ввода и вывода ----
    {
        Board b; b.reset();
        b.setJoystick(0123);
        b.memory().writeWord(0177714, 0252);
        CHECK(b.memory().readWord(0177714) == 0123, "чтение 0177714 отдаёт джойстик, а не записанное");
        CHECK(b.portOut() == 0252, "запись в 0177714 защёлкивается отдельно");
    }

    // ---- 0177716 бит 2: признак записи в системный регистр ----
    {
        Board b; b.reset();
        CHECK(!(b.memory().readWord(0177716) & 004), "бит 2 сброшен, пока записи не было");
        b.memory().writeWord(0177716, 0);
        CHECK((b.memory().readWord(0177716) & 004), "после записи бит 2 взведён");
        CHECK(!(b.memory().readWord(0177716) & 004), "чтение гасит бит 2");
    }

    // ---- RESET: PSW не трогает, периферию сбрасывает ----
    {
        Board b; b.reset();
        b.memory().writeWord(0177712, 020 | 004);   // запустить таймер
        b.memory().writeWord(0177716, 0144);        // динамик на максимум
        b.memory().writeWord(0177714, 0252);        // что-то в порт
        b.memory().pokeWord(01000, 000005);         // RESET
        b.cpu().reset(01000, 0);                    // маска открыта
        b.stepInstruction();
        CHECK(b.cpu().psw == 0, "RESET не меняет PSW");
        CHECK((b.peekReg(0177660) & 0100) != 0, "RESET запрещает прерывания от клавиатуры");
        CHECK(b.portOut() == 0, "RESET обнуляет выходной регистр порта");
        CHECK(!(b.peekReg(0177712) & 020), "RESET останавливает таймер");
    }

    // ---- WAIT: прерывание возвращает управление ЗА команду, а не на неё ----
    {
        Memory m; Cpu c(m);
        m.pokeWord(0100, 03000);    // вектор кадрового прерывания
        m.pokeWord(0102, 0);
        m.pokeWord(01000, 000001);  // WAIT
        m.pokeWord(01002, 005000);  // CLR R0 — сюда обязаны вернуться
        c.reset(01000); c.r[6] = 02000;
        c.step();
        CHECK(c.waiting(), "WAIT переводит ЦП в ожидание");
        CHECK(c.pc() == 01000, "пока ждём, PC стоит на самой команде WAIT");
        c.interrupt(0100);
        CHECK(!c.waiting(), "прерывание выводит из ожидания");
        CHECK(c.pc() == 03000, "ушли в обработчик");
        CHECK(m.peekWord(01774) == 01002, "в стеке адрес ЗА WAIT, а не сам WAIT");
    }
    {
        // Сквозная проверка: WAIT в цикле не должен зависать между кадрами.
        Board b; b.reset();
        b.memory().pokeWord(0100, 03000);   // ISR
        b.memory().pokeWord(0102, 0);
        b.memory().pokeWord(03000, 0005201); // INC R1
        b.memory().pokeWord(03002, 0000002); // RTI
        b.memory().pokeWord(01000, 0000001); // WAIT
        b.memory().pokeWord(01002, 0005200); // INC R0
        b.memory().pokeWord(01004, 0000775); // BR 01000 (смещение -3 слова)
        b.cpu().reset(01000, 0);             // маска открыта
        b.cpu().r[6] = 02000;
        for (int i = 0; i < 5; ++i) b.runFrame();
        CHECK(b.cpu().r[1] >= 4, "ISR вызывается каждый кадр");
        CHECK(b.cpu().r[0] >= 4, "код ПОСЛЕ WAIT выполняется (нет зацикливания)");
    }

    // ---- Кадровое прерывание защёлкивается, а не теряется при закрытой маске ----
    {
        Board b; b.reset();
        b.memory().pokeWord(0100, 03000);
        b.memory().pokeWord(0102, 0);
        b.memory().pokeWord(03000, 0005201); // INC R1
        b.memory().pokeWord(03002, 0000002); // RTI
        b.memory().pokeWord(01000, 0000777); // BR . — критическая секция
        b.cpu().reset(01000, 0340);          // приоритет 7 — прерывания закрыты
        b.cpu().r[6] = 02000;
        b.runFrame();
        CHECK(b.cpu().r[1] == 0, "при закрытой маске прерывание не выдаётся");
        b.cpu().psw = 0;                     // открыли маску
        b.runTicks(200);
        CHECK(b.cpu().r[1] == 1, "защёлкнутый запрос выдаётся сразу после открытия маски");
    }

    // ---- Внутренние регистры ЦП 0177700/02/04 (04-процессор, §4.3) ----
    {
        Board b; b.reset();
        CHECK(b.memory().readWord(0177700) == 0177740, "CPU_MODE читается как 177740");
        CHECK(b.memory().readWord(0177702) == 0177777, "CPU_IVEC: только запись, читается как все единицы");
        CHECK(b.memory().readWord(0177704) == 0177440, "CPU_ERROR: флаги очищены -> 177440");
        // По записи в CPU_MODE доступны только разряды 0..2.
        b.memory().writeWord(0177700, 0177777);
        CHECK(b.memory().readWord(0177700) == 0177747, "в CPU_MODE записываются только разряды 0..2");
        // CPU_ERROR писать бессмысленно: биты гаснут на выборке каждой команды.
        b.memory().writeWord(0177704, 0177777);
        CHECK(b.memory().readWord(0177704) == 0177440, "CPU_ERROR запись не меняет");
        CHECK(b.peekReg(0177700) == 0177747 && b.peekReg(0177704) == 0177440,
              "peekReg отдаёт то же без побочных эффектов");
    }

    // ---- «Баг флага C» ВМ1 [ВМ1 §10]: примеры прямо из документации ----
    {
        // SEC / MOVB R1,R0 / BCC — переход БУДЕТ выполнен (неверно, но так на железе).
        Memory m; Cpu c(m);
        loadProg(m, 01000, {0000261, 0110100, 0103002, 0012700, 1, 0012700, 2});
        //                   SEC     MOVB R1,R0  BCC +2      MOV #1,R0   MOV #2,R0
        c.reset(01000);
        c.step(); c.step();
        CHECK((c.psw & Cpu::CC_C), "в самом PSW флаг C остаётся верным");
        c.step();
        CHECK(c.pc() == 01012, "BCC после MOVB Rd видит C сброшенным — переход выполнен");
    }
    {
        // Тот же код, но между MOVB и BCC вставлен NOP — переход НЕ выполняется.
        Memory m; Cpu c(m);
        loadProg(m, 01000, {0000261, 0110100, 0000240, 0103002, 0012700, 1});
        //                   SEC     MOVB R1,R0  NOP     BCC +2
        c.reset(01000);
        c.step(); c.step(); c.step(); c.step();
        CHECK(c.pc() == 01010, "любая промежуточная команда снимает эффект — перехода нет");
    }
    {
        // Ключевой пример: MFPS R2 между ними НЕ лечит, потому что MFPS Rd — тоже
        // триггер. При этом сам R2 получает верный C из PSW.
        Memory m; Cpu c(m);
        loadProg(m, 01000, {0000261, 0110100, 0106702, 0103002, 0012700, 1});
        //                   SEC     MOVB R1,R0  MFPS R2  BCC +2
        c.reset(01000);
        c.step(); c.step(); c.step();
        CHECK((c.r[2] & 1) == 1, "MFPS читает PSW и получает верный C");
        c.step();
        CHECK(c.pc() == 01014, "MFPS Rd сам является триггером — переход всё равно выполнен");
    }
    {
        // Приёмник в памяти триггером не является, и BCS видит C верно.
        Memory m; Cpu c(m);
        loadProg(m, 01000, {0000261, 0110037, 03000, 0103402, 0012700, 1});
        //                   SEC     MOVB R0,@#3000   BCS +2
        c.reset(01000);
        c.step(); c.step(); c.step();
        CHECK(c.pc() == 01014, "MOVB с приёмником в памяти бага не даёт: BCS сработал");
    }
    {
        // Эмуляцию бага можно выключить.
        Memory m; Cpu c(m);
        c.setEmulateCBug(false);
        loadProg(m, 01000, {0000261, 0110100, 0103002, 0012700, 1});
        c.reset(01000);
        c.step(); c.step(); c.step();
        CHECK(c.pc() == 01006, "с выключенной эмуляцией перехода нет");
    }

    // ---- Длительность кадра: 61440 тактов (48,83 Гц), а не 60000 ----
    {
        Board b;
        CHECK(b.ticksPerFrame() == 61440, "кадр = 61440 тактов ЦП");
        CHECK(b.ticksPerFrame() == Vp037::CLKIN_PER_FRAME / 2,
              "кадр выведен из геометрии 037, а не задан отдельной константой");
        const double hz = b.frameRateHz();
        CHECK(hz > 48.8 && hz < 48.9, "кадровая частота 48,83 Гц");
        // Ресинк развёртки на границе кадра теперь ничего не отбрасывает: за кадр
        // луч проходит ровно 320 строк и возвращается в начало.
        b.reset();
        b.memory().pokeWord(01000, 000777);   // BR .
        b.cpu().reset(01000, 0340);
        b.runTicks(b.ticksPerFrame() / 2);
        CHECK(b.vp037().scanline() >= 158 && b.vp037().scanline() <= 162,
              "за половину кадра луч проходит примерно половину из 320 строк");
        b.runTicks(b.ticksPerFrame() / 2);
        CHECK(b.vp037().scanline() == 0, "к концу кадра луч возвращается в начало (счёт по модулю 320)");
    }

    // ---- Клавиша «СТОП»: вектор 4, приоритет её не запрещает ----
    {
        Board b; b.reset();
        b.memory().pokeWord(0004, 03000);    // обработчик «СТОП»/зависания
        b.memory().pokeWord(0006, 0340);
        b.memory().pokeWord(03000, 0005201); // INC R1
        b.memory().pokeWord(03002, 0000002); // RTI
        b.memory().pokeWord(01000, 0000777); // BR . — программа крутится
        b.cpu().reset(01000, 0340);          // приоритет 7: кадровое НЕ пройдёт
        b.cpu().r[6] = 02000;
        b.runTicks(200);
        CHECK(b.cpu().r[1] == 0, "без нажатия обработчик не вызывается");
        b.pressStop();
        b.runTicks(200);
        CHECK(b.cpu().r[1] == 1, "«СТОП» проходит даже при закрытой маске (внеприоритетное)");
        CHECK(b.memory().peekWord(01774) == 01000, "в стеке PC прерванной программы");
        // Повторного срабатывания без нового нажатия быть не должно.
        b.runTicks(2000);
        CHECK(b.cpu().r[1] == 1, "запрос гасится после выдачи");
    }
    {
        // Вектор не установлен — прерывать некуда, но и виснуть нельзя.
        Board b; b.reset();
        b.memory().pokeWord(0004, 0);
        b.memory().pokeWord(01000, 0000777);
        b.cpu().reset(01000, 0);
        b.cpu().r[6] = 02000;
        b.pressStop();
        b.runTicks(200);
        CHECK(b.cpu().pc() == 01000 && !b.cpu().halted(), "без вектора 4 «СТОП» просто гасится");
    }

    // ---- runUntil («шаг с обходом», «до адреса») продолжает выдавать кадры ----
    {
        Board b; b.reset();
        b.memory().pokeWord(0100, 03000);    // ISR
        b.memory().pokeWord(0102, 0);
        b.memory().pokeWord(03000, 0000002); // RTI
        b.memory().pokeWord(01000, 0000001); // WAIT — без кадрового IRQ отсюда не выйти
        b.memory().pokeWord(01002, 0000240); // NOP — цель прогона
        b.cpu().reset(01000, 0);
        b.cpu().r[6] = 02000;
        CHECK(b.runUntil(01002, 5 * b.ticksPerFrame()),
              "runUntil проходит WAIT: кадры выдаются и в прогоне отладчика");
    }

    // ---- Ловушка по T-биту (вектор 014) и разница RTI/RTT ----
    // На ней держится «музыка параллельно с игрой»: обработчик 014 крутит динамик
    // после каждой команды основной программы и возвращается по RTT.
    {
        Memory m; Cpu c(m);
        m.pokeWord(014, 03000);          // вектор T-ловушки
        m.pokeWord(016, 0340);           // ССП обработчика: приоритет 7, T снят
        m.pokeWord(01000, 005000);       // CLR R0
        m.pokeWord(01002, 005001);       // CLR R1
        c.reset(01000, 020);             // PSW: только T
        c.r[6] = 02000;
        c.step();                        // CLR R0 -> ловушка
        CHECK(c.pc() == 03000, "T-бит: ловушка через вектор 014 после команды");
        CHECK(c.psw == 0340, "ССП обработчика берётся из вектора+2 (T снят)");
        CHECK(m.peekWord(01776) == 024, "на стеке старый ССП (T + Z от CLR)");
        CHECK(m.peekWord(01774) == 01002, "на стеке PC следующей команды");
        CHECK(c.r[0] == 0, "команда всё же выполнилась до ловушки");

        // Обработчик возвращается по RTT: подавляет ловушку ровно на одну команду.
        m.pokeWord(03000, 000006);       // RTT
        c.step();
        CHECK(c.pc() == 01002 && c.psw == 024, "RTT восстановил PC/ССП");
        c.step();                        // CLR R1 выполняется...
        CHECK(c.r[1] == 0, "после RTT одна команда программы выполняется");
        CHECK(c.pc() == 03000, "...и только потом снова ловушка");
    }
    {
        // RTI, в отличие от RTT, ловится сразу: следующая команда не выполняется.
        Memory m; Cpu c(m);
        m.pokeWord(014, 03000);
        m.pokeWord(016, 0340);
        m.pokeWord(01000, 000002);       // RTI
        m.pokeWord(01002, 005000);       // CLR R0 — выполниться не должна
        c.reset(01000, 0);
        c.r[6] = 01774;
        m.pokeWord(01774, 01002);        // PC
        m.pokeWord(01776, 020);          // ССП с T
        c.r[0] = 0123456;
        c.step();
        CHECK(c.pc() == 03000, "RTI с T=1 ловится немедленно");
        CHECK(c.r[0] == 0123456, "команда после RTI не выполнилась");
    }
    {
        // Без T-бита ловушки нет, даже если вектор 014 заполнен.
        Memory m; Cpu c(m);
        m.pokeWord(014, 03000); m.pokeWord(016, 0340);
        m.pokeWord(01000, 005000);
        c.reset(01000, 0);
        c.r[6] = 02000;
        c.step();
        CHECK(c.pc() == 01002, "без T-бита ловушки нет");
    }

    // ---- Таймер при включении: непрограммированный счётчик проходит «минус» ----
    // Идиома «ждать таймер» (TST @#177710 / BPL) ждёт знакового бита счётчика.
    {
        Board b; b.reset();
        CHECK(b.peekReg(0177706) == 0177777, "предел таймера при включении — все единицы");
        b.memory().pokeWord(01000, 000777);        // BR .
        b.cpu().reset(01000);
        b.memory().writeWord(0177712, 020 | 004);  // RUN|MON, предел не программировали
        bool negative = false;
        for (int i = 0; i < 64 && !negative; ++i) {
            b.runTicks(128);
            if (b.memory().readWord(0177710) & 0100000) negative = true;
        }
        CHECK(negative, "счётчик непрограммированного таймера читается отрицательным");
    }

    // ---- Динамик: сигнал далеко за Найквистом не сворачивается в слышимую полосу ----
    {
        // Меандр с полупериодом 10 тактов = 150 кГц. RC-цепь пищалки (tau = 55.8 мкс,
        // срез ~2.85 кГц) обязана погасить его примерно в 50 раз ДО выборки. Если
        // фильтровать после децимации (как было), частота сворачивается в полосу и
        // даёт слышимую грязь: тот же тест на прежней реализации давал пик 1178.
        Speaker sp;                       // 44100 Гц, ЦП 3 МГц
        for (int i = 0; i < 60000; ++i) sp.feed((i & 1) ? 7 : 0, 10);
        std::vector<int16_t> buf(sp.available());
        const size_t n = sp.read(buf.data(), buf.size());
        int peak = 0;
        for (size_t i = n / 2; i < n; ++i) { const int a = std::abs((int)buf[i]); if (a > peak) peak = a; }
        CHECK(n > 1000, "динамик выдал сэмплы");
        CHECK(peak < 800, "150 кГц гасится RC-цепью до выборки, а не сворачивается в полосу");
    }

    // ---- Построчная отрисовка: у каждой строки свой скролл ----
    {
        Board b; b.reset();
        for (int i = 0; i < 256; ++i) b.memory().pokeByte(0040000 + i * 64, 0);
        b.memory().pokeByte(0040000 + 5 * 64, 1);   // единственная «светящаяся» строка ВОЗУ = 5
        Screen& s = b.screen();
        s.setColorMode(false);                      // моно: 1 бит на точку, младший слева
        // Смещение = (скролл & 0377) - 0330; экранная строка y показывает ВОЗУ (y+смещение).
        s.renderLine(b.memory(), 0, 01330);         // смещение 0 -> ВОЗУ 0 (пусто)
        s.renderLine(b.memory(), 2, 01333);         // смещение 3 -> ВОЗУ 5 (светится)
        s.renderLine(b.memory(), 3, 01333);         // смещение 3 -> ВОЗУ 6 (пусто)
        const uint32_t* px = s.pixels();
        CHECK((px[2 * Screen::TEX_W] & 0xFFFFFF) != 0, "строка со «своим» скроллом берёт нужную строку ВОЗУ");
        CHECK((px[0 * Screen::TEX_W] & 0xFFFFFF) == 0, "соседняя строка с другим скроллом не задета");
        CHECK((px[3 * Screen::TEX_W] & 0xFFFFFF) == 0, "и следующая тоже");
    }
    {
        // Луч действительно доезжает до низа кадра: 256 видимых строк за кадр.
        Board b; b.reset();
        b.setScanlineRender(true);
        b.memory().pokeWord(01000, 000777);   // BR .
        b.cpu().reset(01000, 0340);
        b.runTicks(b.ticksPerFrame() * 3 / 4);
        CHECK(b.vp037().scanline() >= 230, "к трём четвертям кадра луч дошёл до нижних строк");
    }

    // ---- Экранная кодировка БК -> UTF-8 ----
    {
        CHECK(bkScreenCodeToUtf8(0101) == "A", "0101 -> латинская A");
        CHECK(bkScreenCodeToUtf8(0341) == "А", "0341 -> кириллическая А");
        CHECK(bkScreenCodeToUtf8(0300) == "ю", "0300 -> строчная ю");
        CHECK(bkScreenCodeToUtf8(0377) == "Ъ", "0377 -> Ъ");
        CHECK(bkScreenCodeToUtf8(040) == " ", "040 -> пробел");
        CHECK(bkScreenCodeToUtf8(0241) == "┴", "0241 -> псевдографика");
        CHECK(bkScreenCodeToUtf8(0200) == "{0200}", "код без глифа -> восьмеричная запись");
    }

    // ---- OCR: текст, нарисованный знакогенератором ПЗУ, читается обратно ----
    {
        Board b;
        if (!b.loadRoms(BK_DEFAULT_ROM_DIR)) {
            std::printf("SKIP: ПЗУ не найдено в %s — тесты OCR пропущены\n", BK_DEFAULT_ROM_DIR);
        } else {
            b.reset();
            // Нарисовать строку экранных кодов знакогенератором ПЗУ прямо в ВОЗУ.
            auto glyphAddr = [](uint16_t code) {
                const int idx = (code <= 0177) ? code - 020 : code - 060;
                return static_cast<uint16_t>(BK_FONT_ADDR + idx * BK_FONT_HEIGHT);
            };
            auto draw = [&](const std::vector<uint16_t>& codes, int col0, int row, bool wide) {
                for (size_t i = 0; i < codes.size(); ++i) {
                    const uint16_t ga = glyphAddr(codes[i]);
                    for (int r = 0; r < BK_FONT_HEIGHT; ++r) {
                        const uint8_t m = b.memory().peekByte(static_cast<uint16_t>(ga + r));
                        const int y = row * BK_FONT_HEIGHT + r;
                        if (!wide) {
                            b.memory().pokeByte(static_cast<uint16_t>(0040000 + y * 64 + col0 + (int)i), m);
                        } else {
                            uint16_t w = 0;                    // удвоение битов, как в ПЗУ
                            for (int k = 0; k < 8; ++k) if ((m >> k) & 1) w |= (uint16_t)(3u << (k * 2));
                            const int byteX = (col0 + (int)i) * 2;
                            b.memory().pokeByte(static_cast<uint16_t>(0040000 + y * 64 + byteX), (uint8_t)(w & 0xFF));
                            b.memory().pokeByte(static_cast<uint16_t>(0040000 + y * 64 + byteX + 1), (uint8_t)(w >> 8));
                        }
                    }
                }
            };
            // "БК-0010" заглавной кириллицей + латиница и цифры.
            const std::vector<uint16_t> line = {0342, 0353, 055, 060, 060, 061, 060};  // БК-0010
            const std::vector<uint16_t> lat  = {0110, 0105, 0114, 0114, 0117};          // HELLO

            // --- узкий режим (64 символа в строке)
            draw(line, 3, 4, false);
            draw(lat, 3, 6, false);
            std::vector<uint8_t> bits;
            screenBitmap(b.memory(), b.peekReg(0177664), bits);
            OcrResult r = ocrAuto(b.memory(), bits, OcrOptions{});
            CHECK(!r.opt.wide, "OCR: авто-режим распознал узкий (64 симв.)");
            CHECK(r.opt.y0 == 0, "OCR: авто-смещение по вертикали = 0");
            CHECK(r.unknown == 0 && r.recognised == 12, "OCR узкий: все 12 знакомест распознаны");
            CHECK(r.text().find("БК-0010") != std::string::npos, "OCR узкий: кириллица и цифры");
            CHECK(r.text().find("HELLO") != std::string::npos, "OCR узкий: латиница");

            // --- широкий режим (32 символа в строке, биты глифа удвоены)
            b.memory().reset();
            draw(line, 2, 3, true);
            screenBitmap(b.memory(), b.peekReg(0177664), bits);
            r = ocrAuto(b.memory(), bits, OcrOptions{});
            CHECK(r.opt.wide, "OCR: авто-режим распознал широкий (32 симв.)");
            CHECK(r.unknown == 0 && r.recognised == 7, "OCR широкий: все знакоместа распознаны");
            CHECK(r.text().find("БК-0010") != std::string::npos, "OCR широкий: текст совпал");

            // --- инверсия (курсор): знакоместо с инвертированным глифом
            b.memory().reset();
            draw({0101}, 0, 2, false);                      // латинская A
            for (int y = 20; y < 30; ++y) {                 // инвертировать её знакоместо
                const uint16_t a = static_cast<uint16_t>(0040000 + y * 64);
                b.memory().pokeByte(a, static_cast<uint8_t>(~b.memory().peekByte(a)));
            }
            screenBitmap(b.memory(), b.peekReg(0177664), bits);
            OcrOptions o; o.wide = false; o.y0 = 0;
            r = ocrScreen(b.memory(), bits, o);
            CHECK(r.cells[2 * r.cols].code == 0101 && r.cells[2 * r.cols].inverted,
                  "OCR: инвертированное знакоместо распознано с пометкой");

            // --- пустой экран: ни одного непустого знакоместа, текст пуст
            b.memory().reset();
            screenBitmap(b.memory(), b.peekReg(0177664), bits);
            r = ocrScreen(b.memory(), bits, o);
            CHECK(r.nonBlank == 0 && r.unknown == 0, "OCR: пустой экран — нечего распознавать");
            CHECK(r.text().find_first_not_of('\n') == std::string::npos, "OCR: пустой экран -> пустой текст");
        }
    }

    // ---- Блок расширения памяти СМК-512 («АльтПро») ------------------------
    // Таблица режимов переписана из документации разработчиков (БК-docs,
    // 17-контроллер-СМК.md, Табл. 1) НЕЗАВИСИМО от реализации — это и есть
    // проверка. Вариант «новой» версии контроллера, то есть реплики СМК-512.
    {
        using bk::Smk512;
        bk::Board b;
        CHECK(!b.smk512(), "СМК: по умолчанию плата не установлена");
        b.setSmk512(true);
        b.reset();
        CHECK(b.smk512(), "СМК: плата устанавливается");
        CHECK(b.smk().ram().size() == Smk512::RAM_BYTES, "СМК: 512 Кбайт ДОЗУ");
        CHECK(b.smk().mode() == Smk512::SYS && b.smk().page() == 0,
              "СМК: после включения питания — режим SYS, страница 0");

        auto& ram = b.smk().ram();
        // Каждое слово ДОЗУ равно своему номеру: внутри одной страницы (16384
        // слова) значения уникальны, так что подмену сегмента видно сразу.
        auto seed = [&] {
            for (size_t i = 0; i + 1 < ram.size(); i += 2) {
                const uint16_t w = static_cast<uint16_t>(i / 2);
                ram[i] = static_cast<uint8_t>(w & 0xff);
                ram[i + 1] = static_cast<uint8_t>(w >> 8);
            }
        };
        seed();
        auto sw = [&](Smk512::Mode m, int page) {   // трёхтактное переключение
            b.memory().writeWord(0177130, Smk512::STROBE);
            b.memory().writeWord(0177130,
                static_cast<uint16_t>(Smk512::modeCode(m) | Smk512::pageCode(page)));
            b.memory().writeWord(0177130, 0);
        };
        auto smkWord = [&](int page, int seg, uint16_t inSeg) {
            const size_t o = static_cast<size_t>(page) * Smk512::PAGE_BYTES
                           + static_cast<size_t>(seg) * Smk512::SEG_BYTES + inSeg;
            return static_cast<uint16_t>(ram[o] | (ram[o + 1] << 8));
        };
        auto bkWord = [&](uint16_t a) {              // что по этому адресу у самой БК
            const uint8_t* r = b.memory().raw();
            return static_cast<uint16_t>(r[a] | (r[a + 1] << 8));
        };

        // Блоки: 0100000 0110000 0120000 0130000 0140000 0150000 0160000
        //        0170000 (до 0176777) и 0177000 (страница регистров).
        // seg: 0..7 — номер сегмента, -1 — контроллер молчит, -2 — его ПЗУ.
        // acc: 'x' чтение и запись, 'r' только чтение, 'w' только запись, '-' нет.
        struct Row { Smk512::Mode m; int seg[9]; const char* acc; };
        static const Row kTab1[8] = {
            {Smk512::SYS,   {-1,-1, 6, 7, 0, 1,-2,-2,-2}, "--xxxx---"},
            {Smk512::STD10, {-1,-1, 2, 3, 4, 5,-2, 7,-1}, "--xxxx-x-"},
            {Smk512::RAM10, { 0, 1, 2, 3, 4, 5, 6, 7,-1}, "xxxxxxxx-"},
            {Smk512::ALL,   { 4, 5, 6, 7, 0, 1, 2, 3, 3}, "xxxxxxxxr"},
            {Smk512::STD11, {-1,-1,-1,-1,-1,-1,-2, 7,-1}, "-------x-"},
            {Smk512::RAM11, {-1,-1,-1,-1, 4, 5, 6, 7,-1}, "----xxxx-"},
            {Smk512::HLT10, { 0, 1, 2, 3, 4, 5, 6, 7, 7}, "rxxxxxxxw"},
            {Smk512::HLT11, {-1,-1,-1,-1, 4, 5, 6, 7, 7}, "----xxxxw"},
        };
        static const uint16_t kBase[9] = {0100000, 0110000, 0120000, 0130000,
                                          0140000, 0150000, 0160000, 0170000, 0177000};
        const int kPage = 5;

        // Чтение: ОЗУ контроллера видно в ячейках 'x' и 'r'; в 'w' (теневая
        // запись) и там, где контроллер молчит, читается сама БК.
        int rdOk = 0;
        for (const Row& row : kTab1) {
            sw(row.m, kPage);
            for (int k = 0; k < 9; ++k) {
                const uint16_t a = kBase[k];
                const bool fromSmk = (row.acc[k] == 'x' || row.acc[k] == 'r');
                const uint16_t want = fromSmk ? smkWord(kPage, row.seg[k], a & 07777) : bkWord(a);
                if (b.memory().readWord(a) == want) ++rdOk;
                else std::printf("  СМК чтение: режим %s адрес %06o -> %06o, ждали %06o\n",
                                 Smk512::modeName(row.m), a, b.memory().readWord(a), want);
            }
        }
        CHECK(rdOk == 72, "СМК: чтение по всем 72 ячейкам Табл. 1");

        // Запись: проходит в ОЗУ в ячейках 'x' и 'w'; в 'r' (квази-ПЗУ) и там,
        // где контроллер молчит, ОЗУ остаётся нетронутым.
        int wrOk = 0;
        for (const Row& row : kTab1) {
            sw(row.m, kPage);
            for (int k = 0; k < 9; ++k) {
                const uint16_t a = static_cast<uint16_t>(kBase[k] + 2);
                const bool toSmk = (row.acc[k] == 'x' || row.acc[k] == 'w');
                const uint16_t before = row.seg[k] >= 0 ? smkWord(kPage, row.seg[k], a & 07777) : 0;
                const uint16_t mark = static_cast<uint16_t>(0140000 + row.m * 16 + k);
                b.memory().writeWord(a, mark);
                const uint16_t now = row.seg[k] >= 0 ? smkWord(kPage, row.seg[k], a & 07777) : 0;
                if (now == (toSmk ? mark : before)) ++wrOk;
                else std::printf("  СМК запись: режим %s адрес %06o -> ОЗУ %06o (ждали %06o)\n",
                                 Smk512::modeName(row.m), a, now, toSmk ? mark : before);
            }
        }
        CHECK(wrOk == 72, "СМК: запись по всем 72 ячейкам Табл. 1 (включая 'R' и 'W')");
        seed();

        // Строб. Без него конфигурация не защёлкивается; реплика опознаёт строб
        // только по точному значению 6 в младшей тетраде.
        sw(Smk512::RAM10, 2);
        b.memory().writeWord(0177130, Smk512::modeCode(Smk512::ALL));
        CHECK(b.smk().mode() == Smk512::RAM10 && b.smk().page() == 2,
              "СМК: запись без строба конфигурацию не меняет");
        b.memory().writeWord(0177130, 016);      // разряды 01 и 02 есть, но и 03 тоже
        b.memory().writeWord(0177130, Smk512::modeCode(Smk512::ALL));
        CHECK(b.smk().mode() == Smk512::RAM10, "СМК: реплика требует точного кода строба 6");

        // Восемь кодов включения режима и шестнадцать кодов страниц.
        static const struct { uint16_t code; Smk512::Mode m; } kModes[8] = {
            {0160, Smk512::SYS},   {060, Smk512::STD10}, {0120, Smk512::RAM10}, {020, Smk512::ALL},
            {0140, Smk512::STD11}, {040, Smk512::RAM11}, {0100, Smk512::HLT10}, {0, Smk512::HLT11}};
        int modeOk = 0;
        for (const auto& e : kModes) {
            b.memory().writeWord(0177130, Smk512::STROBE);
            b.memory().writeWord(0177130, e.code);
            if (b.smk().mode() == e.m && Smk512::modeCode(e.m) == e.code) ++modeOk;
        }
        CHECK(modeOk == 8, "СМК: восемь кодов включения режима (160,60,120,20,140,40,100,0)");

        static const uint16_t kPageCodes[16] = {0, 02000, 04, 02004, 010, 02010, 014, 02014,
                                                01, 02001, 05, 02005, 011, 02011, 015, 02015};
        int pageOk = 0;
        for (int p = 0; p < 16; ++p) {
            b.memory().writeWord(0177130, Smk512::STROBE);
            b.memory().writeWord(0177130,
                static_cast<uint16_t>(Smk512::modeCode(Smk512::RAM10) | kPageCodes[p]));
            if (b.smk().page() == p && Smk512::pageCode(p) == kPageCodes[p]) ++pageOk;
        }
        CHECK(pageOk == 16, "СМК: все 16 кодов страниц (0,2000,4,2004,...,2015)");

        // Одновременно подключена ровно одна страница.
        sw(Smk512::RAM10, 1);  b.memory().writeWord(0100000, 012345);
        sw(Smk512::RAM10, 2);  b.memory().writeWord(0100000, 054321);
        sw(Smk512::RAM10, 1);
        CHECK(b.memory().readWord(0100000) == 012345, "СМК: страница 1 не задета записью в страницу 2");
        sw(Smk512::RAM10, 2);
        CHECK(b.memory().readWord(0100000) == 054321, "СМК: страница 2 хранит своё");

        // Байтовая запись в ДОЗУ.
        sw(Smk512::RAM10, 3);
        b.memory().writeWord(0110000, 0);
        b.memory().writeByte(0110001, 0252);
        CHECK(b.memory().readWord(0110000) == 0125000, "СМК: байтовая запись в ДОЗУ");

        // A15 = 0: на нижние 32 Кбайт дешифратор контроллера не включается вовсе.
        b.memory().writeWord(01000, 07777);
        CHECK(bkWord(01000) == 07777, "СМК: адреса ниже 0100000 идут в ОЗУ самой БК");

        // Слово модели и версии — то, по чему программы опознают контроллер.
        sw(Smk512::STD10, 0);
        CHECK(b.memory().readWord(0167776) == 0177605,
              "СМК: 0167776 = 177605 (СМК-512, ПЗУ 2.05) — CMP #176200/BHIS проходит");
        sw(Smk512::RAM10, 0);
        CHECK(b.memory().readWord(0167776) == smkWord(0, 6, 07776),
              "СМК: в режиме ОЗУ10 по 0167776 уже ДОЗУ, а не ПЗУ");

        // Теневая запись (7W): оседает в ОЗУ И доходит до регистра БК, а чтение
        // по-прежнему идёт из настоящего регистра — ради этого режимы Hlt и нужны.
        sw(Smk512::HLT10, 4);
        b.memory().writeWord(0177716, 0100);
        CHECK(smkWord(4, 7, 07716) == 0100, "СМК: Hlt10 — запись в 0177716 осела в теневом ОЗУ");
        CHECK(b.peekRegWritten(0177716) == 0100, "СМК: теневая запись не отменяет запись в регистр БК");
        const uint16_t sysBefore = b.peekReg(0177716);   // чтение регистра не идемпотентно
        CHECK(b.memory().readWord(0177716) == sysBefore,
              "СМК: Hlt10 — чтение 0177716 идёт из регистра БК, а не из ОЗУ");
        // Побочный эффект, о котором предупреждает документация: при выходе из Hlt
        // первые два такта протокола ещё видны теневому ОЗУ, поэтому в нём по адресу
        // 0177130 остаётся код НОВОГО режима — «<20 + код страницы>» для All.
        sw(Smk512::ALL, 4);
        CHECK(smkWord(4, 7, 07130) == static_cast<uint16_t>(Smk512::modeCode(Smk512::ALL)
                                                          | Smk512::pageCode(4)),
              "СМК: при выходе из Hlt10 в теневом ОЗУ по 0177130 остаётся код режима All");

        // Режим All перекрывает страницу регистров по чтению, но в реплике
        // оставлена «дыра» для клавиатуры и скролла.
        sw(Smk512::ALL, 6);
        CHECK(b.memory().readWord(0177000) == smkWord(6, 3, 07000),
              "СМК: All — 0177000 читается из сегмента 3");
        CHECK(b.memory().readWord(0177660) == b.peekReg(0177660), "СМК: All — 0177660 не перекрыт");
        CHECK(b.memory().readWord(0177664) == b.peekReg(0177664), "СМК: All — 0177664 не перекрыт");
        CHECK(b.memory().readWord(0177740) == bkWord(0177740),
              "СМК: регистры НЖМД 0177740..0177757 контроллеру не отданы");

        // Отладочная запись (poke) доходит до ДОЗУ ВОПРЕКИ ограничениям Табл. 1 —
        // на этом держится правка памяти в отладчике: и в ячейке «только чтение»
        // (квази-ПЗУ Hlt10 по 0100000), и в «только запись», которую сам процессор
        // прочитать не может ни в одном режиме (теневая область 0177000).
        sw(Smk512::HLT10, 3);
        b.memory().pokeWord(0100000, 01234);
        CHECK(smkWord(3, 0, 0) == 01234, "СМК: poke пишет в ячейку «только чтение»");
        b.memory().pokeWord(0177674, 07654);
        CHECK(smkWord(3, 7, 07674) == 07654, "СМК: poke пишет в теневую «только запись»");
        b.memory().pokeByte(0100002, 0321);
        CHECK((smkWord(3, 0, 2) & 0377) == 0321, "СМК: байтовый poke пишет в ДОЗУ");

        // Команда RESET дёргает ШИННЫЙ сброс (INIT, контакт Б19) — регистра режима
        // он не касается: в прошивке ПЛИС реплики это отдельный сигнал, а
        // extended_reg обнуляется только по btn_reset с контакта А1 (ОСТ).
        sw(Smk512::ALL, 6);
        b.memory().writeWord(0140000, 07654);      // All: 0140000 -> сегмент 0
        b.memory().pokeWord(01000, 05);            // RESET
        b.cpu().r[7] = 01000;
        b.stepInstruction();
        CHECK(b.smk().mode() == Smk512::ALL && b.smk().page() == 6,
              "СМК: команда RESET режим и страницу не трогает");
        CHECK(smkWord(6, 0, 0) == 07654, "СМК: RESET не стирает ДОЗУ");

        // А «СТОП» — трогает: кнопка дёргает линию ОСТ (контакт А1), и контроллер
        // возвращается в SYS со страницей 0, сохраняя содержимое ДОЗУ.
        b.pressStop();
        CHECK(b.smk().mode() == Smk512::SYS && b.smk().page() == 0,
              "СМК: «СТОП» (ОСТ) -> режим SYS, страница 0");
        CHECK(smkWord(6, 0, 0) == 07654, "СМК: «СТОП» не стирает ДОЗУ");

        // Снимок состояния: конфигурация контроллера и все 512 Кбайт.
        sw(Smk512::HLT11, 9);
        b.memory().writeWord(0140000, 011111);
        std::vector<uint8_t> snap;
        b.saveStateMem(snap);
        sw(Smk512::SYS, 0);
        b.memory().writeWord(0140000, 022222);
        CHECK(b.loadStateMem(snap), "СМК: снимок состояния прочитан");
        CHECK(b.smk().mode() == Smk512::HLT11 && b.smk().page() == 9,
              "СМК: снимок вернул режим и страницу");
        CHECK(b.memory().readWord(0140000) == 011111, "СМК: снимок вернул содержимое ДОЗУ");

        // Снятие платы возвращает штатную карту памяти БК.
        b.setSmk512(false);
        CHECK(!b.smk512(), "СМК: плату можно снять");
        CHECK(b.memory().readWord(0120000) == bkWord(0120000),
              "СМК: без платы по 0120000 снова ПЗУ Бейсика");
    }

    // ---- Сквозной тест СМК: сторонний SMKTEST.bin -------------------------
    // tests/data/SMKTEST.bin — тест из комплекта Gryphon-MPI (SD/BK_Test), не наш.
    // Он заливает своим узором и посегментно сверяет все 16 страниц ДОЗУ в режимах
    // ОЗУ10 и All, печатая отчёт на экран. Это проверка совсем другого сорта, чем
    // таблица выше: там мы сверяем реализацию с документацией, здесь — с чужой
    // программой, написанной под живое железо. Результат читается с экрана тем же
    // OCR, что и в MCP (bk_ocr).
    {
        Board probe;
        const std::string binPath = std::string(BK_TEST_DATA_DIR) + "/SMKTEST.bin";
        bool haveBin = false;
        if (std::FILE* f = std::fopen(binPath.c_str(), "rb")) { haveBin = true; std::fclose(f); }
        if (!haveBin) {
            std::printf("SKIP: нет %s — сквозной тест СМК пропущен\n", binPath.c_str());
        } else if (!probe.loadRoms(BK_DEFAULT_ROM_DIR)) {
            std::printf("SKIP: ПЗУ не найдено в %s — сквозной тест СМК пропущен\n", BK_DEFAULT_ROM_DIR);
        } else {
            // Итоговая строка теста — «Все ОК!»: в ней ВСЕ буквы омоглифы (В с е
            // О К), и к какому алфавиту их отнесёт разбор большинства по строке —
            // дело вкуса реализации. Принимаем обе записи, чтобы тест ловил
            // регрессию памяти, а не смену правила разрешения омоглифов.
            auto reportedOk = [](const std::string& t) {
                return t.find("Все") != std::string::npos || t.find("Bce") != std::string::npos;
            };
            // Прогнать тест и вернуть текст с экрана. Крутим кадры порциями и
            // выходим, как только тест отчитался, — полный прогон это 4000 кадров.
            auto runTest = [&](bool withSmk) {
                Board brd;
                brd.setSmk512(withSmk);
                brd.loadRoms(BK_DEFAULT_ROM_DIR);
                brd.reset();
                for (int i = 0; i < 25; ++i) brd.runFrame();   // монитор поднимает вектора
                if (!brd.loadBin(binPath, true)) return std::string();
                std::string text;
                std::vector<uint8_t> bits;
                for (int done = 0; done < 4000; done += 250) {
                    for (int i = 0; i < 250; ++i) brd.runFrame();
                    screenBitmap(brd.memory(), brd.peekReg(0177664), bits);
                    // Знакогенератор берём из ЧУЖОЙ памяти с чистым ПЗУ: тест
                    // работает в режимах ОЗУ10 и All, где ДОЗУ платы накрывает и
                    // 0112036 — таблицу глифов. По своей памяти OCR прочёл бы мусор.
                    text = ocrAuto(probe.memory(), bits, OcrOptions{}).text();
                    if (reportedOk(text) || text.find("ОШИБКА") != std::string::npos) break;
                }
                return text;
            };

            const std::string withBoard = runTest(true);
            CHECK(withBoard.find("ОШИБКА") == std::string::npos,
                  "СМК/SMKTEST: с платой ни одной ошибки сверки");
            CHECK(reportedOk(withBoard), "СМК/SMKTEST: с платой тест дошёл до «Все ОК!»");

            // Зеркальная проверка: без платы тест обязан споткнуться на первом же
            // сегменте — иначе он не проверяет ничего и «успех» выше ничего не стоит.
            const std::string noBoard = runTest(false);
            CHECK(noBoard.find("ОШИБКА") != std::string::npos,
                  "СМК/SMKTEST: без платы тест сообщает об ошибке");
        }
    }

    // ---- Контроллер НГМД: поток дорожки и протокол регистров ----------------
    // Микросхема 1801ВП1-128 не ищет сектора — она отдаёт СЫРОЙ поток дорожки и
    // ловит маркер A1. Поэтому проверяем именно то, что видит драйвер: после
    // поиска маркера должно прийти A1A1, затем признак поля (A1FE или A1FB), а
    // за полем идентификатора — дорожка, сторона, номер сектора и код длины.
    {
        using bk::Fdd;
        CHECK(Fdd::looksLikeImage("disk.bkd") && Fdd::looksLikeImage("DISK.BKD"),
              "НГМД: .bkd опознаётся в любом регистре");
        CHECK(Fdd::looksLikeImage("a.img") && Fdd::looksLikeImage("ANDOS.IMG"),
              "НГМД: .img опознаётся в любом регистре");
        CHECK(!Fdd::looksLikeImage("game.bin") && !Fdd::looksLikeImage("noext"),
              "НГМД: посторонние расширения не опознаются");

        // Синтетический образ: 2 дорожки x 2 стороны x 10 секторов. Первое слово
        // каждого сектора — его координаты, дальше — счётчик, чтобы поймать сдвиг.
        const int kTracks = 2;
        std::vector<uint8_t> img(static_cast<size_t>(kTracks) * 2 * Fdd::TRACK_BYTES, 0);
        auto put = [&](int t, int side, int sec, int off, uint16_t v) {
            const size_t o = ((static_cast<size_t>(t) * 2 + side) * Fdd::SECTORS + (sec - 1))
                           * Fdd::SECTOR_SIZE + off;
            img[o] = static_cast<uint8_t>(v >> 8);      // старший байт первым — как в потоке
            img[o + 1] = static_cast<uint8_t>(v & 0xff);
        };
        for (int t = 0; t < kTracks; ++t)
            for (int s = 0; s < 2; ++s)
                for (int sec = 1; sec <= Fdd::SECTORS; ++sec) {
                    put(t, s, sec, 0, static_cast<uint16_t>((t << 12) | (s << 8) | sec));
                    for (int w = 1; w < 8; ++w) put(t, s, sec, w * 2, static_cast<uint16_t>(0100 + w));
                }
        const std::string imgPath = (std::filesystem::temp_directory_path()
                                     / "bk-fdd-test.img").string();
        { std::FILE* f = std::fopen(imgPath.c_str(), "wb");
          if (f) { std::fwrite(img.data(), 1, img.size(), f); std::fclose(f); } }

        Fdd fdd;
        CHECK(fdd.attach(0, imgPath), "НГМД: образ вставлен в привод 0");
        CHECK(fdd.attached(0) && !fdd.attached(1), "НГМД: занят только привод 0");

        // Крутим диск, пока не появится готовность данных, и забираем слово.
        auto nextWord = [&](int limit = 8000) -> uint16_t {
            for (int i = 0; i < limit; ++i) {
                fdd.periodic();
                if (fdd.readStatus() & Fdd::ST_TR) return fdd.readData();
            }
            return 0177777;   // не дождались
        };
        // Выбрать привод 0 и включить двигатель: без него диск не вращается.
        fdd.writeCtrl(1 | Fdd::CMD_MOTOR);
        CHECK((fdd.readStatus() & Fdd::ST_TRACK0) != 0, "НГМД: головка на нулевой дорожке");
        CHECK((fdd.readStatus() & Fdd::ST_RDY) != 0, "НГМД: с включённым двигателем привод готов");
        CHECK((fdd.readStatus() & Fdd::ST_WPROT) != 0, "НГМД: образ вставлен только на чтение");

        // Поиск маркера — двухтактный: единица в GDR, затем ноль (см. Fdd::writeCtrl).
        auto search = [&] {
            fdd.writeCtrl(1 | Fdd::CMD_MOTOR | Fdd::CMD_GDR);
            fdd.writeCtrl(1 | Fdd::CMD_MOTOR);
        };
        // Ищем поле идентификатора: маркер, признак FE, координаты сектора.
        uint16_t id[4] = {0, 0, 0, 0};
        bool foundId = false;
        for (int tries = 0; tries < 30 && !foundId; ++tries) {
            search();
            id[0] = nextWord();
            id[1] = nextWord();
            if (id[0] == 0xA1A1 && id[1] == 0xA1FE) {
                id[2] = nextWord(); id[3] = nextWord();
                foundId = true;
            }
        }
        CHECK(foundId, "НГМД: после поиска приходит маркер A1A1 и признак поля A1FE");
        CHECK((id[2] >> 8) == 0 && (id[2] & 0xff) == 0,
              "НГМД: в поле идентификатора дорожка 0, сторона 0");
        const int sector = id[3] >> 8;
        CHECK(sector >= 1 && sector <= Fdd::SECTORS && (id[3] & 0xff) == 2,
              "НГМД: номер сектора в диапазоне 1..10, код длины 2 (512 байт)");

        // Следом идёт поле данных того же сектора: маркер, признак FB и данные.
        uint16_t d0 = 0, d1 = 0, d2 = 0;
        bool foundData = false;
        for (int tries = 0; tries < 30 && !foundData; ++tries) {
            search();
            d0 = nextWord();
            d1 = nextWord();
            if (d0 == 0xA1A1 && d1 == 0xA1FB) { d2 = nextWord(); foundData = true; }
        }
        CHECK(foundData, "НГМД: поле данных начинается маркером A1A1 и признаком A1FB");
        CHECK((d2 & 0xf000) == 0 && (d2 & 0xff) >= 1 && (d2 & 0xff) <= Fdd::SECTORS,
              "НГМД: первое слово данных — координаты сектора из образа");

        // Шаг головки: DIR=1 уводит от нулевой дорожки, DIR=0 возвращает.
        fdd.writeCtrl(1 | Fdd::CMD_MOTOR | Fdd::CMD_DIR | Fdd::CMD_STEP);
        CHECK(fdd.track() == 1 && !(fdd.readStatus() & Fdd::ST_TRACK0),
              "НГМД: шаг к центру уводит с нулевой дорожки");
        fdd.writeCtrl(1 | Fdd::CMD_MOTOR | Fdd::CMD_STEP);
        CHECK(fdd.track() == 0 && (fdd.readStatus() & Fdd::ST_TRACK0),
              "НГМД: шаг наружу возвращает на нулевую");
        // Снятие привода: контроллер молчит, диск не крутится.
        fdd.writeCtrl(0);
        CHECK(fdd.readStatus() == 0, "НГМД: без выбранного привода состояние нулевое");
        std::filesystem::remove(imgPath);
    }

    // ---- «Зависание»: запись в регистр, который её не принимает --------------
    // Регистр данных клавиатуры 0177662 на БК-0010 доступен только по чтению.
    // Запись шина не подтверждает, и процессор идёт по вектору 4 (§7.4). Этим
    // прошивка дискового контроллера отличает БК-0010 от БК-0011М, где по этому
    // адресу лежит регистр управления экраном: без ловушки она считает машину
    // одиннадцатой, ставит негодный режим памяти, и ни ANDOS, ни игры с дискет
    // не поднимаются.
    {
        Board b;
        b.reset();
        b.memory().pokeWord(Cpu::VEC_BUS_ERROR, 03000);      // обработчик «зависания»
        b.memory().pokeWord(Cpu::VEC_BUS_ERROR + 2, 0);
        b.memory().pokeWord(01000, 012737);                  // MOV #40000,@#177662
        b.memory().pokeWord(01002, 040000);
        b.memory().pokeWord(01004, 0177662);
        b.memory().pokeWord(03000, 000777);                  // BR . — обработчик
        b.cpu().reset(01000, 0);
        b.stepInstruction();
        b.stepInstruction();                                 // запрос разбирается после команды
        CHECK(b.cpu().pc() == 03000, "зависание: запись в 0177662 уводит на вектор 4");

        // Без установленного вектора прерывать некуда — запрос просто гаснет.
        Board c;
        c.reset();
        c.memory().pokeWord(Cpu::VEC_BUS_ERROR, 0);
        c.memory().pokeWord(01000, 012737);
        c.memory().pokeWord(01002, 040000);
        c.memory().pokeWord(01004, 0177662);
        c.memory().pokeWord(01006, 000777);
        c.cpu().reset(01000, 0);
        c.stepInstruction();
        c.stepInstruction();
        CHECK(c.cpu().pc() == 01006, "зависание: без вектора 4 запрос гасится");

        // Запись в исправный регистр (скролл) ничего не ломает.
        Board d;
        d.reset();
        d.memory().pokeWord(Cpu::VEC_BUS_ERROR, 03000);
        d.memory().pokeWord(01000, 012737);
        d.memory().pokeWord(01002, 01330);
        d.memory().pokeWord(01004, 0177664);
        d.memory().pokeWord(01006, 000777);
        d.cpu().reset(01000, 0);
        d.stepInstruction();
        d.stepInstruction();
        CHECK(d.cpu().pc() == 01006, "зависания нет там, где регистр запись принимает");
    }

    // ---- Шина МПИ: блок расширения памяти и дисковод вместе ------------------
    // На живой машине это одна плата, поэтому регистры 0177130/0177132 общие, а
    // окно 0160000 отдаётся ПЗУ драйвера только тогда, когда его не занял ДОЗУ.
    {
        Board b;
        if (!b.loadRoms(BK_DEFAULT_ROM_DIR)) {
            std::printf("SKIP: ПЗУ не найдено — тесты шины МПИ пропущены\n");
        } else {
            b.setSmk512(true);
            b.reset();
            const std::string img = (std::filesystem::temp_directory_path()
                                     / "bk-mux-test.img").string();
            { std::vector<uint8_t> z(2 * 2 * bk::Fdd::TRACK_BYTES, 0);
              std::FILE* f = std::fopen(img.c_str(), "wb");
              if (f) { std::fwrite(z.data(), 1, z.size(), f); std::fclose(f); } }
            const bool ok = b.attachDisk(0, img);
            CHECK(ok, "МПИ: диск вставляется при установленной плате СМК");
            CHECK(b.smk512() && b.diskControllerOn(),
                  "МПИ: СМК и контроллер НГМД на шине одновременно");

            // Режим SYS: 0120000 — ДОЗУ платы, 0160000 — ПЗУ драйвера НГМД.
            b.memory().writeWord(0120000, 01234);
            CHECK(b.memory().readWord(0120000) == 01234, "МПИ: 0120000 — ОЗУ платы СМК");
            const uint16_t romWord = b.memory().readWord(0160000);
            CHECK(romWord == 0410, "МПИ: 0160000 — ПЗУ драйвера НГМД (BR на автозагрузку)");

            // Запись в 0177130 видят ОБА: СМК защёлкивает режим, дисковод — команду.
            b.memory().writeWord(0177130, bk::Smk512::STROBE);
            b.memory().writeWord(0177130, bk::Smk512::modeCode(bk::Smk512::RAM10));
            b.memory().writeWord(0177130, 0);
            CHECK(b.smk().mode() == bk::Smk512::RAM10, "МПИ: СМК защёлкнул режим через общий регистр");
            // В режиме ОЗУ10 ДОЗУ накрывает и окно 0160000 — ПЗУ драйвера уходит.
            b.memory().writeWord(0160000, 07070);
            CHECK(b.memory().readWord(0160000) == 07070,
                  "МПИ: в режиме ОЗУ10 по 0160000 уже ОЗУ платы, а не ПЗУ");
            // Регистры контроллера остаются за микросхемой НГМД в любом режиме.
            b.memory().writeWord(0177130, 1 | bk::Fdd::CMD_MOTOR);
            CHECK((b.memory().readWord(0177130) & bk::Fdd::ST_RDY) != 0,
                  "МПИ: состояние 0177130 отдаёт дисковод, а не ДОЗУ");

            // Переключение страниц НЕ ДОЛЖНО сбивать дисковод. На время трёх тактов
            // протокола плата блокирует регистры НГМД (§17.5), иначе строб «6»
            // дошёл бы до микросхемы как «выбрать привод 1», а завершающий «MOV #0»
            // — как «снять выбор и выключить двигатель», и обмен, идущий в этот
            // момент, оборвался бы. На этом вставала ANDOS с диска LAND.bkd: она
            // меняет страницы, пока драйвер читает сектор.
            const int driveBefore = b.kngmd().fdd().drive();
            const bool motorBefore = b.kngmd().fdd().motor();
            b.memory().writeWord(0177130, bk::Smk512::STROBE);
            CHECK(b.kngmd().fdd().drive() == driveBefore && b.kngmd().fdd().motor() == motorBefore,
                  "МПИ: строб СМК не трогает выбор привода и двигатель");
            b.memory().writeWord(0177130, bk::Smk512::modeCode(bk::Smk512::SYS));
            b.memory().writeWord(0177130, 0);
            CHECK(b.smk().mode() == bk::Smk512::SYS, "МПИ: режим всё-таки защёлкнулся");
            CHECK(b.kngmd().fdd().drive() == driveBefore && b.kngmd().fdd().motor() == motorBefore,
                  "МПИ: после всего протокола дисковод остался в прежнем состоянии");
            // А обычная команда после протокола до дисковода доходит.
            b.memory().writeWord(0177130, 0);
            CHECK(b.kngmd().fdd().drive() == -1, "МПИ: команда вне протокола снимает выбор привода");
            std::filesystem::remove(img);
        }
    }

    std::printf("\n%d/%d checks passed\n", g_total - g_fail, g_total);
    return g_fail ? 1 : 0;
}
