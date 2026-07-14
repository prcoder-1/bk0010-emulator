#pragma once
#include <QWidget>
#include <QRectF>
#include <QPoint>
#include <cstdint>
#include <vector>

namespace bk { class Board; }

// Time-ordered flame chart (like Chrome DevTools / speedscope): the X axis is CPU
// time, the Y axis is call-stack depth, and each horizontal bar is one subroutine
// invocation spanning the ticks it was on the stack. Reconstructed from Trace's
// timestamped call spans. The right edge is "now"; wheel zooms the time window,
// drag scrolls depth, click jumps the disassembler, hover highlights across
// windows. Opened from the Отладка menu.
class FlameChartWidget : public QWidget {
    Q_OBJECT
public:
    explicit FlameChartWidget(bk::Board* board, QWidget* parent = nullptr);
    void refresh();
    void setHighlight(int addr) { if (link_ != addr) { link_ = addr; update(); } }

signals:
    void addressPicked(uint16_t addr);
    void hoverAddress(int addr);

protected:
    void paintEvent(QPaintEvent*) override;
    void wheelEvent(QWheelEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;
    void mouseReleaseEvent(QMouseEvent*) override;
    void leaveEvent(QEvent*) override;

private:
    struct Bar { QRectF rect; uint16_t func; };
    bk::Board* board_;
    double winTicks_ = 2.0e6;    // visible time window width, in CPU ticks
    int    rowH_ = 16;
    double scroll_ = 0.0;        // vertical (depth) scroll, px
    int    maxDepth_ = 0;
    int    link_ = -1, hoverEmit_ = -2;
    bool   dragging_ = false;
    QPoint pressPos_, lastDrag_;
    std::vector<Bar> bars_;      // laid-out bars from the last paint (hit test)
};
