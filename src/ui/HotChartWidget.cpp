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

// Distinct line colours cycled per function. Entry 0 (no known function) is grey.
static const QColor kPalette[] = {
    QColor( 90, 175, 255), QColor(255, 120,  90), QColor(120, 220, 120),
    QColor(240, 200,  80), QColor(205, 130, 240), QColor( 90, 220, 220),
    QColor(255, 150, 200), QColor(170, 205,  95), QColor(255, 180, 120),
    QColor(140, 165, 255), QColor(220, 220, 150), QColor(120, 240, 185),
};
static constexpr int kPaletteN = int(sizeof(kPalette) / sizeof(kPalette[0]));

HotChartWidget::HotChartWidget(Board* board, QWidget* parent)
    : QWidget(parent), board_(board) {
    setWindowTitle("Горячие инструкции во времени");
    setMinimumSize(640, 360);
    setFocusPolicy(Qt::StrongFocus);
    resize(900, 470);
}

uint16_t HotChartWidget::funcOf(uint16_t addr) const {
    if (subEntries_.empty()) return 0;
    auto it = subEntries_.upper_bound(addr);
    if (it == subEntries_.begin()) return 0;  // before the first known entry
    --it;
    return *it;
}

QColor HotChartWidget::colorForFunc(uint16_t entry) {
    if (entry == 0) return QColor(150, 150, 150);
    auto it = funcColor_.find(entry);
    if (it != funcColor_.end()) return it->second;
    QColor c = kPalette[nextColor_ % kPaletteN];
    ++nextColor_;
    funcColor_[entry] = c;
    return c;
}

void HotChartWidget::sample() {
    bk::Trace& tr = board_->trace();
    const bk::Memory& mem = board_->memory();

    // Subroutine entries = JSR destinations, for colouring lines by function.
    subEntries_.clear();
    for (auto& e : tr.edges())
        if (isJsr(mem.peekWord(e.first >> 16))) subEntries_.insert(e.first & 0xFFFF);

    // Rank all executed instructions and take the current top-N by call count.
    std::vector<std::pair<uint32_t, uint16_t>> all;
    for (int a = 0; a < 0x10000; a += 2) {
        uint32_t c = tr.execCount(static_cast<uint16_t>(a));
        if (c) all.push_back({c, static_cast<uint16_t>(a)});
    }
    std::sort(all.rbegin(), all.rend());
    std::set<uint16_t> topSet;
    for (int i = 0; i < topN_ && i < static_cast<int>(all.size()); ++i)
        topSet.insert(all[i].second);

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
            s.hist.assign(len, 0);
            series_.push_back(s);
        }
    }

    // Append this interval's executions to every line.
    for (auto& s : series_) {
        uint32_t c = tr.execCount(s.addr);
        s.hist.push_back(c - s.last);
        s.last = c;
        s.total = c;
        while (s.hist.size() > HIST) s.hist.pop_front();
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

    const int legendW = 310;
    const int marginL = 46, marginT = 24, marginB = 22, marginR = legendW + 12;
    QRect plot(marginL, marginT, width() - marginL - marginR, height() - marginT - marginB);
    if (plot.width() < 20 || plot.height() < 20) return;

    p.setPen(QColor(200, 220, 255));
    p.drawText(8, fm.ascent() + 3,
               QString("Топ %1 горячих инструкций — исполнений за интервал (%2 кадров). "
                       "+/- меняет N, колесо тоже").arg(topN_).arg(SAMPLE_FRAMES));

    // Peak value across all lines sets the Y scale.
    uint32_t yMax = 1;
    size_t maxLen = 0;
    for (auto& s : series_) {
        maxLen = std::max(maxLen, s.hist.size());
        for (uint32_t v : s.hist) yMax = std::max(yMax, v);
    }

    // Plot frame + gridlines with Y labels.
    p.setPen(QColor(60, 66, 80));
    p.drawRect(plot);
    p.setPen(QColor(120, 130, 150));
    for (int g = 0; g <= 4; ++g) {
        int y = plot.bottom() - g * plot.height() / 4;
        p.setPen(QColor(40, 44, 56));
        if (g) p.drawLine(plot.left(), y, plot.right(), y);
        p.setPen(QColor(120, 130, 150));
        p.drawText(2, y + fm.ascent() / 2, QString::number(uint32_t(yMax) * g / 4));
    }
    p.drawText(plot.left(), height() - 6, QString("← старее   время   сейчас →"));

    // Draw each line. X maps the sample index so the newest sample is at the right.
    const double dx = (HIST > 1) ? double(plot.width()) / (HIST - 1) : 0.0;
    for (auto& s : series_) {
        if (s.hist.empty()) continue;
        QColor c = colorForFunc(funcOf(s.addr));
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

    // Legend (ranked by total call count), coloured by function; rows clickable.
    std::vector<const Series*> ranked;
    for (auto& s : series_) ranked.push_back(&s);
    std::sort(ranked.begin(), ranked.end(),
              [](const Series* a, const Series* b) { return a->total > b->total; });
    legendRows_.clear();
    int lx = width() - legendW - 4, ly = marginT;
    p.setPen(QColor(200, 220, 255));
    p.drawText(lx, ly, QString("Инструкция  (функция)  ×вызовы"));
    ly += lineH + 2;
    for (const Series* s : ranked) {
        if (ly > height() - 6) break;
        uint16_t fe = funcOf(s->addr);
        QColor c = colorForFunc(fe);
        p.fillRect(lx, ly - fm.ascent() + 2, 10, 10, c);
        bk::DisasmLine d = bk::disasm(mem, s->addr);
        char fbuf[8]; std::snprintf(fbuf, sizeof(fbuf), "%06o", fe);
        char abuf[8]; std::snprintf(abuf, sizeof(abuf), "%06o", s->addr);
        p.setPen(QColor(225, 228, 235));
        // Right-align the call count so it is never clipped; elide only the
        // address+disasm+function part on the left if the row is too narrow.
        QString right = QString("×%1").arg(fmtCount(s->total));
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
    for (const auto& r : legendRows_)
        if (r.first.contains(e->pos())) { emit addressPicked(r.second); return; }
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
    default: QWidget::keyPressEvent(e);
    }
}

void HotChartWidget::wheelEvent(QWheelEvent* e) {
    int dy = e->angleDelta().y();
    if (dy > 0 && topN_ < 30) ++topN_;
    else if (dy < 0 && topN_ > 1) --topN_;
    update();
}
