#pragma once
#include <QWidget>
#include <cstdint>

// Общее у окон профилировщика (горячий путь, граф вызовов, пламенный граф и хронология,
// горячие инструкции во времени): обновление раз в кадр, щелчок по адресу ведёт
// дизассемблер отладчика туда, наведение подсвечивает тот же адрес во всех окнах сразу.
class ProfilerView : public QWidget {
    Q_OBJECT
public:
    using QWidget::QWidget;
    virtual void refresh() = 0;
    // Подсветка адреса, наведённого в соседнем окне (-1 — снять).
    void setHighlight(int addr) { if (link_ != addr) { link_ = addr; update(); } }

signals:
    void addressPicked(uint16_t addr);
    void hoverAddress(int addr);   // адрес под курсором (-1 — нет)

protected:
    // Под курсором теперь addr (-1 — ничего): подсветить у себя и сообщить соседям.
    void hoverTo(int addr) {
        if (addr == hoverEmit_) return;
        hoverEmit_ = addr;
        setHighlight(addr);
        emit hoverAddress(addr);
    }
    void leaveHover() { hoverTo(-1); setHighlight(-1); }

    int link_ = -1;        // подсвеченный адрес (-1 — нет)

private:
    int hoverEmit_ = -2;   // последний сообщённый соседям адрес
};
