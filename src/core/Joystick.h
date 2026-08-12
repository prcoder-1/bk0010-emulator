#pragma once
#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>
#include "Utf8.h"

// Раскладки джойстика БК-0010 на параллельном порту 0177714 (активный уровень
// высокий: нажато = бит установлен). Единственный источник правды — и для GUI
// (src/ui/Gamepad.cpp, реальный геймпад), и для MCP-сервера (bk_joystick), и для
// юнит-тестов (tests/cpu_tests.cpp линкует только bkcore, поэтому таблица обязана
// жить в ядре, а не в UI).
//
// Историю раскладок и способ определить раскладку конкретной игры см.
// docs/joystick.md.

namespace bk {

enum class JoyStandard : uint8_t { Standard = 0, BreakHouse = 1, SWCorp = 2, Klad2 = 3 };

struct JoyLayout { uint16_t up, down, left, right, fire[5]; };

// fire[0..3] — кнопки геймпада A/B/X/Y, fire[4] — дополнительная (Start/кнопка 5).
inline constexpr JoyLayout kJoyLayouts[4] = {
    // «Стандарт» (Джойвокс / эмулятор gid): ВВ=001 ВПР=002 ВН=004 ВЛ=010
    { 0001, 0004, 0010, 0002, { 0040, 0200, 0100, 0400, 0020 } },
    // «Break House» (BKBTL): кнопки 001-010; ВЛ=020 ВН=040 ВПР=0100 ВВ=0200
    { 0200, 0040, 0020, 0100, { 0001, 0002, 0004, 0010, 0001 } },
    // «SWCorp» (BKBTL): кнопки 001-010; ВПР=020 ВН=040 ВЛ=01000 ВВ=02000
    { 02000, 0040, 01000, 0020, { 0001, 0002, 0004, 0010, 0001 } },
    // «Клад-2» (реверс игры): ВПР=001 ВЛ=002 ВВ=004 ВН=010; стрельба влево/вправо=020/040
    { 0004, 0010, 0002, 0001, { 0020, 0040, 0020, 0040, 0020 } },
};

inline constexpr const char* kJoyStandardNames[4] = { "standard", "breakhouse", "swcorp", "klad2" };
inline constexpr const char* kJoyStandardTitles[4] = {
    "Стандарт (Джойвокс/gid)", "Break House", "SWCorp", "Клад-2"
};

inline const JoyLayout& joyLayout(JoyStandard s) {
    return kJoyLayouts[static_cast<int>(s) & 3];
}
inline const char* joyStandardName(JoyStandard s) { return kJoyStandardNames[static_cast<int>(s) & 3]; }
inline const char* joyStandardTitle(JoyStandard s) { return kJoyStandardTitles[static_cast<int>(s) & 3]; }

// Разобрать название раскладки: "standard"/"стандарт", "breakhouse"/"break house",
// "swcorp", "klad2"/"клад-2"/"клад2", либо номер 0..3.
inline bool joyParseStandard(std::string_view name, JoyStandard& out) {
    const std::string n = normName(name);
    if (n.size() == 1 && n[0] >= '0' && n[0] <= '3') { out = static_cast<JoyStandard>(n[0] - '0'); return true; }
    struct A { const char* alias; uint8_t idx; };
    static constexpr A kAliases[] = {
        { "standard", 0 }, { "стандарт", 0 }, { "джойвокс", 0 }, { "gid", 0 },
        { "breakhouse", 1 }, { "break house", 1 }, { "break-house", 1 },
        { "swcorp", 2 }, { "sw corp", 2 },
        { "klad2", 3 }, { "klad-2", 3 }, { "клад2", 3 }, { "клад-2", 3 },
    };
    for (const A& a : kAliases)
        if (n == a.alias) { out = static_cast<JoyStandard>(a.idx); return true; }
    return false;
}

// Бит одной кнопки в заданной раскладке. Понимает латинские и русские имена, а
// также «bitN»/«битN» (0..15) — сырой бит, независимый от раскладки: им удобно
// перебирать порт, когда раскладка игры ещё неизвестна. 0 = имя не распознано.
inline uint16_t joyBit(JoyStandard s, std::string_view button) {
    const std::string n = normName(button);
    if (n.empty() || n == "none" || n == "нет") return 0;
    // bitN / битN
    for (const char* pref : { "bit", "бит" }) {
        const std::string p = pref;
        if (n.size() > p.size() && n.compare(0, p.size(), p) == 0) {
            const std::string num = n.substr(p.size());
            if (!num.empty() && num.size() <= 2) {
                int v = 0;
                bool ok = true;
                for (char c : num) { if (c < '0' || c > '9') { ok = false; break; } v = v * 10 + (c - '0'); }
                if (ok && v >= 0 && v <= 15) return static_cast<uint16_t>(1u << v);
            }
        }
    }
    const JoyLayout& l = joyLayout(s);
    struct A { const char* alias; int8_t dir; int8_t fire; };  // dir: 0..3, fire: 0..4
    static constexpr A kAliases[] = {
        { "up", 0, -1 }, { "вверх", 0, -1 }, { "u", 0, -1 },
        { "down", 1, -1 }, { "вниз", 1, -1 }, { "d", 1, -1 },
        { "left", 2, -1 }, { "влево", 2, -1 }, { "l", 2, -1 },
        { "right", 3, -1 }, { "вправо", 3, -1 }, { "r", 3, -1 },
        { "fire", -1, 0 }, { "огонь", -1, 0 }, { "fire1", -1, 0 }, { "огонь1", -1, 0 },
        { "fire2", -1, 1 }, { "огонь2", -1, 1 },
        { "fire3", -1, 2 }, { "огонь3", -1, 2 },
        { "fire4", -1, 3 }, { "огонь4", -1, 3 },
        { "fire5", -1, 4 }, { "огонь5", -1, 4 }, { "button5", -1, 4 }, { "кнопка5", -1, 4 },
    };
    for (const A& a : kAliases) {
        if (n != a.alias) continue;
        if (a.fire >= 0) return l.fire[a.fire];
        switch (a.dir) {
        case 0: return l.up;
        case 1: return l.down;
        case 2: return l.left;
        default: return l.right;
        }
    }
    return 0;
}

// Разобрать список кнопок: "up+fire1", "вправо, огонь", "bit3". Разделители —
// '+', ',' и пробел. Пустая строка / "none" -> 0. При неизвестном имени пишет
// сообщение в *err (если он задан) и возвращает 0.
inline uint16_t joyMask(JoyStandard s, std::string_view spec, std::string* err = nullptr) {
    uint16_t bits = 0;
    std::string tok;
    auto flush = [&]() -> bool {
        if (tok.empty()) return true;
        const uint16_t b = joyBit(s, tok);
        if (!b) {
            const std::string t = normName(tok);
            if (!t.empty() && t != "none" && t != "нет") {
                if (err) *err = "неизвестная кнопка джойстика: '" + tok + "'";
                tok.clear();
                return false;
            }
        }
        bits |= b;
        tok.clear();
        return true;
    };
    for (char c : spec) {
        if (c == '+' || c == ',' || c == ' ' || c == '\t' || c == '|') { if (!flush()) return 0; }
        else tok += c;
    }
    if (!flush()) return 0;
    return bits;
}

// Человекочитаемая расшифровка значения порта в заданной раскладке:
// "ВПРАВО+ОГОНЬ1" (+ "неизв. 0400" для битов вне раскладки). Пусто -> "нет".
inline std::string joyDecode(JoyStandard s, uint16_t bits) {
    if (!bits) return "нет";
    const JoyLayout& l = joyLayout(s);
    struct E { uint16_t bit; const char* label; };
    const E kEntries[] = {
        { l.up, "ВВЕРХ" }, { l.down, "ВНИЗ" }, { l.left, "ВЛЕВО" }, { l.right, "ВПРАВО" },
        { l.fire[0], "ОГОНЬ1" }, { l.fire[1], "ОГОНЬ2" }, { l.fire[2], "ОГОНЬ3" },
        { l.fire[3], "ОГОНЬ4" }, { l.fire[4], "КНОПКА5" },
    };
    std::string out;
    uint16_t seen = 0;
    for (const E& e : kEntries) {
        if (!e.bit || !(bits & e.bit) || (seen & e.bit) == e.bit) continue;  // не дублировать общие биты
        seen |= e.bit;
        if (!out.empty()) out += "+";
        out += e.label;
    }
    const uint16_t rest = static_cast<uint16_t>(bits & ~seen);
    if (rest) {
        char b[16];
        std::snprintf(b, sizeof b, "%06o", rest);
        if (!out.empty()) out += " ";
        out += "(неизв. ";
        out += b;
        out += ")";
    }
    return out.empty() ? "нет" : out;
}

} // namespace bk
