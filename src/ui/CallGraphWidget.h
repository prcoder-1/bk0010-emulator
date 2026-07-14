#pragma once
#include <QWidget>
#include <QRectF>
#include <QPointF>
#include <QTransform>
#include <cstdint>
#include <vector>
#include <unordered_map>
#include <set>

namespace bk { class Board; }

// A profiling call graph: executed subroutines are drawn as boxes, coloured by
// how much CPU time they cost (cold blue → hot red), connected by call arrows
// whose thickness grows logarithmically with call frequency. Laid out top-down
// in levels (callers above callees). Routines not called for a while darken (but
// stay visible); Delete/Backspace purges the fully-darkened ones. A second view
// (Tab) shows the same data as a nested-rectangles "callee map" (area ∝ cost).
// Opened from the Отладка menu; pan by dragging, zoom with the wheel, click a
// box to jump the disassembler.
class CallGraphWidget : public QWidget {
    Q_OBJECT
public:
    explicit CallGraphWidget(bk::Board* board, QWidget* parent = nullptr);
    void refresh();
    // Linked highlighting: mark the routine at `addr` (-1 = none) as highlighted.
    void setHighlight(int addr) { if (link_ != addr) { link_ = addr; update(); } }

signals:
    void addressPicked(uint16_t addr);
    void hoverAddress(int addr);   // routine under the cursor (-1 = none)

protected:
    void paintEvent(QPaintEvent*) override;
    void wheelEvent(QWheelEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;
    void mouseReleaseEvent(QMouseEvent*) override;
    void leaveEvent(QEvent*) override;
    void keyPressEvent(QKeyEvent*) override;
    bool event(QEvent*) override;          // intercept Tab (view toggle)

private:
    struct Node {
        uint16_t entry;      // subroutine entry address
        uint64_t cost;       // CPU-time cost (Σ execCount × ticks) of its instructions
        uint32_t calls;      // times it was called (Σ incoming JSR edge weight)
        uint32_t lastActive; // trace timestamp of its most recent execution (for fade)
        int      level;      // layout row (call depth)
        double   x, y;       // centre-x, top-y in graph coordinates
        QRectF   box;        // cached rectangle in graph coordinates
    };
    struct Edge {
        uint16_t from, to;
        uint64_t weight;
        std::vector<QPointF> bends;  // routing channel points (dummy-node centres)
    };

    // A node in the nested-treemap ("callee map") view: functions of the call
    // tree, area ∝ subtree cost, callees nested inside callers.
    struct TNode {
        uint16_t entry;
        uint64_t self;      // own CPU-time cost
        uint64_t subtree;   // self + all descendants
        int      depth;     // 0 = virtual root
        std::vector<int> kids;
        QRectF   rect;      // laid-out rectangle (screen coordinates)
    };

    enum Mode { ModeGraph = 0, ModeTree = 1 };

    void rebuild();                 // build nodes/edges + layout from the trace
    void buildTree();               // build the call tree for the treemap view
    void layoutTree(int idx, QRectF r);
    void paintGraph(QPainter& p);   // layered box-and-arrow view
    void paintTreeMap(QPainter& p); // nested-rectangles (callee map) view
    void fitToView();               // set zoom/pan so the whole graph is visible
    QTransform viewTransform() const;

    bk::Board* board_;
    std::vector<Node> nodes_;
    std::vector<Edge> edges_;
    std::vector<TNode> tree_;       // treemap call tree (tree_[0] = virtual root)
    std::unordered_map<uint16_t, int> nodeIndex_;  // entry → index into nodes_
    std::set<uint16_t> hidden_;     // functions purged by the user (Delete/Backspace)
    std::unordered_map<uint16_t, QPointF> manualPos_;  // user-dragged node positions
    bool     draggingNode_ = false; // a box is being dragged (not the canvas)
    uint16_t dragEntry_ = 0;        // which node (by entry) is being dragged
    QPointF  dragOffset_;           // cursor→node-anchor offset at grab (graph coords)
    int mode_ = ModeGraph;
    uint64_t maxCost_ = 1, totalCost_ = 1, maxWeight_ = 1;

    int      topN_ = 12;            // show only the N costliest blocks (+/- adjust)
    uint32_t lastBuild_ = 0;        // trace clock at last rebuild (throttle)

    double  zoom_ = 1.0;
    QPointF pan_{0, 0};             // view offset (px)
    bool    fitView_ = true;        // auto-fit until the user pans/zooms
    bool    dragging_ = false;
    QPoint  pressPos_, lastDrag_;
    int     link_ = -1;             // linked-highlight address (-1 = none)
    int     hoverEmit_ = -2;        // last address broadcast via hoverAddress
};
