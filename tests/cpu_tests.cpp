// Minimal unit tests for the BK-0010 CPU core.
// Loads short instruction sequences into RAM and checks register/flag results.
#include "Cpu.h"
#include "Memory.h"
#include "Disasm.h"
#include "Board.h"
#include <cstdio>
#include <cstdint>
#include <string>

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
        const char* tmp = "/tmp/claude-1000/bk_state_test.bkst";
        bool saved = b.saveState(tmp);
        CHECK(saved, "saveState succeeds");
        // Corrupt everything, then restore.
        b.memory().pokeWord(01000, 0);
        b.memory().pokeWord(042000, 0);
        b.cpu().r[3] = 0; b.cpu().r[7] = 0; b.cpu().psw = 0;
        bool loaded = b.loadState(tmp);
        CHECK(loaded, "loadState succeeds");
        CHECK(b.memory().peekWord(01000) == 0123456, "RAM restored");
        CHECK(b.memory().peekWord(042000) == 0154321, "video RAM restored");
        CHECK(b.cpu().r[3] == 07777, "R3 restored");
        CHECK(b.cpu().r[7] == 02000, "PC restored");
        CHECK(b.cpu().psw == 017, "PSW restored");
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
        // Register 0177716 bit 6 (key-pressed, active-low) is 0 while a key waits.
        CHECK(!(b.memory().readWord(0177716) & 0100), "keyboard: 0177716 key-pressed bit low");
        CHECK(!b.pressKey(0102), "keyboard: second key dropped while register busy");
        CHECK((b.memory().readWord(0177662) & 0177) == 0101, "keyboard: register holds first code");
        CHECK(!b.keyReady(), "keyboard: ready flag cleared on read");
        CHECK(b.memory().readWord(0177716) & 0100, "keyboard: 0177716 key-pressed bit high again");
        CHECK(b.pressKey(0102), "keyboard: next key latched after previous was read");
        CHECK((b.memory().readWord(0177662) & 0177) == 0102, "keyboard: register holds second code");
    }

    std::printf("\n%d/%d checks passed\n", g_total - g_fail, g_total);
    return g_fail ? 1 : 0;
}
