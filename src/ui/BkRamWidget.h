#pragma once
#include <memory>
#include "HexPageView.h"

namespace bk { class Board; }
class QComboBox;

// Память самой БК по страницам — по образцу окна «ДОЗУ СМК-512 по страницам».
// БК-0011М: восемь страниц ОЗУ по 16 Кбайт (код страницы в 0177716 не двоичный —
// он показан рядом) и вставленные ПЗУ (БОС, расширение, три части Бейсика, МСТД) по
// 8 Кбайт; у каждой строки — адрес(а) БК, по которым она видна сейчас: страница 0
// всегда в 000000, выбранные — в окнах 040000 и 0100000, ПЗУ — в своих окнах.
// Обычный дамп отладчика видит только подключённое; здесь — всё, правка слов идёт
// прямо в ОЗУ страницы (ПЗУ — только просмотр). БК-0010: память не страничная, окно
// делит её на ОЗУ 000000–037777, экран 040000–077777 и ПЗУ.
class BkRamSource;

class BkRamWidget : public HexPageWindow {
    Q_OBJECT
public:
    explicit BkRamWidget(bk::Board* board, QWidget* parent = nullptr);
    void refresh();          // вызывается из главного цикла, троттлится внутри

protected:
    void updateStatus() override;
    void pagePicked() override;

private:
    void rebuild();          // список страниц — по машине и вставленным ПЗУ
    void followPage();
    QStringList titles() const;

    bk::Board*   board_;
    BkRamSource* bsrc_;      // источник окна (принадлежит HexPageWindow)
    QComboBox*   follow_;
    QString      layout_;    // какой список страниц построен (машина + ПЗУ)
};
