#pragma once
#include <QWidget>
#include <memory>
#include "HexPageView.h"

namespace bk { class Board; }
class QScrollBar;
class QComboBox;
class QCheckBox;
class QLabel;

// Просмотр ДОЗУ блока расширения СМК-512: 16 страниц по 32 Кбайт, в каждой
// 8 сегментов по 010000 байт (дамп — HexPageView, память — SmkRamSource).
//
// Зачем отдельное окно, если есть дамп памяти в отладчике. Обычный дамп ходит по
// 16-разрядным адресам БК и потому показывает ровно то, что видит процессор:
// одну подключённую страницу, да и то не целиком. Через окно 0100000..0177777 в
// каждом режиме видна только часть сегментов (Табл. 1), а верхние 512 байт
// сегмента не читаются НИ В ОДНОМ режиме — они подключаются на страницу
// регистров только по записи. Здесь же ДОЗУ показана как она есть: все 512
// Кбайт, включая неподключённые страницы и теневые «хвосты».
class SmkRamSource;

// Окно: выбор страницы, переход к сегменту, слежение за подключённой страницей.
class SmkRamWidget : public QWidget {
    Q_OBJECT
public:
    explicit SmkRamWidget(bk::Board* board, QWidget* parent = nullptr);
    ~SmkRamWidget() override;
    void refresh();          // вызывается из главного цикла, троттлится внутри

private:
    void syncBar();
    void updateStatus();

    bk::Board*  board_;
    std::unique_ptr<SmkRamSource> src_;
    HexPageView* view_;
    QScrollBar* bar_;
    QComboBox*  pageBox_;
    QComboBox*  segBox_;
    QCheckBox*  follow_;
    QLabel*     status_;
    QLabel*     hint_;      // подсказка по правке; сюда же — что записали последним
    int tick_ = 0;
};
