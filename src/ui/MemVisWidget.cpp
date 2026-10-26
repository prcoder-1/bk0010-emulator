#include "MemVisWidget.h"
#include "QtFormat.h"
#include <algorithm>
#include <array>
#include <iterator>
#include <cmath>
#include <cstring>
#include <vector>
#include "Board.h"
#include <QPainter>
#include <QImage>
#include <QComboBox>
#include <QCheckBox>
#include <QSpinBox>
#include <QLabel>
#include <QFontMetrics>
#include <QMouseEvent>
#include <QKeyEvent>
#include <QPixmap>
#include <QGuiApplication>
#include <QSignalBlocker>
#include <QVBoxLayout>
#include <QHBoxLayout>

using bk::Board;

// A small 16-entry colour map for 4-bpp colour mode / 8-bpp colour buckets.
static QRgb colorMap(int v /*0..15*/) {
    static const QRgb c[16] = {
        qRgb(0,0,0),     qRgb(0,0,170),   qRgb(0,170,0),   qRgb(0,170,170),
        qRgb(170,0,0),   qRgb(170,0,170), qRgb(170,85,0),  qRgb(170,170,170),
        qRgb(85,85,85),  qRgb(85,85,255), qRgb(85,255,85), qRgb(85,255,255),
        qRgb(255,85,85), qRgb(255,85,255),qRgb(255,255,85),qRgb(255,255,255),
    };
    return c[v & 15];
}

// Точка (столбец или строка) d окна из dstN показывает элементы источника [s0, s1)
// из srcN: при уменьшении — несколько (их объединяет самый яркий), при увеличении —
// один. Общая для отрисовки и рисования мышью, чтобы мышь попадала ровно в видимое.
static void srcSpan(int d, int srcN, int dstN, int& s0, int& s1) {
    s0 = static_cast<int>(static_cast<int64_t>(d) * srcN / dstN);
    s1 = static_cast<int>(static_cast<int64_t>(d + 1) * srcN / dstN);
    if (s1 <= s0) s1 = s0 + 1;
    if (s1 > srcN) s1 = srcN;
}

static inline QRgb maxRgb(QRgb a, QRgb b) {
    return qRgb(std::max(qRed(a), qRed(b)), std::max(qGreen(a), qGreen(b)),
                std::max(qBlue(a), qBlue(b)));
}

QRgb MemCanvas::pixelColor(int bpp, bool color, unsigned v) {
    // 2 бита — как цветной экран БК: младшие биты слева, палитра 0 (чёрный / синий /
    // зелёный / красный). В этом режиме сразу видно спрайты и экранные буферы в том
    // виде, в каком их рисует машина.
    static const QRgb kBkPal[4] = {qRgb(0,0,0), qRgb(0,0,255), qRgb(0,255,0), qRgb(255,0,0)};
    switch (bpp) {
    case 1:
        v &= 1;
        return color ? colorMap(v ? 15 : 0) : (v ? qRgb(220,220,220) : qRgb(0,0,0));
    case 2: {
        const int c = static_cast<int>(v & 3);
        return color ? kBkPal[c] : qRgb(c * 85, c * 85, c * 85);
    }
    case 4: {
        const int c = static_cast<int>(v & 15);
        return color ? colorMap(c) : qRgb(c * 17, c * 17, c * 17);
    }
    case 16: {                                       // RGB565
        const unsigned w = v & 0xFFFF;
        return color ? qRgb(((w >> 11) & 0x1F) << 3, ((w >> 5) & 0x3F) << 2, (w & 0x1F) << 3)
                     : qRgb(w >> 8, w >> 8, w >> 8); // Ч/Б: high byte as intensity
    }
    default: {                                       // 8 bpp
        const int c = static_cast<int>(v & 0xFF);
        return color ? colorMap(c >> 4) : qRgb(c, c, c);
    }
    }
}

MemCanvas::MemCanvas(Board* board, QWidget* parent)
    : QWidget(parent), board_(board) {
    // Небольшой минимум по высоте: раньше было 400, и в невысоком окне канва не могла
    // ужаться до доступного места — её низ (самые высокие адреса: нижние строки экрана)
    // вылезал за нижний край окна и обрезался. Теперь канва всегда влезает в окно, а
    // вертикальное сжатие строк без потерь обеспечивает paintEvent (OR-объединение).
    setMinimumSize(256, 96);
}

void MemCanvas::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.fillRect(rect(), Qt::black);
    lay_.kind = Layout::None;

    const bk::Memory& mem = board_->memory();
    bk::Trace& tr = board_->trace();
    tr.setEnabled(true); // ensure data is being collected while the view is open

    const bool word16 = (bpp == 16);        // 16 bpp: one pixel per 16-bit word (RGB565)
    int pxPerByte = (bpp == 1) ? 8 : (bpp == 2 ? 4 : (bpp == 4 ? 2 : 1));
    int imgW = word16 ? (bytesPerRow / 2) : (bytesPerRow * pxPerByte);
    if (imgW <= 0) return;

    // Источник: память самой БК, ДОЗУ платы СМК-512 (страница или все 16 подряд) или
    // ОЗУ БК-0011М (страница или все 8, в общем виде — ещё и ПЗУ).
    const bool smkMode = (src == Src::SmkAll || src == Src::SmkPage) && board_->smk512()
                      && board_->smk().ram().size() >= bk::Smk512::RAM_BYTES;
    const uint8_t* smkRam = smkMode ? board_->smk().ram().data() : nullptr;
    const int smkPage = (src == Src::SmkPage)
                      ? std::clamp(page, 0, static_cast<int>(bk::Smk512::PAGES) - 1) : -1;
    const bool b11Mode = (src == Src::Bk11All || src == Src::Bk11Page) && mem.bk11();
    const bool b11All = b11Mode && src == Src::Bk11All;

    // БК-0011М: линейный буфер — 8 страниц ОЗУ, за ними (в общем виде с ПЗУ) загруженные
    // ПЗУ-слоты в порядке карты: ПЗУ-стр. 0, ПЗУ-стр. 1, БОС, МСТД. romLin[слот] — где
    // слот начинается в буфере (-1 — его там нет).
    using M = bk::Memory;
    const uint8_t* lin11 = nullptr;
    size_t lin11Size = 0;
    std::vector<uint8_t> flat11;
    int romLin[M::ROM11_SLOTS];
    std::fill(std::begin(romLin), std::end(romLin), -1);
    if (b11Mode) {
        lin11 = mem.ram11();
        lin11Size = M::RAM11_BYTES;
        if (b11All && !hideRom) {
            static const int kOrder[] = {M::ROM_BASIC2, M::ROM_BASIC3, M::ROM_BASIC1,
                                         M::ROM_EXT, M::ROM_BOS, M::ROM_MSTD};
            flat11.assign(mem.ram11(), mem.ram11() + M::RAM11_BYTES);
            for (int slot : kOrder)
                if (const uint8_t* r = mem.rom11(slot)) {
                    romLin[slot] = static_cast<int>(flat11.size());
                    flat11.insert(flat11.end(), r, r + M::ROM11_BYTES);
                }
            lin11 = flat11.data();
            lin11Size = flat11.size();
        }
    }

    // Build the sequence of *visible* byte addresses, compacting out the hidden
    // regions (ROM and/or video RAM). Hidden memory takes up no space in the view:
    // the remaining bytes are laid out contiguously and the image is scaled to
    // fill the whole window. So with the screen hidden, ROM (if shown) moves up to
    // occupy that space instead of leaving a blank gap.
    // В режимах ДОЗУ и 11М прятать нечего: там сплошная память, и видимые байты —
    // просто её отрезок data[0..n) с началом from в линейном буфере.
    const int scrStart = 0040000, romStart = 0100000;
    std::vector<uint16_t> vis;       // только для адресов ЦП: индекс -> адрес БК
    std::vector<uint8_t> bkBytes;
    const uint8_t* data = nullptr;
    int n = 0, from = 0;
    int scrBoundary = -1, romBoundary = -1; // compacted index where each region begins
    if (b11Mode) {
        from = b11All ? 0 : std::clamp(page, 0, 7) * static_cast<int>(M::PAGE11_BYTES);
        n = b11All ? static_cast<int>(lin11Size) : static_cast<int>(M::PAGE11_BYTES);
        data = lin11 + from;
    } else if (smkMode) {
        from = (smkPage >= 0) ? smkPage * static_cast<int>(bk::Smk512::PAGE_BYTES) : 0;
        n = static_cast<int>((smkPage >= 0) ? bk::Smk512::PAGE_BYTES : bk::Smk512::RAM_BYTES);
        data = smkRam + from;
    } else {
        vis.reserve(static_cast<size_t>(0x10000 - startAddr));
        for (int a = startAddr; a < 0x10000; ++a) {
            uint16_t a16 = static_cast<uint16_t>(a);
            // Скрывая ПЗУ, прячем и всё, что выше него (регистры ввода-вывода 0177600..
            // 0177777) — иначе они висли двумя «лишними» строками под экраном, и нижняя
            // строка экрана (самый высокий адрес видеопамяти) оказывалась не у нижнего
            // края. Теперь при спрятанном ПЗУ видны ровно ОЗУ + экран (0..077777).
            if (hideRom && a16 >= romStart) continue;
            if (hideScreen && a16 >= scrStart && a16 < romStart) continue;
            if (a16 == scrStart) scrBoundary = static_cast<int>(vis.size());
            if (a16 == romStart) romBoundary = static_cast<int>(vis.size());
            vis.push_back(a16);
        }
        bkBytes.resize(vis.size());
        for (size_t i = 0; i < vis.size(); ++i) bkBytes[i] = mem.peekByte(vis[i]);
        data = bkBytes.data();
        n = static_cast<int>(vis.size());
    }
    if (n <= 0) return;

    // Тепловая карта для ДОЗУ. Trace ведёт учёт по 16-разрядным адресам БК, то
    // есть «подогреть» можно только те байты платы, которые прямо сейчас выведены
    // в адресное пространство. Строим карту «смещение в подключённой странице ->
    // адрес БК» один раз за перерисовку: 9 блоков Табл. 1 по decode(), дальше
    // поиск за одно обращение.
    std::vector<int32_t> liveAddr;
    if (smkMode) {
        static const uint16_t kBlockBase[9] = {0100000, 0110000, 0120000, 0130000,
                                               0140000, 0150000, 0160000, 0170000, 0177000};
        liveAddr.assign(bk::Smk512::PAGE_BYTES, -1);
        const bk::Smk512& smk = board_->smk();
        for (int blk = 0; blk < 9; ++blk) {
            bk::Smk512::Slot s;
            if (!smk.decode(kBlockBase[blk], s)) continue;
            if (s.cell == bk::Smk512::Cell::Rom) continue;
            const int base = (blk <= 6) ? (0100000 + blk * 010000) : 0170000;
            const int lo = (blk == 8) ? 07000 : 0;
            const int hi = (blk == 7) ? 06777 : 07777;
            for (int o = lo; o <= hi; ++o)
                liveAddr[s.seg * bk::Smk512::SEG_BYTES + o] = base + o;
        }
    }
    const int livePageBase = smkMode
        ? static_cast<int>(board_->smk().page() * bk::Smk512::PAGE_BYTES) : -1;

    // Тепловая карта для БК-0011М: кусок буфера (HEAT_CH байт) -> до трёх адресов БК,
    // где он виден сейчас (страница 0 может стоять ещё и в окне 0, одна страница — в
    // обоих окнах). Куски, которые сейчас перебивает устройство МПИ (СМК, КНГМД), и
    // страница регистров не в счёт: обращения туда до памяти 11М не доходят.
    constexpr int HEAT_CH = 0200;
    std::vector<std::array<int32_t, 3>> alias11;
    if (b11Mode && heatmap) {
        alias11.assign(lin11Size / HEAT_CH, {-1, -1, -1});
        const bk::MpiDevice* mpi = mem.mpi();
        for (int blk = 0; blk < 8; ++blk) {
            int base = mem.blockRamOffset(blk);
            if (base < 0) {
                const int slot = mem.blockRomSlot(blk);
                base = (slot >= 0) ? romLin[slot] : -1;
            }
            if (base < 0) continue;
            for (int o = 0; o < static_cast<int>(M::ROM11_BYTES); o += HEAT_CH) {
                const int a = blk * static_cast<int>(M::ROM11_BYTES) + o;
                if (a >= bk::ADDR_IO_PAGE) break;
                uint16_t v;
                if (mpi && a >= bk::ADDR_RAM_END && mpi->mpiPeek(static_cast<uint16_t>(a), v)) continue;
                for (int32_t& x : alias11[(base + o) / HEAT_CH])
                    if (x < 0) { x = a; break; }
            }
        }
    }

    int rows = (n + bytesPerRow - 1) / bytesPerRow;
    if (rows < 1) rows = 1;
    const int imgH = rows;

    // Access-recency helpers (heatmap). The dim "recently touched" brightness glow
    // lingers (long FADE, sqrt curve); the coloured flash fades faster and sharper
    // so the green/red/blue access trail clears quickly. heatAt() fills the four
    // intensities for one byte; tint() applies them to a base colour.
    const int FADE = 150;      // brightness glow: frames (~3 s)
    const int FADE_COLOR = 30; // colour flash: frames (~0.6 s)
    auto heatAt = [&](uint16_t a16, double& hr, double& hw, double& he, double& act) {
        hr = hw = he = act = 0.0;
        if (!heatmap) return;
        auto flash = [&](uint32_t t) { double f = tr.fade(t, FADE_COLOR); return f * f; };
        hr = flash(tr.lastRead(a16));
        hw = flash(tr.lastWrite(a16));
        he = flash(tr.lastExec(a16));
        // A fetch also registers a read; show code as execution (blue), not a
        // data-read (green).
        if (tr.lastExec(a16) != 0 && tr.lastExec(a16) >= tr.lastRead(a16)) hr = 0;
        act = std::max({std::sqrt(tr.fade(tr.lastExec(a16),  FADE)),
                        std::sqrt(tr.fade(tr.lastRead(a16),   FADE)),
                        std::sqrt(tr.fade(tr.lastWrite(a16),  FADE))});
    };
    auto tint = [&](QRgb base, double hr, double hw, double he, double act) -> QRgb {
        if (!heatmap) return base;
        double bright = 0.5 + 0.5 * act;   // idle memory dimmer so touched bytes stand out
        int R = int(qRed(base)   * bright);
        int G = int(qGreen(base) * bright);
        int B = int(qBlue(base)  * bright);
        R = qMin(255, R + int(235 * hw));  // write -> red
        G = qMin(255, G + int(235 * hr));  // read  -> green
        B = qMin(255, B + int(235 * he));  // exec  -> blue
        return qRgb(R, G, B);
    };

    // «Тепло» байта idx. У БК адрес берётся из vis; у ДОЗУ и 11М индекс в буфере —
    // from + idx, и адрес у него есть только на подключённой странице и только там,
    // где текущий режим открыл окно.
    auto heatOf = [&](int idx, double& hr, double& hw, double& he, double& act) {
        if (!smkMode && !b11Mode) { heatAt(vis[idx], hr, hw, he, act); return; }
        hr = hw = he = act = 0.0;
        const int i = from + idx;
        if (b11Mode) {
            if (alias11.empty()) return;
            for (int32_t a : alias11[i / HEAT_CH]) {
                if (a < 0) break;
                double r, w, e, c;
                heatAt(static_cast<uint16_t>(a + i % HEAT_CH), r, w, e, c);
                hr = std::max(hr, r); hw = std::max(hw, w);
                he = std::max(he, e); act = std::max(act, c);
            }
            return;
        }
        const int off = i - livePageBase;
        if (off < 0 || off >= static_cast<int>(bk::Smk512::PAGE_BYTES)) return;
        const int32_t a = liveAddr[off];
        if (a >= 0) heatAt(static_cast<uint16_t>(a), hr, hw, he, act);
    };

    // Точки байта (1..8 бит/точку) — из таблицы lut[байт * pxPerByte + k]; cold[] — те
    // же цвета без «тепла» (с тепловой картой нетронутая память вдвое темнее: ровно
    // tint() с нулями). tint() считается только для байтов со следами обращений.
    const int lutPx = word16 ? 0 : pxPerByte;
    const int bitsPx = word16 ? 16 : 8 / pxPerByte;
    std::vector<QRgb> lut(static_cast<size_t>(256 * lutPx)), cold(lut.size());
    for (int b = 0; b < 256 && lutPx > 0; ++b)
        for (int k = 0; k < lutPx; ++k) {
            const QRgb c = pixelColor(bpp, color, static_cast<unsigned>(b >> (k * bitsPx)));
            lut[b * lutPx + k] = c;
            cold[b * lutPx + k] = heatmap ? (((c >> 1) & 0x7F7F7Fu) | 0xFF000000u) : c;
        }

    // Строка источника sy (bytesPerRow байт) -> out[0..imgW).
    auto renderRow = [&](int sy, QRgb* out) {
        std::fill(out, out + imgW, qRgb(0, 0, 0)); // trailing bytes of the last partial row stay black
        const int i0 = sy * bytesPerRow, i1 = std::min(n, i0 + bytesPerRow);
        double hr, hw, he, act;
        if (word16) {                       // one pixel per 16-bit word
            for (int i = i0; i < i1; i += 2) {
                const int x = (i - i0) / 2;
                if (x >= imgW) break;       // dangling byte of an odd-width row
                heatOf(i, hr, hw, he, act);
                uint8_t lo = data[i], hi = 0;
                if (i + 1 < i1) {
                    hi = data[i + 1];
                    double hr2, hw2, he2, act2; // pixel heat = the hotter of the word's two bytes
                    heatOf(i + 1, hr2, hw2, he2, act2);
                    hr = std::max(hr, hr2); hw = std::max(hw, hw2);
                    he = std::max(he, he2); act = std::max(act, act2);
                }
                out[x] = tint(pixelColor(16, color, lo | (hi << 8)), hr, hw, he, act);
            }
            return;
        }
        for (int i = i0; i < i1; ++i) {
            QRgb* o = out + (i - i0) * pxPerByte;
            const int b = data[i] * pxPerByte;
            if (heatmap) {
                heatOf(i, hr, hw, he, act);
                if (hr != 0 || hw != 0 || he != 0 || act != 0) {
                    for (int k = 0; k < pxPerByte; ++k) o[k] = tint(lut[b + k], hr, hw, he, act);
                    continue;
                }
            }
            std::memcpy(o, &cold[b], static_cast<size_t>(pxPerByte) * sizeof(QRgb));
        }
    };

    // Fit the byte grid into the widget. Every destination pixel OR-combines
    // (brightest-wins) all source pixels that map to it (srcSpan) — vertically and,
    // for wide rows, horizontally — instead of QImage::scaled: a plain
    // nearest-neighbour downscale silently DROPS rows when there are more memory rows
    // than pixels — and it never samples the very last row, so the highest-address
    // memory (end of the shown range: last screen line, or end of ROM/I-O when ПЗУ is
    // on) falls off the bottom of the window. Here no byte is ever invisible and the
    // last row always reaches the bottom edge; upscaling is plain nearest-neighbour.
    //
    // Сворачиваем по высоте сразу на ширине источника, строка за строкой, а по ширине
    // — уже свёрнутое. Целиком картинку не строим — у всего ДОЗУ СМК это 8192 строки,
    // и её построение 16 раз в секунду отнимало у эмуляции большую часть времени.
    //
    // Строим целевое изображение в ФИЗИЧЕСКИХ пикселях (× devicePixelRatio) и помечаем
    // его этим коэффициентом. При масштабировании экрана (HiDPI, напр. Mint 150 %)
    // width()/height() — логические пиксели; если рисовать канву в логическом размере,
    // нижние строки экрана (высокие адреса) обрезались. Теперь канва заполняется
    // физически «пиксель-в-пиксель», и низ видеопамяти виден при любом масштабе.
    const qreal dpr = devicePixelRatioF();
    const int Hd = std::max(1, static_cast<int>(std::lround(height() * dpr)));
    const int Wd = std::max(1, static_cast<int>(std::lround(width()  * dpr)));
    QImage dst(Wd, Hd, QImage::Format_RGB32);
    std::vector<QRgb> vrow(static_cast<size_t>(imgW)), tmp(vrow.size());
    std::vector<int> cx0(static_cast<size_t>(Wd)), cx1(cx0.size());
    for (int x = 0; x < Wd; ++x) srcSpan(x, imgW, Wd, cx0[x], cx1[x]);
    const size_t srcBytes = static_cast<size_t>(imgW) * sizeof(QRgb);
    const size_t dstBytes = static_cast<size_t>(Wd) * sizeof(QRgb);
    int prevS0 = -1;
    for (int dy = 0; dy < Hd; ++dy) {
        int s0, s1;
        srcSpan(dy, imgH, Hd, s0, s1);
        auto* out = reinterpret_cast<QRgb*>(dst.scanLine(dy));
        if (s0 == prevS0) {                      // upscale: the same source row again
            std::memcpy(out, dst.constScanLine(dy - 1), dstBytes);
            continue;
        }
        prevS0 = s0;
        QRgb* v = (imgW == Wd) ? out : vrow.data();
        renderRow(s0, v);
        auto* v8 = reinterpret_cast<uint8_t*>(v);
        const auto* t8 = reinterpret_cast<const uint8_t*>(tmp.data());
        for (int sy = s0 + 1; sy < s1; ++sy) {   // fold extra rows in (downscale only)
            renderRow(sy, tmp.data());
            for (size_t k = 0; k < srcBytes; ++k) v8[k] = std::max(v8[k], t8[k]); // альфа 0xFF у всех
        }
        if (v == out) continue;
        for (int x = 0; x < Wd; ++x) {
            QRgb c = v[cx0[x]];
            for (int sx = cx0[x] + 1; sx < cx1[x]; ++sx) c = maxRgb(c, v[sx]);
            out[x] = c;
        }
    }
    // Явно растягиваем всё изображение на весь ЛОГИЧЕСКИЙ прямоугольник канвы
    // (target-версия drawImage), а не полагаемся на «естественный» размер картинки и
    // её devicePixelRatio. Так канва гарантированно заполняется целиком при любом
    // способе/дробности масштабирования дисплея — низ видеопамяти не обрезается.
    p.drawImage(QRectF(0, 0, width(), height()), dst, QRectF(0, 0, Wd, Hd));

    lay_.kind = b11Mode ? Layout::Bk11 : smkMode ? Layout::Smk : Layout::Bk;
    lay_.from = from;
    lay_.n = n;
    lay_.bytesPerRow = bytesPerRow;
    lay_.pxPerByte = pxPerByte;
    lay_.word16 = word16;
    lay_.imgW = imgW;
    lay_.imgH = imgH;
    lay_.Wt = Wd;
    lay_.Hd = Hd;
    lay_.vis = std::move(vis);

    // Mark the region boundaries at their compacted positions: video RAM start
    // (blue) and ROM start (orange). Skipped regions have no boundary to draw.
    auto boundary = [&](int idx, QColor c) {
        if (idx <= 0) return;
        double y = double(idx) / bytesPerRow / imgH * height();
        if (y >= 0 && y < height()) { p.setPen(c); p.drawLine(0, int(y), width(), int(y)); }
    };
    if (b11Mode) {
        if (b11All) drawMarks11(p, romLin, imgH);
    } else if (!smkMode) {
        boundary(scrBoundary, QColor(80, 160, 255, 120));  // 0040000 video RAM
        boundary(romBoundary, QColor(255, 160, 80, 120));  // 0100000 ROM
    } else if (smkPage >= 0) {
        // Одна страница: отчёркиваем восемь сегментов по 010000.
        for (size_t o = bk::Smk512::SEG_BYTES; o < bk::Smk512::PAGE_BYTES;
             o += bk::Smk512::SEG_BYTES)
            boundary(static_cast<int>(o), QColor(255, 180, 60, 90));
    } else {
        // Все 16 страниц: сегментов было бы 128 линий на 600 точек — рисуем только
        // границы страниц, а подключённую подсвечиваем ярче.
        for (size_t o = bk::Smk512::PAGE_BYTES; o < bk::Smk512::RAM_BYTES;
             o += bk::Smk512::PAGE_BYTES)
            boundary(static_cast<int>(o), QColor(255, 160, 80, 110));
        if (livePageBase > 0) boundary(livePageBase, QColor(120, 230, 160, 200));
    }
}

// Разметка общего вида БК-0011М: границы страниц и ПЗУ-слотов с подписями слева.
// Зелёным — то, что процессор видит сейчас (с адресом), синим — экранная страница вне
// окон, ярче оранжевым — начало ПЗУ.
void MemCanvas::drawMarks11(QPainter& p, const int* romLin, int imgH) {
    using M = bk::Memory;
    const M& mem = board_->memory();
    struct Mark { int idx; QString text; QColor color; };
    std::vector<Mark> marks;
    const QColor live(120, 230, 160, 200), plain(255, 160, 80, 110);
    auto at = [](int addr) { return " · " + oct6(static_cast<unsigned>(addr)); };
    const int w0 = mem.window0Page(), w1 = mem.window1Page(), vp = mem.videoPage();
    for (int pg = 0; pg < 8; ++pg) {
        QString t = QString("стр %1").arg(pg);
        QColor c = plain;
        if (pg == 0)  t += at(0);
        if (pg == w0) t += at(0040000);
        if (pg == w1) t += at(0100000);
        if (pg == 0 || pg == w0 || pg == w1) c = live;
        if (pg == vp) { t += " · экран"; if (c != live) c = QColor(80, 160, 255, 200); }
        marks.push_back({pg * static_cast<int>(M::PAGE11_BYTES), t, c});
    }
    static const struct { int slot; const char* name; } kRom[] = {
        {M::ROM_BASIC2, "ПЗУ 328 БЕЙСИК"}, {M::ROM_BASIC3, "ПЗУ 329 БЕЙСИК"},
        {M::ROM_BASIC1, "ПЗУ 327 БЕЙСИК"}, {M::ROM_EXT, "ПЗУ 325 расш. БОС"},
        {M::ROM_BOS, "ПЗУ 324 БОС"},       {M::ROM_MSTD, "ПЗУ 330 МСТД"}};
    bool firstRom = true;
    for (const auto& r : kRom) {
        if (romLin[r.slot] < 0) continue;
        QString t = QString::fromUtf8(r.name);
        QColor c = firstRom ? QColor(255, 160, 80, 200) : QColor(255, 180, 60, 90);
        for (int blk = 0; blk < 8; ++blk)
            if (mem.blockRomSlot(blk) == r.slot) { t += at(blk * static_cast<int>(M::ROM11_BYTES)); c = live; break; }
        marks.push_back({romLin[r.slot], t, c});
        firstRom = false;
    }

    QFont f = p.font();
    f.setPixelSize(10);
    p.setFont(f);
    const QFontMetrics fm(f);
    int lastBottom = -1;
    for (const Mark& m : marks) {
        const double y = double(m.idx) / bytesPerRow / imgH * height();
        if (m.idx > 0) { p.setPen(m.color); p.drawLine(0, int(y), width(), int(y)); }
        const int ty = int(y) + 1;
        if (ty < lastBottom) continue;          // не налезаем на предыдущую подпись
        const QRect box(2, ty, fm.horizontalAdvance(m.text) + 6, fm.height());
        p.fillRect(box, QColor(0, 0, 0, 160));
        p.setPen(QColor(m.color.red(), m.color.green(), m.color.blue()));
        p.drawText(box.adjusted(3, 0, 0, 0), Qt::AlignVCenter | Qt::AlignLeft, m.text);
        lastBottom = box.bottom() + 1;
    }
}

// ---- Рисование мышью ----

void MemCanvas::syncCursor(Qt::KeyboardModifiers mods) {
    if (!paintEnabled) { unsetCursor(); return; }
    setCursor((mods & Qt::ControlModifier) ? Qt::CrossCursor : Qt::ArrowCursor);
}

// Точка картинки (в физических точках, как её строил paintEvent) под указателем.
QPoint MemCanvas::toImage(QPointF pos) const {
    const int x = static_cast<int>(std::floor(pos.x() * lay_.Wt / std::max(1, width())));
    const int y = static_cast<int>(std::floor(pos.y() * lay_.Hd / std::max(1, height())));
    return {std::clamp(x, 0, lay_.Wt - 1), std::clamp(y, 0, lay_.Hd - 1)};
}

// В адресах ЦП пишем только туда, где показано ОЗУ: ОЗУ БК или ДОЗУ СМК, открытое
// на чтение и запись. ПЗУ (своё, контроллеров, «только чтение» СМК) и регистры — нет:
// poke правит ПЗУ в обход защиты.
bool MemCanvas::bkWritable(uint16_t a) const {
    if (a >= bk::ADDR_IO_PAGE) return false;
    const bk::Memory& mem = board_->memory();
    if (a >= bk::ADDR_RAM_END) {
        bk::Smk512::Slot s;
        if (board_->smk512() && board_->smk().decode(a, s))
            return s.cell == bk::Smk512::Cell::Rw;
        uint16_t v;
        if (mem.mpi() && mem.mpi()->mpiPeek(a, v)) return false;
    }
    return mem.isRam(a);
}

int MemCanvas::readSrc(int idx) const {
    if (idx < 0 || idx >= lay_.n) return -1;
    const bk::Memory& mem = board_->memory();
    switch (lay_.kind) {
    case Layout::Bk:
        return mem.peekByte(lay_.vis[idx]);
    case Layout::Smk: {
        const auto& ram = board_->smk().ram();
        const size_t o = static_cast<size_t>(lay_.from + idx);
        return board_->smk512() && o < ram.size() ? ram[o] : -1;
    }
    case Layout::Bk11: {
        const size_t o = static_cast<size_t>(lay_.from + idx);
        return mem.bk11() && o < bk::Memory::RAM11_BYTES ? mem.ram11()[o] : -1;  // дальше — ПЗУ
    }
    default:
        return -1;
    }
}

void MemCanvas::writeSrc(int idx, uint8_t v) {
    const int old = readSrc(idx);
    if (old < 0 || (lay_.kind == Layout::Bk && !bkWritable(lay_.vis[idx]))) {
        strokeRefused_ = true;
        return;
    }
    if (old == v) return;
    bk::Memory& mem = board_->memory();
    switch (lay_.kind) {
    case Layout::Bk:   mem.pokeByte(lay_.vis[idx], v); break;
    case Layout::Smk:  board_->smk().ram()[static_cast<size_t>(lay_.from + idx)] = v; break;
    case Layout::Bk11: mem.ram11()[lay_.from + idx] = v; break;
    default: return;
    }
    strokeBytes_.insert(idx);
}

// Точка окна (x, y) показывает прямоугольник точек источника (srcSpan по обеим осям):
// при уменьшении — несколько строк и столбцов, и красятся все, чтобы точка окна
// стала того цвета, каким рисуют.
void MemCanvas::paintCell(int x, int y) {
    int sy0, sy1, sx0, sx1;
    srcSpan(y, lay_.imgH, lay_.Hd, sy0, sy1);
    srcSpan(x, lay_.imgW, lay_.Wt, sx0, sx1);
    const int bpr = lay_.bytesPerRow;
    for (int sy = sy0; sy < sy1; ++sy)
        for (int sx = sx0; sx < sx1; ++sx) {
            if (lay_.word16) {                 // точка = слово, младший байт первым
                const int bx = sx * 2, idx = sy * bpr + bx;
                if (idx >= lay_.n) continue;
                writeSrc(idx, static_cast<uint8_t>(strokeValue_ & 0xff));
                if (bx + 1 < bpr && idx + 1 < lay_.n)
                    writeSrc(idx + 1, static_cast<uint8_t>((strokeValue_ >> 8) & 0xff));
                continue;
            }
            const int ppb = lay_.pxPerByte, bits = 8 / ppb;
            const int idx = sy * bpr + sx / ppb;
            const int old = readSrc(idx);
            if (old < 0) { strokeRefused_ = true; continue; }
            const int shift = (sx % ppb) * bits;
            const unsigned mask = ((1u << bits) - 1) << shift;
            writeSrc(idx, static_cast<uint8_t>((static_cast<unsigned>(old) & ~mask)
                                               | ((strokeValue_ << shift) & mask)));
        }
}

// Отрезок между двумя точками картинки (Брезенхем): быстрое движение мыши
// не оставляет пропусков.
void MemCanvas::paintLine(QPoint a, QPoint b) {
    const int dx = std::abs(b.x() - a.x()), dy = -std::abs(b.y() - a.y());
    const int sx = a.x() < b.x() ? 1 : -1, sy = a.y() < b.y() ? 1 : -1;
    int err = dx + dy, x = a.x(), y = a.y();
    for (;;) {
        paintCell(x, y);
        if (x == b.x() && y == b.y()) break;
        const int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x += sx; }
        if (e2 <= dx) { err += dx; y += sy; }
    }
}

void MemCanvas::mousePressEvent(QMouseEvent* e) {
    const bool btn = e->button() == Qt::LeftButton || e->button() == Qt::RightButton;
    if (!paintEnabled || !(e->modifiers() & Qt::ControlModifier) || !btn
        || lay_.kind == Layout::None || stroke_) {
        QWidget::mousePressEvent(e);
        return;
    }
    stroke_ = true;
    strokeValue_ = e->button() == Qt::LeftButton ? paintValue : 0;
    strokeBytes_.clear();
    strokeRefused_ = false;
    lastPx_ = toImage(e->position());
    paintLine(lastPx_, lastPx_);
    update();
}

void MemCanvas::mouseMoveEvent(QMouseEvent* e) {
    syncCursor(e->modifiers());
    if (!stroke_) { QWidget::mouseMoveEvent(e); return; }
    // Отпустили Ctrl — мазок кончился, даже если кнопка ещё нажата.
    if (!(e->modifiers() & Qt::ControlModifier) || lay_.kind == Layout::None) {
        stroke_ = false;
        emit strokeDone(static_cast<int>(strokeBytes_.size()), strokeRefused_);
        return;
    }
    const QPoint p = toImage(e->position());
    if (p == lastPx_) return;
    paintLine(lastPx_, p);
    lastPx_ = p;
    update();
}

void MemCanvas::mouseReleaseEvent(QMouseEvent* e) {
    if (!stroke_) { QWidget::mouseReleaseEvent(e); return; }
    stroke_ = false;
    emit strokeDone(static_cast<int>(strokeBytes_.size()), strokeRefused_);
}

MemVisWidget::MemVisWidget(Board* board, QWidget* parent) : QWidget(parent), board_(board) {
    canvas_ = new MemCanvas(board, this);

    auto* bpp = new QComboBox;
    bpp->addItems({"1 бит", "2 бита (БК)", "4 бита", "8 бит", "16 бит"});
    bpp->setCurrentIndex(1);   // по умолчанию — как цветной экран БК: 2 бита на точку
    auto* mode = new QComboBox; mode->addItems({"Ч/Б", "Цвет"}); mode->setCurrentIndex(1);
    auto* heat = new QCheckBox("Тепловая карта"); heat->setChecked(true);
    auto* showRom = new QCheckBox("Показать ПЗУ"); // ROM hidden by default
    auto* showScreen = new QCheckBox("Показать экран"); showScreen->setChecked(true); // video RAM shown by default
    auto* addr = new QSpinBox; addr->setRange(0, 0xFFFF); addr->setDisplayIntegerBase(8);
    addr->setPrefix("адрес 0"); addr->setSingleStep(0100); addr->setValue(0);
    auto* row = new QSpinBox; row->setRange(8, 512); row->setValue(64); row->setPrefix("шир ");

    // Рисование мышью: по умолчанию выключено, и даже включённое пишет только с Ctrl.
    auto* paint = new QCheckBox("Рисовать мышью");
    paint->setToolTip("Ctrl + левая кнопка — рисовать выбранным значением точки,\n"
                      "Ctrl + правая — нулём. ПЗУ и регистры не меняются.");
    auto* value = new QSpinBox; value->setDisplayIntegerBase(8); value->setPrefix("значение 0");
    value->setRange(0, 3); value->setValue(3);
    auto* swatch = new QLabel;
    swatch->setFixedSize(18, 18);
    swatch->setFrameShape(QFrame::Box);
    paintHint_ = new QLabel("Ctrl + левая кнопка — значением, Ctrl + правая — нулём");

    // Источник картинки: адреса процессора, ОЗУ БК-0011М или ДОЗУ платы СМК-512.
    // Список собирает syncSources() — под текущую машину и плату.
    src_ = new QComboBox;
    auto* src = src_;

    auto apply = [=] {
        static const int bppVals[5] = {1, 2, 4, 8, 16};
        canvas_->bpp = bppVals[std::clamp(bpp->currentIndex(), 0, 4)];
        canvas_->color = (mode->currentIndex() == 1);
        canvas_->heatmap = heat->isChecked();
        canvas_->hideRom = !showRom->isChecked();
        canvas_->hideScreen = !showScreen->isChecked();
        canvas_->startAddr = addr->value();
        canvas_->bytesPerRow = row->value();
        const int code = src->currentData().toInt();   // Src << 8 | страница
        canvas_->src = static_cast<MemCanvas::Src>(code >> 8);
        canvas_->page = code & 0377;
        // В постраничных режимах экран прятать нечего и начальный адрес БК ни при чём;
        // ПЗУ есть только в общем виде БК-0011М.
        const bool bkSrc = canvas_->src == MemCanvas::Src::Bk;
        showRom->setEnabled(bkSrc || canvas_->src == MemCanvas::Src::Bk11All);
        showScreen->setEnabled(bkSrc);
        addr->setEnabled(bkSrc);
        // Значение точки — столько бит, сколько в точке при текущем режиме.
        {
            const QSignalBlocker b(value);
            value->setMaximum(canvas_->bpp >= 16 ? 0177777 : (1 << canvas_->bpp) - 1);
        }
        canvas_->paintEnabled = paint->isChecked();
        canvas_->setMouseTracking(canvas_->paintEnabled);   // курсор — по Ctrl при наведении
        canvas_->paintValue = static_cast<unsigned>(value->value());
        canvas_->syncCursor(QGuiApplication::queryKeyboardModifiers());
        value->setEnabled(canvas_->paintEnabled);
        swatch->setEnabled(canvas_->paintEnabled);
        paintHint_->setEnabled(canvas_->paintEnabled);
        QPixmap sw(16, 16);
        sw.fill(QColor::fromRgb(MemCanvas::pixelColor(canvas_->bpp, canvas_->color, canvas_->paintValue)));
        swatch->setPixmap(sw);
        canvas_->update();
    };
    connect(bpp,  QOverload<int>::of(&QComboBox::currentIndexChanged), this, [=](int){ apply(); });
    connect(mode, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [=](int){ apply(); });
    connect(heat, &QCheckBox::toggled, this, [=](bool){ apply(); });
    connect(showRom, &QCheckBox::toggled, this, [=](bool){ apply(); });
    connect(showScreen, &QCheckBox::toggled, this, [=](bool){ apply(); });
    connect(addr, QOverload<int>::of(&QSpinBox::valueChanged), this, [=](int){ apply(); });
    connect(row,  QOverload<int>::of(&QSpinBox::valueChanged), this, [=](int){ apply(); });
    connect(src,  QOverload<int>::of(&QComboBox::currentIndexChanged), this, [=](int){ apply(); });
    connect(paint, &QCheckBox::toggled, this, [=](bool){ apply(); });
    connect(value, QOverload<int>::of(&QSpinBox::valueChanged), this, [=](int){ apply(); });
    connect(canvas_, &MemCanvas::strokeDone, this, [this](int changed, bool refused) {
        QString t = QString("изменено байт: %1").arg(changed);
        if (refused) t += " (ПЗУ и регистры не тронуты)";
        paintHint_->setText(t);
    });

    auto* controls = new QHBoxLayout;
    controls->addWidget(src);
    controls->addWidget(new QLabel("бит/пиксель:")); controls->addWidget(bpp);
    controls->addWidget(mode); controls->addWidget(heat); controls->addWidget(showRom);
    controls->addWidget(showScreen);
    controls->addWidget(addr); controls->addWidget(row);
    controls->addStretch();
    auto* paintRow = new QHBoxLayout;
    paintRow->addWidget(paint); paintRow->addWidget(value); paintRow->addWidget(swatch);
    paintRow->addWidget(paintHint_);
    paintRow->addStretch();
    paintRow->setContentsMargins(0, 0, 0, 0);

    auto* lay = new QVBoxLayout(this);
    lay->setContentsMargins(2, 2, 2, 2);   // почти без отступа до рамки окна
    lay->setSpacing(2);
    controls->setContentsMargins(0, 0, 0, 0);
    lay->addLayout(controls);
    lay->addLayout(paintRow);
    lay->addWidget(canvas_, 1);
    apply_ = apply;
    syncSources();
    resize(560, 640);
}

void MemVisWidget::syncSources() {
    using Src = MemCanvas::Src;
    const bool b11 = board_->bk11(), smk = board_->smk512();
    const int key = (b11 ? 1 : 0) | (smk ? 2 : 0);
    if (key == srcMachine_) return;
    const bool machineChanged = srcMachine_ < 0 || ((srcMachine_ ^ key) & 1);
    srcMachine_ = key;
    const QVariant prev = src_->currentData();
    {
        const QSignalBlocker block(src_);
        auto code = [](Src s, int pg = 0) { return (static_cast<int>(s) << 8) | pg; };
        src_->clear();
        src_->addItem("Память БК (адреса ЦП)", code(Src::Bk));
        if (b11) {
            src_->addItem("11М: вся память", code(Src::Bk11All));
            for (int i = 0; i < 8; ++i)
                src_->addItem(QString("11М: страница %1").arg(i), code(Src::Bk11Page, i));
        }
        if (smk) {
            src_->addItem("СМК: все 16 страниц", code(Src::SmkAll));
            for (int i = 0; i < static_cast<int>(bk::Smk512::PAGES); ++i)
                src_->addItem(QString("СМК: страница %1").arg(i), code(Src::SmkPage, i));
        }
        // Сменилась машина — на 11М сразу вся память; иначе прежний выбор, если он ещё есть.
        int idx = machineChanged ? -1 : src_->findData(prev);
        if (idx < 0) idx = b11 ? 1 : 0;
        src_->setCurrentIndex(idx);
        src_->setEnabled(src_->count() > 1);
    }
    setWindowTitle(QString("Визуализация памяти %1")
                   .arg(QString::fromUtf8(Board::machineName(board_->machine()))));
    if (apply_) apply_();
}

// Throttle the periodic repaint to ~16 Hz. Rebuilding the ~48 KB heatmap image
// every 50 Hz tick is costly and steals time from the emulation on this same GUI
// thread; the access-age fade is slow enough that 16 Hz looks identical.
// Interactive control changes call canvas_->update() directly and are unaffected.
// Ctrl нажат/отпущен без движения мыши — курсор над канвой меняется сразу.
void MemVisWidget::keyPressEvent(QKeyEvent* e) {
    if (e->key() == Qt::Key_Control) canvas_->syncCursor(Qt::ControlModifier);
    QWidget::keyPressEvent(e);
}

void MemVisWidget::keyReleaseEvent(QKeyEvent* e) {
    if (e->key() == Qt::Key_Control) canvas_->syncCursor(Qt::NoModifier);
    QWidget::keyReleaseEvent(e);
}

void MemVisWidget::refresh() {
    syncSources();   // машину и плату могут сменить при открытом окне
    if (++refreshTick_ >= 3) { refreshTick_ = 0; canvas_->update(); }
}
