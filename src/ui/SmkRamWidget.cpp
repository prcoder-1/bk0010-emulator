#include "SmkRamWidget.h"
#include "QtFormat.h"
#include <memory>
#include "Board.h"
#include <QCheckBox>
#include <QLabel>
#include <algorithm>
#include <vector>

using bk::Board;
using bk::Smk512;

namespace {


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
    : HexPageWindow(std::make_unique<SmkRamSource>(board),
                    QString::fromUtf8("Пишется прямо в ДОЗУ платы, мимо ограничений режима."), parent),
      board_(board) {
    setWindowTitle("ДОЗУ СМК-512 по страницам");
    QStringList titles;
    for (size_t i = 0; i < Smk512::PAGES; ++i)
        titles << QString::fromUtf8("страница %1 (код %2)").arg(i).arg(oct6(Smk512::pageCode(static_cast<int>(i))));
    setPageTitles(titles);
    // Слежение включено по умолчанию: чаще всего смотрят как раз ту страницу, с
    // которой программа сейчас работает, а переключается она из-под рук.
    follow_ = new QCheckBox("Следить за подключённой");
    follow_->setChecked(true);
    addControl(follow_);
    // Открываемся сразу на подключённой странице — смотреть чужую пустую
    // страницу 0, пока программа работает с пятнадцатой, смысла нет.
    showPage(board_->smk512() ? board_->smk().page() : 0);
    resize(900, 620);
}

void SmkRamWidget::pagePicked() { follow_->setChecked(false); }   // ручной выбор отменяет слежение

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
        showPage(board_->smk().page());
        return;
    }
    syncBar();
    updateStatus();
    if (repaintDue()) view_->update();
}
