#include "TapeDialog.h"
#include "Board.h"

#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QTableWidget>
#include <QTimer>
#include <QVBoxLayout>

static QString mmss(double t) {
    const int m = static_cast<int>(t) / 60;
    return QString("%1:%2").arg(m, 2, 10, QChar('0')).arg(t - 60 * m, 4, 'f', 1, QChar('0'));
}

TapeDialog::TapeDialog(bk::Board* board, QWidget* parent) : QDialog(parent), board_(board) {
    setWindowTitle("Оглавление кассеты");
    resize(760, 360);
    head_ = new QLabel(this);
    head_->setWordWrap(true);
    pos_ = new QLabel(this);
    table_ = new QTableWidget(0, 8, this);
    table_->setHorizontalHeaderLabels({"№", "Имя", "Адрес", "Длина", "КС", "Время", "Период", "Запись"});
    table_->verticalHeader()->hide();
    table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    table_->setSelectionMode(QAbstractItemView::SingleSelection);
    table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table_->horizontalHeader()->setStretchLastSection(true);
    load_ = new QPushButton("Загрузить программой", this);
    load_->setToolTip("Сразу, как .BIN: машина, пуск; кассета остаётся за этим файлом");
    seek_ = new QPushButton("Поставить ленту на файл", this);
    seek_->setToolTip("Перемотать к началу пилот-тона файла — для загрузки командой БК");
    save_ = new QPushButton("Сохранить .BIN…", this);
    auto* close = new QPushButton("Закрыть", this);
    auto* buttons = new QHBoxLayout;
    buttons->addWidget(load_);
    buttons->addWidget(seek_);
    buttons->addWidget(save_);
    buttons->addStretch();
    buttons->addWidget(close);
    auto* lay = new QVBoxLayout(this);
    lay->addWidget(head_);
    lay->addWidget(table_);
    lay->addWidget(pos_);
    lay->addLayout(buttons);
    connect(load_, &QPushButton::clicked, this, [this] { if (selected() >= 0) emit loadRequested(selected()); });
    connect(seek_, &QPushButton::clicked, this, &TapeDialog::seekSelected);
    connect(save_, &QPushButton::clicked, this, &TapeDialog::saveSelected);
    connect(table_, &QTableWidget::cellDoubleClicked, this, [this](int row, int) { emit loadRequested(row); });
    connect(close, &QPushButton::clicked, this, &QDialog::close);
    connect(table_, &QTableWidget::itemSelectionChanged, this, [this] {
        const bool any = selected() >= 0;
        load_->setEnabled(any); seek_->setEnabled(any); save_->setEnabled(any);
    });
    timer_ = new QTimer(this);
    connect(timer_, &QTimer::timeout, this, &TapeDialog::updatePosition);
}

void TapeDialog::showEvent(QShowEvent* e) {
    refresh();
    timer_->start(200);
    QDialog::showEvent(e);
}

void TapeDialog::hideEvent(QHideEvent* e) {
    timer_->stop();
    QDialog::hideEvent(e);
}

int TapeDialog::selected() const {
    const auto rows = table_->selectionModel()->selectedRows();
    return rows.isEmpty() ? -1 : rows.first().row();
}

void TapeDialog::refresh() {
    const bk::TapeImage* img = board_->tape().image();
    shown_ = img;
    table_->setRowCount(0);
    if (!img) {
        head_->setText("Магнитофон пуст. Кассету (.wav) вставляет меню «Магнитофон».");
        load_->setEnabled(false); seek_->setEnabled(false); save_->setEnabled(false);
        updatePosition();
        return;
    }
    head_->setText(QString("%1 — %2, %3 Гц. Файлы найдены декодером так, как их читает драйвер ПЗУ; "
                           "звёздочка — программа, которую грузит открытие .wav.")
                       .arg(QFileInfo(QString::fromStdString(img->path)).fileName())
                       .arg(mmss(img->seconds())).arg(img->rate));
    const int main = img->mainProgram();
    table_->setRowCount(static_cast<int>(img->files.size()));
    for (int i = 0; i < static_cast<int>(img->files.size()); ++i) {
        const bk::TapeFile& f = img->files[i];
        QString kind;
        if (f.kind == bk::TapeFile::Kind::MaximTurbo) kind = QString("турбо Maxim, пуск %1").arg(f.entry, 6, 8, QChar('0'));
        else kind = f.maximLoader ? "загрузчик Maxim" : "стандартная";
        if (f.inverted) kind += ", инверсная фаза";
        const QStringList cells = {
            QString("%1%2").arg(i + 1).arg(i == main ? " *" : ""),
            QString::fromStdString(f.name),
            QString("%1").arg(f.addr, 6, 8, QChar('0')),
            QString("%1").arg(f.len, 6, 8, QChar('0')),
            !f.complete ? "оборван" : f.crcOk() ? "верна" : "НЕВЕРНА",
            mmss(img->rate ? double(f.start) / img->rate : 0),
            QString("%1 мкс").arg(f.periodUs, 0, 'f', 0),
            kind};
        for (int c = 0; c < cells.size(); ++c) table_->setItem(i, c, new QTableWidgetItem(cells[c]));
    }
    table_->resizeColumnsToContents();
    if (main >= 0) table_->selectRow(main);
    updatePosition();
}

void TapeDialog::updatePosition() {
    const bk::Tape& tp = board_->tape();
    if (tp.image() != shown_) { refresh(); return; }
    if (!tp.loaded()) { pos_->clear(); return; }
    const uint64_t now = board_->totalTicks();
    const int f = tp.fileAt(tp.position(now));
    pos_->setText(QString("Лента: %1 из %2, %3%4; двигатель %5.%6")
        .arg(mmss(tp.seconds(now))).arg(mmss(tp.image()->seconds()))
        .arg(tp.moving(now) ? "идёт" : "стоит")
        .arg(tp.playing() && tp.autoStarted() ? " (автопуск)" : "")
        .arg(tp.motor() ? "включён" : "выключен")
        .arg(f >= 0 ? QString(" Головка на файле %1 или за ним.").arg(f + 1) : QString()));
}

void TapeDialog::seekSelected() {
    const int i = selected();
    const bk::TapeImage* img = board_->tape().image();
    if (!img || i < 0 || i >= static_cast<int>(img->files.size())) return;
    board_->tape().seek(img->files[i].start, board_->totalTicks());
    updatePosition();
}

void TapeDialog::saveSelected() {
    const int i = selected();
    const bk::TapeImage* img = board_->tape().image();
    if (!img || i < 0 || i >= static_cast<int>(img->files.size())) return;
    const bk::TapeFile& f = img->files[i];
    const QString dir = QFileInfo(QString::fromStdString(img->path)).absolutePath();
    const QString name = QString::fromStdString(bk::tapeHostName(f, i + 1)) + ".BIN";
    const QString path = QFileDialog::getSaveFileName(this, "Сохранить файл кассеты как .BIN",
                                                      dir + "/" + name, "BK .BIN (*.bin *.BIN)");
    if (path.isEmpty()) return;
    if (!bk::saveTapeFileBin(f, path.toStdString()))
        QMessageBox::warning(this, "БК", "Не удалось записать " + path);
}
