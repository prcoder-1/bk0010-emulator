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

signals:
    void topChanged(int row);

protected:
    void paintEvent(QPaintEvent*) override;
    void wheelEvent(QWheelEvent*) override;
    void resizeEvent(QResizeEvent*) override;

private:
    bk::Board* board_;
    QFont mono_;
    int lineH_ = 14, cw_ = 8;
    int page_ = 0, top_ = 0;
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
    int tick_ = 0;
};
