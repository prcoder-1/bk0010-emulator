#include "DebuggerOverlay.h"
#include "Board.h"
#include "Disasm.h"
#include <QPainter>
#include <QMouseEvent>
#include <QWheelEvent>
#include <QFontDatabase>
#include <cstdio>
#include <cmath>
#include <algorithm>

using bk::Board;

DebuggerOverlay::DebuggerOverlay(Board* board, QWidget* parent)
    : QWidget(parent), board_(board) {
    setAttribute(Qt::WA_NoSystemBackground);
    setAutoFillBackground(false);
    disasmTop_ = board_->cpu().pc();
    // Record execution so the disassembler can tint hot (frequently-run) code.
    board_->trace().setEnabled(true);
}

void DebuggerOverlay::followPc() {
    uint16_t pc = board_->cpu().pc();
    // If PC is outside the currently shown disasm window, re-anchor near it.
    bool visible = false;
    uint16_t a = disasmTop_;
    for (int i = 0; i < disasmLines_; ++i) {
        if (a == pc) { visible = true; break; }
        a += bk::disasm(board_->memory(), a).words * 2;
    }
    if (!visible) disasmTop_ = pc;
    update();
}

void DebuggerOverlay::scrollDisasm(int lines) {
    if (lines > 0) {
        for (int i = 0; i < lines; ++i)
            disasmTop_ += bk::disasm(board_->memory(), disasmTop_).words * 2;
    } else {
        disasmTop_ += lines * 2; // approximate backward move (1 word/line)
    }
    update();
}

void DebuggerOverlay::scrollMem(int rows) { memAddr_ += rows * 8; update(); }

void DebuggerOverlay::snapshotRegs() {
    auto& cpu = board_->cpu();
    for (int i = 0; i < 8; ++i) prevR_[i] = cpu.r[i];
    prevPsw_ = cpu.psw;
    havePrev_ = true;
}

void DebuggerOverlay::moveCursor(int lines) {
    const bk::Memory& mem = board_->memory();
    if (lines > 0) {
        for (int i = 0; i < lines; ++i) cursorAddr_ += bk::disasm(mem, cursorAddr_).words * 2;
    } else {
        cursorAddr_ += lines * 2;                 // approximate backward move (1 word/line)
    }
    // Re-anchor the disasm window if the cursor scrolled out of view.
    uint16_t a = disasmTop_; bool vis = false;
    for (int i = 0; i < disasmLines_; ++i) {
        if (a == cursorAddr_) { vis = true; break; }
        a += bk::disasm(mem, a).words * 2;
    }
    if (!vis) disasmTop_ = cursorAddr_;
    update();
}

static QString flagsStr(uint16_t psw) {
    auto b = [&](uint16_t m, char c) { return (psw & m) ? c : '-'; };
    char buf[8];
    std::snprintf(buf, sizeof(buf), "%c%c%c%c%c",
        b(bk::Cpu::CC_T, 'T'), b(bk::Cpu::CC_N, 'N'), b(bk::Cpu::CC_Z, 'Z'),
        b(bk::Cpu::CC_V, 'V'), b(bk::Cpu::CC_C, 'C'));
    return buf;
}

static QString oct6(uint16_t v) { char b[8]; std::snprintf(b, sizeof(b), "%06o", v); return b; }

void DebuggerOverlay::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.setRenderHint(QPainter::TextAntialiasing, true);

    QFont mono = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    int fs = qMax(10, height() / 48);
    mono.setPixelSize(fs);
    p.setFont(mono);
    QFontMetrics fm(mono);
    lineH_ = fm.height() + 2;
    int cw = fm.horizontalAdvance('0');

    const QColor panelBg(8, 16, 40, 210);      // translucent dark-blue
    const QColor border(80, 140, 255, 230);
    const QColor fg(210, 220, 255);
    const QColor hi(255, 240, 120);            // current PC
    const QColor bpCol(255, 90, 90);           // breakpoint
    const QColor title(120, 200, 255);
    const QColor chg(255, 180, 60);            // register changed since last step
    const QColor symCol(120, 210, 170);        // symbol name annotation

    auto& cpu = board_->cpu();
    auto& mem = board_->memory();
    auto& tr  = board_->trace();
    const bk::SymbolTable& syms = board_->symbols();
    // "name" / "name+off" for a code address, or empty if no symbol is near.
    auto symLabel = [&](uint16_t a) -> QString {
        uint16_t base; std::string nm;
        if (!syms.nearest(a, base, nm, 0x400)) return QString();
        QString s = QString::fromStdString(nm);
        if (a != base) s += "+" + QString::number(a - base, 8);
        return s;
    };

    int margin = fs;
    int W = width(), H = height();

    // ---- Registers panel (top-left) ----
    // Title on its own line; three data rows below it (R0-3 / R4-7 / PSW). Sized
    // snugly to its content (widest row = "PSW=… [flags]" + "СОСТ: ОСТАНОВ (HALT)")
    // plus inner padding, so the freed width to the right holds the breakpoints
    // panel.
    int regW = 44 * cw + 14;
    QRect regRect(margin, margin, regW, lineH_ * 5);
    p.fillRect(regRect, panelBg);
    p.setPen(border); p.drawRect(regRect);
    p.setPen(title); p.drawText(regRect.adjusted(6, 4, 0, 0), Qt::AlignTop | Qt::AlignLeft, "— РЕГИСТРЫ —");
    int rx = regRect.x() + 6, ry = regRect.y() + 2 * lineH_;
    for (int i = 0; i < 8; ++i) {
        static const char* nm[8] = {"R0", "R1", "R2", "R3", "R4", "R5", "SP", "PC"};
        // A register that changed since the last step is drawn in the "changed"
        // colour (classic Soft-ICE cue).
        p.setPen(havePrev_ && cpu.r[i] != prevR_[i] ? chg : fg);
        QString s = QString("%1=%2").arg(nm[i]).arg(oct6(cpu.r[i]));
        int col = i % 4, row = i / 4;
        p.drawText(rx + col * cw * 11, ry + row * lineH_, s);
    }
    p.setPen(havePrev_ && cpu.psw != prevPsw_ ? chg : fg);
    p.drawText(rx, ry + 2 * lineH_, QString("PSW=%1  [%2]")
        .arg(oct6(cpu.psw)).arg(flagsStr(cpu.psw)));
    p.setPen(fg);
    p.drawText(rx + cw * 24, ry + 2 * lineH_,
        QString(cpu.halted() ? "СОСТ: ОСТАНОВ (HALT)" : "СОСТ: ПАУЗА"));

    // ---- Breakpoints panel (top strip, right of the registers) ----
    // Lists the installed breakpoints; LMB jumps the disassembler there, RMB
    // removes it (handled in mousePressEvent), wheel scrolls the list.
    bpVisible_.clear();
    bpRect_ = QRect();
    {
        int bpX = regRect.right() + margin;
        int bpW = W - margin - bpX;
        if (bpW > 40) {
            QRect bpRect(bpX, margin, bpW, regRect.height());
            bpRect_ = bpRect;
            p.fillRect(bpRect, panelBg);
            p.setPen(border); p.drawRect(bpRect);
            const auto& bps = board_->breakpoints();
            p.setPen(title);
            p.drawText(bpRect.adjusted(6, 4, 0, 0), Qt::AlignTop | Qt::AlignLeft,
                       QString::fromUtf8("— ТОЧКИ ОСТАНОВА (%1) —").arg((int)bps.size()));
            int bpRows = (bpRect.height() - lineH_ - 6) / lineH_;
            if (bpRows < 1) bpRows = 1;
            bpScroll_ = std::clamp(bpScroll_, 0, std::max(0, (int)bps.size() - bpRows));
            int bx = bpRect.x() + 6, by = bpRect.y() + 2 * lineH_;
            int idx = 0;
            for (uint16_t addr : bps) {
                if (idx++ < bpScroll_) continue;
                if ((int)bpVisible_.size() >= bpRows) break;
                bool isPc = (addr == cpu.pc());
                p.setPen(bpCol); p.drawText(bx, by, "●");
                p.setPen(isPc ? hi : fg);
                QString s = oct6(addr) + "  " + QString::fromStdString(bk::disasm(mem, addr, &syms).text);
                p.drawText(bx + cw * 3, by, fm.elidedText(s, Qt::ElideRight, bpRect.width() - cw * 3 - 10));
                bpVisible_.push_back(addr);
                by += lineH_;
            }
            if (bps.empty()) {
                p.setPen(QColor(140, 150, 170));
                p.drawText(bx, by, QString::fromUtf8("нет точек останова"));
            }
        }
    }

    // ---- Disassembly panel (left) ----
    // Reserve a band at the bottom for the help line and size the panels snugly
    // to their content, so the gap after the frame is larger than the padding
    // left inside it after the last line.
    int dTop = regRect.bottom() + margin;
    int helpTop = (H - 4) - lineH_;             // top of the help-line band
    int panelBottomMax = helpTop - margin;      // panels end >= margin above the help line
    disasmLines_ = (panelBottomMax - dTop - 8) / lineH_ - 1;
    if (disasmLines_ < 1) disasmLines_ = 1;
    int dH = (disasmLines_ + 1) * lineH_ + 8;   // snug: title + lines + small pad
    disasmRect_ = QRect(margin, dTop, (W - 3 * margin) * 3 / 5, dH);
    p.fillRect(disasmRect_, panelBg);
    p.setPen(border); p.drawRect(disasmRect_);
    QString disTitle = QString::fromUtf8("— ДИЗАССЕМБЛЕР —");
    { QString pcSym = symLabel(cpu.pc()); if (!pcSym.isEmpty()) disTitle += "   PC: " + pcSym; }
    p.setPen(title); p.drawText(disasmRect_.adjusted(6, 4, 0, 0), Qt::AlignTop | Qt::AlignLeft, disTitle);

    uint16_t a = disasmTop_;
    int y = disasmRect_.y() + lineH_ + lineH_;
    for (int i = 0; i < disasmLines_; ++i) {
        bk::DisasmLine d = bk::disasm(mem, a, &syms);
        bool isPc = (a == cpu.pc());
        bool isBp = board_->hasBreakpoint(a);
        int x = disasmRect_.x() + 6;
        QRect lineRect(disasmRect_.x() + 2, y - lineH_ + 3, disasmRect_.width() - 4, lineH_);
        // Tint frequently-executed ("warm") code: log-scaled exec count → an
        // orange-to-red bar that deepens with heat.
        uint32_t ec = tr.execCount(a);
        if (ec && tr.execMax() > 0) {
            double heat = std::log(double(ec) + 1.0) / std::log(double(tr.execMax()) + 1.0);
            QColor warm = QColor::fromHsvF(float((28.0 - 28.0 * heat) / 360.0), 0.90f, 0.55f);
            warm.setAlpha(int(35 + 150 * heat));
            p.fillRect(lineRect, warm);
        }
        if (isPc) p.fillRect(lineRect, QColor(60, 60, 0, 160));
        if (a == cursorAddr_) {  // selected line (naming/commenting target)
            p.setPen(QPen(QColor(120, 200, 255), 1.3)); p.setBrush(Qt::NoBrush);
            p.drawRect(lineRect);
        }
        if ((int)a == link_) {   // linked highlight from another profiler window
            p.fillRect(lineRect, QColor(255, 245, 150, 45));
            p.setPen(QPen(QColor(255, 245, 150), 1.3)); p.setBrush(Qt::NoBrush);
            p.drawRect(lineRect);
        }
        p.setPen(isBp ? bpCol : fg);
        p.drawText(x, y, isBp ? "●" : (isPc ? "►" : " "));
        // raw words
        QString raw;
        for (int w = 0; w < d.words; ++w) raw += oct6(mem.peekWord(a + w * 2)) + " ";
        p.setPen(isPc ? hi : fg);
        p.drawText(x + cw * 2, y, oct6(a));
        p.setPen(QColor(140, 150, 190));
        p.drawText(x + cw * 9, y, raw);
        int textX = x + cw * 9 + cw * 8 * 3;
        QString dtext = QString::fromStdString(d.text);
        p.setPen(isPc ? hi : fg);
        p.drawText(textX, y, dtext);
        int rightLimit = disasmRect_.right() - 6;
        // Symbol defined at this line: its name, right-aligned.
        if (const std::string* ln = syms.nameAt(a)) {
            QString name = QString::fromStdString(*ln);
            int nx = rightLimit - fm.horizontalAdvance(name);
            p.setPen(symCol); p.drawText(nx, y, name);
            rightLimit = nx - cw;                 // comment must not run into the symbol
        }
        // User comment, after the instruction text.
        if (const std::string* cm = board_->comment(a)) {
            int cx = textX + fm.horizontalAdvance(dtext) + cw * 2;
            if (cx < rightLimit) {
                p.setPen(QColor(150, 165, 130));
                p.drawText(cx, y, fm.elidedText("; " + QString::fromStdString(*cm),
                                                Qt::ElideRight, rightLimit - cx));
            }
        }
        y += lineH_;
        a += d.words * 2;
    }

    // ---- Right column: memory / stack / system registers, stacked vertically.
    // Registers show two values each (written vs read), so they need the full
    // column width. ----
    int mx = disasmRect_.right() + margin;
    int mw = W - mx - margin;
    QRect memRect(mx, dTop, mw, dH * 9 / 32);
    memRect_ = memRect; // cache for the wheel handler
    p.fillRect(memRect, panelBg);
    p.setPen(border); p.drawRect(memRect);
    p.setPen(title); p.drawText(memRect.adjusted(6, 4, 0, 0), Qt::AlignTop | Qt::AlignLeft, "— ПАМЯТЬ —");
    p.setPen(fg);
    int rows = (memRect.height() - lineH_ - 6) / lineH_;
    // Only as many words per row as fit; each column ("001000:" / " 010706") is
    // 7 monospace characters wide.
    int wpr = (memRect.width() - 20 - cw * 7) / (cw * 7);
    if (wpr < 1) wpr = 1; else if (wpr > 8) wpr = 8;
    uint16_t ma = memAddr_;
    int my = memRect.y() + lineH_ + lineH_;
    for (int r = 0; r < rows; ++r) {
        QString line = oct6(ma) + ":";
        for (int c = 0; c < wpr; ++c) line += " " + oct6(mem.peekWord(ma + c * 2));
        p.drawText(memRect.x() + 10, my, line);
        my += lineH_;
        ma += static_cast<uint16_t>(wpr * 2);
    }

    // ---- Stack (middle) fills the space between memory and the bottom-anchored
    // register box, which is sized to fit exactly its title row + 9 registers. ----
    int sregH = 10 * lineH_ + 8;
    QRect stkRect(mx, memRect.bottom() + margin, mw,
                  dTop + dH - sregH - 2 * margin - memRect.bottom());
    p.fillRect(stkRect, panelBg);
    p.setPen(border); p.drawRect(stkRect);
    p.setPen(title); p.drawText(stkRect.adjusted(6, 4, 0, 0), Qt::AlignTop | Qt::AlignLeft, "— СТЕК (SP) —");
    p.setPen(fg);
    int srows = (stkRect.height() - lineH_ - 6) / lineH_;
    uint16_t sa = cpu.sp();
    int sy = stkRect.y() + lineH_ + lineH_;
    for (int r = 0; r < srows; ++r) {
        uint16_t val = mem.peekWord(sa);
        QString line = QString("%1: %2").arg(oct6(sa)).arg(oct6(val));
        // Resolve a stacked word that points at known code (e.g. a return address).
        QString sym = symLabel(val);
        if (!sym.isEmpty()) { p.setPen(fg); }
        p.drawText(stkRect.x() + 10, sy, line);
        if (!sym.isEmpty()) {
            p.setPen(symCol);
            int ax = stkRect.x() + 10 + fm.horizontalAdvance(line + "  ");
            p.drawText(ax, sy, fm.elidedText(sym, Qt::ElideRight, stkRect.right() - 6 - ax));
            p.setPen(fg);
        }
        sy += lineH_;
        sa += 2;
    }

    // ---- System registers (bottom): documented I/O registers (see
    // digger-bk0010/memory.h) with the last value written and the value read back
    // — on real hardware these often differ (e.g. keyboard/timer status). ----
    QRect sregRect(mx, stkRect.bottom() + margin, mw, sregH);
    p.fillRect(sregRect, panelBg);
    p.setPen(border); p.drawRect(sregRect);
    p.setPen(title); p.drawText(sregRect.adjusted(6, 4, 0, 0), Qt::AlignTop | Qt::AlignLeft,
                                "— СИСТ. РЕГИСТРЫ —");
    struct SReg { uint16_t addr; const char* name; };
    static const SReg sregs[] = {
        {0176560, "ИРПС"},         // последовательный порт (не эмулируется)
        {0177660, "клав. сост."},  // регистр состояния клавиатуры
        {0177662, "клав. данные"}, // регистр данных клавиатуры
        {0177664, "верт. скрл"},   // регистр вертикального смещения (скролл)
        {0177706, "тайм. лим."},   // таймер: регистр перезагрузки/захвата
        {0177710, "тайм. счёт"},   // таймер: счётчик (только чтение)
        {0177712, "тай. упр."},    // таймер: управление/статус
        {0177714, "пар. порт"},    // параллельный интерфейс (порт/джойстик)
        {0177716, "внеш устр."},   // управление внешними устройствами (динамик/лента/клавиша)
    };
    const int srx = sregRect.x() + 10, srw = sregRect.width() - 16;
    // Column headers over the two value columns (written / read), on the title row.
    p.setPen(QColor(150, 180, 230));
    p.drawText(srx + 20 * cw, sregRect.y() + lineH_, "запись");
    p.drawText(srx + 27 * cw, sregRect.y() + lineH_, "чтение");
    int gy = sregRect.y() + lineH_ + lineH_;
    for (const auto& sr : sregs) {
        if (gy > sregRect.bottom()) break;
        uint16_t rd = board_->peekReg(sr.addr);
        uint16_t wr = board_->peekRegWritten(sr.addr);
        QString flag;                        // decode a couple of read-side status bits
        if (sr.addr == 0177712) { if (rd & 020) flag += "R"; if (rd & 0200) flag += "F"; }
        else if (sr.addr == 0177716 && !(rd & 0100)) flag = "К";
        // addr | name (padded) | written | read | flag
        QString s = QString("%1 %2 %3 %4%5").arg(oct6(sr.addr))
                        .arg(QString::fromUtf8(sr.name), -12).arg(oct6(wr)).arg(oct6(rd))
                        .arg(flag.isEmpty() ? "" : " " + flag);
        p.setPen(fg);
        p.drawText(srx, gy, fm.elidedText(s, Qt::ElideRight, srw));
        gy += lineH_;
    }

    // ---- Help line ---- (slightly smaller font so all the hints fit on one line)
    QFont helpFont = mono; helpFont.setPixelSize(qMax(9, fs * 3 / 4));
    p.setFont(helpFont);
    QFontMetrics hfm(helpFont);
    p.setPen(QColor(180, 200, 255));
    QString help = QString::fromUtf8(
        "F12-выход  F7/F8-шаг  F9-тчк(PC)  Esc-продолжить  колесо/PgUp/PgDn/↑↓-дизасм  "
        "[/]-память  дизасм: ЛКМ-выбор ПКМ-тчк N-имя ;-коммент  ·  панель тчк: ЛКМ-переход ПКМ-удалить");
    p.drawText(margin, H - 4, hfm.elidedText(help, Qt::ElideRight, W - 2 * margin));
}

void DebuggerOverlay::mousePressEvent(QMouseEvent* e) {
    const QPoint pos = e->pos();
    // Breakpoints panel: left click jumps the disassembler to the breakpoint,
    // right click removes it.
    if (bpRect_.contains(pos)) {
        int idx = (pos.y() - (bpRect_.y() + 2 * lineH_) + lineH_) / lineH_;
        if (idx >= 0 && idx < (int)bpVisible_.size()) {
            uint16_t a = bpVisible_[idx];
            if (e->button() == Qt::RightButton) board_->removeBreakpoint(a);
            else                                 disasmTop_ = a;
            update();
        }
        return;
    }
    // Disasm line: left click selects it (for naming/commenting via N / ;), right
    // click toggles a breakpoint there.
    if (disasmRect_.contains(pos)) {
        int rel = pos.y() - (disasmRect_.y() + lineH_ + lineH_) + lineH_;
        int idx = rel / lineH_;
        if (idx >= 0 && idx < disasmLines_) {
            uint16_t a = disasmTop_;
            for (int i = 0; i < idx; ++i)
                a += bk::disasm(board_->memory(), a).words * 2;
            if (e->button() == Qt::RightButton) board_->toggleBreakpoint(a);
            else                                 cursorAddr_ = a;
            update();
        }
    }
}

void DebuggerOverlay::wheelEvent(QWheelEvent* e) {
    // Scroll the panel under the cursor: the disassembler by default, the memory
    // dump when hovering it. One notch (120 units) = a few lines.
    int notches = e->angleDelta().y() / 120;
    if (notches == 0) { e->ignore(); return; }
    const QPoint pos = e->position().toPoint();
    if (disasmRect_.contains(pos))   scrollDisasm(-notches * 3); // wheel up = earlier addrs
    else if (memRect_.contains(pos)) scrollMem(-notches);
    else if (bpRect_.contains(pos))  { bpScroll_ = std::max(0, bpScroll_ - notches); update(); }
    else { e->ignore(); return; }
    e->accept();
}
