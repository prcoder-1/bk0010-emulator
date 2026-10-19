#pragma once
#include <QWidget>
#include <QFont>
#include <QString>
#include <cstdint>
#include <memory>

class QScrollBar;
class QComboBox;
class QLabel;
class QHBoxLayout;

// Откуда окно «… по страницам» берёт память: страницы ДОЗУ платы СМК-512 или
// страницы ОЗУ/ПЗУ самой БК. Окно показывает память как она есть, мимо карты
// адресов, а рядом — по какому адресу БК строка видна прямо сейчас.
class HexPageSource {
public:
    virtual ~HexPageSource() = default;
    virtual bool available() const = 0;            // false — показать unavailableText()
    virtual QString unavailableText() const = 0;
    virtual int pageBytes(int page) const = 0;
    virtual uint8_t byteAt(int page, int off) const = 0;
    virtual bool writable(int page) const = 0;
    virtual void writeWord(int page, int off, uint16_t v) = 0;
    // Отчерк и подпись каждые sectionBytes байт (сегменты СМК, блоки по 8 Кбайт у БК).
    virtual int sectionBytes(int page) const = 0;
    virtual QString sectionLabel(int page, int section) const = 0;
    virtual QString addrHeader(int page) const = 0;
    // Адрес(а) БК, по которым строка видна сейчас (пусто — нигде), и доступ к ней.
    virtual QString addressOf(int page, int off, QString* access) const = 0;
};

// Дамп одной страницы: смещение, восемь слов, текст, адрес БК. Правка слова на месте:
// ЛКМ по слову, цифры 0–7, Enter — записать и перейти к следующему, Esc — отмена.
class HexPageView : public QWidget {
    Q_OBJECT
public:
    HexPageView(HexPageSource* src, QWidget* parent = nullptr);

    static constexpr int BYTES_PER_ROW = 16;

    void setPage(int p, int pages);
    int  page() const { return page_; }
    int  rows() const;                       // строк в текущей странице
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
        COL_ACC  = COL_ADDR + 15,
    };
    int xOf(int col) const { return 6 + col * cw_; }

    void beginEdit(int row, int word);
    void commitEdit(bool advance);
    void cancelEdit();

    HexPageSource* src_;
    QFont mono_;
    int lineH_ = 14, cw_ = 8;
    int page_ = 0, top_ = 0;
    int editRow_ = -1, editWord_ = 0;   // строка (абсолютная) и слово 0..7
    QString editBuf_;                   // набранные восьмеричные цифры
};

// Окно «… по страницам» вокруг HexPageView: выбор страницы, переход к разделу
// (сегменту, блоку), полоса прокрутки, строка состояния и подсказка по правке. Свои
// органы управления (слежение) подкласс добавляет addControl().
class HexPageWindow : public QWidget {
    Q_OBJECT
public:
    HexPageWindow(std::unique_ptr<HexPageSource> src, const QString& editHint, QWidget* parent = nullptr);
    ~HexPageWindow() override;

protected:
    HexPageSource* source() const { return src_.get(); }
    void addControl(QWidget* w);
    void setPageTitles(const QStringList& titles);   // список страниц, без смены выбора
    void showPage(int page);         // страница в дамп, разделы в список, полоса, статус
    void syncBar();
    bool repaintDue() { if (++tick_ < 3) return false; tick_ = 0; return true; }  // раз в 3 кадра
    virtual void updateStatus() = 0;
    virtual void pagePicked() {}     // страницу выбрали вручную — снять слежение

    std::unique_ptr<HexPageSource> src_;
    HexPageView* view_;
    QScrollBar*  bar_;
    QComboBox*   pageBox_;
    QComboBox*   jumpBox_;
    QLabel*      status_;
    QLabel*      hint_;              // подсказка по правке; сюда же — что записали последним
    QHBoxLayout* controls_;

private:
    int tick_ = 0;
};
