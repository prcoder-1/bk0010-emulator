#include "CodeGraphWidget.h"
#include "Board.h"
#include "Disasm.h"
#include <QPainter>
#include <QPainterPath>
#include <QWheelEvent>
#include <QMouseEvent>
#include <QKeyEvent>
#include <algorithm>
#include <vector>
#include <unordered_map>
#include <cmath>

using bk::Board;

// Format a large count with a K/M/G suffix, e.g. 1209 -> "1.2K", 184090 -> "184K".
static QString fmtCount(uint32_t n) {
    struct { double div; const char* suf; } units[] = {{1e9, "G"}, {1e6, "M"}, {1e3, "K"}};
    for (auto& u : units) {
        if (n >= u.div) {
            double v = n / u.div;
            return (v < 100.0) ? QString::number(v, 'f', 1) + u.suf
                               : QString::number(int(v + 0.5)) + u.suf;
        }
    }
    return QString::number(n);
}

CodeGraphWidget::CodeGraphWidget(Board* board, QWidget* parent)
    : QWidget(parent), board_(board) {
    setWindowTitle("Граф кода и горячие точки");
    setMinimumSize(700, 520);
    setFocusPolicy(Qt::StrongFocus);
    setMouseTracking(false);
    sinceInteraction_.start();
}

void CodeGraphWidget::userInteracted() {
    autoFollow_ = false;
    sinceInteraction_.restart();
}

void CodeGraphWidget::clampPan() {
    double visH = graphRect_.height() - 12;
    if (contentH_ <= visH) { panY_ = 0.0; return; }
    if (panY_ > 0.0) panY_ = 0.0;
    if (panY_ < visH - contentH_) panY_ = visH - contentH_;
}

void CodeGraphWidget::zoomAt(double factor, double centerY) {
    double top = graphRect_.top() + 6;
    double visH = graphRect_.height() - 12;
    double oldContent = std::max(1.0, visH * zoom_);
    double f = (centerY - top - panY_) / oldContent; // content fraction under the point
    double newZoom = std::min(80.0, std::max(1.0, zoom_ * factor));
    if (newZoom == zoom_) return;
    zoom_ = newZoom;
    contentH_ = visH * zoom_;
    panY_ = centerY - top - f * contentH_;            // keep that point fixed
    clampPan();
    update();
}

void CodeGraphWidget::wheelEvent(QWheelEvent* e) {
    userInteracted();
    double dy = e->angleDelta().y();
    if (e->modifiers() & Qt::ControlModifier) {
        zoomAt(std::pow(1.2, dy / 120.0), e->position().y());
    } else {
        panY_ += dy * 0.5;                            // wheel scrolls vertically
        clampPan();
        update();
    }
    e->accept();
}

void CodeGraphWidget::mousePressEvent(QMouseEvent* e) {
    if (e->button() == Qt::LeftButton && graphRect_.contains(e->pos())) {
        dragging_ = true;
        lastDrag_ = e->pos();
    }
}

void CodeGraphWidget::mouseMoveEvent(QMouseEvent* e) {
    if (dragging_) {
        userInteracted();
        panY_ += e->pos().y() - lastDrag_.y();
        lastDrag_ = e->pos();
        clampPan();
        update();
    }
}

void CodeGraphWidget::mouseReleaseEvent(QMouseEvent*) { dragging_ = false; }

void CodeGraphWidget::keyPressEvent(QKeyEvent* e) {
    double cy = graphRect_.center().y();
    double page = graphRect_.height() * 0.8;
    switch (e->key()) {
    case Qt::Key_Plus: case Qt::Key_Equal:   userInteracted(); zoomAt(1.25, cy); break;
    case Qt::Key_Minus:                      userInteracted(); zoomAt(1.0 / 1.25, cy); break;
    case Qt::Key_Up:      userInteracted(); panY_ += 40; clampPan(); update(); break;
    case Qt::Key_Down:    userInteracted(); panY_ -= 40; clampPan(); update(); break;
    case Qt::Key_PageUp:  userInteracted(); panY_ += page; clampPan(); update(); break;
    case Qt::Key_PageDown:userInteracted(); panY_ -= page; clampPan(); update(); break;
    // Home resumes auto-follow of the hot instructions.
    case Qt::Key_Home: case Qt::Key_0: autoFollow_ = true; update(); break;
    default: QWidget::keyPressEvent(e); return;
    }
    e->accept();
}

void CodeGraphWidget::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.fillRect(rect(), QColor(10, 10, 16));

    bk::Trace& tr = board_->trace();
    tr.setEnabled(true);
    const bk::Memory& mem = board_->memory();

    // Font scales with window height.
    const int fs = std::max(9, height() / 46);
    QFont mono("monospace"); mono.setStyleHint(QFont::TypeWriter); mono.setPixelSize(fs);
    p.setFont(mono);
    QFontMetrics fm(mono);
    const int lineH = fm.height() + 2;

    // ---- Collect recently-executed instructions (nodes), sorted by address ----
    // Instructions not executed within GRAPH_WINDOW frames drop out of the graph,
    // so it tracks the currently-active code rather than everything ever run.
    const int GRAPH_WINDOW = 300; // frames (~6 s at 50 Hz) since last execution
    std::vector<std::pair<uint16_t, uint32_t>> nodes; // (addr, count)
    uint32_t localMax = 1;        // max exec count among the visible nodes
    for (int a = 0; a < 0x10000; a += 2) {
        uint16_t a16 = static_cast<uint16_t>(a);
        uint32_t c = tr.execCount(a16);
        if (c && tr.fade(tr.lastExec(a16), GRAPH_WINDOW) > 0.0) {
            nodes.push_back({a16, c});
            if (c > localMax) localMax = c;
        }
    }
    std::unordered_map<uint16_t, int> indexOf;
    for (int i = 0; i < (int)nodes.size(); ++i) indexOf[nodes[i].first] = i;
    const int N = (int)nodes.size();
    const double lmax = std::log2(double(localMax) + 1.0) + 1e-6;

    // ---- Layout: left = control-flow graph, right = hot list ----
    const int margin = fs + 4;
    const int headerY = margin + fm.ascent();
    const int graphW = width() * 54 / 100;
    p.setPen(QColor(200, 210, 255));
    p.drawText(margin, headerY, "Граф исполнения  (cyan → вперёд,  orange → цикл)");
    QRect g(margin, headerY + 8, graphW - margin, height() - headerY - 8 - (lineH + 8));
    p.setPen(QColor(50, 60, 90)); p.drawRect(g);

    // Cache layout for the wheel/mouse/key handlers, apply zoom/pan.
    graphRect_ = g;
    const double visH = g.height() - 12;

    // Auto-follow: until the user moves the view (and again after ~4 s idle),
    // glide the zoom/scroll so the hottest instructions stay centred and legible.
    if (!autoFollow_ && sinceInteraction_.elapsed() > 4000) autoFollow_ = true;
    if (autoFollow_ && N > 0) {
        int hotIdx = 0; uint32_t hotMax = 0;
        for (int i = 0; i < N; ++i)
            if (nodes[i].second > hotMax) { hotMax = nodes[i].second; hotIdx = i; }
        // Zoom so node spacing reaches label height (labels become readable).
        double targetZoom = std::min(60.0, std::max(1.0, N * (fm.height() + 2.0) / std::max(1.0, visH)));
        zoom_ += (targetZoom - zoom_) * 0.08;
        double tContent = visH * zoom_;
        double targetPanY = visH * 0.5 - 6.0 - (hotIdx + 0.5) / N * tContent;
        panY_ += (targetPanY - panY_) * 0.08;
    }

    contentH_ = visH * zoom_;
    clampPan();

    // Vertical position of a node = its index spread over the (zoomed) content.
    const double nodeSpacing = (N > 0) ? contentH_ / N : 0.0;
    const bool showLabels = (N > 0) && (nodeSpacing >= fm.height() - 1);
    const int labelW = showLabels ? fm.horizontalAdvance("022010 BICB (SP)+,(R1)+ ") : 0;
    const double laneX = g.left() + 8 + labelW;

    auto yOf = [&](uint16_t addr) -> double {
        auto it = indexOf.find(addr);
        int idx = (it != indexOf.end()) ? it->second : 0;
        if (N <= 1) return g.center().y();
        return g.top() + 6 + panY_ + (idx + 0.5) / N * contentH_;
    };
    auto hotFrac = [&](uint32_t c) { return std::log2(double(c) + 1.0) / lmax; };

    if (N == 0) {
        p.setPen(QColor(150, 150, 170));
        p.drawText(g.adjusted(10, 10, 0, 0), Qt::AlignLeft | Qt::AlignTop,
                   "Нет данных. Запустите игру, чтобы собрать трассу.");
    }

    // Clip graph drawing to the panel so zoomed content never spills over.
    p.save();
    p.setClipRect(g.adjusted(1, 1, -1, -1));

    // ---- Edges (behind nodes) ----
    std::vector<std::pair<uint32_t, uint32_t>> edges; // (count, key)
    for (auto& e : tr.edges()) edges.push_back({e.second, e.first});
    std::sort(edges.rbegin(), edges.rend());
    const int nEdges = std::min<int>(400, (int)edges.size());
    const uint32_t eMax = edges.empty() ? 1 : edges[0].first;
    const double rightSpace = g.right() - laneX - 6;
    for (int i = 0; i < nEdges; ++i) {
        uint16_t from = edges[i].second >> 16, to = edges[i].second & 0xFFFF;
        if (!indexOf.count(from) || !indexOf.count(to)) continue;
        double y0 = yOf(from), y1 = yOf(to);
        if ((y0 < g.top() && y1 < g.top()) || (y0 > g.bottom() && y1 > g.bottom()))
            continue;                                     // arc fully off-screen
        bool forward = to > from;                        // backward = loop
        double frac = double(edges[i].first) / eMax;     // 0..1 strength
        double span = std::min(1.0, std::abs(y1 - y0) / std::max(1.0, visH)); // 0..1
        double bulge = rightSpace * (0.15 + 0.8 * span); // longer jumps arc wider
        QPointF c0(laneX, y0), c1(laneX, y1);
        QPointF ctrl(laneX + bulge, (y0 + y1) / 2);
        QPainterPath path(c0);
        path.quadTo(ctrl, c1);
        QColor col = forward ? QColor(90, 180, 255) : QColor(255, 170, 70); // cyan fwd, orange loop
        col.setAlpha(int(50 + 190 * frac));
        p.setPen(QPen(col, 1.0 + 3.0 * frac));
        p.drawPath(path);
    }

    // ---- Nodes + labels ----
    for (int i = 0; i < N; ++i) {
        uint16_t addr = nodes[i].first;
        double y = yOf(addr);
        if (y < g.top() - lineH || y > g.bottom() + lineH) continue; // off-screen
        double f = hotFrac(nodes[i].second);
        QColor dot(int(120 + 135 * f), int(70 + 90 * (1 - std::abs(f - 0.5) * 2)), int(60 * (1 - f)));
        double r = 2.5 + 3.5 * f;
        p.setPen(Qt::NoPen);
        p.setBrush(dot);
        p.drawEllipse(QPointF(laneX, y), r, r);
        if (showLabels) {
            bk::DisasmLine d = bk::disasm(mem, addr);
            char buf[8]; std::snprintf(buf, sizeof(buf), "%06o", addr);
            // small hotness bar behind the label
            p.fillRect(QRectF(g.left() + 4, y - fm.ascent() + 2,
                              labelW * f, fm.height() - 2), QColor(120, 60, 30, 140));
            p.setPen(QColor(200 + int(55 * f), 200, 180));
            p.drawText(QPointF(g.left() + 6, y + fm.ascent() / 2 - 1),
                       QString("%1 %2").arg(buf).arg(QString::fromStdString(d.text)));
        }
    }

    p.restore(); // end graph clip

    // ---- Right: ranked hottest instructions ----
    std::vector<std::pair<uint32_t, uint16_t>> hot;
    for (auto& n : nodes) hot.push_back({n.second, n.first});
    std::sort(hot.rbegin(), hot.rend());
    int listX = g.right() + margin;
    p.setPen(QColor(200, 220, 255));
    p.drawText(listX, headerY, "Горячие инструкции:");
    uint32_t top = hot.empty() ? 1 : hot[0].first;
    int y = headerY + lineH;
    // Keep the same `margin` gap from the right edge as the graph has on the left.
    const int listRight = width() - margin;
    const int barMax = std::max(36, (listRight - listX) * 22 / 100);
    const int textX = listX + barMax + 8;
    const int textW = std::max(10, listRight - textX);
    for (size_t i = 0; i < hot.size() && y < height() - 6; ++i) {
        uint16_t a = hot[i].second;
        double frac = double(hot[i].first) / top;
        p.fillRect(listX, y - fm.ascent(), int(frac * barMax), fm.height(),
                   QColor(int(120 + 135 * frac), int(60 + 60 * frac), 40));
        bk::DisasmLine d = bk::disasm(mem, a);
        char buf[8]; std::snprintf(buf, sizeof(buf), "%06o", a);
        p.setPen(QColor(230, 230, 200));
        QString lineText = QString("%1 %2  ×%3")
            .arg(buf).arg(QString::fromStdString(d.text)).arg(fmtCount(hot[i].first));
        p.drawText(textX, y, fm.elidedText(lineText, Qt::ElideRight, textW));
        y += lineH;
    }

    // ---- Totals footer + controls hint ----
    p.setPen(QColor(150, 170, 210));
    p.drawText(margin, height() - 6,
        QString("Адресов: %1  рёбер: %2  зум ×%3  [%4]  |  колесо/перетаск.-скролл, "
                "Ctrl+колесо или ± зум, Home-авто")
            .arg(N).arg(tr.edges().size()).arg(zoom_, 0, 'f', 1)
            .arg(autoFollow_ ? "авто → горячие" : "ручной"));
}
