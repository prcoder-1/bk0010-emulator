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
    QColor colorForFunc(uint16_t entry);

    bk::Board* board_;
    int topN_ = 10;
    static constexpr int HIST = 320;         // samples retained (time-window width)
    static constexpr int SAMPLE_FRAMES = 4;  // emulated frames per sample (~12 Hz)
    uint32_t lastNow_ = 0;

    struct Series { uint16_t addr; uint32_t last; std::deque<uint32_t> hist; uint32_t total; };
    std::vector<Series> series_;
    std::set<uint16_t> subEntries_;          // JSR destinations (function entries)
    std::map<uint16_t, QColor> funcColor_;   // stable colour per function entry
    int nextColor_ = 0;
    std::vector<std::pair<QRect, uint16_t>> legendRows_; // click hit-testing
};
