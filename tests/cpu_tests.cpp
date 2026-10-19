// Minimal unit tests for the BK-0010 CPU core.
// Loads short instruction sequences into RAM and checks register/flag results.
#include "Cpu.h"
#include "Memory.h"
#include "Disasm.h"
#include "Instr.h"
#include "Board.h"
#include "Vp037.h"
#include "Joystick.h"
#include "BkKeys.h"
#include "ScreenOcr.h"
#include "Speaker.h"
#include "Fdd.h"
#include "Ay8910.h"
#include "Zmbv.h"
#include "VideoCapture.h"
#include "VideoSink.h"
#ifdef BK_HAVE_FFMPEG
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>
}
#endif
#include <zlib.h>
#include <fstream>
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include <filesystem>
#include <csignal>
#include <unistd.h>

using namespace bk;

static int g_fail = 0;
static int g_total = 0;

#define CHECK(cond, msg) do { \
    ++g_total; \
    if (!(cond)) { std::printf("FAIL: [%s] %s (line %d)\n", g_section, msg, __LINE__); ++g_fail; } \
} while (0)

// Разделы тестов. `cpu_tests <подстрока>…` гоняет только разделы с такими словами в
// названии (код вне разделов выполняется всегда), а при падении процесса видно, в каком
// разделе оно случилось.
static const char* g_section = "";
static std::vector<std::string> g_filters;
static bool beginSection(const char* name) {
    g_section = name;
    if (g_filters.empty()) return true;
    for (const std::string& f : g_filters)
        if (std::string(name).find(f) != std::string::npos) return true;
    return false;
}
#define SECTION(name) if (!beginSection(name)) {} else

static void onCrash(int sig) {
    const char head[] = "\nCRASH (signal) in section: ";
    (void)!write(2, head, sizeof head - 1);
    (void)!write(2, g_section, std::strlen(g_section));
    (void)!write(2, "\n", 1);
    std::signal(sig, SIG_DFL);
    std::raise(sig);
}

// Helper: assemble a program of raw words into RAM at addr, reset PC there.
static void loadProg(Memory& m, uint16_t addr, std::initializer_list<uint16_t> words) {
    uint16_t a = addr;
    for (uint16_t w : words) { m.pokeWord(a, w); a += 2; }
}

int main(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) g_filters.push_back(argv[i]);
    for (int sig : {SIGSEGV, SIGABRT, SIGFPE, SIGILL}) std::signal(sig, onCrash);
    // ---- MOV immediate ----
    SECTION("MOV immediate")
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
    SECTION("ADD with carry/overflow")
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
    SECTION("SUB / CMP flags")
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
    SECTION("INC / DEC")
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
    SECTION("Rotate / shift")
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
    SECTION("Branch taken (BEQ)")
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
    SECTION("JSR / RTS")
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
    SECTION("SOB loop")
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
    SECTION("Disassembler smoke test")
    {
        Memory m;
        m.pokeWord(01000, 012700); m.pokeWord(01002, 0123456); // MOV #123456,R0
        DisasmLine d = disasm(m, 01000);
        CHECK(d.words == 2, "MOV #imm,R0 is 2 words");
        CHECK(d.text.rfind("MOV", 0) == 0, "disasm mnemonic MOV");
        std::printf("disasm: %s\n", d.text.c_str());
    }

    // ---- Разбор команды (Instr.h): длина совпадает с дизассемблером, цели переходов ----
    SECTION("Разбор команды (Instr.h): длина совпадает с дизассемблером, цели переходов")
    {
        Memory m;
        int bad = -1;
        for (uint32_t ir = 0; ir <= 0177777 && bad < 0; ++ir) {
            m.pokeWord(01000, static_cast<uint16_t>(ir));
            m.pokeWord(01002, 0123); m.pokeWord(01004, 0456);
            if (instrInfo(m, 01000).words != disasm(m, 01000).words) bad = static_cast<int>(ir);
        }
        CHECK(bad < 0, "instrInfo: длина команды = disasm для всех 65536 кодов");
        loadProg(m, 01000, {0000403, 0077005, 0004737, 02000, 0000167, 010, 0104035, 0000207});
        const InstrInfo br = instrInfo(m, 01000), sob = instrInfo(m, 01002), jsr = instrInfo(m, 01004),
                        jmp = instrInfo(m, 01010), emt = instrInfo(m, 01014), rts = instrInfo(m, 01016);
        CHECK(br.flow == Flow::Jump && br.target == 01010 && sob.flow == Flow::Branch && sob.target == 0772
              && jsr.flow == Flow::Call && jsr.words == 2 && jsr.target == 02000
              && jmp.flow == Flow::Jump && jmp.target == 01024 && emt.flow == Flow::Trap && !emt.hasTarget
              && rts.flow == Flow::Return,
              "instrInfo: BR, SOB, JSR @#, JMP a(PC), EMT, RTS — вид и цель");
    }

    // ---- Save / restore state round-trip ----
    SECTION("Save / restore state round-trip")
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
    SECTION("HALT traps through vector 0004 (BK-0010), not a permanent stop")
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
    SECTION("1801VM1 programmable timer (0177706/0177710/0177712)")
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
    SECTION("Keyboard: single-code register, new press OVERWRITES an unread code")
    {
        Board b;
        b.reset();
        CHECK(!b.keyReady(), "keyboard: no key ready initially");
        b.pressKey(0101);   // 'A'
        CHECK(b.keyReady(), "keyboard: ready flag set after keypress");
        // Реальная ВП1-014 (как bk/tty.c): новое нажатие ПЕРЕЗАПИСЫВАЕТ регистр,
        // даже если старый код не прочитан. Игры (PITON) читают 0177662 по биту
        // «клавиша нажата» и должны видеть код ПОСЛЕДНЕЙ клавиши, а не застрявший.
        b.pressKey(0102);
        CHECK((b.memory().readWord(0177662) & 0177) == 0102, "keyboard: register holds LATEST code");
        // Флаг готовности гаснет НЕ мгновенно, а через ~900 тактов после чтения кода
        // (ВП1-014). Сразу после чтения он ещё стоит.
        CHECK(b.keyReady(), "keyboard: флаг готовности ещё стоит сразу после чтения");
        b.memory().pokeWord(01000, 000777);   // BR . — крутимся на месте
        b.cpu().reset(01000);
        b.runTicks(1000);
        CHECK(!b.keyReady(), "keyboard: флаг готовности гаснет через ~900 тактов");
        b.pressKey(0103);
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

    // ---- АР2: прерывание по вектору 0274, в 0177662 — 7-битный код ----------
    // Обработчик монитора 0101362 сам выставляет бит 7 и переводит код в «нижний
    // регистр»: АР2+; (0273) становится командой 0233 «32/64 символа».
    SECTION("АР2: прерывание по вектору 0274, в 0177662 — 7-битный код")
    {
        Board b;
        if (!b.loadRoms(BK_DEFAULT_ROM_DIR)) {
            std::printf("SKIP: ПЗУ не найдено — тест АР2 пропущен\n");
        } else {
            b.reset();
            for (int i = 0; i < 60; ++i) b.runFrame();
            CHECK(b.memory().peekWord(0274) == 0101362, "АР2: вектор 0274 поставлен монитором");
            b.pressKey(bkAr2Key("ар2+;"));
            CHECK((b.peekReg(0177662) & 0177) == 073, "АР2: в 0177662 обычный код ';'");
            bool hit = false;
            for (int i = 0; i < 200000 && !hit; ++i) {
                b.stepInstruction();
                hit = b.cpu().r[7] == 0101362;
            }
            CHECK(hit, "АР2: прерывание пришло в обработчик монитора 0101362");
        }
    }

    // ---- Клавиатура: отсрочка прерывания и его отмена по прочтению кода ----
    // Прерывание клавиатуры выдаётся НЕ мгновенно: игра, которая его разрешила и
    // при этом сама опрашивает регистр (SOKOBAN: CLR 177660, затем TST/CMPB),
    // должна успеть прочитать код раньше ISR монитора. Отсрочка — четверть кадра.
    SECTION("Клавиатура: отсрочка прерывания и его отмена по прочтению кода")
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
    SECTION("Vp037: геометрия развёртки (порт va_037.v)")
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

    // ---- Модель шины ВМ1 + окно 037 (Vm1Timing.h, Board::busStep) ----
    SECTION("Модель шины ВМ1 + окно 037 (Vm1Timing.h, Board::busStep)")
    {
        // Эталоны — тесты XOP2/XOP2N на реальной БК-0010-01 пользователя (MOV R0 -> R1
        // по всем режимам; строка — режим источника, столбец — режим приёмника) и
        // RTL-модель ВМ1 + 037. Время команды зависит от соседей, поэтому меряем, как
        // сами тесты: 48 копий подряд, средний такт на копию на установившемся участке.
        // Память 010000..037777 заполнена указателем на 010000, R0 = 024000,
        // R1 = 034000, R3 = 007000 (там RTS PC), вектор EMT — на RTI по 007100.
        auto clone = [](std::initializer_list<uint16_t> unit, int phase, uint16_t r1 = 034000) {
            Board b; b.setCpuPhase(phase); b.loadRoms(BK_DEFAULT_ROM_DIR); b.reset();
            Memory& m = b.memory();
            for (uint16_t a = 010000; a < 040000; a += 2) m.pokeWord(a, 010000);
            m.pokeWord(030, 07100); m.pokeWord(032, 0);
            m.pokeWord(07000, 0000207); m.pokeWord(07100, 0000002);
            const uint16_t start = 01000, stride = static_cast<uint16_t>(2 * unit.size());
            uint16_t a = start;
            for (int k = 0; k < 48; ++k) for (uint16_t w : unit) { m.pokeWord(a, w); a += 2; }
            m.pokeWord(a, 0000777);
            b.cpu().reset(start, 0340);
            b.cpu().r[0] = 024000; b.cpu().r[1] = r1; b.cpu().r[3] = 07000; b.cpu().r[6] = 01000;
            uint64_t t0 = 0;
            for (int i = 0; i < 20000; ++i) {
                const uint16_t pc = b.cpu().r[7];
                if (pc == start + 12 * stride) t0 = b.totalTicks();
                if (pc == start + 44 * stride) return double(b.totalTicks() - t0) / 32;
                b.stepInstruction();
            }
            return -1.0;
        };
        // Слово MOV R0 -> R1 с режимами sm/dm и словами индекса (0) после него.
        auto movUnit = [](uint16_t base, int sm, int dm, bool nop) {
            std::vector<uint16_t> u{static_cast<uint16_t>(base | (sm << 9) | (dm << 3) | 1)};
            if (sm >= 6) u.push_back(0);
            if (dm >= 6) u.push_back(0);
            if (nop) u.push_back(0000240);
            return u;
        };
        auto cloneV = [&](const std::vector<uint16_t>& u, int phase) {
            switch (u.size()) {
            case 1: return clone({u[0]}, phase);
            case 2: return clone({u[0], u[1]}, phase);
            case 3: return clone({u[0], u[1], u[2]}, phase);
            default: return clone({u[0], u[1], u[2], u[3]}, phase);
            }
        };
        // XOP2, фаза 0 (по умолчанию) — первое фото; фаза 1 — второе.
        static const int XOP2_P0[8][8] = {
            {12, 32, 36, 44, 36, 44, 44, 56}, {28, 40, 40, 48, 40, 48, 48, 60},
            {28, 40, 40, 48, 40, 48, 48, 60}, {40, 52, 52, 60, 52, 60, 60, 72},
            {28, 44, 44, 52, 44, 52, 52, 64}, {40, 56, 56, 64, 56, 64, 64, 76},
            {40, 52, 52, 60, 52, 60, 60, 72}, {52, 64, 64, 72, 64, 72, 72, 84},
        };
        static const int XOP2_P1[8][8] = {
            {12, 32, 36, 44, 36, 44, 44, 52}, {24, 40, 40, 48, 40, 48, 48, 56},
            {24, 40, 40, 48, 40, 48, 48, 56}, {32, 48, 48, 56, 48, 56, 56, 64},
            {28, 40, 40, 48, 40, 48, 48, 56}, {36, 48, 48, 56, 48, 56, 56, 64},
            {32, 48, 48, 56, 48, 56, 56, 64}, {40, 56, 56, 64, 56, 64, 64, 72},
        };
        // XOP2N: после каждой копии — NOP (фаза 0, фото).
        static const int XOP2N_P0[8][8] = {
            {24, 48, 48, 56, 48, 56, 56, 68}, {40, 56, 56, 64, 56, 64, 64, 76},
            {40, 56, 56, 64, 56, 64, 64, 76}, {52, 68, 68, 76, 68, 76, 76, 88},
            {40, 56, 56, 64, 56, 64, 64, 76}, {52, 68, 68, 76, 68, 76, 76, 88},
            {52, 68, 68, 76, 68, 76, 76, 88}, {64, 80, 80, 88, 80, 88, 88, 100},
        };
        struct Table { const int (*t)[8]; uint16_t base; bool nop; int phase; const char* name; };
        const Table tables[] = {
            {XOP2_P0,  0010000, false, 0, "037: XOP2 MOV, фаза 0 — первое фото БК-0010-01"},
            {XOP2_P0,  0110000, false, 0, "037: XOP2 MOVB, фаза 0 — те же числа"},
            {XOP2_P1,  0010000, false, 1, "037: XOP2 MOV, фаза 1 — второе фото"},
            {XOP2N_P0, 0010000, true,  0, "037: XOP2N (MOV + NOP), фаза 0 — фото"},
        };
        for (const Table& tb : tables) {
            int bad = 0;
            for (int sm = 0; sm < 8; ++sm)
                for (int dm = 0; dm < 8; ++dm) {
                    const double t = cloneV(movUnit(tb.base, sm, dm, tb.nop), tb.phase);
                    if (t != tb.t[sm][dm]) {
                        if (bad < 4) std::printf("  %s: %o0,%o1 = %g, на железе %d\n", tb.name, sm, dm, t, tb.t[sm][dm]);
                        ++bad;
                    }
                }
            CHECK(bad == 0, tb.name);
        }
        // Остальное — по RTL-модели в фазе 0.
        struct Case { std::initializer_list<uint16_t> unit; double ticks; const char* name; };
        const Case cases[] = {
            {{0005011},                 28, "CLR (R1)"},       // запись окно не теряет
            {{0005221},                 28, "INC (R1)+"},
            {{0005337, 010000},         40, "DEC @#10000"},
            {{0005051},                 44, "CLR @-(R1)"},
            {{0064011},                 44, "ADD -(R0),(R1)"},
            {{0024011},                 40, "CMP -(R0),(R1)"},
            {{0014001},                 28, "MOV -(R0),R1"},
            {{0005711},                 24, "TST (R1)"},
            {{0005741},                 28, "TST -(R1)"},
            {{0074011},                 32, "XOR R0,(R1)"},
            {{0000311},                 28, "SWAB (R1)"},
            {{0077400 | 000},           20, "SOB R4,.+2"},
            {{0000400},                 16, "BR .+2"},
            {{0000240},                 12, "NOP"},
            {{0004713},                 64, "JSR PC,(R3) + RTS PC"},
            {{0104000},                104, "EMT + RTI"},
            // Выше 0100000 037 не обслуживает: В-В отвечает сразу (eCat3).
            {{0005337, 0177716},        32, "DEC @#177716 (В-В)"},
        };
        for (const Case& c : cases) {
            char msg[96];
            std::snprintf(msg, sizeof msg, "037: %s — %g тактов, код в ДОЗУ", c.name, c.ticks);
            const double t = clone(c.unit, 0);
            if (t != c.ticks) std::printf("  %s = %g\n", c.name, t);
                        CHECK(t == c.ticks, msg);
        }
        // tests/hw/ROMIO.BIN на реальной БК-0010-01 пользователя, по два включения
        // питания в каждой фазе (2026-10-07): В-В отвечает сразу, ПЗУ — с задержкой,
        // которая видна только в фазе 1.
        struct Hw { std::initializer_list<uint16_t> unit; uint16_t r1; int p0, p1; const char* name; };
        const Hw hw[] = {
            {{0005737, 001000},  034000, 36, 32, "TST @#1000"},
            {{0005737, 0100000}, 034000, 32, 32, "TST @#100000 (ПЗУ)"},
            {{0005737, 0177716}, 034000, 32, 28, "TST @#177716 (В-В)"},
            {{0013701, 0100000}, 034000, 36, 32, "MOV @#100000,R1 (ПЗУ)"},
            {{0010037, 0177714}, 034000, 40, 40, "MOV R0,@#177714 (В-В)"},
            {{0005037, 0177714}, 034000, 32, 32, "CLR @#177714 (В-В)"},
            {{0030037, 0177716}, 034000, 40, 36, "BIT R0,@#177716 (В-В)"},
            {{0111100},          0100000, 24, 24, "MOVB (R1),R0, R1 в ПЗУ"},
            {{0111100},          001000, 28, 24, "MOVB (R1),R0, R1 в ДОЗУ"},
            {{0004737, 0100256}, 034000, 68, 68, "JSR PC,@#100256 (RTS PC в ПЗУ)"},
        };
        for (const Hw& c : hw)
            for (int ph = 0; ph < 2; ++ph) {
                char msg[112];
                const int want = ph ? c.p1 : c.p0;
                std::snprintf(msg, sizeof msg, "ROMIO, фаза %d: %s — %d тактов, как на железе", ph, c.name, want);
                const double t = clone(c.unit, ph, c.r1);
                if (t != want) std::printf("  %s, фаза %d = %g\n", c.name, ph, t);
                CHECK(t == want, msg);
            }
        // tests/hw/ROMSOB.BIN: петля `SOB R0,.` подпрограммы монитора 102102, целиком
        // в ПЗУ, и её копия в ДОЗУ. На железе — 18,00 и 20,00 в обеих фазах: ответ ПЗУ
        // на такт позже памяти, отвечающей сразу (17).
        for (int ph = 0; ph < 2; ++ph)
            for (int inRom = 0; inRom < 2; ++inRom) {
                auto loop = [&](uint16_t n) {
                    Board b; b.setCpuPhase(ph); b.loadRoms(BK_DEFAULT_ROM_DIR); b.reset();
                    Memory& m = b.memory();
                    uint16_t sub = 0102102;
                    if (!inRom) {
                        sub = 03000;
                        for (int k = 0; k < 9; ++k) m.pokeWord(03000 + 2 * k, m.peekWord(0102102 + 2 * k));
                    }
                    m.pokeWord(01000, 0004737); m.pokeWord(01002, sub);   // JSR PC,@#sub
                    m.pokeWord(01004, 0000777);
                    b.cpu().reset(01000, 0340);
                    b.cpu().r[2] = 1; b.cpu().r[3] = n; b.cpu().r[6] = 01000;
                    const uint64_t t0 = b.totalTicks();
                    for (int i = 0; i < 100000 && b.cpu().pc() != 01004; ++i) b.stepInstruction();
                    return double(b.totalTicks() - t0);
                };
                const double per = (loop(13800) - loop(1000)) / 12800;
                const double want = inRom ? 18 : 20;
                char msg[96];
                std::snprintf(msg, sizeof msg, "ROMSOB, фаза %d: SOB R0,. в %s — %g такта", ph, inRom ? "ПЗУ" : "ДОЗУ", want);
                if (per != want) std::printf("  SOB в %s, фаза %d = %g\n", inRom ? "ПЗУ" : "ДОЗУ", ph, per);
                CHECK(per == want, msg);
            }
    }

    // ---- Быстрая память в модели шины: ОЗУ СМК-512 -------------------------
    SECTION("Быстрая память в модели шины: ОЗУ СМК-512")
    {
        // Замеры Manwe на реальной БК-0010-01 с СМК-512: тест 45com-hi и полная
        // таблица MOV/CMP/ADD (колонки «всё быстрое» и «быстрый код, данные в ДОЗУ»).
        // Память платы 037 не обслуживает: она отвечает сразу, и та же цепочка обменов
        // ВМ1 с мгновенным ответом даёт эти числа (Board::busStep). Меряем, как сами
        // тесты: 48 копий подряд на плате, средний такт на установившемся участке.
        // Ячейки плате — указатели на 0150200, ячейки ДОЗУ 010000..013777 — на 010200.
        auto smkClone = [](std::initializer_list<uint16_t> unit, uint16_t r1, uint16_t r2) {
            Board b; b.setSmk512(true); b.reset();
            // Трёхтактный протокол: строб, код режима ОЗУ11, снятие строба.
            b.smk().writeCtrl(6); b.smk().writeCtrl(040); b.smk().writeCtrl(0);
            Memory& m = b.memory();
            for (uint16_t a = 0150000; a < 0154000; a += 2) m.pokeWord(a, 0150200);
            for (uint16_t a = 010000; a < 014000; a += 2) m.pokeWord(a, 010200);
            const uint16_t start = 0140000, stride = static_cast<uint16_t>(2 * unit.size());
            uint16_t a = start;
            for (int k = 0; k < 48; ++k) for (uint16_t w : unit) { m.pokeWord(a, w); a += 2; }
            m.pokeWord(a, 0000777);
            b.cpu().reset(start, 0340);
            b.cpu().r[1] = r1; b.cpu().r[2] = r2;
            uint64_t t0 = 0;
            for (int i = 0; i < 20000; ++i) {
                const uint16_t pc = b.cpu().r[7];
                if (pc == start + 12 * stride) t0 = b.totalTicks();
                if (pc == start + 44 * stride) return double(b.totalTicks() - t0) / 32;
                b.stepInstruction();
            }
            return -1.0;
        };
        struct Case { std::initializer_list<uint16_t> unit; uint16_t r1, r2; double ticks; const char* name; };
        const Case cases[] = {
            // Всё на плате.
            {{0010102},  0150400, 0150500,  8, "MOV R1,R2"},
            {{0000240},  0150400, 0150500,  9, "NOP"},
            {{0000300},  0150400, 0150500,  9, "SWAB R0"},
            {{0005011},  0150400, 0150500, 20, "CLR (R1)"},
            {{0005021},  0150400, 0150500, 21, "CLR (R1)+"},
            {{0005711},  0150400, 0150500, 17, "TST (R1)"},
            {{0011112},  0150400, 0150500, 28, "MOV (R1),(R2)"},
            {{0021112},  0150400, 0150500, 25, "CMP (R1),(R2)"},
            {{0021122},  0150400, 0150500, 26, "CMP (R1),(R2)+"},
            {{0061112},  0150400, 0150500, 28, "ADD (R1),(R2)"},
            {{0061122},  0150400, 0150500, 29, "ADD (R1),(R2)+"},
            {{0014131},  0150400, 0150500, 34, "MOV -(R1),@(R1)+"},
            // Код на плате, данные в ДОЗУ. MOV R1,(R2) у Manwe 32 — на его машине запись
            // в ДОЗУ на 4 такта дороже (docs/slow-memory-timing.md), у нас 28.
            {{0021122},       010000, 010400, 32, "CMP (R1),(R2)+, данные в ДОЗУ"},
            {{0027102, 0},    010000, 010400, 40, "CMP @0(R1),R2, данные в ДОЗУ"},
            {{0020152},       010000, 013000, 40, "CMP R1,@-(R2), данные в ДОЗУ"},
            {{0010112},       010000, 010400, 28, "MOV R1,(R2), данные в ДОЗУ"},
        };
        for (const Case& c : cases) {
            char msg[96];
            std::snprintf(msg, sizeof msg, "СМК-512: %s — %g тактов, код на плате", c.name, c.ticks);
            const double t = smkClone(c.unit, c.r1, c.r2);
            if (t != c.ticks) std::printf("  %s = %g\n", c.name, t);
            CHECK(t == c.ticks, msg);
        }
    }

    // ---- Вход в прерывание и ловушка T-бита по шине; фаза шины в снимке ----
    SECTION("Вход в прерывание и ловушка T-бита по шине; фаза шины в снимке")
    {
        // Шаблоны входа — из симуляции кристалла (eCat3): PSW и PC в стек, вектор,
        // у клавиатуры ещё цикл IAKO. Раньше вход стоил константу 40 тактов, а ловушка
        // T-бита не стоила ничего.
        auto setup = [](Board& b) {
            b.reset();
            Memory& m = b.memory();
            m.pokeWord(01000, 0000777);                  // BR .
            m.pokeWord(02000, 0000002);                  // RTI
            m.pokeWord(0100, 02000); m.pokeWord(0102, 0);
            m.pokeWord(060, 02000);  m.pokeWord(062, 0);
            b.cpu().reset(01000, 0);
            b.cpu().r[6] = 01000;
            for (int i = 0; i < 8; ++i) b.stepInstruction();
        };
        {
            Board b; setup(b);
            const uint64_t t0 = b.totalTicks();
            b.runUntil(01000, 1);                       // IRQ2 на входе, затем RTI
            const int t = static_cast<int>(b.totalTicks() - t0);
            if (t != 105) std::printf("  IRQ2 + RTI = %d\n", t);
            CHECK(t == 105, "прерывание: вход в IRQ2 + RTI по шине");
        }
        {
            Board b; setup(b);
            b.pressKey(040);
            int t = 0;
            for (int i = 0; i < 100000 && b.cpu().pc() != 02000; ++i) t = b.stepInstruction();
            if (t != 85) std::printf("  BR + вход по клавиатуре = %d\n", t);
            CHECK(t == 85, "прерывание: BR + вход по клавиатуре (IAKO) по шине");
        }
        {
            // NOP с T=1: каждая копия — NOP, ловушка через 014 и RTT обработчика.
            Board b; b.reset();
            Memory& m = b.memory();
            m.pokeWord(07200, 0000006);                 // RTT
            m.pokeWord(014, 07200); m.pokeWord(016, 0340);
            for (int k = 0; k < 48; ++k) m.pokeWord(01000 + 2 * k, 0000240);
            m.pokeWord(01000 + 96, 0000777);
            b.cpu().reset(01000, 0360);
            b.cpu().r[6] = 01000;
            uint64_t t0 = 0; double t = -1;
            for (int i = 0; i < 2000; ++i) {
                const uint16_t pc = b.cpu().r[7];
                if (pc == 01000 + 2 * 12) t0 = b.totalTicks();
                if (pc == 01000 + 2 * 44) { t = double(b.totalTicks() - t0) / 32; break; }
                b.stepInstruction();
            }
            if (t != 116) std::printf("  NOP + ловушка T + RTT = %g\n", t);
            CHECK(t == 116, "ловушка T-бита стоит тактов (NOP + вход + RTT)");
        }
        {
            // Два прогона из одного снимка дают одно и то же время, как бы ни стоял
            // счётчик тактов у платы, в которую снимок загружают.
            Board a; a.reset();
            Memory& m = a.memory();
            for (int k = 0; k < 64; ++k) m.pokeWord(01000 + 2 * k, k & 1 ? 0011102 : 0005711);
            m.pokeWord(01000 + 128, 0000167); m.pokeWord(01000 + 130, static_cast<uint16_t>(-134));
            a.cpu().reset(01000, 0340);
            a.cpu().r[1] = 010000; a.cpu().r[2] = 0;
            for (int i = 0; i < 37; ++i) a.stepInstruction();
            std::vector<uint8_t> snap;
            a.saveStateMem(snap);
            auto run = [&](Board& b) {
                const uint64_t t0 = b.totalTicks();
                for (int i = 0; i < 500; ++i) b.stepInstruction();
                return b.totalTicks() - t0;
            };
            Board b, c;
            b.reset(); c.reset();
            b.runTicks(12345); c.runTicks(7778);
            b.loadStateMem(snap); c.loadStateMem(snap);
            const uint64_t ta = run(a), tb = run(b), tc = run(c);
            CHECK(ta == tb && tb == tc, "снимок хранит фазу шины: прогоны из него совпадают по тактам");
        }
    }

    // ---- Board: арбитраж 037 замедляет исполнение ----
    SECTION("Board: арбитраж 037 замедляет исполнение")
    {
        // Тесный цикл в ОЗУ, теряющий окно 037 (чтение по -(R) + переход обратно).
        // За фиксированный бюджет тактов с арбитражем проходит МЕНЬШЕ инструкций.
        auto instrs = [](bool arb) {
            Board b; b.setArbitration(arb); b.reset();
            b.memory().pokeWord(01000, 0014112);   // MOV -(R1),(R2)
            b.memory().pokeWord(01002, 0000776);   // BR 01000
            b.cpu().reset(01000, 0340);            // PC=01000, прерывания замаскированы
            b.cpu().r[1] = 030000; b.cpu().r[2] = 04000;
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
    SECTION("Раскладки джойстика (общая таблица ядра, src/core/Joystick.h)")
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
    SECTION("Имена клавиш и кодирование текста в КОИ-7 (src/core/BkKeys.h)")
    {
        CHECK(bkKeyByName("enter") == 012, "имя enter -> 012");
        CHECK(bkKeyByName("ВВОД") == 012, "русское имя, верхний регистр");
        CHECK(bkKeyByName("рус") == BK_CODE_RUS, "рус -> 016");
        CHECK(bkKeyByName("right") == 031 && bkKeyByName("up") == 032, "стрелки");
        CHECK(bkKeyByName("нетакой") == BK_KEY_NONE, "неизвестное имя");
        CHECK(std::string(bkKeyName(012)) == "enter", "обратный поиск имени");

        // АР2: код той же клавиши плюс бит 0200 (вектор 0274).
        CHECK(bkAr2Key("ар2+;") == 0273, "ар2+; -> 0273 (монитор: 32/64 символа)");
        CHECK(bkAr2Key("AR2+A") == 0301 && bkAr2Key("alt+a") == 0341, "регистр буквы сохраняется");
        CHECK(bkAr2Key("Ар2 + сбр") == 0214, "ар2+сбр -> 0214 (режим РП)");
        CHECK(bkAr2Key("ар2+ю") == 0340, "ар2+кириллица — без префикса РУС");
        CHECK(bkAr2Key("ар2+ ") == 0240 && bkAr2Key("ар2++") == 0253, "пробел и плюс");
        CHECK(bkAr2Key("ар2+лат") == 0217, "ар2+лат -> 0217");
        CHECK(bkAr2Key("ар2") == BK_KEY_NONE && bkAr2Key(";") == BK_KEY_NONE
              && bkAr2Key("ар2+ab") == BK_KEY_NONE && bkAr2Key("ctrl+a") == BK_KEY_NONE,
              "без префикса, без клавиши или с двумя символами — не АР2");

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
    SECTION("Снимок состояния: джойстик и удержание клавиши")
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

    // ---- Снимок: хвост BKX1 — звук, порт, клавиатура, фаза таймера (обе машины) ----
    SECTION("Снимок: хвост BKX1 — звук, порт, клавиатура, фаза таймера (обе машины)")
    for (int mach = 0; mach < 2; ++mach) {
        Board b;
        b.setMachine(mach ? Machine::Bk0011m : Machine::Bk0010);
        b.loadRoms(BK_DEFAULT_ROM_DIR);
        b.reset();
        Memory& m = b.memory();
        m.pokeWord(01000, 0000777);                     // BR .
        m.pokeWord(02000, 0000777); m.pokeWord(0274, 02000);
        m.pokeWord(03000, 0000777); m.pokeWord(060, 03000);
        b.cpu().reset(01000, 0340);
        b.setAy(true); b.setAy2(true); b.setCovox(true);
        b.ayChip(0).writeReg(8, 13);
        b.ayChip(1).writeReg(9, 7);
        m.writeWord(0177714, 1);                        // TurboSound: выбран второй AY
        m.writeWord(0177706, 0100);
        m.writeWord(0177712, 020);                      // таймер: пуск, перезагрузка
        b.runTicks(1000);
        b.pressKey(0201);                               // АР2: вектор 0274
        std::vector<uint8_t> snap;
        b.saveStateMem(snap);
        b.runTicks(3000);
        const uint16_t c1 = m.readWord(0177710);
        b.setAy(false); b.setAy2(false); b.setCovox(false);
        b.ayChip(0).writeReg(8, 0);
        m.writeWord(0177714, 0);
        b.pressKey(0101);                               // вектор 060
        CHECK(b.loadStateMem(snap), "BKX1: снимок читается");
        b.runTicks(3000);
        const uint16_t c2 = m.readWord(0177710);
        CHECK(b.ay() && b.ay2() && b.covox() && b.ay2Active() && b.ayTurboChip() == 1
              && b.ayChip(0).regs()[8] == 13 && b.ayChip(1).regs()[9] == 7 && b.portOut() == 1,
              mach ? "BKX1 (11М): AY, второй AY, Covox и выход порта восстановлены"
                   : "BKX1 (БК-0010): AY, второй AY, Covox и выход порта восстановлены");
        CHECK(c1 == c2, mach ? "BKX1 (11М): фаза таймера восстановлена"
                             : "BKX1 (БК-0010): фаза таймера восстановлена");
        b.cpu().psw = 0;
        b.runTicks(b.ticksPerFrame());
        CHECK(b.cpu().pc() == 02000 || b.cpu().pc() == 02002,
              mach ? "BKX1 (11М): прерывание клавиатуры — по восстановленному вектору 0274"
                   : "BKX1 (БК-0010): прерывание клавиатуры — по восстановленному вектору 0274");
    }

    // ---- Лог обращений к В-В: чтения, фильтр адреса, PC инструкции ----
    SECTION("Лог обращений к В-В: чтения, фильтр адреса, PC инструкции")
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
    SECTION("Лог фронтов динамика")
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
    SECTION("Динамик: 8 уровней из битов 6, 5 и 2 регистра 0177716")
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
    SECTION("Параллельный порт: раздельные регистры ввода и вывода")
    {
        Board b; b.reset();
        b.setJoystick(0123);
        b.memory().writeWord(0177714, 0252);
        CHECK(b.memory().readWord(0177714) == 0123, "чтение 0177714 отдаёт джойстик, а не записанное");
        CHECK(b.portOut() == 0252, "запись в 0177714 защёлкивается отдельно");
    }

    // ---- 0177716 бит 2: признак записи в системный регистр ----
    SECTION("0177716 бит 2: признак записи в системный регистр")
    {
        Board b; b.reset();
        CHECK(!(b.memory().readWord(0177716) & 004), "бит 2 сброшен, пока записи не было");
        b.memory().writeWord(0177716, 0);
        CHECK((b.memory().readWord(0177716) & 004), "после записи бит 2 взведён");
        CHECK(!(b.memory().readWord(0177716) & 004), "чтение гасит бит 2");
    }

    // ---- RESET: PSW не трогает, периферию сбрасывает ----
    SECTION("RESET: PSW не трогает, периферию сбрасывает")
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
    SECTION("WAIT: прерывание возвращает управление ЗА команду, а не на неё")
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
    SECTION("Кадровое прерывание защёлкивается, а не теряется при закрытой маске")
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
    SECTION("Внутренние регистры ЦП 0177700/02/04 (04-процессор, §4.3)")
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
    SECTION("«Баг флага C» ВМ1 [ВМ1 §10]: примеры прямо из документации")
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
    SECTION("Длительность кадра: 61440 тактов (48,83 Гц), а не 60000")
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
    SECTION("Клавиша «СТОП»: вектор 4, приоритет её не запрещает")
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
    SECTION("runUntil («шаг с обходом», «до адреса») продолжает выдавать кадры")
    {
        Board b; b.reset();
        b.memory().pokeWord(0100, 03000);    // ISR
        b.memory().pokeWord(0102, 0);
        b.memory().pokeWord(03000, 0000002); // RTI
        b.memory().pokeWord(01000, 0000001); // WAIT — без кадрового IRQ отсюда не выйти
        b.memory().pokeWord(01002, 0000240); // NOP — цель прогона
        b.cpu().reset(01000, 0);
        b.cpu().r[6] = 02000;
        b.trace().setEnabled(true);
        CHECK(b.runUntil(01002, 5 * b.ticksPerFrame()),
              "runUntil проходит WAIT: кадры выдаются и в прогоне отладчика");
        CHECK(b.trace().execCount(01000) <= 2,
              "runUntil простаивает на WAIT, как runTicks, а не исполняет его заново");
    }

    // ---- Ловушка по T-биту (вектор 014) и разница RTI/RTT ----
    // На ней держится «музыка параллельно с игрой»: обработчик 014 крутит динамик
    // после каждой команды основной программы и возвращается по RTT.
    SECTION("Ловушка по T-биту (вектор 014) и разница RTI/RTT")
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
    SECTION("Таймер при включении: непрограммированный счётчик проходит «минус»")
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
    SECTION("Динамик: сигнал далеко за Найквистом не сворачивается в слышимую полосу")
    {
        // Меандр с полупериодом 10 тактов = 150 кГц. RC-цепь пищалки (tau = 55.8 мкс,
        // срез ~2.85 кГц) обязана погасить его примерно в 50 раз ДО выборки. Если
        // фильтровать после децимации (как было), частота сворачивается в полосу и
        // даёт слышимую грязь: тот же тест на прежней реализации давал пик 1178.
        Speaker sp;                       // 44100 Гц, ЦП 3 МГц
        for (int i = 0; i < 60000; ++i) sp.feed((i & 1) ? 7 : 0, 10);
        std::vector<int16_t> buf(2 * sp.available());       // стерео: L, R
        const size_t n = sp.read(buf.data(), sp.available());
        int peak = 0;
        for (size_t i = n; i < 2 * n; ++i) { const int a = std::abs((int)buf[i]); if (a > peak) peak = a; }
        CHECK(n > 1000, "динамик выдал сэмплы");
        CHECK(peak < 800, "150 кГц гасится RC-цепью до выборки, а не сворачивается в полосу");
    }

    // ---- Построчная отрисовка: у каждой строки свой скролл ----
    SECTION("Построчная отрисовка: у каждой строки свой скролл")
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
    SECTION("Экранная кодировка БК -> UTF-8")
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
    SECTION("OCR: текст, нарисованный знакогенератором ПЗУ, читается обратно")
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
    SECTION("Блок расширения памяти СМК-512 («АльтПро»)")
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
    SECTION("Сквозной тест СМК: сторонний SMKTEST.bin")
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

    // ---- 45com-lo (©Manwe) на БК-0010-01 пользователя, фаза 1 ------------
    // tests/data/45com-lo.bin — «CLOCK CYCLES METER 0.1 slow memory»: 45 команд, каждая
    // повторена 1280 раз подряд в ДОЗУ, время меряет таймер 0177710. Результат каждой
    // ячейки (в десятых такта) возвращается в R0 из `CALL @#CODMEM`. Эталон — фото с
    // реальной БК-0010-01 после такого включения питания, что выпала фаза 1
    // (docs/slow-memory-timing.md): 45 из 45.
    SECTION("45com-lo (©Manwe) на БК-0010-01 пользователя, фаза 1")
    {
        const std::string binPath = std::string(BK_TEST_DATA_DIR) + "/45com-lo.bin";
        Board brd;
        brd.setCpuPhase(1);
        if (!brd.loadRoms(BK_DEFAULT_ROM_DIR)) {
            std::printf("SKIP: ПЗУ не найдено в %s — 45com-lo пропущен\n", BK_DEFAULT_ROM_DIR);
        } else {
            brd.reset();
            for (int i = 0; i < 25; ++i) brd.runFrame();
            if (!brd.loadBin(binPath, true)) {
                std::printf("SKIP: нет %s — 45com-lo пропущен\n", binPath.c_str());
            } else {
                // Порядок — как на экране: три столбца сверху вниз.
                static const int expect[45] = {
                    12, 28, 12, 24, 24, 24, 24, 24, 24, 12, 12, 28, 28, 36, 36,   // COM R0 .. CLR @-(R2)
                    12, 24, 36, 40, 40, 40, 40, 28, 28, 12, 48, 48, 16, 16, 12,   // MOV R1,R2 .. NOP
                    56, 48, 32, 36, 44, 40, 32, 24, 28, 16, 48, 48, 48, 48, 28,   // (R1),@0(R1) .. JMP @#fm
                };
                // Адрес возврата из CALL @#CODMEM (004737 025400).
                uint16_t ret = 0;
                for (uint16_t a = 01000; a < 040000 && !ret; a += 2)
                    if (brd.memory().peekWord(a) == 0004737 && brd.memory().peekWord(a + 2) == 025400) ret = a + 4;
                std::vector<int> got;
                for (long i = 0; i < 20000000L && got.size() < 45 && ret; ++i) {
                    brd.stepInstruction();
                    if (brd.cpu().r[7] == ret) got.push_back(brd.cpu().r[0]);
                }
                int bad = 0;
                for (size_t k = 0; k < got.size(); ++k)
                    if (got[k] != 10 * expect[k]) {
                        if (bad < 6) std::printf("  45com-lo: ячейка %zu = %d.%d, на железе %d\n",
                                                 k + 1, got[k] / 10, got[k] % 10, expect[k]);
                        ++bad;
                    }
                CHECK(ret && got.size() == 45, "45com-lo: все 45 замеров выполнены");
                CHECK(bad == 0, "45com-lo: фаза 1 совпадает с фото БК-0010-01 (45 из 45)");
            }
        }
    }

    // ---- Контроллер НГМД: поток дорожки и протокол регистров ----------------
    // Микросхема 1801ВП1-128 не ищет сектора — она отдаёт СЫРОЙ поток дорожки и
    // ловит маркер A1. Поэтому проверяем именно то, что видит драйвер: после
    // поиска маркера должно прийти A1A1, затем признак поля (A1FE или A1FB), а
    // за полем идентификатора — дорожка, сторона, номер сектора и код длины.
    SECTION("Контроллер НГМД: поток дорожки и протокол регистров")
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

        // ---- EMT 36 при вставленной дискете принадлежит ДОС, а не хосту ------
        // Перехват EMT 36 подменяет магнитофон рабочим каталогом хоста, и это
        // верно ТОЛЬКО для голой машины. Если в дисководе образ, файловая
        // система у машины своя: вызов обслуживает ANDOS со своего диска.
        // Перехват здесь доходит до программы как «файл не найден» — а многие
        // игры этот ответ не проверяют и идут работать с непрочитанным буфером
        // (onslaught.bkd: расшифровывает мусор и передаёт на него управление).
        SECTION("EMT 36 при вставленной дискете принадлежит ДОС, а не хосту")
        {
            auto runEmt36 = [](Board& b) {
                b.memory().pokeWord(01000, 0104036);          // EMT 36
                b.memory().pokeWord(01002, 000777);           // BR .
                b.memory().pokeByte(02000, 3);                // команда «читать»
                for (int i = 0; i < 16; ++i)                  // имя, которого на хосте нет
                    b.memory().pokeByte(02000 + 6 + i, i < 11 ? "NOSUCH.BIN"[i] : ' ');
                b.cpu().reset(01000, 0340);
                b.cpu().r[1] = 02000;
                b.stepInstruction();
            };
            Board noDisk;
            noDisk.reset();
            runEmt36(noDisk);
            CHECK(!noDisk.emtLog().empty(),
                  "EMT 36 без дискеты перехватывается (файлы берутся из каталога хоста)");

            // Вставить образ можно только с прошивкой контроллера — без неё
            // дисковой конфигурации не существует и проверять нечего.
            Board withDisk;
            if (!withDisk.loadRoms(BK_DEFAULT_ROM_DIR)) {
                std::printf("SKIP: ПЗУ не найдено в %s — проверка EMT 36 с дискетой пропущена\n",
                            BK_DEFAULT_ROM_DIR);
            } else {
                withDisk.reset();
                CHECK(withDisk.attachDisk(0, imgPath), "образ вставлен для проверки EMT 36");
                runEmt36(withDisk);
                CHECK(withDisk.emtLog().empty(),
                      "EMT 36 с дискетой НЕ перехватывается — вызов уходит в ДОС");
            }
        }
        std::filesystem::remove(imgPath);
    }

    // ---- «Зависание»: запись в регистр, который её не принимает --------------
    // Регистр данных клавиатуры 0177662 на БК-0010 доступен только по чтению.
    // Запись шина не подтверждает, и процессор идёт по вектору 4 (§7.4). Этим
    // прошивка дискового контроллера отличает БК-0010 от БК-0011М, где по этому
    // адресу лежит регистр управления экраном: без ловушки она считает машину
    // одиннадцатой, ставит негодный режим памяти, и ни ANDOS, ни игры с дискет
    // не поднимаются.
    SECTION("«Зависание»: запись в регистр, который её не принимает")
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
    SECTION("Шина МПИ: блок расширения памяти и дисковод вместе")
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

    // ---- Закрытие регистров: зеркало ПЗУ СМК в режиме SYS --------------------
    // Плата выставляет ПЗУ и на страницу регистров, регистры БК тоже отвечают —
    // читается монтажное ИЛИ. Кроме «дыры» 0177660..0177664 и периферийного блока
    // ВМ1 0177700..0177712, который процессор читает изнутри.
    SECTION("Закрытие регистров: зеркало ПЗУ СМК в режиме SYS")
    {
        Board b;
        if (!b.loadRoms(BK_DEFAULT_ROM_DIR)) {
            std::printf("SKIP: ПЗУ не найдено — тесты закрытия регистров пропущены\n");
        } else {
            b.setSmk512(true);
            b.setDiskRom("smk512.rom");
            const std::string img = (std::filesystem::temp_directory_path()
                                     / "bk-sysreg-test.img").string();
            { std::vector<uint8_t> z(2 * 2 * bk::Fdd::TRACK_BYTES, 0);
              std::FILE* f = std::fopen(img.c_str(), "wb");
              if (f) { std::fwrite(z.data(), 1, z.size(), f); std::fclose(f); } }
            CHECK(b.attachDisk(0, img), "SYS: диск с прошивкой СМК вставляется");
            b.reset();
            CHECK(b.smk().mode() == bk::Smk512::SYS && b.kngmd().romStart() != 0,
                  "SYS: плата в режиме SYS, прошивка перехватывает старт");
            auto rom = [&](uint16_t a) { uint16_t v = 0; b.kngmd().romMirror(a, v); return v; };

            uint16_t expect = rom(0177716) | b.peekReg(0177716);
            CHECK(rom(0177716) != 0 && b.memory().readWord(0177716) == expect,
                  "SYS: 0177716 = ПЗУ | системный регистр");
            expect = rom(0177714) | b.peekReg(0177714);
            CHECK(b.memory().readByte(0177715) == (expect >> 8)
                  && b.memory().readByte(0177714) == (expect & 0377),
                  "SYS: 0177714 побайтно = ПЗУ | порт");
            CHECK(b.memory().readWord(0177660) == b.peekReg(0177660)
                  && b.memory().readWord(0177662) == b.peekReg(0177662)
                  && b.memory().readWord(0177664) == b.peekReg(0177664),
                  "SYS: «дыра» 0177660..0177664 — только регистры БК");
            b.memory().writeWord(0177706, 0);
            CHECK(rom(0177706) != 0 && b.memory().readWord(0177706) == 0,
                  "SYS: 0177706 (внутри ВМ1) зеркалом не закрыт");
            b.memory().writeWord(0177664, 01230);
            CHECK(b.peekReg(0177664) == 01230, "SYS: запись в регистр доходит до БК");

            // То же в режиме All: на странице регистров сегмент 3 «только чтение».
            b.memory().writeWord(0177130, bk::Smk512::STROBE);
            b.memory().writeWord(0177130, bk::Smk512::modeCode(bk::Smk512::ALL));
            b.memory().writeWord(0177130, 0);
            CHECK(b.smk().mode() == bk::Smk512::ALL, "All: режим защёлкнут");
            b.smk().ram()[3 * bk::Smk512::SEG_BYTES + 07715] = 0200;   // старший байт 0177714
            b.smk().ram()[3 * bk::Smk512::SEG_BYTES + 07706] = 0123;
            CHECK(b.memory().readWord(0177714) == (0100000 | b.peekReg(0177714)),
                  "All: 0177714 = ОЗУ | порт");
            CHECK(b.memory().readWord(0177706) == 0, "All: 0177706 (внутри ВМ1) ОЗУ не закрыт");
            std::filesystem::remove(img);
        }
    }


    // ---- Таймер ВМ1, режим перезагрузки: отсчёта со значением 0 нет, период = предел ----
    SECTION("Таймер ВМ1, режим перезагрузки: отсчёта со значением 0 нет, период = предел")
    {
        Board b; b.reset();
        Memory& m = b.memory();
        m.pokeWord(01000, 000777);                       // BR .
        b.cpu().reset(01000, 0340);
        m.writeWord(0177706, 5);
        m.writeWord(0177712, 020);                       // RUN, перезагрузка
        const uint64_t t0 = b.totalTicks();
        bool zero = false, seqOk = true;
        int reads = 0;
        while (b.totalTicks() - t0 < 128 * 40) {
            b.stepInstruction();
            const uint16_t v = m.readWord(0177710);
            const uint64_t k = (b.totalTicks() - t0) / 128;   // отсчётов с запуска
            if (v == 0) zero = true;
            if (v != 5 - k % 5) seqOk = false;
            ++reads;
        }
        CHECK(!zero && seqOk && reads > 100, "таймер ВМ1: 5,4,3,2,1,5,… — нуля нет, период равен пределу");
        // То же, когда между чтениями прошло много периодов.
        m.writeWord(0177712, 020);
        const uint64_t t1 = b.totalTicks();
        while (b.totalTicks() - t1 < 128 * 1003) b.stepInstruction();
        const uint64_t k = (b.totalTicks() - t1) / 128;
        CHECK(m.readWord(0177710) == 5 - k % 5, "таймер ВМ1: пропуск многих периодов считается с тем же периодом");
    }

    // ---- Команда RESET останавливает таймер, но счётчик не трогает (К1801ВМ1, §6.6) ----
    SECTION("Команда RESET останавливает таймер, но счётчик не трогает (К1801ВМ1, §6.6)")
    {
        Board b; b.reset();
        Memory& m = b.memory();
        loadProg(m, 01000, {000777, 000005, 000777});    // BR . (ожидание); RESET; BR .
        b.cpu().reset(01000, 0340);
        m.writeWord(0177706, 01000);
        m.writeWord(0177712, 020);                       // RUN, перезагрузка
        while (b.totalTicks() < 128 * 37) b.memory().readWord(0177710), b.stepInstruction();
        b.cpu().r[7] = 01002;
        const uint16_t before = m.readWord(0177710);
        b.stepInstruction();                             // RESET
        const uint16_t after = m.readWord(0177710);
        for (int i = 0; i < 50; ++i) b.stepInstruction();
        CHECK(before != 01000 && (after == before || after == before - 1) && m.readWord(0177710) == after
              && !(m.readWord(0177712) & 020),
              "RESET: таймер остановлен, счётчик остался прежним (предел в него не копируется)");
    }

    // ---- Кадры идут точно по 61440/81920 тактов: перелёт последней команды и вход
    //      в прерывание не удлиняют кадр (иначе всё, что держится за таймер ВМ1, уползает)
    for (int m11 = 0; m11 < 2; ++m11) {
        Board b;
        if (m11) b.setMachine(Machine::Bk0011m);
        b.reset();
        Memory& m = b.memory();
        const int T = b.ticksPerFrame();
        // Таймер ВМ1 с периодом ровно в кадр: в режиме перезагрузки период — сам предел
        // (0 сразу сменяется пределом), 128 тактов на отсчёт. INSULT ставит 01200 = 640.
        loadProg(m, 01000, {012737, static_cast<uint16_t>(T / 128), 0177706,       // MOV #предел,@#177706
                            012737, 020, 0177712,                                 // MOV #20,@#177712 (RUN)
                            012705, 02000,                                        // MOV #2000,R5
                            000240, 000776});                                     // NOP; BR .-2
        m.pokeWord(0100, 03000); m.pokeWord(0102, 0340);
        loadProg(m, 03000, {013725, 0177710, 000002});                            // MOV @#177710,(R5)+; RTI
        if (m11) m.writeWord(0177662, 0);                                         // 11М: кадровый таймер вкл.
        b.cpu().reset(01000, 0);
        b.cpu().r[6] = 01000;
        for (int i = 0; i < 205; ++i) b.runFrame();
        const int first = m.peekWord(02000 + 2 * 3), last = m.peekWord(02000 + 2 * 200);
        if (std::abs(first - last) > 1) { std::printf("  счётчик:"); for (int k = 0; k < 205; k += 20) std::printf(" %o", m.peekWord(02000 + 2 * k)); std::printf("\n"); }
        CHECK(std::abs(first - last) <= 1,
              m11 ? "11М: счётчик таймера на кадровом прерывании не уползает за 200 кадров"
                  : "БК-0010: счётчик таймера на кадровом прерывании не уползает за 200 кадров");
    }

    // ======================= БК-0011М ===========================================
    // ---- Растр: кадр эмулятора начинается с nVSYNC (RTL va_037.v:239-241) -------
    SECTION("Растр: кадр эмулятора начинается с nVSYNC (RTL va_037.v:239-241)")
    {
        Vp037 v;
        v.syncToVsync();
        CHECK(v.vgate(), "nVSYNC — внутри кадрового гашения");
        CHECK((v.lc() & 077) >= 40 && (v.lc() & 077) <= 42, "nVSYNC: LC[5:0] в 42..40 (SYNC2)");
        CHECK(v.scanline() == Vp037::VSYNC_LINE && v.fieldLine() == -43,
              "от синхроимпульса до первой видимой строки 43 строки");
        v.tick(43 * Vp037::CLKIN_PER_LINE);
        CHECK(v.fieldLine() == 0 && v.scanline() == 0 && !v.vgate() && v.lineWord() == 0,
              "через 43 строки луч в начале видимого поля");
        v.tick(256 * Vp037::CLKIN_PER_LINE);
        CHECK(v.fieldLine() == 256 && v.vgate(), "после 256 видимых строк — снова гашение");
        v.tick(10 * Vp037::CLKIN_PER_WORD);
        CHECK(v.lineWord() == 10, "слово строки считается по 8 CLKIN");
    }
    // ---- Частота и кадр -----------------------------------------------------
    SECTION("Частота и кадр")
    {
        Board b; b.setMachine(Machine::Bk0011m);
        CHECK(b.cpuFreqHz() == 4000000, "11М: ЦП 4 МГц");
        CHECK(b.ticksPerFrame() == 81920, "11М: кадр 81920 тактов (037 по-прежнему 6 МГц)");
        CHECK(b.frameRateHz() > 48.8 && b.frameRateHz() < 48.9, "11М: кадровая частота та же 48,83 Гц");
        b.reset();
        b.memory().pokeWord(01000, 000777);   // BR .
        b.cpu().reset(01000, 0340);
        b.runTicks(b.ticksPerFrame() / 2);
        CHECK(b.vp037().scanline() >= 158 && b.vp037().scanline() <= 162,
              "11М: 1 такт = 1,5 CLKIN — за полкадра луч проходит полкадра");
    }
    // ---- Простой по WAIT: время идёт ----------------------------------------
    SECTION("Простой по WAIT: время идёт")
    {
        Board b; b.reset();
        b.memory().pokeWord(01000, 000001);   // WAIT
        b.cpu().reset(01000, 0340);           // прерывания закрыты — разбудить некому
        const uint64_t t0 = b.totalTicks();
        const int ran = b.runTicks(20000);
        CHECK(ran >= 20000 && b.totalTicks() - t0 >= 20000,
              "WAIT: остаток отрезка проходит в простое, такты (звук, таймер, луч) идут");
        CHECK(b.cpu().waiting(), "WAIT: флаг ожидания снимает только прерывание");
    }
    {
        Board b; b.setMachine(Machine::Bk0011m);
        if (!b.loadRoms(BK_DEFAULT_ROM_DIR)) {
            std::printf("SKIP: ПЗУ БК-0011М не найдены в %s — тесты 11М пропущены\n", BK_DEFAULT_ROM_DIR);
        } else {
            b.reset();
            Memory& m = b.memory();
            CHECK(b.cpu().pc() == 0140000, "11М: пуск с 0140000 (БОС)");
            CHECK((m.readWord(0177716) & 0177600) == 0140200, "11М: 0177716 читается как 0140000 | 0200");
            // ---- Страницы: код -> страница {1,5,2,3,4,7,0,6}, оба окна ----
            for (int pg = 0; pg < 8; ++pg) m.ram11()[pg * Memory::PAGE11_BYTES] = static_cast<uint8_t>(0100 + pg);
            bool allOk = true;
            for (int code = 0; code < 8; ++code) {
                m.writeWord(0177716, static_cast<uint16_t>(04000 | code << 12 | code << 8));
                const int pg = Memory::pageForCode(code);
                if (m.readByte(040000) != 0100 + pg || m.readByte(0100000) != 0100 + pg) allOk = false;
                if (Memory::codeForPage(pg) != code) allOk = false;
            }
            CHECK(allOk, "11М: все 8 кодов страниц в обоих окнах");
            m.writeWord(0177716, 04000 | 03);          // биты ПЗУ 0 и 1 сразу
            CHECK(m.window1Rom() == 0 && m.readWord(0100000) == 010103, "11М: бит 0 главнее — ПЗУ-стр. 0 (328)");
            m.writeWord(0177716, 04000 | 02);
            CHECK(m.readWord(0100000) == 000240 && m.readWord(0120000) == 000207,
                  "11М: ПЗУ-стр. 1 — БЕЙСИК 327 + расширение БОС 325");
            CHECK(m.readWord(0140000) == 0106427, "11М: БОС (324) на 0140000 всегда");
            // ---- Карта блоков для визуализатора ----
            m.writeWord(0177716, static_cast<uint16_t>(04000 | 5 << 12 | 2 << 8));
            {
                const int p0 = Memory::pageForCode(5), p1 = Memory::pageForCode(2);
                const int P = static_cast<int>(Memory::PAGE11_BYTES), H = P / 2;
                CHECK(m.blockRamOffset(0) == 0 && m.blockRamOffset(1) == H
                   && m.blockRamOffset(2) == p0 * P && m.blockRamOffset(3) == p0 * P + H
                   && m.blockRamOffset(4) == p1 * P && m.blockRamOffset(5) == p1 * P + H
                   && m.blockRomSlot(4) < 0,
                      "11М: blockRamOffset — страница 0 и страницы обоих окон");
            }
            CHECK(m.blockRomSlot(6) == Memory::ROM_BOS && m.blockRamOffset(6) < 0
               && (m.mstd() || m.blockRomSlot(7) < 0),
                  "11М: blockRomSlot — БОС в блоке 6, без МСТД блок 7 пуст");
            m.writeWord(0177716, 04000 | 01);
            CHECK(m.blockRomSlot(4) == Memory::ROM_BASIC2 && m.blockRomSlot(5) == Memory::ROM_BASIC3
               && m.blockRamOffset(4) < 0, "11М: blockRomSlot — ПЗУ-стр. 0 = 328 + 329");
            m.writeWord(0177716, 04000 | 02);
            CHECK(m.blockRomSlot(4) == Memory::ROM_BASIC1 && m.blockRomSlot(5) == Memory::ROM_EXT,
                  "11М: blockRomSlot — ПЗУ-стр. 1 = 327 + 325");
            m.writeWord(0177716, 04000 | 010);
            CHECK(m.blockRomSlot(4) < 0 && m.blockRamOffset(4) < 0 && m.blockRomSlot(5) < 0,
                  "11М: пустая ПЗУ-стр. 2 — блоки 4/5 ни ОЗУ, ни ПЗУ");
            m.writeWord(0177716, 04000 | 02);
            // Ленточная запись (бит 11 = 0) карту не трогает.
            m.writeWord(0177716, 0100);
            CHECK(m.window1Rom() == 1 && b.reg716tap() == 0100, "11М: бит 11 = 0 — регистр ленты/динамика");
            // Байт в старшую половину 0177716 — как у GID: сдвиг в старший байт.
            m.writeByte(0177717, 0130);                // 0130 << 8 = 054000: окно0 код 5, бит 11
            CHECK(m.window0Page() == Memory::pageForCode(5) && m.window1Rom() < 0,
                  "11М: байтовая запись в 0177717 — старший байт регистра памяти");

            // ---- Пустая ПЗУ-страница и запись в ПЗУ — зависание (вектор 4) ----
            {
                b.reset();
                m.pokeWord(04, 02000); m.pokeWord(06, 0340);
                m.pokeWord(02000, 000777);                         // BR .
                loadProg(m, 01000, {012737, 004010, 0177716,        // MOV #4010,@#177716 (ПЗУ-стр. 2)
                                    013700, 0100000,                // MOV @#100000,R0
                                    000777});
                b.cpu().reset(01000, 0340);
                b.stepInstruction(); b.stepInstruction();
                CHECK(b.cpu().pc() == 02000, "11М: чтение пустой ПЗУ-страницы — вектор 4");
            }
            // ---- 0177662: палитра, экран, кадровый таймер ----
            {
                b.reset();
                m.pokeWord(04, 02000); m.pokeWord(06, 0340);
                m.pokeWord(02000, 000777);
                loadProg(m, 01000, {012737, 0102400, 0177662,       // MOV #102400,@#177662
                                    000240, 000777});
                b.cpu().reset(01000, 0340);
                b.stepInstruction(); b.stepInstruction();
                CHECK(b.cpu().pc() == 01010, "11М: запись в 0177662 принимается (без вектора 4)");
                CHECK(b.screen().palette() == 5 && m.videoPage() == 6, "11М: палитра 5, экран — страница 6");
                CHECK(m.readWord(0177662) == 0, "11М: 0177662 на чтение — по-прежнему код клавиши");
                // Байтовая запись в 0177663 — старший байт регистра: палитра, таймер, экран.
                // Демо «200%» включает так кадровый таймер: MOVB #14,@#177663.
                loadProg(m, 01000, {0112737, 0014, 0177663, 000240, 000777});   // MOVB #14,@#177663; NOP
                b.cpu().reset(01000, 0340);
                b.stepInstruction(); b.stepInstruction();
                CHECK(b.cpu().pc() == 01010 && b.screen().palette() == 12 && !(b.reg662() & 040000)
                      && m.videoPage() == 5,
                      "11М: MOVB #14,@#177663 — палитра 12, кадровый таймер включён, экран стр. 5");
                loadProg(m, 01000, {000005, 000777});           // RESET
                b.cpu().reset(01000, 0340);
                b.stepInstruction();
                CHECK(b.reg662() == 006000, "11М: команда RESET регистр палитры не трогает");
            }
            {
                // Кадровое прерывание 0100 разрешается битом 14 = 0.
                b.reset();
                m.pokeWord(0100, 03000); m.pokeWord(0102, 0340);
                loadProg(m, 03000, {005203, 000002});            // INC R3; RTI
                m.pokeWord(01000, 000777);
                b.cpu().reset(01000, 0);
                b.cpu().r[6] = 01000;
                b.cpu().r[3] = 0;
                m.writeWord(0177662, 040000);                   // таймер выключен
                for (int i = 0; i < 3; ++i) b.runFrame();
                CHECK(b.cpu().r[3] == 0, "11М: бит 14 = 1 — кадровых прерываний нет");
                m.writeWord(0177662, 0);
                for (int i = 0; i < 3; ++i) b.runFrame();
                if (b.cpu().r[3] != 3) std::printf("  R3=%o PC=%o PSW=%o\n", b.cpu().r[3], b.cpu().pc(), b.cpu().psw);
                CHECK(b.cpu().r[3] == 3, "11М: бит 14 = 0 — прерывание 0100 раз в кадр");
            }
            {
                // СТОП блокируется битом 12 ленточного регистра.
                b.reset();
                m.pokeWord(04, 02000); m.pokeWord(06, 0340);
                m.pokeWord(02000, 000777);
                m.pokeWord(01000, 000777);
                b.cpu().reset(01000, 0340);
                m.writeWord(0177716, 010000);
                b.pressStop(); b.stepInstruction();
                CHECK(b.cpu().pc() == 01000, "11М: бит 12 0177716 — СТОП заблокирован");
                m.writeWord(0177716, 0);
                b.pressStop(); b.stepInstruction();
                CHECK(b.cpu().pc() == 02000, "11М: без блокировки СТОП — вектор 4");
            }
            // ---- Палитра по ходу строки: смена видна с того слова, где луч ----
            {
                b.reset();
                b.setScanlineRender(true);
                for (size_t i = 0; i < Memory::PAGE11_BYTES; ++i) m.ram11()[5 * Memory::PAGE11_BYTES + i] = 0377;
                m.pokeWord(01000, 000777);
                b.cpu().reset(01000, 0340);
                m.writeWord(0177662, 017 << 8);                  // палитра 15: цвет 3 — белый
                b.runFrameSlice(0, 1000);                        // верх кадра (nVSYNC)
                while (!(b.vp037().fieldLine() == 100 && b.vp037().lineWord() >= 16)) b.stepInstruction();
                const int splitWord = b.vp037().lineWord();
                m.writeWord(0177662, 0);                         // палитра 0: цвет 3 — красный
                while (b.vp037().fieldLine() <= 101) b.stepInstruction();
                const uint32_t* px = b.screen().pixels();
                const uint32_t left = px[100 * Screen::TEX_W] & 0xFFFFFF;
                const uint32_t right = px[100 * Screen::TEX_W + Screen::TEX_W - 1] & 0xFFFFFF;
                const uint32_t above = px[99 * Screen::TEX_W + Screen::TEX_W - 1] & 0xFFFFFF;
                const uint32_t below = px[101 * Screen::TEX_W] & 0xFFFFFF;
                CHECK(left == 0xFFFFFF && right == 0xFF0000, "11М: смена палитры посреди строки делит строку");
                CHECK(above == 0xFFFFFF && below == 0xFF0000, "11М: строки выше — старой палитрой, ниже — новой");
                CHECK(splitWord >= 16 && splitWord < 32 &&
                      (px[100 * Screen::TEX_W + splitWord * 16 - 1] & 0xFFFFFF) == 0xFFFFFF,
                      "11М: до слова, где стоял луч, — старая палитра");
            }
            // ---- Скролл посреди поля: 037 берёт его только на синхроимпульсе ----
            {
                b.reset();
                b.setScanlineRender(true);
                uint8_t* vram = m.ram11() + 5 * Memory::PAGE11_BYTES;
                for (size_t i = 0; i < Memory::PAGE11_BYTES; ++i) vram[i] = 0;
                for (int i = 0; i < 64; ++i) vram[105 * 64 + i] = 0377;   // светится строка ВОЗУ 105
                m.pokeWord(01000, 000777);
                b.cpu().reset(01000, 0340);
                m.writeWord(0177662, 017 << 8);                  // палитра 15: цвет 3 — белый
                b.runFrameSlice(0, 1000);
                while (b.vp037().fieldLine() < 50) b.stepInstruction();
                m.writeWord(0177664, 01335);                     // смещение +5
                while (b.vp037().fieldLine() <= 110) b.stepInstruction();
                const uint32_t* px = b.screen().pixels();
                auto lit = [&](int y) { return (px[y * Screen::TEX_W] & 0xFFFFFF) != 0; };
                CHECK(lit(105) && !lit(100), "11М: скролл, записанный посреди поля, в этом кадре не действует");
                b.runFrameSlice(0, 1000);                        // следующий кадр
                while (b.vp037().fieldLine() <= 110) b.stepInstruction();
                CHECK(lit(100) && !lit(105), "11М: со следующего кадра — новое смещение");
                m.writeWord(0177664, 01330);
            }
            // ---- Фаза кадрового прерывания 11М и момент записи в регистр палитры ----
            {
                b.reset();
                b.setScanlineRender(true);
                m.pokeWord(01000, 000777);
                b.cpu().reset(01000, 0340);
                b.runFrameSlice(0, 100000);
                // Схема платы (детектор пропадания WTI, OneChipBook bk_evnt.sv): IRQ2 через
                // 452 CLKIN после подъёма VGATE (строка 255, слово 32) — строка 256, слово 40.
                CHECK(b.vp037().fieldLine() == -64 && b.vp037().scanline() == Vp037::IRQ11_LINE
                      && b.vp037().lineWord() == 40,
                      "11М: кадр (и прерывание 0100) — VGATE + 452 CLKIN: строка 256, слово 40");
                Board z; z.reset(); z.memory().pokeWord(01000, 000777); z.cpu().reset(01000, 0340);
                z.runFrameSlice(0, 100000);
                CHECK(z.vp037().fieldLine() == -43, "БК-0010: кадр — на nVSYNC, за 43 строки до поля");
                // Запись MOV #0,@#177662 вступает в силу в конце команды, а не в её начале.
                for (size_t i = 0; i < Memory::PAGE11_BYTES; ++i) m.ram11()[5 * Memory::PAGE11_BYTES + i] = 0377;
                loadProg(m, 01000, {012737, 0, 0177662, 000777});   // MOV #0,@#177662; BR .
                m.pokeWord(02000, 000777);                            // BR . — ожидание
                m.writeWord(0177662, 017 << 8);                  // палитра 15: цвет 3 — белый
                b.cpu().reset(02000, 0340);
                b.runFrameSlice(0, 1000);
                while (!(b.vp037().fieldLine() == 100 && b.vp037().lineWord() >= 6 && b.vp037().lineWord() <= 12))
                    b.stepInstruction();
                b.cpu().r[7] = 01000;                            // встать на MOV на строке 100
                const int w0 = b.vp037().lineWord();
                b.stepInstruction();                             // MOV #0,@#177662
                const int w1 = b.vp037().lineWord();
                while (b.vp037().fieldLine() <= 101) b.stepInstruction();
                const uint32_t* px = b.screen().pixels();
                CHECK(w1 > w0 + 2 && (px[100 * Screen::TEX_W + (w0 + 1) * 16] & 0xFFFFFF) == 0xFFFFFF
                      && (px[100 * Screen::TEX_W + (w1 + 1) * 16] & 0xFFFFFF) == 0xFF0000,
                      "11М: смена палитры вступает в силу в конце команды записи");
            }
            // ---- Модель шины 11М: среднее время команд в ДОЗУ (таблица Manwe) ----
            {
                auto avg = [&](uint16_t ir) {
                    for (uint16_t a = 010000; a < 040000; a += 2) m.pokeWord(a, 033000);
                    for (int i = 0; i < 112; ++i) m.pokeWord(static_cast<uint16_t>(01000 + 2 * i), ir);
                    b.cpu().reset(01000, 0340);
                    b.cpu().r[1] = 024000; b.cpu().r[2] = 026000;
                    for (int i = 0; i < 16; ++i) b.stepInstruction();
                    long t = 0;
                    for (int i = 0; i < 96; ++i) t += b.stepInstruction();
                    return double(t) / 96;
                };
                b.reset();
                CHECK(avg(010102) == 16.0, "11М: MOV R1,R2 = 16 тактов (Manwe)");
                CHECK(avg(011102) == 32.0, "11М: MOV (R1),R2 = 32");
                CHECK(avg(010112) == 32.0, "11М: MOV R1,(R2) = 32");
                CHECK(avg(010122) == 40.0, "11М: MOV R1,(R2)+ = 40");
                CHECK(avg(011112) == 48.0, "11М: MOV (R1),(R2) = 48");
                CHECK(avg(021112) == 48.0, "11М: CMP (R1),(R2) = 48");
                // Код и константа в ДОЗУ, запись — в порт: 7 окон 037, как у OneChipBook
                // (было 8 — кривые полосы палитры в демо «Однажды»).
                for (int i = 0; i < 56; ++i) {
                    m.pokeWord(static_cast<uint16_t>(01000 + 4 * i), 012710);
                    m.pokeWord(static_cast<uint16_t>(01002 + 4 * i), 0377);
                }
                b.cpu().reset(01000, 0340);
                b.cpu().r[0] = 0177714;
                for (int i = 0; i < 8; ++i) b.stepInstruction();
                long t = 0;
                for (int i = 0; i < 48; ++i) t += b.stepInstruction();
                CHECK(std::fabs(t / 48.0 - 112.0 / 3) < 0.01, "11М: MOV #377,(R0) в 0177714 = 37,33 такта");
            }
            // ---- AY-3-8910 на 0177714: слово — номер регистра, байт — данные, инверсно ----
            {
                b.reset();
                CHECK(b.ay(), "11М: AY по умолчанию подключён");
                auto ayw = [&](int r, int v) {
                    m.writeWord(0177714, static_cast<uint16_t>(~r));
                    m.writeByte(0177714, static_cast<uint8_t>(~v));
                };
                ayw(0, 100); ayw(1, 0); ayw(7, 076); ayw(8, 15);
                const uint8_t* r = b.ayChip().regs();
                CHECK(r[0] == 100 && r[7] == 076 && r[8] == 15 && b.ayChip().address() == 8,
                      "AY: запись словом выбирает регистр, байтом — данные (инверсные линии)");
                m.writeByte(0177715, 0);                  // нечётный байт: младший 0 -> данные 0377
                CHECK(r[8] == 037, "AY: байт в 0177715 пишет ~0 = 0377 (по маске регистра)");
                // Команда RESET: INIT идёт на контакт А1 «СБРОС» разъёма УП — AY обнуляется
                // и смолкает (так глушит музыку INSULT, подпрограмма 035204).
                ayw(9, 15); ayw(10, 12);
                m.pokeWord(01000, 000005);                 // RESET
                b.cpu().reset(01000, 0340);
                b.stepInstruction();
                bool silent = b.ayChip().address() == 0;
                for (int i = 0; i < 16; ++i) silent = silent && r[i] == 0;
                CHECK(silent, "AY: команда RESET обнуляет регистры сопроцессора (INIT на разъёме УП)");
                Ay8910 ay;
                ay.writeReg(0, 100); ay.writeReg(1, 0); ay.writeReg(7, 076); ay.writeReg(8, 15);
                int edges = 0; bool hi = false;
                for (int i = 0; i < 44100; ++i) {
                    ay.advance(1.0 / 44100);
                    const bool h = ay.takeSample() > 0.5 * (1.0 / 3.0) * 0.5;
                    if (h && !hi) ++edges;
                    hi = h;
                }
                const double f = Ay8910::DEFAULT_CLOCK / (16.0 * 100);
                CHECK(edges > f * 0.98 && edges < f * 1.02, "AY: частота тона = clock / (16 · TP)");
                ay.writeReg(8, 0);
                double mn = 1, mx = 0;
                for (int i = 0; i < 4410; ++i) { ay.advance(1.0 / 44100); const double v = ay.takeSample(); mn = std::min(mn, v); mx = std::max(mx, v); }
                CHECK(mx - mn < 1e-9, "AY: громкость 0 — тишина");
                // Стерео ABC с коэффициентами GID: A — влево, B — в центр, C — вправо.
                auto pan = [&](int ch) {
                    Ay8910 c;
                    c.writeReg(7, 077); c.writeReg(8 + ch, 15);   // тон и шум выключены — постоянный уровень
                    double l = 0, r = 0;
                    for (int i = 0; i < 441; ++i) { c.advance(1.0 / 44100); c.takeSampleLR(l, r); }
                    return std::make_pair(l, r);
                };
                const auto pa = pan(0), pb = pan(1), pc = pan(2);
                CHECK(pa.first > 0 && std::abs(pa.first / pa.second - 1.149375 / 0.249375) < 1e-6
                      && std::abs(pb.first - pb.second) < 1e-12
                      && std::abs(pc.second / pc.first - 1.149375 / 0.249375) < 1e-6
                      && std::abs(pa.first - pc.second) < 1e-12,
                      "AY: стерео ABC как у GID — A влево, B в центр, C вправо");
                // Два AY: разряд 15 = 0 — первый чип, 14 = 0 — второй (демо «200%»).
                CHECK(!b.ay2Active(), "2AY: пока программа не выбрала второй чип, он не подключён");
                auto ayw2 = [&](uint16_t sel, int r, int v) {
                    m.writeWord(0177714, static_cast<uint16_t>(sel | (~r & 0377)));
                    m.writeByte(0177714, static_cast<uint8_t>(~v));
                };
                ayw2(0100000, 0, 55);                     // второй чип
                ayw2(040000, 0, 77);                      // первый чип
                CHECK(b.ay2Active() && b.ayChip(1).regs()[0] == 55 && b.ayChip(0).regs()[0] == 77,
                      "2AY: 0100000|~рег — второй чип, 040000|~рег — первый");
                ayw2(0, 2, 33);                           // оба
                CHECK(b.ayChip(0).regs()[2] == 33 && b.ayChip(1).regs()[2] == 33, "2AY: оба разряда 0 — оба чипа");
                ayw(4, 11);                               // ~рег целиком: ни один не выбран
                CHECK(b.ayChip(0).regs()[4] == 11 && b.ayChip(1).regs()[4] == 0,
                      "2AY: запись `~рег` без выбора чипа идёт в первый");
                std::vector<uint8_t> st;
                b.saveStateMem(st);
                Board c; c.loadRoms(BK_DEFAULT_ROM_DIR);
                CHECK(c.loadStateMem(st) && c.ay2Active() && c.ayChip(1).regs()[0] == 55,
                      "2AY: второй чип в снимке");
                b.reset();
                CHECK(!b.ay2Active(), "2AY: сброс отключает второй чип");
                // TurboSound: слово, у которого в логическом значении разряды 3–7 единичные,
                // выбирает чип (leCSIcon, «200%»: MOV #0 / MOV #1,@#177714 — логически 0377/0376);
                // обычный `~рег` дальше идёт в выбранный чип.
                m.writeWord(0177714, 1);
                ayw(0, 22);
                CHECK(b.ay2Active() && b.ayTurboChip() == 1 && b.ayChip(1).regs()[0] == 22
                      && b.ayChip(0).regs()[0] == 0, "2AY: TurboSound — слово 1 (логически 0376) выбирает второй чип");
                m.writeWord(0177714, 0);
                ayw(0, 33);
                CHECK(b.ayTurboChip() == 0 && b.ayChip(0).regs()[0] == 33 && b.ayChip(1).regs()[0] == 22,
                      "2AY: TurboSound — слово 0 (логически 0377) выбирает первый чип");
                // Маска «Грифона» главнее выбора TurboSound (так пишет общий плеер leCSIcon).
                m.writeWord(0177714, 1);
                ayw2(040000, 0, 44);
                CHECK(b.ayChip(0).regs()[0] == 44 && b.ayChip(1).regs()[0] == 22,
                      "2AY: разряд 15 = 0 выбирает первый чип и после выбора TurboSound второго");
                // Без платы на два AY выбор чипа не действует — всё в первый.
                b.setAy2(false);
                m.writeWord(0177714, 1);
                ayw(0, 55);
                ayw2(0100000, 1, 7);
                CHECK(!b.ay2Active() && b.ayChip(0).regs()[0] == 55 && b.ayChip(0).regs()[1] == 7,
                      "2AY выключен: выбор чипа не действует, всё идёт в первый AY");
                b.setAy2(true);
            }
            // ---- Covox на 0177714 ----
            {
                b.reset();
                CHECK(b.ay() && b.ay2() && b.covox(), "11М: AY, второй AY и Covox по умолчанию включены");
                Board z;
                z.setMachine(Machine::Bk0010);
                CHECK(!z.ay() && !z.ay2() && !z.covox(), "БК-0010: AY, второго AY и Covox по умолчанию нет");
                auto ayw = [&](int r, int v) {
                    m.writeWord(0177714, static_cast<uint16_t>(~r));
                    m.writeByte(0177714, static_cast<uint8_t>(~v));
                };
                for (int i = 0; i < 4; ++i) ayw(0, i);
                CHECK(b.covoxMode() == Board::CovoxMode::AyMuted,
                      "Covox с AY: слова вперемешку с байтами — протокол AY, ЦАП молчит");
                for (int i = 0; i < 8; ++i) m.writeByte(0177714, static_cast<uint8_t>(0200 + i));
                CHECK(b.covoxMode() == Board::CovoxMode::Active && b.covoxSample() == 0207,
                      "Covox: только байты — звучит");
                for (int i = 0; i < 8; ++i) m.writeWord(0177714, static_cast<uint16_t>(010200 + i));
                CHECK(b.covoxMode() == Board::CovoxMode::Active && b.covoxSample() == 0207,
                      "Covox: только слова — звучит младший байт");
                CHECK(b.covoxStereo() && b.covoxSampleR() == 020,
                      "Covox: только слова — стерео «Мультизвука», старший байт в правый канал");
                m.writeByte(0177715, 0377);
                CHECK(b.covoxSample() == 0207 && b.covoxMode() == Board::CovoxMode::AyMuted,
                      "Covox с AY: байт после слов — протокол AY, ЦАП не меняется");
                b.setAy(false);
                m.writeByte(0177715, 0377);
                CHECK(b.covoxSample() == 0207 && b.covoxSampleR() == 0377 && !b.covoxStereo(),
                      "Covox: байт по нечётному адресу меняет правый ЦАП, с байтами — моно");
                m.writeByte(0177714, 0123);
                CHECK(b.covoxMode() == Board::CovoxMode::Active && b.covoxSample() == 0123,
                      "Covox без AY: любая запись сразу идёт в ЦАП");
                // Пока программа читает порт (джойстик, мышь), записи в него — не звук.
                (void)m.readWord(0177714);
                m.writeWord(0177714, 010);
                CHECK(b.covoxMode() == Board::CovoxMode::PortInput, "Covox: порт опрашивают — ЦАП молчит");
                b.runTicks(2 * 81920);
                // Ступеньки ЦАП слышны: размах на выходе звука.
                b.sound().clear();
                for (int i = 0; i < 2000; ++i) {
                    m.writeByte(0177714, static_cast<uint8_t>((i / 20) & 1 ? 0300 : 0100));
                    b.runTicks(100);
                }
                std::vector<int16_t> pcm(2 * b.sound().available());
                pcm.resize(2 * b.sound().read(pcm.data(), pcm.size() / 2));
                int lo = 0, hi = 0;
                for (int16_t v : pcm) { lo = std::min<int>(lo, v); hi = std::max<int>(hi, v); }
                CHECK(pcm.size() > 1000 && hi - lo > 8000, "Covox: меандр в ЦАП слышен на выходе звука");
                // L − R — только медленный остаток DC-блокеров от прежних записей, меандра в нём нет.
                int dLo = 1 << 20, dHi = -(1 << 20);
                for (size_t i = 0; i + 1 < pcm.size(); i += 2) {
                    dLo = std::min(dLo, pcm[i] - pcm[i + 1]); dHi = std::max(dHi, pcm[i] - pcm[i + 1]);
                }
                CHECK(dHi - dLo < 400, "Covox: байты — моно, один отсчёт в оба канала");
                // Слова: меандр слева, тишина (0200) справа.
                b.sound().clear();
                for (int i = 0; i < 2000; ++i) {
                    m.writeWord(0177714, static_cast<uint16_t>(0100000 | ((i / 20) & 1 ? 0300 : 0100)));
                    b.runTicks(100);
                }
                pcm.assign(2 * b.sound().available(), 0);
                pcm.resize(2 * b.sound().read(pcm.data(), pcm.size() / 2));
                int lL = 0, hL = 0, lR = 0, hR = 0;
                for (size_t i = pcm.size() / 2 & ~size_t(1); i + 1 < pcm.size(); i += 2) {
                    lL = std::min<int>(lL, pcm[i]);     hL = std::max<int>(hL, pcm[i]);
                    lR = std::min<int>(lR, pcm[i + 1]); hR = std::max<int>(hR, pcm[i + 1]);
                }
                CHECK(b.covoxStereo() && hL - lL > 8000 && hR - lR < 200,
                      "Covox: слова — стерео, меандр только в левом канале");
                b.setAy(true);
            }
            // ---- Снимок BK11ST1: ОЗУ 128К и регистры 11М ----
            {
                b.reset();
                for (int i = 0; i < 60; ++i) b.runFrame();
                std::vector<uint8_t> st;
                b.saveStateMem(st);
                std::vector<uint8_t> ram(m.ram11(), m.ram11() + Memory::RAM11_BYTES);
                const uint16_t pc = b.cpu().pc(), pm = m.pageMap(), p662 = b.reg662();
                for (int i = 0; i < 30; ++i) b.runFrame();
                m.ram11()[7 * Memory::PAGE11_BYTES + 5] ^= 0377;
                Board c; c.loadRoms(BK_DEFAULT_ROM_DIR);   // БК-0010: машину переключит сам снимок
                CHECK(c.loadStateMem(st) && c.bk11(), "снимок 11М переключает машину");
                CHECK(std::equal(ram.begin(), ram.end(), c.memory().ram11()) && c.cpu().pc() == pc
                      && c.memory().pageMap() == pm && c.reg662() == p662,
                      "снимок 11М: ОЗУ, PC, страницы и палитра восстановлены");
                CHECK(!c.loadBin("/nonexistent.bin", true), "11М: загрузка .BIN отклоняется");
            }
            // ---- Старт БОС: БЕЙСИК и «Ok» на экране, OCR по шрифту 11М ----
            {
                Board fresh; fresh.setMachine(Machine::Bk0011m); fresh.loadRoms(BK_DEFAULT_ROM_DIR);
                fresh.reset();
                fresh.setScanlineRender(true);
                for (int i = 0; i < 150; ++i) fresh.runFrame();
                Memory& m = fresh.memory();
                std::vector<uint8_t> bits;
                screenBitmap(m, fresh.peekReg(0177664), bits);
                OcrOptions o;
                CHECK(useBk11Font(o, m), "OCR: шрифт 11М из ПЗУ 325");
                o.wide = true;
                o.y0 = 0;   // заголовок БОС — буквы двойной высоты, сбивает автоподбор; сетка от 0
                const std::string txt = ocrAuto(m, bits, o, true, true).text();
                if (txt.find("Ok") == std::string::npos) std::printf("  OCR:\n%s\n", txt.c_str());
                CHECK(txt.find("Ok") != std::string::npos, "11М: БОС поднимает БЕЙСИК — на экране Ok");
            }
        }
    }


    // ---- БК-0011М: БОС грузится с дискеты и после повторного сброса ----------
    // Сброс обнуляет счёт тактов; момент следующего оборота диска раньше оставался из
    // прошлой сессии, диск «стоял», и второй раз дискета не грузилась.
    SECTION("БК-0011М: БОС грузится с дискеты и после повторного сброса")
    {
        Board b; b.setMachine(Machine::Bk0011m);
        if (b.loadRoms(BK_DEFAULT_ROM_DIR)) {
            const std::string img = (std::filesystem::temp_directory_path() / "bk11_boot_test.img").string();
            {
                std::vector<uint8_t> d(819200, 0);
                const uint16_t code[] = {000240, 012737, 0123, 02000, 000777};   // NOP; MOV #123,@#2000; BR .
                for (size_t i = 0; i < sizeof code / sizeof code[0]; ++i) {
                    d[2 * i] = static_cast<uint8_t>(code[i] & 0xff);
                    d[2 * i + 1] = static_cast<uint8_t>(code[i] >> 8);
                }
                FILE* f = std::fopen(img.c_str(), "wb");
                std::fwrite(d.data(), 1, d.size(), f);
                std::fclose(f);
            }
            b.reset();
            CHECK(b.attachDisk(0, img), "11М: дискета вставлена");
            b.bootFromDisk();
            for (int i = 0; i < 300 && b.memory().peekWord(02000) != 0123; ++i) b.runFrame();
            CHECK(b.memory().peekWord(02000) == 0123, "11М: БОС загрузился с дискеты");
            b.memory().pokeWord(02000, 0);
            for (int i = 0; i < 400; ++i) b.runFrame();     // наработать такты дольше окна ожидания ниже
            b.reset();
            for (int i = 0; i < 300 && b.memory().peekWord(02000) != 0123; ++i) b.runFrame();
            CHECK(b.memory().peekWord(02000) == 0123, "11М: повторный сброс снова грузит с дискеты");
            std::filesystem::remove(img);
        }
    }

    // ======================= БК-0011М + СМК-512 ================================
    {
        Board b; b.setMachine(Machine::Bk0011m);
        if (!b.loadRoms(BK_DEFAULT_ROM_DIR)) {
            std::printf("SKIP: ПЗУ БК-0011М не найдены — тесты СМК на 11М пропущены\n");
        } else {
            b.setSmk512(true);
            b.reset();
            Memory& m = b.memory();
            auto smkMode = [&](uint16_t code) {
                m.writeWord(0177130, 6); m.writeWord(0177130, code); m.writeWord(0177130, 0);
            };
            CHECK(b.smk().mode() == Smk512::STD11, "11М+СМК: без прошивки платы пуск в Std11 (а не SYS)");
            // Слово версии платы видно и при ПЗУ драйвера 326 в окне 0160000 (в нём там 0):
            // по нему игры ищут плату — Prince of Persia: CMP @#167776,#177600 ... #177700.
            CHECK(b.memory().readWord(0167776) == Smk512::VERSION && b.memory().peekWord(0167776) == Smk512::VERSION,
                  "11М+СМК: 0167776 — слово версии платы, а не ПЗУ драйвера 326");
            CHECK(b.diskControllerOn(), "11М+СМК: контроллер с ПЗУ на шине и без дискет");
            CHECK(!m.window1Off() && m.readWord(0140000) == 0106427, "11М+СМК Std11: окно 1 и БОС — машины");
            // ВЭД: в SYS/Std10/ОЗУ10/All ОЗУ окна 1 машины отключается.
            for (int pg = 0; pg < 8; ++pg) m.ram11()[pg * Memory::PAGE11_BYTES] = static_cast<uint8_t>(0100 + pg);
            m.writeWord(0177716, 04000);                         // окно 1 — страница 1
            smkMode(0160);                                       // SYS
            CHECK(m.window1Off(), "11М+СМК SYS: ВЭД отключает окно 1");
            b.smk().ram()[6 * Smk512::SEG_BYTES] = 0125;
            CHECK(m.readByte(0120000) == 0125, "11М+СМК SYS: 0120000 — сегмент 6 платы");
            {
                m.pokeWord(04, 02000); m.pokeWord(06, 0340); m.pokeWord(02000, 000777);
                loadProg(m, 01000, {013700, 0100000, 000777});   // MOV @#100000,R0
                b.cpu().reset(01000, 0340);
                b.stepInstruction(); b.stepInstruction();
                CHECK(b.cpu().pc() == 02000, "11М+СМК SYS: 0100000 — ни ОЗУ машины, ни платы: зависание");
            }
            smkMode(0120 | Smk512::pageCode(2));                 // ОЗУ10, страница 2
            m.writeWord(0100000, 0123456);
            CHECK((b.smk().ram()[2 * Smk512::PAGE_BYTES] | (b.smk().ram()[2 * Smk512::PAGE_BYTES + 1] << 8)) == 0123456
                  && m.ram11()[1 * Memory::PAGE11_BYTES] == 0101,
                  "11М+СМК ОЗУ10: 0100000 — сегмент 0 платы, ОЗУ машины не задето");
            smkMode(040);                                        // ОЗУ11
            CHECK(!m.window1Off() && m.readByte(0100000) == 0101, "11М+СМК ОЗУ11: окно 1 — снова машины");
            b.smk().ram()[4 * Smk512::SEG_BYTES] = 0;
            CHECK(m.readWord(0140000) == 0, "11М+СМК ОЗУ11: на месте БОС — сегмент 4 платы");
            // Приём загрузчика leCSIcon: CLR в режиме платы не трогает ОЗУ машины.
            smkMode(0140);
            m.writeWord(0100000, 0123456);
            smkMode(0120 | Smk512::pageCode(2));
            m.writeWord(0100000, 0);
            smkMode(0140);
            CHECK(m.readWord(0100000) == 0123456, "11М+СМК: проверка наличия платы (как в leCSIcon) проходит");
            // «СТОП» возвращает плату в исходный режим машины.
            smkMode(0120);
            b.pressStop();
            CHECK(b.smk().mode() == Smk512::STD11 && !m.window1Off(), "11М+СМК: «СТОП» — снова Std11");
            b.stepInstruction();                                 // забрать прерывание «СТОП»
            // Быстрая память на 4 МГц: код в ОЗУ платы, данные в ДОЗУ (столбец Manwe).
            {
                auto avg = [&](uint16_t ir) {
                    for (uint16_t a = 010000; a < 040000; a += 2) m.pokeWord(a, 033000);
                    for (int i = 0; i < 112; ++i) m.pokeWord(static_cast<uint16_t>(0140000 + 2 * i), ir);
                    b.cpu().reset(0140000, 0340);
                    b.cpu().r[1] = 024000; b.cpu().r[2] = 026000;
                    for (int i = 0; i < 16; ++i) b.stepInstruction();
                    long t = 0;
                    for (int i = 0; i < 96; ++i) t += b.stepInstruction();
                    return double(t) / 96;
                };
                smkMode(040);                                    // ОЗУ11: 0140000 — сегмент 4
                CHECK(avg(010102) == 8.0, "11М+СМК: MOV R1,R2 из ОЗУ платы = 8 (Manwe)");
                CHECK(std::fabs(avg(011102) - 64.0 / 3) < 0.01, "11М+СМК: MOV (R1),R2 = 21,333 (Manwe)");
                CHECK(std::fabs(avg(014102) - 80.0 / 3) < 0.01, "11М+СМК: MOV -(R1),R2 = 26,666 (Manwe)");
                CHECK(avg(010152) == 48.0, "11М+СМК: MOV R1,@-(R2) = 48 (Manwe)");
            }
            // Снимок 11М с платой.
            {
                smkMode(0120 | Smk512::pageCode(5));
                b.smk().ram()[5 * Smk512::PAGE_BYTES + 7] = 0177;
                std::vector<uint8_t> st;
                b.saveStateMem(st);
                Board c; c.loadRoms(BK_DEFAULT_ROM_DIR);
                CHECK(c.loadStateMem(st) && c.bk11() && c.smk512() && c.smk().mode() == Smk512::RAM10
                      && c.smk().page() == 5 && c.smk().ram()[5 * Smk512::PAGE_BYTES + 7] == 0177
                      && c.memory().window1Off(),
                      "снимок 11М+СМК: плата, режим, страница, ДОЗУ и ВЭД восстановлены");
            }
            // БОС с платой: драйвер не находит дискет и возвращает управление — БЕЙСИК.
            {
                Board f; f.setMachine(Machine::Bk0011m); f.loadRoms(BK_DEFAULT_ROM_DIR);
                f.setSmk512(true); f.reset();
                for (int i = 0; i < 800; ++i) f.runFrame();
                std::vector<uint8_t> bits;
                screenBitmap(f.memory(), f.peekReg(0177664), bits);
                OcrOptions o;
                useBk11Font(o, f.memory());
                o.wide = true; o.y0 = 0;
                const std::string txt = ocrAuto(f.memory(), bits, o, true, true).text();
                CHECK(txt.find("Ok") != std::string::npos, "11М+СМК без дискет: БОС доходит до БЕЙСИКа");
            }
        }
    }

    // ---- Вход в прерывание из WAIT: без предвыборки (RTL ВМ1) ----
    SECTION("Вход в прерывание из WAIT: без предвыборки (RTL ВМ1)")
    {
        // Фронт IRQ2 на такте 0 — запись PSW в стек на такте 9 (RTL), а посреди потока
        // команд сначала идёт отброшенная предвыборка. Раньше модель считала её и для WAIT.
        auto entry = [](bool b11, uint16_t idle) {
            Board z; z.setMachine(b11 ? Machine::Bk0011m : Machine::Bk0010);
            z.loadRoms(BK_DEFAULT_ROM_DIR); z.reset();
            Memory& m = z.memory();
            if (b11) m.writeWord(0177662, 0);                 // 11М: кадровое прерывание разрешено
            m.pokeWord(0100, 03000); m.pokeWord(0102, 0340); m.pokeWord(03000, 000777);
            m.pokeWord(01000, idle); m.pokeWord(01002, 000777);
            z.cpu().reset(01000, 0);
            z.runTicks(400);
            const uint64_t t0 = z.totalTicks();
            for (int n = 0; z.cpu().pc() != 03000 && n < 100000; ++n) z.runFrameSlice(n ? 1 : 0, 1000000);
            return static_cast<int>(z.totalTicks() - t0);
        };
        CHECK(entry(true, 000001) == 57 && entry(false, 000001) == 55,
              "WAIT + прерывание: вход без предвыборки (11М 57 тактов, было 68; БК-0010 55, было 59)");
        CHECK(entry(true, 000777) == 65 && entry(false, 000777) == 57,
              "прерывание посреди потока команд — по-прежнему с отброшенной предвыборкой");
    }

    // ---- .BIN на БК-0011М: выбор машины и загрузка через монитор БОС ----
    SECTION(".BIN на БК-0011М: выбор машины и загрузка через монитор БОС")
    {
        const auto dir = std::filesystem::temp_directory_path();
        auto writeBin = [&](const char* name, uint16_t addr, std::initializer_list<uint16_t> words) {
            const std::string path = (dir / name).string();
            FILE* f = std::fopen(path.c_str(), "wb");
            const uint16_t len = static_cast<uint16_t>(words.size() * 2);
            const uint8_t hdr[4] = {uint8_t(addr), uint8_t(addr >> 8), uint8_t(len), uint8_t(len >> 8)};
            std::fwrite(hdr, 1, 4, f);
            for (uint16_t w : words) { const uint8_t b2[2] = {uint8_t(w), uint8_t(w >> 8)}; std::fwrite(b2, 1, 2, f); }
            std::fclose(f);
            return path;
        };
        const std::string p11 = writeBin("bk_t11.bin", 02000, {012737, 0140000, 0177662, 000777});  // MOV #,@#177662
        const std::string pb  = writeBin("bk_tb.bin",  02000, {0110037, 0177663, 000777});          // MOVB R0,@#177663
        const std::string p10 = writeBin("bk_t10.bin", 02000, {013700, 0177662, 000777});           // MOV @#177662,R0
        CHECK(Board::binTargetsBk11(p11) && Board::binTargetsBk11(pb) && !Board::binTargetsBk11(p10),
              ".BIN: запись в 0177662/0177663 — программа для БК-0011М, чтение — нет");
        // Вызов БОС (EMT 51) в коде — 11М; тот же EMT в данных за BR . или при своём векторе
        // EMT (030) — нет.
        const std::string pe  = writeBin("bk_te.bin",  02000, {012700, 1, 0104051, 000777});
        const std::string pd  = writeBin("bk_td.bin",  02000, {000777, 0104051});
        const std::string pv  = writeBin("bk_tv.bin",  02000, {012737, 03000, 030, 0104051, 000777});
        const std::string pbv = writeBin("bk_tbv.bin", 02000, {012737, 0152112, 030, 0104051, 000777});
        CHECK(Board::binTargetsBk11(pe) && !Board::binTargetsBk11(pd) && !Board::binTargetsBk11(pv),
              ".BIN: нечётный EMT (вызов БОС) в достижимом коде — БК-0011М; в данных или со своим EMT — нет");
        CHECK(Board::binTargetsBk11(pbv), ".BIN: вектор EMT в ПЗУ БОС — не свой обработчик");
        for (const auto& p : {pe, pd, pv, pbv}) std::filesystem::remove(p);
        Board b; b.setMachine(Machine::Bk0011m);
        if (b.loadRoms(BK_DEFAULT_ROM_DIR)) {
            b.reset();
            const std::string pr = writeBin("bk_trun.bin", 02000, {012700, 01234, 000777});        // MOV #1234,R0; BR .
            uint16_t a = 0, l = 0;
            const bool ok = b.loadBin(pr, true, &a, &l);
            CHECK(ok && a == 02000 && l == 6 && b.cpu().pc() == 02000 && b.cpu().sp() == 01000
                  && b.cpu().psw == 0 && b.memory().pageMap() == 014400,
                  ".BIN на 11М: «L», «R6/1000», «G» монитора БОС — SP 01000, страницы пользователя 014400");
            b.runFrame();
            CHECK(b.cpu().r[0] == 01234 && b.cpu().pc() == 02004, ".BIN на 11М: программа исполняется");
            std::filesystem::remove(pr);
        }
        for (const auto& p : {p11, pb, p10}) std::filesystem::remove(p);
    }

    // ---- Винчестер СМК-512: образ HDI, регистры 0177740–0177757 в инверсном коде ----
    SECTION("Винчестер СМК-512: образ HDI, регистры 0177740–0177757 в инверсном коде")
    {
        // Синтетический образ: паспорт C/H/S = 4/2/8, слово i сектора n на диске =
        // ~(n << 8 | i), то есть процессор должен прочитать n << 8 | i.
        const int C = 4, H = 2, S = 8, N = C * H * S;
        const std::string path = (std::filesystem::temp_directory_path() / "bk_cpu_tests.hdi").string();
        {
            std::vector<uint8_t> img((N + 1) * 512, 0);
            auto put16 = [&](size_t off, uint16_t v) { img[off] = v & 0xff; img[off + 1] = v >> 8; };
            put16(0, 0x045a); put16(2, C); put16(6, H); put16(12, S);
            const char model[] = "TEST DRIVE";        // ATA: байты в слове переставлены
            for (size_t i = 0; i + 1 < sizeof(model); ++i) img[54 + (i ^ 1)] = static_cast<uint8_t>(model[i]);
            img[510] = 0xA5;
            uint8_t cs = 0;
            for (int i = 0; i < 511; ++i) cs = static_cast<uint8_t>(cs + img[i]);
            img[511] = static_cast<uint8_t>(-cs);
            for (int n = 0; n < N; ++n)
                for (int i = 0; i < 256; ++i) put16((n + 1) * 512 + 2 * i, static_cast<uint16_t>(~(n << 8 | i)));
            FILE* f = std::fopen(path.c_str(), "wb");
            std::fwrite(img.data(), 1, img.size(), f);
            std::fclose(f);
        }
        Board b; b.loadRoms(BK_DEFAULT_ROM_DIR);
        b.setSmk512(true); b.reset();
        Memory& m = b.memory();
        const bool ok = b.attachHdd(0, path, true);
        CHECK(ok && b.ide().drive(0).cylinders() == C && b.ide().drive(0).heads() == H
              && b.ide().drive(0).sectors() == S && b.ide().drive(0).model() == "TEST DRIVE",
              "НЖМД: паспорт HDI — геометрия и модель");
        if (ok) {
            auto st = [&] { return static_cast<uint8_t>(~m.readWord(0177740)); };   // статус в логике ATA
            auto cmd = [&](uint8_t c) { m.writeWord(0177740, static_cast<uint16_t>(~c)); };
            auto chs = [&](int c, int h, int s, int cnt) {
                m.writeWord(0177746, static_cast<uint16_t>(~c & 0xff));
                m.writeWord(0177744, static_cast<uint16_t>(~(c >> 8)));
                m.writeWord(0177742, static_cast<uint16_t>(~(0240 | h)));
                m.writeByte(0177750, static_cast<uint8_t>(~s));
                m.writeWord(0177752, static_cast<uint16_t>(~cnt));
            };
            // Программный сброс так, как его делает прошивка СМК: CLR @#177743 / MOV #6,@#177743 —
            // СЛОВОМ по нечётному адресу. Без A0 это была бы запись в регистр головки 0177742.
            m.writeWord(0177742, static_cast<uint16_t>(~0257));   // головка 15 — чтобы было что сбросить
            m.writeWord(0177743, 0);
            m.writeWord(0177743, 6);
            CHECK((st() & 0xc0) == 0x40 && static_cast<uint8_t>(~m.readWord(0177742)) == 0
                  && static_cast<uint8_t>(~m.readWord(0177754)) == 1,
                  "НЖМД: сброс словом в 0177743 — DRDY, регистры — сигнатура ATA");
            CHECK(static_cast<uint8_t>(m.readByte(0177743)) == static_cast<uint8_t>(m.readWord(0177740)),
                  "НЖМД: 0177743 по чтению — второй статус (байт по нечётному адресу)");
            cmd(0xec);
            bool idOk = (st() & 0x08) != 0;
            uint16_t id[256];
            for (int i = 0; i < 256; ++i) id[i] = static_cast<uint16_t>(~m.readWord(0177756));
            idOk = idOk && id[1] == C && id[3] == H && id[6] == S && !(st() & 0x08);
            CHECK(idOk, "НЖМД: IDENTIFY отдаёт паспорт из заголовка HDI, DRQ снимается");
            // Два сектора с C/H/S = 1/1/3: LBA (1*2+1)*8+2 = 26 и следующий 27.
            chs(1, 1, 3, 2);
            cmd(0x20);
            bool rd = true;
            for (int n = 26; n <= 27; ++n)
                for (int i = 0; i < 256; ++i) rd = rd && m.readWord(0177756) == (n << 8 | i);
            CHECK(rd && (st() & 0x09) == 0, "НЖМД: чтение двух секторов по CHS, данные проинвертированы платой");
            // Запись: в образ только для чтения — в память поверх файла.
            chs(0, 0, 6, 1);                                       // LBA 5
            cmd(0x30);
            for (int i = 0; i < 256; ++i) m.writeWord(0177756, static_cast<uint16_t>(0123000 + i));
            chs(0, 0, 6, 1);
            cmd(0x20);
            bool wr = true;
            for (int i = 0; i < 256; ++i) wr = wr && m.readWord(0177756) == 0123000 + i;
            uint8_t disk[2] = {0, 0};
            if (FILE* f = std::fopen(path.c_str(), "rb")) {
                std::fseek(f, (5 + 1) * 512, SEEK_SET);
                if (std::fread(disk, 1, 2, f) != 2) disk[0] = disk[1] = 0;
                std::fclose(f);
            }
            CHECK(wr && b.ide().drive(0).dirtySectors() == 1
                  && static_cast<uint16_t>(disk[0] | disk[1] << 8) == static_cast<uint16_t>(~(5 << 8)),
                  "НЖМД: запись читается обратно, а файл образа не тронут (только чтение)");
            chs(0, 0, S + 1, 1);                                   // сектора S+1 нет
            cmd(0x20);
            CHECK((st() & 0x01) && (static_cast<uint8_t>(~m.readWord(0177754)) & 0x14) == 0x14,
                  "НЖМД: сектор вне геометрии — ERR, ошибка IDNF|ABRT");
            // Снять плату — регистров НЖМД на шине нет; поставить обратно — есть.
            b.setSmk512(false);
            Memory& m2 = b.memory();
            uint16_t v = 0;
            CHECK(!(m2.mpi() && m2.mpi()->mpiPeek(0177740, v)), "НЖМД: без платы СМК регистров 0177740 нет");
        }
        std::filesystem::remove(path);
    }

    // ---- Винчестер на контроллере «Самара»: разметка, регистры 0177620–0177656, загрузка ----
    SECTION("Винчестер на контроллере «Самара»: разметка, регистры 0177620–0177656, загрузка")
    {
        // Образ C/H/S = 20/2/16 в разметке «Самары»: блок 1 — геометрия и таблица дисков,
        // блок 3 — заголовок диска C:, блок 4 — его загрузчик (в 01000):
        //   NOP; MOV #123456,@#500; MOV R0,@#502; BR .
        const int C = 20, H = 2, S = 16, N = C * H * S;
        const std::string path = (std::filesystem::temp_directory_path() / "bk_cpu_tests_samara.hdi").string();
        {
            std::vector<uint8_t> img((N + 1) * 512, 0);
            auto put16 = [&](size_t off, uint16_t v) { img[off] = v & 0xff; img[off + 1] = v >> 8; };
            put16(0, 0x045a); put16(2, C); put16(6, H); put16(12, S);
            img[510] = 0xA5;
            uint8_t cs = 0;
            for (int i = 0; i < 511; ++i) cs = static_cast<uint8_t>(cs + img[i]);
            img[511] = static_cast<uint8_t>(-cs);
            std::vector<uint16_t> blk(N * 256, 0);         // в логике процессора
            const uint16_t b1[] = {2, H * S, static_cast<uint16_t>(S | (H - 1) << 8), 3, 0};
            std::copy(std::begin(b1), std::end(b1), blk.begin() + 1 * 256);
            const uint16_t hdr[] = {2, 100, 010, 02000, 01000, 014400};   // флаг 010 — защита от записи
            std::copy(std::begin(hdr), std::end(hdr), blk.begin() + 3 * 256);
            const uint16_t boot[] = {000240, 012737, 0123456, 0500, 010037, 0502, 000777};
            std::copy(std::begin(boot), std::end(boot), blk.begin() + 4 * 256);
            for (int i = 0; i < N * 256; ++i) put16(512 + 2 * i, static_cast<uint16_t>(~blk[i]));
            FILE* f = std::fopen(path.c_str(), "wb");
            std::fwrite(img.data(), 1, img.size(), f);
            std::fclose(f);
        }
        HddDrive::Reg r;
        CHECK(Ide::decodeSamara(0177640, r) && r == HddDrive::STATUS_CMD && Ide::decodeSamara(0177656, r)
              && r == HddDrive::DATA && Ide::decodeSamara(0177620, r) && r == HddDrive::DRIVE_ADDR
              && Ide::decodeSamara(0177622, r) && r == HddDrive::CTRL && !Ide::decodeSamara(0177660, r)
              && !Ide::decodeSamara(0177624, r),
              "«Самара»: регистры 0177640–0177656 — блок CS0 по убыванию, CS1 — 0177620/0177622");
        CHECK(hdiLayout(path) == HddMap::Kind::Samara, "«Самара»: разметка опознана по блоку 1");
        for (int mach = 0; mach < 2; ++mach) {
            const bool b11 = mach == 1;
            // На БК-0010 загрузчику нужна память вместо Бейсика (у КНГМД БК-0010-01 там
            // ОЗУ 16 Кбайт; Бейсик держит стек в 01762, а блок читается в 01000–01777) — её
            // даёт СМК-512, как и при загрузке с дискеты. На 11М плата не нужна.
            Board b;
            b.setMachine(b11 ? Machine::Bk0011m : Machine::Bk0010);
            if (!b.loadRoms(BK_DEFAULT_ROM_DIR)) break;
            b.setSmk512(!b11);
            b.reset();
            const bool ok = b.attachHdd(0, path, true);
            CHECK(ok && b.hddSamara() && b.hddOnBus(),
                  b11 ? "«Самара» на 11М: авто по разметке, без платы СМК"
                      : "«Самара» на БК-0010: авто по разметке, вместе с СМК");
            if (!ok) continue;
            if (!b11) {
                const HddMap map = readHddMap(b.ide().drive(0));
                CHECK(map.kind == HddMap::Kind::Samara && map.bootUnit == 2 && map.disks.size() == 1
                      && map.disks[0].unit == 2 && map.disks[0].first == 4 && map.disks[0].blocks == 100
                      && map.disks[0].flags == 010,
                      "«Самара»: таблица дисков — C: с блока 4, 100 блоков, защита от записи");
                uint16_t v = 0;
                CHECK(b.memory().mpi() && !b.memory().mpi()->mpiPeek(0177740, v),
                      "«Самара»: регистров СМК 0177740 на шине нет");
            }
            if (b11) {
                // Имена ПЗУ контроллера (roms/samara.sym) — нижний слой таблицы символов.
                uint16_t a = 0;
                const std::string* n = b.symbols().nameAt(0164316);
                CHECK(n && *n == "GRUZ0D" && b.symbols().addrOf("T.BOOT", a) && a == 0160000
                      && b.symbols().all().empty(),
                      "«Самара»: имена ПЗУ контроллера видны, в свои символы не попадают");
                b.symbols().set(0164316, "MY");
                n = b.symbols().nameAt(0164316);
                CHECK(n && *n == "MY", "Символы: своё имя главнее имени из ПЗУ");
            }
            b.bootFromHdd();
            for (int f = 0; f < 150 && b.memory().peekWord(0500) != 0123456; ++f) b.runFrame();
            CHECK(b.memory().peekWord(0500) == 0123456 && b.memory().peekWord(0502) == 2,
                  b11 ? "«Самара» на 11М: БОС зовёт 0160000, ПЗУ грузит C: с винчестера, R0 = 2"
                      : "«Самара» на БК-0010: 160000 грузит C: с винчестера, R0 = 2");
        }
        {
            Board b; b.loadRoms(BK_DEFAULT_ROM_DIR);
            b.setHddController(Board::HddController::Smk);
            b.setSmk512(true); b.reset();
            CHECK(b.attachHdd(0, path, true) && !b.hddSamara(), "«Самара»: --hdd-ctl smk перекрывает разметку");
        }
        std::filesystem::remove(path);
    }

    // ---- Видео: ZMBV туда и обратно, AVI с частотой БК, продолжение в новом файле ----
    SECTION("Видео: ZMBV туда и обратно, AVI с частотой БК, продолжение в новом файле")
    {
        // Минимальный декодер ZMBV (8 бит, по ffmpeg zmbv.c): кадр -> ARGB.
        struct Dec {
            int w, h; z_stream s{}; uint8_t pal[768] = {};
            std::vector<uint8_t> prev, cur, buf;
            Dec(int w_, int h_) : w(w_), h(h_), prev(w_ * h_, 0), cur(w_ * h_, 0), buf(w_ * h_ * 2 + 4096) { inflateInit(&s); }
            ~Dec() { inflateEnd(&s); }
            bool frame(const std::vector<uint8_t>& d, std::vector<uint32_t>& out) {
                size_t p = 1;
                const uint8_t fl = d[0];
                if (fl & ZmbvEncoder::KEYFRAME) {
                    if (d[1] != 0 || d[2] != 1 || d[3] != 1 || d[4] != ZmbvEncoder::FMT_8BPP || d[5] != 16 || d[6] != 16) return false;
                    p = 7;
                    inflateReset(&s);
                }
                s.next_in = const_cast<uint8_t*>(d.data() + p); s.avail_in = uInt(d.size() - p);
                s.next_out = buf.data(); s.avail_out = uInt(buf.size());
                if (inflate(&s, Z_SYNC_FLUSH) != Z_OK) return false;
                const uint8_t* b = buf.data();
                if (fl & ZmbvEncoder::KEYFRAME) {
                    std::copy(b, b + 768, pal);
                    std::copy(b + 768, b + 768 + w * h, cur.begin());
                } else {
                    if (fl & ZmbvEncoder::DELTA_PALETTE) { for (int i = 0; i < 768; ++i) pal[i] ^= b[i]; b += 768; }
                    const int bx = (w + 15) / 16, by = (h + 15) / 16;
                    const uint8_t* vec = b;
                    const uint8_t* x8 = b + ((bx * by * 2 + 3) & ~3);
                    for (int j = 0; j < by; ++j)
                        for (int i = 0; i < bx; ++i) {
                            const int k = j * bx + i;
                            const int dx = int8_t(vec[2 * k]) >> 1, dy = int8_t(vec[2 * k + 1]) >> 1;
                            const bool xr = vec[2 * k] & 1;
                            const int bw = std::min(16, w - i * 16), bh = std::min(16, h - j * 16);
                            for (int y = 0; y < bh; ++y)
                                for (int x = 0; x < bw; ++x) {
                                    const int sx = i * 16 + x + dx, sy = j * 16 + y + dy;
                                    uint8_t v = (sx < 0 || sy < 0 || sx >= w || sy >= h) ? 0 : prev[sy * w + sx];
                                    if (xr) v ^= *x8++;
                                    cur[(j * 16 + y) * w + i * 16 + x] = v;
                                }
                        }
                }
                out.resize(w * h);
                for (int i = 0; i < w * h; ++i)
                    out[i] = 0xFF000000u | pal[cur[i] * 3] << 16 | pal[cur[i] * 3 + 1] << 8 | pal[cur[i] * 3 + 2];
                prev = cur;
                return true;
            }
        };
        const int W = 72, H = 40;                       // края блоков неполные: 72 = 4·16+8, 40 = 2·16+8
        auto scene = [&](int t) {                       // полосы, ползущие вправо; с 5-го кадра — новые цвета
            std::vector<uint32_t> f(W * H);
            const uint32_t c[5] = {0xFF000000u, 0xFF0000FFu, 0xFF00FF00u, 0xFFFF0000u, 0xFFFFFF00u};
            for (int y = 0; y < H; ++y)
                for (int x = 0; x < W; ++x)
                    f[y * W + x] = c[((x - t) / 3 + y / 7 + 100) % (t >= 5 ? 5 : 3)];
            if (t == 9) f[5 * W + 7] = 0xFF123456u;     // одна точка нового цвета
            return f;
        };
        ZmbvEncoder enc(W, H, 4, 7);                     // опорный — каждые 7 кадров
        Dec dec(W, H);
        bool rt = true, sawDeltaPal = false, sawVector = false, keys = true;
        std::vector<uint8_t> pkt; std::vector<uint32_t> back;
        for (int t = 0; t < 20 && rt; ++t) {
            const std::vector<uint32_t> f = scene(t);
            bool key = false;
            rt = enc.encode(f.data(), false, pkt, key) && dec.frame(pkt, back) && back == f;
            keys = keys && key == (t % 7 == 0);
            sawDeltaPal |= !key && (pkt[0] & ZmbvEncoder::DELTA_PALETTE);
        }
        // Сдвиг на точку должен находиться вектором, а не XOR всего блока.
        {
            ZmbvEncoder e2(64, 32, 4, 1000);
            std::vector<uint32_t> a(64 * 32), b2(64 * 32);
            for (int i = 0; i < 64 * 32; ++i) a[i] = (((i % 64) * 7 + (i / 64) * 13) % 5) ? 0xFFFFFFFFu : 0xFF0000FFu;
            for (int y = 0; y < 32; ++y) for (int x = 0; x < 64; ++x) b2[y * 64 + x] = a[y * 64 + std::max(0, x - 1)];
            std::vector<uint8_t> p1, p2; bool k;
            e2.encode(a.data(), true, p1, k);
            e2.encode(b2.data(), false, p2, k);
            Dec d2(64, 32); std::vector<uint32_t> o;
            sawVector = d2.frame(p1, o) && d2.frame(p2, o) && o == b2 && p2.size() < 64;
        }
        CHECK(rt && keys, "ZMBV: 20 кадров туда и обратно без потерь, опорные — по интервалу");
        CHECK(sawDeltaPal, "ZMBV: новые цвета — изменение палитры в промежуточном кадре");
        CHECK(sawVector, "ZMBV: сдвиг картинки кодируется векторами блоков (кадр меньше 64 байт)");

        // AVI: частота 3125/64 из такта ЦП и длины кадра, размеры в заголовках, продолжение файла.
        const auto dir = std::filesystem::temp_directory_path();
        const std::string base = (dir / "bk_cpu_tests_video.avi").string();
        const std::string part2 = (dir / "bk_cpu_tests_video_002.avi").string();
        std::filesystem::remove(base); std::filesystem::remove(part2);
        VideoCapture vc;
        VideoCapture::Options vo;
        vo.aspect43 = false;
        vo.splitBytes = 1000;                            // маленький порог — проверить продолжение
        bool started = vc.start(base, vo, W, H, 3000000, 61440);
        for (int t = 0; t < 30 && started; ++t) { const auto f = scene(t * 3); vc.addFrame(f.data()); }
        const bool stopped = vc.stop();
        auto rd = [](const std::string& p) {
            std::ifstream in(p, std::ios::binary);
            return std::vector<uint8_t>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        };
        auto u32 = [](const std::vector<uint8_t>& v, size_t o) { return o + 4 <= v.size() ? uint32_t(v[o] | v[o+1] << 8 | v[o+2] << 16 | uint32_t(v[o+3]) << 24) : 0u; };
        const std::vector<uint8_t> a1 = rd(base), a2 = rd(part2);
        auto find = [](const std::vector<uint8_t>& v, const char* tag) {
            for (size_t i = 0; i + 4 <= v.size(); ++i) if (!std::memcmp(&v[i], tag, 4)) return i;
            return size_t(-1);
        };
        bool hdr = a1.size() > 200 && !std::memcmp(a1.data(), "RIFF", 4) && u32(a1, 4) == a1.size() - 8;
        const size_t strh = find(a1, "strh"), avih = find(a1, "avih"), idx = find(a1, "idx1");
        uint32_t frames1 = 0, frames2 = 0;
        if (hdr && strh != size_t(-1) && avih != size_t(-1) && idx != size_t(-1)) {
            frames1 = u32(a1, avih + 8 + 16);
            hdr = u32(a1, strh + 8 + 20) == 64 && u32(a1, strh + 8 + 24) == 3125   // dwScale, dwRate
                  && u32(a1, avih + 8) == 20480                                     // мкс на кадр
                  && u32(a1, strh + 8 + 32) == frames1 && u32(a1, idx + 4) == frames1 * 16
                  && find(a1, "vprp") != size_t(-1);
        }
        const size_t avih2 = find(a2, "avih");
        if (avih2 != size_t(-1)) frames2 = u32(a2, avih2 + 8 + 16);
        CHECK(started && stopped && hdr, "Видео: AVI 3125/64 кадра/с (20480 мкс), счётчики кадров и idx1 сходятся");
        const std::string part3 = (dir / "bk_cpu_tests_video_003.avi").string();
        CHECK(frames1 > 0 && frames2 > 0 && !std::filesystem::exists(part3) && frames1 + frames2 == 30,
              "Видео: файл больше порога продолжается в имя_002.avi, кадры не теряются");
        std::filesystem::remove(base); std::filesystem::remove(part2); std::filesystem::remove(part3);

        // Звук: отвод Speaker пишет каждый отсчёт и при выключенном выводе на карту; за
        // секунду эмуляции — ровно частота отсчётов.
        {
            Speaker spk(44100, 3000000);
            spk.setEnabled(false);
            spk.setCapture(true);
            for (int i = 0; i < 1000; ++i) spk.feed((i & 1) ? 4 : 0, 3000);   // 1000 x 1 мс
            std::vector<int16_t> got;
            spk.takeCaptured(got);
            bool loud = false;
            for (int16_t v : got) loud |= v > 1000 || v < -1000;
            CHECK(got.size() == 2 * 44100 && spk.available() == 0 && loud,
                  "Видео: отвод звука — 44100 стерео-кадров за секунду, при выключенном выводе тоже");
        }
        // AVI со звуком: второй поток PCM, звук кадра — сразу за его картинкой.
        {
            const std::string wav = (dir / "bk_cpu_tests_video_audio.avi").string();
            VideoCapture va;
            VideoCapture::Options ao;
            ao.audioRate = 44100;
            std::vector<int16_t> snd(2 * 904, 1234);     // стерео-кадры: L, R
            bool okA = va.start(wav, ao, W, H, 4000000, 81920);
            for (int t = 0; t < 10 && okA; ++t) { const auto f = scene(t); va.addFrame(f.data(), snd.data(), 903 + (t & 1)); }
            okA = okA && va.stop();
            const std::vector<uint8_t> v = rd(wav);
            const size_t avihA = find(v, "avih");
            size_t auds = size_t(-1);
            for (size_t i = 0; i + 4 <= v.size(); ++i) if (!std::memcmp(&v[i], "auds", 4)) { auds = i; break; }
            int dc = 0, wb = 0;
            const size_t movi = find(v, "movi");
            for (size_t p2 = movi + 4; movi != size_t(-1) && p2 + 8 <= v.size();) {
                if (!std::memcmp(&v[p2], "idx1", 4)) break;
                dc += !std::memcmp(&v[p2], "00dc", 4);
                wb += !std::memcmp(&v[p2], "01wb", 4);
                const uint32_t n = u32(v, p2 + 4);
                p2 += 8 + n + (n & 1);
            }
            const bool hdrA = avihA != size_t(-1) && u32(v, avihA + 8 + 24) == 2 && auds != size_t(-1)
                              && u32(v, auds + 20) == 4 && u32(v, auds + 24) == 176400    // стерео: блок 4, байт/с
                              && u32(v, auds + 32) == 10 * 903 + 5;                        // dwLength — отсчёты
            CHECK(okA && hdrA && dc == 10 && wb == 10,
                  "Видео: звук PCM 44,1 кГц стерео вторым потоком, кусок на кадр, длина дорожки в кадрах");
            std::filesystem::remove(wav);
        }
        CHECK(videoCodecForPath("/x/a.MP4") && videoCodecForPath("/x/a.MP4")->name == "h264"
              && videoCodecForPath("a.avi") && videoCodecForPath("a.avi")->name == "zmbv"
              && !videoCodecForPath("a.mkv") && !videoCodecForPath("dir.mp4/a") && findVideoCodec("zmbv"),
              "Видео: кодек по расширению (.mp4 — h264, .avi — zmbv), ZMBV есть всегда");
#ifdef BK_HAVE_FFMPEG
        // MP4 для Telegram: H.264 + AAC, faststart, частота 3125/64, кадр всегда 2x3.
        if (findVideoCodec("h264")) {
            const std::string mp4 = (dir / "bk_cpu_tests_video.mp4").string();
            VideoCapture vm;
            VideoCapture::Options mo;
            mo.codec = "h264";
            mo.aspect43 = false;                       // MP4 сам переходит на 2x3
            mo.audioRate = 44100;
            std::vector<int16_t> tone(2 * 903);          // стерео: тон слева, тишина справа
            for (size_t i = 0; i < 903; ++i) tone[2 * i] = int16_t((i / 50) & 1 ? 8000 : -8000);
            bool okM = vm.start(mp4, mo, W, H, 3000000, 61440);
            for (int t = 0; t < 60 && okM; ++t) { const auto f = scene(t); vm.addFrame(f.data(), tone.data(), 903); }
            okM = okM && vm.stop();
            const std::vector<uint8_t> v = rd(mp4);
            const size_t moov = find(v, "moov"), mdat = find(v, "mdat");
            AVFormatContext* fc = nullptr;
            bool streams = false, picture = false;
            double vdur = 0, adur = 0;
            if (okM && avformat_open_input(&fc, mp4.c_str(), nullptr, nullptr) == 0 && avformat_find_stream_info(fc, nullptr) >= 0) {
                const AVStream* vs = nullptr; const AVStream* as = nullptr;
                for (unsigned i = 0; i < fc->nb_streams; ++i) {
                    if (fc->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) vs = fc->streams[i];
                    if (fc->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) as = fc->streams[i];
                }
                streams = vs && as && vs->codecpar->codec_id == AV_CODEC_ID_H264 && as->codecpar->codec_id == AV_CODEC_ID_AAC
                          && vs->codecpar->width == 2 * W && vs->codecpar->height == 3 * H
                          && vs->codecpar->format == AV_PIX_FMT_YUV420P
                          && vs->avg_frame_rate.num * 64 == vs->avg_frame_rate.den * 3125
                          && as->codecpar->sample_rate == 44100 && as->codecpar->ch_layout.nb_channels == 2;
                if (vs) vdur = double(vs->duration) * av_q2d(vs->time_base);
                if (as) adur = double(as->duration) * av_q2d(as->time_base);
                // Первый кадр: декодировать и сравнить с исходным (сжатие с потерями).
                if (vs) {
                    const AVCodec* dc = avcodec_find_decoder(vs->codecpar->codec_id);
                    AVCodecContext* cc = avcodec_alloc_context3(dc);
                    avcodec_parameters_to_context(cc, vs->codecpar);
                    AVPacket* pk = av_packet_alloc(); AVFrame* fr = av_frame_alloc();
                    if (avcodec_open2(cc, dc, nullptr) == 0) {
                        bool got = false;
                        while (!got && av_read_frame(fc, pk) >= 0) {
                            if (pk->stream_index == vs->index && avcodec_send_packet(cc, pk) >= 0)
                                got = avcodec_receive_frame(cc, fr) == 0;
                            av_packet_unref(pk);
                        }
                        if (!got) { avcodec_send_packet(cc, nullptr); got = avcodec_receive_frame(cc, fr) == 0; }
                        if (got) {
                            std::vector<uint32_t> rgb(size_t(2 * W) * 3 * H);
                            SwsContext* sc = sws_getContext(fr->width, fr->height, AVPixelFormat(fr->format), fr->width, fr->height,
                                                            AV_PIX_FMT_BGRA, SWS_POINT, nullptr, nullptr, nullptr);
                            sws_setColorspaceDetails(sc, sws_getCoefficients(SWS_CS_ITU709), 0,
                                                     sws_getCoefficients(SWS_CS_DEFAULT), 1, 0, 1 << 16, 1 << 16);
                            uint8_t* dst[1] = {reinterpret_cast<uint8_t*>(rgb.data())};
                            const int ds[1] = {2 * W * 4};
                            sws_scale(sc, fr->data, fr->linesize, 0, fr->height, dst, ds);
                            sws_freeContext(sc);
                            const auto f0 = scene(0);
                            double e = 0;
                            for (int y = 0; y < H; ++y)
                                for (int x = 0; x < W; ++x) {
                                    const uint32_t a = f0[y * W + x], b = rgb[size_t(3 * y + 1) * 2 * W + 2 * x];
                                    for (int k = 0; k < 24; k += 8) e += std::abs(int((a >> k) & 255) - int((b >> k) & 255));
                                }
                            picture = e / (W * H * 3) < 8.0;
                        }
                    }
                    av_frame_free(&fr); av_packet_free(&pk); avcodec_free_context(&cc);
                }
                avformat_close_input(&fc);
            }
            const double want = 60 * 64.0 / 3125;
            CHECK(okM && streams, "MP4: H.264 yuv420p 2x3 кадра + AAC 44,1 кГц стерео, 3125/64 кадра/с");
            CHECK(moov != size_t(-1) && mdat != size_t(-1) && moov < mdat, "MP4: faststart — moov перед mdat");
            CHECK(std::fabs(vdur - want) < 0.03 && std::fabs(adur - want) < 0.05,
                  "MP4: длительности видео и звука — 60 кадров БК (1,229 с)");
            CHECK(picture, "MP4: первый кадр после сжатия близок к исходному");
            std::filesystem::remove(mp4);
        }
#endif
    }

    std::printf("\n%d/%d checks passed\n", g_total - g_fail, g_total);
    return g_fail ? 1 : 0;
}
