#pragma once
#include <QWidget>
#include <QRect>
#include <QPoint>
#include <cstdint>
#include <vector>
#include <set>

namespace bk { class Board; }

// "Горячий путь": the hottest execution paths in the running program. A path is a
// trace of basic blocks linked by the most-frequently-taken branches; the top-N
// by CPU time are shown as a collapsible outline. Each path collapses to a summary
// row; expanding it reveals its basic blocks, and expanding a block reveals its
// instructions (disassembly with per-instruction execution counts). Rows are
// tinted by execution heat. Rebuilt periodically from the execution trace.
//
// Opened from the Отладка menu. Scroll with the wheel or by dragging; click a
// row's triangle to fold/unfold, click its body to jump the disassembler there;
// right-click hides a swamping address; +/- change how many paths are shown.
class HotPathWidget : public QWidget {
    Q_OBJECT
public:
    explicit HotPathWidget(bk::Board* board, QWidget* parent = nullptr);
    void refresh();

signals:
    void addressPicked(uint16_t addr);

protected:
    void paintEvent(QPaintEvent*) override;
    void wheelEvent(QWheelEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;
    void mouseReleaseEvent(QMouseEvent*) override;
    void keyPressEvent(QKeyEvent*) override;

private:
    struct Instr { uint16_t addr; uint32_t count; };
    struct Block {
        uint16_t leader;
        uint16_t endAddr;   // address just past the last instruction
        uint32_t count;     // entry execution count
        int first, last;    // inclusive index range into instrs_
    };
    struct Path {
        std::vector<int> blocks;   // indices into blocks_ (in execution order)
        uint64_t cost;             // Σ count×ticks over its instructions (ranking)
        uint16_t entry;            // leader of the first block
    };
    void rebuild();

    enum { K_PATH, K_BLOCK, K_INSTR };
    struct Row { QRect rect; int level; int kind; uint16_t addr; uint16_t key; bool expandable; };

    bk::Board* board_;
    std::vector<Instr> instrs_;     // executed instructions, address order
    std::vector<Block> blocks_;
    std::vector<Path>  paths_;
    uint64_t totalCost_ = 1;
    uint32_t maxCount_  = 1;

    std::set<uint16_t> pathOpen_;   // expanded path entries
    std::set<uint16_t> blockOpen_;  // expanded block leaders
    std::set<uint16_t> excluded_;   // right-click-hidden addresses

    int      topN_ = 12;
    uint32_t lastBuild_ = 0;
    double   scroll_ = 0.0;         // vertical scroll offset (px, ≥0)
    int      rowH_ = 16;
    double   contentH_ = 0.0;
    bool     dragging_ = false;
    QPoint   lastDrag_, pressPos_;
    std::vector<Row> rows_;         // rows from the last paint (hit-testing)
};
