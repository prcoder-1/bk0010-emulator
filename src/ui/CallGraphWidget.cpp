#include "CallGraphWidget.h"
#include "Board.h"
#include "Disasm.h"
#include <QPainter>
#include <QPainterPath>
#include <QWheelEvent>
#include <QMouseEvent>
#include <QKeyEvent>
#include <algorithm>
#include <cmath>
#include <map>
#include <set>

using bk::Board;

// JSR opcode range (octal) — a subroutine call.
static bool cgIsJsr(uint16_t ir) { return ir >= 0004000 && ir <= 0004777; }

// Format a large count with a K/M/G suffix, e.g. 1209 -> "1.2K".
static QString cgCount(uint64_t n) {
    struct { double div; const char* suf; } units[] = {{1e9, "G"}, {1e6, "M"}, {1e3, "K"}};
    for (auto& u : units)
        if (n >= u.div) {
            double v = n / u.div;
            return (v < 100.0) ? QString::number(v, 'f', 1) + u.suf
                               : QString::number(int(v + 0.5)) + u.suf;
        }
    return QString::number((qulonglong)n);
}

static QString oct6(uint16_t v) {
    return QString("%1").arg(v, 6, 8, QChar('0'));
}

// Box geometry in graph (pre-zoom) coordinates.
static constexpr double BOX_W = 150, BOX_H = 46, H_GAP = 34, V_GAP = 96;

CallGraphWidget::CallGraphWidget(Board* board, QWidget* parent)
    : QWidget(parent), board_(board) {
    setMinimumSize(320, 240);
    setFocusPolicy(Qt::StrongFocus);
    setMouseTracking(false);
}

void CallGraphWidget::refresh() {
    // Rebuild at most ~once per second (the layout is expensive and jitters if
    // reordered every frame); repaint every tick so panning stays smooth. The
    // GUI caller already gates this on isVisible(); headless callers drive it
    // directly.
    uint32_t now = board_->trace().now();
    if (nodes_.empty() || now - lastBuild_ >= 50) { rebuild(); lastBuild_ = now; }
    update();
}

// ---------------------------------------------------------------------------
// Build the function nodes, call edges and a layered (Sugiyama-style) layout
// from the execution trace.
// ---------------------------------------------------------------------------
void CallGraphWidget::rebuild() {
    bk::Trace& tr = board_->trace();
    tr.setEnabled(true);
    const bk::Memory& mem = board_->memory();
    bk::Cpu& cpu = board_->cpu();

    // 1. Function entries = JSR destinations. Add the lowest executed address as
    //    a synthetic "main" entry so pre-subroutine code has a home too.
    std::set<uint16_t> entrySet;
    for (auto& e : tr.edges())
        if (cgIsJsr(mem.peekWord(e.first >> 16))) entrySet.insert(e.first & 0xFFFF);

    int firstExec = -1;
    for (int a = 0; a < 0x10000; a += 2)
        if (tr.execCount((uint16_t)a)) { firstExec = a; break; }
    if (firstExec < 0) { nodes_.clear(); edges_.clear(); nodeIndex_.clear(); return; }
    if (entrySet.empty() || (uint16_t)firstExec < *entrySet.begin())
        entrySet.insert((uint16_t)firstExec);

    std::vector<uint16_t> entries(entrySet.begin(), entrySet.end()); // sorted
    auto funcOf = [&](uint16_t a) -> uint16_t {
        auto it = std::upper_bound(entries.begin(), entries.end(), a);
        return (it == entries.begin()) ? entries.front() : *(--it);
    };

    // 2. Cost per function = Σ execCount × per-instruction ticks over its range.
    std::unordered_map<uint16_t, uint64_t> cost;
    totalCost_ = 0;
    for (int a = 0; a < 0x10000; a += 2) {
        uint32_t c = tr.execCount((uint16_t)a);
        if (!c) continue;
        uint64_t tk = (uint64_t)c * cpu.instrTicks(mem.peekWord((uint16_t)a));
        cost[funcOf((uint16_t)a)] += tk;
        totalCost_ += tk;
    }
    if (totalCost_ == 0) totalCost_ = 1;

    // 3. Call edges: caller-function → callee-entry, aggregated; plus per-callee
    //    incoming call totals.
    std::map<std::pair<uint16_t, uint16_t>, uint64_t> edgeW;
    std::unordered_map<uint16_t, uint64_t> incoming;
    for (auto& e : tr.edges()) {
        uint16_t from = e.first >> 16, to = e.first & 0xFFFF;
        if (!cgIsJsr(mem.peekWord(from))) continue;
        uint16_t cf = funcOf(from);
        incoming[to] += e.second;
        if (cf != to) edgeW[{cf, to}] += e.second;
    }

    // 4. Rank functions by cost, keep the top-N, build nodes.
    std::vector<std::pair<uint64_t, uint16_t>> ranked;
    ranked.reserve(cost.size());
    for (auto& kv : cost) ranked.push_back({kv.second, kv.first});
    std::sort(ranked.begin(), ranked.end(), std::greater<>());
    if ((int)ranked.size() > topN_) ranked.resize(topN_);

    nodes_.clear();
    nodeIndex_.clear();
    maxCost_ = 1;
    for (auto& r : ranked) {
        Node n{};
        n.entry = r.second;
        n.cost  = r.first;
        n.calls = (uint32_t)incoming[r.second];
        n.level = 0;
        nodeIndex_[n.entry] = (int)nodes_.size();
        nodes_.push_back(n);
        maxCost_ = std::max(maxCost_, n.cost);
    }

    // Keep only edges between surviving nodes.
    edges_.clear();
    maxWeight_ = 1;
    for (auto& kv : edgeW) {
        if (!nodeIndex_.count(kv.first.first) || !nodeIndex_.count(kv.first.second)) continue;
        edges_.push_back({kv.first.first, kv.first.second, kv.second});
        maxWeight_ = std::max(maxWeight_, kv.second);
    }

    const int N = (int)nodes_.size();
    if (N == 0) return;

    // 5. Assign levels: longest path from roots (relaxation, capped for cycles).
    for (int iter = 0; iter <= N; ++iter) {
        bool changed = false;
        for (auto& e : edges_) {
            int ci = nodeIndex_[e.from], ti = nodeIndex_[e.to];
            int want = nodes_[ci].level + 1;
            if (want > nodes_[ti].level && want <= N) { nodes_[ti].level = want; changed = true; }
        }
        if (!changed) break;
    }
    int maxLevel = 0;
    for (auto& n : nodes_) maxLevel = std::max(maxLevel, n.level);

    // Adjacency (parents/children) for barycenter ordering.
    std::vector<std::vector<int>> parents(N), children(N);
    for (auto& e : edges_) {
        int ci = nodeIndex_[e.from], ti = nodeIndex_[e.to];
        children[ci].push_back(ti);
        parents[ti].push_back(ci);
    }

    // 6. Order nodes within each level (start by cost, then barycenter passes to
    //    pull children under their parents) and assign coordinates.
    std::vector<std::vector<int>> byLevel(maxLevel + 1);
    for (int i = 0; i < N; ++i) byLevel[nodes_[i].level].push_back(i);
    for (auto& lv : byLevel)
        std::sort(lv.begin(), lv.end(), [&](int a, int b) { return nodes_[a].cost > nodes_[b].cost; });

    auto assignX = [&]() {
        for (int L = 0; L <= maxLevel; ++L) {
            int n = (int)byLevel[L].size();
            double total = n * BOX_W + (n - 1) * H_GAP;
            double x0 = -total / 2 + BOX_W / 2;
            for (int i = 0; i < n; ++i) {
                nodes_[byLevel[L][i]].x = x0 + i * (BOX_W + H_GAP);
                nodes_[byLevel[L][i]].y = L * V_GAP;
            }
        }
    };
    assignX();
    auto meanX = [&](const std::vector<int>& adj, double cur) {
        if (adj.empty()) return cur;
        double s = 0; for (int k : adj) s += nodes_[k].x; return s / adj.size();
    };
    for (int pass = 0; pass < 4; ++pass) {
        for (int L = 1; L <= maxLevel; ++L)
            std::stable_sort(byLevel[L].begin(), byLevel[L].end(),
                             [&](int a, int b) { return meanX(parents[a], nodes_[a].x) < meanX(parents[b], nodes_[b].x); });
        assignX();
        for (int L = maxLevel - 1; L >= 0; --L)
            std::stable_sort(byLevel[L].begin(), byLevel[L].end(),
                             [&](int a, int b) { return meanX(children[a], nodes_[a].x) < meanX(children[b], nodes_[b].x); });
        assignX();
    }
    for (auto& n : nodes_) n.box = QRectF(n.x - BOX_W / 2, n.y, BOX_W, BOX_H);
}

// Cold→hot heat colour for a cost fraction (0..1): blue → cyan → yellow → red.
static QColor heatColor(double frac) {
    frac = std::clamp(frac, 0.0, 1.0);
    double hue = (1.0 - frac) * 0.62;          // 0.62≈blue → 0=red
    return QColor::fromHsvF(hue, 0.72, 0.45 + 0.5 * frac);
}

// Choose zoom/pan so the whole graph fits centred in the widget.
void CallGraphWidget::fitToView() {
    if (nodes_.empty()) return;
    double minX = 1e9, minY = 1e9, maxX = -1e9, maxY = -1e9;
    for (auto& n : nodes_) {
        minX = std::min(minX, n.box.left());   maxX = std::max(maxX, n.box.right());
        minY = std::min(minY, n.box.top());    maxY = std::max(maxY, n.box.bottom());
    }
    double bw = std::max(maxX - minX, 1.0), bh = std::max(maxY - minY, 1.0);
    double z = std::min((width() - 48) / bw, (height() - 84) / bh);
    zoom_ = std::clamp(z, 0.15, 2.0);
    double cx = (minX + maxX) / 2, cy = (minY + maxY) / 2;
    pan_ = QPointF(-zoom_ * cx, height() / 2.0 - 40 - zoom_ * cy);
}

QTransform CallGraphWidget::viewTransform() const {
    // Centre the graph horizontally, start a little below the top, then apply the
    // user's pan/zoom.
    QTransform t;
    t.translate(width() / 2.0 + pan_.x(), 40 + pan_.y());
    t.scale(zoom_, zoom_);
    return t;
}

void CallGraphWidget::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.fillRect(rect(), QColor(16, 18, 26));
    p.setRenderHint(QPainter::Antialiasing, true);

    QFont mono("monospace"); mono.setStyleHint(QFont::TypeWriter);
    mono.setPixelSize(11);

    // Header / legend (screen space).
    p.setFont(mono);
    p.setPen(QColor(200, 210, 255));
    p.drawText(10, 18, QString::fromUtf8(
        "Граф вызовов  —  цвет = доля времени CPU (синий→красный),  толщина стрелки = частота вызовов"));
    if (nodes_.empty()) {
        p.setPen(QColor(140, 150, 170));
        p.drawText(10, 40, QString::fromUtf8("нет данных трассировки — запустите игру"));
        return;
    }

    if (fitView_) fitToView();
    const QTransform vt = viewTransform();

    // ---- Edges (draw first, under the boxes) ----
    const double logMaxW = std::log(double(maxWeight_) + 1.0);
    for (auto& e : edges_) {
        const Node& a = nodes_[nodeIndex_[e.from]];
        const Node& b = nodes_[nodeIndex_[e.to]];
        bool back = b.level <= a.level;            // cycle / recursion / self up-call

        // Endpoints and the last Bézier control point (used for the arrowhead
        // direction). Forward edges drop from the caller's bottom to the callee's
        // top; back edges bow out to the right and re-enter from the side.
        QPointF p1, p2, c1, c2;
        if (back) {
            p1 = vt.map(QPointF(a.box.right(), a.y + BOX_H / 2));
            p2 = vt.map(QPointF(b.box.right(), b.y + BOX_H / 2));
            double t0 = std::log(double(e.weight) + 1.0) / (logMaxW + 1e-9);
            double bow = (60 + 40 * t0) * zoom_;
            c1 = p1 + QPointF(bow, 0);
            c2 = p2 + QPointF(bow, 0);
        } else {
            p1 = vt.map(QPointF(a.x, a.box.bottom()));
            p2 = vt.map(QPointF(b.x, b.box.top()));
            double dy = p2.y() - p1.y();
            c1 = p1 + QPointF(0, dy * 0.4);
            c2 = p2 - QPointF(0, dy * 0.4);
        }
        double t = std::log(double(e.weight) + 1.0) / (logMaxW + 1e-9);   // 0..1
        double w = (1.0 + 4.5 * t) * zoom_;
        QColor ec = back ? QColor(230, 150, 60) : QColor(90, 170, 235);
        ec.setAlpha(150 + int(90 * t));

        QPainterPath path(p1);
        path.cubicTo(c1, c2, p2);
        p.setPen(QPen(ec, std::max(1.0, w), Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        p.setBrush(Qt::NoBrush);
        p.drawPath(path);

        // Arrowhead at the callee end, aimed along the tangent (c2 → p2).
        double ang = std::atan2(p2.y() - c2.y(), p2.x() - c2.x());
        double ah = (8 + 4 * t) * zoom_;
        QPointF a1 = p2 - QPointF(std::cos(ang - 0.5) * ah, std::sin(ang - 0.5) * ah);
        QPointF a2 = p2 - QPointF(std::cos(ang + 0.5) * ah, std::sin(ang + 0.5) * ah);
        QColor head = ec; head.setAlpha(235);
        p.setPen(Qt::NoPen); p.setBrush(head);
        p.drawPolygon(QPolygonF({p2, a1, a2}));
    }

    // ---- Nodes ----
    p.setBrush(Qt::NoBrush);
    for (auto& n : nodes_) {
        QRectF r = vt.mapRect(n.box);
        double frac = double(n.cost) / double(maxCost_);
        double pct  = 100.0 * double(n.cost) / double(totalCost_);
        QColor fill = heatColor(frac);
        p.setPen(QPen(fill.lighter(160), 1.4));
        p.setBrush(fill);
        p.drawRoundedRect(r, 5, 5);

        if (r.height() < 16) continue;   // too small to label
        QFont nf = mono; nf.setPixelSize(std::clamp(int(11 * zoom_), 7, 15));
        p.setFont(nf);
        QFontMetrics fm(nf);
        QString l1 = oct6(n.entry);
        QString l2 = QString("%1%  ×%2").arg(pct, 0, 'f', pct >= 10 ? 0 : 1).arg(cgCount(n.calls));
        p.setPen(QColor(240, 244, 255));
        p.drawText(r.adjusted(4, 2, -4, -2), Qt::AlignHCenter | Qt::AlignTop, l1);
        p.setPen(QColor(210, 220, 240));
        p.drawText(r.adjusted(4, fm.height() + 1, -4, -2), Qt::AlignHCenter | Qt::AlignTop, l2);
    }

    // Footer hint (screen space).
    p.setFont(mono);
    p.setPen(QColor(120, 130, 150));
    p.drawText(10, height() - 8, QString::fromUtf8(
        "тащить — двигать · колесо — масштаб · клик — перейти в дизассемблер · +/- топ-N · 0 — сброс вида"));
}

void CallGraphWidget::wheelEvent(QWheelEvent* e) {
    fitView_ = false;
    double f = (e->angleDelta().y() > 0) ? 1.15 : 1.0 / 1.15;
    double nz = std::clamp(zoom_ * f, 0.15, 4.0);
    // Zoom around the cursor.
    QPointF c = e->position();
    QTransform vt = viewTransform();
    QPointF before = vt.inverted().map(c);
    zoom_ = nz;
    QTransform vt2 = viewTransform();
    QPointF after = vt2.map(before);
    pan_ += c - after;
    update();
}

void CallGraphWidget::mousePressEvent(QMouseEvent* e) {
    if (e->button() == Qt::LeftButton) {
        dragging_ = true; pressPos_ = lastDrag_ = e->pos();
    }
}

void CallGraphWidget::mouseMoveEvent(QMouseEvent* e) {
    if (dragging_) {
        fitView_ = false;
        pan_ += e->pos() - lastDrag_;
        lastDrag_ = e->pos();
        update();
    }
}

void CallGraphWidget::mouseReleaseEvent(QMouseEvent* e) {
    if (e->button() != Qt::LeftButton) return;
    dragging_ = false;
    if ((e->pos() - pressPos_).manhattanLength() > 4) return;   // was a drag
    // Click: hit-test a box and jump the disassembler there.
    QPointF g = viewTransform().inverted().map(QPointF(e->pos()));
    for (auto& n : nodes_)
        if (n.box.contains(g)) { emit addressPicked(n.entry); return; }
}

void CallGraphWidget::keyPressEvent(QKeyEvent* e) {
    switch (e->key()) {
    case Qt::Key_Plus: case Qt::Key_Equal:
        topN_ = std::min(topN_ + 10, 120); rebuild(); update(); break;
    case Qt::Key_Minus: case Qt::Key_Underscore:
        topN_ = std::max(topN_ - 10, 10); rebuild(); update(); break;
    case Qt::Key_0:
        fitView_ = true; update(); break;
    default: QWidget::keyPressEvent(e);
    }
}
