#pragma once
#include <QWidget>
#include <cstdint>
#include <functional>

// Экранная клавиатура БК: плёночная БК-0010 (с ГРАФ, ЗАП, СТИР, УСТ ТАБ, СБР ТАБ, ГТ,
// диагональными стрелками, «НР») или кнопочная БК-0010-01 («АР2»). Обе — одна матрица
// ВП1-014, коды позиций сняты с её нетлиста; модификаторы работают как выводы
// микросхемы: СУ (nCTRL) — буква & 037, ПР (nSHIFT) — цифры и знаки на 020, НР/АР2 (EC2)
// — тот же код через вектор 0274; ЗАГЛ/СТР — фиксация регистра букв.
class BkKeyboardWidget : public QWidget {
    Q_OBJECT
public:
    enum class Kind : uint8_t { Membrane, Buttons };
    explicit BkKeyboardWidget(QWidget* parent = nullptr);
    void setKind(Kind k);
    Kind kind() const { return kind_; }
    // Регистр РУС/ЛАТ, в котором сейчас машина (его знает раскладка клавиатуры хоста).
    std::function<bool()> isRussian;

    QSize sizeHint() const override { return {1024, 401}; }
    bool hasHeightForWidth() const override { return true; }
    int heightForWidth(int w) const override { return w * 401 / 1024; }

    struct Key;

signals:
    void keyDown(uint16_t code);    // код БК; бит 0200 — вектор 0274
    void keyUp();
    void stopPressed();             // «СТОП» — мимо контроллера клавиатуры
    void registerSwitched(bool russian);

protected:
    void paintEvent(QPaintEvent* e) override;
    void mousePressEvent(QMouseEvent* e) override;
    void mouseReleaseEvent(QMouseEvent* e) override;

private:
    int keyAt(const QPointF& p) const;
    uint16_t translate(const Key& k) const;
    QRectF keyRect(const Key& k) const;      // в координатах виджета

    Kind kind_ = Kind::Buttons;
    const Key* keys_ = nullptr;
    int nKeys_ = 0;
    int pressed_ = -1;
    bool ctrl_ = false, ar2_ = false, shift_ = false;   // залипают до следующей клавиши
    bool caps_ = true;                                   // ЗАГЛ (иначе СТР)
};
