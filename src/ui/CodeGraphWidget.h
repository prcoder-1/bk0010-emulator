#pragma once
#include <QWidget>
#include <QRect>
#include <QPoint>
#include <QElapsedTimer>
#include <cstdint>

namespace bk { class Board; }

// Visualises executed code: a control-flow graph (executed instructions as
// evenly-spaced nodes, branch/call edges as arcs) plus a ranked list of the
// hottest instructions. The graph can be scrolled (drag / arrows / wheel) and
// zoomed (Ctrl+wheel or +/-), so crowded traces can be inspected.
class CodeGraphWidget : public QWidget {
    Q_OBJECT
public:
    explicit CodeGraphWidget(bk::Board* board, QWidget* parent = nullptr);
    void refresh() { update(); }

protected:
    void paintEvent(QPaintEvent*) override;
    void wheelEvent(QWheelEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;
    void mouseReleaseEvent(QMouseEvent*) override;
    void keyPressEvent(QKeyEvent*) override;

private:
    void zoomAt(double factor, double centerY);
    void clampPan();
    void userInteracted();   // switch to manual mode, restart the idle timer

    bk::Board* board_;

    // View state for the graph panel.
    double zoom_ = 1.0;      // 1 = fit panel height; >1 = taller content
    double panY_ = 0.0;      // vertical scroll offset (px, <= 0)
    bool   dragging_ = false;
    QPoint lastDrag_;

    // Auto-follow: glide to the hottest instructions until the user moves the
    // view; resume automatically after an idle period.
    bool          autoFollow_ = true;
    QElapsedTimer sinceInteraction_;

    // Cached layout from the last paint (used by the event handlers).
    QRect  graphRect_;
    double contentH_ = 0.0;
};
