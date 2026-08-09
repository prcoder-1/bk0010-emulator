#pragma once
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>
#include "Utf8.h"

// Клавиатура БК-0010 в кодах КОИ-7 — таблица «имя / текст -> код», доступная из
// ядра (без Qt). Коды взяты дословно из src/ui/BkKeymap.cpp: тот файл остаётся
// авторитетом для преобразования «Qt-клавиша -> код» (там есть модификаторы,
// nativeVirtualKey и т.п.), а этот — для «имя клавиши / строка текста -> код»,
// как нужно MCP-серверу (bk_key, bk_type) и тестам.
//
// Кириллица — КОИ-7 Н1 (коды 0140..0177), латиница/цифры/пунктуация — КОИ-7 Н0
// (совпадает с ASCII). Регистр (РУС/ЛАТ) переключается управляющими кодами
// 016/017, которые вставляются автоматически при смене алфавита.

namespace bk {

// Заглавные буквы КОИ-7 Н1 в порядке кодов 0140..0177 (индекс i -> код 0140+i).
inline constexpr const char* kKoi7H1Utf8 = "ЮАБЦДЕФГХИЙКЛМНОПЯРСТУЖВЬЫЗШЭЩЧЪ";

enum : uint16_t {
    BK_CODE_RUS = 016,   // переключение на кириллицу
    BK_CODE_LAT = 017,   // переключение на латиницу
    BK_KEY_NONE = 0177777,
};

struct BkKeyName { const char* name; uint16_t code; };

// Первое (латинское) имя каждого кода — каноническое: его печатает bkKeyName().
inline constexpr BkKeyName kBkKeys[] = {
    { "enter", 012 },     { "ввод", 012 },      { "return", 012 },
    { "space", 040 },     { "пробел", 040 },
    { "left", 010 },      { "влево", 010 },
    { "right", 031 },     { "вправо", 031 },
    { "up", 032 },        { "вверх", 032 },
    { "down", 033 },      { "вниз", 033 },
    { "tab", 015 },       { "таб", 015 },
    { "backspace", 030 }, { "заб", 030 },
    { "home", 023 },      { "вс", 023 },
    { "delete", 014 },    { "сбр", 014 },
    { "povt", 0201 },     { "повт", 0201 },     { "f1", 0201 },
    { "kt", 03 },         { "кт", 03 },         { "f2", 03 },
    { "f3", 0231 },
    { "f4", 026 },
    { "f5", 027 },
    { "indsu", 0202 },    { "инд су", 0202 },   { "f6", 0202 },
    { "rus", BK_CODE_RUS }, { "рус", BK_CODE_RUS },
    { "lat", BK_CODE_LAT }, { "лат", BK_CODE_LAT },
};

// Код по имени клавиши. BK_KEY_NONE, если имя неизвестно. Имя нечувствительно к
// регистру, латиница и кириллица равноправны.
inline uint16_t bkKeyByName(std::string_view name) {
    const std::string n = normName(name);
    if (n.empty()) return BK_KEY_NONE;
    for (const BkKeyName& k : kBkKeys)
        if (n == k.name) return k.code;
    return BK_KEY_NONE;
}

// Каноническое имя кода (nullptr, если это обычный печатный символ).
inline const char* bkKeyName(uint16_t code) {
    for (const BkKeyName& k : kBkKeys)
        if (k.code == code) return k.name;
    return nullptr;
}

// Индекс заглавной кириллической буквы в КОИ-7 Н1 (-1, если не буква Н1).
inline int koi7H1Index(uint32_t cp) {
    cp = toUpperCp(cp);
    if (cp == 0x401) cp = 0x415;                    // Ё -> Е
    const std::string_view t{kKoi7H1Utf8};
    int idx = 0;
    for (size_t i = 0; i < t.size(); ++idx)
        if (utf8Next(t, i) == cp) return idx;
    return -1;
}

// Преобразовать строку UTF-8 в последовательность кодов КОИ-7, вставляя РУС/ЛАТ
// при смене алфавита. `cyrillicState` — текущий регистр машины (обновляется);
// его надо хранить между вызовами, как это делает BkKeymap для GUI.
// Непредставимые символы пропускаются.
inline std::vector<uint16_t> bkEncodeText(std::string_view utf8, bool& cyrillicState) {
    std::vector<uint16_t> out;
    for (size_t i = 0; i < utf8.size();) {
        const uint32_t cp = utf8Next(utf8, i);
        if (!cp) break;
        if (cp == '\n' || cp == '\r') { out.push_back(012); continue; }   // ВВОД
        if (cp == '\t') { out.push_back(015); continue; }
        if (cp >= 0x20 && cp < 0x7F) {
            const bool letter = (cp >= 'A' && cp <= 'Z') || (cp >= 'a' && cp <= 'z');
            if (letter && cyrillicState) { out.push_back(BK_CODE_LAT); cyrillicState = false; }
            out.push_back(static_cast<uint16_t>(cp));
            continue;
        }
        const int idx = koi7H1Index(cp);
        if (idx >= 0) {
            if (!cyrillicState) { out.push_back(BK_CODE_RUS); cyrillicState = true; }
            out.push_back(static_cast<uint16_t>(0140 + idx));
        }
    }
    return out;
}

} // namespace bk
