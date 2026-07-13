#include "Cpu.h"

namespace bk {

// ---------------------------------------------------------------------------
// Reset
// ---------------------------------------------------------------------------
void Cpu::reset(uint16_t pc, uint16_t initPsw) {
    for (auto& x : r) x = 0;
    r[7] = pc;
    psw = initPsw;
    halted_ = waiting_ = false;
    lastBranch_ = pc;
}

// ---------------------------------------------------------------------------
// Effective-address load/store (ports ea.c)
// ---------------------------------------------------------------------------
static bool eaIllegalFlag = false; // set by loadEa on mode 0

uint16_t Cpu::loadSrc() {
    int m = srcMode(), reg = srcReg();
    uint16_t addr, ind, data;
    switch (m) {
    case 0: return r[reg];
    case 1: return rword(r[reg]);
    case 2: addr = r[reg]; data = rword(addr); r[reg] += 2; return data;
    case 3: ind = r[reg]; addr = rword(ind); r[reg] += 2; return rword(addr);
    case 4: r[reg] -= 2; addr = r[reg]; return rword(addr);
    case 5: r[reg] -= 2; ind = r[reg]; addr = rword(ind); return rword(addr);
    case 6: ind = rword(r[7]); r[7] += 2; addr = r[reg] + ind; return rword(addr);
    case 7: ind = rword(r[7]); r[7] += 2; addr = r[reg] + ind; addr = rword(addr); return rword(addr);
    }
    return 0;
}

uint16_t Cpu::loadDst() {
    int m = dstMode(), reg = dstReg();
    uint16_t addr, ind, data;
    switch (m) {
    case 0: return r[reg];
    case 1: addr = r[reg]; eaAddr_ = addr; return rword(addr);
    case 2: addr = r[reg]; eaAddr_ = addr; data = rword(addr); r[reg] += 2; return data;
    case 3: ind = r[reg]; addr = rword(ind); eaAddr_ = addr; data = rword(addr); r[reg] += 2; return data;
    case 4: r[reg] -= 2; addr = r[reg]; eaAddr_ = addr; return rword(addr);
    case 5: r[reg] -= 2; ind = r[reg]; addr = rword(ind); eaAddr_ = addr; return rword(addr);
    case 6: ind = rword(r[7]); r[7] += 2; addr = r[reg] + ind; eaAddr_ = addr; return rword(addr);
    case 7: ind = rword(r[7]); r[7] += 2; addr = r[reg] + ind; addr = rword(addr); eaAddr_ = addr; return rword(addr);
    }
    return 0;
}

uint8_t Cpu::loadbSrc() {
    int m = srcMode(), reg = srcReg();
    uint16_t addr, ind; uint8_t data;
    switch (m) {
    case 0: return r[reg] & 0377;
    case 1: return rbyte(r[reg]);
    case 2: addr = r[reg]; data = rbyte(addr); r[reg] += (reg >= 6 ? 2 : 1); return data;
    case 3: ind = r[reg]; addr = rword(ind); data = rbyte(addr); r[reg] += 2; return data;
    case 4: r[reg] -= (reg >= 6 ? 2 : 1); addr = r[reg]; return rbyte(addr);
    case 5: r[reg] -= 2; ind = r[reg]; addr = rword(ind); return rbyte(addr);
    case 6: ind = rword(r[7]); r[7] += 2; addr = r[reg] + ind; return rbyte(addr);
    case 7: ind = rword(r[7]); r[7] += 2; addr = r[reg] + ind; addr = rword(addr); return rbyte(addr);
    }
    return 0;
}

uint8_t Cpu::loadbDst() {
    int m = dstMode(), reg = dstReg();
    uint16_t addr, ind; uint8_t data;
    switch (m) {
    case 0: return r[reg] & 0377;
    case 1: addr = r[reg]; eaAddr_ = addr; return rbyte(addr);
    case 2: addr = r[reg]; eaAddr_ = addr; data = rbyte(addr); r[reg] += (reg >= 6 ? 2 : 1); return data;
    case 3: ind = r[reg]; addr = rword(ind); eaAddr_ = addr; data = rbyte(addr); r[reg] += 2; return data;
    case 4: r[reg] -= (reg >= 6 ? 2 : 1); addr = r[reg]; eaAddr_ = addr; return rbyte(addr);
    case 5: r[reg] -= 2; ind = r[reg]; addr = rword(ind); eaAddr_ = addr; return rbyte(addr);
    case 6: ind = rword(r[7]); r[7] += 2; addr = r[reg] + ind; eaAddr_ = addr; return rbyte(addr);
    case 7: ind = rword(r[7]); r[7] += 2; addr = r[reg] + ind; addr = rword(addr); eaAddr_ = addr; return rbyte(addr);
    }
    return 0;
}

void Cpu::storeDst(uint16_t v) {
    int m = dstMode(), reg = dstReg();
    uint16_t addr, ind;
    switch (m) {
    case 0: r[reg] = v; return;
    case 1: wword(r[reg], v); return;
    case 2: addr = r[reg]; wword(addr, v); r[reg] += 2; return;
    case 3: ind = r[reg]; addr = rword(ind); r[reg] += 2; wword(addr, v); return;
    case 4: r[reg] -= 2; wword(r[reg], v); return;
    case 5: r[reg] -= 2; ind = r[reg]; addr = rword(ind); wword(addr, v); return;
    case 6: ind = rword(r[7]); r[7] += 2; addr = r[reg] + ind; wword(addr, v); return;
    case 7: ind = rword(r[7]); r[7] += 2; addr = r[reg] + ind; addr = rword(addr); wword(addr, v); return;
    }
}

void Cpu::storeDst2(uint16_t v) {
    if (dstMode() == 0) r[dstReg()] = v;
    else wword(eaAddr_, v);
}

void Cpu::storebDst(uint8_t v) {
    int m = dstMode(), reg = dstReg();
    uint16_t addr, ind;
    switch (m) {
    case 0: r[reg] = (r[reg] & 0177400) + v; return;
    case 1: wbyte(r[reg], v); return;
    case 2: addr = r[reg]; wbyte(addr, v); r[reg] += (reg >= 6 ? 2 : 1); return;
    case 3: ind = r[reg]; addr = rword(ind); wbyte(addr, v); r[reg] += 2; return;
    case 4: r[reg] -= (reg >= 6 ? 2 : 1); wbyte(r[reg], v); return;
    case 5: r[reg] -= 2; ind = r[reg]; addr = rword(ind); wbyte(addr, v); return;
    case 6: ind = rword(r[7]); r[7] += 2; addr = r[reg] + ind; wbyte(addr, v); return;
    case 7: ind = rword(r[7]); r[7] += 2; addr = r[reg] + ind; addr = rword(addr); wbyte(addr, v); return;
    }
}

void Cpu::storebDst2(uint8_t v) {
    if (dstMode() == 0) r[dstReg()] = (r[dstReg()] & 0177400) + v;
    else wbyte(eaAddr_, v);
}

uint16_t Cpu::loadEa() {
    int m = dstMode(), reg = dstReg();
    uint16_t ind;
    eaIllegalFlag = false;
    switch (m) {
    case 0: eaIllegalFlag = true; return 0;
    case 1: return r[reg];
    case 2: { uint16_t a = r[reg]; r[reg] += 2; return a; }
    case 3: { ind = r[reg]; r[reg] += 2; return rword(ind); }
    case 4: r[reg] -= 2; return r[reg];
    case 5: r[reg] -= 2; return rword(r[reg]);
    case 6: ind = rword(r[7]); r[7] += 2; return r[reg] + ind;
    case 7: ind = rword(r[7]); r[7] += 2; return rword(r[reg] + ind);
    }
    return 0;
}

// ---------------------------------------------------------------------------
// Trap / interrupt service
// ---------------------------------------------------------------------------
void Cpu::service(uint16_t vector) {
    lastBranch_ = r[7];
    uint16_t oldpsw = psw, oldpc = r[7];
    push(oldpsw);
    push(oldpc);
    r[7] = rword(vector);
    psw = rword(vector + 2) & 0377;
}

void Cpu::interrupt(uint16_t vector) {
    waiting_ = false;
    service(vector);
}

// ---------------------------------------------------------------------------
// Instruction handlers
// ---------------------------------------------------------------------------
int Cpu::op_illegal() { return R_ILLEGAL; }

// ---- double operand ----
int Cpu::op_mov() {
    uint16_t data = srcMode() ? loadSrc() : r[srcReg()];
    chgN(data); chgZ(data); clrV();
    if (dstMode()) storeDst(data); else r[dstReg()] = data;
    return OK;
}
int Cpu::op_movb() {
    uint8_t data = srcMode() ? loadbSrc() : (r[srcReg()] & 0377);
    chgNb(data); chgZb(data); clrV();
    if (dstMode()) storebDst(data);
    else r[dstReg()] = (data & 0200) ? (0177400 + data) : data; // sign-extend into reg
    return OK;
}
int Cpu::op_cmp() {
    uint16_t d1 = loadSrc(), d2 = loadDst();
    uint32_t temp = (uint32_t)d1 + (uint16_t)~d2 + 1;
    uint16_t d3 = temp & 0177777;
    chgN(d3); chgZ(d3); chgVC(d1, d2, d3); chgIC(temp);
    return OK;
}
int Cpu::op_cmpb() {
    uint8_t d1 = loadbSrc(), d2 = loadbDst();
    uint16_t temp = (uint16_t)d1 + (uint8_t)~d2 + 1;
    uint8_t d3 = temp & 0377;
    chgNb(d3); chgZb(d3); chgVCb(d1, d2, d3); chgICb(temp);
    return OK;
}
int Cpu::op_bit() {
    uint16_t d = loadSrc() & loadDst();
    chgN(d); chgZ(d); clrV();
    return OK;
}
int Cpu::op_bitb() {
    uint8_t d = loadbSrc() & loadbDst();
    chgNb(d); chgZb(d); clrV();
    return OK;
}
int Cpu::op_bic() {
    uint16_t d1 = loadSrc(), d2 = loadDst();
    d2 = (~d1) & d2;
    chgN(d2); chgZ(d2); clrV();
    storeDst2(d2);
    return OK;
}
int Cpu::op_bicb() {
    uint8_t d1 = loadbSrc(), d2 = loadbDst();
    d2 = (~d1) & d2;
    chgNb(d2); chgZb(d2); clrV();
    storebDst2(d2);
    return OK;
}
int Cpu::op_bis() {
    uint16_t d1 = loadSrc(), d2 = loadDst();
    d2 = d1 | d2;
    chgN(d2); chgZ(d2); clrV();
    storeDst2(d2);
    return OK;
}
int Cpu::op_bisb() {
    uint8_t d1 = loadbSrc(), d2 = loadbDst();
    d2 = d1 | d2;
    chgNb(d2); chgZb(d2); clrV();
    storebDst2(d2);
    return OK;
}
int Cpu::op_add() {
    uint16_t d1 = loadSrc(), d2 = loadDst();
    uint32_t temp = (uint32_t)d1 + (uint32_t)d2;
    uint16_t d3 = temp & 0177777;
    chgN(d3); chgZ(d3); chgV(d1, d2, d3); chgC(temp);
    storeDst2(d3);
    return OK;
}
int Cpu::op_sub() {
    uint16_t d1 = loadSrc(), d2 = loadDst();
    uint32_t temp = (uint32_t)d2 + (uint16_t)~d1 + 1;
    uint16_t d3 = temp & 0177777;
    chgN(d3); chgZ(d3); chgVS(d1, d2, d3); chgIC(temp);
    storeDst2(d3);
    return OK;
}
int Cpu::op_xor() {
    uint16_t d2 = r[srcReg()];
    uint16_t d1 = loadDst();
    d2 ^= d1;
    chgN(d2); chgZ(d2); clrV();
    storeDst2(d2);
    return OK;
}

// ---- single operand (word) ----
int Cpu::op_clr() { clrAll(); setZ(); storeDst(0); return OK; }
int Cpu::op_com() {
    uint16_t d = ~loadDst();
    chgN(d); chgZ(d); clrV(); setC();
    storeDst2(d);
    return OK;
}
int Cpu::op_inc() {
    uint16_t d = loadDst();
    (d == 0077777) ? setV() : clrV();
    ++d; chgN(d); chgZ(d);
    storeDst2(d);
    return OK;
}
int Cpu::op_dec() {
    uint16_t d = loadDst();
    (d == 0100000) ? setV() : clrV();
    --d; chgN(d); chgZ(d);
    storeDst2(d);
    return OK;
}
int Cpu::op_neg() {
    uint16_t d = loadDst();
    d = (0177777 - d) + 1;
    chgN(d); chgZ(d);
    (d == 0100000) ? setV() : clrV();
    (d == 0) ? clrC() : setC();
    storeDst2(d);
    return OK;
}
int Cpu::op_adc() {
    uint16_t d = loadDst();
    if (psw & CC_C) {
        (d == 0077777) ? setV() : clrV();
        (d == 0177777) ? setC() : clrC();
        ++d;
    } else { clrV(); clrC(); }
    chgN(d); chgZ(d);
    storeDst2(d);
    return OK;
}
int Cpu::op_sbc() {
    uint16_t d = loadDst();
    (d == 0100000) ? setV() : clrV();
    if (psw & CC_C) { (d) ? clrC() : setC(); --d; }
    else clrC();
    chgN(d); chgZ(d);
    storeDst2(d);
    return OK;
}
int Cpu::op_tst() {
    uint16_t d = loadDst();
    clrAll(); chgN(d); chgZ(d);
    return OK;
}
int Cpu::op_ror() {
    uint16_t d = loadDst();
    uint16_t low = d & 1;
    d >>= 1;
    if (psw & CC_C) d |= 0100000;
    (low) ? setC() : clrC();
    chgN(d); chgZ(d); chgVxorCN();
    storeDst2(d);
    return OK;
}
int Cpu::op_rol() {
    uint16_t d = loadDst();
    uint16_t high = d & 0100000;
    d <<= 1;
    if (psw & CC_C) d |= 1;
    (high) ? setC() : clrC();
    chgN(d); chgZ(d); chgVxorCN();
    storeDst2(d);
    return OK;
}
int Cpu::op_asr() {
    uint16_t d = loadDst();
    (d & 1) ? setC() : clrC();
    d = (d >> 1) | (d & 0100000);
    chgN(d); chgZ(d); chgVxorCN();
    storeDst2(d);
    return OK;
}
int Cpu::op_asl() {
    uint16_t d = loadDst();
    (d & 0100000) ? setC() : clrC();
    d <<= 1;
    chgN(d); chgZ(d); chgVxorCN();
    storeDst2(d);
    return OK;
}
int Cpu::op_swab() {
    uint16_t d1 = loadDst();
    uint16_t hi = (d1 << 8) & 0xff00;
    uint16_t lo = (d1 >> 8) & 0x00ff;
    uint16_t res = hi | lo;
    clrAll(); chgNb(lo & 0xff); chgZb(lo & 0xff);
    storeDst2(res);
    return OK;
}
int Cpu::op_sxt() {
    uint16_t data;
    if (psw & CC_N) { data = 0177777; clrZ(); }
    else { data = 0; setZ(); }
    clrV();
    storeDst(data);
    return OK;
}
int Cpu::op_mark() {
    lastBranch_ = r[7];
    r[6] = r[7] + (ir_ & 077) * 2;
    r[7] = r[5];
    r[5] = pop();
    return OK;
}
int Cpu::op_mtps() {
    uint8_t data = loadbDst();
    // BK-0010: bits C,V,Z,N,T (0..4) and bit 7 (keyboard mask) are writable.
    psw = (psw & ~0217) | (data & 0217);
    return OK;
}
int Cpu::op_mfps() {
    uint8_t data = psw & 0377;
    chgNb(data); chgZb(data); clrV();
    if (dstMode()) storebDst(data);
    else r[dstReg()] = (data & 0200) ? (0177400 + data) : data;
    return OK;
}

// ---- single operand (byte) ----
int Cpu::op_clrb() { clrAll(); setZ(); storebDst(0); return OK; }
int Cpu::op_comb() {
    uint8_t d = ~loadbDst();
    chgNb(d); chgZb(d); clrV(); setC();
    storebDst2(d);
    return OK;
}
int Cpu::op_incb() {
    uint8_t d = loadbDst();
    (d == 0177) ? setV() : clrV();
    ++d; chgNb(d); chgZb(d);
    storebDst2(d);
    return OK;
}
int Cpu::op_decb() {
    uint8_t d = loadbDst();
    (d == 0200) ? setV() : clrV();
    --d; chgNb(d); chgZb(d);
    storebDst2(d);
    return OK;
}
int Cpu::op_negb() {
    uint8_t d = loadbDst();
    d = (0377 - d) + 1;
    chgNb(d); chgZb(d);
    (d == 0200) ? setV() : clrV();
    (d) ? setC() : clrC();
    storebDst2(d);
    return OK;
}
int Cpu::op_adcb() {
    uint8_t d = loadbDst();
    if (psw & CC_C) {
        (d == 0177) ? setV() : clrV();
        (d == 0377) ? setC() : clrC();
        ++d;
    } else { clrV(); clrC(); }
    chgNb(d); chgZb(d);
    storebDst2(d);
    return OK;
}
int Cpu::op_sbcb() {
    uint8_t d = loadbDst();
    if (psw & CC_C) { (d) ? clrC() : setC(); --d; }
    else clrC();
    (d == 0200) ? setV() : clrV();
    chgNb(d); chgZb(d);
    storebDst2(d);
    return OK;
}
int Cpu::op_tstb() {
    uint8_t d = loadbDst();
    chgNb(d); chgZb(d); clrV(); clrC();
    return OK;
}
int Cpu::op_rorb() {
    uint8_t d = loadbDst();
    uint8_t low = d & 1;
    d >>= 1;
    if (psw & CC_C) d |= 0200;
    (low) ? setC() : clrC();
    chgNb(d); chgZb(d); chgVxorCN();
    storebDst2(d);
    return OK;
}
int Cpu::op_rolb() {
    uint8_t d = loadbDst();
    uint8_t high = d & 0200;
    d <<= 1;
    if (psw & CC_C) d |= 1;
    (high) ? setC() : clrC();
    chgNb(d); chgZb(d); chgVxorCN();
    storebDst2(d);
    return OK;
}
int Cpu::op_asrb() {
    uint8_t d = loadbDst();
    (d & 1) ? setC() : clrC();
    d = (d >> 1) | (d & 0200);
    chgNb(d); chgZb(d); chgVxorCN();
    storebDst2(d);
    return OK;
}
int Cpu::op_aslb() {
    uint8_t d = loadbDst();
    (d & 0200) ? setC() : clrC();
    d <<= 1;
    chgNb(d); chgZb(d); chgVxorCN();
    storebDst2(d);
    return OK;
}

// ---- branches ----
int Cpu::brx(uint16_t clear, uint16_t set) {
    lastBranch_ = r[7];
    if (((psw & set) == set) && ((psw & clear) == 0)) {
        uint16_t off = ir_ & 0377;
        if (off & 0200) off |= 0177400;
        r[7] += off * 2;
    }
    return OK;
}
int Cpu::op_br()   { lastBranch_ = r[7]; uint16_t off = ir_ & 0377; if (off & 0200) off |= 0177400; r[7] += off * 2; return OK; }
int Cpu::op_bne()  { return brx(CC_Z, 0); }
int Cpu::op_beq()  { return brx(0, CC_Z); }
int Cpu::op_bpl()  { return brx(CC_N, 0); }
int Cpu::op_bmi()  { return brx(0, CC_N); }
int Cpu::op_bhi()  { return brx(CC_C | CC_Z, 0); }
int Cpu::op_bvc()  { return brx(CC_V, 0); }
int Cpu::op_bvs()  { return brx(0, CC_V); }
int Cpu::op_bcc()  { return brx(CC_C, 0); }
int Cpu::op_bcs()  { return brx(0, CC_C); }
int Cpu::op_blos() {
    lastBranch_ = r[7];
    if ((psw & CC_C) || (psw & CC_Z)) { uint16_t off = ir_ & 0377; if (off & 0200) off |= 0177400; r[7] += off * 2; }
    return OK;
}
int Cpu::op_bge() {
    lastBranch_ = r[7];
    int n = (psw & CC_N) ? 1 : 0, v = (psw & CC_V) ? 1 : 0;
    if ((n ^ v) == 0) { uint16_t off = ir_ & 0377; if (off & 0200) off |= 0177400; r[7] += off * 2; }
    return OK;
}
int Cpu::op_blt() {
    lastBranch_ = r[7];
    int n = (psw & CC_N) ? 1 : 0, v = (psw & CC_V) ? 1 : 0;
    if ((n ^ v) == 1) { uint16_t off = ir_ & 0377; if (off & 0200) off |= 0177400; r[7] += off * 2; }
    return OK;
}
int Cpu::op_ble() {
    lastBranch_ = r[7];
    int n = (psw & CC_N) ? 1 : 0, v = (psw & CC_V) ? 1 : 0;
    if (((n ^ v) == 1) || (psw & CC_Z)) { uint16_t off = ir_ & 0377; if (off & 0200) off |= 0177400; r[7] += off * 2; }
    return OK;
}
int Cpu::op_bgt() {
    lastBranch_ = r[7];
    int n = (psw & CC_N) ? 1 : 0, v = (psw & CC_V) ? 1 : 0;
    if (((n ^ v) == 0) && ((psw & CC_Z) == 0)) { uint16_t off = ir_ & 0377; if (off & 0200) off |= 0177400; r[7] += off * 2; }
    return OK;
}
int Cpu::op_sob() {
    lastBranch_ = r[7];
    r[srcReg()] -= 1;
    if (r[srcReg()]) r[7] -= (ir_ & 077) * 2;
    return OK;
}

// ---- jumps / control ----
int Cpu::op_jmp() {
    lastBranch_ = r[7];
    uint16_t a = loadEa();
    if (eaIllegalFlag) return R_ILLEGAL;
    r[7] = a;
    return OK;
}
int Cpu::op_jsr() {
    lastBranch_ = r[7];
    uint16_t a = loadEa();
    if (eaIllegalFlag) return R_ILLEGAL;
    push(r[srcReg()]);
    r[srcReg()] = r[7];
    r[7] = a;
    return OK;
}
int Cpu::op_rts() {
    lastBranch_ = r[7];
    r[7] = r[dstReg()];
    r[dstReg()] = pop();
    return OK;
}
int Cpu::op_ccc() { psw &= ~(ir_ & 017); return OK; }
int Cpu::op_scc() { psw |= (ir_ & 017); return OK; }

// ---- service ----
int Cpu::op_halt()  { return R_HALT; }
int Cpu::op_wait()  { r[7] -= 2; return R_WAIT; }   // re-execute until interrupt
int Cpu::op_rti()   { r[7] = pop(); psw = pop() & 0377; return OK; }
int Cpu::op_rtt()   { r[7] = pop(); psw = pop() & 0377; return R_RTT; }
int Cpu::op_bpt()   { return R_BPT; }
int Cpu::op_iot()   { return R_IOT; }
int Cpu::op_reset() { psw = 0200; return OK; }
int Cpu::op_emt()   { return R_EMT; }
int Cpu::op_trap()  { return R_TRAP; }

int Cpu::op_sub000() {
    int idx = ir_ & 077;
    switch (idx) {
    case 0: return op_halt();
    case 1: return op_wait();
    case 2: return op_rti();
    case 3: return op_bpt();
    case 4: return op_iot();
    case 5: return op_reset();
    case 6: return op_rtt();
    default: return R_ILLEGAL;
    }
}
int Cpu::op_sub001() {
    int idx = ir_ & 077;
    if (idx < 010) return op_rts();
    if (idx >= 040 && idx <= 057) return op_ccc();
    if (idx >= 060 && idx <= 077) return op_scc();
    return R_ILLEGAL;
}

// ---------------------------------------------------------------------------
// Dispatch table (mirrors itab.c: index = ir >> 6)
// ---------------------------------------------------------------------------
void Cpu::buildTable() {
    for (int i = 0; i < 1024; ++i) table_[i] = &Cpu::op_illegal;
    auto fill = [&](int lo, int hi, Handler h) { for (int i = lo; i <= hi; ++i) table_[i] = h; };

    table_[0] = &Cpu::op_sub000;          // 0000xx
    table_[1] = &Cpu::op_jmp;             // 0001DD
    table_[2] = &Cpu::op_sub001;          // 0002xx (RTS/CCC/SCC)
    table_[3] = &Cpu::op_swab;            // 0003DD
    fill(4, 7, &Cpu::op_br);              // 0004xx BR
    fill(010, 013, &Cpu::op_bne);
    fill(014, 017, &Cpu::op_beq);
    fill(020, 023, &Cpu::op_bge);
    fill(024, 027, &Cpu::op_blt);
    fill(030, 033, &Cpu::op_bgt);
    fill(034, 037, &Cpu::op_ble);
    fill(040, 047, &Cpu::op_jsr);         // 0040R.. JSR
    // 0050..0057 single-operand group
    table_[050] = &Cpu::op_clr; table_[051] = &Cpu::op_com;
    table_[052] = &Cpu::op_inc; table_[053] = &Cpu::op_dec;
    table_[054] = &Cpu::op_neg; table_[055] = &Cpu::op_adc;
    table_[056] = &Cpu::op_sbc; table_[057] = &Cpu::op_tst;
    table_[060] = &Cpu::op_ror; table_[061] = &Cpu::op_rol;
    table_[062] = &Cpu::op_asr; table_[063] = &Cpu::op_asl;
    table_[064] = &Cpu::op_mark;          // 065,066 illegal (already)
    table_[067] = &Cpu::op_sxt;
    fill(0100, 0177, &Cpu::op_mov);       // MOV 01SSDD
    fill(0200, 0277, &Cpu::op_cmp);       // CMP
    fill(0300, 0377, &Cpu::op_bit);       // BIT
    fill(0400, 0477, &Cpu::op_bic);       // BIC
    fill(0500, 0577, &Cpu::op_bis);       // BIS
    fill(0600, 0677, &Cpu::op_add);       // ADD
    // 0700..0737 EIS (mul/div/ash/ashc) not on VM1 -> illegal
    fill(0740, 0747, &Cpu::op_xor);       // XOR 074RDD
    fill(0770, 0777, &Cpu::op_sob);       // SOB
    // second half (bit 15 set)
    fill(01000, 01003, &Cpu::op_bpl);
    fill(01004, 01007, &Cpu::op_bmi);
    fill(01010, 01013, &Cpu::op_bhi);
    fill(01014, 01017, &Cpu::op_blos);
    fill(01020, 01023, &Cpu::op_bvc);
    fill(01024, 01027, &Cpu::op_bvs);
    fill(01030, 01033, &Cpu::op_bcc);
    fill(01034, 01037, &Cpu::op_bcs);
    fill(01040, 01043, &Cpu::op_emt);
    fill(01044, 01047, &Cpu::op_trap);
    table_[01050] = &Cpu::op_clrb; table_[01051] = &Cpu::op_comb;
    table_[01052] = &Cpu::op_incb; table_[01053] = &Cpu::op_decb;
    table_[01054] = &Cpu::op_negb; table_[01055] = &Cpu::op_adcb;
    table_[01056] = &Cpu::op_sbcb; table_[01057] = &Cpu::op_tstb;
    table_[01060] = &Cpu::op_rorb; table_[01061] = &Cpu::op_rolb;
    table_[01062] = &Cpu::op_asrb; table_[01063] = &Cpu::op_aslb;
    table_[01064] = &Cpu::op_mtps; table_[01067] = &Cpu::op_mfps;
    fill(01100, 01177, &Cpu::op_movb);
    fill(01200, 01277, &Cpu::op_cmpb);
    fill(01300, 01377, &Cpu::op_bitb);
    fill(01400, 01477, &Cpu::op_bicb);
    fill(01500, 01577, &Cpu::op_bisb);
    fill(01600, 01677, &Cpu::op_sub);
    // 01700..01777 FIS/FP not on VM1 -> illegal
}

// ---------------------------------------------------------------------------
// Instruction timing (ports timing.c)
// ---------------------------------------------------------------------------
int Cpu::timingFor(uint16_t ir) const {
    static const int a_time[8]  = {0, 12, 12, 20, 12, 20, 20, 28};
    static const int b_time[8]  = {0, 20, 20, 32, 20, 32, 32, 40};
    static const int ab_time[8] = {0, 16, 16, 24, 16, 24, 24, 32};
    static const int a2_time[8] = {0, 20, 20, 28, 20, 28, 28, 36};
    static const int ds_time[8] = {0, 32, 32, 40, 32, 40, 40, 48};
    const int REGREG = 12;
    int sm = (ir & 07000) >> 9, dm = (ir & 070) >> 3;
    int idx = ir >> 6;

    // Two-operand groups: 01xx-06xx and 011xx-016xx (mov/cmp/bit/bic/bis/add/sub + byte)
    int grp = idx >> 6; // top part
    auto dstT = [&](void) { return sm ? ab_time[dm] : b_time[dm]; };
    auto cmpT = [&](void) { return sm ? a_time[dm] : a2_time[dm]; };
    switch (grp) {
    case 001: case 011: return REGREG + a_time[sm] + dstT();  // mov/movb
    case 002: case 012: return REGREG + a_time[sm] + cmpT();  // cmp/cmpb
    case 003: case 013: return REGREG + a_time[sm] + cmpT();  // bit/bitb
    case 004: case 014: return REGREG + a_time[sm] + dstT();  // bic/bicb
    case 005: case 015: return REGREG + a_time[sm] + dstT();  // bis/bisb
    case 006: case 016: return REGREG + a_time[sm] + dstT();  // add/sub
    }
    // Branches
    if ((idx >= 001 && idx <= 003) || (idx >= 004 && idx <= 037) ||
        (idx >= 01000 && idx <= 01037)) {
        if (idx == 001) return a2_time[dm];   // JMP
        if (idx == 002) return 12;            // RTS/CCC/SCC (approx)
        if (idx == 003) return REGREG + ab_time[dm]; // SWAB
        return 16;                            // conditional/unconditional branches
    }
    if (idx >= 040 && idx <= 047) return ds_time[dm];   // JSR
    if (idx >= 050 && idx <= 067) return REGREG + ab_time[dm]; // single-operand word
    if (idx >= 0740 && idx <= 0747) return REGREG + a2_time[dm]; // XOR
    if (idx >= 0770 && idx <= 0777) return 20;          // SOB
    if (idx >= 01040 && idx <= 01047) return 68;        // EMT/TRAP
    if (idx >= 01050 && idx <= 01067) return REGREG + ab_time[dm]; // single-operand byte
    if (idx == 0) return 40;                            // 0000xx (rti/halt/...) approx
    return 40;                                          // default/illegal fallback
}

// ---------------------------------------------------------------------------
// Single-step
// ---------------------------------------------------------------------------
int Cpu::step() {
    if (halted_) return 4;
    ir_ = rword(r[7]);
    r[7] += 2;
    int ticks = timingFor(ir_);
    int res = (this->*table_[ir_ >> 6])();
    switch (res) {
    case OK:
    case R_RTT:   break;
    case R_ILLEGAL: service(VEC_RESERVED); break;
    case R_EMT:
        // EMT 36 = tape/disk file I/O. If a hook handles it (file read/write to
        // the host), skip the ROM handler; PC already points past the EMT.
        if ((ir_ & 0377) == 036 && emt36Hook_ && emt36Hook_()) break;
        service(VEC_EMT);
        break;
    case R_TRAP: service(VEC_TRAP); break;
    case R_IOT:  service(VEC_IOT);  break;
    case R_BPT:  service(VEC_TBIT); break; // BPT vector = 000014
    case R_HALT:
        // BK-0010: HALT is not a permanent stop — it traps through the STOP
        // vector 0004, where the monitor (or the program's own handler) services
        // it. Games use HALT this way; treating it as a hard stop hangs them.
        // Only stop for real if no handler is installed (avoids a HALT->0->HALT
        // loop).
        if (mem_.peekWord(VEC_BUS_ERROR) != 0) service(VEC_BUS_ERROR);
        else halted_ = true;
        break;
    case R_WAIT: waiting_ = true;    break;
    }
    return ticks;
}

} // namespace bk
