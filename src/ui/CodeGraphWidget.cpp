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

// A control-transfer instruction ends a basic block: the next sequential address
// starts a new block. Covers PDP-11/ВМ1 branches, JMP, JSR, RTS, RTI/RTT, SOB,
// EMT/TRAP and HALT (opcode ranges, octal).
static bool isBlockTerminator(uint16_t ir) {
    if (ir >= 0000400 && ir <= 0003477) return true; // BR..BLE (conditional branches)
    if (ir >= 0100000 && ir <= 0103777) return true; // BPL..BCS
    if (ir >= 0000100 && ir <= 0000177) return true; // JMP
    if (ir >= 0004000 && ir <= 0004777) return true; // JSR
    if (ir >= 0000200 && ir <= 0000207) return true; // RTS
    if (ir == 0000002 || ir == 0000006) return true; // RTI / RTT
    if (ir >= 0104000 && ir <= 0104777) return true; // EMT / TRAP
    if (ir >= 0077000 && ir <= 0077777) return true; // SOB
    if (ir == 0000000) return true;                  // HALT
    return false;
}

// One drawn row of the graph: either a collapsed basic block, or a single
// instruction (a standalone one, or a line of an expanded block).
struct GraphRow {
    uint16_t addr;    // leader address for a block, else the instruction address
    uint32_t count;   // aggregate (block) or per-instruction execution count
    bool     block;   // true = collapsed block row
    int      nInstr;  // block size (for a block row, or an expanded header); else 0
    uint16_t leader;  // owning block leader
};

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
    setContextMenuPolicy(Qt::PreventContextMenu); // right-click is "hide instruction"
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
    if (e->button() == Qt::LeftButton) {
        pressPos_ = e->pos();
        if (graphRect_.contains(e->pos())) {   // may become a drag
            dragging_ = true;
            lastDrag_ = e->pos();
        }
    } else if (e->button() == Qt::RightButton) {
        // Right-click hides a hot instruction (e.g. a busy delay loop) from both
        // the graph and the list. Reset with the Delete key (see keyPressEvent).
        uint16_t addr;
        if (addrAtPos(e->pos(), addr)) { excluded_.insert(addr); update(); }
    }
}

bool CodeGraphWidget::addrAtPos(const QPoint& pos, uint16_t& out) const {
    // 1) A row in the hot-instruction list.
    for (const auto& row : hotRows_)
        if (row.first.contains(pos)) { out = row.second; return true; }
    // 2) A node in the graph: the one whose y is nearest the click.
    if (graphRect_.contains(pos) && !nodeYs_.empty()) {
        int cy = pos.y(), bestD = 1 << 30; uint16_t bestA = 0; bool found = false;
        for (const auto& nd : nodeYs_) {
            int d = std::abs(nd.first - cy);
            if (d < bestD) { bestD = d; bestA = nd.second; found = true; }
        }
        if (found) { out = bestA; return true; }
    }
    return false;
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

void CodeGraphWidget::mouseReleaseEvent(QMouseEvent* e) {
    bool wasDragging = dragging_;
    dragging_ = false;
    if (e->button() != Qt::LeftButton) return;
    // A press+release without real movement is a click, not a drag.
    if (wasDragging && (e->pos() - pressPos_).manhattanLength() > 4) return;

    auto navigate = [&](uint16_t addr) {
        scrollToAddr_ = addr;      // scroll this graph to it (visual feedback)
        userInteracted();          // manual mode so it stays put
        emit addressPicked(addr);  // drive the debugger's disassembler
        update();
    };
    // Hot-list row → navigate the disassembler.
    for (const auto& r : hotRows_)
        if (r.first.contains(e->pos())) { navigate(r.second); return; }
    // Graph node → toggle a collapsible block; otherwise navigate.
    if (graphRect_.contains(e->pos()) && !nodeYs_.empty()) {
        int cy = e->pos().y(), bestD = 1 << 30; uint16_t addr = 0; bool found = false;
        for (const auto& nd : nodeYs_) {
            int d = std::abs(nd.first - cy);
            if (d < bestD) { bestD = d; addr = nd.second; found = true; }
        }
        if (!found) return;
        auto it = blockSize_.find(addr);
        if (blockLeaders_.count(addr) && it != blockSize_.end() && it->second > 1) {
            if (expanded_.count(addr)) expanded_.erase(addr);            // collapse
            else { expanded_.insert(addr); scrollToAddr_ = addr; emit addressPicked(addr); }
            userInteracted();
            update();
        } else {
            navigate(addr);
        }
    }
}

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
    // Delete/Backspace restores right-click-excluded instructions.
    case Qt::Key_Delete: case Qt::Key_Backspace:
        if (!excluded_.empty()) { excluded_.clear(); update(); }
        break;
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
    // Raw executed instructions in address order (also feeds the hot list).
    std::vector<std::pair<uint16_t, uint32_t>> raw; // (addr, count)
    for (int a = 0; a < 0x10000; a += 2) {
        uint16_t a16 = static_cast<uint16_t>(a);
        if (excluded_.count(a16)) continue;   // user-hidden (right-click)
        uint32_t c = tr.execCount(a16);
        if (c && tr.fade(tr.lastExec(a16), GRAPH_WINDOW) > 0.0)
            raw.push_back({a16, c});
    }

    // ---- Group instructions into basic blocks ----
    // Leaders: the first instruction, any branch/jump target (edge destination),
    // any instruction after a control-transfer, and any address after a gap.
    std::set<uint16_t> leaders;
    for (auto& e : tr.edges()) leaders.insert(static_cast<uint16_t>(e.first & 0xFFFF));
    for (size_t i = 0; i < raw.size(); ++i) {
        if (i == 0) { leaders.insert(raw[0].first); continue; }
        uint16_t prev = raw[i - 1].first;
        uint16_t prevEnd = prev + static_cast<uint16_t>(bk::disasm(mem, prev).words * 2);
        if (prevEnd != raw[i].first || isBlockTerminator(mem.peekWord(prev)))
            leaders.insert(raw[i].first);
    }

    // Build the display rows (a collapsed block, or one row per instruction when
    // expanded) and the address -> row-index map used to route the edges.
    std::vector<GraphRow> nodes;
    std::unordered_map<uint16_t, int> indexOf;
    blockLeaders_.clear();
    blockSize_.clear();
    uint32_t localMax = 1;        // max count among the visible rows
    for (size_t i = 0; i < raw.size();) {
        uint16_t leader = raw[i].first;
        size_t j = i + 1;
        while (j < raw.size() && !leaders.count(raw[j].first)) ++j;
        int nInstr = static_cast<int>(j - i);
        uint32_t agg = 0;
        for (size_t k = i; k < j; ++k) agg = std::max(agg, raw[k].second);
        blockLeaders_.insert(leader);
        blockSize_[leader] = nInstr;
        bool open = (nInstr > 1) && expanded_.count(leader);
        if (!open) {                                   // collapsed: one row
            int idx = (int)nodes.size();
            nodes.push_back({leader, agg, true, nInstr, leader});
            for (size_t k = i; k < j; ++k) indexOf[raw[k].first] = idx;
            if (agg > localMax) localMax = agg;
        } else {                                       // expanded: a row per line
            for (size_t k = i; k < j; ++k) {
                int idx = (int)nodes.size();
                nodes.push_back({raw[k].first, raw[k].second, false,
                                 (k == i ? nInstr : 0), leader});
                indexOf[raw[k].first] = idx;
                if (raw[k].second > localMax) localMax = raw[k].second;
            }
        }
        i = j;
    }
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
            if (nodes[i].count > hotMax) { hotMax = nodes[i].count; hotIdx = i; }
        // Zoom so node spacing reaches label height (labels become readable).
        double targetZoom = std::min(60.0, std::max(1.0, N * (fm.height() + 2.0) / std::max(1.0, visH)));
        zoom_ += (targetZoom - zoom_) * 0.08;
        double tContent = visH * zoom_;
        double targetPanY = visH * 0.5 - 6.0 - (hotIdx + 0.5) / N * tContent;
        panY_ += (targetPanY - panY_) * 0.08;
    }

    contentH_ = visH * zoom_;
    // Honour a pending "scroll to hot address": zoom in enough that the label is
    // legible, then pan so that instruction sits near the top of the graph.
    if (scrollToAddr_ >= 0 && N > 0) {
        auto it = indexOf.find(static_cast<uint16_t>(scrollToAddr_));
        if (it != indexOf.end()) {
            double targetZoom = std::min(60.0, std::max(1.0, N * (fm.height() + 2.0) / std::max(1.0, visH)));
            zoom_ = std::max(zoom_, targetZoom);
            contentH_ = visH * zoom_;
            panY_ = visH * 0.15 - (it->second + 0.5) / N * contentH_;
        }
        scrollToAddr_ = -1;
    }
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
    nodeYs_.clear();
    for (int i = 0; i < N; ++i) {
        const GraphRow& row = nodes[i];
        double y = yOf(row.addr);
        if (y < g.top() - lineH || y > g.bottom() + lineH) continue; // off-screen
        nodeYs_.push_back({int(y), row.addr}); // for click hit-testing
        double f = hotFrac(row.count);
        QColor dot(int(120 + 135 * f), int(70 + 90 * (1 - std::abs(f - 0.5) * 2)), int(60 * (1 - f)));
        // Collapsed multi-instruction blocks get a square glyph; single lines a dot.
        bool expandable = row.block && row.nInstr > 1;
        bool header     = !row.block && row.nInstr > 1; // first line of an open block
        p.setPen(Qt::NoPen);
        p.setBrush(dot);
        if (expandable) { double r = 3.0 + 3.0 * f; p.drawRect(QRectF(laneX - r, y - r, 2 * r, 2 * r)); }
        else            { double r = 2.5 + 3.5 * f; p.drawEllipse(QPointF(laneX, y), r, r); }
        if (showLabels) {
            bk::DisasmLine d = bk::disasm(mem, row.addr);
            char buf[8]; std::snprintf(buf, sizeof(buf), "%06o", row.addr);
            // small hotness bar behind the label
            p.fillRect(QRectF(g.left() + 4, y - fm.ascent() + 2,
                              labelW * f, fm.height() - 2), QColor(120, 60, 30, 140));
            p.setPen(QColor(200 + int(55 * f), 200, 180));
            QString text;
            if (expandable)                                 // "▶ addr insn  [N]"
                text = QString("▶ %1 %2  [%3]").arg(buf)
                           .arg(QString::fromStdString(d.text)).arg(row.nInstr);
            else if (header)                                // "▼ addr insn"
                text = QString("▼ %1 %2").arg(buf).arg(QString::fromStdString(d.text));
            else                                            // indented instruction
                text = QString("%1%2 %3").arg(row.leader != row.addr ? "  " : "")
                           .arg(buf).arg(QString::fromStdString(d.text));
            p.drawText(QPointF(g.left() + 6, y + fm.ascent() / 2 - 1), text);
        }
    }

    p.restore(); // end graph clip

    // ---- Right: ranked hottest instructions (individual, not blocks) ----
    std::vector<std::pair<uint32_t, uint16_t>> hot;
    for (auto& n : raw) hot.push_back({n.second, n.first});
    std::sort(hot.rbegin(), hot.rend());
    int listX = g.right() + margin;
    p.setPen(QColor(200, 220, 255));
    p.drawText(listX, headerY, excluded_.empty()
        ? QString("Горячие инструкции:")
        : QString("Горячие инструкции:  (скрыто %1)").arg(excluded_.size()));
    uint32_t top = hot.empty() ? 1 : hot[0].first;
    int y = headerY + lineH;
    // Keep the same `margin` gap from the right edge as the graph has on the left.
    const int listRight = width() - margin;
    const int barMax = std::max(36, (listRight - listX) * 22 / 100);
    const int textX = listX + barMax + 8;
    const int textW = std::max(10, listRight - textX);
    hotRows_.clear();
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
        // Record the clickable row so a click scrolls the graph to this address.
        hotRows_.push_back({QRect(listX, y - fm.ascent(), listRight - listX, lineH), a});
        y += lineH;
    }

    // ---- Totals footer + controls hint ----
    p.setPen(QColor(150, 170, 210));
    p.drawText(margin, height() - 6,
        QString("Блоков: %1  рёбер: %2  зум ×%3  [%4]  |  ЛКМ-блок ▶/▼·дизасм, "
                "ПКМ-скрыть, Del-вернуть, колесо-скролл, Ctrl+колесо зум, Home-авто")
            .arg((int)blockLeaders_.size()).arg(tr.edges().size()).arg(zoom_, 0, 'f', 1)
            .arg(autoFollow_ ? "авто → горячие" : "ручной"));
}
