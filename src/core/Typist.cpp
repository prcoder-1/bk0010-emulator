#include "Typist.h"
#include "Board.h"
#include "BkKeys.h"
#include "Utf8.h"
#include <algorithm>

namespace bk {

int systemCyrillic(Board& b) {
    const Memory& m = b.memory();
    const uint16_t isr = m.peekWord(060);
    if (!b.bk11() && isr == 0101136) return (m.peekByte(043) & 0200) ? 1 : 0;
    if (b.bk11() && isr == 0147114) return (m.ram11()[7 * Memory::PAGE11_BYTES + 0455] & 0200) ? 1 : 0;
    return -1;
}

TypeResult typeText(Board& b, std::string_view utf8, bool& cyrState, int holdFrames, int gapFrames, int budget) {
    TypeResult r;
    holdFrames = std::max(1, holdFrames);
    gapFrames = std::max(0, gapFrames);
    for (size_t i = 0; i < utf8.size();) if (utf8Next(utf8, i)) ++r.total; else break;
    auto frame = [&] { b.runFrame(); ++r.frames; };
    // EMT 6 монитора БК-0010: 0101010 MTPS #0, 0101014 MOV @#177662,R0 (сброс), дальше ожидание.
    auto waitFree = [&] {
        while (b.keyReady() && r.frames < budget) frame();
        if (!b.bk11() && b.memory().peekWord(060) == 0101136)
            for (int k = 0; k < 50 && r.frames < budget; ++k) {
                const uint16_t pc = b.cpu().pc();
                if (pc >= 0101020 && pc < 0101136) break;
                frame();
            }
        return r.frames < budget;
    };
    for (size_t pos = 0; pos < utf8.size() && waitFree();) {
        const size_t start = pos;
        if (!utf8Next(utf8, pos)) break;
        if (const int s = systemCyrillic(b); s >= 0) cyrState = s;
        const std::vector<uint16_t> codes = bkEncodeText(utf8.substr(start, pos - start), cyrState);
        bool done = true;
        for (const uint16_t c : codes) {
            if (!waitFree()) { done = false; break; }
            b.pressKey(c);
            b.setKeyHeld(true);
            for (int k = 0; k < holdFrames && r.frames < budget; ++k) frame();
            b.setKeyHeld(false);
            for (int k = 0; k < gapFrames && r.frames < budget; ++k) frame();
            ++r.codes;
        }
        if (!done) break;
        ++r.chars;
    }
    for (int k = 0; k < 12 && b.keyReady() && r.frames < budget; ++k) frame();
    b.setKeyHeld(false);
    if (const int s = systemCyrillic(b); s >= 0) cyrState = s;
    return r;
}

} // namespace bk
