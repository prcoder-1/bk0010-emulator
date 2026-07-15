#include "HotChartWidget.h"
#include <algorithm>
#include <unordered_map>
#include <cstdio>
#include "Board.h"
#include "Disasm.h"
#include <QPainter>
#include <QMouseEvent>
#include <QWheelEvent>
#include <QKeyEvent>

using bk::Board;

// JSR opcode range (subroutine call) — its target is a function entry.
static bool isJsr(uint16_t ir) { return ir >= 0004000 && ir <= 0004777; }

// Compact large counts with K/M/G suffixes (matches the code-graph hot list).
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

// Colour for a normalised address position t in [0,1]: a continuous hue sweep,
// so instructions with nearby addresses get nearby colours and distant ones get
// distinct colours. Hue runs 0°→300° (red→…→violet) to avoid the red wrap-around.
static QColor addrColor(double t) {
    if (t < 0.0) t = 0.0; else if (t > 1.0) t = 1.0;
    return QColor::fromHsvF(t * 300.0 / 360.0, 0.72, 0.93);
}

HotChartWidget::HotChartWidget(Board* board, QWidget* parent)
    : QWidget(parent), board_(board) {
    setWindowTitle("Горячие инструкции во времени");
    setMinimumSize(640, 360);
    setFocusPolicy(Qt::StrongFocus);
    resize(940, 470);
}

uint16_t HotChartWidget::funcOf(uint16_t addr) const {
    if (subEntries_.empty()) return 0;
    auto it = subEntries_.upper_bound(addr);
    if (it == subEntries_.begin()) return 0;  // before the first known entry
    --it;
    return *it;
}

void HotChartWidget::sample() {
    bk::Trace& tr = board_->trace();
    const bk::Memory& mem = board_->memory();
    bk::Cpu& cpu = board_->cpu();

    // Subroutine entries = JSR destinations (for colouring and aggregation).
    subEntries_.clear();
    for (auto& e : tr.edges())
        if (isJsr(mem.peekWord(e.first >> 16))) subEntries_.insert(e.first & 0xFFFF);

    // Cumulative CPU ticks per key (instruction, or subroutine when aggregating),
    // plus a representative call count. Hidden keys leave the ranking but still
    // count toward the grand total.
    std::unordered_map<uint16_t, uint64_t> curTicks;
    std::unordered_map<uint16_t, uint32_t> keyCalls;
    uint64_t totalTicks = 0;
    for (int a = 0; a < 0x10000; a += 2) {
        uint16_t a16 = static_cast<uint16_t>(a);
        uint32_t c = tr.execCount(a16);
        if (!c) continue;
        uint64_t tk = static_cast<uint64_t>(c) * cpu.instrTicks(mem.peekWord(a16));
        totalTicks += tk;
        uint16_t key = a16;
        if (aggregate_) { uint16_t fe = funcOf(a16); if (fe) key = fe; }
        if (excluded_.count(key)) continue;
        curTicks[key] += tk;
        if (key == a16) keyCalls[key] = c;      // instruction / entry own count
    }

    std::vector<std::pair<uint64_t, uint16_t>> all;
    all.reserve(curTicks.size());
    for (auto& kv : curTicks) all.push_back({kv.second, kv.first});
    std::sort(all.rbegin(), all.rend());
    std::set<uint16_t> topSet;
    for (int i = 0; i < topN_ && i < static_cast<int>(all.size()); ++i)
        topSet.insert(all[i].second);

    const uint64_t intervalTotal = totalTicks - lastTotalTicks_;
    lastTotalTicks_ = totalTicks;

    // Record this sample's CPU-tick timestamp, so timer-based frame boundaries can
    // be mapped onto the time axis (frames aren't a fixed 50 Hz on the BK).
    sampleTicks_.push_back(board_->totalTicks());
    while (sampleTicks_.size() > HIST) sampleTicks_.pop_front();

    // Drop keys that fell out of the top-N.
    series_.erase(std::remove_if(series_.begin(), series_.end(),
        [&](const Series& s) { return !topSet.count(s.addr); }), series_.end());

    // Add newcomers, back-filling zeros so every line stays the same length.
    size_t len = 0;
    for (auto& s : series_) len = std::max(len, s.hist.size());
    for (uint16_t k : topSet) {
        bool found = false;
        for (auto& s : series_) if (s.addr == k) { found = true; break; }
        if (!found) {
            Series s; s.addr = k; s.lastTicks = curTicks[k]; s.calls = keyCalls[k];
            s.pctAll = 0.0; s.hist.assign(len, 0.0);
            series_.push_back(s);
        }
    }

    // Append this interval's CPU-time share (%) to every line and refresh totals.
    for (auto& s : series_) {
        uint64_t cur = curTicks.count(s.addr) ? curTicks[s.addr] : 0;
        uint64_t d = cur > s.lastTicks ? cur - s.lastTicks : 0;
        s.hist.push_back(intervalTotal ? 100.0 * double(d) / double(intervalTotal) : 0.0);
        while (s.hist.size() > HIST) s.hist.pop_front();
        s.lastTicks = cur;
        if (keyCalls.count(s.addr)) s.calls = keyCalls[s.addr];
        s.pctAll = totalTicks ? 100.0 * double(cur) / double(totalTicks) : 0.0;
    }
}

void HotChartWidget::refresh() {
    bk::Trace& tr = board_->trace();
    tr.setEnabled(true);
    // The emulator reset (Board::reset → Trace::reset) zeroes exec counts and the
    // frame clock. Detect the clock running backwards and drop our accumulated
    // state, otherwise the next sample would compute deltas via unsigned underflow
    // (a spurious off-scale spike). See the code-review note.
    bool changed = false;
    if (tr.now() < lastNow_) {
        series_.clear();
        sampleTicks_.clear();
        lastTotalTicks_ = 0;
        lastNow_ = 0;
        changed = true;
    }
    // Sample on emulated time (frozen while paused), not wall-clock. Repaint only
    // when a new sample was taken (or state was reset) — between samples the chart
    // is identical, so repainting at the full 50 Hz just steals time from the
    // emulation on this same GUI thread. Interactive pan/zoom repaint directly.
    if (tr.now() - lastNow_ >= static_cast<uint32_t>(SAMPLE_FRAMES)) {
        lastNow_ = tr.now();
        sample();
        changed = true;
    }
    if (changed) update();
}

void HotChartWidget::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.fillRect(rect(), QColor(16, 18, 24));
    const bk::Memory& mem = board_->memory();
    QFontMetrics fm(font());
    const int lineH = fm.height() + 2;

    const int legendW = 330;
    const int marginL = 54, marginT = 24, marginB = 22, marginR = legendW + 12;
    QRect plot(marginL, marginT, width() - marginL - marginR, height() - marginT - marginB);
    if (plot.width() < 20 || plot.height() < 20) return;

    p.setPen(QColor(200, 220, 255));
    p.drawText(8, fm.ascent() + 3,
               QString::fromUtf8("Топ %1 %2 — %% времени CPU. G: инстр/функции · S: линии/стопка · "
                       "F: кадры · колесо на графике — зум времени (0 сброс), на списке — N%3")
                   .arg(topN_).arg(aggregate_ ? QString::fromUtf8("подпрограмм") : QString::fromUtf8("инструкций"))
                   .arg(excluded_.empty() ? QString() : QString::fromUtf8("  (скрыто %1)").arg(excluded_.size())));

    // Fixed full-scale axis: 0..100% of CPU time.
    const double yMax = 100.0;

    // Plot frame + gridlines with percentage Y labels.
    p.setPen(QColor(60, 66, 80));
    p.drawRect(plot);
    for (int g = 0; g <= 4; ++g) {
        int y = plot.bottom() - g * plot.height() / 4;
        p.setPen(QColor(40, 44, 56));
        if (g) p.drawLine(plot.left(), y, plot.right(), y);
        p.setPen(QColor(120, 130, 150));
        p.drawText(2, y + fm.ascent() / 2, QString::number(yMax * g / 4, 'f', 0) + "%");
    }
    p.drawText(plot.left(), height() - 6, QString("← старее   время   сейчас →"));

    // ---- Zoomable time-axis mapping ---------------------------------------
    // The visible slice is [viewL_, viewR_] as fractions of the retained history
    // (wheel over the plot zooms it). Map a history fraction 0..1 to an X pixel.
    plotRect_ = plot;
    const int m = static_cast<int>(sampleTicks_.size());
    const double vspan = std::max(1e-6, viewR_ - viewL_);
    auto fracToX = [&](double f) { return plot.left() + (f - viewL_) / vspan * plot.width(); };
    auto idxFrac = [&](int i) { return (m > 1) ? double(i) / (m - 1) : 1.0; };
    auto tickFrac = [&](uint64_t T) -> double {         // tick -> history fraction
        if (m < 2) return 1.0;
        if (T <= sampleTicks_[0]) return 0.0;
        if (T >= sampleTicks_[m - 1]) return 1.0;
        int lo = 0, hi = m - 1;
        while (lo + 1 < hi) { int mid = (lo + hi) / 2; if (sampleTicks_[mid] <= T) lo = mid; else hi = mid; }
        double sp = double(std::max<uint64_t>(1, sampleTicks_[lo + 1] - sampleTicks_[lo]));
        return (lo + double(T - sampleTicks_[lo]) / sp) / (m - 1);
    };

    // ---- Timer-based frame markers ----
    // BK frames are not a fixed 50 Hz: a game paces itself on the programmable
    // timer, spinning until it reaches/passes zero. Each such underflow is a real
    // frame boundary; draw them and report the frame rate / jitter over the window.
    if (showFrames_ && m >= 2) {
        std::vector<uint64_t> win;
        for (uint64_t T : board_->frameBoundaries())
            if (T >= sampleTicks_[0] && T <= sampleTicks_[m - 1]) win.push_back(T);
        int step = win.size() > 400 ? (int(win.size()) + 399) / 400 : 1;
        p.setPen(QColor(95, 130, 195, 90));
        for (size_t i = 0; i < win.size(); i += step) {
            double x = fracToX(tickFrac(win[i]));
            if (x >= plot.left() - 1 && x <= plot.right() + 1)
                p.drawLine(int(x + 0.5), plot.top(), int(x + 0.5), plot.bottom());
        }
        if (win.size() >= 2) {
            uint64_t avg = (win.back() - win.front()) / (win.size() - 1);
            uint64_t mn = ~0ull, mx = 0;
            for (size_t i = 1; i < win.size(); ++i) { uint64_t d = win[i] - win[i - 1]; mn = std::min(mn, d); mx = std::max(mx, d); }
            double avgMs = avg / 3000.0, fps = avg ? 3.0e6 / double(avg) : 0.0, jitMs = (mx - mn) / 3000.0;
            p.setPen(QColor(150, 185, 240));
            p.drawText(plot.left() + 4, plot.top() + fm.ascent() + 2,
                       QString::fromUtf8("кадры: %1  ~%2 мс (%3 к/с)  разброс %4 мс")
                           .arg(win.size()).arg(avgMs, 0, 'f', 1).arg(fps, 0, 'f', 0).arg(jitMs, 0, 'f', 1));
        }
    }

    // Colour: hue spread across the address range of the shown keys.
    uint16_t aMin = 0xFFFF, aMax = 0;
    for (auto& s : series_) { aMin = std::min(aMin, s.addr); aMax = std::max(aMax, s.addr); }
    const double aSpan = (aMax > aMin) ? double(aMax - aMin) : 1.0;
    auto lineColor = [&](uint16_t a) { return addrColor((a - aMin) / aSpan); };

    // ---- Series: overlaid lines, or stacked area (streamgraph) ----
    p.save();
    p.setClipRect(plot);
    if (stacked_) {
        std::vector<Series*> order;
        for (auto& s : series_) order.push_back(&s);
        std::sort(order.begin(), order.end(),
                  [](const Series* a, const Series* b) { return a->pctAll > b->pctAll; });
        std::vector<double> cum(m > 0 ? m : 1, 0.0);
        for (Series* sp : order) {
            const int sn = static_cast<int>(sp->hist.size());
            if (sn == 0) continue;
            QPolygonF poly;
            for (int j = 0; j < sn; ++j) {         // top edge, left→right
                int idx = m - sn + j;
                double base = (idx >= 0 && idx < m) ? cum[idx] : 0.0;
                poly << QPointF(fracToX(idxFrac(idx)),
                                plot.bottom() - (base + sp->hist[j]) / yMax * plot.height());
            }
            for (int j = sn - 1; j >= 0; --j) {    // bottom edge, right→left
                int idx = m - sn + j;
                double base = (idx >= 0 && idx < m) ? cum[idx] : 0.0;
                poly << QPointF(fracToX(idxFrac(idx)), plot.bottom() - base / yMax * plot.height());
            }
            QColor fillc = lineColor(sp->addr); fillc.setAlpha(140);
            p.setPen(QPen(lineColor(sp->addr), 1.0));
            p.setBrush(fillc);
            p.drawPolygon(poly);
            for (int j = 0; j < sn; ++j) { int idx = m - sn + j; if (idx >= 0 && idx < m) cum[idx] += sp->hist[j]; }
        }
        p.setBrush(Qt::NoBrush);
    } else {
        for (auto& s : series_) {
            if (s.hist.empty()) continue;
            p.setPen(QPen(lineColor(s.addr), 1.5));
            QPolygonF poly;
            int n = static_cast<int>(s.hist.size());
            for (int j = 0; j < n; ++j) {
                int idx = m - n + j;
                poly << QPointF(fracToX(idxFrac(idx)),
                                plot.bottom() - double(s.hist[j]) / yMax * plot.height());
            }
            p.drawPolyline(poly);
        }
    }
    p.restore();

    // Legend (ranked by CPU-time share), coloured by function; rows clickable.
    std::vector<const Series*> ranked;
    for (auto& s : series_) ranked.push_back(&s);
    std::sort(ranked.begin(), ranked.end(),
              [](const Series* a, const Series* b) { return a->pctAll > b->pctAll; });
    legendRows_.clear();
    int lx = width() - legendW - 4, ly = marginT;
    p.setPen(QColor(200, 220, 255));
    p.drawText(lx, ly, QString::fromUtf8(aggregate_ ? "Подпрограмма   %CPU  ×вызовы"
                                                    : "Инструкция (функция)   %CPU  ×вызовы"));
    ly += lineH + 2;
    for (const Series* s : ranked) {
        if (ly > height() - 6) break;
        uint16_t fe = funcOf(s->addr);
        QColor c = lineColor(s->addr);
        p.fillRect(lx, ly - fm.ascent() + 2, 10, 10, c);
        bk::DisasmLine d = bk::disasm(mem, s->addr);
        char fbuf[8]; std::snprintf(fbuf, sizeof(fbuf), "%06o", fe);
        char abuf[8]; std::snprintf(abuf, sizeof(abuf), "%06o", s->addr);
        p.setPen(QColor(225, 228, 235));
        QString right = QString("%1%  ×%2")
                            .arg(s->pctAll, 0, 'f', s->pctAll >= 10.0 ? 0 : 1)
                            .arg(fmtCount(s->calls));
        int rw = fm.horizontalAdvance(right);
        int rightX = width() - 6 - rw;
        int tx = lx + 16;
        // In aggregate mode a row IS a subroutine; otherwise show instr + its func.
        QString left = aggregate_
            ? QString("%1  %2").arg(abuf).arg(QString::fromStdString(d.text))
            : QString("%1 %2  ф%3").arg(abuf).arg(QString::fromStdString(d.text)).arg(fe ? fbuf : "—");
        p.drawText(tx, ly, fm.elidedText(left, Qt::ElideRight, std::max(10, rightX - 6 - tx)));
        p.drawText(rightX, ly, right);
        legendRows_.push_back({QRect(lx, ly - fm.ascent(), width() - 6 - lx, lineH), s->addr});
        ly += lineH;
    }
    if (series_.empty()) {
        p.setPen(QColor(150, 160, 180));
        p.drawText(plot, Qt::AlignCenter, "Нет данных — запустите программу");
    }
}

void HotChartWidget::mousePressEvent(QMouseEvent* e) {
    for (const auto& r : legendRows_) {
        if (!r.first.contains(e->pos())) continue;
        if (e->button() == Qt::RightButton) {
            // Hide this instruction from the chart (e.g. a busy delay loop that
            // swamps the ranking). Drop its line now; Del clears the hidden set.
            excluded_.insert(r.second);
            series_.erase(std::remove_if(series_.begin(), series_.end(),
                [&](const Series& s) { return s.addr == r.second; }), series_.end());
            update();
        } else {
            emit addressPicked(r.second);
        }
        return;
    }
    if (e->button() == Qt::LeftButton && plotRect_.contains(e->pos())) {
        dragging_ = true; lastDrag_ = e->pos();   // pan the time axis
        return;
    }
    QWidget::mousePressEvent(e);
}

void HotChartWidget::keyPressEvent(QKeyEvent* e) {
    switch (e->key()) {
    case Qt::Key_Plus: case Qt::Key_Equal:
        if (topN_ < 30) ++topN_;
        update(); return;
    case Qt::Key_Minus: case Qt::Key_Underscore:
        if (topN_ > 1) --topN_;
        update(); return;
    case Qt::Key_Delete:
        excluded_.clear();
        update(); return;
    case Qt::Key_F:
        showFrames_ = !showFrames_;
        update(); return;
    case Qt::Key_G:                              // aggregate instructions ↔ subroutines
        aggregate_ = !aggregate_;
        series_.clear();                         // keys change meaning; rebuild lines
        excluded_.clear();                       // hidden keys aren't comparable across modes
        update(); return;
    case Qt::Key_S:                              // lines ↔ stacked area
        stacked_ = !stacked_;
        update(); return;
    case Qt::Key_0: case Qt::Key_Home:           // reset the time-axis zoom
        viewL_ = 0.0; viewR_ = 1.0;
        update(); return;
    default: QWidget::keyPressEvent(e);
    }
}

void HotChartWidget::wheelEvent(QWheelEvent* e) {
    const QPoint pos = e->position().toPoint();
    // Over the plot: zoom the time axis around the cursor. Elsewhere (the legend):
    // keep the old behaviour of changing the number of shown series.
    if (!plotRect_.contains(pos)) {
        int dy = e->angleDelta().y();
        if (dy > 0 && topN_ < 30) ++topN_;
        else if (dy < 0 && topN_ > 1) --topN_;
        update();
        return;
    }
    double f = double(pos.x() - plotRect_.left()) / std::max(1, plotRect_.width());
    double histF = viewL_ + f * (viewR_ - viewL_);
    double factor = e->angleDelta().y() > 0 ? 1.0 / 1.25 : 1.25;   // scroll up = zoom in
    double w = std::clamp((viewR_ - viewL_) * factor, 0.02, 1.0);
    viewL_ = histF - f * w;
    viewR_ = viewL_ + w;
    if (viewL_ < 0) { viewL_ = 0; viewR_ = w; }
    if (viewR_ > 1) { viewR_ = 1; viewL_ = 1.0 - w; }
    update();
}

void HotChartWidget::mouseMoveEvent(QMouseEvent* e) {
    if (!dragging_) return;
    double w = viewR_ - viewL_;
    double dxFrac = double(e->pos().x() - lastDrag_.x()) / std::max(1, plotRect_.width()) * w;
    viewL_ -= dxFrac; viewR_ -= dxFrac;
    if (viewL_ < 0) { viewR_ -= viewL_; viewL_ = 0; }
    if (viewR_ > 1) { viewL_ -= (viewR_ - 1); viewR_ = 1; }
    lastDrag_ = e->pos();
    update();
}

void HotChartWidget::mouseReleaseEvent(QMouseEvent*) { dragging_ = false; }
