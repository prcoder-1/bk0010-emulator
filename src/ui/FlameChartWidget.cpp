#include "FlameChartWidget.h"
#include "Board.h"
#include "Disasm.h"
#include <QPainter>
#include <QMouseEvent>
#include <QWheelEvent>
#include <QKeyEvent>
#include <algorithm>
#include <vector>
#include <climits>

using bk::Board;

static QString oct6(uint16_t v) { return QString("%1").arg(v, 6, 8, QChar('0')); }

// Stable warm colour per function (flame look), brighter when highlighted/hovered.
static QColor flameColor(uint16_t func, bool hot) {
    uint32_t h = func * 2654435761u;
    double hue = 0.02 + 0.11 * ((h >> 8) & 0xFF) / 255.0;
    double sat = 0.72 - 0.15 * ((h >> 16) & 0xFF) / 255.0;
    return QColor::fromHsvF(float(hue), float(sat), hot ? 0.96f : 0.78f);
}

FlameChartWidget::FlameChartWidget(Board* board, QWidget* parent)
    : QWidget(parent), board_(board) {
    setMinimumSize(460, 240);
    setFocusPolicy(Qt::StrongFocus);
    setMouseTracking(true);
    board_->trace().setFlameEnabled(true);
    board_->trace().setSpansEnabled(true);
}

// Called every 50 Hz emulation tick. A full repaint of the chart is expensive, so
// throttle the periodic refresh to ~16 Hz — the emulation runs in the same GUI
// thread, and repainting at 50 Hz starves it (esp. with a wide time window).
// Interactive repaints (zoom/scroll/hover) call update() directly and are unaffected.
void FlameChartWidget::refresh() { if (++refreshTick_ >= 3) { refreshTick_ = 0; update(); } }

void FlameChartWidget::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.fillRect(rect(), QColor(14, 15, 22));
    QFont mono("monospace"); mono.setStyleHint(QFont::TypeWriter); mono.setPixelSize(11);
    p.setFont(mono);
    QFontMetrics fm(mono);
    rowH_ = fm.height() + 4;

    bk::Trace& tr = board_->trace();
    const uint64_t now = tr.flameTick();
    p.setPen(QColor(200, 210, 255));
    double winMs = winTicks_ / 3000.0;
    p.drawText(10, 16, QString::fromUtf8(
        "Хронология вызовов — X = время (%1 мс в окне), Y = глубина стека; вправо = сейчас")
        .arg(winMs, 0, 'f', winMs >= 10 ? 0 : 1));

    if (now == 0) {
        p.setPen(QColor(140, 150, 170));
        p.drawText(10, 40, QString::fromUtf8("нет данных — запустите игру (стек собирается на лету)"));
        return;
    }

    const int top = 24, bottom = height() - 20;
    const double t1 = double(now);
    const double t0 = t1 - winTicks_;
    const double plotL = 8, plotR = width() - 8, plotW = plotR - plotL;
    auto tickToX = [&](double T) { return plotL + (T - t0) / winTicks_ * plotW; };

    bars_.clear();
    maxDepth_ = 0;

    // Sub-pixel coalescing: with a wide window (10 s = 30M ticks over ~1000 px)
    // thousands of short calls collapse onto the same pixel column. Drawing each is
    // pointless and is what makes the chart choke the emulator, so keep only the
    // first (newest) bar per (depth, pixel-column) among sub-pixel spans. Wider bars
    // always draw. This bounds draw calls to ~pixels×depth regardless of span count.
    std::vector<int> lastCol;                           // last drawn column per depth

    auto drawSpan = [&](uint16_t func, int depth, uint64_t s, uint64_t e) {
        if (func == 0) return;                          // skip the synthetic root
        double a = std::max<double>(s, t0), b = std::min<double>(e, t1);
        if (b <= a) return;
        double x0 = tickToX(a), x1 = tickToX(b);
        double y = top + depth * rowH_ - scroll_;
        maxDepth_ = std::max(maxDepth_, depth);
        if (y + rowH_ < top || y > bottom) return;
        int col = int(x0);
        if (x1 - x0 < 1.0 && depth < (int)lastCol.size() && lastCol[depth] == col) return;
        if (depth >= (int)lastCol.size()) lastCol.resize(depth + 1, INT_MIN);
        lastCol[depth] = col;
        QRectF r(x0, y, std::max(1.0, x1 - x0), rowH_ - 1);
        bars_.push_back({ r, func });
        bool hl = ((int)func == link_);
        p.setBrush(flameColor(func, hl));
        p.setPen(hl ? QPen(QColor(255, 245, 150), 1.6) : QPen(QColor(18, 18, 26)));
        p.drawRect(r);
        if (r.width() > 34) {
            p.setPen(QColor(25, 20, 15));
            QString lbl = oct6(func);
            if (r.width() > 120) lbl += "  " + QString::fromStdString(bk::disasm(board_->memory(), func).text);
            p.drawText(r.adjusted(3, 0, -2, 0), Qt::AlignVCenter | Qt::AlignLeft,
                       fm.elidedText(lbl, Qt::ElideRight, int(r.width() - 5)));
        }
    };

    // Completed spans (iterate from the newest until they fall before the window),
    // then the currently-open frames extending to "now". Clamp the window bounds to
    // unsigned: t0 can be negative (window wider than the elapsed time / zoomed out),
    // and casting a negative double to uint64_t would wrap to a huge value.
    const uint64_t t0u = t0 > 0.0 ? static_cast<uint64_t>(t0) : 0;
    const uint64_t t1u = static_cast<uint64_t>(t1);
    const auto& spans = tr.spans();
    int scanned = 0;
    for (auto it = spans.rbegin(); it != spans.rend() && scanned < 200000; ++it, ++scanned) {
        if (it->end < t0u) break;
        if (it->start >= t1u) continue;
        drawSpan(it->func, it->depth, it->start, it->end);
    }
    std::vector<bk::Trace::Span> open;
    tr.openFrames(open);
    for (const auto& s : open) drawSpan(s.func, s.depth, s.start, s.end);

    p.setPen(QColor(120, 130, 150));
    p.drawText(10, height() - 6, QString::fromUtf8(
        "колесо — зум времени · тащить — глубина · клик — в дизасм · наведение — подсветка · 0 — сброс зума"));
}

void FlameChartWidget::wheelEvent(QWheelEvent* e) {
    double factor = e->angleDelta().y() > 0 ? 1.0 / 1.25 : 1.25;   // scroll up = zoom in
    winTicks_ = std::clamp(winTicks_ * factor, 20000.0, 3.0e8);
    update();
}

void FlameChartWidget::mousePressEvent(QMouseEvent* e) {
    if (e->button() == Qt::LeftButton) { dragging_ = true; pressPos_ = lastDrag_ = e->pos(); }
}

void FlameChartWidget::mouseMoveEvent(QMouseEvent* e) {
    if (dragging_) {
        scroll_ = std::clamp(scroll_ - (e->pos().y() - lastDrag_.y()), 0.0,
                             std::max(0.0, double(maxDepth_ + 1) * rowH_ - (height() - 44)));
        lastDrag_ = e->pos();
        update();
        return;
    }
    int found = -1;
    for (const Bar& b : bars_) if (b.rect.contains(e->pos())) { found = b.func; break; }
    if (found != hoverEmit_) { hoverEmit_ = found; setHighlight(found); emit hoverAddress(found); }
}

void FlameChartWidget::mouseReleaseEvent(QMouseEvent* e) {
    if (e->button() != Qt::LeftButton) return;
    dragging_ = false;
    if ((e->pos() - pressPos_).manhattanLength() > 4) return;    // was a drag
    for (const Bar& b : bars_)
        if (b.rect.contains(e->pos())) { emit addressPicked(b.func); return; }
}

void FlameChartWidget::leaveEvent(QEvent*) {
    if (hoverEmit_ != -1) { hoverEmit_ = -1; emit hoverAddress(-1); }
    setHighlight(-1);
}
