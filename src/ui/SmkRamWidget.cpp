#include "SmkRamWidget.h"
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
    const int np = std::clamp(p, 0, static_cast<int>(Smk512::PAGES) - 1);
    if (np != page_) cancelEdit();   // набранное относилось к прежней странице
    page_ = np;
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

    auto X = [&](int col) { return xOf(col); };
    const int xWord = COL_WORD, xText = COL_TEXT, xAddr = COL_ADDR, xAcc = COL_ACC;

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

        QString text;
        for (int w = 0; w < 8; ++w) {
            const size_t o = pageBase + static_cast<size_t>(off) + w * 2;
            const QString v = oct6(static_cast<unsigned>(ram[o] | (ram[o + 1] << 8)));
            const int x = X(xWord + w * 7);
            if (editRow_ == top_ + i && editWord_ == w) {
                // Правка на месте — как в оверлее отладчика: рамка, прежнее
                // значение приглушённым, пока ничего не набрано.
                const QRect r(x - 1, y - QFontMetrics(mono_).ascent(),
                              cw_ * 7 + 1, lineH_);
                p.fillRect(r, QColor(230, 200, 40, 50));
                p.setPen(QColor(255, 220, 80));
                p.drawRect(r);
                if (editBuf_.isEmpty()) {
                    p.setPen(QColor(150, 150, 150));
                    p.drawText(x, y, v);
                    p.setPen(QColor(255, 240, 120));
                    p.drawText(x + cw_ * v.size(), y, "_");
                } else {
                    p.setPen(QColor(255, 240, 120));
                    p.drawText(x, y, editBuf_ + "_");
                }
            } else {
                p.setPen(fg);
                p.drawText(x, y, v);
            }
        }
        for (int b = 0; b < BYTES_PER_ROW; ++b) {
            const uint8_t c = ram[pageBase + off + b];
            text += (c >= 040 && c < 0177) ? QChar(c) : QChar('.');
        }
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

// ---- Правка слова на месте -------------------------------------------------
//
// Пишем ПРЯМО в ДОЗУ платы, а не через Memory::poke по адресу БК. Так правится
// и неподключённая страница, и «хвост» сегмента, для которого в текущем режиме
// окна нет вовсе — а это ровно то, ради чего окно и заведено. Ограничения
// «только чтение / только запись» отладчику, как и везде, не помеха.

void SmkRamView::beginEdit(int row, int word) {
    editRow_ = row;
    editWord_ = word;
    editBuf_.clear();
    setFocus(Qt::MouseFocusReason);
    update();
}

void SmkRamView::cancelEdit() {
    editRow_ = -1;
    editBuf_.clear();
    update();
}

void SmkRamView::commitEdit(bool advance) {
    if (editRow_ < 0) return;
    const int row = editRow_, word = editWord_;
    if (!editBuf_.isEmpty() && board_->smk512()) {
        bool ok = false;
        const uint16_t v = static_cast<uint16_t>(editBuf_.toUInt(&ok, 8) & 0xFFFF);
        if (ok) {
            std::vector<uint8_t>& ram = board_->smk().ram();
            const size_t o = static_cast<size_t>(page_) * Smk512::PAGE_BYTES
                           + static_cast<size_t>(row) * BYTES_PER_ROW + word * 2;
            if (o + 1 < ram.size()) {
                ram[o]     = static_cast<uint8_t>(v & 0xff);
                ram[o + 1] = static_cast<uint8_t>(v >> 8);
                emit edited(page_, static_cast<int>(row * BYTES_PER_ROW + word * 2), v);
            }
        }
    }
    editRow_ = -1;
    editBuf_.clear();
    if (advance) {   // ввод таблицы подряд: Enter переводит на следующее слово
        int nr = row, nw = word + 1;
        if (nw > 7) { nw = 0; ++nr; }
        if (nr < ROWS) {
            if (nr >= top_ + visibleRows()) setTop(nr - visibleRows() + 1);
            beginEdit(nr, nw);
            return;
        }
    }
    update();
}

void SmkRamView::mousePressEvent(QMouseEvent* e) {
    if (editing()) { commitEdit(false); return; }   // клик в стороне применяет набранное
    if (e->button() != Qt::LeftButton || !board_->smk512()) return;
    const int r = (e->position().y() - lineH_ - 2) / lineH_;   // минус строка заголовка
    const int chars = (e->position().x() - xOf(0)) / cw_;
    const int w = (chars - COL_WORD) / 7;
    if (r < 0 || top_ + r >= ROWS) return;
    if (chars < COL_WORD || w < 0 || w > 7) return;
    beginEdit(top_ + r, w);
}

void SmkRamView::keyPressEvent(QKeyEvent* e) {
    if (editing()) {
        switch (e->key()) {
        case Qt::Key_Escape:    cancelEdit(); return;
        case Qt::Key_Return:
        case Qt::Key_Enter:
        case Qt::Key_Tab:       commitEdit(true); return;
        case Qt::Key_Backspace: if (!editBuf_.isEmpty()) editBuf_.chop(1); update(); return;
        default: break;
        }
        const QString t = e->text();
        if (t.size() == 1 && t[0] >= '0' && t[0] <= '7') {   // значения БК — восьмеричные
            if (editBuf_.size() < 6) editBuf_ += t;
            update();
        }
        return;   // пока идёт правка, прочие клавиши никуда не уходят
    }
    switch (e->key()) {
    case Qt::Key_Up:       setTop(top_ - 1); return;
    case Qt::Key_Down:     setTop(top_ + 1); return;
    case Qt::Key_PageUp:   setTop(top_ - visibleRows()); return;
    case Qt::Key_PageDown: setTop(top_ + visibleRows()); return;
    case Qt::Key_Home:     setTop(0); return;
    case Qt::Key_End:      setTop(ROWS); return;
    default: QWidget::keyPressEvent(e);
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
    hint_ = new QLabel(QString::fromUtf8(
        "ЛКМ по слову — правка: цифры 0-7, Enter — применить и перейти к следующему, "
        "Esc — отмена. Пишется прямо в ДОЗУ платы, мимо ограничений режима."));
    hint_->setEnabled(false);

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
    connect(view_, &SmkRamView::edited, this, [this](int page, int off, uint16_t v) {
        hint_->setText(QString::fromUtf8("записано: страница %1, смещение %2 = %3")
                           .arg(page).arg(oct6(static_cast<unsigned>(off))).arg(oct6(v)));
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
    lay->addWidget(hint_);

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
    if (board_->smk512() && follow_->isChecked() && !view_->editing()
        && view_->page() != board_->smk().page()) {
        view_->setPage(board_->smk().page());
        QSignalBlocker b(pageBox_);
        pageBox_->setCurrentIndex(view_->page());
    }
    syncBar();
    updateStatus();
    if (++tick_ >= 3) { tick_ = 0; view_->update(); }
}
