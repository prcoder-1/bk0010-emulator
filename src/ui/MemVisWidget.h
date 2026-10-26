#pragma once
#include <QWidget>
#include <cstdint>
#include <functional>
#include <unordered_set>
#include <vector>

namespace bk { class Board; }
class QComboBox;
class QCheckBox;
class QSpinBox;
class QLabel;
class QPainter;
class QKeyEvent;

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
    // Рисование мышью по показанной памяти (по умолчанию выключено) — только с зажатым
    // Ctrl: левая кнопка пишет в точки paintValue (значение точки при текущих
    // бит/точку), правая — 0. ПЗУ и регистры не меняются.
    bool paintEnabled = false;
    unsigned paintValue = 3;
    void syncCursor(Qt::KeyboardModifiers mods);
    // Цвет точки со значением v — как её рисует канва (без тепловой карты).
    static QRgb pixelColor(int bpp, bool color, unsigned v);
signals:
    // Мазок окончен: изменено байт; были ли точки, которые писать нельзя (ПЗУ, регистры).
    void strokeDone(int changed, bool refused);
protected:
    void paintEvent(QPaintEvent*) override;
    void mousePressEvent(QMouseEvent* e) override;
    void mouseMoveEvent(QMouseEvent* e) override;
    void mouseReleaseEvent(QMouseEvent* e) override;
private:
    void drawMarks11(QPainter& p, const int* romLin, int imgH);
    // Раскладка последней перерисовки: по ней мышь находит байты под указателем —
    // ровно те, что сейчас показаны в этой точке окна.
    struct Layout {
        enum Kind { None, Bk, Smk, Bk11 } kind = None;
        int from = 0, n = 0, bytesPerRow = 1, pxPerByte = 1, imgW = 1, imgH = 1;
        int Wt = 1, Hd = 1;                 // размер картинки в физических точках
        bool word16 = false;
        std::vector<uint16_t> vis;          // Bk: индекс -> адрес БК
    };
    QPoint toImage(QPointF pos) const;
    void paintLine(QPoint a, QPoint b);
    void paintCell(int x, int y);
    int  readSrc(int idx) const;            // байт показанной памяти; -1 — нет его
    void writeSrc(int idx, uint8_t v);
    bool bkWritable(uint16_t a) const;
    bk::Board* board_;
    Layout lay_;
    bool stroke_ = false;
    unsigned strokeValue_ = 0;
    QPoint lastPx_;
    std::unordered_set<int> strokeBytes_;
    bool strokeRefused_ = false;
};

class MemVisWidget : public QWidget {
    Q_OBJECT
public:
    explicit MemVisWidget(bk::Board* board, QWidget* parent = nullptr);
    void refresh();
protected:
    void keyPressEvent(QKeyEvent* e) override;
    void keyReleaseEvent(QKeyEvent* e) override;
private:
    void syncSources();     // список источников — под текущую машину и плату
    bk::Board* board_;
    MemCanvas* canvas_;
    std::function<void()> apply_;
    QComboBox* src_;
    QLabel* paintHint_;
    int srcMachine_ = -1;   // для чего собран список: bk11 | smk512 << 1
    int refreshTick_ = 0;   // throttles the heatmap repaint to ~16 Hz
};
