#include "FlameWidget.h"
#include "Board.h"
#include "Disasm.h"
#include <QPainter>
#include <QMouseEvent>
#include <QWheelEvent>
#include <QKeyEvent>
#include <algorithm>
#include <cmath>

using bk::Board;

static QString oct6(uint16_t v) { return QString("%1").arg(v, 6, 8, QChar('0')); }

// A stable warm colour per function (flame-graph look): hue in the red→yellow band
// derived from the address, brighter for the box itself.
static QColor flameColor(uint16_t func, bool hot) {
    uint32_t h = func * 2654435761u;
    double hue = 0.02 + 0.11 * ((h >> 8) & 0xFF) / 255.0;   // 0.02..0.13 (red→amber)
    double val = hot ? 0.95 : 0.78;
    double sat = 0.72 - 0.15 * ((h >> 16) & 0xFF) / 255.0;
    return QColor::fromHsvF(float(hue), float(sat), float(val));
}

FlameWidget::FlameWidget(Board* board, QWidget* parent)
    : QWidget(parent), board_(board) {
    setMinimumSize(420, 260);
    setFocusPolicy(Qt::StrongFocus);
    setMouseTracking(true);
    board_->trace().setFlameEnabled(true);
}

void FlameWidget::refresh() {
    uint32_t now = board_->trace().now();
    // The layout only changes when the profile is re-snapshotted, so repaint *only*
    // then. Interactive changes (hover/click/scroll/zoom/highlight) repaint on their
    // own. Previously refresh() repainted at the full 50 Hz while the data changed
    // far slower — redrawing an identical image many times per update and stealing
    // time from the emulation, which runs in this same GUI thread.
    if (incl_.empty() || now - lastBuild_ >= 12) { rebuild(); lastBuild_ = now; update(); }
}

// Inclusive ticks per node = self + Σ children. A child is always created after
// its parent, so its index is higher → one reverse-index pass accumulates it.
void FlameWidget::rebuild() {
    board_->trace().setFlameEnabled(true);
    const auto& f = board_->trace().flame();
    const int N = (int)f.size();
    incl_.assign(N, 0);
    for (int i = 0; i < N; ++i) incl_[i] = f[i].self;
    for (int i = N - 1; i >= 1; --i) incl_[f[i].parent] += incl_[i];
    total_ = std::max<uint64_t>(incl_.empty() ? 1 : incl_[0], 1);
    if (focus_ >= N) focus_ = 0;
}

void FlameWidget::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.fillRect(rect(), QColor(14, 15, 22));
    p.setRenderHint(QPainter::Antialiasing, false);
    QFont mono("monospace"); mono.setStyleHint(QFont::TypeWriter); mono.setPixelSize(11);
    p.setFont(mono);
    QFontMetrics fm(mono);
    rowH_ = fm.height() + 5;

    const auto& f = board_->trace().flame();
    p.setPen(QColor(200, 210, 255));
    p.drawText(10, 16, QString::fromUtf8(
        "Пламенный граф — ширина ∝ времени CPU (inclusive), вниз = глубже по стеку"));

    if ((int)f.size() < 2 || incl_.empty() || incl_[0] == 0) {
        p.setPen(QColor(140, 150, 170));
        p.drawText(10, 40, QString::fromUtf8("нет данных — запустите игру (стек вызовов собирается на лету)"));
        return;
    }
    if (focus_ >= (int)f.size()) focus_ = 0;

    const int top = 26;
    const int baseDepth = f[focus_].depth;
    boxes_.clear();
    double maxDepthPx = 0;

    // Recursive icicle layout starting from the focused node at full width.
    struct Frame { int node; double x, w; int depth; };
    std::vector<Frame> st{ { focus_, 0.0, double(width()), 0 } };
    std::vector<int> kids;
    while (!st.empty()) {
        Frame fr = st.back(); st.pop_back();
        if (fr.w < 1.0) continue;
        double y = top + fr.depth * rowH_ - scroll_;
        QRectF r(fr.x, y, fr.w - 1, rowH_ - 1);
        boxes_.push_back({ r, fr.node });
        maxDepthPx = std::max(maxDepthPx, double((fr.depth + 1) * rowH_));

        if (y + rowH_ >= top && y <= height()) {
            double frac = double(incl_[fr.node]) / double(total_);
            bool hl = ((int)f[fr.node].func == link_ && fr.node != 0);
            QColor c = flameColor(f[fr.node].func, fr.node == hover_ || hl);
            p.fillRect(r, c);
            p.setPen(hl ? QPen(QColor(255, 245, 150), 2.0) : QPen(QColor(20, 20, 28)));
            p.drawRect(r);
            if (r.width() > 34) {
                double pct = 100.0 * frac;
                QString lbl = (f[fr.node].func == 0 && fr.node == 0)
                    ? QString("ВСЁ  %1%").arg(pct, 0, 'f', 0)
                    : QString("%1  %2%").arg(oct6(f[fr.node].func)).arg(pct, 0, 'f', pct >= 10 ? 0 : 1);
                // Semantic zoom: wider boxes also reveal the routine's first instruction.
                if (r.width() > 120 && f[fr.node].func != 0)
                    lbl += "   " + QString::fromStdString(bk::disasm(board_->memory(), f[fr.node].func).text);
                p.setPen(QColor(25, 20, 15));
                p.drawText(r.adjusted(4, 0, -2, 0), Qt::AlignVCenter | Qt::AlignLeft,
                           fm.elidedText(lbl, Qt::ElideRight, int(r.width() - 6)));
            }
        }

        // Children, widest (costliest) first, packed left-to-right.
        kids.clear();
        for (auto& kv : f[fr.node].kids) kids.push_back(kv.second);
        std::sort(kids.begin(), kids.end(), [&](int a, int b) { return incl_[a] > incl_[b]; });
        double cx = fr.x, scale = fr.w / double(incl_[fr.node]);
        // push in reverse so the widest child is processed first (nicer overlap order)
        std::vector<Frame> row;
        for (int c : kids) { double cw = incl_[c] * scale; row.push_back({ c, cx, cw, fr.depth + 1 }); cx += cw; }
        for (auto it = row.rbegin(); it != row.rend(); ++it) st.push_back(*it);
    }
    (void)baseDepth;
    contentH_ = maxDepthPx;

    // Footer: hovered frame details, else the hint.
    p.setPen(QColor(120, 130, 150));
    QString footer;
    if (hover_ >= 0 && hover_ < (int)f.size()) {
        double selfPct = 100.0 * double(f[hover_].self) / double(total_);
        double inclPct = 100.0 * double(incl_[hover_]) / double(total_);
        footer = QString::fromUtf8("%1 — собств. %2%, вкл. %3%   ·   клик — вглубь · ПКМ/Backspace — вверх · 0 — весь · Del — сброс")
                     .arg(f[hover_].func == 0 ? QString("ВСЁ") : oct6(f[hover_].func))
                     .arg(selfPct, 0, 'f', 1).arg(inclPct, 0, 'f', 1);
    } else {
        footer = QString::fromUtf8("клик — вглубь · ПКМ/Backspace — вверх · 0 — весь граф · Del — сброс · колесо — прокрутка");
    }
    p.drawText(10, height() - 6, footer);
}

void FlameWidget::mousePressEvent(QMouseEvent* e) {
    if (e->button() == Qt::RightButton) {          // zoom out one level
        const auto& f = board_->trace().flame();
        if (focus_ < (int)f.size() && f[focus_].parent >= 0) { focus_ = f[focus_].parent; update(); }
        return;
    }
    if (e->button() != Qt::LeftButton) return;
    for (const Box& b : boxes_)
        if (b.rect.contains(e->pos())) {
            const auto& f = board_->trace().flame();
            // Clicking the focused root (top row) steps back up; any other box drills in.
            if (b.node == focus_) { if (f[focus_].parent >= 0) focus_ = f[focus_].parent; }
            else focus_ = b.node;
            if (f[b.node].func != 0) emit addressPicked(f[b.node].func);
            update();
            return;
        }
}

void FlameWidget::mouseMoveEvent(QMouseEvent* e) {
    int h = -1;
    for (const Box& b : boxes_) if (b.rect.contains(e->pos())) { h = b.node; break; }
    if (h != hover_) { hover_ = h; update(); }
    const auto& f = board_->trace().flame();
    int addr = (h >= 0 && h < (int)f.size()) ? int(f[h].func) : -1;
    if (addr == 0) addr = -1;                       // root has no address
    if (addr != hoverEmit_) { hoverEmit_ = addr; setHighlight(addr); emit hoverAddress(addr); }
}

void FlameWidget::leaveEvent(QEvent*) {
    if (hover_ != -1) { hover_ = -1; update(); }
    if (hoverEmit_ != -1) { hoverEmit_ = -1; emit hoverAddress(-1); }
    setHighlight(-1);
}

void FlameWidget::wheelEvent(QWheelEvent* e) {
    scroll_ -= e->angleDelta().y() * 0.5;
    scroll_ = std::clamp(scroll_, 0.0, std::max(0.0, contentH_ - (height() - 40.0)));
    update();
}

void FlameWidget::keyPressEvent(QKeyEvent* e) {
    const auto& f = board_->trace().flame();
    switch (e->key()) {
    case Qt::Key_Backspace: case Qt::Key_Left:
        if (focus_ < (int)f.size() && f[focus_].parent >= 0) { focus_ = f[focus_].parent; update(); }
        break;
    case Qt::Key_0: case Qt::Key_Home:
        focus_ = 0; scroll_ = 0; update(); break;
    case Qt::Key_Delete:
        board_->trace().flameClear(); focus_ = 0; scroll_ = 0; rebuild(); update(); break;
    default: QWidget::keyPressEvent(e);
    }
}
