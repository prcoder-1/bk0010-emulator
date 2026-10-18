#include "HexPageView.h"
#include <QPainter>
#include <QFontDatabase>
#include <QWheelEvent>
#include <QMouseEvent>
#include <QKeyEvent>
#include <algorithm>

namespace {
QString oct6(unsigned v) { return QString("%1").arg(v & 0177777, 6, 8, QChar('0')); }
}

HexPageView::HexPageView(HexPageSource* src, QWidget* parent) : QWidget(parent), src_(src) {
    mono_ = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    mono_.setPixelSize(13);
    QFontMetrics fm(mono_);
    lineH_ = fm.height() + 1;
    cw_ = fm.horizontalAdvance('0');
    setMinimumSize(cw_ * 104, lineH_ * 8);
    setFocusPolicy(Qt::WheelFocus);
}

int HexPageView::rows() const {
    return src_->available() ? src_->pageBytes(page_) / BYTES_PER_ROW : 0;
}

int HexPageView::visibleRows() const {
    return std::max(1, (height() - lineH_ - 4) / lineH_);   // минус строка заголовка
}

void HexPageView::setPage(int p, int pages) {
    const int np = std::clamp(p, 0, std::max(0, pages - 1));
    if (np != page_) cancelEdit();   // набранное относилось к прежней странице
    page_ = np;
    setTop(top_);                    // страницы бывают разной длины
    update();
}

void HexPageView::setTop(int row) {
    const int maxTop = std::max(0, rows() - visibleRows());
    row = std::clamp(row, 0, maxTop);
    if (row == top_) return;
    top_ = row;
    emit topChanged(top_);
    update();
}

void HexPageView::resizeEvent(QResizeEvent*) { setTop(top_); }   // подтянуть к новому дну

void HexPageView::wheelEvent(QWheelEvent* e) {
    const int lines = e->angleDelta().y() / 40;
    if (lines) { setTop(top_ - lines); e->accept(); } else e->ignore();
}

void HexPageView::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.fillRect(rect(), QColor(8, 16, 40));
    p.setFont(mono_);

    const QColor fg(210, 220, 255), dim(120, 140, 190), hdr(120, 200, 255);
    const QColor segCol(255, 180, 60), liveCol(120, 230, 160), romCol(190, 190, 150);

    if (!src_->available()) {
        p.setPen(dim);
        p.drawText(rect(), Qt::AlignCenter, src_->unavailableText());
        return;
    }
    const bool rw = src_->writable(page_);
    const int sect = std::max(1, src_->sectionBytes(page_));
    auto X = [&](int col) { return xOf(col); };

    int y = lineH_;
    p.setPen(hdr);
    p.drawText(X(0),        y, QString::fromUtf8("смещ."));
    p.drawText(X(COL_WORD), y, QString::fromUtf8(rw ? "слова" : "слова (ПЗУ — только просмотр)"));
    p.drawText(X(COL_TEXT), y, QString::fromUtf8("текст"));
    p.drawText(X(COL_ADDR), y, src_->addrHeader(page_));
    y += lineH_;

    const int total = rows();
    const int vis = visibleRows();
    for (int i = 0; i < vis && top_ + i < total; ++i, y += lineH_) {
        const int off = (top_ + i) * BYTES_PER_ROW;          // смещение внутри страницы
        const bool sectStart = off % sect == 0;
        if (sectStart) {   // граница сегмента/блока — отчеркнуть и подписать
            p.setPen(QColor(255, 180, 60, 90));
            p.drawLine(4, y - lineH_ + 3, width() - 4, y - lineH_ + 3);
            p.setPen(segCol);
            p.drawText(X(COL_ACC), y, src_->sectionLabel(page_, off / sect));
        }

        p.setPen(dim);
        p.drawText(X(0), y, oct6(static_cast<unsigned>(off)));

        QString text;
        for (int w = 0; w < 8; ++w) {
            const int o = off + w * 2;
            const QString v = oct6(static_cast<unsigned>(src_->byteAt(page_, o) | (src_->byteAt(page_, o + 1) << 8)));
            const int x = X(COL_WORD + w * 7);
            if (editRow_ == top_ + i && editWord_ == w) {
                // Правка на месте — как в оверлее отладчика: рамка, прежнее
                // значение приглушённым, пока ничего не набрано.
                const QRect r(x - 1, y - QFontMetrics(mono_).ascent(), cw_ * 7 + 1, lineH_);
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
                p.setPen(rw ? fg : romCol);
                p.drawText(x, y, v);
            }
        }
        for (int b = 0; b < BYTES_PER_ROW; ++b) {
            const uint8_t c = src_->byteAt(page_, off + b);
            text += (c >= 040 && c < 0177) ? QChar(c) : QChar('.');
        }
        p.setPen(dim);
        p.drawText(X(COL_TEXT), y, text);

        QString access;
        const QString addr = src_->addressOf(page_, off, &access);
        if (!addr.isEmpty()) {
            p.setPen(liveCol);
            p.drawText(X(COL_ADDR), y, addr);
            if (!sectStart && !access.isEmpty()) {   // подпись доступа не спорит с подписью сегмента
                p.setPen(dim);
                p.drawText(X(COL_ACC), y, access);
            }
        }
    }
}

// ---- Правка слова на месте -------------------------------------------------
//
// Пишем ПРЯМО в память источника, а не через Memory::poke по адресу БК: так правится
// и неподключённая страница — ровно то, ради чего окно и заведено.

void HexPageView::beginEdit(int row, int word) {
    editRow_ = row;
    editWord_ = word;
    editBuf_.clear();
    setFocus(Qt::MouseFocusReason);
    update();
}

void HexPageView::cancelEdit() {
    editRow_ = -1;
    editBuf_.clear();
    update();
}

void HexPageView::commitEdit(bool advance) {
    if (editRow_ < 0) return;
    const int row = editRow_, word = editWord_;
    if (!editBuf_.isEmpty() && src_->available() && src_->writable(page_)) {
        bool ok = false;
        const uint16_t v = static_cast<uint16_t>(editBuf_.toUInt(&ok, 8) & 0xFFFF);
        const int off = row * BYTES_PER_ROW + word * 2;
        if (ok && off + 1 < src_->pageBytes(page_)) {
            src_->writeWord(page_, off, v);
            emit edited(page_, off, v);
        }
    }
    editRow_ = -1;
    editBuf_.clear();
    if (advance) {   // ввод таблицы подряд: Enter переводит на следующее слово
        int nr = row, nw = word + 1;
        if (nw > 7) { nw = 0; ++nr; }
        if (nr < rows()) {
            if (nr >= top_ + visibleRows()) setTop(nr - visibleRows() + 1);
            beginEdit(nr, nw);
            return;
        }
    }
    update();
}

void HexPageView::mousePressEvent(QMouseEvent* e) {
    if (editing()) { commitEdit(false); return; }   // клик в стороне применяет набранное
    if (e->button() != Qt::LeftButton || !src_->available() || !src_->writable(page_)) return;
    const int r = (e->position().y() - lineH_ - 2) / lineH_;   // минус строка заголовка
    const int chars = (e->position().x() - xOf(0)) / cw_;
    const int w = (chars - COL_WORD) / 7;
    if (r < 0 || top_ + r >= rows()) return;
    if (chars < COL_WORD || w < 0 || w > 7) return;
    beginEdit(top_ + r, w);
}

void HexPageView::keyPressEvent(QKeyEvent* e) {
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
    case Qt::Key_End:      setTop(rows()); return;
    default: QWidget::keyPressEvent(e);
    }
}
