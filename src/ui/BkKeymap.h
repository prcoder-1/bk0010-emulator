#pragma once
#include <cstdint>
#include <vector>

class QKeyEvent;

// Translates Qt key events into BK-0010 (KOI-7) key codes. Handles:
//  - special keys (arrows, Enter, Tab, Backspace, edit/function keys),
//  - Latin letters / digits / punctuation via the event's produced character,
//  - Cyrillic letters mapped to the KOI-7 H1 range (0140..0177),
//  - Ctrl (СУ) -> control codes,
//  - automatic РУС/ЛАТ switch (0016/0017) emitted once when the input language
//    changes, mirroring pressing the РУС/ЛАТ key on the real machine.
// Special/function codes carry bit 0200 so the Board routes them through the
// АР2 interrupt vector 0274; ordinary codes use vector 060.
class BkKeymap {
public:
    // Returns the sequence of raw BK codes to inject for this key press
    // (usually one code; two when an РУС/ЛАТ switch is prepended). Empty if the
    // key has no BK equivalent.
    std::vector<uint16_t> translate(QKeyEvent* e);

    void reset() { cyrillic_ = false; }

private:
    bool cyrillic_ = false; // current register we believe the BK is in
};
