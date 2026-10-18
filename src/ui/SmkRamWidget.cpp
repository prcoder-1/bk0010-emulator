#include "SmkRamWidget.h"
#include <memory>
#include "Board.h"
#include <QPainter>
#include <QFontDatabase>
#include <QWheelEvent>
#include <QMouseEvent>
#include <QKeyEvent>
#include <QScrollBar>
#include <QComboBox>
#include <QCheckBox>
#include <QLabel>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <algorithm>
#include <vector>

using bk::Board;
using bk::Smk512;

namespace {

QString oct6(unsigned v) { return QString("%1").arg(v & 0177777, 6, 8, QChar('0')); }

// Адрес БК, отвечающий смещению 0 внутри сегмента, для каждого из 8 сегментов
// текущей страницы; -1 — сегмент сейчас не выведен никуда.
//
// Ключ к формуле: блоки 0..6 — это 0100000 + blk*010000, а блоки 7 (0170000..
// 0176777) и 8 (0177000..0177777) вместе покрывают один сегмент целиком, и для
// обоих адрес считается одинаково: 0170000 + смещение. Поэтому одной базы на
// сегмент хватает, а видно ли конкретную ячейку и с каким доступом — спросим у
// самого дешифратора (decode), чтобы не повторять Табл. 1 второй раз.
void buildSegBases(const Smk512& smk, int (&base)[Smk512::SEGS]) {
    static const uint16_t kBlockBase[9] = {0100000, 0110000, 0120000, 0130000,
                                           0140000, 0150000, 0160000, 0170000, 0177000};
    for (int& b : base) b = -1;
    for (int blk = 0; blk < 9; ++blk) {
        Smk512::Slot s;
        if (!smk.decode(kBlockBase[blk], s)) continue;
        if (s.cell == Smk512::Cell::Rom) continue;
        base[s.seg & 7] = (blk <= 6) ? (0100000 + blk * 010000) : 0170000;
    }
}

const char* accessName(Smk512::Cell c) {
    switch (c) {
    case Smk512::Cell::Rw: return "чт/зп";
    case Smk512::Cell::Ro: return "чт";
    case Smk512::Cell::Wo: return "зп";
    default: return "";
    }
}

} // namespace

// ДОЗУ платы как источник для HexPageView. Пишем ПРЯМО в ДОЗУ, а не через
// Memory::poke по адресу БК: так правится и неподключённая страница, и «хвост»
// сегмента, для которого в текущем режиме окна нет вовсе. Ограничения «только
// чтение / только запись» отладчику, как и везде, не помеха.
class SmkRamSource : public HexPageSource {
public:
    explicit SmkRamSource(Board* b) : board_(b) {}
    bool available() const override {
        return board_->smk512() && board_->smk().ram().size() >= Smk512::RAM_BYTES;
    }
    QString unavailableText() const override {
        return QString::fromUtf8("Блок расширения памяти СМК-512 не установлен.\n"
                                 "Меню «Машина» → «Блок расширения памяти СМК-512»");
    }
    int pageBytes(int) const override { return static_cast<int>(Smk512::PAGE_BYTES); }
    uint8_t byteAt(int page, int off) const override {
        return board_->smk().ram()[static_cast<size_t>(page) * Smk512::PAGE_BYTES + off];
    }
    bool writable(int) const override { return true; }
    void writeWord(int page, int off, uint16_t v) override {
        std::vector<uint8_t>& ram = board_->smk().ram();
        const size_t o = static_cast<size_t>(page) * Smk512::PAGE_BYTES + off;
        ram[o] = static_cast<uint8_t>(v & 0xff);
        ram[o + 1] = static_cast<uint8_t>(v >> 8);
    }
    int sectionBytes(int) const override { return static_cast<int>(Smk512::SEG_BYTES); }
    QString sectionLabel(int, int seg) const override { return QString::fromUtf8("сегм %1").arg(seg); }
    QString addrHeader(int page) const override {
        return QString::fromUtf8(page == board_->smk().page() ? "адрес БК" : "не подкл.");
    }
    // Где эта строка лежит в адресном пространстве БК прямо сейчас. Видно только на
    // подключённой странице и только там, где окно есть: «хвосты» сегментов (смещения
    // 07000..07777 вне режимов Hlt и All) не видны нигде.
    QString addressOf(int page, int off, QString* access) const override {
        const Smk512& smk = board_->smk();
        if (page != smk.page()) return QString();
        int segBase[Smk512::SEGS];
        buildSegBases(smk, segBase);
        const int seg = off / static_cast<int>(Smk512::SEG_BYTES);
        const int inSeg = off % static_cast<int>(Smk512::SEG_BYTES);
        if (segBase[seg] < 0) return QString();
        const uint16_t a = static_cast<uint16_t>(segBase[seg] + inSeg);
        Smk512::Slot s;
        if (!smk.decode(a, s) || s.seg != seg || s.cell == Smk512::Cell::Rom) return QString();
        if (access) *access = QString::fromUtf8(accessName(s.cell));
        return oct6(a);
    }

private:
    Board* board_;
};

// ---------------------------------------------------------------------------

SmkRamWidget::SmkRamWidget(Board* board, QWidget* parent)
    : QWidget(parent), board_(board) {
    setWindowTitle("ДОЗУ СМК-512 по страницам");
    src_ = std::make_unique<SmkRamSource>(board);
    view_ = new HexPageView(src_.get(), this);
    bar_ = new QScrollBar(Qt::Vertical, this);
    bar_->setRange(0, 0);              // точный диапазон — в syncBar()
    bar_->setSingleStep(1);

    pageBox_ = new QComboBox;
    for (size_t i = 0; i < Smk512::PAGES; ++i)
        pageBox_->addItem(QString::fromUtf8("страница %1 (код %2)")
                              .arg(i).arg(oct6(Smk512::pageCode(static_cast<int>(i)))));
    segBox_ = new QComboBox;
    segBox_->addItem(QString::fromUtf8("к началу…"));
    for (size_t s = 0; s < Smk512::SEGS; ++s)
        segBox_->addItem(QString::fromUtf8("сегмент %1 (0%2)").arg(s).arg(s * 010000, 0, 8));
    // Слежение включено по умолчанию: чаще всего смотрят как раз ту страницу, с
    // которой программа сейчас работает, а переключается она из-под рук.
    follow_ = new QCheckBox("Следить за подключённой");
    follow_->setChecked(true);
    status_ = new QLabel;
    hint_ = new QLabel(QString::fromUtf8(
        "ЛКМ по слову — правка: цифры 0-7, Enter — применить и перейти к следующему, "
        "Esc — отмена. Пишется прямо в ДОЗУ платы, мимо ограничений режима."));
    hint_->setEnabled(false);

    connect(pageBox_, QOverload<int>::of(&QComboBox::activated), this, [this](int i) {
        follow_->setChecked(false);     // ручной выбор отменяет слежение
        view_->setPage(i, static_cast<int>(Smk512::PAGES));
        updateStatus();
    });
    connect(segBox_, QOverload<int>::of(&QComboBox::activated), this, [this](int i) {
        if (i <= 0) return;
        view_->setTop((i - 1) * static_cast<int>(Smk512::SEG_BYTES) / HexPageView::BYTES_PER_ROW);
        syncBar();
        segBox_->setCurrentIndex(0);
    });
    connect(view_, &HexPageView::edited, this, [this](int page, int off, uint16_t v) {
        hint_->setText(QString::fromUtf8("записано: страница %1, смещение %2 = %3")
                           .arg(page).arg(oct6(static_cast<unsigned>(off))).arg(oct6(v)));
    });
    connect(bar_, &QScrollBar::valueChanged, this, [this](int v) { view_->setTop(v); });
    connect(view_, &HexPageView::topChanged, this, [this](int v) {
        QSignalBlocker b(bar_);
        bar_->setValue(v);
    });

    auto* controls = new QHBoxLayout;
    controls->addWidget(pageBox_);
    controls->addWidget(segBox_);
    controls->addWidget(follow_);
    controls->addWidget(status_, 1);
    controls->setContentsMargins(0, 0, 0, 0);

    auto* body = new QHBoxLayout;
    body->addWidget(view_, 1);
    body->addWidget(bar_);
    body->setContentsMargins(0, 0, 0, 0);
    body->setSpacing(2);

    auto* lay = new QVBoxLayout(this);
    lay->setContentsMargins(4, 4, 4, 4);
    lay->setSpacing(3);
    lay->addLayout(controls);
    lay->addLayout(body, 1);
    lay->addWidget(hint_);

    // Открываемся сразу на подключённой странице — смотреть чужую пустую
    // страницу 0, пока программа работает с пятнадцатой, смысла нет.
    if (board_->smk512()) {
        view_->setPage(board_->smk().page(), static_cast<int>(Smk512::PAGES));
        QSignalBlocker b(pageBox_);
        pageBox_->setCurrentIndex(view_->page());
    }
    syncBar();
    updateStatus();
    resize(900, 620);
}

SmkRamWidget::~SmkRamWidget() = default;

void SmkRamWidget::syncBar() {
    QSignalBlocker b(bar_);
    bar_->setRange(0, std::max(0, view_->rows() - view_->visibleRows()));
    bar_->setPageStep(view_->visibleRows());
    bar_->setValue(view_->top());
}

void SmkRamWidget::updateStatus() {
    if (!board_->smk512()) { status_->setText(QString::fromUtf8("плата не установлена")); return; }
    const Smk512& smk = board_->smk();
    QString s = QString::fromUtf8("режим %1 (%2), подключена страница %3")
                    .arg(QString::fromUtf8(Smk512::modeName(smk.mode())))
                    .arg(oct6(Smk512::modeCode(smk.mode())))
                    .arg(smk.page());
    if (smk.armed()) s += QString::fromUtf8("; строб взведён");
    if (view_->page() != smk.page())
        s += QString::fromUtf8("  •  показана НЕподключённая страница %1").arg(view_->page());
    status_->setText(s);
}

// Троттлинг как у визуализатора памяти: перерисовывать дамп на каждом кадре ни к
// чему, а время это отнимает у эмуляции — она в том же потоке.
void SmkRamWidget::refresh() {
    if (board_->smk512() && follow_->isChecked() && !view_->editing()
        && view_->page() != board_->smk().page()) {
        view_->setPage(board_->smk().page(), static_cast<int>(Smk512::PAGES));
        QSignalBlocker b(pageBox_);
        pageBox_->setCurrentIndex(view_->page());
    }
    syncBar();
    updateStatus();
    if (++tick_ >= 3) { tick_ = 0; view_->update(); }
}
