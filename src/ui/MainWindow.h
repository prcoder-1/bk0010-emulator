#pragma once
#include <QMainWindow>
#include <memory>
#include "Board.h"

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
    bool colorMode_ = true;
    bool paused_ = false;
};
