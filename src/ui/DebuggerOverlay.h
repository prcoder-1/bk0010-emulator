#pragma once
#include <QWidget>
#include <cstdint>

namespace bk { class Board; }

// Soft-ICE style translucent debugger drawn on top of the BK screen. Shows
// register, disassembly, memory and stack panels. Painting is on-demand; the
// MainWindow drives stepping and toggles visibility with a hotkey. The BK
// screen underneath stays visible through the translucent panels.
class DebuggerOverlay : public QWidget {
    Q_OBJECT
public:
    explicit DebuggerOverlay(bk::Board* board, QWidget* parent = nullptr);

    void followPc();                 // scroll disasm so PC is visible
    void setDisasmAddr(uint16_t a) { disasmTop_ = a; update(); } // jump disasm to addr
    void setMemAddr(uint16_t a) { memAddr_ = a; update(); }
    void scrollDisasm(int lines);    // move the disasm window
    void scrollMem(int rows);

protected:
    void paintEvent(QPaintEvent*) override;
    void mousePressEvent(QMouseEvent*) override;

private:
    bk::Board* board_;
    uint16_t disasmTop_ = 0;         // first address shown in the disasm panel
    uint16_t memAddr_   = 01000;     // first address shown in the memory panel
    int lineH_ = 14;                 // pixel height of a text line (recomputed)
    int disasmLines_ = 20;

    // Layout rectangles (computed each paint) used by mousePressEvent.
    QRect disasmRect_;
};
