#include "BkKeymap.h"
#include <QKeyEvent>
#include <QString>

// КОИ-7 Н1 (РУС) uppercase Cyrillic letters, in code order 0140..0177.
// index i -> BK code (0140 + i).
static const QString kCyrH1 = QString::fromUtf8("ЮАБЦДЕФГХИЙКЛМНОПЯРСТУЖВЬЫЗШЭЩЧЪ");

// Control codes for switching the register (as sent by the РУС/ЛАТ keys).
enum : uint16_t { CODE_LAT = 017, CODE_RUS = 016 };

std::vector<uint16_t> BkKeymap::translate(QKeyEvent* e) {
    // ---- Special keys (authoritative BK codes; bit 0200 => vector 0274) ----
    switch (e->key()) {
    case Qt::Key_Down:      return {0033};
    case Qt::Key_Up:        return {0032};
    case Qt::Key_Left:      return {0010};
    case Qt::Key_Right:     return {0031};
    case Qt::Key_Return:
    case Qt::Key_Enter:     return {0012};   // ВВОД
    case Qt::Key_Tab:       return {0015};
    case Qt::Key_Space:     return {0040};
    case Qt::Key_Backspace: return {0030};   // ЗАБ
    case Qt::Key_Home:      return {0023};   // ВС
    case Qt::Key_Delete:    return {0014};   // СБР
    case Qt::Key_F1:        return {0201};   // ПОВТ
    case Qt::Key_F2:        return {0003};   // КТ
    case Qt::Key_F3:        return {0231};   // =|=>
    case Qt::Key_F4:        return {0026};   // |<==
    case Qt::Key_F5:        return {0027};   // |==>
    case Qt::Key_F6:        return {0202};   // ИНД СУ
    // F7/F8/F10/F12 are reserved by the debugger / screen-mode UI.
    default: break;
    }

    // ---- РУС / ЛАТ register keys, mapped to the two Shift keys ----
    // Besides switching the BK letter register, games use them as fire-left /
    // fire-right. Left Shift = РУС, right Shift = ЛАТ. Shift (not Ctrl) is used so
    // the app's Ctrl-shortcuts don't emit a stray register code. The two sides are
    // told apart by the X keysym (Shift_L=0xffe1, Shift_R=0xffe2) under the xcb
    // platform; a bare modifier press has no e->text(), so handle it here.
    if (e->key() == Qt::Key_Shift) {
        if (e->nativeVirtualKey() == 0xffe2) { cyrillic_ = false; return {CODE_LAT}; }
        cyrillic_ = true; return {CODE_RUS};
    }

    const bool ctrl = e->modifiers() & Qt::ControlModifier;

    // ---- Ctrl (СУ): letter -> control code 001..032 ----
    if (ctrl && e->key() >= Qt::Key_A && e->key() <= Qt::Key_Z)
        return {static_cast<uint16_t>(e->key() - Qt::Key_A + 1)};

    const QString t = e->text();
    if (t.isEmpty()) return {};
    const QChar ch = t[0];
    const ushort u = ch.unicode();

    // ---- Latin / digits / punctuation (KOI-7 H0 == ASCII) ----
    if (u >= 0x20 && u < 0x7f) {
        std::vector<uint16_t> out;
        if (ch.isLetter() && cyrillic_) { out.push_back(CODE_LAT); cyrillic_ = false; }
        out.push_back(static_cast<uint16_t>(u));
        return out;
    }

    // ---- Cyrillic letters -> KOI-7 H1 (0140..0177) ----
    int idx = kCyrH1.indexOf(ch.toUpper());
    if (idx < 0 && (u == 0x0401 || u == 0x0451)) idx = kCyrH1.indexOf(QChar(0x0415)); // Ё -> Е
    if (idx >= 0) {
        std::vector<uint16_t> out;
        if (!cyrillic_) { out.push_back(CODE_RUS); cyrillic_ = true; }
        out.push_back(static_cast<uint16_t>(0140 + idx));
        return out;
    }

    return {};
}
