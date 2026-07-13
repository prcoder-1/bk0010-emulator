#pragma once
#include <cstdint>
#include <functional>
#include "Memory.h"

namespace bk {

// K1801VM1 (PDP-11 compatible) CPU core.
// Faithfully ports the decode/flag semantics of the 'bk' reference emulator.
class Cpu {
public:
    // General registers: r[0..5] general, r[6]=SP, r[7]=PC.
    uint16_t r[8] = {0};
    uint16_t psw = 0;        // processor status word (low 8 bits used)

    // PSW condition-code bits.
    enum : uint16_t { CC_C = 001, CC_V = 002, CC_Z = 004, CC_N = 010, CC_T = 020, CC_PRI = 0200 };

    // Result codes from executing one instruction.
    enum Result { OK = 0, R_ILLEGAL, R_HALT, R_WAIT, R_EMT, R_TRAP, R_IOT, R_BPT, R_RTT };

    // Standard interrupt/trap vectors (octal).
    enum : uint16_t {
        VEC_BUS_ERROR = 0004, VEC_RESERVED = 0010, VEC_TBIT = 0014,
        VEC_IOT = 0020, VEC_POWERFAIL = 0024, VEC_EMT = 0030, VEC_TRAP = 0034,
        VEC_KEYBOARD = 0060, VEC_IRQ2 = 0100,
    };

    explicit Cpu(Memory& mem) : mem_(mem) { buildTable(); }

    void reset(uint16_t pc, uint16_t initPsw = 0);

    // Execute one instruction. Returns the number of CPU ticks it consumed.
    // Traps (EMT/TRAP/IOT/BPT/illegal) are serviced internally via vectors.
    int step();

    // Deliver a hardware interrupt via the given vector (used by devices).
    // Pushes PSW/PC and loads the new PC/PSW from the vector. No masking here —
    // the caller checks priority/mask bits before calling.
    void interrupt(uint16_t vector);

    bool halted() const { return halted_; }
    bool waiting() const { return waiting_; }
    void clearHalt() { halted_ = false; }
    void clearWait() { waiting_ = false; }

    uint16_t pc() const { return r[7]; }
    uint16_t sp() const { return r[6]; }
    uint16_t lastBranch() const { return lastBranch_; }

    // Hook for intercepting EMT 36 (tape/disk file I/O). Called when an
    // `EMT 036` instruction executes; if it returns true the call is considered
    // handled and the ROM handler (vector 030) is skipped. See Board::handleEmt36.
    void setEmt36Hook(std::function<bool()> h) { emt36Hook_ = std::move(h); }

private:
    Memory& mem_;
    uint16_t ir_ = 0;          // current instruction register
    uint16_t eaAddr_ = 0;      // cached destination address (modify-in-place)
    uint16_t lastBranch_ = 0;  // PC of the last control-flow instruction
    bool halted_ = false;
    bool waiting_ = false;
    std::function<bool()> emt36Hook_;  // EMT 36 intercept (true = handled, skip ROM)

    // Instruction-field accessors (bits of ir_).
    int srcMode() const { return (ir_ & 07000) >> 9; }
    int srcReg()  const { return (ir_ & 00700) >> 6; }
    int dstMode() const { return (ir_ & 00070) >> 3; }
    int dstReg()  const { return ir_ & 00007; }

    // Memory helpers (word access masks odd addresses like the real bus).
    uint16_t rword(uint16_t a) { return mem_.readWord(a); }
    uint8_t  rbyte(uint16_t a) { return mem_.readByte(a); }
    void     wword(uint16_t a, uint16_t v) { mem_.writeWord(a, v); }
    void     wbyte(uint16_t a, uint8_t v)  { mem_.writeByte(a, v); }

    // Stack.
    void push(uint16_t v) { r[6] -= 2; wword(r[6], v); }
    uint16_t pop() { uint16_t v = rword(r[6]); r[6] += 2; return v; }

    // Effective-address load/store (ports ea.c).
    uint16_t loadSrc();
    uint16_t loadDst();
    uint8_t  loadbSrc();
    uint8_t  loadbDst();
    void     storeDst(uint16_t v);
    void     storeDst2(uint16_t v);   // uses eaAddr_ cache
    void     storebDst(uint8_t v);
    void     storebDst2(uint8_t v);
    uint16_t loadEa();                // for JMP/JSR (address, not value)

    void service(uint16_t vector);    // trap/interrupt dispatch

    // Condition-code helpers.
    void setN() { psw |= CC_N; } void clrN() { psw &= ~CC_N; }
    void setZ() { psw |= CC_Z; } void clrZ() { psw &= ~CC_Z; }
    void setV() { psw |= CC_V; } void clrV() { psw &= ~CC_V; }
    void setC() { psw |= CC_C; } void clrC() { psw &= ~CC_C; }
    void clrAll() { psw &= ~(CC_N | CC_Z | CC_V | CC_C); }
    void chgN(uint16_t d)  { (d & 0100000) ? setN() : clrN(); }
    void chgNb(uint8_t d)  { (d & 0200) ? setN() : clrN(); }
    void chgZ(uint16_t d)  { (d) ? clrZ() : setZ(); }
    void chgZb(uint8_t d)  { (d & 0xff) ? clrZ() : setZ(); }
    void chgC(uint32_t d)  { (d & 0200000) ? setC() : clrC(); }
    void chgIC(uint32_t d) { (d & 0200000) ? clrC() : setC(); }   // inverted (sub/cmp)
    void chgICb(uint16_t d){ (d & 0400) ? clrC() : setC(); }
    void chgV(uint16_t a, uint16_t b, uint16_t r3) {
        (((a & 0100000) == (b & 0100000)) && ((a & 0100000) != (r3 & 0100000))) ? setV() : clrV();
    }
    void chgVb(uint8_t a, uint8_t b, uint8_t r3) {
        (((a & 0200) == (b & 0200)) && ((a & 0200) != (r3 & 0200))) ? setV() : clrV();
    }
    void chgVC(uint16_t a, uint16_t b, uint16_t r3) { // cmp: src - dst
        (((a & 0100000) != (b & 0100000)) && ((b & 0100000) == (r3 & 0100000))) ? setV() : clrV();
    }
    void chgVCb(uint8_t a, uint8_t b, uint8_t r3) {
        (((a & 0200) != (b & 0200)) && ((b & 0200) == (r3 & 0200))) ? setV() : clrV();
    }
    void chgVS(uint16_t a, uint16_t b, uint16_t r3) { // sub: dst - src
        (((a & 0100000) != (b & 0100000)) && ((a & 0100000) == (r3 & 0100000))) ? setV() : clrV();
    }
    void chgVxorCN() { // V = C xor N (for shifts/rotates)
        bool c = psw & CC_C, n = psw & CC_N;
        (c == n) ? clrV() : setV();
    }

    // ---- Instruction handlers (return a Result code) ----
    typedef int (Cpu::*Handler)();
    Handler table_[1024];
    void buildTable();

    int op_illegal();
    // double-operand
    int op_mov();  int op_movb(); int op_cmp();  int op_cmpb();
    int op_bit();  int op_bitb(); int op_bic();  int op_bicb();
    int op_bis();  int op_bisb(); int op_add();  int op_sub();
    int op_xor();
    // single-operand
    int op_clr();  int op_clrb(); int op_com();  int op_comb();
    int op_inc();  int op_incb(); int op_dec();  int op_decb();
    int op_neg();  int op_negb(); int op_adc();  int op_adcb();
    int op_sbc();  int op_sbcb(); int op_tst();  int op_tstb();
    int op_ror();  int op_rorb(); int op_rol();  int op_rolb();
    int op_asr();  int op_asrb(); int op_asl();  int op_aslb();
    int op_swab(); int op_sxt();  int op_mark(); int op_mtps(); int op_mfps();
    // branches
    int brx(uint16_t clear, uint16_t set);
    int op_br();   int op_bne();  int op_beq();  int op_bge();  int op_blt();
    int op_bgt();  int op_ble();  int op_bpl();  int op_bmi();  int op_bhi();
    int op_blos(); int op_bvc();  int op_bvs();  int op_bcc();  int op_bcs();
    int op_sob();
    // jumps / control
    int op_jmp();  int op_jsr();  int op_rts();
    int op_ccc();  int op_scc();
    // service (via sub-tables 000xxx / 001xxx)
    int op_sub000();  // dispatches ir&077 for the 0000xx group
    int op_sub001();  // dispatches ir&077 for the 0002xx (RTS/CCC/SCC) group
    int op_halt();    int op_wait(); int op_rti(); int op_bpt(); int op_iot();
    int op_reset();   int op_rtt();  int op_emt(); int op_trap();

    int timingFor(uint16_t ir) const;
};

} // namespace bk
