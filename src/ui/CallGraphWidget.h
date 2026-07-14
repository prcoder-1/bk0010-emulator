#pragma once
#include <QWidget>
#include <QRectF>
#include <QPointF>
#include <QTransform>
#include <cstdint>
#include <vector>
#include <unordered_map>

namespace bk { class Board; }

// A KCachegrind-style call graph: executed subroutines are drawn as boxes,
// coloured by how much CPU time they cost (cold blue → hot red), connected by
// call arrows whose thickness grows with call frequency. Laid out top-down in
// levels (callers above callees). Opened from the Отладка menu; pan by dragging,
// zoom with the wheel, click a box to jump the disassembler to that routine.
class CallGraphWidget : public QWidget {
    Q_OBJECT
public:
    explicit CallGraphWidget(bk::Board* board, QWidget* parent = nullptr);
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
    struct Node {
        uint16_t entry;   // subroutine entry address
        uint64_t cost;    // CPU-time cost (Σ execCount × ticks) of its instructions
        uint32_t calls;   // times it was called (Σ incoming JSR edge weight)
        int      level;   // layout row (call depth)
        double   x, y;    // centre-x, top-y in graph coordinates
        QRectF   box;     // cached rectangle in graph coordinates
    };
    struct Edge { uint16_t from, to; uint64_t weight; };

    void rebuild();                 // build nodes/edges + layout from the trace
    void fitToView();               // set zoom/pan so the whole graph is visible
    QTransform viewTransform() const;

    bk::Board* board_;
    std::vector<Node> nodes_;
    std::vector<Edge> edges_;
    std::unordered_map<uint16_t, int> nodeIndex_;  // entry → index into nodes_
    uint64_t maxCost_ = 1, totalCost_ = 1, maxWeight_ = 1;

    int      topN_ = 40;            // keep the N costliest functions
    uint32_t lastBuild_ = 0;        // trace clock at last rebuild (throttle)

    double  zoom_ = 1.0;
    QPointF pan_{0, 0};             // view offset (px)
    bool    fitView_ = true;        // auto-fit until the user pans/zooms
    bool    dragging_ = false;
    QPoint  pressPos_, lastDrag_;
};
