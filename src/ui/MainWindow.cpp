#include "MainWindow.h"
#include "GlScreen.h"
#include "DebuggerOverlay.h"
#include "MemVisWidget.h"
#include "HotPathWidget.h"
#include "CallGraphWidget.h"
#include "FlameWidget.h"
#include "FlameChartWidget.h"
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
#include <QInputDialog>
#include <QLineEdit>
#include <QStringList>
#include <QCloseEvent>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>

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
    dbg->addAction("Горячий &путь…", this, &MainWindow::openHotPath, QKeySequence("Ctrl+G"));
    dbg->addAction("Граф &вызовов…", this, &MainWindow::openCallGraph, QKeySequence("Ctrl+K"));
    dbg->addAction("&Пламенный граф…", this, &MainWindow::openFlame, QKeySequence("Ctrl+F"));
    dbg->addAction("&Хронология вызовов…", this, &MainWindow::openFlameChart, QKeySequence("Ctrl+T"));
    dbg->addAction("Горячие инструкции во &времени…", this, &MainWindow::openHotChart, QKeySequence("Ctrl+H"));
    dbg->addSeparator();
    dbg->addAction("&Сохранить состояние…", this, &MainWindow::saveState, QKeySequence("Ctrl+S"));
    dbg->addAction("В&осстановить состояние…", this, &MainWindow::loadState, QKeySequence("Ctrl+L"));
    dbg->addAction("Загрузить си&мволы (.map)…", this, [this] {
        QString path = QFileDialog::getOpenFileName(this, "Загрузить символы (linker .map)",
            lastBin_.isEmpty() ? QString() : lastBin_,
            "Карты/символы (*.map *.sym *.lst);;Все файлы (*)");
        if (path.isEmpty()) return;
        int n = board_->loadSymbols(path.toStdString());
        status_->setText(n < 0 ? QString("Не удалось открыть файл символов")
                               : QString("Загружено символов: %1").arg(n));
        overlay_->update();
    });
    dbg->addAction("Сохранить &аннотации (.bkdb)", this, [this] {
        QString p = annotationsPath();
        if (p.isEmpty()) p = QFileDialog::getSaveFileName(this, "Сохранить аннотации",
            QString(), "Аннотации БК (*.bkdb)");
        if (p.isEmpty()) return;
        saveAnnotations(p);
        status_->setText("Аннотации сохранены: " + p);
    });
    dbg->addAction("Загрузить а&ннотации (.bkdb)", this, [this] {
        QString p = QFileDialog::getOpenFileName(this, "Загрузить аннотации",
            annotationsPath(), "Аннотации БК (*.bkdb);;Все файлы (*)");
        if (p.isEmpty()) return;
        loadAnnotations(p);
        status_->setText("Аннотации загружены: " + p);
    });

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
    if (hotpath_ && hotpath_->isVisible()) hotpath_->refresh();
    if (callgraph_ && callgraph_->isVisible()) callgraph_->refresh();
    if (flame_ && flame_->isVisible()) flame_->refresh();
    if (flamechart_ && flamechart_->isVisible()) flamechart_->refresh();
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
        overlay_->snapshotRegs();   // baseline for the changed-register highlight
        overlay_->setCursor(board_->cpu().pc());   // selection starts at PC
        overlay_->setGeometry(screen_->rect());
        overlay_->followPc();
        overlay_->show();
        overlay_->raise();
        status_->setText("ОТЛАДЧИК: F7 шаг, F8 через, F9 точка, Esc продолжить, F12 выход");
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
    overlay_->snapshotRegs();   // so registers changed by this step are highlighted
    board_->stepInstruction();
    overlay_->followPc();
    renderScreen();
    overlay_->update();
}

void MainWindow::stepOver() {
    overlay_->snapshotRegs();   // so registers changed by this step are highlighted
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

// ---- Interactive-disassembler annotations (symbols + comments) --------------
static QString octAddr(uint16_t a) { return QString("%1").arg(a, 6, 8, QChar('0')); }

void MainWindow::nameCursorSymbol() {
    if (!paused_) return;
    uint16_t a = overlay_->cursorAddr();
    const std::string* cur = board_->symbols().nameAt(a);
    bool ok = false;
    QString name = QInputDialog::getText(this, "Символ",
        QString("Имя для адреса %1 (пусто — удалить):").arg(octAddr(a)),
        QLineEdit::Normal, cur ? QString::fromStdString(*cur) : QString(), &ok);
    if (!ok) return;
    name = name.trimmed();
    if (name.isEmpty()) board_->symbols().remove(a);
    else board_->symbols().set(a, name.toStdString());
    overlay_->update();
}

void MainWindow::commentCursor() {
    if (!paused_) return;
    uint16_t a = overlay_->cursorAddr();
    const std::string* cur = board_->comment(a);
    bool ok = false;
    QString text = QInputDialog::getText(this, "Комментарий",
        QString("Комментарий к адресу %1 (пусто — удалить):").arg(octAddr(a)),
        QLineEdit::Normal, cur ? QString::fromStdString(*cur) : QString(), &ok);
    if (!ok) return;
    board_->setComment(a, text.trimmed().toStdString());   // empty clears
    overlay_->update();
}

void MainWindow::markData(bk::DataType t) {
    if (!paused_) return;
    uint16_t a = overlay_->cursorAddr();
    uint16_t len = (t == bk::DataType::Word || t == bk::DataType::Ptr) ? 2 : 1;
    if (t == bk::DataType::String) {
        // Auto-length: printable run up to (and including) a terminating NUL, capped.
        len = 0;
        for (int i = 0; i < 80; ++i) {
            uint8_t c = board_->memory().peekByte(a + i);
            if (c == 0) { ++len; break; }
            if (c < 32 || c >= 127) break;
            ++len;
        }
        if (len == 0) len = 1;
    }
    board_->setData(a, t, len);
    overlay_->update();
}

void MainWindow::gotoDialog() {
    if (!paused_) return;
    // Editable picker: the sorted symbol names act as a function list; the field
    // also accepts a typed address (octal by default, or 0x hex).
    QStringList items;
    for (const auto& kv : board_->symbols().all()) items << QString::fromStdString(kv.second);
    items.sort();
    bool ok = false;
    QString sel = QInputDialog::getItem(this, "Перейти",
        "Символ или адрес (восьмер., или 0x…):", items, 0, /*editable*/ true, &ok);
    if (!ok) return;
    sel = sel.trimmed();
    uint16_t addr;
    if (board_->symbols().addrOf(sel.toStdString(), addr)) { overlay_->navigateTo(addr); return; }
    bool num = false; uint parsed = sel.startsWith("0x", Qt::CaseInsensitive)
        ? sel.mid(2).toUInt(&num, 16) : sel.toUInt(&num, 8);
    if (num) overlay_->navigateTo(static_cast<uint16_t>(parsed));
    else status_->setText("Не найдено: " + sel);
}

void MainWindow::xrefsDialog() {
    if (!paused_) return;
    uint16_t t = overlay_->cursorAddr();
    std::vector<uint16_t> refs = overlay_->xrefsTo(t);
    QString what = QString("%1").arg(t, 6, 8, QChar('0'));
    if (const std::string* nm = board_->symbols().nameAt(t)) what += " (" + QString::fromStdString(*nm) + ")";
    if (refs.empty()) { status_->setText("Нет ссылок на " + what); return; }
    QStringList items;
    for (uint16_t a : refs)
        items << QString("%1  %2").arg(a, 6, 8, QChar('0'))
                     .arg(QString::fromStdString(bk::disasm(board_->memory(), a, &board_->symbols()).text));
    bool ok = false;
    QString sel = QInputDialog::getItem(this, "Ссылки на " + what,
        QString("Кто ссылается (%1):").arg(refs.size()), items, 0, /*editable*/ false, &ok);
    if (!ok) return;
    bool num = false; uint a = sel.section(' ', 0, 0).toUInt(&num, 8);
    if (num) overlay_->navigateTo(static_cast<uint16_t>(a));
}

QString MainWindow::annotationsPath() const {
    return lastBin_.isEmpty() ? QString() : lastBin_ + ".bkdb";
}

void MainWindow::saveAnnotations(const QString& path) {
    if (path.isEmpty()) return;
    QJsonArray syms, coms;
    for (const auto& kv : board_->symbols().all()) {
        QJsonObject o; o["a"] = int(kv.first); o["n"] = QString::fromStdString(kv.second);
        syms.append(o);
    }
    for (const auto& kv : board_->comments()) {
        QJsonObject o; o["a"] = int(kv.first); o["c"] = QString::fromStdString(kv.second);
        coms.append(o);
    }
    QJsonArray dat;
    for (const auto& kv : board_->dataItems()) {
        QJsonObject o; o["a"] = int(kv.first); o["t"] = int(kv.second.type); o["l"] = int(kv.second.len);
        dat.append(o);
    }
    if (syms.isEmpty() && coms.isEmpty() && dat.isEmpty()) return;   // nothing worth writing
    QJsonObject root; root["symbols"] = syms; root["comments"] = coms; root["data"] = dat;
    QFile f(path);
    if (f.open(QIODevice::WriteOnly))
        f.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
}

void MainWindow::loadAnnotations(const QString& path) {
    if (path.isEmpty()) return;
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return;
    QJsonObject root = QJsonDocument::fromJson(f.readAll()).object();
    for (const QJsonValue& v : root["symbols"].toArray()) {
        QJsonObject o = v.toObject();
        board_->symbols().set(uint16_t(o["a"].toInt()), o["n"].toString().toStdString());
    }
    for (const QJsonValue& v : root["comments"].toArray()) {
        QJsonObject o = v.toObject();
        board_->setComment(uint16_t(o["a"].toInt()), o["c"].toString().toStdString());
    }
    for (const QJsonValue& v : root["data"].toArray()) {
        QJsonObject o = v.toObject();
        board_->setData(uint16_t(o["a"].toInt()), bk::DataType(o["t"].toInt()), uint16_t(o["l"].toInt()));
    }
    if (overlay_) overlay_->update();
}

void MainWindow::closeEvent(QCloseEvent* e) {
    saveAnnotations(annotationsPath());   // persist symbols/comments next to the .bin
    QMainWindow::closeEvent(e);
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
    loadAnnotations(annotationsPath());   // pick up "<bin>.bkdb" symbols/comments if present
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
        case Qt::Key_Escape:   setPaused(false); return;
        case Qt::Key_PageUp:   overlay_->scrollDisasm(-8); return;
        case Qt::Key_PageDown: overlay_->scrollDisasm(8); return;
        case Qt::Key_Up:       overlay_->moveCursor(-1); return;
        case Qt::Key_Down:     overlay_->moveCursor(1); return;
        case Qt::Key_N:        nameCursorSymbol(); return;   // name/rename the selected line
        case Qt::Key_Semicolon: commentCursor(); return;     // comment the selected line
        case Qt::Key_B:        markData(bk::DataType::Byte);   return;  // mark data types
        case Qt::Key_W:        markData(bk::DataType::Word);   return;
        case Qt::Key_S:        markData(bk::DataType::String); return;
        case Qt::Key_P:        markData(bk::DataType::Ptr);    return;
        case Qt::Key_U:        board_->clearData(overlay_->cursorAddr()); overlay_->update(); return; // back to code
        case Qt::Key_G:        gotoDialog(); return;                       // goto symbol/address
        case Qt::Key_X:        xrefsDialog(); return;                      // who references the cursor
        case Qt::Key_Return:
        case Qt::Key_Enter:    overlay_->followTarget(); return;           // follow branch/call target
        case Qt::Key_Left:     overlay_->navBack(); return;                // history back / forward
        case Qt::Key_Right:    overlay_->navForward(); return;
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

void MainWindow::broadcastHighlight(int addr, QWidget* src) {
    if (hotpath_    && hotpath_    != src) hotpath_->setHighlight(addr);
    if (callgraph_  && callgraph_  != src) callgraph_->setHighlight(addr);
    if (flame_      && flame_      != src) flame_->setHighlight(addr);
    if (flamechart_ && flamechart_ != src) flamechart_->setHighlight(addr);
    if (overlay_) overlay_->setHighlight(addr);   // debugger disasm is always a receiver
}

void MainWindow::openHotPath() {
    if (!hotpath_) {
        hotpath_ = new HotPathWidget(board_.get());
        hotpath_->resize(720, 620);
        // Clicking a row jumps the debugger's disassembler to that address,
        // opening the debugger overlay if it isn't already showing.
        connect(hotpath_, &HotPathWidget::addressPicked, this, [this](uint16_t addr) {
            if (!paused_) setPaused(true);   // setPaused re-anchors disasm to PC…
            overlay_->setDisasmAddr(addr);   // …so set the picked address after it
            overlay_->update();
        });
        connect(hotpath_, &HotPathWidget::hoverAddress, this,
                [this](int a) { broadcastHighlight(a, hotpath_); });
    }
    board_->trace().setEnabled(true);
    hotpath_->show();
    hotpath_->raise();
}

void MainWindow::openCallGraph() {
    if (!callgraph_) {
        callgraph_ = new CallGraphWidget(board_.get());
        callgraph_->resize(900, 680);
        // Clicking a function box jumps the debugger's disassembler to its entry.
        connect(callgraph_, &CallGraphWidget::addressPicked, this, [this](uint16_t addr) {
            if (!paused_) setPaused(true);
            overlay_->setDisasmAddr(addr);
            overlay_->update();
        });
        connect(callgraph_, &CallGraphWidget::hoverAddress, this,
                [this](int a) { broadcastHighlight(a, callgraph_); });
    }
    board_->trace().setEnabled(true);
    callgraph_->show();
    callgraph_->raise();
}

void MainWindow::openFlame() {
    if (!flame_) {
        flame_ = new FlameWidget(board_.get());
        flame_->resize(820, 560);
        connect(flame_, &FlameWidget::addressPicked, this, [this](uint16_t addr) {
            if (!paused_) setPaused(true);
            overlay_->setDisasmAddr(addr);
            overlay_->update();
        });
        connect(flame_, &FlameWidget::hoverAddress, this,
                [this](int a) { broadcastHighlight(a, flame_); });
    }
    board_->trace().setEnabled(true);
    board_->trace().setFlameEnabled(true);
    flame_->show();
    flame_->raise();
}

void MainWindow::openFlameChart() {
    if (!flamechart_) {
        flamechart_ = new FlameChartWidget(board_.get());
        flamechart_->resize(900, 480);
        connect(flamechart_, &FlameChartWidget::addressPicked, this, [this](uint16_t addr) {
            if (!paused_) setPaused(true);
            overlay_->setDisasmAddr(addr);
            overlay_->update();
        });
        connect(flamechart_, &FlameChartWidget::hoverAddress, this,
                [this](int a) { broadcastHighlight(a, flamechart_); });
    }
    board_->trace().setEnabled(true);
    board_->trace().setFlameEnabled(true);
    board_->trace().setSpansEnabled(true);
    flamechart_->show();
    flamechart_->raise();
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
