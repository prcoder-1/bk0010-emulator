#include "MainWindow.h"
#include "GlScreen.h"
#include "DebuggerOverlay.h"
#include "MemVisWidget.h"
#include "CodeGraphWidget.h"
#include "AudioOut.h"
#include "Disasm.h"
#include <vector>
#include <QMenuBar>
#include <QMenu>
#include <QAction>
#include <QFileDialog>
#include <QMessageBox>
#include <QTimer>
#include <QStatusBar>
#include <QLabel>
#include <QFileInfo>
#include <QKeySequence>
#include <QKeyEvent>

MainWindow::MainWindow(const QString& romDir, QWidget* parent)
    : QMainWindow(parent) {
    board_ = std::make_unique<bk::Board>();
    if (!board_->loadRoms(romDir.toStdString())) {
        QMessageBox::warning(this, "BK-0010",
            QString("Не удалось загрузить ПЗУ из:\n%1\nПоложите monit10.rom (и basic10.rom) в эту папку.")
                .arg(romDir));
    }
    board_->reset();

    screen_ = new GlScreen(this);
    setCentralWidget(screen_);
    // Keyboard goes to the main window; the GL widget must not steal focus.
    screen_->setFocusPolicy(Qt::NoFocus);
    setFocusPolicy(Qt::StrongFocus);

    overlay_ = new DebuggerOverlay(board_.get(), screen_);
    overlay_->setGeometry(screen_->rect());
    overlay_->hide();

    // --- Menus ---
    QMenu* file = menuBar()->addMenu("&Файл");
    file->addAction("&Загрузить .BIN…", this, &MainWindow::openBin, QKeySequence::Open);
    file->addSeparator();
    file->addAction("В&ыход", this, &QWidget::close, QKeySequence::Quit);

    QMenu* emu = menuBar()->addMenu("&Эмуляция");
    emu->addAction("&Сброс", this, &MainWindow::resetMachine, QKeySequence("Ctrl+R"));
    emu->addAction("Режим &цвет/ч-б", this, &MainWindow::toggleColorMode, QKeySequence("F10"));
    emu->addAction("&Отладчик (Soft-ICE)", this, [this]{ setPaused(!paused_); }, QKeySequence("F12"));

    QMenu* dbg = menuBar()->addMenu("&Отладка");
    dbg->addAction("&Визуализация памяти…", this, &MainWindow::openMemVis);
    dbg->addAction("&Граф кода / горячие точки…", this, &MainWindow::openCodeGraph);
    dbg->addSeparator();
    dbg->addAction("&Сохранить состояние…", this, &MainWindow::saveState, QKeySequence("Ctrl+S"));
    dbg->addAction("В&осстановить состояние…", this, &MainWindow::loadState, QKeySequence("Ctrl+L"));

    status_ = new QLabel("Готов", this);
    statusBar()->addWidget(status_);

    // --- 50 Hz emulation timer ---
    timer_ = new QTimer(this);
    connect(timer_, &QTimer::timeout, this, &MainWindow::onTick);
    timer_->start(20); // ~50 Hz

    // --- Audio (only if built with Qt6 Multimedia) ---
#ifdef HAVE_QT_MULTIMEDIA
    audio_ = new AudioOut(&board_->sound(), this);
    audio_->start();
    emu->addAction("Звук вкл/&выкл", this, [this]{
        static bool muted = false; muted = !muted;
        if (audio_) audio_->setVolume(muted ? 0.f : 0.6f);
        status_->setText(muted ? "Звук выключен" : "Звук включён");
    }, QKeySequence("Ctrl+M"));
#else
    board_->sound().setEnabled(false); // no sink; don't accumulate samples
#endif

    updateTitle();
    resize(1024, 768);
}

MainWindow::~MainWindow() = default;

void MainWindow::onTick() {
    if (!paused_) {
        board_->runFrame();
        if (board_->breakHit()) {          // stopped at a breakpoint
            board_->clearBreakHit();
            setPaused(true);
        }
    }
    renderScreen();
    if (paused_) overlay_->update();
    if (memvis_ && memvis_->isVisible()) memvis_->refresh();
    if (codegraph_ && codegraph_->isVisible()) codegraph_->refresh();
}

void MainWindow::renderScreen() {
    board_->screen().setColorMode(colorMode_);
    board_->screen().render(board_->memory());
    screen_->setFrame(board_->screen().pixels());
}

void MainWindow::setPaused(bool paused) {
    paused_ = paused;
    if (paused_) {
        overlay_->setGeometry(screen_->rect());
        overlay_->followPc();
        overlay_->show();
        overlay_->raise();
        status_->setText("ОТЛАДЧИК: F7 шаг, F8 через, F9 точка, G продолжить, F12 выход");
    } else {
        overlay_->hide();
        status_->setText("Выполнение");
    }
    renderScreen();
}

void MainWindow::stepInto() {
    board_->stepInstruction();
    overlay_->followPc();
    renderScreen();
    overlay_->update();
}

void MainWindow::stepOver() {
    uint16_t pc = board_->cpu().pc();
    bk::DisasmLine d = bk::disasm(board_->memory(), pc);
    uint16_t ir = board_->memory().peekWord(pc);
    int idx = ir >> 6;
    bool isCall = (idx >= 040 && idx <= 047) ||           // JSR
                  (idx >= 01040 && idx <= 01047);         // EMT/TRAP
    if (isCall) {
        uint16_t ret = pc + d.words * 2;
        board_->runUntil(ret, 20'000'000);                // cap to avoid hangs
        board_->clearBreakHit();
    } else {
        board_->stepInstruction();
    }
    overlay_->followPc();
    renderScreen();
    overlay_->update();
}

void MainWindow::resizeEvent(QResizeEvent* e) {
    QMainWindow::resizeEvent(e);
    if (overlay_) overlay_->setGeometry(screen_->rect());
}

void MainWindow::openBin() {
    QString path = QFileDialog::getOpenFileName(this, "Загрузить .BIN",
        lastBin_.isEmpty() ? QString() : lastBin_, "BK images (*.bin *.BIN);;Все файлы (*)");
    if (!path.isEmpty()) loadBinFromPath(path);
}

bool MainWindow::loadBinFromPath(const QString& path) {
    uint16_t addr = 0, len = 0;
    if (!board_->loadBin(path.toStdString(), true, &addr, &len)) {
        QMessageBox::warning(this, "BK-0010", QString("Не удалось загрузить %1").arg(path));
        return false;
    }
    lastBin_ = path;
    status_->setText(QString("Загружено %1: адрес %2, длина %3 (восьм.)")
        .arg(QFileInfo(path).fileName())
        .arg(QString::number(addr, 8))
        .arg(QString::number(len, 8)));
    updateTitle();
    return true;
}

void MainWindow::resetMachine() {
    board_->reset();
    keymap_.reset();
    if (!lastBin_.isEmpty()) loadBinFromPath(lastBin_);
    status_->setText("Сброс");
}

void MainWindow::toggleColorMode() {
    colorMode_ = !colorMode_;
    status_->setText(colorMode_ ? "Режим: 256×256 цвет" : "Режим: 512×256 ч/б");
}

void MainWindow::keyPressEvent(QKeyEvent* e) {
    // F12 toggles the debugger regardless of state.
    if (e->key() == Qt::Key_F12) { setPaused(!paused_); e->accept(); return; }

    if (paused_) {
        // --- Debugger controls (SoftICE overlay is visible) ---
        switch (e->key()) {
        case Qt::Key_F7:       stepInto(); return;
        case Qt::Key_F8:       stepOver(); return;
        case Qt::Key_F9:       board_->toggleBreakpoint(board_->cpu().pc()); overlay_->update(); return;
        case Qt::Key_G:        setPaused(false); return;
        case Qt::Key_PageUp:   overlay_->scrollDisasm(-8); return;
        case Qt::Key_PageDown: overlay_->scrollDisasm(8); return;
        case Qt::Key_BracketLeft:  overlay_->scrollMem(-8); return;
        case Qt::Key_BracketRight: overlay_->scrollMem(8); return;
        default: e->accept(); return; // swallow other keys while debugging
        }
    }

    // --- SoftICE off: feed the BK-0010 keyboard ---
    std::vector<uint16_t> codes = keymap_.translate(e);
    if (!codes.empty()) {
        for (uint16_t c : codes) board_->pressKey(c);
        e->accept();
        return;
    }
    QMainWindow::keyPressEvent(e);
}

void MainWindow::openMemVis() {
    if (!memvis_) memvis_ = new MemVisWidget(board_.get());
    board_->trace().setEnabled(true);
    memvis_->show();
    memvis_->raise();
}

void MainWindow::openCodeGraph() {
    if (!codegraph_) codegraph_ = new CodeGraphWidget(board_.get());
    board_->trace().setEnabled(true);
    codegraph_->show();
    codegraph_->raise();
}

void MainWindow::saveState() {
    QString path = QFileDialog::getSaveFileName(this, "Сохранить состояние", QString(),
        "BK state (*.bkst);;Все файлы (*)");
    if (path.isEmpty()) return;
    if (!path.endsWith(".bkst")) path += ".bkst";
    if (board_->saveState(path.toStdString()))
        status_->setText("Состояние сохранено: " + QFileInfo(path).fileName());
    else
        QMessageBox::warning(this, "BK-0010", "Не удалось сохранить состояние");
}

void MainWindow::loadState() {
    QString path = QFileDialog::getOpenFileName(this, "Восстановить состояние", QString(),
        "BK state (*.bkst);;Все файлы (*)");
    if (path.isEmpty()) return;
    if (board_->loadState(path.toStdString())) {
        status_->setText("Состояние восстановлено: " + QFileInfo(path).fileName());
        renderScreen();
        if (paused_) overlay_->followPc();
    } else {
        QMessageBox::warning(this, "BK-0010", "Не удалось восстановить состояние");
    }
}

void MainWindow::updateTitle() {
    QString t = "БК-0010 эмулятор-отладчик";
    if (!lastBin_.isEmpty()) t += " — " + QFileInfo(lastBin_).fileName();
    setWindowTitle(t);
}
