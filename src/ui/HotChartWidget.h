#pragma once
#include <QWidget>
#include <QRect>
#include <QColor>
#include <cstdint>
#include <vector>
#include <deque>
#include <map>
#include <set>
#include <utility>

namespace bk { class Board; }

// A time-series chart of the top-N hottest instructions. Each line traces one
// instruction's executions-per-interval over time; lines are coloured by the
// subroutine (function) the instruction belongs to, so activity of different
// functions is easy to tell apart. Opened from the Отладка menu.
class HotChartWidget : public QWidget {
    Q_OBJECT
public:
    explicit HotChartWidget(bk::Board* board, QWidget* parent = nullptr);
    // Driven from the 50 Hz loop; takes a new sample once enough emulated frames
    // have elapsed (so the time axis follows emulated, not wall-clock, time).
    void refresh();

signals:
    // Emitted when the user clicks a legend row, so the debugger's disassembler
    // can jump to that instruction (same wiring as the code graph).
    void addressPicked(uint16_t addr);

protected:
    void paintEvent(QPaintEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
    void keyPressEvent(QKeyEvent*) override;
    void wheelEvent(QWheelEvent*) override;

private:
    void sample();
    uint16_t funcOf(uint16_t addr) const;  // nearest subroutine entry <= addr (0 = none)

    bk::Board* board_;
    int topN_ = 10;
    static constexpr int HIST = 320;         // samples retained (time-window width)
    static constexpr int SAMPLE_FRAMES = 4;  // emulated frames per sample (~12 Hz)
    uint32_t lastNow_ = 0;
    uint64_t lastTotalTicks_ = 0;   // grand-total CPU ticks at the previous sample

    // hist = per-interval share of CPU time (%); total = cumulative call count;
    // pctAll = cumulative share of total CPU time (%). last = previous call count.
    struct Series { uint16_t addr; uint32_t last; std::deque<double> hist; uint32_t total; double pctAll; };
    std::vector<Series> series_;
    std::set<uint16_t> subEntries_;          // JSR destinations (function entries)
    std::set<uint16_t> excluded_;            // right-click-hidden instructions (Del resets)
    std::vector<std::pair<QRect, uint16_t>> legendRows_; // click hit-testing
};
