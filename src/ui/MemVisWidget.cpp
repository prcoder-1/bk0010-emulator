#include "MemVisWidget.h"
#include <algorithm>
#include <cmath>
#include <vector>
#include "Board.h"
#include <QPainter>
#include <QImage>
#include <QComboBox>
#include <QCheckBox>
#include <QSpinBox>
#include <QLabel>
#include <QVBoxLayout>
#include <QHBoxLayout>

using bk::Board;

// A small 16-entry colour map for 4-bpp colour mode / 8-bpp colour buckets.
static QRgb colorMap(int v /*0..15*/) {
    static const QRgb c[16] = {
        qRgb(0,0,0),     qRgb(0,0,170),   qRgb(0,170,0),   qRgb(0,170,170),
        qRgb(170,0,0),   qRgb(170,0,170), qRgb(170,85,0),  qRgb(170,170,170),
        qRgb(85,85,85),  qRgb(85,85,255), qRgb(85,255,85), qRgb(85,255,255),
        qRgb(255,85,85), qRgb(255,85,255),qRgb(255,255,85),qRgb(255,255,255),
    };
    return c[v & 15];
}

MemCanvas::MemCanvas(Board* board, QWidget* parent)
    : QWidget(parent), board_(board) { setMinimumSize(512, 400); }

void MemCanvas::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.fillRect(rect(), Qt::black);

    const bk::Memory& mem = board_->memory();
    bk::Trace& tr = board_->trace();
    tr.setEnabled(true); // ensure data is being collected while the view is open

    const bool word16 = (bpp == 16);        // 16 bpp: one pixel per 16-bit word (RGB565)
    int pxPerByte = (bpp == 1) ? 8 : (bpp == 4 ? 2 : 1);
    int imgW = word16 ? (bytesPerRow / 2) : (bytesPerRow * pxPerByte);
    if (imgW <= 0) return;

    // Build the sequence of *visible* byte addresses, compacting out the hidden
    // regions (ROM and/or video RAM). Hidden memory takes up no space in the view:
    // the remaining bytes are laid out contiguously and the image is scaled to
    // fill the whole window. So with the screen hidden, ROM (if shown) moves up to
    // occupy that space instead of leaving a blank gap.
    const int scrStart = 0040000, romStart = 0100000;
    std::vector<uint16_t> vis;
    vis.reserve(static_cast<size_t>(0x10000 - startAddr));
    int scrBoundary = -1, romBoundary = -1; // compacted index where each region begins
    for (int a = startAddr; a < 0x10000; ++a) {
        uint16_t a16 = static_cast<uint16_t>(a);
        if (hideRom && mem.isRom(a16)) continue;
        if (hideScreen && a16 >= scrStart && a16 < romStart) continue;
        if (a16 == scrStart) scrBoundary = static_cast<int>(vis.size());
        if (a16 == romStart) romBoundary = static_cast<int>(vis.size());
        vis.push_back(a16);
    }
    if (vis.empty()) return;

    int rows = (static_cast<int>(vis.size()) + bytesPerRow - 1) / bytesPerRow;
    if (rows < 1) rows = 1;
    int imgH = rows;
    QImage img(imgW, imgH, QImage::Format_RGB32);
    img.fill(qRgb(0, 0, 0)); // trailing bytes of the last partial row stay black

    // Access-recency helpers (heatmap). The dim "recently touched" brightness glow
    // lingers (long FADE, sqrt curve); the coloured flash fades faster and sharper
    // so the green/red/blue access trail clears quickly. heatAt() fills the four
    // intensities for one byte; tint() applies them to a base colour.
    const int FADE = 150;      // brightness glow: frames (~3 s)
    const int FADE_COLOR = 30; // colour flash: frames (~0.6 s)
    auto heatAt = [&](uint16_t a16, double& hr, double& hw, double& he, double& act) {
        hr = hw = he = act = 0.0;
        if (!heatmap) return;
        auto flash = [&](uint32_t t) { double f = tr.fade(t, FADE_COLOR); return f * f; };
        hr = flash(tr.lastRead(a16));
        hw = flash(tr.lastWrite(a16));
        he = flash(tr.lastExec(a16));
        // A fetch also registers a read; show code as execution (blue), not a
        // data-read (green).
        if (tr.lastExec(a16) != 0 && tr.lastExec(a16) >= tr.lastRead(a16)) hr = 0;
        act = std::max({std::sqrt(tr.fade(tr.lastExec(a16),  FADE)),
                        std::sqrt(tr.fade(tr.lastRead(a16),   FADE)),
                        std::sqrt(tr.fade(tr.lastWrite(a16),  FADE))});
    };
    auto tint = [&](QRgb base, double hr, double hw, double he, double act) -> QRgb {
        if (!heatmap) return base;
        double bright = 0.5 + 0.5 * act;   // idle memory dimmer so touched bytes stand out
        int R = int(qRed(base)   * bright);
        int G = int(qGreen(base) * bright);
        int B = int(qBlue(base)  * bright);
        R = qMin(255, R + int(235 * hw));  // write -> red
        G = qMin(255, G + int(235 * hr));  // read  -> green
        B = qMin(255, B + int(235 * he));  // exec  -> blue
        return qRgb(R, G, B);
    };

    for (int idx = 0; idx < static_cast<int>(vis.size()); ++idx) {
        int row = idx / bytesPerRow, bx = idx % bytesPerRow;
        uint16_t a16 = vis[idx];
        double hr, hw, he, act;

        if (word16) {                       // one pixel per 16-bit word
            if (bx & 1) continue;           // odd byte handled with its even partner
            if (bx / 2 >= imgW) continue;   // dangling byte of an odd-width row
            heatAt(a16, hr, hw, he, act);
            uint8_t lo = mem.peekByte(a16), hi = 0;
            if (bx + 1 < bytesPerRow && idx + 1 < static_cast<int>(vis.size())) {
                uint16_t a2 = vis[idx + 1];
                hi = mem.peekByte(a2);
                double hr2, hw2, he2, act2; // pixel heat = the hotter of the word's two bytes
                heatAt(a2, hr2, hw2, he2, act2);
                hr = std::max(hr, hr2); hw = std::max(hw, hw2);
                he = std::max(he, he2); act = std::max(act, act2);
            }
            uint16_t word = static_cast<uint16_t>(lo | (hi << 8));
            QRgb base = color
                ? qRgb(((word >> 11) & 0x1F) << 3, ((word >> 5) & 0x3F) << 2, (word & 0x1F) << 3)
                : qRgb(word >> 8, word >> 8, word >> 8);   // Ч/Б: high byte as intensity
            img.setPixel(bx / 2, row, tint(base, hr, hw, he, act));
            continue;
        }

        heatAt(a16, hr, hw, he, act);
        uint8_t byte = mem.peekByte(a16);
        int px = bx * pxPerByte;
        if (bpp == 1) {
            for (int b = 0; b < 8; ++b) {
                int v = (byte >> b) & 1;
                QRgb base = color ? colorMap(v ? 15 : 0) : (v ? qRgb(220,220,220) : qRgb(0,0,0));
                img.setPixel(px + b, row, tint(base, hr, hw, he, act));
            }
        } else if (bpp == 4) {
            for (int n = 0; n < 2; ++n) {
                int v = (byte >> (n * 4)) & 15;
                QRgb base = color ? colorMap(v) : qRgb(v * 17, v * 17, v * 17);
                img.setPixel(px + n, row, tint(base, hr, hw, he, act));
            }
        } else { // 8 bpp
            QRgb base = color ? colorMap(byte >> 4) : qRgb(byte, byte, byte);
            img.setPixel(px, row, tint(base, hr, hw, he, act));
        }
    }

    // Scale to fill the widget width, preserve aspect, nearest-neighbour.
    QImage scaled = img.scaled(width(), height(), Qt::IgnoreAspectRatio, Qt::FastTransformation);
    p.drawImage(0, 0, scaled);

    // Mark the region boundaries at their compacted positions: video RAM start
    // (blue) and ROM start (orange). Skipped regions have no boundary to draw.
    auto boundary = [&](int idx, QColor c) {
        if (idx <= 0) return;
        double y = double(idx) / bytesPerRow / imgH * height();
        if (y >= 0 && y < height()) { p.setPen(c); p.drawLine(0, int(y), width(), int(y)); }
    };
    boundary(scrBoundary, QColor(80, 160, 255, 120));  // 0040000 video RAM
    boundary(romBoundary, QColor(255, 160, 80, 120));  // 0100000 ROM
}

MemVisWidget::MemVisWidget(Board* board, QWidget* parent) : QWidget(parent) {
    setWindowTitle("Визуализация памяти БК-0010");
    canvas_ = new MemCanvas(board, this);

    auto* bpp = new QComboBox; bpp->addItems({"1 бит", "4 бита", "8 бит", "16 бит"}); bpp->setCurrentIndex(1);
    auto* mode = new QComboBox; mode->addItems({"Ч/Б", "Цвет"}); mode->setCurrentIndex(1);
    auto* heat = new QCheckBox("Тепловая карта"); heat->setChecked(true);
    auto* showRom = new QCheckBox("Показать ПЗУ"); // ROM hidden by default
    auto* showScreen = new QCheckBox("Показать экран"); showScreen->setChecked(true); // video RAM shown by default
    auto* addr = new QSpinBox; addr->setRange(0, 0xFFFF); addr->setDisplayIntegerBase(8);
    addr->setPrefix("адрес 0"); addr->setSingleStep(0100); addr->setValue(0);
    auto* row = new QSpinBox; row->setRange(8, 512); row->setValue(64); row->setPrefix("шир ");

    auto apply = [=] {
        static const int bppVals[4] = {1, 4, 8, 16};
        canvas_->bpp = bppVals[std::clamp(bpp->currentIndex(), 0, 3)];
        canvas_->color = (mode->currentIndex() == 1);
        canvas_->heatmap = heat->isChecked();
        canvas_->hideRom = !showRom->isChecked();
        canvas_->hideScreen = !showScreen->isChecked();
        canvas_->startAddr = addr->value();
        canvas_->bytesPerRow = row->value();
        canvas_->update();
    };
    connect(bpp,  QOverload<int>::of(&QComboBox::currentIndexChanged), this, [=](int){ apply(); });
    connect(mode, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [=](int){ apply(); });
    connect(heat, &QCheckBox::toggled, this, [=](bool){ apply(); });
    connect(showRom, &QCheckBox::toggled, this, [=](bool){ apply(); });
    connect(showScreen, &QCheckBox::toggled, this, [=](bool){ apply(); });
    connect(addr, QOverload<int>::of(&QSpinBox::valueChanged), this, [=](int){ apply(); });
    connect(row,  QOverload<int>::of(&QSpinBox::valueChanged), this, [=](int){ apply(); });

    auto* controls = new QHBoxLayout;
    controls->addWidget(new QLabel("бит/пиксель:")); controls->addWidget(bpp);
    controls->addWidget(mode); controls->addWidget(heat); controls->addWidget(showRom);
    controls->addWidget(showScreen);
    controls->addWidget(addr); controls->addWidget(row);
    controls->addStretch();

    auto* lay = new QVBoxLayout(this);
    lay->addLayout(controls);
    lay->addWidget(canvas_, 1);
    apply();
    resize(560, 640);
}

// Throttle the periodic repaint to ~16 Hz. Rebuilding the ~48 KB heatmap image
// every 50 Hz tick is costly and steals time from the emulation on this same GUI
// thread; the access-age fade is slow enough that 16 Hz looks identical.
// Interactive control changes call canvas_->update() directly and are unaffected.
void MemVisWidget::refresh() { if (++refreshTick_ >= 3) { refreshTick_ = 0; canvas_->update(); } }
