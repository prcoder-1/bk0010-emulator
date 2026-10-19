#pragma once
#include <QString>
#include <cstdint>

// Общие форматы чисел для окон отладчика и MCP-сервера.

// Адрес или слово БК — восьмерично, 6 знаков.
inline QString oct6(unsigned v) { return QString("%1").arg(v & 0177777, 6, 8, QChar('0')); }

// Большой счётчик с суффиксом K/M/G: 1209 → «1.2K», 123456 → «123K».
inline QString countKMG(uint64_t n) {
    static const struct { double div; const char* suf; } units[] = {{1e9, "G"}, {1e6, "M"}, {1e3, "K"}};
    for (const auto& u : units)
        if (n >= u.div) {
            const double v = n / u.div;
            return (v < 100.0) ? QString::number(v, 'f', 1) + u.suf : QString::number(int(v + 0.5)) + u.suf;
        }
    return QString::number(static_cast<qulonglong>(n));
}
