#include "MainWindow.h"
#include "GlScreen.h"
#include "DebuggerOverlay.h"
#include "MemVisWidget.h"
#include "CodeGraphWidget.h"
#include "HotChartWidget.h"
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
    emu->addAction("&Пауза", this, [this]{ setSuspended(!suspended_); }, QKeySequence(Qt::Key_Pause));
    emu->addAction("&Отладчик (Soft-ICE)", this, [this]{ setPaused(!paused_); }, QKeySequence("F12"));

    QMenu* dbg = menuBar()->addMenu("&Отладка");
    dbg->addAction("&Визуализация памяти…", this, &MainWindow::openMemVis, QKeySequence("Ctrl+I"));
    dbg->addAction("&Граф кода / горячие точки…", this, &MainWindow::openCodeGraph, QKeySequence("Ctrl+G"));
    dbg->addAction("Горячие инструкции во &времени…", this, &MainWindow::openHotChart, QKeySequence("Ctrl+H"));
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
    if (!paused_ && !suspended_) {
        // Feed one buffered key into the register once it is free, so the BK
        // controller sees a stream of single codes just like real hardware.
        if (!keyFeed_.empty() && !board_->keyReady()) {
            board_->pressKey(keyFeed_.front());
            keyFeed_.pop_front();
        }
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
    if (hotchart_ && hotchart_->isVisible()) hotchart_->refresh();
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
    // Entering/leaving the debugger drops any held keys so a game doesn't see a
    // key stuck down across the pause.
    heldKeys_.clear();
    board_->setKeyHeld(false);
    renderScreen();
}

// Simple pause via the Pause key: freezes the 50 Hz emulation loop without the
// Soft-ICE overlay. The last frame stays on screen; the status bar shows PAUSED.
void MainWindow::setSuspended(bool suspended) {
    suspended_ = suspended;
    // Drop any held/buffered keys so nothing is stuck down or replayed on resume.
    heldKeys_.clear();
    keyFeed_.clear();
    board_->setKeyHeld(false);
    if (!paused_)  // don't fight the debugger for the status line
        status_->setText(suspended_ ? "ПАУЗА — нажмите Pause для продолжения"
                                     : "Выполнение");
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
    // The monitor ROM must have initialised before a game is started; when a bin
    // is passed on the command line this happens before the 50 Hz timer has run
    // a single frame, so boot it here first.
    board_->ensureMonitorBooted();
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
    keyFeed_.clear();
    heldKeys_.clear();
    board_->setKeyHeld(false);
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
    // Pause key toggles the simple emulation freeze.
    if (e->key() == Qt::Key_Pause) { setSuspended(!suspended_); e->accept(); return; }

    // While suspended, swallow all other keys so nothing reaches the game or the
    // key buffer (they'd otherwise be replayed on resume).
    if (suspended_ && !paused_) { e->accept(); return; }

    // Track the physical key-down state (ignoring auto-repeat) so 0177716 bit 6
    // stays low while a key is held — games poll it to detect input.
    if (!paused_ && !e->isAutoRepeat()) {
        heldKeys_.insert(e->key());
        board_->setKeyHeld(true);
    }

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
        for (uint16_t c : codes) {
            // Drop pile-ups from auto-repeat (a duplicate of the last queued code)
            // and cap the buffer, mirroring the fact that the real controller
            // keeps only one pending code.
            if (keyFeed_.size() >= 16) break;
            if (!keyFeed_.empty() && keyFeed_.back() == c) continue;
            keyFeed_.push_back(c);
        }
        e->accept();
        return;
    }
    QMainWindow::keyPressEvent(e);
}

void MainWindow::keyReleaseEvent(QKeyEvent* e) {
    // Release the physical key-held state (0177716 bit 6) when the last game key
    // is lifted. Auto-repeat generates spurious release events — ignore those.
    if (!e->isAutoRepeat()) {
        heldKeys_.erase(e->key());
        if (heldKeys_.empty()) board_->setKeyHeld(false);
    }
    QMainWindow::keyReleaseEvent(e);
}

void MainWindow::openMemVis() {
    if (!memvis_) memvis_ = new MemVisWidget(board_.get());
    board_->trace().setEnabled(true);
    memvis_->show();
    memvis_->raise();
}

void MainWindow::openCodeGraph() {
    if (!codegraph_) {
        codegraph_ = new CodeGraphWidget(board_.get());
        // Clicking a hot instruction jumps the debugger's disassembler to it,
        // opening the debugger overlay if it isn't already showing.
        connect(codegraph_, &CodeGraphWidget::addressPicked, this, [this](uint16_t addr) {
            if (!paused_) setPaused(true);   // setPaused re-anchors disasm to PC…
            overlay_->setDisasmAddr(addr);   // …so set the picked address after it
            overlay_->update();
            // Don't raise/activate the main window: the user is clicking in the
            // code-graph window and it must stay in front so its own reposition to
            // the block is visible. Both panels update in place (side-by-side).
        });
    }
    board_->trace().setEnabled(true);
    codegraph_->show();
    codegraph_->raise();
}

void MainWindow::openHotChart() {
    if (!hotchart_) {
        hotchart_ = new HotChartWidget(board_.get());
        // Clicking a legend row jumps the debugger's disassembler to that address.
        connect(hotchart_, &HotChartWidget::addressPicked, this, [this](uint16_t addr) {
            if (!paused_) setPaused(true);
            overlay_->setDisasmAddr(addr);
            overlay_->update();
        });
    }
    board_->trace().setEnabled(true);
    hotchart_->show();
    hotchart_->raise();
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
