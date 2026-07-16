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
class HotPathWidget;
class CallGraphWidget;
class FlameWidget;
class FlameChartWidget;
class HotChartWidget;
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
    void closeEvent(QCloseEvent* e) override;   // auto-save annotations

private slots:
    void onTick();
    void openBin();
    void resetMachine();
    void toggleColorMode();
    void openMemVis();
    void openHotPath();
    void openCallGraph();
    void openFlame();
    void openFlameChart();
    void openHotChart();
    void saveState();
    void loadState();

private:
    void updateTitle();
    void renderScreen();
    // Linked highlighting: relay a hovered address to every profiler window (and
    // the debugger's disassembler) except the one it came from.
    void broadcastHighlight(int addr, QWidget* src);
    void setPaused(bool paused);
    void setSuspended(bool suspended);   // simple pause (Pause key), no debugger
    void stepInto();
    void stepOver();
    // Interactive-disassembler annotations (symbols + comments).
    void nameCursorSymbol();             // N: name/rename the symbol at the cursor
    void commentCursor();                // ;: edit the comment at the cursor
    void markData(bk::DataType t);       // B/W/S/P: mark the cursor as data of a type
    void saveAnnotations(const QString& path);
    void loadAnnotations(const QString& path);
    QString annotationsPath() const;     // "<loaded .bin>.bkdb", or empty

    std::unique_ptr<bk::Board> board_;
    GlScreen* screen_ = nullptr;
    DebuggerOverlay* overlay_ = nullptr;
    MemVisWidget* memvis_ = nullptr;
    HotPathWidget* hotpath_ = nullptr;
    CallGraphWidget* callgraph_ = nullptr;
    FlameWidget* flame_ = nullptr;
    FlameChartWidget* flamechart_ = nullptr;
    HotChartWidget* hotchart_ = nullptr;
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
    bool paused_ = false;       // Soft-ICE debugger overlay active
    bool suspended_ = false;    // emulation frozen via the Pause key
};
