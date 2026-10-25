#include "MainWindow.h"
#include <algorithm>
#include "QtFormat.h"
#include "GlScreen.h"
#include "DebuggerOverlay.h"
#include "MemVisWidget.h"
#include "SmkRamWidget.h"
#include "AzRamWidget.h"
#include "AzPaths.h"
#include "BkRamWidget.h"
#include "HotPathWidget.h"
#include "CallGraphWidget.h"
#include "FlameWidget.h"
#include "FlameChartWidget.h"
#include "HotChartWidget.h"
#include "AudioOut.h"
#include "Disasm.h"
#include "VideoCapture.h"
#include "VideoSink.h"
#include "TapeDialog.h"
#include "BkKeyboardWidget.h"
#include <QDateTime>
#include <QStandardPaths>
#include <vector>
#include <QMenuBar>
#include <QMenu>
#include <QActionGroup>
#include <QAction>
#include <QFileDialog>
#include <QMessageBox>
#include <QTimer>
#include <QStatusBar>
#include <QLabel>
#include <QFileInfo>
#include <QKeySequence>
#include <QKeyEvent>
#include <QInputDialog>
#include <QLineEdit>
#include <QStringList>
#include <QCloseEvent>
#include <QShowEvent>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QSettings>
#include <QSignalBlocker>
#include <QProxyStyle>
#include <QVBoxLayout>
#include <QMouseEvent>
#include <QCursor>
#include <cstdio>

bool MainWindow::savedAzbk(bool bk11) {
    return QSettings().value(bk11 ? "azbk_11m" : "azbk", false).toBool();
}

QString MainWindow::azWriteDir(const QString& sdDir, bool rw) { return azWriteDirFor(sdDir, rw); }

bk::MachineChoice MainWindow::savedMachineChoice() {
    bk::MachineChoice c = bk::MachineChoice::Auto;
    bk::Board::parseMachineChoice(QSettings().value("machineMode", "auto").toString().toStdString(), c);
    return c;
}

MainWindow::MainWindow(const QString& romDir, bk::MachineChoice choice, bk::Machine machine,
                       int smkOverride, int aspectOverride,
                       int ayOverride, int ay2Override, int covoxOverride, const AzOptions& az,
                       QWidget* parent)
    : QMainWindow(parent), machineChoice_(choice), azCli_(az), romDir_(romDir) {
    board_ = std::make_unique<bk::Board>();
    // Выбор машины запоминается между запусками (меню), --machine перекрывает его.
    board_->setMachine(machine);
    applySoundSettings(ayOverride, ay2Override, covoxOverride);
    applyScanline();
    // Блок расширения памяти СМК-512: настройка запоминается между запусками,
    // ключ --smk/--no-smk перекрывает её на один запуск.
    const bool smkOn = smkOverride >= 0 ? smkOverride > 0
                                        : QSettings().value(smkKey(), smkDefault()).toBool();
    board_->setSmk512(smkOn);
    if (board_->smk512()) {
        // Стоит сказать вслух: с платой карта верхней половины адресов не штатная
        // (в режиме SYS ОЗУ контроллера перекрывает ПЗУ Бейсика), и это объясняет
        // «странности», если пользователь про плату забыл.
        std::fprintf(stderr, "%s: блок расширения памяти СМК-512 подключён (512 Кбайт)\n",
                     bk::Board::machineName(board_->machine()));
    }
    if (!board_->loadRoms(romDir.toStdString())) {
        QMessageBox::warning(this, "БК",
            QString("Не удалось загрузить ПЗУ из:\n%1\nПоложите в эту папку %2.")
                .arg(romDir).arg(board_->bk11()
                    ? "bk11m_324_bos.rom (и bk11m_325_ext, bk11m_327..329_basic*.rom)"
                    : board_->focalRoms() ? "monit10.rom (и bk10_018_focal.rom, bk10_019_mstd.rom)"
                    : "monit10.rom (и basic10.rom)"));
    }
    // Контроллер AZБК: настройка своя у каждой машины, ключ --az/--no-az её перекрывает.
    if (azCli_.on >= 0 ? azCli_.on > 0 : QSettings().value(azKey(), false).toBool()) applyAz(true);
    board_->reset();

    screen_ = new GlScreen(this);
    // Пиксель БК не квадратный: настройка запоминается между запусками,
    // ключ --aspect34/--no-aspect34 перекрывает её на один запуск.
    const bool aspect34 = aspectOverride >= 0 ? aspectOverride > 0
                                              : QSettings().value("aspect34", true).toBool();
    screen_->setPixelAspect34(aspect34);
    setCentralWidget(screen_);
    // Keyboard goes to the main window; the GL widget must not steal focus.
    screen_->setFocusPolicy(Qt::NoFocus);
    setFocusPolicy(Qt::StrongFocus);

    overlay_ = new DebuggerOverlay(board_.get(), screen_);
    overlay_->setGeometry(screen_->rect());
    overlay_->hide();

    // --- Menus ---
    // Alt — это АР2 БК. Одиночное нажатие Alt не должно уводить фокус в меню.
    struct NoAltNavStyle : QProxyStyle {
        int styleHint(StyleHint h, const QStyleOption* o, const QWidget* w,
                      QStyleHintReturn* r) const override {
            return h == SH_MenuBar_AltKeyNavigation ? 0 : QProxyStyle::styleHint(h, o, w, r);
        }
    };
    auto* menuStyle = new NoAltNavStyle;
    menuStyle->setParent(menuBar());
    menuBar()->setStyle(menuStyle);
    QMenu* file = menuBar()->addMenu("&Файл");
    file->addAction("&Загрузить .BIN…", this, &MainWindow::openBin, QKeySequence::Open);
    file->addAction("Вставить образ &диска… (привод A)", this,
                    [this] { openDisk(0); }, QKeySequence("Ctrl+B"));
    file->addAction("Вставить образ диска в привод &B…", this, [this] { openDisk(1); });
    file->addSeparator();
    file->addAction("Подключить &винчестер (HDI)…", this, [this] { openHdd(0); });
    file->addAction("Подключить второй винчестер (slave)…", this, [this] { openHdd(1); });
    file->addAction("Отключить винчестеры", this, &MainWindow::detachHdds);
    // Контроллер винчестера: по разметке образа или явно. Действует со следующего
    // подключения образа.
    QMenu* hddCtl = file->addMenu("Контроллер винчестера");
    auto* hddGroup = new QActionGroup(this);
    const std::pair<const char*, bk::Board::HddController> ctls[] = {
        {"По разметке образа (авто)", bk::Board::HddController::Auto},
        {"СМК-512 (0177740–0177757)", bk::Board::HddController::Smk},
        {"«Самара» (0177620–0177656)", bk::Board::HddController::Samara}};
    for (const auto& c : ctls) {
        QAction* a = hddCtl->addAction(c.first);
        a->setCheckable(true);
        a->setChecked(c.second == board_->hddController());
        hddGroup->addAction(a);
        const bk::Board::HddController v = c.second;
        connect(a, &QAction::triggered, this, [this, v] { board_->setHddController(v); });
        hddCtlActions_.push_back({a, v});
    }
    file->addSeparator();
    file->addAction("В&ыход", this, &QWidget::close, QKeySequence::Quit);

    // --- Магнитофон: кассета .wav, сигнал — в разряд 5 регистра 0177716 ---
    QMenu* tapeMenu = menuBar()->addMenu("&Магнитофон");
    tapeMenu->addAction("Вставить &кассету (.wav)…", this, &MainWindow::openTape);
    tapeMenu->addAction("&Оглавление кассеты…", this, &MainWindow::showTapeCatalog);
    tapeMenu->addSeparator();
    tapePlayAction_ = tapeMenu->addAction("&Воспроизведение");
    tapePlayAction_->setCheckable(true);
    connect(tapePlayAction_, &QAction::triggered, this, [this](bool on) {
        if (!board_->tape().loaded()) { tapePlayAction_->setChecked(false); openTape(); return; }
        if (on) board_->tape().play(board_->totalTicks()); else board_->tape().stop(board_->totalTicks());
        updateTapeLabel();
    });
    tapeMenu->addAction("&Перемотать в начало", this, [this] {
        board_->tape().seek(0, board_->totalTicks());
        updateTapeLabel();
    });
    tapeMenu->addAction("&Извлечь кассету", this, [this] {
        board_->ejectTape();
        updateTapeLabel();
        status_->setText("Кассета извлечена: EMT 36 снова обслуживается файлами хоста");
    });
    tapeMenu->addSeparator();
    auto tapeOption = [&](const QString& text, const char* key, bool def, auto apply) {
        QAction* a = tapeMenu->addAction(text);
        a->setCheckable(true);
        const bool on = QSettings().value(QString("tape/") + key, def).toBool();
        a->setChecked(on);
        apply(on);
        connect(a, &QAction::toggled, this, [this, key, apply](bool v) {
            QSettings().setValue(QString("tape/") + key, v);
            apply(v);
            updateTapeLabel();
        });
    };
    tapeOption("&Автопуск: лента трогается, когда драйвер ПЗУ начинает чтение", "autostart", true,
               [this](bool v) { board_->tape().setAutoStart(v); });
    tapeOption("&ДУ: лента идёт только при включённом двигателе (разряд 7)", "remote", false,
               [this](bool v) { board_->tape().setRemote(v, board_->totalTicks()); });
    tapeOption("Ин&вертировать сигнал", "invert", false,
               [this](bool v) { board_->tape().setInverted(v); });
    tapeOption("&Ускорять эмуляцию, пока идёт лента", "warp", true, [this](bool v) { tapeWarp_ = v; });
    lastTape_ = QSettings().value("tape/last").toString();

    QMenu* emu = menuBar()->addMenu("&Эмуляция");
    emu->addAction("&Сброс", this, &MainWindow::resetMachine, QKeySequence("Ctrl+R"));
    emu->addAction("Режим &цвет/ч-б", this, &MainWindow::toggleColorMode, QKeySequence("F11"));
    // Пиксель БК шире, чем выше: кадр ложится на телевизионное поле 4:3. Включённый
    // режим рисует пиксель цветного режима блоком 4x3 точек, выключённый — квадратом.
    aspectAction_ = emu->addAction("Пиксели &3:4 (как на телевизоре)");
    aspectAction_->setCheckable(true);
    aspectAction_->setChecked(aspect34);
    aspectAction_->setShortcut(QKeySequence("Ctrl+F11"));
    connect(aspectAction_, &QAction::toggled, this, &MainWindow::setPixelAspect34);
    // С AZБК на экране — его VGA; переключатель показывает экран самой БК (её ТВ-выход).
    azVgaAction_ = emu->addAction("Экран &VGA платы AZБК (иначе — экран БК)");
    azVgaAction_->setCheckable(true);
    azVgaAction_->setChecked(QSettings().value("azbk/vga", true).toBool());
    azVgaAction_->setShortcut(QKeySequence("Ctrl+E"));
    board_->setAzVga(azVgaAction_->isChecked());
    connect(azVgaAction_, &QAction::toggled, this, &MainWindow::setAzVga);
    emu->addAction("&Пауза", this, [this]{ setSuspended(!suspended_); }, QKeySequence(Qt::Key_Pause));
    emu->addAction("&Отладчик (Soft-ICE)", this, [this]{ setPaused(!paused_); }, QKeySequence("F12"));
    emu->addAction("Экранная &клавиатура БК…", this, &MainWindow::openKeyboard);

    // --- Машина: БК-0010-01 или БК-0011М и её оборудование: СМК-512, AY, джойстик ---
    QMenu* mach = menuBar()->addMenu("&Машина");
    auto* machGroup = new QActionGroup(this);
    machGroup->setExclusive(true);
    // Авто — машину выбирает программа (.BIN), явный выбор программу не проверяет.
    machineAutoAction_ = mach->addAction("&Автоматически — по программе");
    machine10fAction_ = mach->addAction("БК-00&10 (Фокал, тесты МСТД, плёночная клавиатура)");
    machine10Action_ = mach->addAction("БК-0010-0&1 (3 МГц, 32 Кбайт ОЗУ, Бейсик)");
    machine11Action_ = mach->addAction("БК-00&11М (4 МГц, 128 Кбайт ОЗУ, 16 палитр)");
    for (auto* a : {machineAutoAction_, machine10fAction_, machine10Action_, machine11Action_}) {
        a->setCheckable(true);
        machGroup->addAction(a);
    }
    connect(machineAutoAction_, &QAction::triggered, this, [this] { setMachineChoice(bk::MachineChoice::Auto); });
    connect(machine10Action_, &QAction::triggered, this, [this] { setMachineChoice(bk::MachineChoice::Bk0010); });
    connect(machine11Action_, &QAction::triggered, this, [this] { setMachineChoice(bk::MachineChoice::Bk0011m); });
    connect(machine10fAction_, &QAction::triggered, this, [this] { setMachineChoice(bk::MachineChoice::Bk0010Focal); });
    mach->addSeparator();
    // Блок расширения памяти СМК-512 («АльтПро») в разъёме МПИ. На живой машине
    // плату ставят только при выключенном питании, поэтому переключатель
    // перезапускает машину — раскладка верхней половины адресов меняется целиком.
    smkAction_ = mach->addAction("Блок расширения памяти &СМК-512 (512 Кбайт)");
    smkAction_->setCheckable(true);
    smkAction_->setChecked(board_->smk512());
    connect(smkAction_, &QAction::toggled, this, [this](bool on) { setSmk512(on); });
    // Контроллер AZБК (MAXIOL): маппер 32 Мбайт, VGA, блиттер, звук, диски с карты SD.
    // Он сам эмулирует СМК-512, поэтому пока он стоит, галка СМК недоступна.
    QMenu* azMenu = mach->addMenu("Контроллер A&ZБК");
    azAction_ = azMenu->addAction("&Установлен");
    azAction_->setCheckable(true);
    connect(azAction_, &QAction::toggled, this, [this](bool on) { setAzbk(on); });
    azMenu->addAction("&Каталог карты SD…", this, &MainWindow::chooseAzSd);
    azMenu->addAction("&Диск AZ0…", this, &MainWindow::chooseAzUnit0);
    azMenu->addAction("Убрать диск AZ0 (как в AZ.INI)", this, [this] {
        QSettings().remove("azbk/unit0");
        azCli_.units.erase(0);
        if (board_->azbk()) {
            const auto& u = azConfig().units;          // AZ.INI решит, если и в нём нет
            board_->setAzUnit(0, u.count(0) ? u.at(0) : std::string());
            resetMachine();
        }
        status_->setText("Диск AZ0 — по AZ.INI");
    });
    // Образы AZ по умолчанию только на чтение (записи БК — в памяти поверх файла); галка
    // пишет изменения диска AZ0 в его файл, включая уже сделанные.
    azUnit0RwAction_ = azMenu->addAction("Диск AZ0 на &запись (изменения — в файл образа)");
    azUnit0RwAction_->setCheckable(true);
    connect(azUnit0RwAction_, &QAction::toggled, this, [this](bool on) {
        QSettings().setValue("azbk/unit0rw", on);
        azCli_.unitsRw = false;
        board_->az().setUnitRw(0, on);
        status_->setText(on ? "Диск AZ0: запись в файл образа" : "Диск AZ0: только чтение, изменения — в памяти");
    });
    azMenu->addAction("&Сеть (кабель сетевой карты)…", this, &MainWindow::chooseAzNet);
    az037Action_ = azMenu->addAction("Доработка &037 (БК-0010: эмуляция БК-0011М)");
    az037Action_->setCheckable(true);
    connect(az037Action_, &QAction::toggled, this, [this](bool on) {
        QSettings().setValue("azbk/mod037", on);
        azCli_.mod037 = -1;
        if (board_->azbk()) { applyAz(true); resetMachine(); }
    });
    ayAction_ = mach->addAction("Звуковой сопроцессор &AY-3-8910 / YM2149 (порт 0177714)");
    ayAction_->setCheckable(true);
    connect(ayAction_, &QAction::toggled, this, [this](bool on) { setAy(on); });
    ay2Action_ = mach->addAction("Второй AY — плата «&2AY» (TurboSound / «Грифон»)");
    ay2Action_->setCheckable(true);
    connect(ay2Action_, &QAction::toggled, this, [this](bool on) { setAy2(on); });
    covoxAction_ = mach->addAction("&Covox — ЦАП на порту 0177714");
    covoxAction_->setCheckable(true);
    connect(covoxAction_, &QAction::toggled, this, [this](bool on) { setCovox(on); });
    mach->addSeparator();
    // Джойстик: геймпад через SDL2, порт 0177714. Раскладка выбирается — у БК их две
    // (несовместимые). Если геймпад подключён — покажем его имя в подменю.
    QMenu* joy = mach->addMenu("&Джойстик (геймпад)");
    auto* joyGroup = new QActionGroup(this);
    joyGroup->setExclusive(true);
    auto* aStd = joy->addAction("Раскладка «Стандарт» (Джойвокс/gid, по умолч.)");
    auto* aBH  = joy->addAction("Раскладка «Break House»");
    auto* aSW  = joy->addAction("Раскладка «SWCorp»");
    auto* aKl  = joy->addAction("Раскладка «Клад-2»");
    for (auto* a : {aStd, aBH, aSW, aKl}) { a->setCheckable(true); joyGroup->addAction(a); }
    aStd->setChecked(true);
    connect(aStd, &QAction::triggered, this, [this]{ gamepad_.setStandard(Gamepad::Standard::Standard); });
    connect(aBH,  &QAction::triggered, this, [this]{ gamepad_.setStandard(Gamepad::Standard::BreakHouse); });
    connect(aSW,  &QAction::triggered, this, [this]{ gamepad_.setStandard(Gamepad::Standard::SWCorp); });
    connect(aKl,  &QAction::triggered, this, [this]{ gamepad_.setStandard(Gamepad::Standard::Klad2); });
    joy->addSeparator();
    QAction* joyStatus = joy->addAction(gamepad_.connected()
        ? QString::fromStdString("Подключён: " + gamepad_.name())
        : QString::fromUtf8("Геймпад не найден"));
    joyStatus->setEnabled(false);
    // Мышь «Марсианка» (УВК-01): тот же порт 0177714, вход — мышь ПК над экраном БК.
    QMenu* mouseMenu = mach->addMenu("&Мышь «Марсианка»");
    mouseAction_ = mouseMenu->addAction("&Подключена (порт 0177714, мышь ПК над экраном)");
    mouseAction_->setCheckable(true);
    connect(mouseAction_, &QAction::toggled, this, [this](bool on) { setMarsMouse(on); });
    mouseMenu->addSeparator();
    // Шаг шара в точках экрана ПК — свой по каждой оси. Шар у «Марсианки» один на обе оси,
    // но программы двигают курсор неодинаково: CSI-DOS — байт экрана (4 цветные точки) по
    // горизонтали и две строки по вертикали, в окне это 16 и 6 точек ПК. Значения по
    // умолчанию (8 и 3) выравнивают такой курсор.
    auto scaleMenu = [&](const QString& title, const char* key, int def, int& var) {
        QMenu* m = mouseMenu->addMenu(title);
        auto* group = new QActionGroup(this);
        var = std::clamp(QSettings().value(key, def).toInt(), 1, 32);
        for (int sc : {1, 2, 3, 4, 6, 8, 12, 16}) {
            QAction* a = m->addAction(QString("%1 точ%2 экрана ПК на шаг шара").arg(sc)
                                      .arg(sc == 1 ? "ка" : sc < 5 ? "ки" : "ек"));
            a->setCheckable(true);
            a->setChecked(sc == var);
            group->addAction(a);
            connect(a, &QAction::triggered, this, [&var, key, sc] {
                var = sc;
                QSettings().setValue(key, sc);
            });
        }
    };
    scaleMenu("Чувствительность по &горизонтали", "marsMouseScaleX", 8, marsScaleX_);
    scaleMenu("Чувствительность по &вертикали", "marsMouseScaleY", 3, marsScaleY_);
    mouseMenu->addSeparator();
    QAction* mouseHelp = mouseMenu->addAction("Средняя кнопка — захват мыши (и отпустить)");
    mouseHelp->setEnabled(false);
    screen_->installEventFilter(this);
    setMarsMouse(QSettings().value("marsMouse", false).toBool(), false);
    syncMachineMenu();

    // Запись экрана в видео. Кодек и размер кадра — настройки, действуют со следующей
    // записи; ключи --video-codec / --video-size перекрывают их на один запуск.
    QMenu* vid = menuBar()->addMenu("&Видео");
    videoAction_ = vid->addAction("&Записывать видео…");
    videoAction_->setCheckable(true);
    videoAction_->setShortcut(QKeySequence("Ctrl+F5"));
    connect(videoAction_, &QAction::triggered, this, [this](bool on) {
        if (on) startVideo(QString(), QString(), -1); else stopVideo();
    });
    QMenu* codecMenu = vid->addMenu("&Кодек");
    auto* codecGroup = new QActionGroup(this);
    const QString curCodec = QSettings().value("video/codec", QString::fromStdString(bk::videoCodecList().front().name)).toString();
    for (const auto& c : bk::videoCodecList()) {
        const QString name = QString::fromStdString(c.name);
        QAction* a = codecMenu->addAction(QString::fromStdString(c.title));
        a->setCheckable(true);
        a->setChecked(name == curCodec);
        codecGroup->addAction(a);
        connect(a, &QAction::triggered, this, [name] { QSettings().setValue("video/codec", name); });
    }
    QMenu* sizeMenu = vid->addMenu("Кадр &видео");
    auto* sizeGroup = new QActionGroup(this);
    const bool cur43 = QSettings().value("video/aspect43", true).toBool();
    for (int v = 1; v >= 0; --v) {
        QAction* a = sizeMenu->addAction(v ? "1024×768 — точка БК блоком 2×3 (4:3, как в окне)"
                                           : "512×256 — как буфер экрана, с пометкой «показывать 4:3» (только AVI)");
        a->setCheckable(true);
        a->setChecked(cur43 == (v == 1));
        sizeGroup->addAction(a);
        connect(a, &QAction::triggered, this, [v] { QSettings().setValue("video/aspect43", v == 1); });
    }
    QAction* vAudio = vid->addAction("Записывать &звук");
    vAudio->setCheckable(true);
    vAudio->setChecked(QSettings().value("video/audio", true).toBool());
    connect(vAudio, &QAction::toggled, this, [](bool on) { QSettings().setValue("video/audio", on); });

    QMenu* dbg = menuBar()->addMenu("&Отладка");
    dbg->addAction("&Визуализация памяти…", this, &MainWindow::openMemVis, QKeySequence("Ctrl+I"));
    dbg->addAction("&ДОЗУ СМК-512 по страницам…", this, &MainWindow::openSmkRam, QKeySequence("Ctrl+D"));
    dbg->addAction("Память &БК по страницам…", this, &MainWindow::openBkRam, QKeySequence("Ctrl+P"));
    dbg->addAction("Память &AZБК по страницам…", this, &MainWindow::openAzRam, QKeySequence("Ctrl+J"));
    dbg->addAction("Горячий &путь…", this, &MainWindow::openHotPath, QKeySequence("Ctrl+G"));
    dbg->addAction("Граф &вызовов…", this, &MainWindow::openCallGraph, QKeySequence("Ctrl+K"));
    dbg->addAction("&Пламенный граф…", this, &MainWindow::openFlame, QKeySequence("Ctrl+F"));
    dbg->addAction("&Хронология вызовов…", this, &MainWindow::openFlameChart, QKeySequence("Ctrl+T"));
    dbg->addAction("Горячие инструкции во &времени…", this, &MainWindow::openHotChart, QKeySequence("Ctrl+H"));
    dbg->addSeparator();
    dbg->addAction("&Сохранить состояние…", this, &MainWindow::saveState, QKeySequence("Ctrl+S"));
    dbg->addAction("В&осстановить состояние…", this, &MainWindow::loadState, QKeySequence("Ctrl+L"));
    dbg->addAction("Загрузить си&мволы (.map)…", this, [this] {
        QString path = QFileDialog::getOpenFileName(this, "Загрузить символы (linker .map)",
            lastBin_.isEmpty() ? QString() : lastBin_,
            "Карты/символы (*.map *.sym *.lst);;Все файлы (*)");
        if (path.isEmpty()) return;
        int n = board_->loadSymbols(path.toStdString());
        status_->setText(n < 0 ? QString("Не удалось открыть файл символов")
                               : QString("Загружено символов: %1").arg(n));
        overlay_->update();
    });
    dbg->addAction("Сохранить &аннотации (.bkdb)", this, [this] {
        QString p = annotationsPath();
        if (p.isEmpty()) p = QFileDialog::getSaveFileName(this, "Сохранить аннотации",
            QString(), "Аннотации БК (*.bkdb)");
        if (p.isEmpty()) return;
        saveAnnotations(p);
        status_->setText("Аннотации сохранены: " + p);
    });
    dbg->addAction("Загрузить а&ннотации (.bkdb)", this, [this] {
        QString p = QFileDialog::getOpenFileName(this, "Загрузить аннотации",
            annotationsPath(), "Аннотации БК (*.bkdb);;Все файлы (*)");
        if (p.isEmpty()) return;
        loadAnnotations(p);
        status_->setText("Аннотации загружены: " + p);
    });
    dbg->addAction("&Автоанализ процедур (JSR + трасса)", this, [this] {
        int n = overlay_->analyzeProcedures();
        status_->setText(QString("Автоанализ: добавлено процедур sub_… — %1").arg(n));
        if (paused_) overlay_->update();
    });

    status_ = new QLabel("Готов", this);
    statusBar()->addWidget(status_);
    videoLabel_ = new QLabel(this);
    videoLabel_->setStyleSheet("color: #d22; font-weight: bold;");
    videoLabel_->hide();
    statusBar()->addPermanentWidget(videoLabel_);
    tapeLabel_ = new QLabel(this);
    tapeLabel_->hide();
    statusBar()->addPermanentWidget(tapeLabel_);

    // --- Таймер эмуляции. Кадр разбит на 4 слайса, чтобы звук подавался непрерывно,
    // а не одним всплеском на кадр (это давало слышимое «вобблирование»); экран и
    // профилировщик обновляются раз в кадр. Сам темп задаётся НЕ частотой таймера:
    // кадр БК = 61440 тактов = 20,48 мс, слайс = 5,12 мс, целым числом миллисекунд
    // это не выражается. Поэтому таймер только «будит» нас почаще, а сколько слайсов
    // исполнить — считается по реальному времени в onTick(). ---
    timer_ = new QTimer(this);
    timer_->setTimerType(Qt::PreciseTimer);
    connect(timer_, &QTimer::timeout, this, &MainWindow::onTick);
    clock_.start();
    timer_->start(4);

    // --- Audio (only if built with Qt6 Multimedia) ---
#ifdef HAVE_QT_MULTIMEDIA
    audio_ = new AudioOut(&board_->sound(), this);
    audio_->start();
    emu->addAction("Звук вкл/&выкл", this, [this]{
        static bool muted = false; muted = !muted;
        if (audio_) audio_->setVolume(muted ? 0.f : 0.6f);
        status_->setText(muted ? "Звук выключен" : "Звук включён");
    }, QKeySequence("Ctrl+M"));
#else
    board_->sound().setEnabled(false); // no sink; don't accumulate samples
#endif

    updateTitle();
    resize(1024, 768);
}

MainWindow::~MainWindow() { stopVideo(); }

// Запись видео. path пусто — спросить имя (по умолчанию bk_ГГГГММДД_ЧЧММСС.avi в папке
// прошлой записи); codec пусто и aspect < 0 — из настроек.
bool MainWindow::startVideo(const QString& path, const QString& codec, int aspect, int audio) {
    stopVideo();
    QString file = path;
    // Кодек: явный (--video-codec), иначе по расширению файла, иначе из настроек.
    std::string cname = codec.toStdString();
    if (cname.empty() && !file.isEmpty())
        if (const bk::VideoCodecInfo* c = bk::videoCodecForPath(file.toStdString())) cname = c->name;
    if (cname.empty()) cname = QSettings().value("video/codec", "zmbv").toString().toStdString();
    if (!bk::findVideoCodec(cname)) cname = bk::videoCodecList().front().name;   // настройка от другой сборки
    if (file.isEmpty()) {
        QSettings st;
        QString dir = st.value("video/dir").toString();
        if (dir.isEmpty()) dir = QStandardPaths::writableLocation(QStandardPaths::MoviesLocation);
        const QString ext = QString::fromStdString(bk::findVideoCodec(cname)->ext);
        const QString def = dir + "/bk_" + QDateTime::currentDateTime().toString("yyyyMMdd_HHmmss") + ext;
        QStringList filters;
        QString sel;
        for (const auto& c : bk::videoCodecList()) {
            filters << QString("%1 (*%2)").arg(QString::fromStdString(c.title), QString::fromStdString(c.ext));
            if (c.name == cname) sel = filters.back();
        }
        file = QFileDialog::getSaveFileName(this, "Записывать видео в файл", def, filters.join(";;"), &sel);
        if (file.isEmpty()) { videoAction_->setChecked(false); return false; }
        // Расширение известного кодека решает, в чём писать; без него — дописать своё.
        if (const bk::VideoCodecInfo* c = bk::videoCodecForPath(file.toStdString())) cname = c->name;
        else file += ext;
        st.setValue("video/dir", QFileInfo(file).absolutePath());
    }
    if (const std::string why = bk::videoCodecUnavailableReason(cname); !why.empty()) {
        status_->setText(QString("Видео не записывается: %1").arg(QString::fromStdString(why)));
        videoAction_->setChecked(false);
        return false;
    }
    bk::VideoCapture::Options o;
    o.codec = cname;
    o.aspect43 = aspect < 0 ? QSettings().value("video/aspect43", true).toBool() : aspect != 0;
    // Звук — тот же поток, что идёт в колонки (пищалка, AY, Covox), но с отдельного
    // отвода: выключенный звук (Ctrl+M) запись не глушит. Отвод включается в начале
    // следующего кадра (runOneSlice), чтобы звук и картинка начались вместе.
    if (audio < 0 ? QSettings().value("video/audio", true).toBool() : audio != 0)
        o.audioRate = board_->sound().sampleRate();
    frameRec_ = false;
    if (!video_) video_ = std::make_unique<bk::VideoCapture>();
    std::string err;
    // С AZБК кадр — его VGA 1024x768 (экран БК в это время растягивается до того же размера).
    const bool vga = board_->azbk();
    if (!video_->start(file.toStdString(), o, vga ? bk::Azbk::VGA_W : bk::Screen::TEX_W, vga ? bk::Azbk::VGA_H : bk::Screen::TEX_H,
                       static_cast<uint32_t>(board_->cpuFreqHz()), static_cast<uint32_t>(board_->ticksPerFrame()), &err)) {
        status_->setText(QString("Видео не записывается: %1 — %2").arg(file, QString::fromStdString(err)));
        videoAction_->setChecked(false);
        return false;
    }
    videoAction_->setChecked(true);
    status_->setText(QString("Запись видео: %1 (%2, %3×%4, %5 Гц%6)").arg(file, QString::fromStdString(bk::findVideoCodec(cname)->format))
                         .arg(video_->outWidth()).arg(video_->outHeight())
                         .arg(double(board_->cpuFreqHz()) / board_->ticksPerFrame(), 0, 'f', 6)
                         .arg(o.audioRate ? QString(", звук %1 Гц").arg(o.audioRate) : QString(", без звука")));
    updateVideoLabel();
    return true;
}

void MainWindow::stopVideo() {
    if (!video_ || !video_->active()) return;
    const QString file = QString::fromStdString(video_->currentFile());
    const uint64_t n = video_->frames();
    const double sec = video_->seconds();
    const bool ok = video_->stop();
    board_->sound().setCapture(false);
    frameRec_ = false;
    if (videoAction_) videoAction_->setChecked(false);
    if (videoLabel_) videoLabel_->hide();
    if (status_)
        status_->setText(ok ? QString("Видео записано: %1 — %2 кадров, %3 с").arg(file).arg(n).arg(sec, 0, 'f', 1)
                            : QString("Ошибка записи видео: %1").arg(QString::fromStdString(video_->error())));
}

void MainWindow::updateVideoLabel() {
    if (!video_ || !video_->active()) return;
    const int s = static_cast<int>(video_->seconds());
    videoLabel_->setText(QString("● ЗАПИСЬ %1:%2").arg(s / 60, 2, 10, QChar('0')).arg(s % 60, 2, 10, QChar('0')));
    videoLabel_->show();
}

void MainWindow::onTick() {
    if (quitRequested_) { quitRequested_ = 0; close(); return; }
    static constexpr int kSlices = 4;   // слайсов на кадр
    // Длительность слайса в наносекундах: кадр 61440 тактов при 3 МГц (81920 при
    // 4 МГц у БК-0011М) = 20,48 мс.
    const qint64 kSliceNs = qint64(board_->ticksPerFrame()) * 1000000000LL
                          / (qint64(board_->cpuFreqHz()) * kSlices);
    // Сколько слайсов должно было пройти к этому моменту реального времени. Догоняем,
    // но не больше kCatchUp за раз — иначе после паузы (или тормоза системы) эмулятор
    // ушёл бы в долгую «отработку долга» и подвис бы интерфейс.
    static constexpr qint64 kCatchUp = kSlices * 2;
    qint64 due = clock_.nsecsElapsed() / kSliceNs;
    if (due - slicesDone_ > kCatchUp) slicesDone_ = due - kCatchUp;  // долг списываем
    int steps = static_cast<int>(due - slicesDone_);
    if (steps > 0) {
        slicesDone_ = due;
        while (steps-- > 0) runOneSlice(kSlices);
    }
    // Пока идёт лента, загрузка не ждёт реального времени: слайсы подряд, до 12 мс за тик
    // (окну остаётся время на события), затем отсчёт темпа — заново от «сейчас».
    auto tapeMoving = [this] {
        return tapeWarp_ && !paused_ && !suspended_ && board_->tape().moving(board_->totalTicks());
    };
    if (tapeMoving()) {
        QElapsedTimer t;
        t.start();
        while (t.elapsed() < 12 && tapeMoving()) runOneSlice(kSlices);
        slicesDone_ = clock_.nsecsElapsed() / kSliceNs;
    }
}

void MainWindow::runOneSlice(int kSlices) {
    if (!paused_ && !suspended_) {
        // Опросить геймпад раз в кадр и обновить байт джойстика (порт 0177714) до
        // прогона кадра, чтобы игра увидела актуальное состояние в своём ISR.
        if (phase_ == 0) board_->setJoystick(gamepad_.poll());
        // Подать один код из очереди раз в кадр. НЕ ждём освобождения регистра:
        // на реальной БК новое нажатие перезаписывает непрочитанный код (см.
        // Board::pressKey) — иначе игра, читающая 0177662 только при физически
        // нажатой клавише (PITON) или только по событию нажатия (LAND), получала
        // бы застрявший код ПРЕДЫДУЩЕЙ клавиши.
        if (phase_ == 0 && !keyFeed_.empty()) {
            board_->pressKey(keyFeed_.front());
            keyFeed_.pop_front();
        }
        // Запись видео: кадр попадает в файл, только если начался при идущей записи, —
        // тогда и звук для него набран с его первого такта.
        if (phase_ == 0 && video_ && video_->active()) {
            if (!frameRec_ && video_->hasAudio()) board_->sound().setCapture(true);
            frameRec_ = true;
        }
        board_->runFrameSlice(phase_, kSlices);
        if (phase_ == kSlices - 1) frameDone_ = frameRec_;   // кадр БК досчитан — в видео
        if (board_->breakHit()) {          // stopped at a breakpoint
            board_->clearBreakHit();
            setPaused(true);
        }
    }
    // The heavy per-frame work (screen render + profiler refresh) stays at 50 Hz:
    // run it once every kSlices ticks. While paused/suspended nothing is emulated,
    // but this still ticks so the overlay stays responsive.
    if (++phase_ < kSlices) return;
    phase_ = 0;
    renderScreen();
    if (frameDone_ && video_ && video_->active()) {
        if (video_->hasAudio()) board_->sound().takeCaptured(videoAudio_);
        // Кадр — того размера, с каким запись начата: AZБК ставят и снимают на ходу.
        const bk::Board::Frame fr = board_->displayFrame();
        const uint32_t* px = fr.px;
        if (video_->srcWidth() == bk::Azbk::VGA_W && fr.w != bk::Azbk::VGA_W) {
            vgaFrame_.resize(size_t(bk::Azbk::VGA_W) * bk::Azbk::VGA_H);
            bk::VideoCapture::toVga(fr.px, vgaFrame_.data());
            px = vgaFrame_.data();
        } else if (video_->srcWidth() == bk::Screen::TEX_W && fr.w != bk::Screen::TEX_W) {
            px = board_->screen().pixels();
        }
        video_->addFrame(px, videoAudio_.data(), video_->hasAudio() ? videoAudio_.size() / 2 : 0);
        if (!video_->error().empty()) stopVideo();
        else if (video_->frames() % 25 == 0) updateVideoLabel();
    }
    frameDone_ = false;
    if (paused_) overlay_->update();
    if (memvis_ && memvis_->isVisible()) memvis_->refresh();
    if (smkram_ && smkram_->isVisible()) smkram_->refresh();
    if (bkram_ && bkram_->isVisible()) bkram_->refresh();
    if (azram_ && azram_->isVisible()) azram_->refresh();
    for (ProfilerView* p : profilers_)
        if (p->isVisible()) p->refresh();
}

void MainWindow::renderScreen() {
    // Кадр VGA построчно — только пока его видно или пишут; иначе он рисуется целиком здесь.
    board_->setAzLive(board_->azVga() || (video_ && video_->active()));
    board_->screen().setColorMode(colorMode_);
    if (!board_->scanlineRender()) board_->screen().render(board_->memory());
    const bk::Board::Frame fr = board_->displayFrame();
    screen_->setFrame(fr.px, fr.w, fr.h);
    updateTapeLabel();
}

void MainWindow::updateTapeLabel() {
    const bk::Tape& tp = board_->tape();
    if (tapePlayAction_ && tapePlayAction_->isChecked() != tp.playing()) {
        const QSignalBlocker b(tapePlayAction_);
        tapePlayAction_->setChecked(tp.playing());
    }
    if (!tapeLabel_) return;
    if (!tp.loaded()) { tapeLabel_->hide(); tapeLabelText_.clear(); return; }
    const uint64_t now = board_->totalTicks();
    auto mmss = [](double t) {
        const int m = static_cast<int>(t) / 60;
        return QString("%1:%2").arg(m, 2, 10, QChar('0')).arg(static_cast<int>(t) % 60, 2, 10, QChar('0'));
    };
    const int f = tp.fileAt(tp.position(now));
    const QString text = QString("%1 лента %2/%3%4")
        .arg(tp.moving(now) ? "▶" : tp.playing() ? "⏸" : "■")
        .arg(mmss(tp.seconds(now))).arg(mmss(tp.image()->seconds()))
        .arg(f >= 0 ? " · " + QString::fromStdString(tp.image()->files[f].name) : QString());
    if (text != tapeLabelText_) {
        tapeLabelText_ = text;
        tapeLabel_->setText(text);
        tapeLabel_->setToolTip(QString::fromStdString(tp.image()->path));
    }
    tapeLabel_->show();
}

bool MainWindow::insertTape(const QString& path, bool play, int remote) {
    std::string err;
    if (!board_->insertTape(path.toStdString(), err)) {
        QMessageBox::warning(this, "Магнитофон", QString("Кассета не вставлена: %1").arg(QString::fromStdString(err)));
        return false;
    }
    lastTape_ = path;
    QSettings().setValue("tape/last", path);
    if (remote >= 0) board_->tape().setRemote(remote > 0, board_->totalTicks());
    if (play) board_->tape().play(board_->totalTicks());
    const bk::TapeImage& img = *board_->tape().image();
    status_->setText(QString("Кассета %1: файлов %2. Файлы читает драйвер ПЗУ (EMT 36 не перехватывается); "
                             "лента тронется сама, когда БК начнёт чтение")
                         .arg(QFileInfo(path).fileName()).arg(img.files.size()));
    updateTapeLabel();
    if (tapeDialog_ && tapeDialog_->isVisible()) tapeDialog_->refresh();
    return true;
}

void MainWindow::openTape() {
    const QString path = QFileDialog::getOpenFileName(this, "Вставить кассету", lastTape_.isEmpty() ? lastBin_ : lastTape_,
                                                      "Кассеты БК (*.wav *.WAV *.Wav);;Все файлы (*)");
    if (!path.isEmpty()) insertTape(path);
}

void MainWindow::setMarsMouse(bool on, bool save) {
    board_->mouse().setEnabled(on);
    if (save) QSettings().setValue("marsMouse", on);
    if (mouseAction_ && mouseAction_->isChecked() != on) {
        const QSignalBlocker b(mouseAction_);
        mouseAction_->setChecked(on);
    }
    screen_->setMouseTracking(on);
    marsHaveLast_ = false;
    marsAccX_ = marsAccY_ = 0;
    if (!on) setMarsGrab(false);
    if (status_)
        status_->setText(on ? QString("Мышь «Марсианка» подключена: движение и кнопки мыши над экраном БК, "
                                      "средняя кнопка — захват")
                            : QString("Мышь «Марсианка» отключена"));
}

// Захват: курсор спрятан и возвращается в центр экрана, движение считается от центра —
// мышь не упирается в край окна. Отпускает средняя кнопка, отладчик и потеря фокуса.
void MainWindow::setMarsGrab(bool on) {
    if (on == marsGrab_) return;
    marsGrab_ = on;
    marsHaveLast_ = false;
    if (on) {
        screen_->setCursor(Qt::BlankCursor);
        screen_->grabMouse();
        QCursor::setPos(screen_->mapToGlobal(screen_->rect().center()));
        status_->setText("Мышь захвачена — отпустить: средняя кнопка");
    } else {
        screen_->releaseMouse();
        screen_->unsetCursor();
        board_->mouse().setButtons(false, false);
    }
}

void MainWindow::changeEvent(QEvent* e) {
    if (e->type() == QEvent::ActivationChange && !isActiveWindow()) setMarsGrab(false);
    QMainWindow::changeEvent(e);
}

bool MainWindow::eventFilter(QObject* obj, QEvent* e) {
    if (obj != screen_ || !board_->mouse().enabled() || paused_ || suspended_)
        return QMainWindow::eventFilter(obj, e);
    switch (e->type()) {
    case QEvent::MouseMove: {
        const QPoint p = static_cast<QMouseEvent*>(e)->position().toPoint();
        QPoint d;
        if (marsGrab_) {
            const QPoint c = screen_->rect().center();
            d = p - c;
            if (d.isNull()) return true;
            QCursor::setPos(screen_->mapToGlobal(c));
        } else {
            if (marsHaveLast_) d = p - marsLast_;
            marsLast_ = p;
            marsHaveLast_ = true;
        }
        // Дробные остатки копятся: медленное движение тоже доходит до шара.
        marsAccX_ += double(d.x()) / marsScaleX_;
        marsAccY_ += double(d.y()) / marsScaleY_;
        const int sx = int(marsAccX_), sy = int(marsAccY_);
        marsAccX_ -= sx;
        marsAccY_ -= sy;
        if (sx || sy) board_->mouse().move(sx, sy);
        return true;
    }
    case QEvent::MouseButtonPress:
    case QEvent::MouseButtonRelease: {
        auto* me = static_cast<QMouseEvent*>(e);
        if (me->button() == Qt::MiddleButton) {
            if (e->type() == QEvent::MouseButtonPress) setMarsGrab(!marsGrab_);
            return true;
        }
        const Qt::MouseButtons b = me->buttons();
        board_->mouse().setButtons(b & Qt::LeftButton, b & Qt::RightButton);
        return true;
    }
    case QEvent::Leave:
        marsHaveLast_ = false;
        break;
    case QEvent::FocusOut:
        setMarsGrab(false);
        break;
    default:
        break;
    }
    return QMainWindow::eventFilter(obj, e);
}

void MainWindow::syncKeyboardKind() {
    if (!vk_) return;
    vk_->setKind(board_->membraneKeyboard() ? BkKeyboardWidget::Kind::Membrane : BkKeyboardWidget::Kind::Buttons);
    vkWindow_->setWindowTitle(QString("Клавиатура %1 (%2)")
        .arg(QString::fromUtf8(bk::Board::machineName(board_->machine())))
        .arg(board_->membraneKeyboard() ? "плёночная" : "кнопочная"));
}

void MainWindow::openKeyboard() {
    if (!vk_) {
        vkWindow_ = new QWidget(this, Qt::Tool);
        auto* lay = new QVBoxLayout(vkWindow_);
        lay->setContentsMargins(0, 0, 0, 0);
        vk_ = new BkKeyboardWidget(vkWindow_);
        lay->addWidget(vk_);
        vk_->isRussian = [this] { return keymap_.cyrillic(); };
        connect(vk_, &BkKeyboardWidget::keyDown, this, [this](uint16_t c) { virtualKey(c, true); });
        connect(vk_, &BkKeyboardWidget::keyUp, this, [this] { virtualKey(0, false); });
        connect(vk_, &BkKeyboardWidget::registerSwitched, this, [this](bool rus) { keymap_.setCyrillic(rus); });
        connect(vk_, &BkKeyboardWidget::stopPressed, this, [this] {
            if (paused_ || suspended_) return;
            board_->pressStop();
            status_->setText(QString::fromUtf8("Клавиша СТОП (прерывание по вектору 4)"));
        });
        vkWindow_->resize(860, 337);
        syncKeyboardKind();
    }
    vkWindow_->show();
    vkWindow_->raise();
}

// Как нажатие клавиши матрицы с клавиатуры хоста: бит «нажата» держится, пока нажата
// кнопка мыши; второе нажатие при уже нажатой клавише кода не даёт (ВП1-014).
void MainWindow::virtualKey(uint16_t code, bool down) {
    static constexpr int kVirtualKey = -1;     // не совпадает ни с одним Qt::Key
    if (paused_ || suspended_) return;
    if (down) {
        const bool busy = !heldKeys_.empty();
        heldKeys_.insert(kVirtualKey);
        board_->setKeyHeld(true);
        if (busy) return;
        if (keyFeed_.empty()) board_->pressKey(code);
        else keyFeed_.push_back(code);
    } else if (heldKeys_.erase(kVirtualKey) && heldKeys_.empty()) {
        board_->setKeyHeld(false);
    }
}

void MainWindow::showTapeCatalog() {
    if (!tapeDialog_) {
        tapeDialog_ = new TapeDialog(board_.get(), this);
        connect(tapeDialog_, &TapeDialog::loadRequested, this, [this](int i) {
            if (const bk::TapeImage* img = board_->tape().image())
                loadBinFromPath(QString::fromStdString(img->path) + QString("#%1").arg(i + 1));
        });
    }
    tapeDialog_->show();
    tapeDialog_->raise();
}

QString MainWindow::chooseTapeProgram(const QString& wav) {
    std::string err;
    const auto img = bk::loadTapeImage(wav.toStdString(), err);
    if (!img) {
        QMessageBox::warning(this, "БК", QString("Кассета не читается: %1").arg(QString::fromStdString(err)));
        return {};
    }
    // Загрузчик Maxim с прочитанным турбо-потоком за ним — не отдельная программа.
    QStringList items;
    std::vector<int> idx;
    for (size_t i = 0; i < img->files.size(); ++i) {
        const bk::TapeFile& f = img->files[i];
        const bool loaderOfTurbo = f.maximLoader && i + 1 < img->files.size()
                                   && img->files[i + 1].kind == bk::TapeFile::Kind::MaximTurbo;
        if (f.data.empty() || loaderOfTurbo) continue;
        items << QString("%1. %2 — адрес %3, длина %4%5").arg(i + 1).arg(QString::fromStdString(f.name))
                     .arg(f.addr, 6, 8, QChar('0')).arg(f.len, 6, 8, QChar('0'))
                     .arg(f.crcOk() ? QString() : QString(", КС не сошлась"));
        idx.push_back(static_cast<int>(i));
    }
    if (idx.empty()) {
        QMessageBox::warning(this, "БК", QString("На кассете %1 нет записей БК").arg(QFileInfo(wav).fileName()));
        return {};
    }
    if (idx.size() == 1) return wav + QString("#%1").arg(idx[0] + 1);
    int cur = 0;
    for (size_t k = 0; k < idx.size(); ++k) if (idx[k] == img->mainProgram()) cur = static_cast<int>(k);
    bool ok = false;
    const QString pick = QInputDialog::getItem(this, "Кассета", "Какой файл загрузить программой:", items, cur, false, &ok);
    if (!ok) return {};
    return wav + QString("#%1").arg(idx[items.indexOf(pick)] + 1);
}

void MainWindow::setPaused(bool paused) {
    paused_ = paused;
    if (paused_) setMarsGrab(false);
    phase_ = 0;                     // start the next run at a fresh frame boundary
    if (paused_) {
        overlay_->snapshotRegs();   // baseline for the changed-register highlight
        overlay_->setCursor(board_->cpu().pc());   // selection starts at PC
        overlay_->setGeometry(screen_->rect());
        overlay_->followPc();
        overlay_->show();
        overlay_->raise();
        status_->setText("ОТЛАДЧИК: F7 шаг, F8 через, F9 точка, Esc продолжить, F12 выход");
    } else {
        overlay_->hide();
        status_->setText("Выполнение");
    }
    // Entering/leaving the debugger drops any held keys so a game doesn't see a
    // key stuck down across the pause.
    releaseAllKeys();
    renderScreen();
}

// Simple pause via the Pause key: freezes the 50 Hz emulation loop without the
// Soft-ICE overlay. The last frame stays on screen; the status bar shows PAUSED.
void MainWindow::setSuspended(bool suspended) {
    suspended_ = suspended;
    phase_ = 0;                     // resume at a fresh frame boundary
    // Drop any held/buffered keys so nothing is stuck down or replayed on resume.
    releaseAllKeys();
    keyFeed_.clear();
    if (!paused_)  // don't fight the debugger for the status line
        status_->setText(suspended_ ? "ПАУЗА — нажмите Pause для продолжения"
                                     : "Выполнение");
}

void MainWindow::stepInto() {
    overlay_->snapshotRegs();   // so registers changed by this step are highlighted
    board_->stepInstruction();
    overlay_->setCursor(board_->cpu().pc());  // курсор — на исполняемую инструкцию
    overlay_->followPc();
    renderScreen();
    overlay_->update();
}

// Прогнать эмуляцию до инструкции под курсором и остановиться на ней (клавиша F4).
// Позволяет во время отладки поставить курсор ниже/выше и «дойти» туда автоматически.
// По пути честно срабатывают обычные точки останова; если за отведённый бюджет тактов
// курсор не достигнут, останавливаемся там, где оказались.
void MainWindow::runToCursor() {
    uint16_t target = overlay_->cursorAddr();
    if (target == board_->cpu().pc()) return;  // уже на месте
    overlay_->snapshotRegs();
    board_->runUntil(target, 3 * bk::Board::kDebugRunTicks);   // до курсора бывает и далеко
    board_->clearBreakHit();
    overlay_->setCursor(board_->cpu().pc());   // курсор — куда реально дошли
    overlay_->followPc();
    renderScreen();
    overlay_->update();
}

void MainWindow::stepOver() {
    overlay_->snapshotRegs();   // so registers changed by this step are highlighted
    board_->stepOver();
    overlay_->setCursor(board_->cpu().pc());  // курсор — на исполняемую инструкцию
    overlay_->followPc();
    renderScreen();
    overlay_->update();
}

void MainWindow::resizeEvent(QResizeEvent* e) {
    QMainWindow::resizeEvent(e);
    if (overlay_) overlay_->setGeometry(screen_->rect());
}

// Разница между размером окна и его «начинкой» (меню, строка состояния) заранее
// неизвестна, поэтому окно подгоняем один раз уже после показа: только так экран
// получает ровно целый масштаб (1024×768 при пикселях 3:4).
void MainWindow::showEvent(QShowEvent* e) {
    QMainWindow::showEvent(e);
    if (sizeFitted_) return;
    sizeFitted_ = true;
    const QSize want = screen_->preferredSize();
    resize(size() + (want - screen_->size()));
}

// ---- Interactive-disassembler annotations (symbols + comments) --------------

void MainWindow::nameCursorSymbol() {
    if (!paused_) return;
    uint16_t a = overlay_->cursorAddr();
    const std::string* cur = board_->symbols().nameAt(a);
    bool ok = false;
    QString name = QInputDialog::getText(this, "Символ",
        QString("Имя для адреса %1 (пусто — удалить):").arg(oct6(a)),
        QLineEdit::Normal, cur ? QString::fromStdString(*cur) : QString(), &ok);
    if (!ok) return;
    name = name.trimmed();
    if (name.isEmpty()) board_->symbols().remove(a);
    else board_->symbols().set(a, name.toStdString());
    overlay_->update();
}

void MainWindow::commentCursor() {
    if (!paused_) return;
    uint16_t a = overlay_->cursorAddr();
    const std::string* cur = board_->comment(a);
    bool ok = false;
    QString text = QInputDialog::getText(this, "Комментарий",
        QString("Комментарий к адресу %1 (пусто — удалить):").arg(oct6(a)),
        QLineEdit::Normal, cur ? QString::fromStdString(*cur) : QString(), &ok);
    if (!ok) return;
    board_->setComment(a, text.trimmed().toStdString());   // empty clears
    overlay_->update();
}

void MainWindow::markData(bk::DataType t) {
    if (!paused_) return;
    uint16_t a = overlay_->cursorAddr();
    uint16_t len = (t == bk::DataType::Word || t == bk::DataType::Ptr) ? 2 : 1;
    if (t == bk::DataType::String) {
        // Auto-length: printable run up to (and including) a terminating NUL, capped.
        len = 0;
        for (int i = 0; i < 80; ++i) {
            uint8_t c = board_->memory().peekByte(a + i);
            if (c == 0) { ++len; break; }
            if (c < 32 || c >= 127) break;
            ++len;
        }
        if (len == 0) len = 1;
    }
    board_->setData(a, t, len);
    overlay_->update();
}

void MainWindow::gotoDialog() {
    if (!paused_) return;
    // Editable picker: the sorted symbol names act as a function list; the field
    // also accepts a typed address (octal by default, or 0x hex).
    QStringList items;
    for (const auto& kv : board_->symbols().all()) items << QString::fromStdString(kv.second);
    items.sort();
    bool ok = false;
    QString sel = QInputDialog::getItem(this, "Перейти",
        "Символ или адрес (восьмер., или 0x…):", items, 0, /*editable*/ true, &ok);
    if (!ok) return;
    sel = sel.trimmed();
    uint16_t addr;
    if (board_->symbols().addrOf(sel.toStdString(), addr)) { overlay_->navigateTo(addr); return; }
    bool num = false; uint parsed = sel.startsWith("0x", Qt::CaseInsensitive)
        ? sel.mid(2).toUInt(&num, 16) : sel.toUInt(&num, 8);
    if (num) overlay_->navigateTo(static_cast<uint16_t>(parsed));
    else status_->setText("Не найдено: " + sel);
}

void MainWindow::xrefsDialog() {
    if (!paused_) return;
    uint16_t t = overlay_->cursorAddr();
    std::vector<uint16_t> refs = overlay_->xrefsTo(t);
    QString what = oct6(t);
    if (const std::string* nm = board_->symbols().nameAt(t)) what += " (" + QString::fromStdString(*nm) + ")";
    if (refs.empty()) { status_->setText("Нет ссылок на " + what); return; }
    QStringList items;
    for (uint16_t a : refs)
        items << QString("%1  %2").arg(oct6(a))
                     .arg(QString::fromStdString(bk::disasm(board_->memory(), a, &board_->symbols()).text));
    bool ok = false;
    QString sel = QInputDialog::getItem(this, "Ссылки на " + what,
        QString("Кто ссылается (%1):").arg(refs.size()), items, 0, /*editable*/ false, &ok);
    if (!ok) return;
    bool num = false; uint a = sel.section(' ', 0, 0).toUInt(&num, 8);
    if (num) overlay_->navigateTo(static_cast<uint16_t>(a));
}

QString MainWindow::annotationsPath() const {
    return lastBin_.isEmpty() ? QString() : lastBin_ + ".bkdb";
}

void MainWindow::saveAnnotations(const QString& path) {
    if (path.isEmpty()) return;
    QJsonArray syms, coms;
    for (const auto& kv : board_->symbols().all()) {
        QJsonObject o; o["a"] = int(kv.first); o["n"] = QString::fromStdString(kv.second);
        syms.append(o);
    }
    for (const auto& kv : board_->comments()) {
        QJsonObject o; o["a"] = int(kv.first); o["c"] = QString::fromStdString(kv.second);
        coms.append(o);
    }
    QJsonArray dat;
    for (const auto& kv : board_->dataItems()) {
        QJsonObject o; o["a"] = int(kv.first); o["t"] = int(kv.second.type); o["l"] = int(kv.second.len);
        dat.append(o);
    }
    if (syms.isEmpty() && coms.isEmpty() && dat.isEmpty()) return;   // nothing worth writing
    QJsonObject root; root["symbols"] = syms; root["comments"] = coms; root["data"] = dat;
    QFile f(path);
    if (f.open(QIODevice::WriteOnly))
        f.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
}

void MainWindow::loadAnnotations(const QString& path) {
    if (path.isEmpty()) return;
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return;
    QJsonObject root = QJsonDocument::fromJson(f.readAll()).object();
    for (const QJsonValue& v : root["symbols"].toArray()) {
        QJsonObject o = v.toObject();
        board_->symbols().set(uint16_t(o["a"].toInt()), o["n"].toString().toStdString());
    }
    for (const QJsonValue& v : root["comments"].toArray()) {
        QJsonObject o = v.toObject();
        board_->setComment(uint16_t(o["a"].toInt()), o["c"].toString().toStdString());
    }
    for (const QJsonValue& v : root["data"].toArray()) {
        QJsonObject o = v.toObject();
        board_->setData(uint16_t(o["a"].toInt()), bk::DataType(o["t"].toInt()), uint16_t(o["l"].toInt()));
    }
    if (overlay_) overlay_->update();
}

void MainWindow::closeEvent(QCloseEvent* e) {
    stopVideo();                          // дописать индекс AVI
    saveAnnotations(annotationsPath());   // persist symbols/comments next to the .bin
    saveDisks();
    QMainWindow::closeEvent(e);
    // Окна отладки — отдельные окна верхнего уровня и держали бы приложение открытым
    // уже без платы (она принадлежит главному окну).
    QCoreApplication::quit();
}

void MainWindow::openBin() {
    std::string wav;
    int n = 0;
    QString start = lastBin_;
    if (bk::parseTapeSpec(start.toStdString(), wav, n)) start = QString::fromStdString(wav);
    QString path = QFileDialog::getOpenFileName(this, "Загрузить программу", start,
        "Программы БК (*.bin *.BIN *.wav *.WAV);;Кассеты (*.wav *.WAV);;Все файлы (*)");
    if (path.isEmpty()) return;
    if (bk::parseTapeSpec(path.toStdString(), wav, n) && !n) {
        path = chooseTapeProgram(path);
        if (path.isEmpty()) return;
    }
    loadBinFromPath(path);
}

bool MainWindow::loadBinFromPath(const QString& path) {
    uint16_t addr = 0, len = 0;
    if (path != lastBin_) forgetBin();
    // В режиме auto машину выбирает программа (Board::binTargetsBk11): палитра 0177662 или
    // вызовы БОС — БК-0011М, иначе БК-0010-01. Явно выбранная машина остаётся какой есть.
    if (machineChoice_ == bk::MachineChoice::Auto) {
        const bk::Machine m = bk::Board::machineFor(machineChoice_, path.toStdString());
        if (m != board_->machine()) switchMachine(m);
    }
    // AZБК грузится со своих дисков, монитору с .BIN он мешает: на этот запуск его нет
    // (настройка остаётся, плата вернётся с переключением машины или галкой).
    const bool azDropped = board_->azbk();
    if (azDropped) { applyAz(false); board_->reset(); }
    // The monitor ROM must have initialised before a game is started; when a bin
    // is passed on the command line this happens before the 50 Hz timer has run
    // a single frame, so boot it here first. (На БК-0011М монитор БОС поднимает сам
    // loadBin.)
    board_->ensureMonitorBooted();
    if (!board_->loadBin(path.toStdString(), true, &addr, &len)) {
        QMessageBox::warning(this, "БК", QString("Не удалось загрузить %1").arg(path));
        return false;
    }
    const bool fresh = lastBin_ != path;
    lastBin_ = path;
    boot_ = Boot::Bin;
    if (fresh) loadAnnotations(annotationsPath());   // "<bin>.bkdb" — символы и комментарии
    std::string wav;
    int n = 0;
    const bool fromTape = bk::parseTapeSpec(path.toStdString(), wav, n);
    status_->setText(QString("Загружено %1: адрес %2, длина %3 (восьм.)%4%5")
        .arg(QFileInfo(path).fileName())
        .arg(QString::number(addr, 8))
        .arg(QString::number(len, 8))
        .arg(azDropped ? " — AZБК на этот запуск снят" : "")
        .arg(fromTape ? " — кассета в магнитофоне за этим файлом" : ""));
    if (fromTape) updateTapeLabel();
    updateTitle();
    return true;
}

void MainWindow::resetMachine() {
    board_->reset();
    keymap_.reset();
    keyFeed_.clear();
    releaseAllKeys();
    // Сброс повторяет последний пуск: программу, дискету в приводе A или винчестер.
    switch (boot_) {
    case Boot::Bin:
        if (!lastBin_.isEmpty()) loadBinFromPath(lastBin_);
        break;
    case Boot::Disk:
        if (board_->diskAttached(0)) { board_->ensureMonitorBooted(); board_->bootFromDisk(); }
        break;
    case Boot::Hdd:
        board_->bootFromHdd();
        break;
    case Boot::None:
        break;
    }
    status_->setText("Сброс");
}

void MainWindow::forgetBin() {
    if (!lastBin_.isEmpty()) {
        saveAnnotations(annotationsPath());
        board_->symbols().clear();
        board_->clearComments();
        board_->clearAllData();
        lastBin_.clear();
    }
    if (boot_ == Boot::Bin) boot_ = Boot::None;
    updateTitle();
}

// Подключение и снятие платы — операция «при выключенном питании»: меняется вся
// карта верхней половины адресов, поэтому машину перезапускаем. Настройка
// сохраняется между запусками; ключ --smk её не перезаписывает.
void MainWindow::setSmk512(bool on) {
    board_->setSmk512(on);
    QSettings().setValue(smkKey(), on);
    resetMachine();
    status_->setText(on ? "СМК-512 подключён, машина перезапущена"
                        : "СМК-512 снят, машина перезапущена");
}

bk::Azbk::Config MainWindow::azConfig() const {
    QSettings st;
    bk::Azbk::Config c;
    const QString sd = !azCli_.sd.isEmpty() ? azCli_.sd : st.value("azbk/sd", romDir_ + "/azbk").toString();
    c.sdDir = sd.toStdString();
    c.rw = azCli_.rw;
    c.writeDir = azWriteDir(sd, c.rw).toStdString();
    c.mod037 = azCli_.mod037 >= 0 ? azCli_.mod037 > 0 : st.value("azbk/mod037", false).toBool();
    const QString u0 = st.value("azbk/unit0").toString();
    if (!u0.isEmpty()) c.units[0] = u0.toStdString();
    for (const auto& kv : azCli_.units) {
        c.units[kv.first] = kv.second.toStdString();
        if (azCli_.unitsRw) c.rwUnits.insert(kv.first);
    }
    if (st.value("azbk/unit0rw", false).toBool()) c.rwUnits.insert(0);
    const QString net = !azCli_.net.isEmpty() ? azCli_.net : st.value("azbk/net").toString();
    if (net != "-") c.net = net.toStdString();
    return c;
}

// Сетевая карта платы: кабель — интерфейс хоста (tap/macvtap). Вставляется на ходу, как
// настоящий; MAC, DHCP и адрес — из [NETWORK] в AZ.INI (их меняет Setup платы).
void MainWindow::chooseAzNet() {
    bool ok = false;
    const QString cur = QString::fromStdString(azConfig().net);
    const QString spec = QInputDialog::getText(this, "Сеть AZБК",
        "Интерфейс хоста для сетевой карты платы:\n"
        "  macvtap:ИМЯ — свой MAC в локальной сети, адрес от её DHCP\n"
        "  tap:ИМЯ — tap-интерфейс (мост или NAT)\n"
        "  /dev/tapN — устройство macvtap\n"
        "Пусто — кабель вынут. Как создать интерфейс — docs/azbk.md, «Сеть».",
        QLineEdit::Normal, cur, &ok).trimmed();
    if (!ok) return;
    QSettings().setValue("azbk/net", spec.isEmpty() ? QString("-") : spec);
    azCli_.net.clear();
    if (!board_->azbk()) {
        status_->setText(spec.isEmpty() ? "Сеть AZБК: без кабеля" : "Сеть AZБК: " + spec + " (подключится с платой)");
        return;
    }
    const bool on = board_->az().setNet(spec.toStdString());
    const QString note = QString::fromStdString(board_->az().ctrl().netStatus());
    if (!on) status_->setText("Сеть AZБК не подключена: " + note);
    else if (spec.isEmpty()) status_->setText("Сеть AZБК: кабель вынут");
    else status_->setText("Сеть AZБК: " + spec + (note.isEmpty() ? QString() : " — " + note));
}

bool MainWindow::applyAz(bool on) {
    std::string err;
    if (on && !bk::Azbk::usable(azConfig(), &err)) {
        if (status_) status_->setText(QString("AZБК не установлен: %1").arg(QString::fromStdString(err)));
        else std::fprintf(stderr, "AZБК: %s\n", err.c_str());
        syncMachineMenu();
        return false;
    }
    if (on) saveDisks();
    const bool ok = board_->setAzbk(on, azConfig(), &err);
    if (!ok && status_) status_->setText(QString("AZБК не установлен: %1").arg(QString::fromStdString(err)));
    else if (!ok) std::fprintf(stderr, "AZБК: %s\n", err.c_str());
    if (on && ok) forgetBin();
    syncMachineMenu();
    updateTitle();
    return ok;
}

// Плату ставят при выключенном питании: машина перезапускается и грузится с AZBOOT.
void MainWindow::setAzbk(bool on) {
    azCli_.on = -1;
    if (!applyAz(on)) return;                         // машина не тронута, галка вернулась
    QSettings().setValue(azKey(), on);
    if (!on) board_->setSmk512(QSettings().value(smkKey(), smkDefault()).toBool());
    resetMachine();
    status_->setText(on ? "Контроллер AZБК установлен, машина перезапущена" : "Контроллер AZБК снят, машина перезапущена");
}

void MainWindow::setAzVga(bool on) {
    board_->setAzVga(on);
    QSettings().setValue("azbk/vga", on);
    renderScreen();
    status_->setText(on ? "Экран: VGA платы AZБК" : "Экран: сама БК (ТВ)");
}

void MainWindow::chooseAzSd() {
    const QString cur = QString::fromStdString(azConfig().sdDir);
    const QString dir = QFileDialog::getExistingDirectory(this, "Каталог карты SD AZБК (с AZ.INI)", cur);
    if (dir.isEmpty()) return;
    bk::Azbk::Config c = azConfig();
    c.sdDir = dir.toStdString();
    c.writeDir = azWriteDir(dir, c.rw).toStdString();
    std::string err;
    if (!bk::Azbk::usable(c, &err)) {
        status_->setText(QString("Карта SD не подходит: %1").arg(QString::fromStdString(err)));
        return;
    }
    const QString prevSd = azCli_.sd;
    QSettings st;
    const QVariant prev = st.value("azbk/sd");
    st.setValue("azbk/sd", dir);
    azCli_.sd.clear();
    if (board_->azbk()) {
        if (!applyAz(true)) {                          // не включилась — прежняя карта
            if (prev.isValid()) st.setValue("azbk/sd", prev); else st.remove("azbk/sd");
            azCli_.sd = prevSd;
            return;
        }
        resetMachine();
    }
    status_->setText("Карта SD AZБК: " + dir);
}

void MainWindow::chooseAzUnit0() {
    const QString path = QFileDialog::getOpenFileName(this, "Диск AZ0 (загрузочный)",
        QSettings().value("azbk/unit0", QString::fromStdString(azConfig().sdDir)).toString(),
        "Образы дисков (*.img *.IMG *.bkd *.BKD *.dsk *.DSK);;Все файлы (*)");
    if (path.isEmpty()) return;
    QSettings().setValue("azbk/unit0", path);
    azCli_.units.erase(0);
    if (!board_->azbk()) { setAzbk(true); return; }
    board_->setAzUnit(0, path.toStdString());
    resetMachine();
    status_->setText("Диск AZ0: " + QFileInfo(path).fileName());
}

void MainWindow::openAzRam() {
    if (!azram_) azram_ = new AzRamWidget(board_.get());
    azram_->show();
    azram_->raise();
    if (!board_->azbk()) status_->setText("AZБК не установлен: окно памяти пустое");
}

void MainWindow::applyScanline() {
    board_->setScanlineRender(scanlineOverride_ >= 0 ? scanlineOverride_ > 0 : board_->bk11());
}

void MainWindow::syncMachineMenu() {
    for (QAction* a : {machineAutoAction_, machine10Action_, machine11Action_, machine10fAction_, ayAction_,
                       ay2Action_, covoxAction_, smkAction_})
        if (!a) return;
    QSignalBlocker b0(machineAutoAction_), b1(machine10Action_), b2(machine11Action_), b3(ayAction_),
                   b4(smkAction_), b5(ay2Action_), b6(covoxAction_), b7(machine10fAction_);
    machineAutoAction_->setChecked(machineChoice_ == bk::MachineChoice::Auto);
    machine10Action_->setChecked(machineChoice_ == bk::MachineChoice::Bk0010);
    machine10fAction_->setChecked(machineChoice_ == bk::MachineChoice::Bk0010Focal);
    machine11Action_->setChecked(machineChoice_ == bk::MachineChoice::Bk0011m);
    machineAutoAction_->setText(QString("&Автоматически — по программе (сейчас %1)")
                                    .arg(QString::fromUtf8(bk::Board::machineName(board_->machine()))));
    ayAction_->setChecked(board_->ay());
    ay2Action_->setChecked(board_->ay2());
    covoxAction_->setChecked(board_->covox());
    smkAction_->setChecked(board_->smk512());
    smkAction_->setEnabled(!board_->azbk());
    if (azAction_) {
        QSignalBlocker ba(azAction_), bb(az037Action_), bc(azUnit0RwAction_);
        azAction_->setChecked(board_->azbk());
        az037Action_->setEnabled(!board_->bk11());
        az037Action_->setChecked(azConfig().mod037);
        azUnit0RwAction_->setChecked(azConfig().rwUnits.count(0) != 0);
    }
}

void MainWindow::setMachineChoice(bk::MachineChoice c) {
    machineChoice_ = c;
    QSettings().setValue("machineMode", QString::fromLatin1(bk::Board::machineChoiceName(c)));
    if (c != bk::MachineChoice::Auto) { switchMachine(bk::Board::machineFor(c)); return; }
    // Авто: машину выбирает загруженная программа; без программы машина остаётся, её
    // выберет следующая.
    if (!lastBin_.isEmpty() && bk::Board::machineFor(c, lastBin_.toStdString()) != board_->machine()) {
        loadBinFromPath(QString(lastBin_));
    } else {
        syncMachineMenu();
        updateTitle();
        status_->setText(lastBin_.isEmpty() ? QString("Машина: автоматически — её выберет следующая программа")
                                            : QString("Машина: автоматически — программа подходит этой машине"));
    }
}

void MainWindow::switchMachine(bk::Machine m) {
    if (m == board_->machine()) { syncMachineMenu(); updateTitle(); return; }
    saveDisks();
    board_->setMachine(m);
    applySoundSettings();
    // Плата — своя настройка у каждой машины (на БК-0011М её ставят реже).
    board_->setSmk512(QSettings().value(smkKey(), smkDefault()).toBool());
    applyAz(QSettings().value(azKey(), false).toBool());
    applyScanline();
    forgetBin();                       // .BIN другой машины перезапускать нельзя
    syncMachineMenu();
    syncKeyboardKind();
    resetMachine();
    updateTitle();
    status_->setText(QString("Машина: %1 — перезапущена").arg(QString::fromUtf8(bk::Board::machineName(board_->machine()))));
}

// Звук — по настройкам этой машины (по умолчанию на БК-0011М всё включено: AY, второй
// AY и Covox, на БК-0010-01 — ничего); ключи командной строки перекрывают их на запуск.
void MainWindow::applySoundSettings(int ayOverride, int ay2Override, int covoxOverride) {
    const bool b11 = board_->bk11();
    auto pick = [&](int ovr, const char* base) {
        return ovr >= 0 ? ovr > 0 : QSettings().value(soundKey(base), b11).toBool();
    };
    board_->setAy(pick(ayOverride, "ay"));
    board_->setAy2(pick(ay2Override, "ay2_"));
    board_->setCovox(pick(covoxOverride, "covox"));
}

void MainWindow::setAy(bool on) {
    board_->setAy(on);
    QSettings().setValue(soundKey("ay"), on);
    status_->setText(on ? "Звуковой сопроцессор AY-3-8910 подключён" : "Звуковой сопроцессор AY-3-8910 снят");
}

void MainWindow::setAy2(bool on) {
    board_->setAy2(on);
    QSettings().setValue(soundKey("ay2_"), on);
    status_->setText(on ? "Второй AY подключён (плата «2AY»: TurboSound и «Грифон»)" : "Второй AY снят");
}

void MainWindow::setCovox(bool on) {
    board_->setCovox(on);
    QSettings().setValue(soundKey("covox"), on);
    status_->setText(on ? "Covox подключён к порту 0177714" : "Covox снят");
}

void MainWindow::setPixelAspect34(bool on) {
    screen_->setPixelAspect34(on);
    QSettings().setValue("aspect34", on);
    if (aspectAction_ && aspectAction_->isChecked() != on) {
        QSignalBlocker b(aspectAction_);
        aspectAction_->setChecked(on);
    }
    status_->setText(on ? "Пиксели: 3:4, блок 4×3 точки (кадр 1024×768)"
                        : "Пиксели: квадратные (кадр 512×256)");
    // Кадру 3:4 нужно больше высоты, чем квадратному: если окно мельче целого кадра,
    // масштаб перестал бы быть кратным — подрастим окно (сужать при обратном
    // переключении не будем, место уже отдано пользователю).
    const QSize want = screen_->preferredSize(), have = screen_->size();
    if (want.width() > have.width() || want.height() > have.height())
        resize(size() + QSize(qMax(0, want.width() - have.width()),
                              qMax(0, want.height() - have.height())));
}

void MainWindow::toggleColorMode() {
    colorMode_ = !colorMode_;
    status_->setText(colorMode_ ? "Режим: 256×256 цвет" : "Режим: 512×256 ч/б");
}

// Alt+клавиша при работающей БК — это АР2, а не мнемоника меню («&Файл»):
// приняв ShortcutOverride, отдаём нажатие в keyPressEvent.
bool MainWindow::event(QEvent* e) {
    if (e->type() == QEvent::ShortcutOverride && !paused_ && !suspended_) {
        auto* k = static_cast<QKeyEvent*>(e);
        if (k->modifiers() & Qt::AltModifier) { k->accept(); return true; }
    }
    return QMainWindow::event(e);
}

void MainWindow::keyPressEvent(QKeyEvent* e) {
    // F12 toggles the debugger regardless of state.
    if (e->key() == Qt::Key_F12) { setPaused(!paused_); e->accept(); return; }
    // Pause key toggles the simple emulation freeze.
    if (e->key() == Qt::Key_Pause) { setSuspended(!suspended_); e->accept(); return; }

    // While suspended, swallow all other keys so nothing reaches the game or the
    // key buffer (they'd otherwise be replayed on resume).
    if (suspended_ && !paused_) { e->accept(); return; }

    if (paused_) {
        // Идёт правка значения на месте — все клавиши уходят в поле ввода,
        // иначе цифры и Enter разошлись бы по командам отладчика.
        if (overlay_->handleEditKey(e)) { e->accept(); return; }
        // --- Debugger controls (SoftICE overlay is visible) ---
        switch (e->key()) {
        case Qt::Key_F4:       runToCursor(); return;   // дойти до инструкции под курсором
        case Qt::Key_F7:       stepInto(); return;
        case Qt::Key_F8:       stepOver(); return;
        case Qt::Key_F9:       board_->toggleBreakpoint(board_->cpu().pc()); overlay_->update(); return;
        case Qt::Key_Escape:   setPaused(false); return;
        case Qt::Key_PageUp:   overlay_->scrollDisasm(-8); return;
        case Qt::Key_PageDown: overlay_->scrollDisasm(8); return;
        case Qt::Key_Up:       overlay_->moveCursor(-1); return;
        case Qt::Key_Down:     overlay_->moveCursor(1); return;
        case Qt::Key_N:        nameCursorSymbol(); return;   // name/rename the selected line
        case Qt::Key_Semicolon: commentCursor(); return;     // comment the selected line
        case Qt::Key_B:        markData(bk::DataType::Byte);   return;  // mark data types
        case Qt::Key_W:        markData(bk::DataType::Word);   return;
        case Qt::Key_S:        markData(bk::DataType::String); return;
        case Qt::Key_P:        markData(bk::DataType::Ptr);    return;
        case Qt::Key_U:        board_->clearData(overlay_->cursorAddr()); overlay_->update(); return; // back to code
        case Qt::Key_G:        gotoDialog(); return;                       // goto symbol/address
        case Qt::Key_X:        xrefsDialog(); return;                      // who references the cursor
        case Qt::Key_Return:
        case Qt::Key_Enter:    overlay_->followTarget(); return;           // follow branch/call target
        case Qt::Key_Left:     overlay_->navBack(); return;                // history back / forward
        case Qt::Key_Right:    overlay_->navForward(); return;
        case Qt::Key_BracketLeft:  overlay_->scrollMem(-8); return;
        case Qt::Key_BracketRight: overlay_->scrollMem(8); return;
        default: e->accept(); return; // swallow other keys while debugging
        }
    }

    // Esc при выключенном отладчике ничего не делает (в отладчике — «продолжить»).
    if (e->key() == Qt::Key_Escape) { e->accept(); return; }

    // F10 — клавиша «СТОП» БК. Она вынесена из матрицы и кода не даёт, поэтому идёт
    // мимо BkKeymap: внеприоритетное прерывание по вектору 4.
    if (e->key() == Qt::Key_F10) {
        board_->pressStop();
        status_->setText(QString::fromUtf8("Клавиша СТОП (прерывание по вектору 4)"));
        e->accept();
        return;
    }

    // --- SoftICE off: feed the BK-0010 keyboard ---
    // Автоповтор Qt отбрасываем целиком: у реальной БК типematic-повтора нет
    // (для повтора была клавиша ПОВТ). Иначе дубликаты удерживаемой клавиши
    // встают в очередь ВПЕРЕДИ только что нажатой — и игра, читающая код по биту
    // «клавиша нажата» (PITON), срабатывает по коду предыдущей клавиши.
    if (e->isAutoRepeat()) { e->accept(); return; }
    const BkKeymap keymapWas = keymap_;
    std::vector<uint16_t> codes = keymap_.translate(e);
    if (!codes.empty()) {
        // Клавиша матрицы: разряд 6 0177716 — «нажата». Если уже нажата другая,
        // 014 новую не видит: ни кода, ни прерывания (и РУС/ЛАТ не переключаем).
        const bool busy = !heldKeys_.empty();
        heldKeys_.insert(e->key());
        board_->setKeyHeld(true);
        if (busy) { keymap_ = keymapWas; e->accept(); return; }
        // Одиночный код при пустой очереди защёлкиваем НЕМЕДЛЕННО — атомарно с
        // битом «клавиша нажата» (setKeyHeld выше): эмуляция однопоточная, между
        // ними не исполняется ни одной инструкции БК. Если отложить код до подачи
        // из очереди в onTick, игра, читающая 0177662 по биту 0177716 (PITON),
        // целый кадр видела бы «клавиша нажата» с кодом ПРЕДЫДУЩЕЙ клавиши.
        // Многокодовые последовательности (РУС/ЛАТ + символ) и накопившийся хвост
        // по-прежнему идут через очередь — по одному коду на кадр, чтобы монитор
        // успевал прочитать каждый.
        if (keyFeed_.empty() && codes.size() == 1) {
            board_->pressKey(codes[0]);
        } else {
            for (uint16_t c : codes) {
                // Cap the buffer, mirroring the fact that the real controller
                // keeps only one pending code.
                if (keyFeed_.size() >= 16) break;
                keyFeed_.push_back(c);
            }
        }
        e->accept();
        return;
    }
    QMainWindow::keyPressEvent(e);
}

void MainWindow::releaseAllKeys() {
    heldKeys_.clear();
    board_->setKeyHeld(false);
}

void MainWindow::keyReleaseEvent(QKeyEvent* e) {
    // Release the physical key-held state (0177716 bit 6) when the last game key
    // is lifted. Auto-repeat generates spurious release events — ignore those.
    if (!e->isAutoRepeat() && heldKeys_.erase(e->key()) && heldKeys_.empty())
        board_->setKeyHeld(false);
    QMainWindow::keyReleaseEvent(e);
}

// Вставить дискету и загрузиться с неё. Контроллер появляется на шине вместе с
// образом, поэтому машину перезапускаем: на живой БК плату в разъём МПИ ставят
// при выключенном питании. Дальше поднимаем монитор и отдаём управление
// автозагрузчику ПЗУ контроллера — то же, что набрать в мониторе «160000G».
void MainWindow::openDisk(int drive) {
    const QString path = QFileDialog::getOpenFileName(
        this, drive ? "Вставить образ диска в привод B" : "Вставить образ диска",
        lastDisk_.isEmpty() ? lastBin_ : lastDisk_,
        "Образы дисков БК (*.bkd *.BKD *.bkD *.img *.IMG *.Img);;Все файлы (*)");
    if (path.isEmpty()) return;
    insertDiskAndBoot(path, drive);
}

void MainWindow::saveDisks() {
    bk::Fdd& f = board_->kngmd().fdd();
    for (int d = 0; d < bk::Fdd::DRIVES; ++d)
        if (f.attached(d) && !f.readOnly(d)) f.save(d);   // save() дописывает и текущую дорожку
}

bool MainWindow::insertDiskAndBoot(const QString& path, int drive) {
    // С AZБК дискеты нет: образ становится его диском AZ0/AZ1 на этот запуск.
    if (board_->azbk()) {
        azCli_.units[drive] = path;
        board_->setAzUnit(drive, path.toStdString());   // AZ1 — на ходу, AZ0 — с загрузкой
        if (drive == 0) resetMachine();
        status_->setText(QString("Диск AZ%1: %2").arg(drive).arg(QFileInfo(path).fileName()));
        return true;
    }
    saveDisks();
    if (!board_->attachDisk(drive, path.toStdString(), !diskRw_)) {
        status_->setText("Не удалось вставить образ — нет roms/disk326.rom или файл не читается");
        return false;
    }
    // Второй привод не перезапускает машину: дискету в него меняют на ходу. Привод A —
    // пуск с диска (монитор, затем автозагрузчик контроллера), и сброс повторяет его.
    if (drive == 0) {
        forgetBin();
        boot_ = Boot::Disk;
        resetMachine();
    }
    lastDisk_ = path;
    // СМК и дисковод уживаются на шине вместе (это и есть историческая машина с
    // диском), поэтому плату не снимаем. Наоборот, без неё большинству систем
    // некуда грузиться: у голой БК-0010-01 свободно около 16 Кбайт ОЗУ.
    status_->setText(drive
        ? QString("Дискета в приводе B: %1").arg(QFileInfo(path).fileName())
        : QString("Загрузка с диска: %1%2").arg(QFileInfo(path).fileName())
              .arg(board_->smk512() ? ", ОЗУ платы СМК-512 подключено" : board_->bk11() ? ""
                                    : " — ВНИМАНИЕ: плата СМК-512 снята, памяти может не хватить"));
    updateTitle();
    return true;
}

// Винчестер — на плате СМК-512 (регистры 0177740–0177757, грузит прошивка платы,
// перехватывающая пуск) или на «Самаре» (0177620–0177656, грузит автозагрузчик КНГМД).
// Master подключается с включением питания, slave — на ходу, как вторая дискета.
void MainWindow::openHdd(int unit) {
    const QString path = QFileDialog::getOpenFileName(
        this, unit ? "Подключить второй винчестер (slave)" : "Подключить винчестер",
        lastHdd_.isEmpty() ? lastDisk_ : lastHdd_,
        "Образы винчестеров HDI (*.hdi *.HDI *.Hdi);;Все файлы (*)");
    if (path.isEmpty()) return;
    attachHdd(path, unit, unit == 0);
}

void MainWindow::setHddController(bk::Board::HddController c) {
    board_->setHddController(c);
    for (const auto& a : hddCtlActions_) a.first->setChecked(a.second == c);
}

bool MainWindow::attachHdd(const QString& path, int unit, bool boot) {
    if (board_->azbk()) {
        status_->setText("С контроллером AZБК винчестеры СМК и «Самары» не подключаются");
        return false;
    }
    // Винчестер СМК — узел платы СМК-512; «Самаре» плата не нужна.
    if (board_->resolveHddController(path.toStdString()) == bk::Board::HddController::Smk
        && !board_->smk512()) {
        board_->setSmk512(true);
        syncMachineMenu();
    }
    std::string err;
    if (!board_->attachHdd(unit, path.toStdString(), !hddRw_, &err)) {
        status_->setText(QString("Винчестер не подключён: %1").arg(QString::fromStdString(err)));
        return false;
    }
    lastHdd_ = path;
    if (boot) { forgetBin(); boot_ = Boot::Hdd; resetMachine(); }
    const bk::HddDrive& d = board_->ide().drive(unit);
    status_->setText(QString("Винчестер %1 (%9): %2 — %3/%4/%5, %6 Мбайт%7%8")
        .arg(unit ? "slave" : "master").arg(QFileInfo(path).fileName())
        .arg(d.cylinders()).arg(d.heads()).arg(d.sectors())
        .arg(d.totalSectors() / 2048)
        .arg(hddRw_ ? "" : ", только чтение (запись — в памяти)")
        .arg(boot ? ", загрузка" : "")
        .arg(board_->hddSamara() ? "«Самара»" : "СМК-512")
        + (board_->hddSamara() && !board_->bk11() && !board_->smk512()
               ? " — ВНИМАНИЕ: плата СМК-512 снята, загрузчику может не хватить памяти" : ""));
    updateTitle();
    return true;
}

void MainWindow::detachHdds() {
    if (!board_->hddAttached(0) && !board_->hddAttached(1)) return;
    board_->detachHdd(0);
    board_->detachHdd(1);
    if (boot_ == Boot::Hdd) boot_ = Boot::None;
    resetMachine();
    status_->setText("Винчестеры отключены, машина перезапущена");
}

void MainWindow::openMemVis() {
    if (!memvis_) memvis_ = new MemVisWidget(board_.get());
    board_->trace().setEnabled(true);
    memvis_->show();
    memvis_->raise();
}

// Отдельное окно на ДОЗУ нужно потому, что обычный дамп ходит по адресам БК и
// показывает только подключённую страницу, да и то не целиком: верхние 512 байт
// сегмента не читаются ни в одном режиме (docs/smk512.md).
void MainWindow::openBkRam() {
    if (!bkram_) bkram_ = new BkRamWidget(board_.get());
    bkram_->show();
    bkram_->raise();
}

void MainWindow::openSmkRam() {
    if (!smkram_) smkram_ = new SmkRamWidget(board_.get());
    smkram_->show();
    smkram_->raise();
    if (!board_->smk512())
        status_->setText("СМК-512 не установлен: окно ДОЗУ пустое");
}

void MainWindow::broadcastHighlight(int addr, ProfilerView* src) {
    for (ProfilerView* p : profilers_)
        if (p != src) p->setHighlight(addr);
    if (overlay_) overlay_->setHighlight(addr);   // debugger disasm is always a receiver
}

template <class W>
void MainWindow::openProfiler(W*& w, QSize size, bool flame, bool spans) {
    if (!w) {
        w = new W(board_.get());
        if (size.isValid()) w->resize(size);
        ProfilerView* v = w;
        // Щелчок по адресу ведёт дизассемблер отладчика туда (отладчик открывается).
        connect(v, &ProfilerView::addressPicked, this, [this](uint16_t addr) {
            if (!paused_) setPaused(true);   // setPaused re-anchors disasm to PC…
            overlay_->setDisasmAddr(addr);   // …so set the picked address after it
            overlay_->update();
        });
        connect(v, &ProfilerView::hoverAddress, this, [this, v](int a) { broadcastHighlight(a, v); });
        profilers_.push_back(v);
    }
    board_->trace().setEnabled(true);
    if (flame) board_->trace().setFlameEnabled(true);
    if (spans) board_->trace().setSpansEnabled(true);
    w->show();
    w->raise();
}

void MainWindow::openHotPath()    { openProfiler(hotpath_, QSize(720, 620)); }
void MainWindow::openCallGraph()  { openProfiler(callgraph_, QSize(900, 680)); }
void MainWindow::openFlame()      { openProfiler(flame_, QSize(820, 560), true); }
void MainWindow::openFlameChart() { openProfiler(flamechart_, QSize(900, 480), true, true); }
void MainWindow::openHotChart()   { openProfiler(hotchart_, QSize()); }

void MainWindow::saveState() {
    QString path = QFileDialog::getSaveFileName(this, "Сохранить состояние", QString(),
        "BK state (*.bkst);;Все файлы (*)");
    if (path.isEmpty()) return;
    if (!path.endsWith(".bkst")) path += ".bkst";
    if (board_->saveState(path.toStdString()))
        status_->setText("Состояние сохранено: " + QFileInfo(path).fileName());
    else
        QMessageBox::warning(this, "БК", "Не удалось сохранить состояние");
}

void MainWindow::loadState() {
    QString path = QFileDialog::getOpenFileName(this, "Восстановить состояние", QString(),
        "BK state (*.bkst);;Все файлы (*)");
    if (path.isEmpty()) return;
    if (board_->loadState(path.toStdString())) {
        status_->setText("Состояние восстановлено: " + QFileInfo(path).fileName());
        // В снимке лежит и конфигурация машины, поэтому плата СМК могла появиться
        // или исчезнуть. Галку в меню синхронизируем без сигнала: иначе она тут же
        // перезапустила бы машину и стёрла только что восстановленное состояние.
        syncMachineMenu();
        syncKeyboardKind();
        applyScanline();
        updateTitle();
        renderScreen();
        if (paused_) overlay_->followPc();
    } else {
        QMessageBox::warning(this, "БК", "Не удалось восстановить состояние");
    }
}

void MainWindow::updateTitle() {
    QString t = QString("%1%2 — эмулятор-отладчик").arg(QString::fromUtf8(bk::Board::machineName(board_->machine())))
                    .arg(machineChoice_ == bk::MachineChoice::Auto ? " (авто)" : "");
    if (board_->azbk()) t += " + AZБК";
    if (!lastBin_.isEmpty()) t += " — " + QFileInfo(lastBin_).fileName();
    setWindowTitle(t);
}
