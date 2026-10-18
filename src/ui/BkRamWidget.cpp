#include "BkRamWidget.h"
#include "Board.h"
#include <QScrollBar>
#include <QComboBox>
#include <QLabel>
#include <QSignalBlocker>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <algorithm>
#include <vector>

using bk::Board;
using bk::Memory;

namespace {
QString oct6(unsigned v) { return QString("%1").arg(v & 0177777, 6, 8, QChar('0')); }

constexpr int BLOCK = 020000;              // блок карты памяти БК-0011М, 8 Кбайт
}

// Страницы памяти БК для HexPageView. Пишем прямо в ОЗУ страницы, мимо карты адресов:
// так правится и неподключённая страница.
class BkRamSource : public HexPageSource {
public:
    enum class Kind { Ram11, Rom11, Flat };
    struct Page {
        Kind kind;
        int index;                         // страница ОЗУ / слот ПЗУ / адрес начала (Flat)
        int bytes;
        bool rom;
        QString title;
    };

    explicit BkRamSource(Board* b) : board_(b) {}

    void build() {
        pages_.clear();
        const Memory& m = board_->memory();
        if (board_->bk11()) {
            for (int p = 0; p < 8; ++p)
                pages_.push_back({Kind::Ram11, p, int(Memory::PAGE11_BYTES), false, QString()});
            struct R { int slot; const char* name; };
            static const R roms[] = {
                {Memory::ROM_BOS, "ПЗУ БОС (324), 140000"},
                {Memory::ROM_EXT, "ПЗУ расширения БОС (325), ПЗУ-стр. 1"},
                {Memory::ROM_BASIC1, "ПЗУ Бейсик ч. 1 (327), ПЗУ-стр. 1"},
                {Memory::ROM_BASIC2, "ПЗУ Бейсик ч. 2 (328), ПЗУ-стр. 0"},
                {Memory::ROM_BASIC3, "ПЗУ Бейсик ч. 3 (329), ПЗУ-стр. 0"},
                {Memory::ROM_MSTD, "ПЗУ МСТД (330), 160000"},
            };
            for (const R& r : roms)
                if (m.rom11Loaded(r.slot))
                    pages_.push_back({Kind::Rom11, r.slot, int(Memory::ROM11_BYTES), true, QString::fromUtf8(r.name)});
        } else {
            pages_.push_back({Kind::Flat, 0, 040000, false, QString::fromUtf8("ОЗУ 000000–037777")});
            pages_.push_back({Kind::Flat, 040000, 040000, false, QString::fromUtf8("ОЗУ 040000–077777 (экран)")});
            pages_.push_back({Kind::Flat, 0100000, 020000, true, QString::fromUtf8("ПЗУ монитора 100000–117777")});
            pages_.push_back({Kind::Flat, 0120000, 057600, true, QString::fromUtf8("ПЗУ 120000–177577 (Бейсик / МСТД)")});
        }
    }

    const std::vector<Page>& pages() const { return pages_; }

    // Название страницы ОЗУ 11М с тем, где она сейчас: окно 0, окно 1, экран.
    QString title(int i) const {
        const Page& pg = pages_[i];
        if (pg.kind != Kind::Ram11) return pg.title;
        const Memory& m = board_->memory();
        QString s = QString::fromUtf8("стр. %1 (код %2)").arg(pg.index).arg(Memory::codeForPage(pg.index));
        QStringList where;
        if (pg.index == 0) where << QString::fromUtf8("000000");
        if (m.window0Page() == pg.index) where << QString::fromUtf8("окно 0");
        if (m.window1Page() == pg.index) where << QString::fromUtf8("окно 1");
        if (m.videoPage() == pg.index) where << QString::fromUtf8("экран");
        if (!where.isEmpty()) s += " — " + where.join(", ");
        return s;
    }

    bool available() const override { return !pages_.empty(); }
    QString unavailableText() const override { return QString::fromUtf8("Нет памяти"); }
    int pageBytes(int page) const override { return valid(page) ? pages_[page].bytes : 0; }

    uint8_t byteAt(int page, int off) const override {
        const Page& pg = pages_[page];
        const Memory& m = board_->memory();
        switch (pg.kind) {
        case Kind::Ram11: return m.ram11()[static_cast<size_t>(pg.index) * Memory::PAGE11_BYTES + off];
        case Kind::Rom11: { const uint8_t* r = m.rom11(pg.index); return r ? r[off] : 0; }
        case Kind::Flat:  return m.raw()[pg.index + off];
        }
        return 0;
    }
    bool writable(int page) const override { return valid(page) && !pages_[page].rom; }
    void writeWord(int page, int off, uint16_t v) override {
        const Page& pg = pages_[page];
        Memory& m = board_->memory();
        uint8_t* p = pg.kind == Kind::Ram11 ? m.ram11() + static_cast<size_t>(pg.index) * Memory::PAGE11_BYTES + off
                   : pg.kind == Kind::Flat  ? m.raw() + pg.index + off : nullptr;
        if (!p) return;
        p[0] = static_cast<uint8_t>(v & 0xff);
        p[1] = static_cast<uint8_t>(v >> 8);
    }
    int sectionBytes(int page) const override {
        return valid(page) && pages_[page].kind == Kind::Flat ? 020000 : BLOCK;
    }
    QString sectionLabel(int page, int sect) const override {
        if (pages_[page].kind == Kind::Flat) return QString::fromUtf8("блок %1").arg(oct6(pages_[page].index + sect * 020000));
        return QString("+%1").arg(oct6(sect * BLOCK));
    }
    QString addrHeader(int) const override { return QString::fromUtf8("адрес БК сейчас"); }

    QString addressOf(int page, int off, QString* access) const override {
        const Page& pg = pages_[page];
        const Memory& m = board_->memory();
        QStringList addrs;
        auto add = [&](unsigned a) {
            // Выше 0100000 может ответить плата в разъёме МПИ (СМК, ПЗУ КНГМД) — тогда
            // по этому адресу видна не эта память.
            uint16_t dummy;
            if (a >= 0100000 && m.mpi() && m.mpi()->mpiPeek(static_cast<uint16_t>(a), dummy)) return;
            addrs << oct6(a);
        };
        if (pg.kind == Kind::Flat) {
            add(static_cast<unsigned>(pg.index + off));
        } else {
            for (int blk = 0; blk < 8; ++blk) {
                if (pg.kind == Kind::Ram11) {
                    const int base = m.blockRamOffset(blk);
                    const int abs = pg.index * int(Memory::PAGE11_BYTES) + off;
                    if (base >= 0 && abs >= base && abs < base + BLOCK) add(static_cast<unsigned>(blk * BLOCK + abs - base));
                } else if (m.blockRomSlot(blk) == pg.index) {
                    add(static_cast<unsigned>(blk * BLOCK + off));
                }
            }
        }
        if (addrs.isEmpty()) return QString();
        if (access) *access = QString::fromUtf8(pg.rom ? "чт" : "чт/зп");
        return addrs.join(",");
    }

private:
    bool valid(int page) const { return page >= 0 && page < static_cast<int>(pages_.size()); }
    Board* board_;
    std::vector<Page> pages_;
};

BkRamWidget::BkRamWidget(Board* board, QWidget* parent) : QWidget(parent), board_(board) {
    setWindowTitle("Память БК по страницам");
    src_ = std::make_unique<BkRamSource>(board);
    view_ = new HexPageView(src_.get(), this);
    bar_ = new QScrollBar(Qt::Vertical, this);
    bar_->setSingleStep(1);
    pageBox_ = new QComboBox;
    pageBox_->setMinimumContentsLength(34);
    jumpBox_ = new QComboBox;
    follow_ = new QComboBox;
    follow_->addItem(QString::fromUtf8("не следить"));
    follow_->addItem(QString::fromUtf8("следить за окном 0 (040000)"));
    follow_->addItem(QString::fromUtf8("следить за окном 1 (100000)"));
    follow_->addItem(QString::fromUtf8("следить за экранной страницей"));
    follow_->setCurrentIndex(1);    // окно 0 — страница, которую программа чаще всего переключает
    status_ = new QLabel;
    hint_ = new QLabel(QString::fromUtf8(
        "ЛКМ по слову — правка: цифры 0-7, Enter — применить и перейти к следующему, "
        "Esc — отмена. Пишется прямо в ОЗУ страницы, мимо карты адресов; ПЗУ — только просмотр."));
    hint_->setEnabled(false);

    connect(pageBox_, QOverload<int>::of(&QComboBox::activated), this, [this](int i) {
        follow_->setCurrentIndex(0);    // ручной выбор отменяет слежение
        showPage(i);
    });
    connect(follow_, QOverload<int>::of(&QComboBox::activated), this, [this](int) { followPage(); });
    connect(jumpBox_, QOverload<int>::of(&QComboBox::activated), this, [this](int i) {
        if (i <= 0) return;
        view_->setTop((i - 1) * src_->sectionBytes(view_->page()) / HexPageView::BYTES_PER_ROW);
        syncBar();
        jumpBox_->setCurrentIndex(0);
    });
    connect(view_, &HexPageView::edited, this, [this](int page, int off, uint16_t v) {
        hint_->setText(QString::fromUtf8("записано: %1, смещение %2 = %3")
                           .arg(src_->title(page)).arg(oct6(static_cast<unsigned>(off))).arg(oct6(v)));
    });
    connect(bar_, &QScrollBar::valueChanged, this, [this](int v) { view_->setTop(v); });
    connect(view_, &HexPageView::topChanged, this, [this](int v) {
        QSignalBlocker b(bar_);
        bar_->setValue(v);
    });

    auto* controls = new QHBoxLayout;
    controls->addWidget(pageBox_);
    controls->addWidget(jumpBox_);
    controls->addWidget(follow_);
    controls->addStretch(1);
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
    lay->addWidget(status_);
    lay->addLayout(body, 1);
    lay->addWidget(hint_);

    rebuild();
    resize(980, 620);
}

BkRamWidget::~BkRamWidget() = default;

// Список страниц зависит от машины и вставленных ПЗУ: окно может быть открыто, когда
// машину переключают из меню.
void BkRamWidget::rebuild() {
    const Memory& m = board_->memory();
    QString key = board_->bk11() ? "11" : "10";
    for (int s = 0; s < Memory::ROM11_SLOTS; ++s) key += m.rom11Loaded(s) ? '1' : '0';
    if (key == layout_) return;
    layout_ = key;
    src_->build();
    follow_->setVisible(board_->bk11());
    QSignalBlocker b(pageBox_);
    pageBox_->clear();
    for (size_t i = 0; i < src_->pages().size(); ++i) pageBox_->addItem(src_->title(static_cast<int>(i)));
    // 11М — со страницы в окне 0, БК-0010 — с экрана; дальше — по слежению.
    showPage(board_->bk11() ? m.window0Page() : 1);
    followPage();
}

void BkRamWidget::showPage(int i) {
    view_->setPage(i, static_cast<int>(src_->pages().size()));
    {
        QSignalBlocker b(pageBox_);
        pageBox_->setCurrentIndex(view_->page());
    }
    QSignalBlocker b(jumpBox_);
    jumpBox_->clear();
    jumpBox_->addItem(QString::fromUtf8("к началу…"));
    const int sect = src_->sectionBytes(view_->page());
    for (int s = 0; s * sect < src_->pageBytes(view_->page()); ++s)
        jumpBox_->addItem(src_->sectionLabel(view_->page(), s));
    syncBar();
    updateStatus();
}

void BkRamWidget::followPage() {
    if (!board_->bk11() || view_->editing()) return;
    const Memory& m = board_->memory();
    int p = -1;
    switch (follow_->currentIndex()) {
    case 1: p = m.window0Page(); break;
    case 2: p = m.window1Page(); break;     // -1: в окне 1 ПЗУ или оно отключено
    case 3: p = m.videoPage(); break;
    default: break;
    }
    if (p >= 0 && p != view_->page()) showPage(p);   // -1 — следить не за чем, страница та же
    else updateStatus();
}

void BkRamWidget::syncBar() {
    QSignalBlocker b(bar_);
    bar_->setRange(0, std::max(0, view_->rows() - view_->visibleRows()));
    bar_->setPageStep(view_->visibleRows());
    bar_->setValue(view_->top());
}

void BkRamWidget::updateStatus() {
    const Memory& m = board_->memory();
    if (!board_->bk11()) {
        status_->setText(QString::fromUtf8("БК-0010: память не страничная — адреса БК совпадают со смещением"));
        return;
    }
    QString w1;
    if (m.window1Off()) w1 = QString::fromUtf8("отключено (ВЭД СМК-512)");
    else if (m.window1Rom() >= 0) w1 = QString::fromUtf8("ПЗУ-стр. %1").arg(m.window1Rom());
    else w1 = QString::fromUtf8("стр. %1").arg(m.window1Page());
    status_->setText(QString::fromUtf8("0177716 = %1: окно 0 — стр. %2, окно 1 — %3; экран — стр. %4")
                         .arg(oct6(m.pageMap())).arg(m.window0Page()).arg(w1).arg(m.videoPage()));
    // Названия страниц несут «окно 0 / окно 1 / экран» — обновить без смены выбора.
    QSignalBlocker b(pageBox_);
    for (int i = 0; i < pageBox_->count(); ++i) pageBox_->setItemText(i, src_->title(i));
}

// Троттлинг как у остальных окон: дамп на каждом кадре не нужен, а время это отнимает
// у эмуляции — она в том же потоке.
void BkRamWidget::refresh() {
    rebuild();
    if (follow_->currentIndex() > 0) followPage();
    syncBar();
    if (++tick_ >= 3) { tick_ = 0; view_->update(); updateStatus(); }
}
