#pragma once
#include <cstdint>
#include <vector>
#include "Memory.h"

namespace bk {

// Разбор команды ВМ1 для анализа потока управления: шаг с обходом, перекрёстные
// ссылки, профилировщики, автоанализ процедур, выбор машины по .BIN. Один разбор
// на всех — диапазоны кодов не повторяются по файлам. Текст команды даёт disasm().

// Куда команда передаёт управление.
enum class Flow : uint8_t {
    Next,     // обычная команда — дальше следующая
    Branch,   // условный переход (Bxx, SOB): цель или следующая
    Jump,     // BR, JMP: только цель
    Call,     // JSR
    Trap,     // EMT, TRAP, IOT, BPT — вызов через вектор
    Return,   // RTS, RTI, RTT
    Halt,     // HALT
};

// У операнда (6 бит: режим и регистр) есть своё слово: #n, @#n, X(Rn), @X(Rn).
inline bool operandHasWord(unsigned modeReg) {
    const unsigned mode = (modeReg >> 3) & 7, reg = modeReg & 7;
    return (reg == 7 && (mode == 2 || mode == 3)) || mode >= 6;
}

// Сколько слов операндов идёт за словом команды (0..2).
inline int operandWords(uint16_t ir) {
    const unsigned grp = ir >> 12, idx = ir >> 6, src = (ir >> 6) & 077, dst = ir & 077;
    if ((grp >= 1 && grp <= 6) || (grp >= 011 && grp <= 016))          // двухоперандные
        return int(operandHasWord(src)) + int(operandHasWord(dst));
    const bool oneOp = idx == 001 || idx == 003                          // JMP, SWAB
        || (idx >= 040 && idx <= 047) || (idx >= 050 && idx <= 063)      // JSR; CLR…ASL
        || idx == 067 || (idx >= 0740 && idx <= 0747)                    // SXT; XOR
        || (idx >= 01050 && idx <= 01064) || idx == 01067;               // CLRB…MTPS; MFPS
    return oneOp ? int(operandHasWord(dst)) : 0;
}

inline bool isJsr(uint16_t ir) { return (ir & 0177000) == 0004000; }

// Поток управления по одному слову команды (цели без памяти не узнать).
inline Flow instrFlow(uint16_t ir) {
    const unsigned idx = ir >> 6;
    if (idx >= 004 && idx <= 007) return Flow::Jump;                     // BR
    if ((idx >= 010 && idx <= 037) || (idx >= 01000 && idx <= 01037)) return Flow::Branch;
    if (idx >= 0770 && idx <= 0777) return Flow::Branch;                 // SOB
    if (idx == 001) return Flow::Jump;                                   // JMP
    if (isJsr(ir)) return Flow::Call;
    if ((ir & 0177770) == 0000200 || ir == 0000002 || ir == 0000006) return Flow::Return;
    if ((idx >= 01040 && idx <= 01047) || ir == 0000003 || ir == 0000004) return Flow::Trap;
    if (ir == 0000000) return Flow::Halt;
    return Flow::Next;
}

struct InstrInfo {
    uint16_t ir = 0;
    int      words = 1;        // длина команды в словах (1..3)
    Flow     flow = Flow::Next;
    bool     hasTarget = false; // цель известна статически
    uint16_t target = 0;       // Bxx/BR/SOB; JMP/JSR по @#a, a(PC), @a(PC)
};

// Разбор команды по адресу addr без побочных эффектов (peek).
inline InstrInfo instrInfo(const Memory& mem, uint16_t addr) {
    InstrInfo i;
    i.ir = mem.peekWord(addr);
    i.words = 1 + operandWords(i.ir);
    i.flow = instrFlow(i.ir);
    const uint16_t ir = i.ir;
    const unsigned idx = ir >> 6;
    auto target = [&](uint16_t t) { i.hasTarget = true; i.target = t; };
    if ((idx >= 004 && idx <= 037) || (idx >= 01000 && idx <= 01037))
        target(static_cast<uint16_t>(addr + 2 + static_cast<int8_t>(ir & 0377) * 2));
    else if (idx >= 0770 && idx <= 0777)
        target(static_cast<uint16_t>(addr + 2 - (ir & 077) * 2));
    else if ((i.flow == Flow::Jump || i.flow == Flow::Call) && (ir & 7) == 7) {
        const uint16_t x = mem.peekWord(static_cast<uint16_t>(addr + 2));
        switch ((ir >> 3) & 7) {
        case 3: target(x); break;                                                         // @#a
        case 6: target(static_cast<uint16_t>(addr + 4 + x)); break;                       // a(PC)
        case 7: target(mem.peekWord(static_cast<uint16_t>(addr + 4 + x))); break;         // @a(PC)
        default: break;
        }
    }
    return i;
}

// Статические ссылки на target: каждое чётное слово памяти пробуется как начало команды
// (без разбора потока — бывают ложные попадания посреди команды, зато видны и ветки,
// которые ещё не исполнялись). Динамические — рёбра трассы (Trace::edges).
inline std::vector<uint16_t> staticXrefs(const Memory& mem, uint16_t target, size_t max = 500) {
    std::vector<uint16_t> refs;
    for (uint32_t a = 0; a <= 0177776 && refs.size() < max; a += 2) {
        const InstrInfo in = instrInfo(mem, static_cast<uint16_t>(a));
        if (in.hasTarget && in.target == target) refs.push_back(static_cast<uint16_t>(a));
    }
    return refs;
}

} // namespace bk
