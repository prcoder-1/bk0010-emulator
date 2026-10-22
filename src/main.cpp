#include <QApplication>
#include <QSurfaceFormat>
#include <QDir>
#include <QFileInfo>
#include <QImage>
#include <QPainter>
#include <QPixmap>
#include <QStandardPaths>
#include <map>
#include <cstdio>
#include <csignal>
#include <vector>
#include <cstdint>
#include "ui/MainWindow.h"
#include "ui/DebuggerOverlay.h"
#include "ui/MemVisWidget.h"
#include "ui/SmkRamWidget.h"
#include "ui/BkRamWidget.h"
#include "ui/HotPathWidget.h"
#include "ui/CallGraphWidget.h"
#include "ui/FlameWidget.h"
#include "ui/FlameChartWidget.h"
#include "ui/HotChartWidget.h"
#include "VideoCapture.h"
#include "VideoSink.h"
#include <QMouseEvent>
#include "ui/BkKeymap.h"
#include "mcp/McpServer.h"
#include "Board.h"
#include "Fdd.h"
#include "Disasm.h"
#include "BkKeys.h"
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

// Ключи командной строки. Один разбор на все режимы: GUI, безголовый и MCP-сервер.
// -1 — ключ не задан: в GUI берётся сохранённая настройка, иначе — значение по машине.
struct CliOptions {
    QString romDir;
    // Машина и оборудование.
    // Машина: выбор (--machine auto|bk10|bk11m; не задан — в GUI настройка, иначе auto) и
    // машина, с которой стартуем (resolveOptions: явный выбор — он сам, auto — по .BIN).
    bool machineGiven = false;
    bk::MachineChoice machineChoice = bk::MachineChoice::Auto;
    bk::Machine machine = bk::Machine::Bk0011m;
    int  ay = -1, ay2 = -1, covox = -1;   // звук на 0177714
    int  smk = -1;                        // блок расширения памяти СМК-512
    // Контроллер AZБК: -1 — не задан (GUI — настройка, иначе нет), карта SD (по умолчанию
    // <ПЗУ>/azbk), диски AZn поверх AZ.INI, запись в образы, доработка 037 (БК-0010).
    int  az = -1;
    QString azSd;
    std::map<int, QString> azUnits;
    bool azRw = false, az037 = false;
    int  aspect34 = -1;                   // GUI: неквадратный пиксель БК
    bool mstd = false;                    // блок МСТД (ПЗУ 330) в окне 0160000 БК-0011М
    // Арбитраж КР1801ВП1-037: доступ к ДОЗУ квантован окном процессора (раз в 4 такта),
    // и команда, промахнувшаяся мимо окна, теряет 4 такта. --no-arb037 возвращает
    // «идеальную» память (тайминг статического ОЗУ).
    bool arb037 = true;
    // Фаза такта ЦП относительно окна 037 — на железе выпадает при включении питания;
    // 0 — `MOV (R0),R1` = 28 тактов, 1 — 24.
    int  cpuPhase = 0;
    int  scanline = -1;                   // построчная отрисовка: -1 — по машине (11М — вкл)
    // Носители. Образы по умолчанию открываются ТОЛЬКО НА ЧТЕНИЕ — чужой рабочий образ
    // портить нельзя; --disk-rw / --hdd-rw разрешают запись обратно в файл.
    QString disk, diskB, hdd, hddB;
    bool diskRw = false, hddRw = false;
    int  diskSides = 0;                   // 0 — по размеру образа, 1 или 2 — принудительно
    QString diskRom;                      // --disk-rom smk|326|<файл>: прошивка контроллера
    bk::Board::HddController hddCtl = bk::Board::HddController::Auto;
    // Запись видео: --video-codec, --video-size, --video-audio перекрывают настройки GUI.
    QString video, videoCodec;
    int  videoAspect = -1;                // 1 — кадр 1024x768, 0 — 512x256
    int  videoAudio = -1;
    // Режимы.
    bool server = false, keytest = false, headless = false, color = true;
    // Безголовый режим.
    QString bin, shot, dbgShot, memvisShot, hpShot, caShot, faShot, fcShot, hcShot, typeStr, keysList;
    int  frames = 0, keyCode = -1, keyFrame = 0;

    bk::Board::Setup setup() const {      // конфигурация платы для безголового режима и MCP
        bk::Board::Setup st;
        st.machine = machine;
        st.ay = ay; st.ay2 = ay2; st.covox = covox;
        st.scanline = scanline;
        st.smk = smk != 0;                // по умолчанию плата стоит
        st.mstd = mstd; st.arb037 = arb037; st.cpuPhase = cpuPhase;
        st.diskRom = diskRom.toStdString();
        st.hddCtl = hddCtl;
        st.az = az > 0;
        st.azCfg = azConfig();
        return st;
    }
    bk::Azbk::Config azConfig() const {
        bk::Azbk::Config c;
        c.sdDir = (azSd.isEmpty() ? romDir + "/azbk" : azSd).toStdString();
        c.writeDir = MainWindow::azWriteDir(QString::fromStdString(c.sdDir), azRw).toStdString();
        c.rw = azRw; c.mod037 = az037;
        for (const auto& kv : azUnits) c.units[kv.first] = kv.second.toStdString();
        return c;
    }
    bk::Board::Media media() const {
        bk::Board::Media m;
        m.disk = disk.toStdString(); m.diskB = diskB.toStdString();
        m.hdd = hdd.toStdString(); m.hddB = hddB.toStdString();
        m.diskRw = diskRw; m.hddRw = hddRw; m.diskSides = diskSides;
        return m;
    }
};

// Попадёт ли образ на контроллер «Самара» (ему плата СМК-512 не нужна).
static bool hddOnSamara(bk::Board::HddController ctl, const std::string& path) {
    if (ctl != bk::Board::HddController::Auto) return ctl == bk::Board::HddController::Samara;
    return bk::hdiLayout(path) == bk::HddMap::Kind::Samara;
}

// 0 — разобрано; иначе код выхода (сообщение уже напечатано).
static int parseArgs(const QStringList& args, CliOptions& o) {
    auto fail = [](const char* msg) { std::fprintf(stderr, "%s\n", msg); return 2; };
    for (int i = 1; i < args.size(); ++i) {
        const QString& a = args[i];
        const bool hasVal = i + 1 < args.size();
        if (a == "--server") o.server = true;
        else if (a == "--keytest") o.keytest = true;
        else if (a == "--roms" && hasVal) o.romDir = args[++i];
        else if (a == "--frames" && hasVal) { o.frames = args[++i].toInt(); o.headless = true; }
        else if (a == "--shot" && hasVal) o.shot = args[++i];
        else if (a == "--dbgshot" && hasVal) { o.dbgShot = args[++i]; o.headless = true; }
        else if (a == "--memvis" && hasVal) { o.memvisShot = args[++i]; o.headless = true; }
        else if (a == "--hotpath" && hasVal) { o.hpShot = args[++i]; o.headless = true; }
        else if (a == "--callgraph" && hasVal) { o.caShot = args[++i]; o.headless = true; }
        else if (a == "--flame" && hasVal) { o.faShot = args[++i]; o.headless = true; }
        else if (a == "--flamechart" && hasVal) { o.fcShot = args[++i]; o.headless = true; }
        else if (a == "--hotchart" && hasVal) { o.hcShot = args[++i]; o.headless = true; }
        else if (a == "--type" && hasVal) { o.typeStr = args[++i]; o.headless = true; }
        else if (a == "--keys" && hasVal) { o.keysList = args[++i]; o.headless = true; }
        else if (a == "--key" && hasVal) o.keyCode = args[++i].toInt(nullptr, 0);
        else if (a == "--keyframe" && hasVal) o.keyFrame = args[++i].toInt();
        else if (a == "--mono") o.color = false;
        else if (a == "--scanline") o.scanline = 1;
        else if (a == "--no-scanline") o.scanline = 0;
        else if (a == "--machine" && hasVal) {
            if (!bk::Board::parseMachineChoice(args[++i].toLower().toStdString(), o.machineChoice))
                return fail("--machine: ожидается auto, bk10 или bk11m");
            o.machineGiven = true;
        }
        else if (a == "--bk10")  { o.machineChoice = bk::MachineChoice::Bk0010;  o.machineGiven = true; }
        else if (a == "--bk11m") { o.machineChoice = bk::MachineChoice::Bk0011m; o.machineGiven = true; }
        else if (a == "--ay") o.ay = 1;
        else if (a == "--no-ay") o.ay = 0;
        else if (a == "--2ay") o.ay2 = 1;
        else if (a == "--no-2ay") o.ay2 = 0;
        else if (a == "--covox") o.covox = 1;
        else if (a == "--no-covox") o.covox = 0;
        else if (a == "--az") o.az = 1;
        else if (a == "--no-az") o.az = 0;
        else if (a == "--az-sd" && hasVal) o.azSd = args[++i];
        else if (a == "--az-rw") o.azRw = true;
        else if (a == "--az-037") o.az037 = true;
        else if (a == "--az-unit" && hasVal) {
            // N=образ; без «N=» — диск 0.
            const QString v = args[++i];
            const int eq = v.indexOf('=');
            bool okN = true;
            const int n = eq > 0 ? v.left(eq).toInt(&okN) : 0;
            if (!okN || n < 0 || n > 31) return fail("--az-unit: ожидается N=образ, N от 0 до 31");
            o.azUnits[n] = eq > 0 ? v.mid(eq + 1) : v;
            if (o.az < 0) o.az = 1;
        }
        else if (a == "--smk") o.smk = 1;
        else if (a == "--no-smk") o.smk = 0;
        else if (a == "--mstd") o.mstd = true;
        else if (a == "--aspect34") o.aspect34 = 1;
        else if (a == "--no-aspect34") o.aspect34 = 0;
        else if (a == "--no-arb037") o.arb037 = false;
        else if (a == "--cpu-phase" && hasVal) o.cpuPhase = args[++i].toInt() & 1;
        else if (a == "--disk" && hasVal) o.disk = args[++i];
        else if (a == "--disk-b" && hasVal) o.diskB = args[++i];
        else if (a == "--disk-rw") o.diskRw = true;
        else if (a == "--disk-sides" && hasVal) {
            o.diskSides = args[++i].toInt();
            if (o.diskSides < 0 || o.diskSides > 2) return fail("--disk-sides: ожидается 1 или 2");
        }
        else if (a == "--disk-rom" && hasVal) {
            const QString v = args[++i];
            o.diskRom = (v == "smk") ? "smk512.rom" : (v == "326") ? "disk326.rom" : v;
        }
        else if (a == "--hdd" && hasVal) o.hdd = args[++i];
        else if (a == "--hdd-b" && hasVal) o.hddB = args[++i];
        else if (a == "--hdd-rw") o.hddRw = true;
        else if (a == "--hdd-ctl" && hasVal) {
            if (!bk::Board::parseHddController(args[++i].toLower().toStdString(), o.hddCtl))
                return fail("--hdd-ctl: ожидается auto, smk или samara");
        }
        else if (a == "--video" && hasVal) o.video = args[++i];
        else if (a == "--video-codec" && hasVal) {
            o.videoCodec = args[++i].toLower();
            if (o.videoCodec == "mp4" || o.videoCodec == "h.264") o.videoCodec = "h264";
            if (o.videoCodec == "avi") o.videoCodec = "zmbv";
            const std::string why = bk::videoCodecUnavailableReason(o.videoCodec.toStdString());
            if (!why.empty()) {
                std::fprintf(stderr, "--video-codec %s: %s\n", qPrintable(o.videoCodec), why.c_str());
                return 2;
            }
        }
        else if (a == "--video-audio") o.videoAudio = 1;
        else if (a == "--no-video-audio") o.videoAudio = 0;
        else if (a == "--video-size" && hasVal) {
            const QString v = args[++i].toLower();
            if (v == "1024x768" || v == "4:3") o.videoAspect = 1;
            else if (v == "512x256" || v == "raw") o.videoAspect = 0;
            else return fail("--video-size: ожидается 1024x768 или 512x256");
        }
        // Позиционный аргумент: .bkd и .img (в любом регистре) — образ дискеты, .hdi —
        // винчестер, всё остальное — программа в формате .BIN.
        else if (!a.startsWith("--")) {
            if (bk::Fdd::looksLikeImage(a.toStdString())) o.disk = a;
            else if (bk::looksLikeHdi(a.toStdString())) o.hdd = a;
            else o.bin = a;
        }
        else std::fprintf(stderr, "Неизвестный ключ %s (список — --help)\n", qPrintable(a));
    }
    return 0;
}

// Что из ключей следует для машины и платы: машина по .BIN, плата СМК для дискеты и
// винчестера, кодек по расширению файла видео. smkSaved — стоит ли плата по настройке
// GUI (в безголовом режиме и MCP она стоит всегда). 0 — порядок, иначе код выхода.
static int resolveOptions(CliOptions& o) {
    const bool gui = !o.headless && !o.server;
    // Выбор машины: ключ, иначе настройка GUI, иначе auto. Явный выбор — это и есть машина,
    // программу не проверяем. В auto машину выбирает программа (.BIN пишет в 0177662 или
    // зовёт БОС — БК-0011М, иначе БК-0010-01), если пуск не с дискеты; без неё — БК-0011М.
    if (!o.machineGiven && gui) o.machineChoice = MainWindow::savedMachineChoice();
    const bool fromBin = o.machineChoice == bk::MachineChoice::Auto && !o.server
                         && !o.bin.isEmpty() && o.disk.isEmpty();
    o.machine = bk::Board::machineFor(o.machineChoice, fromBin ? o.bin.toStdString() : std::string());
    if (fromBin)
        std::fprintf(stderr, o.machine == bk::Machine::Bk0011m
            ? ".BIN: программа для БК-0011М (палитра 0177662 или вызовы БОС) — машина БК-0011М\n"
            : ".BIN: машина БК-0010-01 (явно — ключ --machine bk10|bk11m)\n");
    const int effMachine = o.machine == bk::Machine::Bk0011m ? 1 : 0;
    // AZБК: GUI — по настройке (если ключом не задано), образ дискеты становится диском AZ0.
    // Плата только из настройки не мешает запуску .BIN и винчестера — на этот запуск её нет.
    const bool azExplicit = o.az > 0;
    if (o.az < 0 && gui) o.az = MainWindow::savedAzbk(effMachine == 1) ? 1 : -1;
    if (o.az > 0 && !azExplicit && (!o.bin.isEmpty() || !o.hdd.isEmpty() || !o.hddB.isEmpty())) {
        std::fprintf(stderr, "AZБК: на этот запуск снят (программа .BIN или винчестер)\n");
        o.az = 0;
    }
    if (o.az > 0) {
        if (!o.hdd.isEmpty() || !o.hddB.isEmpty()) {
            std::fprintf(stderr, "AZБК: винчестеры СМК и «Самары» с ним не работают — диски AZ (--az-unit)\n");
            return 2;
        }
        if (o.smk == 1) {
            std::fprintf(stderr, "AZБК сам эмулирует СМК-512 — уберите --smk\n");
            return 2;
        }
        if (!o.bin.isEmpty()) {
            std::fprintf(stderr, "AZБК: программа .BIN не загружается — AZ грузится со своих дисков (--az-unit)\n");
            return 2;
        }
        if (!o.disk.isEmpty()) { o.azUnits[0] = o.disk; o.disk.clear(); }
        if (!o.diskB.isEmpty()) { o.azUnits[1] = o.diskB; o.diskB.clear(); }
        o.smk = 0;
    }
    // Дискета почти всегда подразумевает и блок расширения памяти: у голой БК-0010-01
    // под ядро остаётся около 16 Кбайт ОЗУ, и большинство систем без платы просто
    // некуда грузить. Плата стоит по умолчанию, но в GUI могла быть снята
    // переключателем — тогда образ включает её сам на этот запуск, если ключ не задан
    // явно; --no-smk по-прежнему сильнее. Сохранённую настройку это не меняет.
    const bool smkSaved = !gui || QSettings().value(effMachine == 1 ? "smk512_11m" : "smk512", true).toBool();
    if (!o.disk.isEmpty() && o.smk < 0 && effMachine == 0 && !smkSaved) {
        o.smk = 1;
        std::fprintf(stderr, "Дискета: включён блок расширения памяти СМК-512 (отключить — ключом --no-smk)\n");
    }
    // Винчестер СМК — узел платы СМК-512: без неё регистров НЖМД на шине нет. У «Самары»
    // своя плата, а драйвер — в прошивке КНГМД.
    const QString hddFirst = o.hdd.isEmpty() ? o.hddB : o.hdd;
    if (!hddFirst.isEmpty() && hddOnSamara(o.hddCtl, hddFirst.toStdString())) {
        std::fprintf(stderr, "Винчестер: контроллер «Самара» (0177620–0177656, прошивка samara.rom)\n");
        // На БК-0010 загрузчику нужна память на месте Бейсика — как и дискете.
        if (effMachine == 0 && o.smk < 0 && !smkSaved) {
            o.smk = 1;
            std::fprintf(stderr, "Винчестер: включён блок расширения памяти СМК-512 (отключить — ключом --no-smk)\n");
        }
    } else if (!hddFirst.isEmpty()) {
        if (o.smk == 0) {
            std::fprintf(stderr, "Винчестер подключается к плате СМК-512 — уберите --no-smk\n");
            return 2;
        }
        if (o.smk < 0 && !smkSaved) std::fprintf(stderr, "Винчестер: включён блок расширения памяти СМК-512\n");
        o.smk = 1;
    }
    // Кодек по расширению файла --video, если не задан ключом: .mp4 — H.264 + AAC.
    if (!o.video.isEmpty() && o.videoCodec.isEmpty()) {
        if (const bk::VideoCodecInfo* c = bk::videoCodecForPath(o.video.toStdString())) {
            const std::string why = bk::videoCodecUnavailableReason(c->name);
            if (!why.empty()) {
                std::fprintf(stderr, "--video %s: %s\n", qPrintable(o.video), why.c_str());
                return 2;
            }
            o.videoCodec = QString::fromStdString(c->name);
        }
    }
    return 0;
}

// Headless verification: boot the monitor, optionally load a .BIN, run N frames
// and save the screen (rendered from the CPU-side pixel buffer, no GL needed).
static int runHeadless(const CliOptions& o) {
    const QString& bin = o.bin;
    const int frames = o.frames;
    const QString& shot = o.shot;
    const QString& dbgShot = o.dbgShot;
    const QString& memvisShot = o.memvisShot;
    const QString& hpShot = o.hpShot;
    const QString& caShot = o.caShot;
    const QString& faShot = o.faShot;
    const QString& fcShot = o.fcShot;
    const QString& hcShot = o.hcShot;
    // Parse "frame:code,frame:code,..." into precisely-timed key injections.
    std::vector<std::pair<int,int>> keys;
    for (const QString& part : o.keysList.split(',', Qt::SkipEmptyParts)) {
        const auto fc = part.split(':');
        if (fc.size() == 2) keys.push_back({fc[0].toInt(), fc[1].toInt(nullptr, 0)});
    }
    bk::Board board;
    if (!board.powerOn(o.setup(), o.romDir.toStdString())) {
        std::fprintf(stderr, "headless: failed to load ROMs from %s\n", qPrintable(o.romDir));
        return 2;
    }
    if (!memvisShot.isEmpty() || !hpShot.isEmpty() || !caShot.isEmpty() || !hcShot.isEmpty()
        || !dbgShot.isEmpty() || !faShot.isEmpty() || !fcShot.isEmpty())
        board.trace().setEnabled(true);
    if (!faShot.isEmpty() || !fcShot.isEmpty()) board.trace().setFlameEnabled(true);
    if (!fcShot.isEmpty()) board.trace().setSpansEnabled(true);
    // Let the monitor ROM initialise (vectors, stack, display driver) before
    // jumping into a game.
    board.ensureMonitorBooted();
    // Дискета — автозагрузчику ПЗУ контроллера, как «160000G» в мониторе; винчестер —
    // прошивке контроллера, перехватывающей пуск.
    std::string err;
    if (!board.bootMedia(o.media(), err)) {
        std::fprintf(stderr, "headless: %s\n", err.c_str());
        return 6;
    }
    if (!err.empty()) std::fprintf(stderr, "headless: %s\n", err.c_str());
    else if (!o.disk.isEmpty()) std::printf("headless: загрузка с диска %s\n", qPrintable(o.disk));
    else if (!o.hdd.isEmpty() || !o.hddB.isEmpty())
        std::printf("headless: загрузка с винчестера %s\n", qPrintable(o.hdd.isEmpty() ? o.hddB : o.hdd));
    if (!bin.isEmpty() && !board.loadBin(bin.toStdString(), true)) {
        std::fprintf(stderr, "headless: failed to load bin %s\n", qPrintable(bin));
        return 3;
    }
    board.screen().setColorMode(o.color);
    // --video: запись с первого кадра основного прогона (загрузка уже позади).
    bk::VideoCapture video;
    std::vector<int16_t> videoAudio;
    if (!o.video.isEmpty()) {
        bk::VideoCapture::Options vo;
        if (!o.videoCodec.isEmpty()) vo.codec = o.videoCodec.toStdString();
        vo.aspect43 = o.videoAspect != 0;
        if (o.videoAudio != 0) {
            vo.audioRate = board.sound().sampleRate();
            board.sound().setCapture(true);       // отвод звука — с начала первого кадра записи
        }
        std::string err;
        // С AZБК кадр — VGA платы 1024x768.
        const bool vga = board.azbk();
        if (!video.start(o.video.toStdString(), vo, vga ? bk::Azbk::VGA_W : bk::Screen::TEX_W,
                         vga ? bk::Azbk::VGA_H : bk::Screen::TEX_H,
                         static_cast<uint32_t>(board.cpuFreqHz()), static_cast<uint32_t>(board.ticksPerFrame()), &err)) {
            std::fprintf(stderr, "headless: видео %s не открыто: %s\n", qPrintable(o.video), err.c_str());
            return 8;
        }
    }
    // Optional: type a string, one character every few frames, through the same
    // keyboard queue the GUI uses (verifies the key -> register -> ISR -> echo path).
    // Текст — в коды КОИ-7 с переключением РУС/ЛАТ, как bk_type в MCP.
    bool cyr = false;
    const std::vector<uint16_t> typeCodes = bk::bkEncodeText(o.typeStr.toStdString(), cyr);
    size_t typeIdx = 0;
    for (int i = 0; i < frames; ++i) {
        if (o.keyCode >= 0 && i == o.keyFrame) board.pressKey(static_cast<uint16_t>(o.keyCode));
        for (const auto& kv : keys) if (kv.first == i) board.pressKey(static_cast<uint16_t>(kv.second));
        if (i >= 60 && typeIdx < typeCodes.size() && !board.keyReady())
            board.pressKey(typeCodes[typeIdx++]);
        board.runFrame();
        if (video.active()) {
            if (!board.scanlineRender()) board.screen().render(board.memory());
            board.sound().takeCaptured(videoAudio);
            video.addFrame(board.azbk() ? board.az().frame() : board.screen().pixels(), videoAudio.data(), videoAudio.size() / 2);
        }
    }
    board.sound().setCapture(false);
    if (video.active()) {
        const uint64_t n = video.frames();
        if (video.stop())
            std::printf("headless: видео %s — %llu кадров, %.2f с\n", qPrintable(o.video),
                        static_cast<unsigned long long>(n), double(n) * board.ticksPerFrame() / board.cpuFreqHz());
        else
            std::fprintf(stderr, "headless: видео не записано: %s\n", video.error().c_str());
    }
    if (!board.scanlineRender()) board.screen().render(board.memory());

    const bk::Board::Frame fr = board.displayFrame();
    QImage img(reinterpret_cast<const uchar*>(fr.px), fr.w, fr.h, QImage::Format_ARGB32);
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
        // Окно «Память БК по страницам» (на 11М — со страницы в окне 0).
        {
            BkRamWidget br(&board);
            br.resize(980, 620);
            QString brPath = memvisShot; brPath.replace(".png", "_bkram.png");
            br.grab().save(brPath);
            std::printf("headless: wrote память БК по страницам %s\n", qPrintable(brPath));
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
        std::vector<int16_t> s(2 * board.sound().available());   // стерео: L, R
        size_t n = s.empty() ? 0 : board.sound().read(s.data(), s.size() / 2);
        int16_t lo = 0, hi = 0;
        for (size_t i = 0; i < 2 * n; ++i) { if (s[i] < lo) lo = s[i]; if (s[i] > hi) hi = s[i]; }
        std::printf("headless: audio samples=%zu range=[%d..%d]%s\n", n, lo, hi,
                    (hi - lo > 1000) ? " (speaker active)" : " (silent)");
    }

    // Report whether the screen has any non-black pixels (sanity signal).
    int nonblack = 0;
    const uint32_t* px = board.screen().pixels();
    for (int i = 0; i < bk::Screen::TEX_W * bk::Screen::TEX_H; ++i)
        if ((px[i] & 0x00FFFFFF) != 0) ++nonblack;
    std::printf("headless: %s, PC=%06o non-black pixels=%d\n", bk::Board::machineName(board.machine()),
                board.cpu().pc(), nonblack);
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
        "программа в формате .BIN.\n"
        "\n"
        "Машина:\n"
        "  --machine auto|bk10|bk11m  эмулируемая машина. auto (по умолчанию; в GUI — выбор\n"
        "                         из меню «Машина») — машину выбирает программа: .BIN, который\n"
        "                         пишет в палитру 0177662 или зовёт БОС, — БК-0011М, иначе\n"
        "                         БК-0010-01; без программы — БК-0011М. bk10 и bk11m — только\n"
        "                         эта машина, программа не проверяется\n"
        "  --bk10, --bk11m        то же, что --machine bk10 / bk11m\n"
        "  --ay, --no-ay          звуковой сопроцессор AY-3-8910 на порту 0177714\n"
        "                         (по умолчанию: на БК-0011М вкл, на БК-0010 выкл)\n"
        "  --2ay, --no-2ay        второй AY — плата «2AY», выбор чипа по TurboSound и\n"
        "                         «Грифону» (по умолчанию: на БК-0011М вкл, на БК-0010 выкл)\n"
        "  --covox, --no-covox    Covox — ЦАП на порту 0177714 (то же по умолчанию)\n"
        "  --smk, --no-smk        блок расширения памяти СМК-512 (512 Кбайт; по умолчанию вкл)\n"
        "  --mstd                 БК-0011М: вставить тестовый блок МСТД в окно 0160000\n"
        "\n"
        "Контроллер AZБК (MAXIOL; VGA 1024x768, маппер 32 Мбайт, блиттер, звук, диски с SD):\n"
        "  --az, --no-az          установить контроллер (GUI — по умолчанию по настройке).\n"
        "                         С ним ПЗУ БК, СМК-512, КНГМД и винчестеры сняты; машина\n"
        "                         стартует с его ПЗУ (AZBOOT), экран — VGA платы\n"
        "  --az-sd <каталог>      «карта SD» с AZ.INI (по умолчанию <каталог ПЗУ>/azbk —\n"
        "                         прошивка 00019 без дисков)\n"
        "  --az-unit N=<образ>    диск AZn поверх [DISKS] из AZ.INI (без «N=» — AZ0);\n"
        "                         включает --az. Образ дискеты позиционным аргументом — AZ0\n"
        "  --az-rw                образы на запись и запись БК (EEPROM, файлы) — прямо на\n"
        "                         карту; иначе изменения дисков в памяти, остальное — в\n"
        "                         каталоге данных эмулятора\n"
        "  --az-037               БК-0010: доработка 037 (эмуляция БК-0011М, быстрая память)\n"
        "  --roms <каталог>       каталог ПЗУ (иначе BK_ROM_DIR или каталог при сборке)\n"
        "\n"
        "Дисковод:\n"
        "  --disk <образ>         вставить дискету в привод A и загрузиться с неё\n"
        "  --disk-b <образ>       вставить дискету в привод B\n"
        "  --disk-rw              образы без защиты записи (изменения пишутся в файл)\n"
        "  --disk-sides 1|2       число сторон (по умолчанию — по размеру образа)\n"
        "  --disk-rom smk|326|<файл>  прошивка контроллера: smk512.rom, disk326.rom или своя\n"
        "\n"
        "Винчестер (IDE платы СМК-512 или контроллера «Самара», образы HDI эмулятора GID):\n"
        "  --hdd <образ.hdi>      подключить винчестер (master) и загрузиться с него;\n"
        "                         СМК: включает плату и её прошивку smk512.rom,\n"
        "                         «Самара»: прошивка КНГМД samara.rom, плата не нужна\n"
        "  --hdd-b <образ.hdi>    второй винчестер (slave)\n"
        "  --hdd-rw               изменения пишутся в файл образа (иначе — только в память)\n"
        "  --hdd-ctl auto|smk|samara  контроллер винчестера; auto (по умолчанию) — по\n"
        "                         разметке образа: «Самара» — её контроллер, иначе СМК\n"
        "\n"
        "Видео (запись экрана со звуком в AVI или MP4, частота кадров БК — 48,828125 Гц):\n"
        "  --video <файл.avi|.mp4>  записывать экран с первого кадра (GUI — до остановки\n"
        "                         записи или выхода, безголовый режим — все --frames кадров)\n"
        "  --video-codec zmbv|h264  zmbv — AVI без потерь (ZMBV + PCM); h264 — MP4 (H.264 +\n"
        "                         AAC) для Telegram и телефонов, нужна сборка с FFmpeg. Без\n"
        "                         ключа — по расширению (.mp4 — h264), иначе GUI — из\n"
        "                         настроек, безголовый режим — zmbv\n"
        "  --video-size 1024x768|512x256  кадр: точка БК блоком 2x3 (4:3, по умолчанию)\n"
        "                         или как буфер экрана с пометкой «показывать 4:3» (только AVI)\n"
        "  --no-video-audio       без звука (по умолчанию звук пищалки, AY и Covox пишется\n"
        "                         в тот же файл, стерео 44,1 кГц); --video-audio — со звуком\n"
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
        "  --shot <файл.png>      снимок экрана 512x256 после прогона (с AZБК — VGA 1024x768)\n"
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
        "                         понимает ключи машины, дисковода, винчестера и тайминга\n"
        "                         и позиционный образ диска или винчестера\n"
        "  --keytest              самопроверка перевода клавиш Qt в коды БК\n"
        "  -h, --help             эта справка\n"
        "\n"
        "Примеры:\n"
        "  bk0010-emulator game.bin\n"
        "  bk0010-emulator --machine bk11m INSULT.IMG\n"
        "  bk0010-emulator --az --az-unit 0=ANDOS330.IMG\n"
        "  QT_QPA_PLATFORM=offscreen bk0010-emulator --frames 200 --shot out.png game.bin\n",
        stdout);
}

int main(int argc, char** argv) {
    // Имя организации и приложения нужно QSettings: без них настройки легли бы в
    // файл, названный по имени исполняемого файла.
    QCoreApplication::setOrganizationName("bk0010-emulator");
    QCoreApplication::setApplicationName("bk0010-emulator");

    QStringList args;
    for (int i = 0; i < argc; ++i) args << QString::fromLocal8Bit(argv[i]);
    if (args.contains("--help") || args.contains("-h")) { printHelp(argv[0]); return 0; }

    CliOptions o;
    // ROM directory: --roms <dir>, else BK_ROM_DIR env, else compiled default.
#ifdef BK_DEFAULT_ROM_DIR
    o.romDir = BK_DEFAULT_ROM_DIR;
#endif
    if (qEnvironmentVariableIsSet("BK_ROM_DIR")) o.romDir = qEnvironmentVariable("BK_ROM_DIR");
    if (const int rc = parseArgs(args, o)) return rc;

    // MCP server mode: a headless JSON-RPC-over-stdio server for debugging BK
    // programs from an MCP client (e.g. Claude Code). Handled before the GUI so
    // no display/OpenGL is needed; offscreen QGuiApplication enables PNG output.
    if (o.server) {
        qputenv("QT_QPA_PLATFORM", "offscreen");
        QGuiApplication app(argc, argv);
        if (const int rc = resolveOptions(o)) return rc;
        McpServer server(o.romDir.toStdString(), o.setup(), o.media(), o.machineChoice);
        return server.run();
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

    if (o.keytest) return runKeyTest();
    if (const int rc = resolveOptions(o)) return rc;
    if (o.headless) return runHeadless(o);

    MainWindow::AzOptions azo;
    azo.on = o.az; azo.sd = o.azSd; azo.units = o.azUnits; azo.rw = o.azRw; azo.mod037 = o.az037 ? 1 : -1;
    MainWindow w(o.romDir, o.machineChoice, o.machine, o.smk, o.aspect34, o.ay, o.ay2, o.covox, azo);
    w.setScanlineOverride(o.scanline);
    w.setMstd(o.mstd);
    w.setArbitration(o.arb037);
    w.setCpuPhase(o.cpuPhase);
    w.setDiskWritable(o.diskRw);
    if (!o.diskRom.isEmpty()) w.setDiskRom(o.diskRom);
    w.show();
    if (!o.disk.isEmpty()) w.insertDiskAndBoot(o.disk, 0);
    else if (!o.bin.isEmpty()) w.loadBinFromPath(o.bin);
    if (!o.diskB.isEmpty()) w.insertDiskAndBoot(o.diskB, 1);
    w.setHddWritable(o.hddRw);
    w.setHddController(o.hddCtl);
    if (!o.video.isEmpty()) w.startVideo(o.video, o.videoCodec, o.videoAspect, o.videoAudio);
    if (!o.hddB.isEmpty()) w.attachHdd(o.hddB, 1, o.hdd.isEmpty());
    if (!o.hdd.isEmpty()) w.attachHdd(o.hdd, 0, true);

    // Ctrl+C / kill в терминале: закрыть окно как обычно — иначе файл видео остался бы
    // без индекса, а изменённые дискеты не записались бы.
    std::signal(SIGINT, [](int) { MainWindow::requestQuit(); });
    std::signal(SIGTERM, [](int) { MainWindow::requestQuit(); });
    return app.exec();
}
