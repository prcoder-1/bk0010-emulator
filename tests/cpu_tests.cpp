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

    // ---- Keyboard: single-code register with ready/drop semantics ----
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
        CHECK(!b.keyReady(), "keyboard: ready flag cleared on read");
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

        // Снимок старого формата (без хвоста ext[2]) должен читаться как «ввод отпущен».
        std::vector<uint8_t> old(snap.begin(), snap.end() - 4);
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
        b.memory().writeWord(0177716, 0100);   // бит 6 -> 1
        b.memory().writeWord(0177716, 0100);   // повтор — не фронт
        b.memory().writeWord(0177716, 0);      // бит 6 -> 0
        const auto& sl = b.speakerLog();
        CHECK(sl.size() == 2, "пишутся только фронты");
        if (sl.size() == 2) CHECK(sl[0].level == 1 && sl[1].level == 0, "уровни фронтов");
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

    std::printf("\n%d/%d checks passed\n", g_total - g_fail, g_total);
    return g_fail ? 1 : 0;
}
