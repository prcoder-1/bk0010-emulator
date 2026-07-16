#include "Disasm.h"
#include "Symbols.h"
#include <cstdio>

namespace bk {

namespace {

const char* regName(int r) {
    static const char* n[8] = {"R0", "R1", "R2", "R3", "R4", "SP", "PC"};
    return (r < 7) ? n[r] : "PC";
}

std::string oct(uint16_t v) {
    char buf[8];
    std::snprintf(buf, sizeof(buf), "%o", v);
    return buf;
}

// A target address rendered as its symbol name if one matches, else octal.
std::string symOrOct(uint16_t v, const SymbolTable* syms) {
    if (syms) { const std::string* n = syms->nameAt(v); if (n) return *n; }
    return oct(v);
}

// Format one operand (mode+reg). Consumes an extra word from mem for modes 6/7
// and for PC-relative immediate/absolute (modes 2/3 with reg==7). `pc` points
// at the word following the current instruction word and is advanced.
std::string operand(const Memory& mem, int mode, int reg, uint16_t& pc, int& words,
                    const SymbolTable* syms) {
    char buf[32];
    switch (mode) {
    case 0: return regName(reg);
    case 1: std::snprintf(buf, sizeof(buf), "(%s)", regName(reg)); return buf;
    case 2:
        if (reg == 7) { // immediate: #n
            uint16_t imm = mem.peekWord(pc); pc += 2; words++;
            std::snprintf(buf, sizeof(buf), "#%s", oct(imm).c_str());
            return buf;
        }
        std::snprintf(buf, sizeof(buf), "(%s)+", regName(reg));
        return buf;
    case 3:
        if (reg == 7) { // absolute: @#n
            uint16_t a = mem.peekWord(pc); pc += 2; words++;
            return "@#" + symOrOct(a, syms);
        }
        std::snprintf(buf, sizeof(buf), "@(%s)+", regName(reg));
        return buf;
    case 4: std::snprintf(buf, sizeof(buf), "-(%s)", regName(reg)); return buf;
    case 5: std::snprintf(buf, sizeof(buf), "@-(%s)", regName(reg)); return buf;
    case 6: {
        uint16_t disp = mem.peekWord(pc); pc += 2; words++;
        if (reg == 7) return symOrOct((uint16_t)(pc + disp), syms); // PC-relative address
        std::snprintf(buf, sizeof(buf), "%s(%s)", oct(disp).c_str(), regName(reg));
        return buf;
    }
    case 7: {
        uint16_t disp = mem.peekWord(pc); pc += 2; words++;
        if (reg == 7) return "@" + symOrOct((uint16_t)(pc + disp), syms);
        std::snprintf(buf, sizeof(buf), "@%s(%s)", oct(disp).c_str(), regName(reg));
        return buf;
    }
    }
    return "?";
}

std::string branchTarget(uint16_t pcAfter, uint16_t ir, const SymbolTable* syms) {
    int off = ir & 0377;
    if (off & 0200) off |= 0177400;
    return symOrOct((uint16_t)(pcAfter + off * 2), syms);
}

} // namespace

DisasmLine disasm(const Memory& mem, uint16_t addr, const SymbolTable* syms) {
    DisasmLine out;
    out.addr = addr;
    uint16_t ir = mem.peekWord(addr);
    uint16_t pc = addr + 2; // points past the opcode word
    int words = 1;

    int sm = (ir & 07000) >> 9, sr = (ir & 00700) >> 6;
    int dm = (ir & 00070) >> 3, dr = ir & 00007;
    char buf[96];

    auto twoOp = [&](const char* m) {
        std::string s = operand(mem, sm, sr, pc, words, syms);
        std::string d = operand(mem, dm, dr, pc, words, syms);
        std::snprintf(buf, sizeof(buf), "%-6s%s, %s", m, s.c_str(), d.c_str());
        return std::string(buf);
    };
    auto oneOp = [&](const char* m) {
        std::string d = operand(mem, dm, dr, pc, words, syms);
        std::snprintf(buf, sizeof(buf), "%-6s%s", m, d.c_str());
        return std::string(buf);
    };
    auto branch = [&](const char* m) {
        std::snprintf(buf, sizeof(buf), "%-6s%s", m, branchTarget(pc, ir, syms).c_str());
        return std::string(buf);
    };

    int idx = ir >> 6;
    int grp = ir >> 12;      // top 4 bits: two-operand major opcode
    std::string t;

    // Two-operand instructions (grp 1..6 word, 011..016 byte)
    static const char* two_w[7] = {nullptr, "MOV", "CMP", "BIT", "BIC", "BIS", "ADD"};
    static const char* two_b[7] = {nullptr, "MOVB", "CMPB", "BITB", "BICB", "BISB", nullptr};
    if (grp >= 1 && grp <= 6) { out.text = twoOp(two_w[grp]); out.words = words; return out; }
    if (grp >= 011 && grp <= 015) { out.text = twoOp(two_b[grp - 010]); out.words = words; return out; }
    if (grp == 016) { out.text = twoOp("SUB"); out.words = words; return out; }

    // Everything else: decode by idx
    switch (idx) {
    case 0: { // 0000xx
        switch (ir & 077) {
        case 0: t = "HALT"; break;
        case 1: t = "WAIT"; break;
        case 2: t = "RTI"; break;
        case 3: t = "BPT"; break;
        case 4: t = "IOT"; break;
        case 5: t = "RESET"; break;
        case 6: t = "RTT"; break;
        default: t = ".WORD " + oct(ir); break;
        }
        break;
    }
    case 1: t = oneOp("JMP"); break;
    case 2: { // RTS / CCC / SCC
        int sub = ir & 077;
        if (sub < 010) { std::snprintf(buf, sizeof(buf), "%-6s%s", "RTS", regName(ir & 7)); t = buf; }
        else if (sub >= 040 && sub <= 057) { std::snprintf(buf, sizeof(buf), "CCC   %o", ir & 017); t = buf; }
        else if (sub >= 060 && sub <= 077) { std::snprintf(buf, sizeof(buf), "SCC   %o", ir & 017); t = buf; }
        else t = ".WORD " + oct(ir);
        break;
    }
    case 3: t = oneOp("SWAB"); break;
    default:
        if (idx >= 004 && idx <= 007) t = branch("BR");
        else if (idx >= 010 && idx <= 013) t = branch("BNE");
        else if (idx >= 014 && idx <= 017) t = branch("BEQ");
        else if (idx >= 020 && idx <= 023) t = branch("BGE");
        else if (idx >= 024 && idx <= 027) t = branch("BLT");
        else if (idx >= 030 && idx <= 033) t = branch("BGT");
        else if (idx >= 034 && idx <= 037) t = branch("BLE");
        else if (idx >= 040 && idx <= 047) { // JSR
            std::string d = operand(mem, dm, dr, pc, words, syms);
            std::snprintf(buf, sizeof(buf), "%-6s%s, %s", "JSR", regName(sr), d.c_str());
            t = buf;
        }
        else if (idx == 050) t = oneOp("CLR");
        else if (idx == 051) t = oneOp("COM");
        else if (idx == 052) t = oneOp("INC");
        else if (idx == 053) t = oneOp("DEC");
        else if (idx == 054) t = oneOp("NEG");
        else if (idx == 055) t = oneOp("ADC");
        else if (idx == 056) t = oneOp("SBC");
        else if (idx == 057) t = oneOp("TST");
        else if (idx == 060) t = oneOp("ROR");
        else if (idx == 061) t = oneOp("ROL");
        else if (idx == 062) t = oneOp("ASR");
        else if (idx == 063) t = oneOp("ASL");
        else if (idx == 064) { std::snprintf(buf, sizeof(buf), "MARK  %o", ir & 077); t = buf; }
        else if (idx == 067) t = oneOp("SXT");
        else if (idx >= 0740 && idx <= 0747) { // XOR
            std::string d = operand(mem, dm, dr, pc, words, syms);
            std::snprintf(buf, sizeof(buf), "%-6s%s, %s", "XOR", regName(sr), d.c_str());
            t = buf;
        }
        else if (idx >= 0770 && idx <= 0777) { // SOB
            int reg = (ir & 0700) >> 6;
            uint16_t target = (uint16_t)(pc - (ir & 077) * 2);
            std::snprintf(buf, sizeof(buf), "%-6s%s, %s", "SOB", regName(reg), symOrOct(target, syms).c_str());
            t = buf;
        }
        else if (idx >= 01000 && idx <= 01003) t = branch("BPL");
        else if (idx >= 01004 && idx <= 01007) t = branch("BMI");
        else if (idx >= 01010 && idx <= 01013) t = branch("BHI");
        else if (idx >= 01014 && idx <= 01017) t = branch("BLOS");
        else if (idx >= 01020 && idx <= 01023) t = branch("BVC");
        else if (idx >= 01024 && idx <= 01027) t = branch("BVS");
        else if (idx >= 01030 && idx <= 01033) t = branch("BCC");
        else if (idx >= 01034 && idx <= 01037) t = branch("BCS");
        else if (idx >= 01040 && idx <= 01043) { std::snprintf(buf, sizeof(buf), "EMT   %o", ir & 0377); t = buf; }
        else if (idx >= 01044 && idx <= 01047) { std::snprintf(buf, sizeof(buf), "TRAP  %o", ir & 0377); t = buf; }
        else if (idx == 01050) t = oneOp("CLRB");
        else if (idx == 01051) t = oneOp("COMB");
        else if (idx == 01052) t = oneOp("INCB");
        else if (idx == 01053) t = oneOp("DECB");
        else if (idx == 01054) t = oneOp("NEGB");
        else if (idx == 01055) t = oneOp("ADCB");
        else if (idx == 01056) t = oneOp("SBCB");
        else if (idx == 01057) t = oneOp("TSTB");
        else if (idx == 01060) t = oneOp("RORB");
        else if (idx == 01061) t = oneOp("ROLB");
        else if (idx == 01062) t = oneOp("ASRB");
        else if (idx == 01063) t = oneOp("ASLB");
        else if (idx == 01064) t = oneOp("MTPS");
        else if (idx == 01067) t = oneOp("MFPS");
        else t = ".WORD " + oct(ir);
        break;
    }

    out.text = t;
    out.words = words;
    return out;
}

} // namespace bk
