#include "MemVisWidget.h"
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

    int pxPerByte = (bpp == 1) ? 8 : (bpp == 4 ? 2 : 1);
    int imgW = bytesPerRow * pxPerByte;
    if (imgW <= 0) return;
    int rows = (0x10000 - startAddr + bytesPerRow - 1) / bytesPerRow;
    int imgH = rows;
    QImage img(imgW, imgH, QImage::Format_RGB32);

    for (int row = 0; row < rows; ++row) {
        for (int bx = 0; bx < bytesPerRow; ++bx) {
            int addr = startAddr + row * bytesPerRow + bx;
            if (addr >= 0x10000) { addr = 0xFFFF; }
            uint8_t byte = mem.peekByte(static_cast<uint16_t>(addr));
            uint8_t hw = heatmap ? tr.heat(static_cast<uint16_t>(addr), true) : 0;
            uint8_t hr = heatmap ? tr.heat(static_cast<uint16_t>(addr), false) : 0;

            auto tint = [&](QRgb base) -> QRgb {
                if (!heatmap || (hw == 0 && hr == 0)) return base;
                int r = qMin(255, qRed(base)   + hw);
                int g = qMin(255, qGreen(base) + (hr > hw ? hr - hw : 0));
                int b = qBlue(base);
                return qRgb(r, g, b);
            };

            int px = bx * pxPerByte;
            if (bpp == 1) {
                for (int b = 0; b < 8; ++b) {
                    int v = (byte >> b) & 1;
                    QRgb base = color ? colorMap(v ? 15 : 0) : (v ? qRgb(220,220,220) : qRgb(0,0,0));
                    img.setPixel(px + b, row, tint(base));
                }
            } else if (bpp == 4) {
                for (int n = 0; n < 2; ++n) {
                    int v = (byte >> (n * 4)) & 15;
                    QRgb base = color ? colorMap(v) : qRgb(v * 17, v * 17, v * 17);
                    img.setPixel(px + n, row, tint(base));
                }
            } else { // 8 bpp
                QRgb base = color ? colorMap(byte >> 4) : qRgb(byte, byte, byte);
                img.setPixel(px, row, tint(base));
            }
        }
    }

    // Scale to fill the widget width, preserve aspect, nearest-neighbour.
    QImage scaled = img.scaled(width(), height(), Qt::IgnoreAspectRatio, Qt::FastTransformation);
    p.drawImage(0, 0, scaled);

    // Highlight the video RAM region boundary (0040000..0100000).
    p.setPen(QColor(80, 160, 255, 120));
    double yv = double(0040000 - startAddr) / (bytesPerRow) / imgH * height();
    if (yv >= 0 && yv < height()) p.drawLine(0, int(yv), width(), int(yv));
}

MemVisWidget::MemVisWidget(Board* board, QWidget* parent) : QWidget(parent) {
    setWindowTitle("Визуализация памяти БК-0010");
    canvas_ = new MemCanvas(board, this);

    auto* bpp = new QComboBox; bpp->addItems({"1 бит", "4 бита", "8 бит"}); bpp->setCurrentIndex(2);
    auto* mode = new QComboBox; mode->addItems({"Ч/Б", "Цвет"});
    auto* heat = new QCheckBox("Тепловая карта"); heat->setChecked(true);
    auto* addr = new QSpinBox; addr->setRange(0, 0xFFFF); addr->setDisplayIntegerBase(8);
    addr->setPrefix("адрес 0"); addr->setSingleStep(0100); addr->setValue(0);
    auto* row = new QSpinBox; row->setRange(8, 512); row->setValue(64); row->setPrefix("шир ");

    auto apply = [=] {
        canvas_->bpp = (bpp->currentIndex() == 0) ? 1 : (bpp->currentIndex() == 1 ? 4 : 8);
        canvas_->color = (mode->currentIndex() == 1);
        canvas_->heatmap = heat->isChecked();
        canvas_->startAddr = addr->value();
        canvas_->bytesPerRow = row->value();
        canvas_->update();
    };
    connect(bpp,  QOverload<int>::of(&QComboBox::currentIndexChanged), this, [=](int){ apply(); });
    connect(mode, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [=](int){ apply(); });
    connect(heat, &QCheckBox::toggled, this, [=](bool){ apply(); });
    connect(addr, QOverload<int>::of(&QSpinBox::valueChanged), this, [=](int){ apply(); });
    connect(row,  QOverload<int>::of(&QSpinBox::valueChanged), this, [=](int){ apply(); });

    auto* controls = new QHBoxLayout;
    controls->addWidget(new QLabel("бит/пиксель:")); controls->addWidget(bpp);
    controls->addWidget(mode); controls->addWidget(heat);
    controls->addWidget(addr); controls->addWidget(row);
    controls->addStretch();

    auto* lay = new QVBoxLayout(this);
    lay->addLayout(controls);
    lay->addWidget(canvas_, 1);
    apply();
    resize(560, 640);
}

void MemVisWidget::refresh() { canvas_->update(); }
