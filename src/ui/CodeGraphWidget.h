#pragma once
#include <QWidget>
#include <cstdint>

namespace bk { class Board; }

// Visualises executed code: a 256x256 "heat map" of the address space coloured
// by execution count (hot spots glow), a ranked list of the hottest
// instructions, and branch/call edges drawn as arcs between hot points.
class CodeGraphWidget : public QWidget {
    Q_OBJECT
public:
    explicit CodeGraphWidget(bk::Board* board, QWidget* parent = nullptr);
    void refresh() { update(); }
protected:
    void paintEvent(QPaintEvent*) override;
private:
    bk::Board* board_;
};
