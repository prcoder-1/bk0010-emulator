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
    std::deque<uint64_t> sampleTicks_;  // CPU-tick timestamp at each sample column
    bool     showFrames_ = true;    // draw timer-based frame boundary markers (F)

    // One tracked key = an instruction, or a subroutine when aggregating. hist =
    // per-interval share of CPU time (%); lastTicks = cumulative ticks at the
    // previous sample; calls = representative call count; pctAll = cumulative
    // share of total CPU time (%).
    struct Series { uint16_t addr; uint64_t lastTicks; std::deque<double> hist; uint32_t calls; double pctAll; };
    std::vector<Series> series_;
    bool aggregate_ = false;   // rank/plot by subroutine instead of instruction (G)
    bool stacked_ = false;     // stacked-area (streamgraph) instead of lines (S)
    std::set<uint16_t> subEntries_;          // JSR destinations (function entries)
    std::set<uint16_t> excluded_;            // right-click-hidden instructions (Del resets)
    std::vector<std::pair<QRect, uint16_t>> legendRows_; // click hit-testing

    // Visible slice of the history, as fractions 0..1 (0 = oldest, 1 = newest).
    // Wheel over the plot zooms this range; the legend keeps the old wheel (N).
    double viewL_ = 0.0, viewR_ = 1.0;
    QRect  plotRect_;                        // plot area from the last paint
    bool   dragging_ = false;                // horizontal pan of the time axis
    QPoint lastDrag_;

    void mouseMoveEvent(QMouseEvent*) override;
    void mouseReleaseEvent(QMouseEvent*) override;
};
