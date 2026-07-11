#include "CodeGraphWidget.h"
#include "Board.h"
#include "Disasm.h"
#include <QPainter>
#include <QImage>
#include <algorithm>
#include <vector>
#include <cmath>

using bk::Board;

CodeGraphWidget::CodeGraphWidget(Board* board, QWidget* parent)
    : QWidget(parent), board_(board) {
    setWindowTitle("Граф кода и горячие точки");
    setMinimumSize(700, 520);
}

// Map an address 0..0xFFFF onto a 256x256 grid: x = word column, y = page row.
static QPoint addrToXY(uint16_t a) { return QPoint((a >> 1) & 0xFF, (a >> 9) & 0x7F); }

void CodeGraphWidget::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.fillRect(rect(), QColor(10, 10, 16));

    bk::Trace& tr = board_->trace();
    tr.setEnabled(true);
    const bk::Memory& mem = board_->memory();

    // ---- Left: code heat map (256 wide x 128 tall grid, log-scaled) ----
    const int GW = 256, GH = 128;
    QImage img(GW, GH, QImage::Format_RGB32);
    img.fill(qRgb(0, 0, 0));
    double lmax = std::log2(double(tr.execMax()) + 1.0) + 1e-6;
    for (int a = 0; a < 0x10000; a += 2) {
        uint32_t c = tr.execCount(static_cast<uint16_t>(a));
        if (!c) continue;
        double v = std::log2(double(c) + 1.0) / lmax; // 0..1
        int r = int(std::min(1.0, v * 1.5) * 255);
        int g = int(std::min(1.0, std::max(0.0, v * 1.5 - 0.5)) * 255);
        int b = int(std::max(0.0, 0.3 - v) * 255);
        QPoint xy = addrToXY(static_cast<uint16_t>(a));
        img.setPixel(xy.x(), xy.y(), qRgb(r, g, b));
    }
    int mapW = width() * 55 / 100, mapH = mapW / 2;
    QRect mapRect(10, 30, mapW, mapH);
    p.setPen(QColor(200, 200, 255));
    p.drawText(10, 22, "Карта исполнения (ярче = горячее). Ось X=слово, Y=страница");
    p.drawImage(mapRect, img);
    p.setPen(QColor(70, 90, 140)); p.drawRect(mapRect);

    // ---- Branch edges among hot points, drawn as arcs over the map ----
    auto mapPt = [&](uint16_t a) {
        QPoint xy = addrToXY(a);
        return QPointF(mapRect.x() + (xy.x() + 0.5) / GW * mapRect.width(),
                       mapRect.y() + (xy.y() + 0.5) / GH * mapRect.height());
    };
    // Take the strongest edges.
    std::vector<std::pair<uint32_t, uint32_t>> edges; // (count, key)
    for (auto& e : tr.edges()) edges.push_back({e.second, e.first});
    std::sort(edges.rbegin(), edges.rend());
    int nEdges = std::min<int>(200, edges.size());
    for (int i = 0; i < nEdges; ++i) {
        uint16_t from = edges[i].second >> 16, to = edges[i].second & 0xFFFF;
        int alpha = 40 + 160 * (nEdges - i) / std::max(1, nEdges);
        p.setPen(QColor(120, 200, 255, alpha));
        p.drawLine(mapPt(from), mapPt(to));
    }

    // ---- Right: ranked hottest instructions ----
    int listX = mapRect.right() + 16;
    p.setPen(QColor(200, 220, 255));
    p.drawText(listX, 22, "Горячие инструкции:");
    // Collect hottest PCs.
    std::vector<std::pair<uint32_t, uint16_t>> hot;
    for (int a = 0; a < 0x10000; a += 2) {
        uint32_t c = tr.execCount(static_cast<uint16_t>(a));
        if (c) hot.push_back({c, static_cast<uint16_t>(a)});
    }
    std::sort(hot.rbegin(), hot.rend());
    QFont mono("monospace"); mono.setStyleHint(QFont::TypeWriter); mono.setPixelSize(12);
    p.setFont(mono);
    int y = 40, n = std::min<int>(28, hot.size());
    uint32_t top = hot.empty() ? 1 : hot[0].first;
    for (int i = 0; i < n; ++i) {
        uint16_t a = hot[i].second;
        bk::DisasmLine d = bk::disasm(mem, a);
        double frac = double(hot[i].first) / top;
        int bar = int(frac * 80);
        p.fillRect(listX, y - 9, bar, 11, QColor(int(120 + 135 * frac), int(60 + 60 * frac), 40));
        p.setPen(QColor(230, 230, 200));
        char buf[16]; std::snprintf(buf, sizeof(buf), "%06o", a);
        p.drawText(listX + 84, y, QString("%1 %2  ×%3")
            .arg(buf).arg(QString::fromStdString(d.text)).arg(hot[i].first));
        y += 15;
        if (y > height() - 10) break;
    }

    // ---- Legend / totals ----
    p.setPen(QColor(150, 170, 210));
    p.drawText(10, mapRect.bottom() + 20,
        QString("Уникальных исполненных адресов: %1, рёбер переходов: %2, макс. счётчик: %3")
            .arg(hot.size()).arg(tr.edges().size()).arg(tr.execMax()));
}
