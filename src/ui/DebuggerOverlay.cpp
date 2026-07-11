#include "DebuggerOverlay.h"
#include "Board.h"
#include "Disasm.h"
#include <QPainter>
#include <QMouseEvent>
#include <QFontDatabase>
#include <cstdio>

using bk::Board;

DebuggerOverlay::DebuggerOverlay(Board* board, QWidget* parent)
    : QWidget(parent), board_(board) {
    setAttribute(Qt::WA_NoSystemBackground);
    setAutoFillBackground(false);
    disasmTop_ = board_->cpu().pc();
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
    int fs = qMax(10, height() / 42);
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

    auto& cpu = board_->cpu();
    auto& mem = board_->memory();

    int margin = fs;
    int W = width(), H = height();

    // ---- Registers panel (top strip) ----
    QRect regRect(margin, margin, W - 2 * margin, lineH_ * 4 + fs);
    p.fillRect(regRect, panelBg);
    p.setPen(border); p.drawRect(regRect);
    p.setPen(title); p.drawText(regRect.adjusted(6, 4, 0, 0), Qt::AlignTop | Qt::AlignLeft, "— РЕГИСТРЫ —");
    p.setPen(fg);
    int rx = regRect.x() + 6, ry = regRect.y() + lineH_ + 4;
    for (int i = 0; i < 8; ++i) {
        static const char* nm[8] = {"R0", "R1", "R2", "R3", "R4", "R5", "SP", "PC"};
        QString s = QString("%1=%2").arg(nm[i]).arg(oct6(cpu.r[i]));
        int col = i % 4, row = i / 4;
        p.drawText(rx + col * cw * 11, ry + row * lineH_, s);
    }
    p.drawText(rx, ry + 2 * lineH_, QString("PSW=%1  [%2]")
        .arg(oct6(cpu.psw)).arg(flagsStr(cpu.psw)));
    p.drawText(rx + cw * 24, ry + 2 * lineH_,
        QString(cpu.halted() ? "СОСТ: ОСТАНОВ (HALT)" : "СОСТ: ПАУЗА"));

    // ---- Disassembly panel (left) ----
    int dTop = regRect.bottom() + margin;
    int dH = H - dTop - margin;
    disasmRect_ = QRect(margin, dTop, (W - 3 * margin) * 3 / 5, dH);
    p.fillRect(disasmRect_, panelBg);
    p.setPen(border); p.drawRect(disasmRect_);
    p.setPen(title); p.drawText(disasmRect_.adjusted(6, 4, 0, 0), Qt::AlignTop | Qt::AlignLeft, "— ДИЗАССЕМБЛЕР —");

    disasmLines_ = (disasmRect_.height() - lineH_ - 6) / lineH_;
    uint16_t a = disasmTop_;
    int y = disasmRect_.y() + lineH_ + lineH_;
    for (int i = 0; i < disasmLines_; ++i) {
        bk::DisasmLine d = bk::disasm(mem, a);
        bool isPc = (a == cpu.pc());
        bool isBp = board_->hasBreakpoint(a);
        int x = disasmRect_.x() + 6;
        if (isPc) {
            p.fillRect(QRect(disasmRect_.x() + 2, y - lineH_ + 3,
                             disasmRect_.width() - 4, lineH_), QColor(60, 60, 0, 160));
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
        p.setPen(isPc ? hi : fg);
        p.drawText(x + cw * 9 + cw * 8 * 3, y, QString::fromStdString(d.text));
        y += lineH_;
        a += d.words * 2;
    }

    // ---- Memory panel (right-top) ----
    int mx = disasmRect_.right() + margin;
    int mw = W - mx - margin;
    QRect memRect(mx, dTop, mw, dH * 3 / 5);
    p.fillRect(memRect, panelBg);
    p.setPen(border); p.drawRect(memRect);
    p.setPen(title); p.drawText(memRect.adjusted(6, 4, 0, 0), Qt::AlignTop | Qt::AlignLeft, "— ПАМЯТЬ —");
    p.setPen(fg);
    int rows = (memRect.height() - lineH_ - 6) / lineH_;
    uint16_t ma = memAddr_;
    int my = memRect.y() + lineH_ + lineH_;
    for (int r = 0; r < rows; ++r) {
        QString line = oct6(ma) + ":";
        for (int c = 0; c < 8; ++c) line += " " + oct6(mem.peekWord(ma + c * 2));
        p.drawText(memRect.x() + 6, my, line);
        my += lineH_;
        ma += 16;
    }

    // ---- Stack panel (right-bottom) ----
    QRect stkRect(mx, memRect.bottom() + margin, mw, dH - memRect.height() - margin);
    p.fillRect(stkRect, panelBg);
    p.setPen(border); p.drawRect(stkRect);
    p.setPen(title); p.drawText(stkRect.adjusted(6, 4, 0, 0), Qt::AlignTop | Qt::AlignLeft, "— СТЕК (SP) —");
    p.setPen(fg);
    int srows = (stkRect.height() - lineH_ - 6) / lineH_;
    uint16_t sa = cpu.sp();
    int sy = stkRect.y() + lineH_ + lineH_;
    for (int r = 0; r < srows; ++r) {
        p.drawText(stkRect.x() + 6, sy,
            QString("%1: %2").arg(oct6(sa)).arg(oct6(mem.peekWord(sa))));
        sy += lineH_;
        sa += 2;
    }

    // ---- Help line ----
    p.setPen(QColor(180, 200, 255));
    p.drawText(margin, H - 4,
        "F12-выход  F7-шаг(в)  F8-шаг(через)  F9-тчк.останова  G-продолжить  "
        "PgUp/PgDn-дизасм  [/]-память");
}

void DebuggerOverlay::mousePressEvent(QMouseEvent* e) {
    // Click on a disasm line toggles a breakpoint there.
    if (disasmRect_.contains(e->pos())) {
        int rel = e->pos().y() - (disasmRect_.y() + lineH_ + lineH_) + lineH_;
        int idx = rel / lineH_;
        if (idx >= 0 && idx < disasmLines_) {
            uint16_t a = disasmTop_;
            for (int i = 0; i < idx; ++i)
                a += bk::disasm(board_->memory(), a).words * 2;
            board_->toggleBreakpoint(a);
            update();
        }
    }
}
