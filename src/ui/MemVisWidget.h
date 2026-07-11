#pragma once
#include <QWidget>
#include <cstdint>

namespace bk { class Board; }
class QComboBox;
class QCheckBox;
class QSpinBox;

// Draws a region of BK memory as an image at 1/4/8 bits-per-pixel, in monochrome
// or colour, with an optional access "heatmap" overlay (recently written bytes
// glow red, recently read bytes glow green, fading over time).
class MemCanvas : public QWidget {
    Q_OBJECT
public:
    explicit MemCanvas(bk::Board* board, QWidget* parent = nullptr);
    int bpp = 4;
    bool color = true;
    bool heatmap = true;
    int startAddr = 0;
    int bytesPerRow = 64;
protected:
    void paintEvent(QPaintEvent*) override;
private:
    bk::Board* board_;
};

class MemVisWidget : public QWidget {
    Q_OBJECT
public:
    explicit MemVisWidget(bk::Board* board, QWidget* parent = nullptr);
    void refresh();
private:
    MemCanvas* canvas_;
};
