#pragma once
#include <QWidget>
#include <QFont>
#include <cstdint>

namespace bk { class Board; }
class QScrollBar;
class QComboBox;
class QCheckBox;
class QLabel;

// Просмотр ДОЗУ блока расширения СМК-512: 16 страниц по 32 Кбайт, в каждой
// 8 сегментов по 010000 байт.
//
// Зачем отдельное окно, если есть дамп памяти в отладчике. Обычный дамп ходит по
// 16-разрядным адресам БК и потому показывает ровно то, что видит процессор:
// одну подключённую страницу, да и то не целиком. Через окно 0100000..0177777 в
// каждом режиме видна только часть сегментов (Табл. 1), а верхние 512 байт
// сегмента не читаются НИ В ОДНОМ режиме — они подключаются на страницу
// регистров только по записи. Здесь же ДОЗУ показана как она есть: все 512
// Кбайт, включая неподключённые страницы и теневые «хвосты».
class SmkRamView : public QWidget {
    Q_OBJECT
public:
    explicit SmkRamView(bk::Board* board, QWidget* parent = nullptr);

    static constexpr int BYTES_PER_ROW = 16;
    static constexpr int ROWS = 32768 / BYTES_PER_ROW;   // строк в странице

    void setPage(int p);
    int  page() const { return page_; }
    void setTop(int row);
    int  top() const { return top_; }
    int  visibleRows() const;
    // Идёт правка слова: пока она не кончилась, окно не переключает страницу
    // само (иначе набранное значение ушло бы в чужую страницу).
    bool editing() const { return editRow_ >= 0; }

signals:
    void topChanged(int row);
    void edited(int page, int offset, uint16_t value);

protected:
    void paintEvent(QPaintEvent*) override;
    void wheelEvent(QWheelEvent*) override;
    void resizeEvent(QResizeEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
    void keyPressEvent(QKeyEvent*) override;

private:
    // Колонки в знакоместах: нужны и рисованию, и попаданию мышью, поэтому
    // считаются в одном месте.
    enum : int {
        COL_OFF  = 0,                            // смещение, 6 знакомест
        COL_WORD = 14,                           // восемь слов по 7 знакомест
        COL_TEXT = COL_WORD + 8 * 7 + 2,
        COL_ADDR = COL_TEXT + BYTES_PER_ROW + 3,
        COL_ACC  = COL_ADDR + 8,
    };
    int xOf(int col) const { return 6 + col * cw_; }

    void beginEdit(int row, int word);
    void commitEdit(bool advance);
    void cancelEdit();

    bk::Board* board_;
    QFont mono_;
    int lineH_ = 14, cw_ = 8;
    int page_ = 0, top_ = 0;
    int editRow_ = -1, editWord_ = 0;   // строка (абсолютная) и слово 0..7
    QString editBuf_;                   // набранные восьмеричные цифры
};

// Окно: выбор страницы, переход к сегменту, слежение за подключённой страницей.
class SmkRamWidget : public QWidget {
    Q_OBJECT
public:
    explicit SmkRamWidget(bk::Board* board, QWidget* parent = nullptr);
    void refresh();          // вызывается из главного цикла, троттлится внутри

private:
    void syncBar();
    void updateStatus();

    bk::Board*  board_;
    SmkRamView* view_;
    QScrollBar* bar_;
    QComboBox*  pageBox_;
    QComboBox*  segBox_;
    QCheckBox*  follow_;
    QLabel*     status_;
    QLabel*     hint_;      // подсказка по правке; сюда же — что записали последним
    int tick_ = 0;
};
