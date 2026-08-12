#include "SmkRamWidget.h"
#include "Board.h"
#include <QPainter>
#include <QFontDatabase>
#include <QWheelEvent>
#include <QScrollBar>
#include <QComboBox>
#include <QCheckBox>
#include <QLabel>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <algorithm>

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

SmkRamView::SmkRamView(Board* board, QWidget* parent) : QWidget(parent), board_(board) {
    mono_ = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    mono_.setPixelSize(13);
    QFontMetrics fm(mono_);
    lineH_ = fm.height() + 1;
    cw_ = fm.horizontalAdvance('0');
    setMinimumSize(cw_ * 96, lineH_ * 8);
    setFocusPolicy(Qt::WheelFocus);
}

int SmkRamView::visibleRows() const {
    return std::max(1, (height() - lineH_ - 4) / lineH_);   // минус строка заголовка
}

void SmkRamView::setPage(int p) {
    page_ = std::clamp(p, 0, static_cast<int>(Smk512::PAGES) - 1);
    update();
}

void SmkRamView::setTop(int row) {
    const int maxTop = std::max(0, ROWS - visibleRows());
    row = std::clamp(row, 0, maxTop);
    if (row == top_) return;
    top_ = row;
    emit topChanged(top_);
    update();
}

void SmkRamView::resizeEvent(QResizeEvent*) { setTop(top_); }   // подтянуть к новому дну

void SmkRamView::wheelEvent(QWheelEvent* e) {
    const int lines = e->angleDelta().y() / 40;
    if (lines) { setTop(top_ - lines); e->accept(); } else e->ignore();
}

void SmkRamView::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.fillRect(rect(), QColor(8, 16, 40));
    p.setFont(mono_);

    const QColor fg(210, 220, 255), dim(120, 140, 190), hdr(120, 200, 255);
    const QColor segCol(255, 180, 60), liveCol(120, 230, 160);

    if (!board_->smk512()) {
        p.setPen(dim);
        p.drawText(rect(), Qt::AlignCenter,
                   QString::fromUtf8("Блок расширения памяти СМК-512 не установлен.\n"
                                     "Меню «Эмуляция» → «Блок расширения памяти СМК-512»"));
        return;
    }
    const Smk512& smk = board_->smk();
    const std::vector<uint8_t>& ram = smk.ram();
    if (ram.size() < Smk512::RAM_BYTES) return;

    int segBase[Smk512::SEGS];
    const bool livePage = (page_ == smk.page());
    if (livePage) buildSegBases(smk, segBase); else for (int& b : segBase) b = -1;

    // Колонки в знакоместах: смещение, восемь слов, текст, адрес БК, доступ.
    const int xOff  = 6;
    const int xWord = xOff + 8;
    const int xText = xWord + 8 * 7 + 2;
    const int xAddr = xText + BYTES_PER_ROW + 3;
    const int xAcc  = xAddr + 8;
    auto X = [&](int col) { return 6 + col * cw_; };

    int y = lineH_;
    p.setPen(hdr);
    p.drawText(X(0),     y, QString::fromUtf8("смещ."));
    p.drawText(X(xWord), y, QString::fromUtf8("слова"));
    p.drawText(X(xText), y, QString::fromUtf8("текст"));
    p.drawText(X(xAddr), y, QString::fromUtf8(livePage ? "адрес БК" : "не подкл."));
    y += lineH_;

    const int rows = visibleRows();
    const size_t pageBase = static_cast<size_t>(page_) * Smk512::PAGE_BYTES;
    for (int i = 0; i < rows && top_ + i < ROWS; ++i, y += lineH_) {
        const int off = (top_ + i) * BYTES_PER_ROW;         // смещение внутри страницы
        const int seg = off / static_cast<int>(Smk512::SEG_BYTES);
        const int inSeg = off % static_cast<int>(Smk512::SEG_BYTES);

        if (inSeg == 0) {   // граница сегмента — отчеркнуть и подписать
            p.setPen(QColor(255, 180, 60, 90));
            p.drawLine(4, y - lineH_ + 3, width() - 4, y - lineH_ + 3);
            p.setPen(segCol);
            p.drawText(X(xAcc), y, QString::fromUtf8("сегм %1").arg(seg));
        }

        p.setPen(dim);
        p.drawText(X(0), y, oct6(static_cast<unsigned>(off)));

        QString words, text;
        for (int w = 0; w < 8; ++w) {
            const size_t o = pageBase + static_cast<size_t>(off) + w * 2;
            words += oct6(static_cast<unsigned>(ram[o] | (ram[o + 1] << 8))) + " ";
        }
        for (int b = 0; b < BYTES_PER_ROW; ++b) {
            const uint8_t c = ram[pageBase + off + b];
            text += (c >= 040 && c < 0177) ? QChar(c) : QChar('.');
        }
        p.setPen(fg);
        p.drawText(X(xWord), y, words);
        p.setPen(dim);
        p.drawText(X(xText), y, text);

        // Где эта строка лежит в адресном пространстве БК прямо сейчас. Видно
        // только на подключённой странице и только там, где окно есть: «хвосты»
        // сегментов (смещения 07000..07777 вне режимов Hlt и All) не видны нигде.
        if (segBase[seg] >= 0) {
            const uint16_t a = static_cast<uint16_t>(segBase[seg] + inSeg);
            Smk512::Slot s;
            if (smk.decode(a, s) && s.seg == seg && s.cell != Smk512::Cell::Rom) {
                p.setPen(liveCol);
                p.drawText(X(xAddr), y, oct6(a));
                if (inSeg != 0) {   // подпись доступа не спорит с подписью сегмента
                    p.setPen(dim);
                    p.drawText(X(xAcc), y, QString::fromUtf8(accessName(s.cell)));
                }
            }
        }
    }
}

// ---------------------------------------------------------------------------

SmkRamWidget::SmkRamWidget(Board* board, QWidget* parent)
    : QWidget(parent), board_(board) {
    setWindowTitle("ДОЗУ СМК-512 по страницам");
    view_ = new SmkRamView(board, this);
    bar_ = new QScrollBar(Qt::Vertical, this);
    bar_->setRange(0, SmkRamView::ROWS - 1);
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

    connect(pageBox_, QOverload<int>::of(&QComboBox::activated), this, [this](int i) {
        follow_->setChecked(false);     // ручной выбор отменяет слежение
        view_->setPage(i);
        updateStatus();
    });
    connect(segBox_, QOverload<int>::of(&QComboBox::activated), this, [this](int i) {
        if (i <= 0) return;
        view_->setTop((i - 1) * static_cast<int>(Smk512::SEG_BYTES) / SmkRamView::BYTES_PER_ROW);
        syncBar();
        segBox_->setCurrentIndex(0);
    });
    connect(bar_, &QScrollBar::valueChanged, this, [this](int v) { view_->setTop(v); });
    connect(view_, &SmkRamView::topChanged, this, [this](int v) {
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

    // Открываемся сразу на подключённой странице — смотреть чужую пустую
    // страницу 0, пока программа работает с пятнадцатой, смысла нет.
    if (board_->smk512()) {
        view_->setPage(board_->smk().page());
        QSignalBlocker b(pageBox_);
        pageBox_->setCurrentIndex(view_->page());
    }
    syncBar();
    updateStatus();
    resize(900, 620);
}

void SmkRamWidget::syncBar() {
    QSignalBlocker b(bar_);
    bar_->setRange(0, std::max(0, SmkRamView::ROWS - view_->visibleRows()));
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
    if (board_->smk512() && follow_->isChecked() && view_->page() != board_->smk().page()) {
        view_->setPage(board_->smk().page());
        QSignalBlocker b(pageBox_);
        pageBox_->setCurrentIndex(view_->page());
    }
    syncBar();
    updateStatus();
    if (++tick_ >= 3) { tick_ = 0; view_->update(); }
}
