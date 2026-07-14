#pragma once
#include <QWidget>
#include <QRectF>
#include <cstdint>
#include <vector>

namespace bk { class Board; }

// Flame graph (icicle orientation): the reconstructed calling-context tree, where
// each box is a call frame, its width ∝ inclusive CPU time (ticks spent with that
// frame on the stack), stacked by call depth (root/main at the top, callees below).
// Built from Trace's per-instruction shadow call stack. Click a box to drill into
// its subtree, right-click / Backspace to go back up, click jumps the disassembler
// to the routine. Opened from the Отладка menu.
class FlameWidget : public QWidget {
    Q_OBJECT
public:
    explicit FlameWidget(bk::Board* board, QWidget* parent = nullptr);
    void refresh();

signals:
    void addressPicked(uint16_t addr);

protected:
    void paintEvent(QPaintEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;
    void wheelEvent(QWheelEvent*) override;
    void keyPressEvent(QKeyEvent*) override;
    void leaveEvent(QEvent*) override;

private:
    struct Box { QRectF rect; int node; };
    void rebuild();                 // snapshot inclusive costs from the CCT

    bk::Board* board_;
    std::vector<uint64_t> incl_;    // inclusive ticks per flame node
    uint64_t total_ = 1;            // inclusive ticks of the whole program (root)
    int      focus_ = 0;            // drilled-in subtree root (0 = whole program)
    int      hover_ = -1;
    double   scroll_ = 0.0;         // vertical scroll (px)
    double   contentH_ = 0.0;
    uint32_t lastBuild_ = 0;
    int      rowH_ = 18;
    std::vector<Box> boxes_;        // laid-out boxes from the last paint (hit test)
};
