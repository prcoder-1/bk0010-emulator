#pragma once
#include <cstdint>
#include <string>
#include <string_view>

// Маленькие UTF-8 утилиты для ядра (без Qt): нужны таблицам имён клавиш и кнопок
// джойстика, которые принимают как латинские, так и русские названия в любом
// регистре («ВПРАВО», «Огонь1», «Enter»).

namespace bk {

// Декодировать одну UTF-8 последовательность из `s`, начиная с `i`; `i`
// продвигается на её длину. Возвращает кодовую точку (0xFFFD при битой
// последовательности, 0 — если строка кончилась).
inline uint32_t utf8Next(std::string_view s, size_t& i) {
    if (i >= s.size()) return 0;
    const unsigned char c = static_cast<unsigned char>(s[i]);
    if (c < 0x80) { ++i; return c; }
    const int n = (c >= 0xF0) ? 3 : (c >= 0xE0) ? 2 : (c >= 0xC0) ? 1 : -1;
    if (n < 0 || i + static_cast<size_t>(n) >= s.size()) { ++i; return 0xFFFD; }
    uint32_t cp = static_cast<uint32_t>(c & (0x3F >> n));
    for (int k = 1; k <= n; ++k) {
        const unsigned char cc = static_cast<unsigned char>(s[i + k]);
        if ((cc & 0xC0) != 0x80) { ++i; return 0xFFFD; }
        cp = (cp << 6) | (cc & 0x3F);
    }
    i += static_cast<size_t>(n) + 1;
    return cp;
}

inline void utf8Append(std::string& out, uint32_t cp) {
    if (cp < 0x80) {
        out += static_cast<char>(cp);
    } else if (cp < 0x800) {
        out += static_cast<char>(0xC0 | (cp >> 6));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        out += static_cast<char>(0xE0 | (cp >> 12));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | (cp >> 18));
        out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    }
}

// Нижний регистр для латиницы и кириллицы (А-Я, Ё). Прочие символы — как есть.
inline uint32_t toLowerCp(uint32_t cp) {
    if (cp >= 'A' && cp <= 'Z') return cp + 32;
    if (cp >= 0x410 && cp <= 0x42F) return cp + 0x20;   // А-Я -> а-я
    if (cp == 0x401) return 0x451;                       // Ё -> ё
    return cp;
}

inline uint32_t toUpperCp(uint32_t cp) {
    if (cp >= 'a' && cp <= 'z') return cp - 32;
    if (cp >= 0x430 && cp <= 0x44F) return cp - 0x20;   // а-я -> А-Я
    if (cp == 0x451) return 0x401;                       // ё -> Ё
    return cp;
}

// Нижний регистр + обрезка пробелов по краям — канонизация имени кнопки/клавиши.
inline std::string normName(std::string_view s) {
    size_t b = 0, e = s.size();
    while (b < e && (s[b] == ' ' || s[b] == '\t')) ++b;
    while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t')) --e;
    const std::string_view t = s.substr(b, e - b);
    std::string out;
    out.reserve(t.size());
    for (size_t i = 0; i < t.size();) utf8Append(out, toLowerCp(utf8Next(t, i)));
    return out;
}

} // namespace bk
