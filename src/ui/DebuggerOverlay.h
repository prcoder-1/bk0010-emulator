#pragma once
#include <QWidget>
#include <QRect>
#include <cstdint>
#include <vector>

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
    void snapshotRegs();             // remember current regs so the next paint can highlight changes
    void setDisasmAddr(uint16_t a) { disasmTop_ = a; update(); } // jump disasm to addr
    void setMemAddr(uint16_t a) { memAddr_ = a; update(); }
    // Linked highlighting: mark the disasm line at `addr` (-1 = none).
    void setHighlight(int addr) { if (link_ != addr) { link_ = addr; update(); } }
    void scrollDisasm(int lines);    // move the disasm window
    void scrollMem(int rows);

protected:
    void paintEvent(QPaintEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
    void wheelEvent(QWheelEvent*) override;

private:
    bk::Board* board_;
    uint16_t disasmTop_ = 0;         // first address shown in the disasm panel
    uint16_t memAddr_   = 01000;     // first address shown in the memory panel
    int lineH_ = 14;                 // pixel height of a text line (recomputed)
    int disasmLines_ = 20;
    int link_ = -1;                  // linked-highlight address (-1 = none)
    int bpScroll_ = 0;               // index of the first breakpoint shown (wheel-scroll)
    uint16_t prevR_[8] = {0};        // registers at the last snapshot (changed-reg highlight)
    uint16_t prevPsw_ = 0;
    bool havePrev_ = false;

    // Layout rectangles (computed each paint) used by the mouse handlers.
    QRect disasmRect_;
    QRect memRect_;
    QRect bpRect_;                   // breakpoints panel (top-right of the registers)
    std::vector<uint16_t> bpVisible_; // breakpoint addresses currently drawn, row order (hit test)
};
