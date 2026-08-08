#include "ScreenOcr.h"
#include "Memory.h"
#include "BkKeys.h"
#include "Utf8.h"
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string_view>

namespace bk {

// ---------------------------------------------------------------------------
// экранная кодировка -> UTF-8
// ---------------------------------------------------------------------------
namespace {

// Управляющие глифы 020..037 (видны только в режиме «ИНД СУ»). Формы взяты из
// самой таблицы ПЗУ; для неочевидных отдаём восьмеричный код в фигурных скобках.
constexpr const char* kCtrlGlyphs[16 + 16] = {
    /*020*/ "P",  /*021*/ "←", /*022*/ "{022}", /*023*/ "{023}",
    /*024*/ "↕",  /*025*/ "{025}", /*026*/ "⇤", /*027*/ "⇥",
    /*030*/ "↔",  /*031*/ "→", /*032*/ "↑",     /*033*/ "↓",
    /*034*/ "↘",  /*035*/ "↙", /*036*/ "↗",     /*037*/ "↖",
};

// Псевдографика 240..277 (АР2 + буква). Порядок по таблице монитора; 240 —
// именно «π» (так нарисовано в ПЗУ), хотя часть справочников печатает «¶».
constexpr const char* kPseudo[32] = {
    "π", "┴", "♥", "┐", "╡", "├", "└", "═",
    "╤", "♠", "┌", "┬", "╨", "↓", "┼", "║",
    "┤", "←", "╬", "↑", "♣", "─", "╫", "│",
    "♦", "┘", "╪", "╥", "╧", "╞", "→", "▓",
};

} // namespace

std::string bkScreenCodeToUtf8(uint16_t code) {
    code &= 0377;
    if (code >= 020 && code <= 037) return kCtrlGlyphs[code - 020];
    if (code == 044) return "¤";                       // «солнышко» вместо '$'
    if (code >= 040 && code <= 0176) return std::string(1, static_cast<char>(code));
    if (code == 0177) return "■";
    if (code >= 0240 && code <= 0277) return kPseudo[code - 0240];
    if (code >= 0300 && code <= 0377) {
        // 0300..0337 — строчные, 0340..0377 — заглавные, порядок КОИ-7 Н1.
        const bool upper = code >= 0340;
        const int idx = static_cast<int>(code - (upper ? 0340 : 0300));
        const std::string_view h1{kKoi7H1Utf8};
        size_t i = 0;
        for (int k = 0; k < idx; ++k) utf8Next(h1, i);
        uint32_t cp = utf8Next(h1, i);
        if (!upper) cp = toLowerCp(cp);
        std::string out;
        utf8Append(out, cp);
        return out;
    }
    char b[12];
    std::snprintf(b, sizeof b, "{%04o}", code);
    return b;
}

// ---------------------------------------------------------------------------
// битовая карта экрана (та же развёртка скролла, что и в Screen::render)
// ---------------------------------------------------------------------------
void screenBitmap(const Memory& mem, uint16_t scrollReg, std::vector<uint8_t>& out) {
    out.assign(64 * 256, 0);
    const int scrollOffset = (static_cast<int>(scrollReg & 0377) - 0330) & 0377;
    const int nlines = (scrollReg & 01000) ? 256 : 64;   // бит 9 — полный/малый экран
    for (int y = 0; y < nlines; ++y) {
        const int memLine = (y + scrollOffset) & 0377;
        std::copy_n(mem.videoRam() + memLine * 64, 64, out.begin() + y * 64);
    }
}

// ---------------------------------------------------------------------------
// распознавание
// ---------------------------------------------------------------------------
namespace {

struct Glyph { uint16_t code; uint16_t twin; uint8_t rows[16]; };

// Буква латиницы/цифра?
inline bool isLatinCode(uint16_t c) {
    return (c >= 0101 && c <= 0132) || (c >= 0141 && c <= 0172);
}
inline bool isCyrCode(uint16_t c) { return c >= 0300 && c <= 0377; }

inline int popcount8(uint8_t v) {
    static constexpr uint8_t kBits[16] = {0,1,1,2,1,2,2,3,1,2,2,3,2,3,3,4};
    return kBits[v & 15] + kBits[v >> 4];
}

// Считать nPix точек экрана начиная с (x, y) в младшие биты результата.
inline uint32_t sampleBits(const std::vector<uint8_t>& bits, int x, int y, int nPix) {
    uint32_t v = 0;
    if (y < 0 || y >= 256) return 0;
    const uint8_t* line = bits.data() + y * 64;
    for (int i = 0; i < nPix; ++i) {
        const int px = x + i;
        if (px < 0 || px >= 512) continue;
        if ((line[px >> 3] >> (px & 7)) & 1) v |= 1u << i;
    }
    return v;
}

std::vector<Glyph> buildFont(const Memory& mem, const OcrOptions& o) {
    std::vector<Glyph> font;
    const int h = std::min(o.fontHeight, 16);
    auto add = [&](uint16_t code, int idx) {
        Glyph g{};
        g.code = code;
        for (int r = 0; r < h; ++r)
            g.rows[r] = mem.peekByte(static_cast<uint16_t>(o.fontAddr + idx * o.fontHeight + r));
        font.push_back(g);
    };
    if (o.bkLayout) {
        int idx = 0;
        for (uint16_t c = 020; c <= 0177; ++c) add(c, idx++);
        for (uint16_t c = 0240; c <= 0377; ++c) add(c, idx++);
    } else {
        const int n = o.fontCount > 0 ? o.fontCount : (256 - o.fontBase);
        for (int i = 0; i < n; ++i) add(static_cast<uint16_t>(o.fontBase + i), i);
    }
    // Омоглифы: у БК глифы латинских A B C E H K M O P T X и кириллических
    // А В С Е Н К М О Р Т Х побитно совпадают, различить их по картинке нельзя.
    // Запоминаем двойника, чтобы потом выбрать алфавит по контексту строки.
    for (size_t i = 0; i < font.size(); ++i)
        for (size_t j = i + 1; j < font.size(); ++j) {
            if (std::memcmp(font[i].rows, font[j].rows, sizeof font[i].rows) != 0) continue;
            if (!font[i].twin) font[i].twin = font[j].code;
            if (!font[j].twin) font[j].twin = font[i].code;
        }
    return font;
}

// Свести знакоместо к маске: h байт по glyphW значащих младших битов.
// Для широкого режима каждая пара битов схлопывается в один бит.
void cellMask(const std::vector<uint8_t>& bits, const OcrOptions& o, int x, int y,
              int h, uint8_t* outA, uint8_t* outB, bool& twoRules) {
    const int gw = std::min(o.glyphW, 8);
    twoRules = false;
    if (!o.wide) {
        const uint8_t keep = static_cast<uint8_t>(gw >= 8 ? 0xFF : ((1 << gw) - 1));
        for (int r = 0; r < h; ++r)
            outA[r] = outB[r] = static_cast<uint8_t>(sampleBits(bits, x, y + r, gw) & keep);
        return;
    }
    // Широкий режим: 2 бита экрана на бит глифа. Правило A — «пара не нулевая»
    // (фон чёрный, обычный случай). Правило B — «пара отличается от самой частой
    // в знакоместе» (цветной фон): даёт верную маску, когда фон не чёрный.
    int hist[4] = {0, 0, 0, 0};
    uint16_t words[16];
    for (int r = 0; r < h; ++r) {
        words[r] = static_cast<uint16_t>(sampleBits(bits, x, y + r, gw * 2));
        for (int i = 0; i < gw; ++i) ++hist[(words[r] >> (i * 2)) & 3];
    }
    int bg = 0;
    for (int v = 1; v < 4; ++v) if (hist[v] > hist[bg]) bg = v;
    for (int r = 0; r < h; ++r) {
        uint8_t a = 0, b = 0;
        for (int i = 0; i < gw; ++i) {
            const int pair = (words[r] >> (i * 2)) & 3;
            if (pair != 0)  a |= static_cast<uint8_t>(1 << i);
            if (pair != bg) b |= static_cast<uint8_t>(1 << i);
        }
        outA[r] = a;
        outB[r] = b;
    }
    twoRules = (bg != 0);
}

// Ближайший глиф к маске. Инвертированный вариант проигрывает прямому при
// равном расстоянии, поэтому пустое знакоместо — «пробел», а не «инверсный ■».
// Инверсия — ЗАПАСНОЙ вариант: её берём, только если она строго лучше любого
// прямого совпадения. Иначе глиф 020 (в ПЗУ он нарисован уже инвертированным —
// это индикатор «СУ») своей инверсией забивает обычную букву Р, а инверсный
// пробел — закрашенный квадрат 0177.
void matchMask(const std::vector<Glyph>& font, const uint8_t* mask, int h, uint8_t keep,
               bool allowInverse, int& bestDist, uint16_t& bestCode, uint16_t& bestTwin,
               bool& bestInv) {
    int dNorm = bestDist;
    uint16_t cNorm = bestCode, tNorm = bestTwin;
    for (const Glyph& g : font) {
        int d = 0;
        for (int r = 0; r < h && d < dNorm; ++r) d += popcount8(mask[r] ^ (g.rows[r] & keep));
        if (d < dNorm) { dNorm = d; cNorm = g.code; tNorm = g.twin; }
    }
    if (dNorm < bestDist) { bestDist = dNorm; bestCode = cNorm; bestTwin = tNorm; bestInv = false; }
    if (!allowInverse || bestDist == 0) return;
    int dInv = bestDist;
    uint16_t cInv = 0, tInv = 0;
    for (const Glyph& g : font) {
        int d = 0;
        for (int r = 0; r < h && d < dInv; ++r)
            d += popcount8(mask[r] ^ static_cast<uint8_t>(~g.rows[r] & keep));
        if (d < dInv) { dInv = d; cInv = g.code; tInv = g.twin; }
    }
    if (dInv < bestDist) { bestDist = dInv; bestCode = cInv; bestTwin = tInv; bestInv = true; }
}

// Латиница или кириллица? Глифы части букв совпадают, поэтому алфавит выбирается
// по большинству ОДНОЗНАЧНЫХ букв — сначала в самой строке, при отсутствии улик
// в строке — по всему экрану. Иначе русский текст выходит вперемешку с латиницей.
void resolveScript(OcrResult& res) {
    auto tally = [&](int from, int to, int& cyr, int& lat) {
        for (int i = from; i < to; ++i) {
            const OcrCell& c = res.cells[i];
            if (!c.ok || c.altCode) continue;         // считаем только однозначные буквы
            if (isCyrCode(c.code)) ++cyr;
            else if (isLatinCode(c.code)) ++lat;
        }
    };
    int scrCyr = 0, scrLat = 0;
    tally(0, (int)res.cells.size(), scrCyr, scrLat);
    for (int row = 0; row < res.rows; ++row) {
        const int a = row * res.cols, b = a + res.cols;
        int cyr = 0, lat = 0;
        tally(a, b, cyr, lat);
        if (!cyr && !lat) { cyr = scrCyr; lat = scrLat; }
        if (cyr <= lat) continue;                     // латиница или ничья — оставляем как есть
        for (int i = a; i < b; ++i) {
            OcrCell& c = res.cells[i];
            if (c.ok && c.altCode && isLatinCode(c.code) && isCyrCode(c.altCode))
                c.code = c.altCode;
        }
    }
}

} // namespace

OcrResult ocrScreen(const Memory& mem, const std::vector<uint8_t>& bits, const OcrOptions& opt) {
    OcrResult res;
    res.opt = opt;
    const int h = std::max(1, std::min(opt.fontHeight, 16));
    const int cellW = opt.cellW > 0 ? opt.cellW : (opt.wide ? 16 : 8);
    const int gw = std::max(1, std::min(opt.glyphW, 8));
    const uint8_t keep = static_cast<uint8_t>(gw >= 8 ? 0xFF : ((1 << gw) - 1));

    res.cols = opt.cols > 0 ? opt.cols : std::max(0, (512 - opt.x0) / cellW);
    res.rows = opt.rows > 0 ? opt.rows : std::max(0, (256 - opt.y0) / h);
    if (res.cols <= 0 || res.rows <= 0) return res;
    res.cells.assign(static_cast<size_t>(res.cols) * res.rows, OcrCell{});

    const std::vector<Glyph> font = buildFont(mem, opt);
    uint8_t maskA[16], maskB[16];

    for (int row = 0; row < res.rows; ++row) {
        const int y = opt.y0 + row * h;
        for (int col = 0; col < res.cols; ++col) {
            const int x = opt.x0 + col * cellW;
            bool twoRules = false;
            cellMask(bits, opt, x, y, h, maskA, maskB, twoRules);

            OcrCell& c = res.cells[static_cast<size_t>(row) * res.cols + col];
            bool empty = true;
            for (int r = 0; r < h; ++r) if (maskA[r]) { empty = false; break; }
            c.blank = empty;
            if (!empty) ++res.nonBlank;

            int dist = INT32_MAX;
            uint16_t code = 0, twin = 0;
            bool inv = false;
            matchMask(font, maskA, h, keep, opt.allowInverse, dist, code, twin, inv);
            if (twoRules) matchMask(font, maskB, h, keep, opt.allowInverse, dist, code, twin, inv);

            c.dist = dist;
            c.code = code;
            c.altCode = twin;
            c.inverted = inv;
            c.ok = dist <= opt.tolerance;
            res.residual += dist;
            if (empty) continue;                      // пустые в статистику не идут
            if (c.ok) { ++res.recognised; if (inv) ++res.inverted; }
            else      { ++res.unknown; }
        }
    }
    resolveScript(res);
    return res;
}

OcrResult ocrAuto(const Memory& mem, const std::vector<uint8_t>& bits, OcrOptions base,
                  bool fixedMode, bool fixedY0) {
    OcrResult best;
    bool haveBest = false;
    const bool modes[2] = {false, true};
    for (int mi = 0; mi < 2; ++mi) {
        if (fixedMode && modes[mi] != base.wide) continue;
        OcrOptions o = base;
        o.wide = modes[mi];
        const int h = std::max(1, std::min(o.fontHeight, 16));
        // Строки текста идут с шагом h, поэтому все возможные выравнивания
        // покрываются смещениями 0..h-1 (штатное y0=16 служебной строки — это 6 при h=10).
        for (int dy = 0; dy < (fixedY0 ? 1 : h); ++dy) {
            o.y0 = fixedY0 ? base.y0 : dy;
            OcrResult r = ocrScreen(mem, bits, o);
            // Меньше остаточных точек — вернее сетка; при равенстве побеждает та,
            // где распознано больше знакомест.
            if (!haveBest || r.residual < best.residual ||
                (r.residual == best.residual && r.recognised > best.recognised)) {
                best = std::move(r);
                haveBest = true;
            }
        }
    }
    return best;
}

std::string OcrResult::text(char unknownChar) const {
    std::string out;
    for (int row = 0; row < rows; ++row) {
        std::string line;
        int lastNonSpace = -1;
        for (int col = 0; col < cols; ++col) {
            const OcrCell& c = cells[static_cast<size_t>(row) * cols + col];
            if (c.ok) {
                if (c.code != 040) lastNonSpace = static_cast<int>(line.size());
                const std::string s = bkScreenCodeToUtf8(c.code);
                line += s;
                if (c.code != 040) lastNonSpace = static_cast<int>(line.size());
            } else {
                line += unknownChar;
                lastNonSpace = static_cast<int>(line.size());
            }
        }
        if (lastNonSpace >= 0) line.resize(static_cast<size_t>(lastNonSpace));
        else line.clear();
        out += line;
        out += '\n';
    }
    return out;
}

} // namespace bk
