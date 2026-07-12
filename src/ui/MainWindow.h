#pragma once
#include <QMainWindow>
#include <memory>
#include <deque>
#include <set>
#include "Board.h"
#include "BkKeymap.h"

class GlScreen;
class DebuggerOverlay;
class MemVisWidget;
class CodeGraphWidget;
class AudioOut;
class QTimer;
class QLabel;

// Main application window: hosts the GL screen, drives the emulation at 50 Hz
// and provides the File/Emulation menus (load .BIN, reset, screen mode).
class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(const QString& romDir, QWidget* parent = nullptr);
    ~MainWindow() override;

    bool loadBinFromPath(const QString& path);

protected:
    void keyPressEvent(QKeyEvent* e) override;
    void keyReleaseEvent(QKeyEvent* e) override;
    void resizeEvent(QResizeEvent* e) override;

private slots:
    void onTick();
    void openBin();
    void resetMachine();
    void toggleColorMode();
    void openMemVis();
    void openCodeGraph();
    void saveState();
    void loadState();

private:
    void updateTitle();
    void renderScreen();
    void setPaused(bool paused);
    void stepInto();
    void stepOver();

    std::unique_ptr<bk::Board> board_;
    GlScreen* screen_ = nullptr;
    DebuggerOverlay* overlay_ = nullptr;
    MemVisWidget* memvis_ = nullptr;
    CodeGraphWidget* codegraph_ = nullptr;
    AudioOut* audio_ = nullptr;
    QTimer* timer_ = nullptr;
    QLabel* status_ = nullptr;
    QString lastBin_;
    BkKeymap keymap_;
    // Host-side typing buffer: a keypress may translate to more than one BK code
    // (e.g. a РУС/ЛАТ switch + the character). Because the BK register holds only
    // one code at a time, we feed codes one per frame as the register frees up.
    std::deque<uint16_t> keyFeed_;
    // Qt key codes physically held down (auto-repeat ignored), so 0177716 bit 6
    // stays low while any game key is held — polled by games like Digger.
    std::set<int> heldKeys_;
    bool colorMode_ = true;
    bool paused_ = false;
};
