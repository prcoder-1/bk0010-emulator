#include "AzRamWidget.h"
#include "QtFormat.h"
#include "Board.h"
#include <QComboBox>
#include <QLabel>

using bk::Azbk;
using bk::Board;

namespace {
constexpr int CHUNK = 0200000;             // 64 Кбайт — пункт списка
constexpr int PAGE = 010000;               // страница AZ, 4 Кбайт
constexpr int CHUNKS = Azbk::MEM_BYTES / CHUNK;

QString chunkTitle(int c) {
    const int p0 = c * (CHUNK / PAGE);
    QString what;
    if (p0 < 040) what = QString::fromUtf8(" — тень ОЗУ БК-0011М");
    else if (p0 < 0100) what = QString::fromUtf8(" — служебная (040 — заставка)");
    else if (p0 < 0200) what = QString::fromUtf8(" — слоты ПЗУ");
    else if (p0 < 0400) what = QString::fromUtf8(" — эмуляция СМК-512");
    return QString::fromUtf8("стр. %1–%2%3").arg(p0, 5, 8, QChar('0')).arg(p0 + 15, 5, 8, QChar('0')).arg(what);
}
}

class AzRamSource : public HexPageSource {
public:
    explicit AzRamSource(Board* b) : board_(b) {}
    bool available() const override { return board_->azbk(); }
    QString unavailableText() const override { return QString::fromUtf8("Контроллер AZБК не установлен"); }
    int pageBytes(int) const override { return CHUNK; }
    uint8_t byteAt(int page, int off) const override {
        return board_->azbk() ? board_->az().mem()[size_t(page) * CHUNK + off] : 0;
    }
    bool writable(int) const override { return board_->azbk(); }
    void writeWord(int page, int off, uint16_t v) override {
        board_->az().setMemWord(static_cast<uint32_t>(page) * CHUNK + static_cast<uint32_t>(off), v);
    }
    int sectionBytes(int) const override { return PAGE; }
    QString sectionLabel(int page, int sect) const override {
        return QString::fromUtf8("стр. %1").arg(page * (CHUNK / PAGE) + sect, 5, 8, QChar('0'));
    }
    QString addrHeader(int) const override { return QString::fromUtf8("адрес БК сейчас"); }
    QString addressOf(int page, int off, QString* access) const override {
        if (!board_->azbk()) return QString();
        const int azPage = page * (CHUNK / PAGE) + off / PAGE;
        QStringList addrs, acc;
        for (int w = 0; w < 16; ++w) {
            const Azbk::Window win = board_->az().window(w);
            if (win.page != azPage || !(win.active || win.shadow)) continue;
            addrs << oct6(static_cast<unsigned>(w * PAGE + off % PAGE));
            acc << QString::fromUtf8(win.active ? (win.ro || (azPage >= 0100 && azPage < 0200) ? "A чт" : "A чт/зп") : "S зп");
        }
        if (addrs.isEmpty()) return QString();
        if (access) *access = acc.join(",");
        return addrs.join(",");
    }
private:
    Board* board_;
};

AzRamWidget::AzRamWidget(Board* board, QWidget* parent)
    : HexPageWindow(std::make_unique<AzRamSource>(board),
                    QString::fromUtf8("Пишется прямо в память платы AZБК, мимо окон маппера."), parent),
      board_(board) {
    setWindowTitle("Память AZБК по страницам");
    QStringList t;
    for (int c = 0; c < CHUNKS; ++c) t << chunkTitle(c);
    setPageTitles(t);
    pageBox_->setMinimumContentsLength(40);
    showPage(0);
    resize(980, 620);
}

void AzRamWidget::updateStatus() {
    status_->setText(QString::fromStdString(board_->az().summary()));
}

void AzRamWidget::refresh() {
    syncBar();
    if (repaintDue()) { view_->update(); updateStatus(); }
}
