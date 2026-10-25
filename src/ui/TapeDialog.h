#pragma once
#include <QDialog>

namespace bk { class Board; }
class QTableWidget;
class QLabel;
class QPushButton;
class QTimer;

// Оглавление кассеты в магнитофоне: файлы, найденные декодером, позиция ленты.
// Поставить ленту на файл и сохранить файл как .BIN окно умеет само; загрузить файл
// программой просит MainWindow (смена машины, разметка отладчика, сброс).
class TapeDialog : public QDialog {
    Q_OBJECT
public:
    TapeDialog(bk::Board* board, QWidget* parent = nullptr);
    void refresh();                 // перечитать оглавление (кассету сменили)

signals:
    void loadRequested(int index);  // файл index (с 0) — загрузить программой

protected:
    void showEvent(QShowEvent* e) override;
    void hideEvent(QHideEvent* e) override;

private:
    int selected() const;
    void updatePosition();
    void seekSelected();
    void saveSelected();

    bk::Board* board_;
    QTableWidget* table_ = nullptr;
    QLabel* head_ = nullptr;
    QLabel* pos_ = nullptr;
    QPushButton* load_ = nullptr;
    QPushButton* seek_ = nullptr;
    QPushButton* save_ = nullptr;
    QTimer* timer_ = nullptr;
    const void* shown_ = nullptr;   // какая кассета сейчас в таблице
};
