#pragma once
#include <QWidget>
#include <cstdint>
#include <functional>

namespace bk { class Board; }
class QComboBox;
class QCheckBox;
class QSpinBox;
class QPainter;

// Draws a region of BK memory as an image at 1/2/4/8/16 bits-per-pixel, in monochrome
// or colour, with an optional access "heatmap" overlay (recently written bytes
// glow red, recently read bytes glow green, fading over time).
class MemCanvas : public QWidget {
    Q_OBJECT
public:
    explicit MemCanvas(bk::Board* board, QWidget* parent = nullptr);
    int bpp = 2;   // по умолчанию — палитра БК, 2 бита на точку
    bool color = true;
    bool heatmap = true;
    bool hideRom = true;    // hide the ROM region (0100000..0177777); RAM only by default
    bool hideScreen = false; // blank out the video RAM region (0040000..0077777)
    int startAddr = 0;
    int bytesPerRow = 64;
    // Что показываем: память по адресам процессора (как было), ДОЗУ СМК-512 (все 16
    // страниц подряд или одну) либо ОЗУ БК-0011М (все 8 страниц, с hideRom = false
    // ещё и ПЗУ-слоты, или одну страницу). Тепловая карта в этих режимах есть только
    // у того, что ПОДКЛЮЧЕНО сейчас: Trace знает лишь 16-разрядные адреса БК, а
    // неподключённой страницы на шине нет вовсе.
    enum class Src { Bk, SmkAll, SmkPage, Bk11All, Bk11Page };
    Src src = Src::Bk;
    int page = 0;       // для SmkPage / Bk11Page
protected:
    void paintEvent(QPaintEvent*) override;
private:
    void drawMarks11(QPainter& p, const int* romLin, int imgH);
    bk::Board* board_;
};

class MemVisWidget : public QWidget {
    Q_OBJECT
public:
    explicit MemVisWidget(bk::Board* board, QWidget* parent = nullptr);
    void refresh();
private:
    void syncSources();     // список источников — под текущую машину и плату
    bk::Board* board_;
    MemCanvas* canvas_;
    std::function<void()> apply_;
    QComboBox* src_;
    int srcMachine_ = -1;   // для чего собран список: bk11 | smk512 << 1
    int refreshTick_ = 0;   // throttles the heatmap repaint to ~16 Hz
};
