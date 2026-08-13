#pragma once
#include <QMainWindow>
#include <memory>
#include <deque>
#include <set>
#include "Board.h"
#include "BkKeymap.h"
#include "Gamepad.h"

class GlScreen;
class DebuggerOverlay;
class MemVisWidget;
class SmkRamWidget;
class HotPathWidget;
class CallGraphWidget;
class FlameWidget;
class FlameChartWidget;
class HotChartWidget;
class AudioOut;
#include <QElapsedTimer>
class QAction;

class QTimer;
class QLabel;

// Main application window: hosts the GL screen, drives the emulation at 50 Hz
// and provides the File/Emulation menus (load .BIN, reset, screen mode).
class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    // smkOverride: 1 — включить блок СМК-512, 0 — выключить, -1 — взять
    // сохранённую настройку (ключ командной строки перекрывает её, но не
    // перезаписывает: настройка меняется только переключателем в меню).
    explicit MainWindow(const QString& romDir, int smkOverride = -1, QWidget* parent = nullptr);
    ~MainWindow() override;

    bool loadBinFromPath(const QString& path);

    // Потактовая эмуляция арбитража КР1801ВП1-037 (ожидания доступа к ДОЗУ).
    void setArbitration(bool on) { if (board_) board_->setArbitration(on); }
    void setSmk512(bool on);            // подключить/снять плату и перезапустить машину

protected:
    void keyPressEvent(QKeyEvent* e) override;
    void keyReleaseEvent(QKeyEvent* e) override;
    void resizeEvent(QResizeEvent* e) override;
    void closeEvent(QCloseEvent* e) override;   // auto-save annotations

private slots:
    void onTick();
    void runOneSlice(int kSlices);   // один слайс кадра + раз в кадр — отрисовка
    void openBin();
    void resetMachine();
    void toggleColorMode();
    void openDisk(int drive = 0);
    void openMemVis();
    void openSmkRam();
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
    void runToCursor();                  // F4: прогнать до инструкции под курсором
    // Interactive-disassembler annotations (symbols + comments).
    void nameCursorSymbol();             // N: name/rename the symbol at the cursor
    void commentCursor();                // ;: edit the comment at the cursor
    void markData(bk::DataType t);       // B/W/S/P: mark the cursor as data of a type
    void gotoDialog();                   // G: jump to a symbol (from a list) or a typed address
    void xrefsDialog();                  // X: list who references the cursor address, jump to one
    void saveAnnotations(const QString& path);
    void loadAnnotations(const QString& path);
    QString annotationsPath() const;     // "<loaded .bin>.bkdb", or empty

    std::unique_ptr<bk::Board> board_;
    GlScreen* screen_ = nullptr;
    DebuggerOverlay* overlay_ = nullptr;
    MemVisWidget* memvis_ = nullptr;
    SmkRamWidget* smkram_ = nullptr;
    HotPathWidget* hotpath_ = nullptr;
    CallGraphWidget* callgraph_ = nullptr;
    FlameWidget* flame_ = nullptr;
    FlameChartWidget* flamechart_ = nullptr;
    HotChartWidget* hotchart_ = nullptr;
    AudioOut* audio_ = nullptr;
    QTimer* timer_ = nullptr;
    QLabel* status_ = nullptr;
    QAction* smkAction_ = nullptr;   // галка «СМК-512» — синхронизируется при загрузке снимка
    QString lastBin_;
    QString lastDisk_;      // последний вставленный образ диска
    BkKeymap keymap_;
    Gamepad gamepad_;           // джойстик на порту 0177714 через SDL2-геймпад
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
    int  phase_ = 0;            // emulation sub-slice within the current frame
    // Темп эмуляции привязан к РЕАЛЬНОМУ времени, а не к числу срабатываний таймера:
    // кадр БК длится 61440 тактов = 20,48 мс, что целым числом миллисекунд в QTimer
    // не выражается. Считаем, сколько слайсов должно было пройти к текущему моменту.
    QElapsedTimer clock_;
    qint64 slicesDone_ = 0;
};
