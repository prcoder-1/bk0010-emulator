#include <QApplication>
#include <QSurfaceFormat>
#include <QDir>
#include <QFileInfo>
#include <QImage>
#include <QPainter>
#include <QPixmap>
#include <cstdio>
#include <vector>
#include <cstdint>
#include "ui/MainWindow.h"
#include "ui/DebuggerOverlay.h"
#include "ui/MemVisWidget.h"
#include "ui/SmkRamWidget.h"
#include "ui/HotPathWidget.h"
#include "ui/CallGraphWidget.h"
#include "ui/FlameWidget.h"
#include "ui/FlameChartWidget.h"
#include "ui/HotChartWidget.h"
#include <QMouseEvent>
#include "ui/BkKeymap.h"
#include "mcp/McpServer.h"
#include "Board.h"
#include "Fdd.h"
#include "Disasm.h"
#include <QKeyEvent>
#include <QGuiApplication>
#include <QSettings>

// Self-test of the Qt-key -> BK-code translation (Cyrillic, РУС/ЛАТ, Ctrl, specials).
static int runKeyTest() {
    BkKeymap km;
    struct Case { int key; Qt::KeyboardModifiers mods; const char* text; const char* label; };
    const Case cases[] = {
        {Qt::Key_A, Qt::NoModifier, "A", "Latin A"},
        {Qt::Key_1, Qt::NoModifier, "1", "digit 1"},
        {0,         Qt::NoModifier, "б", "Cyrillic б (switch to РУС)"},
        {0,         Qt::NoModifier, "а", "Cyrillic а (stay РУС)"},
        {Qt::Key_A, Qt::NoModifier, "A", "Latin A (switch to ЛАТ)"},
        {Qt::Key_Left,  Qt::NoModifier, "",  "Left arrow"},
        {Qt::Key_Return,Qt::NoModifier, "\r","Enter"},
        {Qt::Key_C, Qt::ControlModifier, "\x03", "Ctrl+C"},
    };
    for (const auto& c : cases) {
        QKeyEvent e(QEvent::KeyPress, c.key, c.mods, QString::fromUtf8(c.text));
        auto codes = km.translate(&e);
        std::printf("%-32s ->", c.label);
        for (uint16_t x : codes) std::printf(" %04o", x);
        std::printf("\n");
    }
    // РУС / ЛАТ mapped to the two Shift keys (told apart by the X keysym).
    struct { quint32 vk; const char* label; } shifts[] = {
        {0xffe1, "Left Shift  -> РУС (016)"},
        {0xffe2, "Right Shift -> ЛАТ (017)"},
    };
    for (const auto& c : shifts) {
        QKeyEvent e(QEvent::KeyPress, Qt::Key_Shift, Qt::ShiftModifier, 0, c.vk, 0);
        auto codes = km.translate(&e);
        std::printf("%-32s ->", c.label);
        for (uint16_t x : codes) std::printf(" %04o", x);
        std::printf("\n");
    }
    return 0;
}

// Арбитраж КР1801ВП1-037: доступ к ДОЗУ квантован окном процессора (раз в 4 такта),
// и команда, промахнувшаяся мимо окна, теряет 4 такта. По умолчанию ВКЛ; ключ
// --no-arb037 возвращает «идеальную» память (тайминг статического ОЗУ).
static bool g_arb037 = true;
// --cpu-phase 0|1: фаза такта ЦП относительно окна 037 — на железе выпадает при
// включении питания; 0 (по умолчанию) — `MOV (R0),R1` = 28 тактов, 1 — 24.
static int  g_cpuPhase = 0;
static bool g_scanline = false;   // --scanline: построчная отрисовка экрана
// --machine bk10|bk11m (--bk10, --bk11m): эмулируемая машина. -1 — ключ не задан
// (в GUI берётся сохранённая настройка, по умолчанию БК-0011М; в безголовом режиме
// и MCP — БК-0011М; с .BIN в командной строке — по программе, см. Board::binTargetsBk11).
static int  g_machine = -1;
// --ay / --no-ay: звуковой сопроцессор AY-3-8910 на 0177714. -1 — по машине
// (на БК-0011М включён, на БК-0010 выключен) или сохранённая настройка GUI.
static int  g_ay = -1;
// --no-scanline: выключить построчную отрисовку (на БК-0011М она по умолчанию ВКЛ).
static bool g_noScanline = false;
// --mstd: вставить блок МСТД (ПЗУ 330) в окно 0160000 БК-0011М.
static bool g_mstd = false;

// Разбор значения --machine: bk10 / 0010 / bk0010 → 0; bk11m / 0011m / bk0011m → 1.
static int parseMachine(const QString& v) {
    const QString s = v.toLower();
    if (s.contains("11")) return 1;
    if (s.contains("10")) return 0;
    std::fprintf(stderr, "Неизвестная машина «%s» (bk10 | bk11m)\n", qPrintable(v));
    return -1;
}
// --smk / --no-smk: блок расширения памяти СМК-512. -1 — ключ не задан (в GUI
// берётся сохранённая настройка, по умолчанию ВКЛ; в безголовом режиме и в MCP — ВКЛ).
static int  g_smk = -1;
// --aspect34 / --no-aspect34: неквадратный пиксель БК (блок 4x3 точки хоста, кадр
// 1024x768). -1 — ключ не задан, в GUI берётся сохранённая настройка (по умолчанию
// ВКЛ). На безголовый режим не влияет: --shot пишет буфер как есть, 512x256.
static int  g_aspect34 = -1;
// --disk <образ>: вставить дискету в привод 0 и передать управление автозагрузчику
// в ПЗУ контроллера. По умолчанию образ открывается ТОЛЬКО НА ЧТЕНИЕ — чужой
// рабочий образ портить нельзя; --disk-rw разрешает запись обратно в файл.
static QString g_disk, g_diskB;
static bool    g_diskRw = false;
static int     g_diskSides = 0;      // 0 — по размеру образа, 1 или 2 — принудительно
static QString g_diskRom;            // --disk-rom smk|326|<файл>: прошивка контроллера
// --hdd / --hdd-b <образ.hdi>: винчестер (master / slave) на плате СМК-512. Как и
// дискета, по умолчанию только на чтение; --hdd-rw пишет изменения в файл.
static QString g_hdd, g_hddB;
static bool    g_hddRw = false;

// Headless verification: boot the monitor, optionally load a .BIN, run N frames
// and save the screen (rendered from the CPU-side pixel buffer, no GL needed).
static int runHeadless(const QString& romDir, const QString& bin,
                       int frames, bool color, const QString& shot,
                       int keyCode, int keyFrame, const QString& dbgShot,
                       const QString& memvisShot, const QString& hpShot,
                       const QString& caShot, const QString& faShot, const QString& fcShot,
                       const QString& hcShot,
                       const QString& typeStr, const QString& keysList) {
    // Parse "frame:code,frame:code,..." into precisely-timed key injections.
    std::vector<std::pair<int,int>> keys;
    for (const QString& part : keysList.split(',', Qt::SkipEmptyParts)) {
        const auto fc = part.split(':');
        if (fc.size() == 2) keys.push_back({fc[0].toInt(), fc[1].toInt(nullptr, 0)});
    }
    bk::Board board;
    board.setMachine(g_machine == 0 ? bk::Machine::Bk0010 : bk::Machine::Bk0011m);
    if (g_ay >= 0) board.setAy(g_ay != 0);
    board.setMstd(g_mstd);
    board.setArbitration(g_arb037);
    board.setCpuPhase(g_cpuPhase);
    board.setScanlineRender((g_scanline || board.bk11()) && !g_noScanline);
    board.setSmk512(g_smk != 0);              // по умолчанию плата стоит
    if (!board.loadRoms(romDir.toStdString())) {
        std::fprintf(stderr, "headless: failed to load ROMs from %s\n", qPrintable(romDir));
        return 2;
    }
    board.reset();
    if (!memvisShot.isEmpty() || !hpShot.isEmpty() || !caShot.isEmpty() || !hcShot.isEmpty()
        || !dbgShot.isEmpty() || !faShot.isEmpty() || !fcShot.isEmpty())
        board.trace().setEnabled(true);
    if (!faShot.isEmpty() || !fcShot.isEmpty()) board.trace().setFlameEnabled(true);
    if (!fcShot.isEmpty()) board.trace().setSpansEnabled(true);
    // Let the monitor ROM initialise (vectors, stack, display driver) before
    // jumping into a game.
    for (int i = 0; i < 25; ++i) board.runFrame();
    // Дискета: вставляем после того, как поднялся монитор, и сразу отдаём
    // управление автозагрузчику ПЗУ контроллера — как «160000G» в мониторе.
    if (!g_disk.isEmpty()) {
        if (!g_diskRom.isEmpty()) board.setDiskRom(g_diskRom.toStdString());
        if (!g_diskB.isEmpty())
            board.attachDisk(1, g_diskB.toStdString(), !g_diskRw, g_diskSides);
        if (!board.attachDisk(0, g_disk.toStdString(), !g_diskRw, g_diskSides)) {
            std::fprintf(stderr, "headless: не удалось вставить образ %s\n", qPrintable(g_disk));
            return 6;
        }
        board.bootFromDisk();
        std::printf("headless: загрузка с диска %s\n", qPrintable(g_disk));
    }
    // Винчестер: прошивка СМК перехватывает пуск и грузит систему с него сама, поэтому
    // после подключения образа — включение питания.
    if (!g_hdd.isEmpty() || !g_hddB.isEmpty()) {
        if (!g_diskRom.isEmpty()) board.setDiskRom(g_diskRom.toStdString());
        if (!g_hddB.isEmpty() && !board.attachHdd(1, g_hddB.toStdString(), !g_hddRw)) return 7;
        if (!g_hdd.isEmpty() && !board.attachHdd(0, g_hdd.toStdString(), !g_hddRw)) return 7;
        if (board.bootFromHdd())
            std::printf("headless: загрузка с винчестера %s\n", qPrintable(g_hdd.isEmpty() ? g_hddB : g_hdd));
        else
            std::fprintf(stderr, "headless: винчестер подключён, но прошивка контроллера не "
                                 "перехватывает пуск (--disk-rom?) — загрузки с него не будет\n");
    }
    if (!bin.isEmpty() && !board.loadBin(bin.toStdString(), true)) {
        std::fprintf(stderr, "headless: failed to load bin %s\n", qPrintable(bin));
        return 3;
    }
    board.screen().setColorMode(color);
    // Optional: type a string, one character every few frames, through the same
    // keyboard queue the GUI uses (verifies the key -> register -> ISR -> echo path).
    int typeIdx = 0;
    for (int i = 0; i < frames; ++i) {
        if (keyCode >= 0 && i == keyFrame) board.pressKey(static_cast<uint16_t>(keyCode));
        for (const auto& kv : keys) if (kv.first == i) board.pressKey(static_cast<uint16_t>(kv.second));
        if (!typeStr.isEmpty() && i >= 60 && typeIdx < typeStr.size() && !board.keyReady()) {
            QChar c = typeStr[typeIdx++];
            uint16_t code = (c == '\n') ? 012 : static_cast<uint16_t>(c.unicode() & 0x7f);
            board.pressKey(code);
        }
        board.runFrame();
    }
    if (!board.scanlineRender()) board.screen().render(board.memory());

    QImage img(reinterpret_cast<const uchar*>(board.screen().pixels()),
               bk::Screen::TEX_W, bk::Screen::TEX_H, QImage::Format_ARGB32);
    QImage out = img.copy(); // detach from the board's buffer
    if (!shot.isEmpty()) {
        if (!out.save(shot)) { std::fprintf(stderr, "headless: cannot save %s\n", qPrintable(shot)); return 4; }
        std::printf("headless: ran %d frames, wrote %s\n", frames, qPrintable(shot));
    }
    if (!dbgShot.isEmpty()) {
        // Composite the Soft-ICE overlay over the (scaled) BK screen.
        QImage big = out.scaled(1024, 768).convertToFormat(QImage::Format_ARGB32);
        DebuggerOverlay ov(&board);
        ov.resize(1024, 768);
        ov.followPc();
        ov.render(&big, QPoint(), QRegion(), QWidget::DrawChildren);
        if (!big.save(dbgShot)) { std::fprintf(stderr, "headless: cannot save %s\n", qPrintable(dbgShot)); return 5; }
        std::printf("headless: wrote debugger overlay %s (PC=%06o)\n", qPrintable(dbgShot), board.cpu().pc());
    }
    if (!memvisShot.isEmpty()) {
        MemVisWidget w(&board);
        w.resize(560, 640);
        w.refresh();
        QPixmap pm = w.grab();
        pm.save(memvisShot); // default: ROM hidden, RAM fills the window
        std::printf("headless: wrote memvis (RAM only) %s\n", qPrintable(memvisShot));
        // Verification: the same view with ROM shown (full 64 KB, smaller scale).
        MemCanvas cv(&board);
        cv.resize(560, 560);
        cv.hideRom = false;
        QString withRomPath = memvisShot; withRomPath.replace(".png", "_withrom.png");
        cv.grab().save(withRomPath);
        std::printf("headless: wrote memvis (RAM+ROM) %s\n", qPrintable(withRomPath));
        // Verification: video RAM hidden and ROM shown — the ROM compacts up to
        // fill the space the screen would occupy, and the image fills the window
        // (no blank gap for the hidden region).
        MemCanvas cs(&board);
        cs.resize(560, 640);
        cs.hideScreen = true;
        cs.hideRom = false;
        QString noScreenPath = memvisShot; noScreenPath.replace(".png", "_noscreen.png");
        cs.grab().save(noScreenPath);
        std::printf("headless: wrote memvis (screen hidden, ROM shown) %s\n", qPrintable(noScreenPath));
        // Вид по умолчанию — 2 бита/пиксель в палитре БК (экранное ОЗУ выглядит как
        // сама картинка), поэтому проверяем заодно и байтовый режим 4 бита/пиксель.
        MemCanvas c4(&board);
        c4.resize(560, 640);
        c4.bpp = 4;
        QString bk4Path = memvisShot; bk4Path.replace(".png", "_4bpp.png");
        c4.grab().save(bk4Path);
        std::printf("headless: wrote memvis (4 бита/пиксель) %s\n", qPrintable(bk4Path));
        // С установленной платой СМК-512 — заодно её ДОЗУ: все 16 страниц подряд,
        // отдельно подключённая страница и дамп окна «ДОЗУ по страницам».
        if (board.smk512()) {
            MemCanvas ca(&board);
            ca.resize(560, 640);
            ca.src = MemCanvas::Src::SmkAll;
            QString allPath = memvisShot; allPath.replace(".png", "_smkall.png");
            ca.grab().save(allPath);
            std::printf("headless: wrote memvis (СМК, все 16 страниц) %s\n", qPrintable(allPath));

            MemCanvas cp(&board);
            cp.resize(560, 640);
            cp.src = MemCanvas::Src::SmkPage;
            cp.page = board.smk().page();
            QString pgPath = memvisShot; pgPath.replace(".png", "_smkpage.png");
            cp.grab().save(pgPath);
            std::printf("headless: wrote memvis (СМК, страница %d) %s\n",
                        board.smk().page(), qPrintable(pgPath));

            SmkRamWidget sr(&board);
            sr.resize(900, 620);
            QString srPath = memvisShot; srPath.replace(".png", "_smkram.png");
            sr.grab().save(srPath);
            std::printf("headless: wrote ДОЗУ СМК по страницам %s\n", qPrintable(srPath));
        }
        // БК-0011М: всё ОЗУ (8 страниц), оно же с ПЗУ-слотами и экранная страница.
        if (board.bk11()) {
            auto shot11 = [&](MemCanvas::Src s, int page, bool rom, const char* suffix, const char* what) {
                MemCanvas c(&board);
                c.resize(560, 640);
                c.src = s;
                c.page = page;
                c.hideRom = !rom;
                QString path = memvisShot; path.replace(".png", suffix);
                c.grab().save(path);
                std::printf("headless: wrote memvis (%s) %s\n", what, qPrintable(path));
            };
            shot11(MemCanvas::Src::Bk11All, 0, false, "_11all.png", "11М, всё ОЗУ");
            shot11(MemCanvas::Src::Bk11All, 0, true, "_11allrom.png", "11М, ОЗУ и ПЗУ");
            shot11(MemCanvas::Src::Bk11Page, board.memory().videoPage(), false, "_11page.png",
                   "11М, экранная страница");
        }
    }
    if (!hpShot.isEmpty()) {
        HotPathWidget w(&board);
        w.resize(760, 620);
        w.refresh();
        w.grab().save(hpShot);
        std::printf("headless: wrote hot path %s\n", qPrintable(hpShot));
        // Expand the top path, then its first block, and capture the detail view.
        auto click = [&](int x, int y) {
            QMouseEvent pr(QEvent::MouseButtonPress, QPointF(x, y), QPointF(x, y),
                           Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
            QMouseEvent rl(QEvent::MouseButtonRelease, QPointF(x, y), QPointF(x, y),
                           Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
            QApplication::sendEvent(&w, &pr); QApplication::sendEvent(&w, &rl);
        };
        w.grab();  click(12, 30);        // triangle of path #1 → expand
        w.grab();  click(24, 52);        // triangle of its first block → expand
        QString exp = hpShot; exp.replace(".png", "_expanded.png");
        w.grab().save(exp);
        std::printf("headless: wrote hot path (expanded) %s\n", qPrintable(exp));
    }
    if (!caShot.isEmpty()) {
        CallGraphWidget w(&board);
        w.resize(940, 700);
        w.refresh();
        w.grab().save(caShot);
        std::printf("headless: wrote call graph %s\n", qPrintable(caShot));
        // Exercise wheel-zoom and capture a second, zoomed-in image (also shows
        // the per-box instruction disassembly that appears at high zoom).
        for (int k = 0; k < 14; ++k) {
            QWheelEvent we(QPointF(470, 350), QPointF(470, 350), QPoint(), QPoint(0, 120),
                           Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
            QApplication::sendEvent(&w, &we);
        }
        QString zoomPath = caShot; zoomPath.replace(".png", "_zoom.png");
        w.grab().save(zoomPath);
        std::printf("headless: wrote call graph (zoom) %s\n", qPrintable(zoomPath));
        // Toggle to the nested-rectangles (treemap) view and capture it.
        QKeyEvent tab(QEvent::KeyPress, Qt::Key_Tab, Qt::NoModifier);
        QApplication::sendEvent(&w, &tab);
        QString treePath = caShot; treePath.replace(".png", "_tree.png");
        w.grab().save(treePath);
        std::printf("headless: wrote call treemap %s\n", qPrintable(treePath));
        // Back to the graph, then purge the darkened (long-idle) functions (Delete).
        QApplication::sendEvent(&w, &tab);
        QKeyEvent del(QEvent::KeyPress, Qt::Key_Delete, Qt::NoModifier);
        QApplication::sendEvent(&w, &del);
        QString purgePath = caShot; purgePath.replace(".png", "_purged.png");
        w.grab().save(purgePath);
        std::printf("headless: wrote call graph after purge %s\n", qPrintable(purgePath));
    }
    if (!faShot.isEmpty()) {
        FlameWidget w(&board);
        w.resize(900, 560);
        w.refresh();
        w.grab().save(faShot);
        std::printf("headless: wrote flame graph %s\n", qPrintable(faShot));
    }
    if (!fcShot.isEmpty()) {
        FlameChartWidget w(&board);
        w.resize(940, 480);
        w.refresh();
        w.grab().save(fcShot);
        std::printf("headless: wrote flame chart %s\n", qPrintable(fcShot));
    }
    if (!hcShot.isEmpty()) {
        // Build a real time-series: interleave running frames with the widget's
        // sampler so its history fills up as if driven by the 50 Hz GUI loop.
        HotChartWidget w(&board);
        w.resize(880, 480);
        for (int k = 0; k < 500; ++k) { board.runFrame(); w.refresh(); }
        w.grab().save(hcShot);
        std::printf("headless: wrote hot chart %s\n", qPrintable(hcShot));
        // Exercise the right-click "hide" on a legend row (a second grab lays out
        // the legend; the header then shows "(скрыто 1)").
        w.grab();
        QMouseEvent rc(QEvent::MouseButtonPress, QPointF(650, 62),
                       Qt::RightButton, Qt::RightButton, Qt::NoModifier);
        QApplication::sendEvent(&w, &rc);
        for (int k = 0; k < 20; ++k) { board.runFrame(); w.refresh(); }
        QString hidePath = hcShot; hidePath.replace(".png", "_hidden.png");
        w.grab().save(hidePath);
        std::printf("headless: wrote hot chart after hide %s\n", qPrintable(hidePath));
    }

    // Report speaker sample activity (verifies the audio sample path).
    {
        std::vector<int16_t> s(board.sound().available());
        size_t n = s.empty() ? 0 : board.sound().read(s.data(), s.size());
        int16_t lo = 0, hi = 0;
        for (size_t i = 0; i < n; ++i) { if (s[i] < lo) lo = s[i]; if (s[i] > hi) hi = s[i]; }
        std::printf("headless: audio samples=%zu range=[%d..%d]%s\n", n, lo, hi,
                    (hi - lo > 1000) ? " (speaker active)" : " (silent)");
    }

    // Report whether the screen has any non-black pixels (sanity signal).
    int nonblack = 0;
    const uint32_t* px = board.screen().pixels();
    for (int i = 0; i < bk::Screen::TEX_W * bk::Screen::TEX_H; ++i)
        if ((px[i] & 0x00FFFFFF) != 0) ++nonblack;
    std::printf("headless: PC=%06o non-black pixels=%d\n", board.cpu().pc(), nonblack);
    return 0;
}

// Справка по ключам командной строки (--help, -h). Печатается до создания окна и
// до разбора остальных ключей.
static void printHelp(const char* prog) {
    std::printf("Эмулятор-отладчик БК-0010-01 / БК-0011М\n\n"
                "Запуск:\n"
                "  %s [ключи] [игра.bin | диск.bkd | диск.img | винчестер.hdi]\n\n", prog);
    std::fputs(
        "Позиционный аргумент: .bkd и .img (в любом регистре) — образ дискеты, с него сразу\n"
        "идёт загрузка; .hdi — образ винчестера (то же, что --hdd); любой другой файл —\n"
        "программа в формате .BIN: машину выбирает она сама (пишет в регистр 0177662 —\n"
        "БК-0011М, иначе БК-0010-01), --machine задаёт явно.\n"
        "\n"
        "Машина:\n"
        "  --machine bk10|bk11m   эмулируемая машина; по умолчанию БК-0011М (в GUI — выбор\n"
        "                         из меню «Машина»), а с .BIN — по программе\n"
        "  --bk10, --bk11m        то же коротко\n"
        "  --ay, --no-ay          звуковой сопроцессор AY-3-8910 на порту 0177714\n"
        "                         (по умолчанию: на БК-0011М вкл, на БК-0010 выкл)\n"
        "  --smk, --no-smk        блок расширения памяти СМК-512 (512 Кбайт; по умолчанию вкл)\n"
        "  --mstd                 БК-0011М: вставить тестовый блок МСТД в окно 0160000\n"
        "  --roms <каталог>       каталог ПЗУ (иначе BK_ROM_DIR или каталог при сборке)\n"
        "\n"
        "Дисковод:\n"
        "  --disk <образ>         вставить дискету в привод A и загрузиться с неё\n"
        "  --disk-b <образ>       вставить дискету в привод B\n"
        "  --disk-rw              образы без защиты записи (изменения пишутся в файл)\n"
        "  --disk-sides 1|2       число сторон (по умолчанию — по размеру образа)\n"
        "  --disk-rom smk|326|<файл>  прошивка контроллера: smk512.rom, disk326.rom или своя\n"
        "\n"
        "Винчестер (IDE платы СМК-512, образы HDI эмулятора GID):\n"
        "  --hdd <образ.hdi>      подключить винчестер (master) и загрузиться с него;\n"
        "                         включает СМК-512 и её прошивку smk512.rom\n"
        "  --hdd-b <образ.hdi>    второй винчестер (slave)\n"
        "  --hdd-rw               изменения пишутся в файл образа (иначе — только в память)\n"
        "\n"
        "Тайминг и экран:\n"
        "  --cpu-phase 0|1        фаза такта ЦП относительно окна 037 (по умолчанию 0)\n"
        "  --no-arb037            «идеальная» память без ожиданий 037\n"
        "  --scanline             построчная отрисовка по лучу (на БК-0011М — по умолчанию)\n"
        "  --no-scanline          выключить построчную отрисовку\n"
        "  --mono                 чёрно-белый режим экрана (безголовый режим)\n"
        "  --aspect34, --no-aspect34  неквадратный пиксель БК в окне (GUI; по умолчанию вкл)\n"
        "\n"
        "Безголовый режим (без окна). Включается ключами --frames, --dbgshot, --memvis,\n"
        "профилировщиков, --type или --keys; --shot, --key и --keyframe работают вместе с ними:\n"
        "  --frames N             прогнать N кадров\n"
        "  --shot <файл.png>      снимок экрана 512x256 после прогона\n"
        "  --dbgshot <файл.png>   снимок с отладчиком Soft-ICE\n"
        "  --memvis <файл.png>    визуализация памяти\n"
        "  --hotpath, --callgraph, --flame, --flamechart, --hotchart <файл.png>\n"
        "                         профилировщики: горячий путь, граф вызовов, пламенный\n"
        "                         граф, хронология вызовов, горячие инструкции во времени\n"
        "  --key <код>            нажать клавишу с кодом БК (восьм. с ведущим 0, 0x — шестн.)\n"
        "  --keyframe N           ... на кадре N (по умолчанию 0)\n"
        "  --keys кадр:код,...    несколько нажатий по кадрам\n"
        "  --type <текст>         набрать текст с клавиатуры (с 60-го кадра)\n"
        "\n"
        "Прочее:\n"
        "  --server               MCP-сервер (JSON-RPC 2.0 по stdio) для отладки через Claude;\n"
        "                         понимает --roms, --machine, --bk10/--bk11m, --ay/--no-ay,\n"
        "                         --smk/--no-smk, --disk, --disk-rw, --hdd, --hdd-rw\n"
        "                         и позиционный образ диска или винчестера\n"
        "  --keytest              самопроверка перевода клавиш Qt в коды БК\n"
        "  -h, --help             эта справка\n"
        "\n"
        "Примеры:\n"
        "  bk0010-emulator game.bin\n"
        "  bk0010-emulator --machine bk11m INSULT.IMG\n"
        "  QT_QPA_PLATFORM=offscreen bk0010-emulator --frames 200 --shot out.png game.bin\n",
        stdout);
}

int main(int argc, char** argv) {
    // Имя организации и приложения нужно QSettings: без них настройки легли бы в
    // файл, названный по имени исполняемого файла.
    QCoreApplication::setOrganizationName("bk0010-emulator");
    QCoreApplication::setApplicationName("bk0010-emulator");

    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--help" || a == "-h") { printHelp(argv[0]); return 0; }
    }

    // MCP server mode: a headless JSON-RPC-over-stdio server for debugging BK
    // programs from an MCP client (e.g. Claude Code). Handled before the GUI so
    // no display/OpenGL is needed; offscreen QGuiApplication enables PNG output.
    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) == "--server") {
            qputenv("QT_QPA_PLATFORM", "offscreen");
            QGuiApplication app(argc, argv);
            QString romDir;
#ifdef BK_DEFAULT_ROM_DIR
            romDir = BK_DEFAULT_ROM_DIR;
#endif
            if (qEnvironmentVariableIsSet("BK_ROM_DIR")) romDir = qEnvironmentVariable("BK_ROM_DIR");
            bool smk = true;               // СМК-512 по умолчанию стоит
            std::string disk; bool diskRw = false;
            std::string hdd; bool hddRw = false;
            int machine = 1, ay = -1;      // по умолчанию — БК-0011М
            for (int j = 1; j < argc; ++j) {
                if (std::string(argv[j]) == "--machine" && j + 1 < argc)
                    machine = std::max(0, parseMachine(QString::fromLocal8Bit(argv[j + 1])));
                else if (std::string(argv[j]) == "--bk11m") machine = 1;
                else if (std::string(argv[j]) == "--bk10") machine = 0;
                else if (std::string(argv[j]) == "--ay") ay = 1;
                else if (std::string(argv[j]) == "--no-ay") ay = 0;
                if (std::string(argv[j]) == "--roms" && j + 1 < argc) romDir = argv[j + 1];
                else if (std::string(argv[j]) == "--smk") smk = true;
                else if (std::string(argv[j]) == "--no-smk") smk = false;
                else if (std::string(argv[j]) == "--disk" && j + 1 < argc) disk = argv[j + 1];
                else if (argv[j][0] != '-' && bk::Fdd::looksLikeImage(argv[j])) disk = argv[j];
                else if (std::string(argv[j]) == "--disk-rw") diskRw = true;
                else if (std::string(argv[j]) == "--hdd" && j + 1 < argc) hdd = argv[j + 1];
                else if (argv[j][0] != '-' && bk::looksLikeHdi(argv[j])) hdd = argv[j];
                else if (std::string(argv[j]) == "--hdd-rw") hddRw = true;
            }
            if (!hdd.empty()) smk = true;  // винчестер — узел платы СМК-512
            McpServer server(romDir.toStdString(), smk, disk, diskRw, machine, ay, hdd, hddRw);
            return server.run();
        }
    }

    // NVIDIA under a Wayland session: Qt6's default EGL/GLES path fails to create
    // GL contexts (both the QRhi backing store and our QOpenGLWidget) with
    // EGL_BAD_MATCH (3009). Route through XWayland/GLX, which is reliable, whenever
    // an X display is available and the user hasn't chosen a platform explicitly.
    // Linux-only: on macOS the native plugin is "cocoa" and on Windows "windows" —
    // forcing "xcb" there aborts startup ("Could not find the Qt platform plugin
    // xcb"), and DISPLAY may well be set on macOS via XQuartz.
#if defined(Q_OS_LINUX)
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM") && !qEnvironmentVariableIsEmpty("DISPLAY"))
        qputenv("QT_QPA_PLATFORM", "xcb");
#endif
    // Prefer desktop GL over GLES. Must be set before QApplication.
    qputenv("QT_OPENGL", "desktop");
    QCoreApplication::setAttribute(Qt::AA_UseDesktopOpenGL);

    QApplication app(argc, argv);

    // Request an OpenGL 3.3 core context for the screen widget.
    QSurfaceFormat fmt;
    fmt.setVersion(3, 3);
    fmt.setProfile(QSurfaceFormat::CoreProfile);
    fmt.setDepthBufferSize(0);
    QSurfaceFormat::setDefaultFormat(fmt);

    // ROM directory: --roms <dir>, else BK_ROM_DIR env, else compiled default.
    QString romDir;
#ifdef BK_DEFAULT_ROM_DIR
    romDir = BK_DEFAULT_ROM_DIR;
#endif
    if (qEnvironmentVariableIsSet("BK_ROM_DIR"))
        romDir = qEnvironmentVariable("BK_ROM_DIR");

    QString binToLoad, shot, dbgShot, memvisShot, hpShot, caShot, faShot, fcShot, hcShot, typeStr, keysList;
    int frames = 0, keyCode = -1, keyFrame = 0;
    bool color = true, headless = false;
    const QStringList args = app.arguments();
    for (int i = 1; i < args.size(); ++i) {
        if (args[i] == "--roms" && i + 1 < args.size()) romDir = args[++i];
        else if (args[i] == "--frames" && i + 1 < args.size()) { frames = args[++i].toInt(); headless = true; }
        else if (args[i] == "--shot" && i + 1 < args.size()) shot = args[++i];
        else if (args[i] == "--dbgshot" && i + 1 < args.size()) { dbgShot = args[++i]; headless = true; }
        else if (args[i] == "--memvis" && i + 1 < args.size()) { memvisShot = args[++i]; headless = true; }
        else if (args[i] == "--hotpath" && i + 1 < args.size()) { hpShot = args[++i]; headless = true; }
        else if (args[i] == "--callgraph" && i + 1 < args.size()) { caShot = args[++i]; headless = true; }
        else if (args[i] == "--flame" && i + 1 < args.size()) { faShot = args[++i]; headless = true; }
        else if (args[i] == "--flamechart" && i + 1 < args.size()) { fcShot = args[++i]; headless = true; }
        else if (args[i] == "--hotchart" && i + 1 < args.size()) { hcShot = args[++i]; headless = true; }
        else if (args[i] == "--mono") color = false;
        else if (args[i] == "--scanline") g_scanline = true;
        else if (args[i] == "--no-scanline") g_noScanline = true;
        else if (args[i] == "--machine" && i + 1 < args.size()) g_machine = parseMachine(args[++i]);
        else if (args[i] == "--bk10") g_machine = 0;
        else if (args[i] == "--bk11m") g_machine = 1;
        else if (args[i] == "--ay") g_ay = 1;
        else if (args[i] == "--no-ay") g_ay = 0;
        else if (args[i] == "--mstd") g_mstd = true;
        else if (args[i] == "--no-arb037") g_arb037 = false;
        else if (args[i] == "--cpu-phase" && i + 1 < args.size()) g_cpuPhase = args[++i].toInt() & 1;
        else if (args[i] == "--disk" && i + 1 < args.size()) g_disk = args[++i];
        else if (args[i] == "--disk-b" && i + 1 < args.size()) g_diskB = args[++i];
        else if (args[i] == "--disk-sides" && i + 1 < args.size()) g_diskSides = args[++i].toInt();
        else if (args[i] == "--disk-rom" && i + 1 < args.size()) {
            const QString v = args[++i];
            g_diskRom = (v == "smk") ? "smk512.rom" : (v == "326") ? "disk326.rom" : v;
        }
        else if (args[i] == "--disk-rw") g_diskRw = true;
        else if (args[i] == "--hdd" && i + 1 < args.size()) g_hdd = args[++i];
        else if (args[i] == "--hdd-b" && i + 1 < args.size()) g_hddB = args[++i];
        else if (args[i] == "--hdd-rw") g_hddRw = true;
        else if (args[i] == "--smk") g_smk = 1;
        else if (args[i] == "--no-smk") g_smk = 0;
        else if (args[i] == "--aspect34") g_aspect34 = 1;
        else if (args[i] == "--no-aspect34") g_aspect34 = 0;
        else if (args[i] == "--key" && i + 1 < args.size()) keyCode = args[++i].toInt(nullptr, 0);
        else if (args[i] == "--keyframe" && i + 1 < args.size()) keyFrame = args[++i].toInt();
        else if (args[i] == "--type" && i + 1 < args.size()) { typeStr = args[++i]; headless = true; }
        else if (args[i] == "--keys" && i + 1 < args.size()) { keysList = args[++i]; headless = true; }
        // Позиционный аргумент: .bkd и .img (в любом регистре) — это образ диска,
        // всё остальное — игра в формате .BIN.
        else if (!args[i].startsWith("--")) {
            if (bk::Fdd::looksLikeImage(args[i].toStdString())) g_disk = args[i];
            else if (bk::looksLikeHdi(args[i].toStdString())) g_hdd = args[i];
            else binToLoad = args[i];
        }
    }

    // Машина по умолчанию — БК-0011М. Но почти все .BIN — программы для БК-0010-01,
    // поэтому без явного --machine машину выбирает сама программа: пишет в регистр
    // 0177662 (палитра и экран 11М) — БК-0011М, иначе БК-0010-01 (сохранённую
    // настройку GUI это не меняет).
    if (g_machine < 0 && !binToLoad.isEmpty() && g_disk.isEmpty()) {
        g_machine = bk::Board::binTargetsBk11(binToLoad.toStdString()) ? 1 : 0;
        std::fprintf(stderr, g_machine
            ? ".BIN: программа для БК-0011М (пишет в регистр 0177662) — машина БК-0011М\n"
            : ".BIN: машина БК-0010-01 (для БК-0011М — ключ --machine bk11m)\n");
    }
    // Какая машина реально запустится: ключ, иначе сохранённая настройка GUI, иначе 11М.
    const int effMachine = g_machine >= 0 ? g_machine
                         : headless ? 1 : QSettings().value("machine", 1).toInt();

    // Дискета почти всегда подразумевает и блок расширения памяти: у голой
    // БК-0010-01 под ядро остаётся около 16 Кбайт ОЗУ, и большинство систем без
    // платы просто некуда грузить. Плата стоит по умолчанию, но в GUI могла быть
    // снята переключателем — тогда образ включает её сам на этот запуск, если ключ не
    // задан явно; --no-smk по-прежнему сильнее. Сохранённую настройку это не меняет.
    const bool smkSaved = headless || QSettings().value(effMachine == 1 ? "smk512_11m" : "smk512", true).toBool();
    if (!g_disk.isEmpty() && g_smk < 0 && effMachine == 0 && !smkSaved) {
        g_smk = 1;
        std::fprintf(stderr, "Дискета: включён блок расширения памяти СМК-512 "
                             "(отключить — ключом --no-smk)\n");
    }

    // Винчестер — узел платы СМК-512: без неё регистров НЖМД на шине нет.
    if (!g_hdd.isEmpty() || !g_hddB.isEmpty()) {
        if (g_smk == 0) {
            std::fprintf(stderr, "Винчестер подключается к плате СМК-512 — уберите --no-smk\n");
            return 2;
        }
        if (g_smk < 0 && !smkSaved)
            std::fprintf(stderr, "Винчестер: включён блок расширения памяти СМК-512\n");
        g_smk = 1;
    }

    if (args.contains("--keytest"))
        return runKeyTest();

    if (headless)
        return runHeadless(romDir, binToLoad, frames, color, shot, keyCode, keyFrame,
                           dbgShot, memvisShot, hpShot, caShot, faShot, fcShot, hcShot, typeStr, keysList);

    MainWindow w(romDir, g_smk, g_aspect34, g_machine, g_ay);
    w.setScanlineOverride(g_noScanline ? 0 : g_scanline ? 1 : -1);
    w.setMstd(g_mstd);
    w.setArbitration(g_arb037);
    w.setCpuPhase(g_cpuPhase);
    w.setDiskWritable(g_diskRw);
    w.show();
    if (!g_disk.isEmpty()) w.insertDiskAndBoot(g_disk, 0);
    else if (!binToLoad.isEmpty()) w.loadBinFromPath(binToLoad);
    if (!g_diskB.isEmpty()) w.insertDiskAndBoot(g_diskB, 1);
    w.setHddWritable(g_hddRw);
    if (!g_hddB.isEmpty()) w.attachHdd(g_hddB, 1, g_hdd.isEmpty());
    if (!g_hdd.isEmpty()) w.attachHdd(g_hdd, 0, true);

    return app.exec();
}

