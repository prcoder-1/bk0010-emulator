#include "HotChartWidget.h"
#include <algorithm>
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

    // Subroutine entries = JSR destinations, for colouring lines by function.
    subEntries_.clear();
    for (auto& e : tr.edges())
        if (isJsr(mem.peekWord(e.first >> 16))) subEntries_.insert(e.first & 0xFFFF);

    // Rank all executed instructions by cumulative CPU time (calls × per-instruction
    // ticks) and accumulate the grand total, so lines can be shown as a share of it.
    std::vector<std::pair<uint64_t, uint16_t>> all;
    uint64_t totalTicks = 0;
    for (int a = 0; a < 0x10000; a += 2) {
        uint16_t a16 = static_cast<uint16_t>(a);
        uint32_t c = tr.execCount(a16);
        if (!c) continue;
        uint64_t tk = static_cast<uint64_t>(c) * cpu.instrTicks(mem.peekWord(a16));
        totalTicks += tk;                       // total CPU time still counts hidden code
        if (excluded_.count(a16)) continue;     // but hidden instructions leave the ranking
        all.push_back({tk, a16});
    }
    std::sort(all.rbegin(), all.rend());
    std::set<uint16_t> topSet;
    for (int i = 0; i < topN_ && i < static_cast<int>(all.size()); ++i)
        topSet.insert(all[i].second);

    const uint64_t intervalTotal = totalTicks - lastTotalTicks_;
    lastTotalTicks_ = totalTicks;

    // Drop lines that fell out of the top-N.
    series_.erase(std::remove_if(series_.begin(), series_.end(),
        [&](const Series& s) { return !topSet.count(s.addr); }), series_.end());

    // Add newcomers, back-filling zeros so every line stays the same length.
    size_t len = 0;
    for (auto& s : series_) len = std::max(len, s.hist.size());
    for (uint16_t a : topSet) {
        bool found = false;
        for (auto& s : series_) if (s.addr == a) { found = true; break; }
        if (!found) {
            Series s; s.addr = a; s.last = tr.execCount(a); s.total = tr.execCount(a);
            s.pctAll = 0.0; s.hist.assign(len, 0.0);
            series_.push_back(s);
        }
    }

    // Append this interval's CPU-time share (%) to every line and refresh totals.
    for (auto& s : series_) {
        uint32_t c = tr.execCount(s.addr);
        int tk = cpu.instrTicks(mem.peekWord(s.addr));
        uint64_t dTicks = static_cast<uint64_t>(c - s.last) * tk;
        s.hist.push_back(intervalTotal ? 100.0 * double(dTicks) / double(intervalTotal) : 0.0);
        while (s.hist.size() > HIST) s.hist.pop_front();
        s.last = c;
        s.total = c;
        s.pctAll = totalTicks ? 100.0 * double(static_cast<uint64_t>(c) * tk) / double(totalTicks) : 0.0;
    }
}

void HotChartWidget::refresh() {
    bk::Trace& tr = board_->trace();
    tr.setEnabled(true);
    // Sample on emulated time (frozen while paused), not wall-clock.
    if (tr.now() - lastNow_ >= static_cast<uint32_t>(SAMPLE_FRAMES)) {
        lastNow_ = tr.now();
        sample();
    }
    update();
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
               QString("Топ %1 горячих инструкций — %% времени CPU за интервал (%2 кадров). "
                       "+/- N, колесо N, ПКМ скрыть, Del сброс%3")
                   .arg(topN_).arg(SAMPLE_FRAMES)
                   .arg(excluded_.empty() ? QString() : QString("  (скрыто %1)").arg(excluded_.size())));

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

    // Colour by address: spread the hue across the address range of the shown
    // instructions, so nearby addresses (usually the same routine) get similar
    // colours and far-apart code gets clearly different colours.
    uint16_t aMin = 0xFFFF, aMax = 0;
    for (auto& s : series_) { aMin = std::min(aMin, s.addr); aMax = std::max(aMax, s.addr); }
    const double aSpan = (aMax > aMin) ? double(aMax - aMin) : 1.0;
    auto lineColor = [&](uint16_t a) { return addrColor((a - aMin) / aSpan); };

    // Draw each line. X maps the sample index so the newest sample is at the right.
    const double dx = (HIST > 1) ? double(plot.width()) / (HIST - 1) : 0.0;
    for (auto& s : series_) {
        if (s.hist.empty()) continue;
        QColor c = lineColor(s.addr);
        p.setPen(QPen(c, 1.5));
        QPolygonF poly;
        int n = static_cast<int>(s.hist.size());
        for (int i = 0; i < n; ++i) {
            double x = plot.right() - (n - 1 - i) * dx;
            double y = plot.bottom() - double(s.hist[i]) / yMax * plot.height();
            poly << QPointF(x, y);
        }
        p.drawPolyline(poly);
    }

    // Legend (ranked by CPU-time share), coloured by function; rows clickable.
    std::vector<const Series*> ranked;
    for (auto& s : series_) ranked.push_back(&s);
    std::sort(ranked.begin(), ranked.end(),
              [](const Series* a, const Series* b) { return a->pctAll > b->pctAll; });
    legendRows_.clear();
    int lx = width() - legendW - 4, ly = marginT;
    p.setPen(QColor(200, 220, 255));
    p.drawText(lx, ly, QString("Инструкция (функция)   %CPU  ×вызовы"));
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
        // Right-align the CPU-time share and call count so they are never clipped;
        // elide only the address+disasm+function part on the left if too narrow.
        QString right = QString("%1%  ×%2")
                            .arg(s->pctAll, 0, 'f', s->pctAll >= 10.0 ? 0 : 1)
                            .arg(fmtCount(s->total));
        int rw = fm.horizontalAdvance(right);
        int rightX = width() - 6 - rw;
        int tx = lx + 16;
        QString left = QString("%1 %2  ф%3")
                           .arg(abuf).arg(QString::fromStdString(d.text)).arg(fe ? fbuf : "—");
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
    default: QWidget::keyPressEvent(e);
    }
}

void HotChartWidget::wheelEvent(QWheelEvent* e) {
    int dy = e->angleDelta().y();
    if (dy > 0 && topN_ < 30) ++topN_;
    else if (dy < 0 && topN_ > 1) --topN_;
    update();
}
