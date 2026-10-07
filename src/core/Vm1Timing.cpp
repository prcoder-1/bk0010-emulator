#include "Vm1Timing.h"

namespace bk {

namespace {

struct Builder {
    Vm1Sched& s;
    void add(char k, int gap) { s.op[s.n++] = {k, static_cast<uint8_t>(gap), 0}; }
    // Приёмник (R)+, который читается: инкремент регистра виден только после быстрого
    // обмена — следующий шаг на 2 позже.
    void autoinc() {
        if (s.n && s.op[s.n - 1].kind == 'M') s.op[s.n - 1].xf = 2;
        else s.pfXf = 2;
    }
};

// Чтения адреса/указателя до самого операнда у режима m (0..7) — после первого обмена.
int ptrReads(int m) { return (m == 3 || m == 5 || m == 6) ? 1 : (m == 7 ? 2 : 0); }

// Обмены, нужные чтобы добраться до операнда (адресная часть), и сам операнд, если
// `withData`. Первый обмен — через `first`, указатели — через 8.
void operand(Builder& b, int m, int first, bool withData) {
    const int reads = ptrReads(m) + (withData ? 1 : 0);
    for (int k = 0; k < reads; ++k) b.add('R', k == 0 ? first : 8);
}

} // namespace

bool vm1Schedule(uint16_t ir, Vm1Sched& s) {
    s = Vm1Sched{};
    Builder b{s};
    const int op = (ir >> 12) & 7;
    const int sm = (ir >> 9) & 7, dm = (ir >> 3) & 7;
    const int idx = ir >> 6;

    // --- Двухоперандные: MOV(B), CMP(B), BIT(B), BIC(B), BIS(B), ADD, SUB ---
    if (op >= 1 && op <= 6) {
        const int cls = op & 7;                         // 1 MOV, 2/3 CMP/BIT, 4..6 RMW
        const bool mov = cls == 1, cmp = cls == 2 || cls == 3;
        // Источник: первый обмен через 12 после S (14, если сначала декремент).
        if (sm) operand(b, sm, (sm == 4 || sm == 5) ? 14 : 12, true);
        if (dm == 0) {
            s.tail = sm ? 12 : 8;
            s.early = true;
            s.teFast = op == 1 && (ir & 0100000) ? 8 : 2;      // MOVB в регистр
            return true;
        }
        // Первый обмен приёмника: от конца чтения источника, а при регистровом
        // источнике — от S, на 12 позже.
        int first;
        if (mov) first = dm == 1 ? 17 : (dm == 2 || dm == 4) ? 23 : dm == 5 ? 14 : 12;
        else     first = (dm == 4 || dm == 5) ? 14 : 12;
        if (sm == 0) first += 12;
        if (mov) {
            int pr = ptrReads(dm);
            if (pr == 0) b.add('W', first);
            else { operand(b, dm, first, false); b.add('W', 11); }
            s.tail = 12;
        } else {
            operand(b, dm, first, true);
            if (cmp) s.tail = 12;
            else { b.add('M', 5); s.tail = 6; }
            if (dm == 2) b.autoinc();
        }
        return true;
    }
    if ((ir & 0170000) == 0170000) return false;        // FP — нет на ВМ1

    // --- XOR ---
    if ((ir & 0177000) == 0074000) {
        if (dm == 0) { s.tail = 8; s.early = true; s.teFast = 2; return true; }
        operand(b, dm, (dm == 4 || dm == 5) ? 20 : 18, true);
        b.add('M', 5); s.tail = 6;
        if (dm == 2) b.autoinc();
        return true;
    }
    // --- SOB ---
    if ((ir & 0177000) == 0077000) { s.tail = 28; return true; }
    // --- EMT, TRAP ---
    if ((ir & 0177000) == 0104000) {
        b.add('W', 15); b.add('W', 7); b.add('R', 14); b.add('R', 14);
        s.tail = 22;
        return true;
    }
    // --- Переходы ---
    if ((idx >= 004 && idx <= 037) || (idx >= 01000 && idx <= 01037)) { s.tail = 18; return true; }
    // --- JSR ---
    if (idx >= 040 && idx <= 047) {
        if (dm == 0) return false;                      // недопустимо — ловушка
        if (dm == 1) b.add('W', 29);
        else if (dm == 2 || dm == 4) b.add('W', 35);
        else { operand(b, dm, dm == 5 ? 14 : 12, false); b.add('W', 23); }
        s.tail = 10;
        return true;
    }
    // --- JMP ---
    if (idx == 001) {
        if (dm == 0) return false;
        if (dm == 1) s.tail = 30;
        else if (dm == 2 || dm == 4) s.tail = 36;
        else { operand(b, dm, dm == 5 ? 14 : 12, false); s.tail = 24; }
        return true;
    }
    // --- Одноместные: CLR..ASL, SXT, SWAB, MFPS, MTPS, TST (и байтовые) ---
    const bool single = (idx >= 050 && idx <= 063) || idx == 067 || idx == 003
                     || (idx >= 01050 && idx <= 01064) || idx == 01067;
    if (single) {
        const bool tst = idx == 057 || idx == 01057;
        const bool mtps = idx == 01064;
        const bool swab = idx == 003, mfps = idx == 01067;
        if (dm == 0) {
            s.tail = mtps ? 6 : 8; s.early = true;
            s.teFast = mtps ? 10 : mfps ? 8 : swab ? 4 : 2;
            if (mtps) s.pfXf = 2;
            return true;
        }
        operand(b, dm, (dm == 4 || dm == 5) ? 14 : 12, true);
        if (tst) s.tail = 12;
        else if (mtps) s.tail = 24;
        else { b.add('M', 5); s.tail = 6; }
        if (dm == 2) b.autoinc();
        if (swab) s.pfXf = 2;                           // SWAB: хвост после записи
        return true;
    }
    // --- 0000xx ---
    if (ir == 0000002 || ir == 0000006) {               // RTI, RTT
        b.add('R', 8); b.add('R', 12); s.tail = 22;
        return true;
    }
    if (ir == 0000003 || ir == 0000004) {               // BPT, IOT
        b.add('W', 15); b.add('W', 7); b.add('R', 14); b.add('R', 14);
        s.tail = 22;
        return true;
    }
    if ((ir & 0177770) == 0000200) { b.add('R', 24); s.tail = 12; return true; }   // RTS
    if (ir >= 0000240 && ir <= 0000277) {                                           // CCC/SCC/NOP
        s.tail = 8; s.early = true; s.teFast = 4;
        return true;
    }
    return false;
}

void vm1InterruptSchedule(bool iako, Vm1Sched& s) {
    s = Vm1Sched{};
    Builder b{s};
    b.add('W', 11); b.add('W', 9);                      // PSW, PC в стек
    if (iako) { b.add('I', 6); b.add('R', 8); }         // вектор от устройства
    else b.add('R', 14);
    b.add('R', 14);                                     // новые PC и PSW
    s.tail = 22;
}

} // namespace bk
