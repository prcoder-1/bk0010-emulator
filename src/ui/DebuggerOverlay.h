#pragma once
#include <QWidget>
#include <QRect>
#include <QString>
#include <cstdint>
#include <vector>

class QFontMetrics;
class QKeyEvent;
class QPainter;

namespace bk { class Board; }

// Soft-ICE style translucent debugger drawn on top of the BK screen. Shows
// register, disassembly, memory and stack panels. Painting is on-demand; the
// MainWindow drives stepping and toggles visibility with a hotkey. The BK
// screen underneath stays visible through the translucent panels.
class DebuggerOverlay : public QWidget {
    Q_OBJECT
public:
    explicit DebuggerOverlay(bk::Board* board, QWidget* parent = nullptr);

    void followPc();                 // scroll disasm so PC is visible
    void snapshotRegs();             // remember current regs so the next paint can highlight changes
    void setDisasmAddr(uint16_t a) { disasmTop_ = a; update(); } // jump disasm to addr
    uint16_t cursorAddr() const { return cursorAddr_; }          // selected disasm line (for naming/commenting)
    void setCursor(uint16_t a) { cursorAddr_ = a; update(); }
    void moveCursor(int lines);      // move the selection up/down, keeping it visible
    void navigateTo(uint16_t a);     // jump the cursor to an address, recording history
    void navBack();                  // go back / forward through the navigation history
    void navForward();
    bool followTarget();             // follow the branch/call target of the selected line
    // Addresses whose instruction branches/calls/jumps to `target` (a static scan).
    std::vector<uint16_t> xrefsTo(uint16_t target) const;
    // Seed procedure symbols (sub_oooooo) from JSR call targets and the live call
    // trace. Returns how many new symbols were created.
    int analyzeProcedures();
    void setMemAddr(uint16_t a) { memAddr_ = a; update(); }
    // Linked highlighting: mark the disasm line at `addr` (-1 = none).
    void setHighlight(int addr) { if (link_ != addr) { link_ = addr; update(); } }
    void scrollDisasm(int lines);    // move the disasm window
    void scrollMem(int rows);

    // --- Правка значения на месте ---
    // Клик по регистру, ССП или слову в дампе начинает ввод нового значения
    // восьмеричными цифрами. Пока правка идёт, MainWindow отдаёт клавиши сюда:
    // цифры 0-7 набирают значение, Backspace стирает, Enter применяет, Esc отменяет.
    bool editing() const { return editTarget_ != EditTarget::None; }
    bool handleEditKey(QKeyEvent* e);   // true = клавиша использована правкой

protected:
    void paintEvent(QPaintEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
    void wheelEvent(QWheelEvent*) override;

private:
    bk::Board* board_;
    uint16_t disasmTop_ = 0;         // first address shown in the disasm panel
    uint16_t cursorAddr_ = 0;        // selected disasm line (naming/commenting target)
    uint16_t memAddr_   = 01000;     // first address shown in the memory panel
    int lineH_ = 14;                 // pixel height of a text line (recomputed)
    int disasmLines_ = 20;
    int link_ = -1;                  // linked-highlight address (-1 = none)
    int bpScroll_ = 0;               // index of the first breakpoint shown (wheel-scroll)
    uint16_t prevR_[8] = {0};        // registers at the last snapshot (changed-reg highlight)
    uint16_t prevPsw_ = 0;
    bool havePrev_ = false;

    // --- состояние правки на месте ---
    enum class EditTarget { None, Reg, Psw, Mem, Stack };
    EditTarget editTarget_ = EditTarget::None;
    int      editReg_  = 0;          // 0..7 для EditTarget::Reg
    uint16_t editAddr_ = 0;          // адрес слова для EditTarget::Mem / EditTarget::Stack
    QString  editBuf_;               // набранные восьмеричные цифры
    void beginEdit(EditTarget t, int reg, uint16_t addr);
    void commitEdit();
    void cancelEdit();
    // Нарисовать редактируемое поле поверх обычного значения. `current` —
    // прежнее значение: пока ничего не набрано, показываем его приглушённым.
    void drawEditField(QPainter& p, int x, int baselineY, const QFontMetrics& fm,
                       const QString& current) const;

    // Layout rectangles (computed each paint) used by the mouse handlers.
    QRect disasmRect_;
    QRect memRect_;
    QRect regRect_;                  // панель регистров (попадание мышью)
    QRect stkRect_;                  // панель стека (попадание мышью)
    int regX_ = 0, regY_ = 0;        // левый верхний угол сетки регистров (базовая линия первой строки)
    int cw_ = 8;                     // ширина моноширинного символа на последней отрисовке
    int memWpr_ = 8;                 // слов в строке дампа памяти
    QRect bpRect_;                   // breakpoints panel (top-right of the registers)
    std::vector<uint16_t> bpVisible_; // breakpoint addresses currently drawn, row order (hit test)
    std::vector<uint16_t> navBack_, navForward_;  // disasm navigation history

    // Static branch/call/jump target of the instruction at `addr` (false if it is
    // register-indirect or not a control-flow instruction).
    bool targetOf(uint16_t addr, uint16_t& out) const;
    // If the pixel `relX` (relative to the instruction-text origin) falls on an
    // address token in `text` (a symbol name, or an octal number not prefixed by
    // '#'), returns its address. Used to make operand addresses click-to-navigate.
    bool addrTokenAtX(const QString& text, int relX, const QFontMetrics& fm, uint16_t& out) const;
};
