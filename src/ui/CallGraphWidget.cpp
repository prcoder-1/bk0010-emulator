#include "CallGraphWidget.h"
#include "Board.h"
#include "Disasm.h"
#include <QPainter>
#include <QPainterPath>
#include <QWheelEvent>
#include <QMouseEvent>
#include <QKeyEvent>
#include <QEvent>
#include <algorithm>
#include <cmath>
#include <functional>
#include <map>
#include <set>

using bk::Board;

static QColor heatColor(double frac);        // cold→hot cost colour (defined below)
static QColor dim(QColor c, double k);       // scale brightness (defined below)

// A function darkens to its dimmest over this many frames (~50 Hz) without
// executing; once fully dark it can be purged with Delete/Backspace.
static constexpr int ACTIVE_WINDOW = 350;

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
    setMouseTracking(true);   // hover → linked highlighting across profiler windows
}

void CallGraphWidget::refresh() {
    // Rebuild at most ~once per second (the layout is expensive and jitters if
    // reordered every frame). The GUI caller already gates this on isVisible();
    // headless callers drive it directly.
    uint32_t now = board_->trace().now();
    // Don't reorder nodes_ while the user is dragging a box (indices must stay put).
    bool rebuilt = false;
    if (!draggingNode_ && (nodes_.empty() || now - lastBuild_ >= 50)) { rebuild(); lastBuild_ = now; rebuilt = true; }
    // Repaint immediately on rebuild; otherwise the layout is static and only the
    // node/edge colours fade, so throttle that animation to ~16 Hz instead of the
    // full 50 Hz — the emulation shares this GUI thread. Interactive events
    // (drag/pan/zoom/hover) repaint directly and bypass the throttle.
    if (rebuilt || ++refreshTick_ >= 3) { refreshTick_ = 0; update(); }
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

    // 2. Cost per function = Σ execCount × per-instruction ticks over its range;
    //    also track each function's most recent execution time (for the fade-out).
    std::unordered_map<uint16_t, uint64_t> cost;
    std::unordered_map<uint16_t, uint32_t> lastAct;
    totalCost_ = 0;
    for (int a = 0; a < 0x10000; a += 2) {
        uint32_t c = tr.execCount((uint16_t)a);
        if (!c) continue;
        uint16_t f = funcOf((uint16_t)a);
        cost[f] += (uint64_t)c * cpu.instrTicks(mem.peekWord((uint16_t)a));
        totalCost_ += (uint64_t)c * cpu.instrTicks(mem.peekWord((uint16_t)a));
        uint32_t le = tr.lastExec((uint16_t)a);
        if (le > lastAct[f]) lastAct[f] = le;
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

    // 4. Rank functions by cost, keep the top-N, build nodes. Idle functions are
    //    kept (they darken via lastActive), except those the user purged with
    //    Delete/Backspace — but a purged function re-appears once it runs again.
    for (auto it = hidden_.begin(); it != hidden_.end(); ) {
        auto c = lastAct.find(*it);
        if (c != lastAct.end() && tr.fade(c->second, ACTIVE_WINDOW) > 0.0) it = hidden_.erase(it);
        else ++it;
    }
    std::vector<std::pair<uint64_t, uint16_t>> ranked;
    ranked.reserve(cost.size());
    for (auto& kv : cost)
        if (!hidden_.count(kv.first)) ranked.push_back({kv.second, kv.first});
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
        n.lastActive = lastAct[r.second];
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

    // 5. Assign levels by longest path over the DAG obtained by dropping the edges
    //    that close a cycle. A plain longest-path relaxation would follow cycles
    //    (recursion / mutual calls) and inflate the layout all the way to N levels,
    //    stretching it vertically with huge inter-level gaps. So: DFS to get a
    //    topological order, then relax only forward edges (topoIdx[to] > topoIdx[from]).
    std::vector<std::vector<int>> adj(N);
    for (auto& e : edges_) adj[nodeIndex_[e.from]].push_back(nodeIndex_[e.to]);
    std::vector<char> state(N, 0);       // 0=new, 1=on-stack, 2=done
    std::vector<int> childIdx(N, 0), topo, stk;
    for (int s = 0; s < N; ++s) {
        if (state[s]) continue;
        stk.push_back(s); state[s] = 1;
        while (!stk.empty()) {
            int u = stk.back();
            if (childIdx[u] < (int)adj[u].size()) {
                int v = adj[u][childIdx[u]++];
                if (state[v] == 0) { state[v] = 1; stk.push_back(v); }
            } else { state[u] = 2; topo.push_back(u); stk.pop_back(); }
        }
    }
    std::reverse(topo.begin(), topo.end());
    std::vector<int> topoIdx(N);
    for (int i = 0; i < N; ++i) topoIdx[topo[i]] = i;
    for (auto& n : nodes_) n.level = 0;
    for (int u : topo)
        for (int v : adj[u])
            if (topoIdx[v] > topoIdx[u] && nodes_[v].level < nodes_[u].level + 1)
                nodes_[v].level = nodes_[u].level + 1;

    int maxLevel = 0;
    for (auto& n : nodes_) maxLevel = std::max(maxLevel, n.level);

    // 6. Layered layout with a dummy node on every level a long (multi-level)
    //    forward edge crosses, so such edges are routed through channels between
    //    boxes instead of passing under them.
    struct LP { double x = 0; int level = 0; int real = -1; };  // real≥0 → nodes_ index
    std::vector<LP> lp(N);
    for (int i = 0; i < N; ++i) lp[i] = { 0.0, nodes_[i].level, i };

    std::vector<std::vector<int>> path(edges_.size());   // lp-index chain per edge
    for (size_t ei = 0; ei < edges_.size(); ++ei) {
        int u = nodeIndex_[edges_[ei].from], v = nodeIndex_[edges_[ei].to];
        int lu = nodes_[u].level, lv = nodes_[v].level;
        path[ei].push_back(u);
        for (int L = lu + 1; L < lv; ++L) {              // forward, multi-level only
            int d = (int)lp.size(); lp.push_back({ 0.0, L, -1 });
            path[ei].push_back(d);
        }
        path[ei].push_back(v);
    }
    const int T = (int)lp.size();

    // Up/down adjacency over adjacent-level links (for barycenter ordering).
    std::vector<std::vector<int>> up(T), down(T);
    for (auto& pth : path)
        for (size_t k = 0; k + 1 < pth.size(); ++k) {
            int aa = pth[k], bb = pth[k + 1];
            if (lp[aa].level + 1 == lp[bb].level) { down[aa].push_back(bb); up[bb].push_back(aa); }
        }

    // Order each layer: real nodes first by cost, then barycenter passes.
    std::vector<std::vector<int>> layer(maxLevel + 1);
    for (int i = 0; i < T; ++i) layer[lp[i].level].push_back(i);
    auto costOf = [&](int i) { return lp[i].real >= 0 ? (double)nodes_[lp[i].real].cost : -1.0; };
    for (auto& lyr : layer)
        std::sort(lyr.begin(), lyr.end(), [&](int a, int b) { return costOf(a) > costOf(b); });

    const double DUMMY_W = 18.0;
    auto widthOf = [&](int i) { return lp[i].real >= 0 ? BOX_W : DUMMY_W; };
    auto assignX = [&]() {
        for (int L = 0; L <= maxLevel; ++L) {
            auto& lyr = layer[L];
            double total = H_GAP * std::max(0, (int)lyr.size() - 1);
            for (int i : lyr) total += widthOf(i);
            double x = -total / 2;
            for (int i : lyr) { double w = widthOf(i); lp[i].x = x + w / 2; x += w + H_GAP; }
        }
    };
    assignX();
    auto meanX = [&](const std::vector<int>& adj, double cur) {
        if (adj.empty()) return cur;
        double s = 0; for (int k : adj) s += lp[k].x; return s / adj.size();
    };
    for (int pass = 0; pass < 6; ++pass) {
        for (int L = 1; L <= maxLevel; ++L)
            std::stable_sort(layer[L].begin(), layer[L].end(),
                             [&](int a, int b) { return meanX(up[a], lp[a].x) < meanX(up[b], lp[b].x); });
        assignX();
        for (int L = maxLevel - 1; L >= 0; --L)
            std::stable_sort(layer[L].begin(), layer[L].end(),
                             [&](int a, int b) { return meanX(down[a], lp[a].x) < meanX(down[b], lp[b].x); });
        assignX();
    }

    // Write back real-node coordinates and per-edge routing bends.
    for (int i = 0; i < N; ++i) { nodes_[i].x = lp[i].x; nodes_[i].y = nodes_[i].level * V_GAP; }
    for (size_t ei = 0; ei < edges_.size(); ++ei) {
        edges_[ei].bends.clear();
        for (size_t k = 1; k + 1 < path[ei].size(); ++k) {
            int d = path[ei][k];
            edges_[ei].bends.push_back(QPointF(lp[d].x, lp[d].level * V_GAP + BOX_H / 2));
        }
    }

    // Apply the user's manual placements, then cache box rectangles.
    for (auto& n : nodes_) {
        auto it = manualPos_.find(n.entry);
        if (it != manualPos_.end()) { n.x = it->second.x(); n.y = it->second.y(); }
        n.box = QRectF(n.x - BOX_W / 2, n.y, BOX_W, BOX_H);
    }

    buildTree();
}

// ---------------------------------------------------------------------------
// Treemap ("callee map"): a spanning call tree, each box's area proportional to
// its subtree cost, callees nested inside callers via a squarified layout.
// ---------------------------------------------------------------------------
void CallGraphWidget::buildTree() {
    tree_.clear();
    const int N = (int)nodes_.size();
    if (!N) return;

    // Outgoing calls per node (sorted heaviest-first) and in-degrees.
    std::vector<std::vector<std::pair<uint64_t, int>>> out(N);
    std::vector<int> indeg(N, 0);
    for (auto& e : edges_) {
        int ci = nodeIndex_[e.from], ti = nodeIndex_[e.to];
        out[ci].push_back({e.weight, ti});
        ++indeg[ti];
    }
    for (auto& v : out) std::sort(v.rbegin(), v.rend());

    // Forest roots: functions nobody calls (top of the tree), hottest first. If
    // everything is in a cycle, seed with the costliest node.
    std::vector<int> roots;
    for (int i = 0; i < N; ++i) if (indeg[i] == 0) roots.push_back(i);
    if (roots.empty()) {
        int h = 0; for (int i = 1; i < N; ++i) if (nodes_[i].cost > nodes_[h].cost) h = i;
        roots.push_back(h);
    }
    std::sort(roots.begin(), roots.end(), [&](int a, int b) { return nodes_[a].cost > nodes_[b].cost; });

    tree_.push_back({0xFFFF, 0, 0, 0, {}, QRectF()});   // virtual root
    std::vector<char> visited(N, 0);

    std::function<int(int, int)> dfs = [&](int ni, int depth) -> int {
        visited[ni] = 1;
        int ti = (int)tree_.size();
        tree_.push_back({nodes_[ni].entry, nodes_[ni].cost, nodes_[ni].cost, depth, {}, QRectF()});
        if (depth < 9) {
            for (auto& pr : out[ni]) {
                int callee = pr.second;
                if (visited[callee]) continue;          // each function once (breaks cycles)
                int ci = dfs(callee, depth + 1);
                tree_[ti].kids.push_back(ci);
                tree_[ti].subtree += tree_[ci].subtree;
            }
        }
        return ti;
    };
    auto addTop = [&](int ni) {
        int ci = dfs(ni, 1);
        tree_[0].kids.push_back(ci);
        tree_[0].subtree += tree_[ci].subtree;
    };
    for (int r : roots)         if (!visited[r]) addTop(r);
    for (int i = 0; i < N; ++i) if (!visited[i]) addTop(i);   // unreachable → own top-level trees
}

// Squarified treemap: fill `r` with rectangles whose areas are given (descending
// order recommended); `out[i]` receives the rectangle for `area[i]`.
static void squarify(const std::vector<double>& area, QRectF r, std::vector<QRectF>& out) {
    const int n = (int)area.size();
    out.assign(n, QRectF());
    int i = 0;
    while (i < n) {
        double side = std::min(r.width(), r.height());
        if (side <= 0) break;
        double sum = 0, mn = 1e300, mx = 0, bestWorst = 1e300;
        int j = i;
        while (j < n) {
            double a = std::max(area[j], 1e-9);
            double nsum = sum + a, nmn = std::min(mn, a), nmx = std::max(mx, a);
            double s2 = side * side;
            double w = std::max(s2 * nmx / (nsum * nsum), (nsum * nsum) / (s2 * nmn));
            if (j == i || w <= bestWorst) { sum = nsum; mn = nmn; mx = nmx; bestWorst = w; ++j; }
            else break;
        }
        double thick = sum / side;                       // row/column depth
        if (r.width() >= r.height()) {                   // column on the left
            double y = r.top();
            for (int k = i; k < j; ++k) { double h = std::max(area[k], 1e-9) / thick; out[k] = QRectF(r.left(), y, thick, h); y += h; }
            r = QRectF(r.left() + thick, r.top(), r.width() - thick, r.height());
        } else {                                         // row along the top
            double x = r.left();
            for (int k = i; k < j; ++k) { double wd = std::max(area[k], 1e-9) / thick; out[k] = QRectF(x, r.top(), wd, thick); x += wd; }
            r = QRectF(r.left(), r.top() + thick, r.width(), r.height() - thick);
        }
        i = j;
    }
}

void CallGraphWidget::layoutTree(int idx, QRectF r) {
    tree_[idx].rect = r;
    const std::vector<int>& kids = tree_[idx].kids;
    if (kids.empty()) return;

    // Leave a header strip (label) + border; the interior is divided among the
    // children AND a "self" slot so the node's own cost keeps proportional area
    // (shown as the uncovered parent colour behind its nested callees).
    double header = (idx == 0) ? 2 : (r.height() > 26 ? 15.0 : 2.0);
    QRectF inner = r.adjusted(3, header, -3, -3);
    if (inner.width() < 7 || inner.height() < 7) return;

    std::vector<std::pair<uint64_t, int>> items;   // (weight, kid index; -1 = self)
    for (int k : kids) items.push_back({std::max<uint64_t>(tree_[k].subtree, 1), k});
    if (idx != 0 && tree_[idx].self > 0) items.push_back({tree_[idx].self, -1});
    std::sort(items.rbegin(), items.rend());

    double total = 0; for (auto& it : items) total += it.first;
    double scale = inner.width() * inner.height() / total;
    std::vector<double> areas; areas.reserve(items.size());
    for (auto& it : items) areas.push_back(it.first * scale);

    std::vector<QRectF> rects;
    squarify(areas, inner, rects);
    for (size_t i = 0; i < items.size(); ++i) {
        int k = items[i].second;
        if (k < 0) continue;                       // self slot: leave parent colour
        if (rects[i].width() > 2 && rects[i].height() > 2) layoutTree(k, rects[i]);
        else tree_[k].rect = QRectF();             // too small to show
    }
}

void CallGraphWidget::paintTreeMap(QPainter& p) {
    if (tree_.size() < 2) return;
    layoutTree(0, QRectF(8, 26, width() - 16, height() - 44));

    // Draw parents before children (ascending depth) so nesting shows.
    std::vector<int> order;
    for (int i = 1; i < (int)tree_.size(); ++i) if (!tree_[i].rect.isEmpty()) order.push_back(i);
    std::sort(order.begin(), order.end(), [&](int a, int b) { return tree_[a].depth < tree_[b].depth; });

    bk::Trace& tr = board_->trace();
    QFont mono("monospace"); mono.setStyleHint(QFont::TypeWriter);
    for (int i : order) {
        const TNode& t = tree_[i];
        QRectF r = t.rect;
        auto it = nodeIndex_.find(t.entry);
        double vis = it != nodeIndex_.end()
            ? std::clamp(tr.fade(nodes_[it->second].lastActive, ACTIVE_WINDOW), 0.0, 1.0) : 1.0;
        double dk = 0.22 + 0.78 * vis;   // idle boxes darken (but stay visible)
        double frac = double(t.self) / double(maxCost_);
        QColor fill = dim(heatColor(frac), dk);
        bool hl = ((int)t.entry == link_);
        p.setPen(hl ? QPen(QColor(255, 245, 150), 2.0) : QPen(fill.darker(220), 1.0));
        p.setBrush(fill);
        p.drawRect(r);

        // Label if the box is big enough (address; add % on the header row).
        if (r.width() < 34 || r.height() < 13) continue;
        double pct = 100.0 * double(t.self) / double(totalCost_);
        mono.setPixelSize(10);
        p.setFont(mono);
        double bright = (0.45 + 0.5 * frac) * dk;   // ≈ dimmed fill brightness
        p.setPen(bright > 0.55 ? QColor(20, 15, 15) : QColor(230, 236, 246));
        QString lbl = oct6(t.entry);
        if (r.width() > 96 && pct >= 0.1) lbl += QString("  %1%").arg(pct, 0, 'f', pct >= 10 ? 0 : 1);
        p.drawText(r.adjusted(4, 1, -3, 0), Qt::AlignLeft | Qt::AlignTop, lbl);
    }
}

// Scale a colour's brightness by k (0..1); idle nodes are drawn darker, not
// transparent, so they stay visible until the user purges them.
static QColor dim(QColor c, double k) {
    float h, s, v, a; c.getHsvF(&h, &s, &v, &a);
    if (h < 0) h = 0;   // achromatic guard (fromHsvF needs a valid hue)
    return QColor::fromHsvF(h, s, std::clamp(v * float(k), 0.0f, 1.0f), a);
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
    p.setFont(mono);
    p.setPen(QColor(200, 210, 255));
    QString hdr = (mode_ == ModeTree)
        ? QString::fromUtf8("Карта вызовов — площадь ∝ доле времени CPU, вложение = вызываемые подпрограммы, цвет = собств. стоимость")
        : QString::fromUtf8("Граф вызовов (топ-%1 блоков) — цвет = доля времени CPU, толщина стрелки = частота вызовов").arg(topN_);
    p.drawText(10, 18, hdr);
    if (nodes_.empty()) {
        p.setPen(QColor(140, 150, 170));
        p.drawText(10, 40, QString::fromUtf8("нет данных трассировки — запустите игру"));
        return;
    }

    if (mode_ == ModeTree) paintTreeMap(p);
    else                   paintGraph(p);

    // Footer hint (screen space).
    p.setFont(mono);
    p.setPen(QColor(120, 130, 150));
    p.drawText(10, height() - 8, QString::fromUtf8(mode_ == ModeTree
        ? "Tab — граф вызовов · клик — дизасм · Del — убрать потемневшие"
        : "Tab — карта · тащить блок — двигать, фон — панорама · колесо — масштаб · [ ] — число блоков · L — авто-раскладка · Del — убрать · клик — дизасм"));
}

void CallGraphWidget::paintGraph(QPainter& p) {
    QFont mono("monospace"); mono.setStyleHint(QFont::TypeWriter); mono.setPixelSize(11);
    if (fitView_) fitToView();
    const QTransform vt = viewTransform();

    // ---- Edges (draw first, under the boxes) ----
    bk::Trace& tr = board_->trace();
    const double logMaxW = std::log(double(maxWeight_) + 1.0);
    for (auto& e : edges_) {
        const Node& a = nodes_[nodeIndex_[e.from]];
        const Node& b = nodes_[nodeIndex_[e.to]];
        bool back = b.level <= a.level;            // cycle / recursion / self up-call

        // Darken the edge with the less-recently-active of its two endpoints.
        double vis = std::clamp(std::min(tr.fade(a.lastActive, ACTIVE_WINDOW),
                                         tr.fade(b.lastActive, ACTIVE_WINDOW)), 0.0, 1.0);
        double dk = 0.22 + 0.78 * vis;   // brightness factor (idle → dark)

        // Exit point on the caller and entry (tip) on the callee, plus the unit
        // directions of travel there. Forward edges leave the bottom and enter the
        // top; back edges (cycles) leave the right side and re-enter the right side.
        QPointF p1, tip, exitDir, nrm;
        if (back) {
            p1  = vt.map(QPointF(a.box.right(), a.y + BOX_H / 2));
            tip = vt.map(QPointF(b.box.right(), b.y + BOX_H / 2));
            exitDir = QPointF(1, 0);
            nrm     = QPointF(-1, 0);      // travels left into the right edge
        } else {
            p1  = vt.map(QPointF(a.x, a.box.bottom()));
            tip = vt.map(QPointF(b.x, b.box.top()));
            exitDir = QPointF(0, 1);
            nrm     = QPointF(0, 1);       // travels down into the top edge
        }
        double t = std::log(double(e.weight) + 1.0) / (logMaxW + 1e-9);   // 0..1 (colour)
        // Thickness grows logarithmically with the absolute call count:
        // ×1 → ~1px, ×10 → ~3px, ×100 → ~5px, ×1000 → ~7px (before zoom).
        double w = std::clamp(1.0 + 0.9 * std::log(double(e.weight)), 1.0, 9.0) * zoom_;

        // Slim arrowhead sitting on a short straight approach segment that runs
        // perpendicular into the box, so the head is always clean regardless of
        // how the curve arrives.
        double ahLen = std::max(12.0 * zoom_, 2.1 * w);   // arrowhead length
        double ahW   = std::max(3.4 * zoom_, 0.8 * w);    // half-width (narrow)
        double seg   = ahLen + 8.0 * zoom_;               // straight segment length
        QPointF pB    = tip - nrm * seg;                  // curve ends here
        QPointF aBase = tip - nrm * ahLen;                // line ends / head begins

        // Control points: leave the caller along exitDir, arrive at pB along nrm
        // (so the curve is tangent to the straight segment — no kink).
        double d = std::hypot(pB.x() - p1.x(), pB.y() - p1.y());
        double h = std::clamp(d * 0.4, 12.0, 500.0);
        QPointF c1, c2;
        if (back) {
            double bow = std::max(h, (50.0 + 40.0 * t) * zoom_);
            c1 = p1 + exitDir * bow;
            c2 = pB - nrm * bow;
        } else {
            c1 = p1 + exitDir * h;
            c2 = pB - nrm * h;
        }

        QColor ec = dim(back ? QColor(230, 150, 60) : QColor(90, 170, 235), dk);
        ec.setAlpha(150 + int(90 * t));

        QPainterPath path(p1);
        if (!back && !e.bends.empty()) {
            // Route a smooth spline through the layout's channel points (dummy
            // nodes) so a multi-level edge passes between boxes, not under them.
            std::vector<QPointF> pts; pts.push_back(p1);
            for (const QPointF& bd : e.bends) pts.push_back(vt.map(bd));
            pts.push_back(pB);
            for (size_t k = 0; k + 1 < pts.size(); ++k) {
                QPointF s = pts[k], d2 = pts[k + 1];
                double dy = d2.y() - s.y();
                path.cubicTo(s + QPointF(0, dy * 0.4), d2 - QPointF(0, dy * 0.4), d2);
            }
        } else {
            path.cubicTo(c1, c2, pB);
        }
        path.lineTo(aBase);                               // straight run into the head
        p.setPen(QPen(ec, std::max(1.0, w), Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        p.setBrush(Qt::NoBrush);
        p.drawPath(path);

        // Slim isosceles arrowhead from aBase to the tip, along nrm.
        QPointF perp(-nrm.y(), nrm.x());
        QColor head = ec; head.setAlpha(240);
        p.setPen(Qt::NoPen); p.setBrush(head);
        p.drawPolygon(QPolygonF({tip, aBase + perp * ahW, aBase - perp * ahW}));

        // Call-count label at the curve midpoint (skip when zoomed too far out to
        // keep it readable / uncluttered).
        if (zoom_ >= 0.30) {
            QPointF mid = (!back && !e.bends.empty())
                ? vt.map(e.bends[e.bends.size() / 2])
                : 0.125 * p1 + 0.375 * c1 + 0.375 * c2 + 0.125 * pB;
            QString lbl = cgCount(e.weight);
            QFont ef = mono; ef.setPixelSize(std::clamp(int(10 * zoom_), 7, 12));
            p.setFont(ef);
            QFontMetrics fm(ef);
            QRectF br = QRectF(0, 0, fm.horizontalAdvance(lbl) + 6, fm.height() + 1);
            br.moveCenter(mid);
            p.setPen(Qt::NoPen);
            p.setBrush(QColor(10, 12, 20, 205));
            p.drawRoundedRect(br, 3, 3);
            p.setPen(QColor(215, 228, 248));
            p.drawText(br, Qt::AlignCenter, lbl);
        }
    }

    // ---- Nodes ----
    p.setBrush(Qt::NoBrush);
    for (auto& n : nodes_) {
        double vis = std::clamp(tr.fade(n.lastActive, ACTIVE_WINDOW), 0.0, 1.0);
        double dk  = 0.22 + 0.78 * vis;   // idle boxes darken (but stay visible)
        QRectF r = vt.mapRect(n.box);
        double frac = double(n.cost) / double(maxCost_);
        double pct  = 100.0 * double(n.cost) / double(totalCost_);
        QColor fill = dim(heatColor(frac), dk);
        bool hl = ((int)n.entry == link_);
        p.setPen(hl ? QPen(QColor(255, 245, 150), 2.5) : QPen(fill.lighter(160), 1.4));
        p.setBrush(fill);
        p.drawRoundedRect(r, 5, 5);

        // Semantic zoom: at low zoom (fit view) boxes are tiny — skip labels and
        // show just colour + heat, so the overview stays legible; detail appears
        // as you zoom in (address+% below, then a disassembly preview).
        if (r.height() < 16 || zoom_ < 0.55) continue;
        QFont nf = mono; nf.setPixelSize(std::clamp(int(11 * zoom_), 7, 15));
        p.setFont(nf);
        QFontMetrics fm(nf);
        QString l1 = oct6(n.entry);
        QString l2 = QString("%1%  ×%2").arg(pct, 0, 'f', pct >= 10 ? 0 : 1).arg(cgCount(n.calls));
        p.setPen(dim(QColor(240, 244, 255), dk));
        p.drawText(r.adjusted(4, 2, -4, -2), Qt::AlignHCenter | Qt::AlignTop, l1);
        p.setPen(dim(QColor(210, 220, 240), dk));
        p.drawText(r.adjusted(4, fm.height() + 1, -4, -2), Qt::AlignHCenter | Qt::AlignTop, l2);

        // At high zoom, list a few instructions disassembled from the entry, so a
        // box doubles as a code preview of the routine.
        if (zoom_ < 1.3) continue;
        QFont df = mono; df.setPixelSize(std::clamp(int(10 * zoom_), 8, 13));
        p.setFont(df);
        QFontMetrics dfm(df);
        double dlh = dfm.height();
        double y = r.top() + 2 * fm.height() + 5;
        if (r.bottom() - y < dlh) continue;                 // no room for even one
        p.setPen(QColor(255, 255, 255, int(40 * (0.3 + 0.7 * vis))));   // faint separator
        p.drawLine(QPointF(r.left() + 5, y - 2), QPointF(r.right() - 5, y - 2));
        const bk::Memory& mem = board_->memory();
        uint16_t a = n.entry;
        p.setPen(dim(QColor(205, 224, 210), dk));
        for (int k = 0; k < 16 && r.bottom() - y >= dlh; ++k) {
            bk::DisasmLine dl = bk::disasm(mem, a);
            QString s = oct6(a) + "  " + QString::fromStdString(dl.text);
            p.drawText(QRectF(r.left() + 6, y, r.width() - 10, dlh),
                       Qt::AlignLeft | Qt::AlignVCenter,
                       dfm.elidedText(s, Qt::ElideRight, int(r.width() - 12)));
            y += dlh;
            a += static_cast<uint16_t>(dl.words * 2);
        }
    }
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
    if (e->button() != Qt::LeftButton) return;
    pressPos_ = lastDrag_ = e->pos();
    draggingNode_ = false;
    dragging_ = false;
    // In the graph view, grab a box under the cursor to move it; empty space pans.
    if (mode_ == ModeGraph) {
        QPointF g = viewTransform().inverted().map(QPointF(e->pos()));
        for (int i = (int)nodes_.size() - 1; i >= 0; --i)
            if (nodes_[i].box.contains(g)) {
                draggingNode_ = true;
                dragEntry_ = nodes_[i].entry;
                dragOffset_ = g - QPointF(nodes_[i].x, nodes_[i].y);
                fitView_ = false;   // keep the view still while arranging
                return;
            }
    }
    dragging_ = true;   // pan the canvas
}

void CallGraphWidget::mouseMoveEvent(QMouseEvent* e) {
    if (draggingNode_) {
        auto it = nodeIndex_.find(dragEntry_);
        if (it == nodeIndex_.end()) { draggingNode_ = false; return; }
        QPointF np = viewTransform().inverted().map(QPointF(e->pos())) - dragOffset_;
        Node& n = nodes_[it->second];
        n.x = np.x(); n.y = np.y();
        n.box = QRectF(n.x - BOX_W / 2, n.y, BOX_W, BOX_H);
        manualPos_[dragEntry_] = np;     // survives rebuilds; edges follow the box
        update();
    } else if (dragging_) {
        fitView_ = false;
        pan_ += e->pos() - lastDrag_;
        lastDrag_ = e->pos();
        update();
    } else if (mode_ == ModeGraph) {
        // Hover → broadcast the routine under the cursor for linked highlighting.
        QPointF g = viewTransform().inverted().map(QPointF(e->pos()));
        int found = -1;
        for (int i = (int)nodes_.size() - 1; i >= 0; --i)
            if (nodes_[i].box.contains(g)) { found = nodes_[i].entry; break; }
        if (found != hoverEmit_) { hoverEmit_ = found; setHighlight(found); emit hoverAddress(found); }
    }
}

void CallGraphWidget::leaveEvent(QEvent*) {
    if (hoverEmit_ != -1) { hoverEmit_ = -1; emit hoverAddress(-1); }
    setHighlight(-1);
}

void CallGraphWidget::mouseReleaseEvent(QMouseEvent* e) {
    if (e->button() != Qt::LeftButton) return;
    bool wasNode = draggingNode_;
    dragging_ = false;
    draggingNode_ = false;
    if ((e->pos() - pressPos_).manhattanLength() > 4) return;   // was a drag
    if (wasNode) { emit addressPicked(dragEntry_); return; }     // click on a box
    // Click: hit-test a box and jump the disassembler to that routine.
    if (mode_ == ModeTree) {
        // Deepest (most specific) rectangle under the cursor wins.
        int best = -1;
        for (int i = 1; i < (int)tree_.size(); ++i)
            if (!tree_[i].rect.isEmpty() && tree_[i].rect.contains(e->pos())
                && (best < 0 || tree_[i].depth > tree_[best].depth))
                best = i;
        if (best >= 0) emit addressPicked(tree_[best].entry);
        return;
    }
    QPointF g = viewTransform().inverted().map(QPointF(e->pos()));
    for (auto& n : nodes_)
        if (n.box.contains(g)) { emit addressPicked(n.entry); return; }
}

bool CallGraphWidget::event(QEvent* e) {
    // Tab/Backtab would otherwise be swallowed by focus navigation; use it to
    // switch between the graph and the treemap views.
    if (e->type() == QEvent::KeyPress) {
        auto* ke = static_cast<QKeyEvent*>(e);
        if (ke->key() == Qt::Key_Tab || ke->key() == Qt::Key_Backtab) {
            mode_ = (mode_ == ModeGraph) ? ModeTree : ModeGraph;
            update();
            return true;
        }
    }
    return QWidget::event(e);
}

void CallGraphWidget::keyPressEvent(QKeyEvent* e) {
    switch (e->key()) {
    case Qt::Key_BracketRight: case Qt::Key_Plus: case Qt::Key_Equal:
        topN_ = std::min(topN_ + 4, 80); rebuild(); update(); break;
    case Qt::Key_BracketLeft: case Qt::Key_Minus: case Qt::Key_Underscore:
        topN_ = std::max(topN_ - 4, 3); rebuild(); update(); break;
    case Qt::Key_0:
        fitView_ = true; update(); break;
    case Qt::Key_L:
        // Re-layout automatically: discard manual placements.
        manualPos_.clear(); rebuild(); fitView_ = true; update(); break;
    case Qt::Key_Delete: case Qt::Key_Backspace: {
        // Purge the fully-darkened (long-idle) functions from the graph.
        bk::Trace& tr = board_->trace();
        for (auto& n : nodes_)
            if (tr.fade(n.lastActive, ACTIVE_WINDOW) <= 0.0) hidden_.insert(n.entry);
        rebuild(); update();
        break;
    }
    default: QWidget::keyPressEvent(e);
    }
}
