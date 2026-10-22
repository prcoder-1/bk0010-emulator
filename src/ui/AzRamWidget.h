#pragma once
#include "HexPageView.h"

namespace bk { class Board; }

// Память платы AZБК (32 Мбайт) — по образцу окна «Память БК по страницам». Список —
// куски по 64 Кбайт (16 страниц AZ по 4 Кбайт), страница AZ — раздел дампа. У каждой
// строки — адрес БК, по которому она видна сейчас через окна маппера (A — активное
// окно, S — теневое: запись БК копируется сюда). Правка — прямо в память платы.
class AzRamWidget : public HexPageWindow {
    Q_OBJECT
public:
    explicit AzRamWidget(bk::Board* board, QWidget* parent = nullptr);
    void refresh();

protected:
    void updateStatus() override;

private:
    bk::Board* board_;
};
